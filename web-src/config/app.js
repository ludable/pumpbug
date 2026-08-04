// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

let authed = null;
let status = null;
let msg = null;
let nets;  // undefined: no scan yet; null: scan running; array: results
let selectedSsid = null;
let manualEntry = false;
let changingNetwork = false;
// True when this visit should greet a fresh pairing: the device's setup QR
// redirects here with #paired, and the manual PIN path sets it directly.
// The hash is consumed so a later reload doesn't re-greet.
let justPaired = location.hash === '#paired';
if (justPaired) {
  history.replaceState(null, '', location.pathname + location.search);
}
// True once AP-mode setup has sent Wi-Fi credentials: the action moves to
// the device screen, so the page parks on a hand-off panel.
let staSent = false;
// This client's paired auth_token cookie, or null until the user reveals it.
// It's a bearer credential, so it stays off-screen until asked for.
let cliToken = null;

function el(tag, attrs, ...kids) {
  attrs = attrs || {};
  const e = document.createElement(tag);
  for (const k in attrs) {
    if (k === 'class') e.className = attrs[k];
    else if (k.slice(0, 2) === 'on') {
      e.addEventListener(k.slice(2), attrs[k]);
    } else {
      e.setAttribute(k, attrs[k]);
    }
  }
  for (const k of kids) if (k != null) e.append(k);
  return e;
}

function renderMsg() {
  return msg ? el('div', { class: 'msg ' + msg.kind }, msg.text) : null;
}

// Copies text to the clipboard. The device serves plain HTTP on the LAN, so
// navigator.clipboard is usually absent (non-secure context); fall back to a
// throwaway textarea + execCommand, which works there.
function copyText(text) {
  const done = () => { msg = { kind: 'ok', text: 'Copied to clipboard.' }; render(); };
  if (navigator.clipboard && navigator.clipboard.writeText) {
    navigator.clipboard.writeText(text).then(done, () => execCopy(text, done));
  } else {
    execCopy(text, done);
  }
}

function execCopy(text, done) {
  const ta = el('textarea', { style: 'position:fixed;opacity:0' });
  ta.value = text;
  document.body.append(ta);
  ta.select();
  try {
    document.execCommand('copy');
    done();
  } catch (_) {
    msg = { kind: 'warn', text: 'Select the command and copy it manually.' };
    render();
  }
  ta.remove();
}

function renderUnauth() {
  const onSubmit = (e) => {
    e.preventDefault();
    const v = e.target.pin.value.trim();
    if (v.length === 4) submitPin(v);
  };
  const onInput = (e) => {
    if (e.target.value.length >= 4) {
      e.target.value = e.target.value.slice(0, 4);
      submitPin(e.target.value);
    }
  };
  return el('section', {},
    el('h2', {}, 'Pair'),
    el('p', { class: 'muted' },
      'On the device, open Wi-Fi and tap A (VISIT). Enter the 4-digit PIN ' +
      'shown, or scan the QR code.'),
    el('form', { onsubmit: onSubmit },
      el('input', {
        class: 'pin',
        type: 'text',
        name: 'pin',
        inputmode: 'numeric',
        pattern: '[0-9]*',
        maxlength: '4',
        autocomplete: 'off',
        autofocus: 'true',
        oninput: onInput,
      })),
    renderMsg(),
  );
}

// ----- network picker -------------------------------------------------------

function signalDots(rssi) {
  const n = rssi >= -50 ? 4 : rssi >= -60 ? 3 : rssi >= -70 ? 2 : 1;
  return '●'.repeat(n) + '○'.repeat(4 - n);
}

function renderPasswordPrompt(net) {
  const onSubmit = (e) => {
    e.preventDefault();
    const pass = e.target.pass ? e.target.pass.value : '';
    if (net.secure && !pass) return;
    submitSta(net.ssid, pass);
  };
  return el('form', { class: 'netpass', onsubmit: onSubmit },
    net.secure
      ? el('input', {
          type: 'password',
          name: 'pass',
          placeholder: 'Password for ' + net.ssid,
          autocomplete: 'new-password',
          autofocus: 'true',
        })
      : el('p', { class: 'muted' }, net.ssid + ' is an open network.'),
    el('button', { type: 'submit' }, 'Connect'),
  );
}

