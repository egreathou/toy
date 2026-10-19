// ToY — app.js
// Three responsibilities, kept separate: API calls, view management,
// and (later) notification polling. Implements registration ("Add User")
// and partner search / game creation, with the view switching between them.

'use strict';

// ── Session storage ─────────────────────────────────────────────────────
// The token is the only credential — no passwords, no recovery.
// Stored in localStorage so it survives reloads on this device.

const Session = {
  KEY: 'toy.session',

  get() {
    try {
      const raw = localStorage.getItem(Session.KEY);
      return raw ? JSON.parse(raw) : null;
    } catch {
      return null;
    }
  },

  set(session) {
    localStorage.setItem(Session.KEY, JSON.stringify(session));
  },

  clear() {
    localStorage.removeItem(Session.KEY);
  },
};

// ── API layer ────────────────────────────────────────────────────────────

function authHeader() {
  const token = Session.get()?.token;
  return token ? { Authorization: `Bearer ${token}` } : {};
}

const Api = {
  async registerUser(name) {
    const res = await fetch('/api/users', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ name }),
    });

    const body = await res.json().catch(() => ({}));

    if (!res.ok) {
      throw new ApiError(res.status, body.error || 'Something went wrong.');
    }

    return body; // { id, name, token }
  },

  // Takes an explicit token rather than reading Session, since at the point
  // this is called (restoring on a fresh device) there is no session yet.
  async whoAmI(token) {
    const res = await fetch('/api/me', {
      headers: { Authorization: `Bearer ${token}` },
    });

    const body = await res.json().catch(() => ({}));

    if (!res.ok) {
      throw new ApiError(res.status, body.error || 'Something went wrong.');
    }

    return body; // { id, name }
  },

  async searchUser(name) {
    const res = await fetch(`/api/users/search?name=${encodeURIComponent(name)}`, {
      headers: authHeader(),
    });

    const body = await res.json().catch(() => ({}));

    if (!res.ok) {
      throw new ApiError(res.status, body.error || 'Something went wrong.');
    }

    return body; // { id, name }
  },

  async createGame(partnerId) {
    const res = await fetch('/api/games', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json', ...authHeader() },
      body: JSON.stringify({ partnerId }),
    });

    const body = await res.json().catch(() => ({}));

    if (!res.ok) {
      throw new ApiError(res.status, body.error || 'Something went wrong.');
    }

    return body; // { id, player1, player1Name, player2, player2Name, status }
  },

  async listGames() {
    const res = await fetch('/api/games', { headers: authHeader() });
    const body = await res.json().catch(() => ([]));

    if (!res.ok) {
      throw new ApiError(res.status, body.error || 'Something went wrong.');
    }

    return body; // [{ id, player1, player1Name, player2, player2Name, status }]
  },

  async acceptGame(gameId) {
    const res = await fetch(`/api/games/${gameId}/accept`, {
      method: 'POST',
      headers: authHeader(),
    });

    const body = await res.json().catch(() => ({}));

    if (!res.ok) {
      throw new ApiError(res.status, body.error || 'Something went wrong.');
    }

    return body; // { id, status }
  },

  async listEvents(gameId) {
    const res = await fetch(`/api/games/${gameId}/events`, { headers: authHeader() });
    const body = await res.json().catch(() => ([]));

    if (!res.ok) {
      throw new ApiError(res.status, body.error || 'Something went wrong.');
    }

    return body; // [{ id, userId, ts, status, matched, score?, matchedEventId? }]
  },

  async sendEvent(gameId) {
    const res = await fetch(`/api/games/${gameId}/events`, {
      method: 'POST',
      headers: authHeader(),
    });

    const body = await res.json().catch(() => ({}));

    if (!res.ok) {
      throw new ApiError(res.status, body.error || 'Something went wrong.');
    }

    return body; // { id, gameId, matched, matchedEventId?, score? }
  },
};

