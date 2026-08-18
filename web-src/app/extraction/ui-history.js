// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

// Newest-first pagination with a cheap in-app ETag revalidation. Each page's
// validator is kept in memory (shotsEtags) and sent as If-None-Match, so an
// unchanged in-app refresh is a 304 with no directory walk on the device. The
// map lives only in memory, so a full page reload simply re-fetches page 1 (a
// 200, one bounded page) — we don't try to make reloads 304. Shot detail blobs
// are revalidated so reusing an ID after storage is erased cannot serve an old
// shot from the browser cache.
const SHOTS_LIMIT = 20;
const SHOTS_FETCH_TIMEOUT_MS = 10000;

// Monotonic token so an older /shots response can't overwrite a
// newer one. Each call captures myToken; an awaited result is
// discarded if shotsRefreshSeq has advanced past it.
let shotsRefreshSeq = 0;
let shotsRefreshInFlight = false;
let shotsRefreshPending = false;
let shotsRefreshPendingForce = false;
let shotsLoadMoreInFlight = false;

// In-memory ETag per page URL (lost on reload, by design).
// We do not rely on the browser HTTP cache here: fetch/cache behavior around
// auth, chunked JSON, and reloads is opaque, while this map only validates
// pages currently applied to UI state.
const shotsEtags = {};

async function fetchWithTimeout(path, opts, timeoutMs) {
  const ctrl = new AbortController();
  const timer = setTimeout(() => ctrl.abort(),
                           timeoutMs != null ? timeoutMs : SHOTS_FETCH_TIMEOUT_MS);
  try {
    return await fetch(path, { ...(opts || {}), signal: ctrl.signal });
  } finally {
    clearTimeout(timer);
  }
}

async function fetchBinary(path) {
  // Default cache mode lets the browser revalidate a previously viewed shot
  // with its ETag instead of downloading it again.
  const r = await fetchWithTimeout(path, { cache: 'default' });
  if (r.status === 404) return null;
  if (!r.ok) throw new Error(path + ' ' + r.status);
  return parseExtraction(await r.arrayBuffer());
}

// Fetch one page (before=0 → newest). Returns { status:304 } when the cached
// ETag still matches, else { status:200, shots, hasMore, total }.
// Does NOT touch the ETag cache — the caller commits it (commitShotsEtag) only
// after it has passed its token check and actually applied the page, so a
// superseded or truncated fetch can never cache a validator for data the UI
// never displayed (which would make a later retry 304 onto stale/empty state).
async function fetchShotsPage(before) {
  let url = '/app/extraction/shots?limit=' + SHOTS_LIMIT;
  if (before) url += '&before=' + before;
  const headers = {};
  if (shotsEtags[url]) headers['If-None-Match'] = shotsEtags[url];
  const r = await fetchWithTimeout(url, { cache: 'no-store', headers });
  if (r.status === 304) return { status: 304 };
  if (!r.ok) throw new Error('shots ' + r.status);
  const j = await r.json();
  return {
    status: 200,
    url,
    etag: r.headers.get('ETag'),
    shots: Array.isArray(j.shots) ? j.shots : [],
    hasMore: !!j.hasMore,
    total: typeof j.total === 'number' ? j.total : 0,
  };
}

// Cache the page's validator. Call only after the page has been applied to UI
// state, so shotsEtags[url] always corresponds to data we actually displayed —
// a future 304 against it then correctly means "what you have is current".
function commitShotsEtag(res) {
  if (res.etag) shotsEtags[res.url] = res.etag;
}

