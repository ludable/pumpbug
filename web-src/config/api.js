// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

const STA_POLL_FIRST_DELAY_MS = 15000;
const STA_POLL_INTERVAL_MS = 2000;
const STA_POLL_TIMEOUT_MS = 45000;
const STA_POLL_REQUEST_TIMEOUT_MS = 1500;

let pairingInFlight = false;
let staPollToken = 0;

async function api(path, opts) {
  opts = opts || {};
  const timeoutMs = opts.timeoutMs || 0;
  const fetchOpts = Object.assign({}, opts);
  delete fetchOpts.timeoutMs;

  let timeoutId = null;
  if (timeoutMs && typeof AbortController !== 'undefined') {
    const controller = new AbortController();
    fetchOpts.signal = controller.signal;
    timeoutId = setTimeout(() => controller.abort(), timeoutMs);
  }

  try {
    const r = await fetch(path, fetchOpts);
    let body = {};
    try { body = await r.json(); } catch (_) {}
    return { ok: r.ok, status: r.status, body };
  } finally {
    if (timeoutId) clearTimeout(timeoutId);
  }
}

async function refresh() {
  const who = await api('/auth/whoami');
  authed = !!who.body.authenticated;
  if (authed) {
    const s = await api('/sys/wifi/status');
    status = s.ok ? s.body : null;
  } else {
    status = null;
  }
  render();
}

function sleep(ms) {
  return new Promise(resolve => setTimeout(resolve, ms));
}

let scanPollToken = 0;

// Kicks off (or joins) a device-side scan and polls until results arrive.
// The endpoint answers {"scanning":true} while the scan runs and frees each
// result set once served, so the networks are kept client-side in `nets`.
async function scanNetworks() {
  const token = ++scanPollToken;
  nets = null;
  render();
  const deadline = Date.now() + 30000;
  while (Date.now() < deadline && token === scanPollToken) {
    let r = null;
    try {
      r = await api('/sys/wifi/scan', { cache: 'no-store', timeoutMs: 4000 });
    } catch (_) {
      // Transient fetch failure; keep polling until the deadline.
    }
    if (token !== scanPollToken) return;
    if (r && r.ok && r.body.networks) {
      nets = r.body.networks;
      render();
      return;
    }
    await sleep(1500);
  }
  if (token === scanPollToken) {
    nets = [];
    render();
  }
}

async function pollStaFallback(host, token) {
  const deadline = Date.now() + STA_POLL_TIMEOUT_MS;
  await sleep(STA_POLL_FIRST_DELAY_MS);
  while (token === staPollToken && Date.now() < deadline) {
    try {
      const s = await api('/sys/wifi/status', {
        cache: 'no-store',
        timeoutMs: STA_POLL_REQUEST_TIMEOUT_MS,
      });
      if (token !== staPollToken) return;
      if (s.ok) {
        status = s.body;
        authed = true;
        const reason = status.reason || '';
        if (reason) {
          staSent = false;
          msg = { kind: 'bad',
                  text: 'Could not join Wi-Fi: ' + reason };
          render();
          return;
        } else if (status.mode === 'ap' || status.state === 'ap_up') {
          staSent = false;
          msg = { kind: 'bad',
                  text: 'Could not join Wi-Fi. The device returned to AP mode.' };
          render();
          return;
        } else if (status.state === 'sta_connected') {
          msg = { kind: 'ok',
                  text: 'Pump Bug joined the selected Wi-Fi network.' };
          render();
          return;
        }
      } else if (s.status === 401) {
        authed = false;
        status = null;
        staSent = false;
        msg = { kind: 'warn',
                text: 'The device is back in setup mode. Re-pair to retry Wi-Fi setup.' };
        render();
        return;
      }
    } catch (_) {
      // Expected while the AP is down or the client is switching networks.
    }
    await sleep(STA_POLL_INTERVAL_MS);
  }
  if (token === staPollToken) {
    // Going silent is the likely-success case during guided setup — the AP
    // dropped and the client moved networks — so the hand-off panel stays
    // and only gets a soft caveat.
    msg = staSent
        ? { kind: 'warn',
            text: 'Could not verify the result from here — check the ' +
                  'device screen.' }
        : { kind: 'warn',
            text: 'Could not verify the Wi-Fi result. If it joined, connect to your Wi-Fi and open ' +
                  host + '.local. Otherwise reconnect to the setup AP to retry.' };
    render();
  }
}

async function submitPin(pin) {
  // Both oninput (when 4 digits typed) and onsubmit (Enter / virtual
  // keyboard "go") fire on a fast 4-digit entry, so guard against
  // the second submission consuming an already-spent PIN and
  // surfacing a stale 401.
  if (pairingInFlight) return;
  pairingInFlight = true;
  try {
    msg = null;
    const r = await api('/auth/pair', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: 'pin=' + encodeURIComponent(pin),
    });
    if (!r.ok) {
      msg = { kind: 'bad', text: r.body.error || 'pairing failed' };
      render();
      return;
    }
    justPaired = true;
    await refresh();
  } finally {
    pairingInFlight = false;
  }
}

async function submitSta(ssid, pass) {
  msg = null;
  const body = 'ssid=' + encodeURIComponent(ssid) +
               '&pass=' + encodeURIComponent(pass);
  const r = await api('/sys/wifi/sta', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: body,
  });
  if (!r.ok) {
    msg = { kind: 'bad', text: r.body.error || 'set failed' };
  } else {
    const host = (status && status.hostname) ||
                 (status && status.device_id) || 'pumpbug';
    if (status && status.mode === 'ap') {
      // Guided setup: the action moves to the device screen. The poller
      // below only reports back if the attempt visibly fails.
      staSent = true;
      msg = null;
    } else {
      msg = { kind: 'ok',
              text: 'Trying to join the new network. This page will report ' +
                    'the result; if the name changes, reopen ' + host +
                    '.local.' };
    }
    const token = ++staPollToken;
    pollStaFallback(host, token);
  }
  render();
}

async function revealCliToken() {
  msg = null;
  const r = await api('/auth/token');
  if (!r.ok || !r.body.token) {
    msg = { kind: 'bad', text: r.body.error || 'Could not read the cookie.' };
    render();
    return;
  }
  cliToken = 'auth_token=' + r.body.token;
  render();
}

async function submitHostname(name) {
  msg = null;
  const body = 'name=' + encodeURIComponent(name);
  const r = await api('/sys/wifi/hostname', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: body,
  });
  if (!r.ok) {
    msg = { kind: 'bad',
            text: r.body.error || 'failed to save hostname' };
  } else {
    msg = { kind: 'ok',
            text: 'Saved. Takes effect on next Wi-Fi restart; ' +
                  'you will need to re-pair from the new name.' };
    await refresh();
  }
  render();
}

async function forgetAll() {
  if (!confirm('Forget Wi-Fi config and all paired clients?')) return;
  staPollToken++;
  msg = null;
  await api('/sys/wifi/forget', { method: 'POST' });
  authed = false;
  status = null;
  msg = { kind: 'ok',
          text: 'Reset. This client is no longer paired.' };
  render();
}