class ApiError extends Error {
  constructor(status, message) {
    super(message);
    this.status = status;
  }
}

// Mirrors Status::PENDING_INVITE in toy.h — a game with this bit set is
// awaiting acceptance by player2. Update both if that bit ever changes.
const Status = { PENDING_INVITE: 0x00000020 };

// Mirrors MATCH_SPAN_SECONDS in toy.h — used to derive the human-readable
// time gap (Δ) from the score the server returns, since the server sends
// score rather than the raw gap. score = MATCH_SPAN_SECONDS - gap.
const MATCH_SPAN_SECONDS = 3600;

// Mirrors EventStatus in toy.h. Matching is decided instantly (ev.matched
// is authoritative for that); EXPIRED only ever comes from the server's
// periodic sweep, once an unmatched event ages past MATCH_SPAN_SECONDS.
const EventStatus = { EXPIRED: 2 };

// Maps a failed registration onto a message a person can act on.
function registerErrorMessage(err) {
  switch (err.status) {
    case 409: return 'That name is taken. Try another.';
    case 422: return 'That name is too long.';
    case 507: return 'ToY is full right now. Please try again later.';
    default:  return 'Could not reach the server. Check your connection and try again.';
  }
}

// Maps a failed restore-from-code attempt onto a message a person can act on.
function restoreErrorMessage(err) {
  switch (err.status) {
    case 401: return "That code didn't work. Check that you copied all of it.";
    default:  return 'Could not reach the server. Check your connection and try again.';
  }
}

// Maps a failed partner search onto a message a person can act on.
function searchErrorMessage(err) {
  switch (err.status) {
    case 404: return 'No one found with that name.';
    case 400: return 'Enter a name to search.';
    default:  return 'Could not reach the server. Check your connection and try again.';
  }
}

// Maps a failed game creation onto a message a person can act on.
function createGameErrorMessage(err, partnerName) {
  switch (err.status) {
    case 409: return `You already have a game with ${partnerName}.`;
    case 403: return `That match isn't available right now.`;
    case 404: return `${partnerName} isn't available right now.`;
    case 507: return 'ToY is full right now. Please try again later.';
    default:  return 'Could not reach the server. Check your connection and try again.';
  }
}

// Maps a failed invitation acceptance onto a message a person can act on.
function acceptErrorMessage(err) {
  switch (err.status) {
    case 409: return 'That invitation was already accepted.';
    case 403: return 'Only the invited player can accept this.';
    case 404: return 'That invitation no longer exists.';
    default:  return 'Could not reach the server. Check your connection and try again.';
  }
}

// Maps a failed event submission onto a message a person can act on.
// Deliberately doesn't state an exact "next available" time even for the
// rate limit — a little uncertainty fits the spirit of the game better
// than a countdown would.
function sendEventErrorMessage(err) {
  switch (err.status) {
    case 429: return "You've already reached out this hour. Try again a bit later.";
    case 403: return 'This game is not active yet.';
    default:  return 'Could not reach the server. Check your connection and try again.';
  }
}

// ── View management ──────────────────────────────────────────────────────

const Views = {
  register: document.getElementById('view-register'),
  home:     document.getElementById('view-home'),
  search:   document.getElementById('view-search'),
  game:     document.getElementById('view-game'),
  restore:  document.getElementById('view-restore'),
};

function showView(name) {
  for (const view of Object.values(Views)) {
    view.hidden = view !== Views[name];
  }
}

// ── Register view ────────────────────────────────────────────────────────

const registerForm    = document.getElementById('register-form');
const nameInput       = document.getElementById('name-input');
const registerSubmit  = document.getElementById('register-submit');
const registerError   = document.getElementById('register-error');

function showRegisterError(message) {
  registerError.textContent = message;
  registerError.hidden = false;
}

function clearRegisterError() {
  registerError.hidden = true;
  registerError.textContent = '';
}

