// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

// Fetch helpers for the device logs. Auth is the same-origin auth_token
// cookie set by pairing on /config; we never send it explicitly.
//
// One log is fetched at a time (?log=<name>). Bounded runtime logs use ETags,
// so polling an unchanged log mostly returns 304. Power includes live readings;
// its persistent history is fetched in pages sized for the device's response
// memory budget and then cached.

const etags = {};  // per-log ETag cache
let powerHistory = null;

async function api(path, opts) {
  opts = opts || {};
  const r = await fetch(path, opts);
  let body = null;
  const ct = r.headers.get('Content-Type') || '';
  if (ct.indexOf('application/json') >= 0) {
    try { body = await r.json(); } catch (_) {}
  }
  return { ok: r.ok, status: r.status, etag: r.headers.get('ETag'), body };
}

// The panic log has its own dedicated endpoint (readable without pairing); the
// rest share the ?log= query route. Power also accepts a history offset.
function logUrl(log, offset) {
  return log === 'panic'
    ? '/sys/diagnostics/panic'
    : '/sys/diagnostics?log=' + encodeURIComponent(log) +
      (offset == null ? '' : '&offset=' + encodeURIComponent(offset));
}

async function loadPowerLog() {
  const first = await api(logUrl('power', 0), { cache: 'no-store' });
  if (!first.ok || !first.body) {
    return { status: first.status, changed: false };
  }

  const body = first.body;
  const firstPageKey = JSON.stringify(body.entries || []);
  const liveKey = JSON.stringify(body.live || null);
  if (powerHistory && powerHistory.writes === body.writes &&
      powerHistory.total === body.total &&
      powerHistory.firstPageKey === firstPageKey) {
    const changed = powerHistory.liveKey !== liveKey;
    body.entries = powerHistory.entries.slice();
    body.nextOffset = null;
    powerHistory.liveKey = liveKey;
    return { status: 200, changed, data: body };
  }

  const entries = (body.entries || []).slice();
  let next = body.nextOffset;
  while (next != null) {
    const page = await api(logUrl('power', next), { cache: 'no-store' });
    if (!page.ok || !page.body || page.body.writes !== body.writes ||
        page.body.total !== body.total || page.body.offset !== next) {
      // A concurrent clear or malformed page leaves the previous view intact;
      // the next poll retries from the first page.
      return { status: page.status || 0, changed: false };
    }
    entries.push(...(page.body.entries || []));
    next = page.body.nextOffset;
  }

  body.entries = entries;
  body.nextOffset = null;
  powerHistory = {
    writes: body.writes,
    total: body.total,
    firstPageKey,
    liveKey,
    entries: entries.slice(),
  };
  return { status: 200, changed: true, data: body };
}

// Fetch one log. Returns { status, changed, data }; data only on a 200.
async function loadLog(log) {
  if (log === 'power') return loadPowerLog();
  const headers = {};
  if (etags[log]) headers['If-None-Match'] = etags[log];
  const r = await api(logUrl(log), { headers, cache: 'no-store' });
  if (r.status === 304) return { status: 304, changed: false };
  if (r.ok) {
    if (r.etag) etags[log] = r.etag;
    return { status: 200, changed: true, data: r.body };
  }
  return { status: r.status, changed: false };
}

async function clearLog(log) {
  const r = await api('/sys/diagnostics/clear', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: 'log=' + encodeURIComponent(log),
  });
  // Drop the cached validator(s) so the next fetch re-pulls the cleared log.
  if (log === 'all') { for (const k in etags) delete etags[k]; }
  else delete etags[log];
  if (log === 'power' || log === 'all') powerHistory = null;
  return r.ok;
}
