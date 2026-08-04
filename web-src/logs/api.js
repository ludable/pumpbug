// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

// Fetch helpers for the device logs. Auth is the same-origin auth_token
// cookie set by pairing on /config; we never send it explicitly.
//
// One log is fetched at a time (?log=<name>). Each log is revalidated with
// If-None-Match against its own ETag, so tailing a tab is mostly 304s and
// the body only transfers when that log changes.

const etags = {};  // per-log ETag cache

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
// rest share the ?log= query route. Both honour the same If-None-Match/ETag
// dance, so the only difference is the URL.
function logUrl(log) {
  return log === 'panic'
    ? '/sys/diagnostics/panic'
    : '/sys/diagnostics?log=' + encodeURIComponent(log);
}

// Fetch one log. Returns { status, changed, data }; data only on a 200.
async function loadLog(log) {
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
  return r.ok;
}