registerForm.addEventListener('submit', async (event) => {
  event.preventDefault();
  clearRegisterError();

  const name = nameInput.value.trim();
  if (!name) {
    showRegisterError('Enter a name to continue.');
    return;
  }

  registerSubmit.disabled = true;
  nameInput.disabled = true;

  try {
    const user = await Api.registerUser(name);
    Session.set(user);
    enterHome(user);
  } catch (err) {
    showRegisterError(err instanceof ApiError
      ? registerErrorMessage(err)
      : 'Could not reach the server. Check your connection and try again.');
  } finally {
    registerSubmit.disabled = false;
    nameInput.disabled = false;
  }
});

// ── Join from another device (same device-key model, second device) ───────────
// There's no password to recover — the token already stored on your first
// device IS the credential. This just lets you copy it onto a second one.

const showRestore    = document.getElementById('show-restore');
const hideRestore     = document.getElementById('hide-restore');
const restoreForm     = document.getElementById('restore-form');
const restoreInput    = document.getElementById('restore-input');
const restoreSubmit   = document.getElementById('restore-submit');
const restoreError    = document.getElementById('restore-error');

function showRestoreError(message) {
  restoreError.textContent = message;
  restoreError.hidden = false;
}

function clearRestoreError() {
  restoreError.hidden = true;
  restoreError.textContent = '';
}

// A plain link rather than a button — it's navigation, not an action.
// preventDefault keeps the href="#" from touching the URL or scroll position.
showRestore.addEventListener('click', (event) => {
  event.preventDefault();
  restoreInput.value = '';
  clearRestoreError();
  showView('restore');
  restoreInput.focus();
});

hideRestore.addEventListener('click', () => {
  showView('register');
});

restoreForm.addEventListener('submit', async (event) => {
  event.preventDefault();
  clearRestoreError();

  const token = restoreInput.value.trim();
  if (!token) {
    showRestoreError('Paste your code to continue.');
    return;
  }

  restoreSubmit.disabled = true;
  restoreInput.disabled = true;

  try {
    const identity = await Api.whoAmI(token);
    const user = { id: identity.id, name: identity.name, token };
    Session.set(user);
    restoreInput.value = '';
    enterHome(user);
  } catch (err) {
    showRestoreError(err instanceof ApiError
      ? restoreErrorMessage(err)
      : 'Could not reach the server. Check your connection and try again.');
  } finally {
    restoreSubmit.disabled = false;
    restoreInput.disabled = false;
  }
});

// ── Home view ─────────────────────────────────────────────────────────────
// Shows three groups sourced from GET /api/games: invitations waiting on
// this player to accept, invitations this player sent that are still
// pending, and active games. A minimal slice of the full game list — enough
// to close the invite loop without building the whole thing yet.

const homeName        = document.getElementById('home-name');
const homeStatus      = document.getElementById('home-status');
const homeEmpty       = document.getElementById('home-empty');
const findPartner     = document.getElementById('find-partner');
const invitationsGroup = document.getElementById('home-invitations');
const invitationsList  = document.getElementById('invitations-list');
const sentGroup         = document.getElementById('home-sent');
const sentList          = document.getElementById('sent-list');
const activeGroup       = document.getElementById('home-active');
const activeList        = document.getElementById('active-list');

function enterHome(user) {
  homeName.textContent = user.name;
  showView('home');
  refreshHomeGames();
}

function showHomeStatus(message) {
  homeStatus.textContent = message;
  homeStatus.hidden = false;
}

// The name of whichever player in a game isn't the current user.
function otherPlayerName(game) {
  const myId = Session.get()?.id;
  return game.player1 === myId ? game.player2Name : game.player1Name;
}