function renderManualForm() {
  const onSubmit = (e) => {
    e.preventDefault();
    const ssid = e.target.ssid.value.trim();
    if (ssid) submitSta(ssid, e.target.pass.value);
  };
  return el('div', {},
    el('form', { onsubmit: onSubmit },
      el('label', { for: 'ssid' }, 'Network name'),
      el('input', { type: 'text', name: 'ssid', autocomplete: 'off' }),
      el('label', { for: 'pass' }, 'Password'),
      el('input', { type: 'password', name: 'pass',
                    autocomplete: 'new-password' }),
      el('button', { type: 'submit' }, 'Connect'),
    ),
    el('div', { class: 'actions' },
      el('button', {
        class: 'linklike',
        onclick: () => { manualEntry = false; render(); },
      }, 'Back to network list')),
  );
}

function renderNetworkPicker() {
  if (manualEntry) return renderManualForm();
  if (nets === undefined) scanNetworks();

  const kids = [];
  if (nets == null) {
    kids.push(el('p', { class: 'muted' }, 'Scanning for networks…'));
  } else if (!nets.length) {
    kids.push(el('p', { class: 'muted' }, 'No networks found.'));
  } else {
    const list = el('div', { class: 'netlist' });
    for (const n of nets) {
      const selected = n.ssid === selectedSsid;
      list.append(el('button', {
        class: 'net' + (selected ? ' selected' : ''),
        onclick: () => {
          selectedSsid = selected ? null : n.ssid;
          render();
        },
      },
        el('span', {}, n.ssid),
        el('span', { class: 'sig' }, signalDots(n.rssi))));
      if (selected) list.append(renderPasswordPrompt(n));
    }
    kids.push(list);
  }
  kids.push(el('div', { class: 'actions' },
    el('button', {
      class: 'linklike',
      onclick: () => { selectedSsid = null; scanNetworks(); },
    }, 'Rescan'),
    el('button', {
      class: 'linklike',
      onclick: () => { manualEntry = true; render(); },
    }, 'Enter name manually')));
  return el('div', {}, ...kids);
}

// ----- shared sections -------------------------------------------------------

function renderStatusList() {
  const s = status || {};
  const hostname = s.hostname || s.device_id || '';
  return el('dl', {},
    el('div', { class: 'row' }, el('dt', {}, 'mode'),
      el('dd', {}, s.mode || '-')),
    el('div', { class: 'row' }, el('dt', {}, 'state'),
      el('dd', {}, s.state || '-')),
    el('div', { class: 'row' }, el('dt', {}, 'ssid'),
      el('dd', {}, s.ssid || '-')),
    el('div', { class: 'row' }, el('dt', {}, 'ip'),
      el('dd', {}, s.ip || '-')),
    el('div', { class: 'row' }, el('dt', {}, 'hostname'),
      el('dd', {}, hostname ? hostname + '.local' : '-')),
  );
}

function renderHostnameSection() {
  const s = status || {};
  const hostname = s.hostname || s.device_id || '';
  const onSubmit = (e) => {
    e.preventDefault();
    const name = e.target.name.value.trim();
    if (name) submitHostname(name);
  };
  return el('section', {},
    el('h2', {}, 'Hostname'),
    el('p', { class: 'muted' },
      'mDNS name. Defaults to the device ID. Applies on the next ' +
      'Wi-Fi restart; you will re-pair from the new name.'),
    el('form', { onsubmit: onSubmit },
      el('label', { for: 'name' }, 'Hostname'),
      el('input', { type: 'text', name: 'name',
                    value: hostname,
                    autocomplete: 'off',
                    pattern: '[A-Za-z0-9-]{1,31}',
                    maxlength: '31' }),
      el('button', { type: 'submit' }, 'Save hostname'),
    ),
  );
}