// Refresh page 1. `force` (on entering the list) always revalidates — a 304
// keeps the cached list, a 200 replaces it. Without force (SSE STATE), the
// savedSeq check skips the request unless a new shot was saved, so a busy STATE
// stream doesn't poll every tick. Either way this resets to page 1, dropping any
// loaded older pages.
async function maybeRefreshShots(force) {
  if (view !== 'list') return;
  // /state already supplies the user-facing storage warning. Do not issue a
  // list request that can only repeat the same condition as an HTTP error.
  if (state && state.storage === 'unavailable') return;
  const seq = state ? state.savedSeq : -1;
  if (!force && shots !== null && seq === lastShotsSavedSeq) return;
  if (shotsRefreshInFlight) {
    shotsRefreshPending = true;
    shotsRefreshPendingForce = shotsRefreshPendingForce || !!force;
    return;
  }
  shotsRefreshInFlight = true;
  const myToken = ++shotsRefreshSeq;
  try {
    const res = await fetchShotsPage(0);
    if (myToken !== shotsRefreshSeq) return;  // superseded — don't apply or cache
    lastShotsSavedSeq = seq;
    if (res.status === 200) {
      shots = res.shots;
      shotsHasMore = res.hasMore;
      shotsTotal = res.total;
      commitShotsEtag(res);  // only after applying
      scheduleRender();
    }
  } catch (err) {
    if (myToken === shotsRefreshSeq) setBanner('error: ' + err.message, 'bad');
  } finally {
    shotsRefreshInFlight = false;
    if (shotsRefreshPending) {
      const pendingForce = shotsRefreshPendingForce;
      shotsRefreshPending = false;
      shotsRefreshPendingForce = false;
      maybeRefreshShots(pendingForce);
    }
  }
}

// Append the next (older) page. Pages are disjoint id ranges, so no dedupe.
async function loadMoreShots() {
  if (!shotsHasMore || !shots || !shots.length) return;
  if (shotsLoadMoreInFlight) return;
  const before = shots[shots.length - 1].id;  // lowest loaded id
  shotsLoadMoreInFlight = true;
  const myToken = ++shotsRefreshSeq;
  try {
    const res = await fetchShotsPage(before);
    if (myToken !== shotsRefreshSeq) return;  // superseded — don't apply or cache
    if (res.status === 200) {
      shots = shots.concat(res.shots);
      shotsHasMore = res.hasMore;
      shotsTotal = res.total;
      commitShotsEtag(res);  // only after applying
      scheduleRender();
    }
  } catch (err) {
    if (myToken === shotsRefreshSeq) setBanner('error: ' + err.message, 'bad');
  } finally {
    shotsLoadMoreInFlight = false;
  }
}

function switchView(next) {
  if (view === next) return;
  view = next;
  if (next !== 'detail') {
    selectedShotId = null;
    selectedShot = null;
    selectedShotError = null;
  }
  scheduleRender();
  // On entry: render the cached list immediately, then revalidate page 1 (304
  // keeps it, 200 if a shot was recorded while we were away).
  if (next === 'list') maybeRefreshShots(true);
}

async function selectShot(id) {
  selectedShotId = id;
  selectedShot = null;
  selectedShotError = null;
  view = 'detail';
  scheduleRender();
  try {
    const blob = await fetchBinary('/app/extraction/shots?id=' + id);
    if (selectedShotId !== id) return;  // user navigated away
    if (blob) {
      selectedShot = blob;
    } else {
      // fetchBinary returns null on 404 — the shot was likely
      // FIFO-evicted between the list render and this fetch.
      selectedShotError = 'not_found';
    }
    scheduleRender();
  } catch (err) {
    if (selectedShotId !== id) return;
    selectedShotError = 'error';
    setBanner('error loading shot #' + id + ': ' + err.message, 'bad');
    scheduleRender();
  }
}

// Load this shot onto the device as its on-device "last shot", so it can be
// replayed on the device for UI testing. Device-local only: it does not touch
// the shot history or other web clients (see ExtractionController).
async function setAsLastShot(id) {
  try {
    const res = await fetchWithTimeout('/app/extraction/loaded?id=' + id,
                                       { method: 'POST' });
    if (res.ok) {
      // Jump to the live view — the operator's next step is at the device
      // (replay from the LAST SHOT screen). flashBanner survives the render the
      // view switch triggers, so the confirmation actually shows.
      switchView('live');
      flashBanner('Loaded shot #' + id + ' on the device — replay it from the '
                  + 'LAST SHOT screen', 'live');
    } else {
      flashBanner('Could not load shot #' + id + ' (HTTP ' + res.status + ')',
                  'bad');
    }
  } catch (err) {
    flashBanner('Could not load shot #' + id + ': ' + err.message, 'bad');
  }
}