// Builds one <li> for a game group. Pass onAccept for an actionable row
// (invitations to accept), onOpen for a row that navigates into a game
// (active games), or tagLabel for a plain read-only status row.
function renderGameRow(game, { onAccept, onOpen, tagLabel } = {}) {
  const li = document.createElement('li');
  li.className = 'game-row';

  if (onOpen) {
    const btn = document.createElement('button');
    btn.className = 'game-row-button';
    btn.type = 'button';

    const name = document.createElement('span');
    name.className = 'game-row__name';
    name.textContent = otherPlayerName(game);
    btn.appendChild(name);

    const chevron = document.createElement('span');
    chevron.className = 'game-row__chevron';
    chevron.textContent = '›';
    chevron.setAttribute('aria-hidden', 'true');
    btn.appendChild(chevron);

    btn.addEventListener('click', () => onOpen(game));
    li.appendChild(btn);
    return li;
  }

  const name = document.createElement('span');
  name.className = 'game-row__name';
  name.textContent = otherPlayerName(game);
  li.appendChild(name);

  if (onAccept) {
    const btn = document.createElement('button');
    btn.className = 'button button--small';
    btn.type = 'button';
    btn.textContent = 'Accept';
    btn.addEventListener('click', () => onAccept(game, btn));
    li.appendChild(btn);
  } else if (tagLabel) {
    const tag = document.createElement('span');
    tag.className = 'game-row__tag';
    tag.textContent = tagLabel;
    li.appendChild(tag);
  }

  return li;
}

async function acceptInvitation(game, btn) {
  btn.disabled = true;
  try {
    await Api.acceptGame(game.id);
    showHomeStatus(`You're now connected with ${otherPlayerName(game)}.`);
    await refreshHomeGames();
  } catch (err) {
    showHomeStatus(err instanceof ApiError
      ? acceptErrorMessage(err)
      : 'Could not reach the server. Check your connection and try again.');
    btn.disabled = false;
  }
}

function renderGameGroup(groupEl, listEl, games, rowOptions) {
  groupEl.hidden = games.length === 0;
  listEl.innerHTML = '';
  for (const game of games) {
    listEl.appendChild(renderGameRow(game, rowOptions));
  }
}

async function refreshHomeGames() {
  const session = Session.get();
  if (!session) return;

  let games;
  try {
    games = await Api.listGames();
  } catch {
    return; // non-fatal — leave whatever was last shown on screen
  }

  // Games have no timestamp field — id order is creation order, so
  // sorting by id descending is the closest available proxy for
  // "most recent first". Revisit once event history exposes real
  // last-activity timestamps for active games.
  const byNewestFirst = (a, b) => b.id - a.id;

  const myId = session.id;
  const toAccept = games.filter(g => g.player2 === myId && (g.status & Status.PENDING_INVITE)).sort(byNewestFirst);
  const sent     = games.filter(g => g.player1 === myId && (g.status & Status.PENDING_INVITE)).sort(byNewestFirst);
  const active   = games.filter(g => g.status === 0).sort(byNewestFirst);

  renderGameGroup(invitationsGroup, invitationsList, toAccept, { onAccept: acceptInvitation });
  renderGameGroup(sentGroup, sentList, sent, { tagLabel: 'Waiting' });
  renderGameGroup(activeGroup, activeList, active, { onOpen: openGame });

  homeEmpty.hidden = games.length > 0;
}

findPartner.addEventListener('click', () => {
  resetSearchView();
  showView('search');
});

// ── Device code panel (use on another device) ────────────────────────────
// The token already in this device's Session IS the credential — this just
// surfaces it so it can be copied onto a second device's restore screen.

const showDeviceCode      = document.getElementById('show-device-code');
const deviceCodePanel     = document.getElementById('device-code-panel');
const deviceCodeValue     = document.getElementById('device-code-value');
const copyDeviceCode      = document.getElementById('copy-device-code');
const copyDeviceCodeStatus = document.getElementById('copy-device-code-status');

showDeviceCode.addEventListener('click', () => {
  deviceCodeValue.textContent = Session.get()?.token ?? '';
  copyDeviceCodeStatus.hidden = true;
  deviceCodePanel.hidden = !deviceCodePanel.hidden;
});

