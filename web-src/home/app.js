// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

// Disable the auth-protected links until this client is paired. The
// auth_token cookie is HttpOnly
// (see Auth.cpp), so it isn't visible to document.cookie — ask the server
// instead via the public /auth/whoami endpoint, which reads the cookie the
// browser sends automatically. Static assets are public, so an unpaired
// client still loads this hub; we just dim those links until pairing
// completes. Fail open on a network error (leave links enabled).
fetch('/auth/whoami', { headers: { Accept: 'application/json' } })
  .then((r) => r.json())
  .then((d) => { if (!d.authenticated) lock(); })
  .catch(() => {});

function lock() {
  const note = document.getElementById('lock');
  if (note) note.hidden = false;
  for (const a of document.querySelectorAll('a.card[data-protected]')) {
    a.classList.add('disabled');
    a.setAttribute('aria-disabled', 'true');
    a.removeAttribute('href');
  }
}