// Render an enum index through its label table, falling back to the raw number
// for any value the table doesn't cover (e.g. a newer firmware enum).
function labelOf(table, i) {
  return i in table ? table[i] : i;
}

// Save the already-decoded shot as JSON, entirely client-side — the browser
// parsed the binary blob to render the detail view, so there's nothing to ask
// the device for. We tag the file with the shot id (which the decoded object
// itself doesn't carry) so the download is self-describing, and map the wire
// enums (phase, end cause, flag bits, event kinds) to their labels so the file
// reads on its own. Numeric measurements stay in centigrams, as decoded.
function downloadShotJson(id, shot) {
  const readable = {
    id,
    ...shot,
    phase: labelOf(PHASE, shot.phase),
    endCause: labelOf(END_CAUSE, shot.endCause),
    flags: {
      eventsOverflowed: (shot.flags & 0x01) !== 0,
      scaleTimers: (shot.flags & RECORD_FLAG_SCALE_TIMERS) !== 0,
    },
    events: shot.events.map(ev => ({
      ...ev,
      kind: eventKindLabel(shot.version, ev.kind),
    })),
  };
  const json = JSON.stringify(readable, null, 2);
  const url = URL.createObjectURL(new Blob([json], { type: 'application/json' }));
  const a = el('a', { href: url, download: 'shot-' + id + '.json' });
  document.body.append(a);
  a.click();
  a.remove();
  URL.revokeObjectURL(url);
}

function renderList(app) {
  if (state.storage === 'unavailable') {
    return;
  }
  if (shots === null) {
    app.append(el('p', { class: 'empty' }, 'Loading shots…'));
    return;
  }
  if (!shots.length) {
    app.append(el('p', { class: 'empty' }, 'No shots recorded yet.'));
    return;
  }
  const ul = el('ul', { class: 'shotlist' });
  for (const s of shots) {
    const when = fmtDate(s.startUtcSec) || 'time unknown';
    ul.append(el('li', { onclick: () => selectShot(s.id) },
      el('div', { class: 'head' },
        el('span', { class: 'when' }, when),
        el('span', { class: 'id' }, '#' + s.id),
      ),
      el('div', { class: 'body' },
        el('span', {},
          el('span', { class: 'v' }, fmtMMSS(s.durationMs)),
          el('span', { class: 'u' }, 'mm:ss')),
        el('span', {},
          el('span', { class: 'v' }, fmtG(s.yieldCg)),
          el('span', { class: 'u' }, 'g')),
        el('span', {},
          el('span', { class: 'v' }, fmtSec(s.totalPumpOnMs)),
          el('span', { class: 'u' }, 's pump')),
      ),
    ));
  }
  const section = el('section', {},
    el('h2', {}, `Shots (${shotsTotal})`), ul);
  if (shotsHasMore) {
    section.append(el('button',
      { class: 'loadmore', onclick: () => loadMoreShots() }, 'Load older'));
  }
  app.append(section);
}

function renderDetail(app) {
  app.append(el('button', { class: 'backlink',
                            onclick: () => switchView('list') },
    '← Back to history'));

  if (selectedShotError === 'not_found') {
    app.append(el('p', { class: 'empty' },
      `Shot #${selectedShotId} is no longer available ` +
      `(likely evicted to make room for newer shots).`));
    return;
  }
  if (selectedShotError === 'error') {
    app.append(el('p', { class: 'empty' },
      `Couldn't load shot #${selectedShotId}.`));
    return;
  }
  if (!selectedShot) {
    app.append(el('p', { class: 'empty' }, 'Loading shot…'));
    return;
  }
  const e = selectedShot;
  const when = fmtDate(e.startUtcSec) || 'time unknown';
  renderShotRecord(app, e, { title: `Shot #${selectedShotId} · ${when}` });
  // Send this shot to the device's Last Shot slot for on-device replay.
  app.append(el('button',
    { class: 'backlink', onclick: () => setAsLastShot(selectedShotId) },
    'Set as last shot (replay on device)'));
  // Save the decoded shot locally — no device round-trip, we already have it.
  app.append(el('button',
    { class: 'backlink', onclick: () => downloadShotJson(selectedShotId, e) },
    'Download shot data (JSON)'));
}