copyDeviceCode.addEventListener('click', async () => {
  const token = Session.get()?.token ?? '';
  try {
    await navigator.clipboard.writeText(token);
  } catch {
    // Clipboard access can be denied (permissions, non-HTTPS context, etc.)
    // — the value is already selectable text on screen either way, so this
    // is a convenience that degrades gracefully, not a hard requirement.
  }
  copyDeviceCodeStatus.hidden = false;
});

// ── Search view ──────────────────────────────────────────────────────────

const searchBack     = document.getElementById('search-back');
const searchForm     = document.getElementById('search-form');
const searchInput    = document.getElementById('search-input');
const searchSubmit   = document.getElementById('search-submit');
const searchError    = document.getElementById('search-error');
const searchResult   = document.getElementById('search-result');
const resultName     = document.getElementById('result-name');
const startGame      = document.getElementById('start-game');
const searchAgain    = document.getElementById('search-again');
const startGameError = document.getElementById('start-game-error');

let foundPartner = null; // { id, name } — set once a search succeeds

function showSearchError(message) {
  searchError.textContent = message;
  searchError.hidden = false;
}

function clearSearchError() {
  searchError.hidden = true;
  searchError.textContent = '';
}

function showStartGameError(message) {
  startGameError.textContent = message;
  startGameError.hidden = false;
}

function clearStartGameError() {
  startGameError.hidden = true;
  startGameError.textContent = '';
}

function resetSearchView() {
  searchForm.hidden = false;
  searchResult.hidden = true;
  searchInput.value = '';
  foundPartner = null;
  clearSearchError();
  clearStartGameError();
}

searchBack.addEventListener('click', () => {
  showView('home');
});

searchForm.addEventListener('submit', async (event) => {
  event.preventDefault();
  clearSearchError();

  const name = searchInput.value.trim();
  if (!name) {
    showSearchError('Enter a name to search.');
    return;
  }

  const session = Session.get();
  if (name === session.name) {
    showSearchError("That's your own name — enter your partner's.");
    return;
  }

  searchSubmit.disabled = true;
  searchInput.disabled = true;

  try {
    const partner = await Api.searchUser(name);
    foundPartner = partner;
    resultName.textContent = partner.name;
    searchForm.hidden = true;
    searchResult.hidden = false;
  } catch (err) {
    showSearchError(err instanceof ApiError
      ? searchErrorMessage(err)
      : 'Could not reach the server. Check your connection and try again.');
  } finally {
    searchSubmit.disabled = false;
    searchInput.disabled = false;
  }
});

searchAgain.addEventListener('click', () => {
  resetSearchView();
});

startGame.addEventListener('click', async () => {
  if (!foundPartner) return;
  clearStartGameError();

  startGame.disabled = true;
  searchAgain.disabled = true;

  try {
    await Api.createGame(foundPartner.id);
    showHomeStatus(`Invitation sent to ${foundPartner.name}.`);
    showView('home');
    refreshHomeGames();
  } catch (err) {
    showStartGameError(err instanceof ApiError
      ? createGameErrorMessage(err, foundPartner.name)
      : 'Could not reach the server. Check your connection and try again.');
  } finally {
    startGame.disabled = false;
    searchAgain.disabled = false;
  }
});

// ── Game detail view ───────────────────────────────────────────────────────
// Reached only from an active game row on home (onOpen). The game object
// passed in already has both player names, so no extra fetch is needed
// just to open the view — only the event history is fetched here.

const gameBack        = document.getElementById('game-back');
const gameTitle       = document.getElementById('game-title');
const sendEventBtn    = document.getElementById('send-event');
const sendEventStatus = document.getElementById('send-event-status');
const sendEventError  = document.getElementById('send-event-error');
const gameLogEmpty    = document.getElementById('game-log-empty');
const gameLog         = document.getElementById('game-log');

