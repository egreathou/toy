// tests/seed-players.js
//
// Seeds a running toy-server with a small, fixed set of named players in
// a few different states, for manual UI exploration. This is a dev
// utility, not an automated test — it makes no assertions, it just gets
// you into an interesting starting state quickly.
//
// Since ToY has no login form by design (the device's saved token is the
// only credential), "logging in as" a seeded player means setting that
// token directly in the browser. This script prints a ready-to-paste
// snippet for each player that does exactly that.
//
// ── Prerequisites ────────────────────────────────────────────────────────
//   toy-server running, ideally against a fresh/empty toy.dat.
//   For full isolation from anything else (automated tests, other manual
//   sessions), copy the whole build output folder somewhere separate and
//   run toy-server from there — toy.dat is opened as a relative path, so
//   a copied build gets its own data file automatically.
//
// ── Run ──────────────────────────────────────────────────────────────────
//   node seed-players.js
//
// ── Resetting ─────────────────────────────────────────────────────────────
//   Stop the server, delete toy.dat, restart it, then rerun this script.
//   Rerunning against an already-seeded toy.dat will fail with "name is
//   taken" errors — that's expected; reset first.

'use strict';

// toy-server now serves over HTTPS with a self-signed certificate (no
// public CA for a local dev server), so Node's fetch would otherwise
// reject it with DEPTH_ZERO_SELF_SIGNED_CERT. This is safe to disable
// here specifically because this script only ever talks to localhost —
// don't carry this pattern into anything that reaches a real network.
process.env.NODE_TLS_REJECT_UNAUTHORIZED = '0';

const SERVER_URL = 'https://localhost:8088';

async function registerUser(name) {
    const res = await fetch(`${SERVER_URL}/api/users`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ name }),
    });
    const body = await res.json().catch(() => ({}));
    if (!res.ok) {
        throw new Error(`Could not register "${name}": ${body.error || res.status}. Did you forget to delete toy.dat first?`);
    }
    return body; // { id, name, token }
}

async function createGame(inviterToken, partnerId) {
    const res = await fetch(`${SERVER_URL}/api/games`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', Authorization: `Bearer ${inviterToken}` },
        body: JSON.stringify({ partnerId }),
    });
    const body = await res.json().catch(() => ({}));
    if (!res.ok) {
        throw new Error(`Could not create game: ${body.error || res.status}`);
    }
    return body;
}

async function acceptGame(accepterToken, gameId) {
    const res = await fetch(`${SERVER_URL}/api/games/${gameId}/accept`, {
        method: 'POST',
        headers: { Authorization: `Bearer ${accepterToken}` },
    });
    const body = await res.json().catch(() => ({}));
    if (!res.ok) {
        throw new Error(`Could not accept game: ${body.error || res.status}`);
    }
    return body;
}

// The snippet a person pastes into their browser's console to become
// this player on that device, then reload to see it take effect.
function loginSnippet(user) {
    const session = { id: user.id, name: user.name, token: user.token };
    return `localStorage.setItem('toy.session', JSON.stringify(${JSON.stringify(session)})); location.reload();`;
}

async function run() {
    try {
        await fetch(`${SERVER_URL}/`);
    } catch {
        console.error(`\nCould not reach toy-server at ${SERVER_URL}. Is it running?\n`);
        process.exitCode = 1;
        return;
    }

    console.log('Registering players...\n');

    let alice, bob, carol, dave, eve;
    try {
        alice = await registerUser('Alice');
        bob = await registerUser('Bob');
        carol = await registerUser('Carol');
        dave = await registerUser('Dave');
        eve = await registerUser('Eve');

        // Alice invites Bob and leaves it pending — Alice sees it under
        // "Invitations sent", Bob sees it under "Waiting for your response".
        await createGame(alice.token, bob.id);

        // Carol invites Dave, and Dave accepts right away — an active game
        // for both of them.
        const carolDaveGame = await createGame(carol.token, dave.id);
        await acceptGame(dave.token, carolDaveGame.id);

        // Eve is registered but has no games at all — the empty-state view.
    } catch (err) {
        console.error(`\n${err.message}\n`);
        process.exitCode = 1;
        return;
    }

    console.log('Done. Seeded state:');
    console.log('  Alice  -> Bob    (pending — Alice: sent, Bob: waiting to accept)');
    console.log('  Carol <-> Dave   (active game, both sides)');
    console.log('  Eve              (no games — empty-state view)\n');

    console.log("Paste one of these into your browser's console to become that player");
    console.log('on this device, then reload (the snippet does that for you):\n');

    for (const user of [alice, bob, carol, dave, eve]) {
        console.log(`--- ${user.name} (id ${user.id}) ---`);
        console.log(loginSnippet(user));
        console.log();
    }
}

run();