// Reveals this client's paired auth_token cookie so scripts and API clients
// can reuse the session without digging the HttpOnly cookie out of the
// browser. The cookie is only fetched (and shown) on request.
function renderCliAccessSection() {
  const body = [el('p', { class: 'muted' },
    'The device’s endpoints require this paired cookie. Reveal it to reuse ' +
    'the session from a script or the command line.')];
  if (cliToken == null) {
    body.push(el('button', { onclick: revealCliToken }, 'Show cookie'));
  } else {
    const box = el('textarea', {
      class: 'cli', readonly: 'true', rows: '2',
      onclick: (e) => e.target.select(),
    });
    box.value = cliToken;
    body.push(box,
      el('div', { class: 'actions' },
        el('button', { class: 'linklike', onclick: () => copyText(cliToken) },
          'Copy'),
        el('button', {
          class: 'linklike',
          onclick: () => { cliToken = null; render(); },
        }, 'Hide')));
  }
  return el('section', {}, el('h2', {}, 'Command-line access'), ...body);
}

function renderResetSection() {
  return el('section', {},
    el('h2', {}, 'Reset'),
    el('button', { class: 'danger', onclick: forgetAll },
      'Forget Wi-Fi and unpair all clients'),
  );
}

// ----- top-level views --------------------------------------------------------

// AP mode: the device is waiting to be pointed at a network, so the page is
// a single guided step. Everything else hides behind Advanced.
function renderSetup() {
  if (staSent) {
    const host = (status && status.hostname) ||
                 (status && status.device_id) || 'pumpbug';
    return el('div', {},
      el('section', {},
        el('h2', {}, 'Connecting'),
        el('p', {},
          'Wi-Fi details sent. Reconnect this phone or computer to your ' +
          'usual Wi-Fi, then use the link below.'),
        el('a', {
          class: 'primary-action',
          href: 'http://' + host + '.local/',
        }, 'Open the Pump Bug web app')),
      renderMsg());
  }
  return el('div', {},
    justPaired
      ? el('div', { class: 'msg ok' }, 'Paired with the device.')
      : null,
    el('section', {},
      el('h2', {}, 'Connect to your Wi-Fi'),
      el('p', { class: 'muted' },
        'Choose the Wi-Fi network for Pump Bug. Your browser will disconnect ' +
        'while Pump Bug joins it.'),
      renderNetworkPicker()),
    renderMsg(),
    el('details', {},
      el('summary', {}, 'Advanced'),
      el('section', {}, el('h2', {}, 'Status'), renderStatusList()),
      renderHostnameSection(),
      renderResetSection()),
  );
}

// STA mode: routine management, status first.
function renderManage() {
  const s = status || {};
  const failureBanner = (s.reason && s.reason !== '')
    ? el('div', { class: 'msg bad' },
        'Last Wi-Fi attempt: ' + s.reason)
    : null;
  return el('div', {},
    el('section', {},
      el('h2', {}, 'Status'),
      renderStatusList()),
    failureBanner,
    el('section', {},
      el('h2', {}, 'Network'),
      changingNetwork
        ? renderNetworkPicker()
        : el('button', {
            onclick: () => { changingNetwork = true; render(); },
          }, 'Change network')),
    renderMsg(),
    renderHostnameSection(),
    renderCliAccessSection(),
    renderResetSection(),
  );
}

function renderAuth() {
  const s = status || {};
  return staSent || s.mode === 'ap' ? renderSetup() : renderManage();
}

function render() {
  const app = document.getElementById('app');
  const title = document.getElementById('title');
  let view;
  if (authed === null) {
    view = el('p', { class: 'muted' }, 'Loading…');
  } else if (authed) {
    view = renderAuth();
  } else {
    view = renderUnauth();
  }
  app.replaceChildren(view);
  if (status && status.device_id) {
    title.textContent = status.device_id;
    document.title = status.device_id;
  }
  const focus = app.querySelector('input.pin, .netpass input');
  if (focus) focus.focus();
}

refresh();