let currentGame = null;

function nameForPlayer(game, userId) {
  return game.player1 === userId ? game.player1Name : game.player2Name;
}

// "14:02" on the same day, "08-09 14:02" otherwise — kept short since
// the log is meant to be scanned, not read line by line.
function formatEventTime(tsSeconds) {
  const d = new Date(tsSeconds * 1000);
  const now = new Date();
  const time = d.toLocaleTimeString(undefined, { hour: '2-digit', minute: '2-digit' });
  if (d.toDateString() === now.toDateString()) return time;
  const date = `${String(d.getMonth() + 1).padStart(2, '0')}-${String(d.getDate()).padStart(2, '0')}`;
  return `${date} ${time}`;
}

// Server sends score, not the raw time gap — derive it back for display.
// "Δ38s" under a minute, "Δ12m04s" otherwise.
function formatDelta(score) {
  const gap = MATCH_SPAN_SECONDS - score;
  if (gap < 60) return `Δ${gap}s`;
  const m = Math.floor(gap / 60);
  const s = gap % 60;
  return `Δ${m}m${String(s).padStart(2, '0')}s`;
}

// Three real outcomes an event can be in: matched (decided the instant the
// second event landed), expired with no match (the server's sweep caught
// it after MATCH_SPAN_SECONDS with nothing arriving), or still open — sent,
// within the window, could still match if the partner responds. This is
// what makes "check back yourself" a real answer rather than a guess.
function eventLogLabel(ev) {
  if (ev.matched) return `match ${formatDelta(ev.score)} score ${ev.score}`;
  if (ev.status === EventStatus.EXPIRED) return 'no match';
  return 'open';
}

function openGame(game) {
  currentGame = game;
  gameTitle.textContent = `You and ${otherPlayerName(game)}`;
  sendEventStatus.hidden = true;
  sendEventError.hidden = true;
  showView('game');
  refreshGameLog();
}

gameBack.addEventListener('click', () => {
  currentGame = null;
  showView('home');
});

async function refreshGameLog() {
  if (!currentGame) return;

  let events;
  try {
    events = await Api.listEvents(currentGame.id);
  } catch {
    return; // non-fatal — leave whatever was last shown
  }

  events.sort((a, b) => b.ts - a.ts); // real timestamp sort, newest first

  gameLogEmpty.hidden = events.length > 0;
  gameLog.hidden = events.length === 0;
  gameLog.innerHTML = '';

  for (const ev of events) {
    const li = document.createElement('li');
    li.className = 'game-log-row';

    const time = document.createElement('span');
    time.textContent = formatEventTime(ev.ts);
    li.appendChild(time);

    const name = document.createElement('span');
    name.textContent = nameForPlayer(currentGame, ev.userId);
    li.appendChild(name);

    const info = document.createElement('span');
    info.className = 'game-log-row__info';
    info.textContent = eventLogLabel(ev);
    li.appendChild(info);

    gameLog.appendChild(li);
  }
}

sendEventBtn.addEventListener('click', async () => {
  if (!currentGame) return;
  sendEventStatus.hidden = true;
  sendEventError.hidden = true;
  sendEventBtn.disabled = true;

  try {
    const result = await Api.sendEvent(currentGame.id);
    sendEventStatus.textContent = result.matched
      ? `It's a match — ${formatDelta(result.score)} apart, score ${result.score}.`
      : 'Sent.';
    sendEventStatus.hidden = false;
    await refreshGameLog();
  } catch (err) {
    sendEventError.textContent = err instanceof ApiError
      ? sendEventErrorMessage(err)
      : 'Could not reach the server. Check your connection and try again.';
    sendEventError.hidden = false;
  } finally {
    sendEventBtn.disabled = false;
  }
});

// ── Init ─────────────────────────────────────────────────────────────────

(function init() {
  const session = Session.get();
  if (session) {
    enterHome(session);
  } else {
    showView('register');
  }
})();
