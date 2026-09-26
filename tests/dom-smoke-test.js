// tests/dom-smoke-test.js
//
// A jsdom-driven smoke test for the ToY frontend. Fetches the real,
// currently-served index.html and app.js straight from a running
// toy-server — the same bytes a real browser would receive — and drives
// them the way a person would: typing into fields, clicking buttons.
// Nothing here is mocked. Two separate "devices" (Alice and Bob) exercise
// the full register -> search -> invite -> accept flow over actual HTTP.
//
// This test deliberately has no opinion about where your source files
// live (pwa/) or where your build places them (www/, possibly nested in
// a per-configuration folder depending on your CMake generator) — it
// only needs a running server. That also means it validates routing
// itself, not just app logic: if a static route were ever missing (this
// project has hit that before, with /app.js), this test would fail loudly
// instead of silently reading a local file that was never actually served.
//
// This is the DOM-simulation tier discussed for ToY's test strategy:
// no real browser, but a real DOM implementation (jsdom) and real
// network calls, layered on top of plain assertions in the same
// PASS/FAIL style as toy-tests.cpp.
//
// ── Prerequisites ────────────────────────────────────────────────────────
//   1. From this tests/ directory:  npm install
//   2. toy-server built and running on http://localhost:8088
//      (an empty toy.dat is fine — this test creates its own users)
//
// ── Run ──────────────────────────────────────────────────────────────────
//   node dom-smoke-test.js

'use strict';

// toy-server now serves over HTTPS with a self-signed certificate (no
// public CA for a local dev server), so Node's fetch would otherwise
// reject it with DEPTH_ZERO_SELF_SIGNED_CERT. This is safe to disable
// here specifically because this script only ever talks to localhost —
// don't carry this pattern into anything that reaches a real network.
process.env.NODE_TLS_REJECT_UNAUTHORIZED = '0';

const { JSDOM } = require('jsdom');

const SERVER_URL = 'https://localhost:8088';

// ── Minimal test framework — mirrors toy-tests.cpp's CHECK/CHECK_EQ ──────

let testsRun = 0;
let testsPassed = 0;
let testsFailed = 0;

function check(condition, description) {
  testsRun++;
  if (condition) {
    testsPassed++;
    console.log(`  PASS  ${description}`);
  } else {
    testsFailed++;
    console.log(`  FAIL  ${description}`);
  }
}

function checkEqual(actual, expected, description) {
  testsRun++;
  if (actual === expected) {
    testsPassed++;
    console.log(`  PASS  ${description}`);
  } else {
    testsFailed++;
    console.log(`  FAIL  ${description}  expected=${JSON.stringify(expected)} actual=${JSON.stringify(actual)}`);
  }
}

function printSummary() {
  console.log('\n----------------------------------------');
  console.log(`  ${testsPassed} / ${testsRun} tests passed${testsFailed > 0 ? `  (${testsFailed} FAILED)` : ''}`);
  console.log('----------------------------------------');
}

// ── Device simulation ─────────────────────────────────────────────────────
// Each call to loadDevice() is a fresh jsdom window with the real,
// server-served index.html + app.js loaded into it — i.e. a separate
// browser/device. Pass a session to simulate a returning user on that
// device. html/appJs are fetched once in run() from the live server, not
// read off disk — this test has no opinion about build layout (pwa/ vs.
// www/, generator-specific output folders, etc.) and instead validates
// exactly what a real browser would receive, including routing itself.

function loadDevice(html, appJs, session) {
  const dom = new JSDOM(html, {
    url: `${SERVER_URL}/`,
    runScripts: 'dangerously',
    resources: undefined,
  });

  const { window } = dom;

  // Node's built-in fetch doesn't resolve relative URLs against a page
  // the way a browser does — resolve them against SERVER_URL ourselves.
  window.fetch = (url, opts) => fetch(new window.URL(url, SERVER_URL), opts);

  if (session) {
    window.localStorage.setItem('toy.session', JSON.stringify(session));
  }

  const script = window.document.createElement('script');
  script.textContent = appJs;
  window.document.body.appendChild(script);

  return dom;
}

const wait = (ms) => new Promise((r) => setTimeout(r, ms));

function byId(dom, id) {
  const el = dom.window.document.getElementById(id);
  if (!el) throw new Error(`No element with id="${id}" found`);
  return el;
}

function fireEvent(dom, id, eventType) {
  byId(dom, id).dispatchEvent(new dom.window.Event(eventType, { bubbles: true, cancelable: true }));
}

function uniqueName(base) {
  return `${base}_${Math.random().toString(36).slice(2, 8)}`;
}

// ── The test itself ───────────────────────────────────────────────────────

async function checkServerReachable() {
  try {
    await fetch(`${SERVER_URL}/`);
  } catch {
    console.error(`\nCould not reach toy-server at ${SERVER_URL}.`);
    console.error('Is it built and running? (an empty toy.dat is fine — this test creates its own users)\n');
    process.exit(1);
  }
}

async function run() {
  await checkServerReachable();

  const aliceName = uniqueName('Alice');
  const bobName = uniqueName('Bob');

  // Fetch exactly what the server is serving, once, for reuse across
  // both simulated devices below.
  const html = await (await fetch(`${SERVER_URL}/`)).text();
  const appJs = await (await fetch(`${SERVER_URL}/app.js`)).text();
  check(html.includes('view-register'), 'Server serves index.html with the expected register view');
  check(appJs.includes('registerUser'), 'Server serves app.js with the expected registration logic');

  // ── Alice's device: register ──────────────────────────────────────────
  const alice = loadDevice(html, appJs);
  await wait(300);

  check(!byId(alice, 'view-register').hidden, "Alice sees the register view on first load");

  byId(alice, 'name-input').value = aliceName;
  fireEvent(alice, 'register-form', 'submit');
  await wait(400);

  check(!byId(alice, 'view-home').hidden, 'Alice lands on home after registering');
  checkEqual(byId(alice, 'home-name').textContent, aliceName, "Alice's name is shown on home");
  check(!byId(alice, 'home-empty').hidden, 'Home shows the empty state before any games exist');

  // ── Bob registers via the real API directly (a separate person/device) ─
  const bobRes = await fetch(`${SERVER_URL}/api/users`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ name: bobName }),
  });
  const bob = await bobRes.json();
  check(bobRes.ok, 'Bob registers successfully via the API');

  // ── Alice finds Bob and sends an invitation ──────────────────────────
  fireEvent(alice, 'find-partner', 'click');
  await wait(100);
  check(!byId(alice, 'view-search').hidden, "Clicking 'Find a partner' opens the search view");

  byId(alice, 'search-input').value = bobName;
  fireEvent(alice, 'search-form', 'submit');
  await wait(400);

  checkEqual(byId(alice, 'result-name').textContent, bobName, 'Search finds Bob by exact name');
  check(!byId(alice, 'search-result').hidden, 'Search result card is shown');

  fireEvent(alice, 'start-game', 'click');
  await wait(400);

  check(!byId(alice, 'view-home').hidden, 'Alice returns to home after starting a game');
  checkEqual(byId(alice, 'home-status').textContent, `Invitation sent to ${bobName}.`,
    'Home reports "Invitation sent", not "Game started" (games start pending)');
  check(byId(alice, 'home-invitations').hidden, "Alice has no invitations waiting on her");
  check(!byId(alice, 'home-sent').hidden, "Alice's sent invitation appears in the Sent group");
  check(byId(alice, 'sent-list').textContent.includes(bobName), "Sent group names Bob specifically");

  // ── Bob's device: a returning user loading with a saved session ──────
  const bobDevice = loadDevice(html, appJs, bob);
  await wait(400);

  check(!byId(bobDevice, 'view-home').hidden, 'Bob skips registration — his session is already saved');
  check(!byId(bobDevice, 'home-invitations').hidden, "Bob sees Alice's invitation waiting for him");
  check(byId(bobDevice, 'invitations-list').textContent.includes(aliceName),
    "The invitation names Alice specifically");

  // ── Bob accepts ────────────────────────────────────────────────────────
  const acceptButton = byId(bobDevice, 'invitations-list').querySelector('button');
  check(acceptButton !== null, 'An Accept button is present on the invitation');
  acceptButton.dispatchEvent(new bobDevice.window.Event('click', { bubbles: true }));
  await wait(400);

  checkEqual(byId(bobDevice, 'home-status').textContent, `You're now connected with ${aliceName}.`,
    'Bob sees confirmation after accepting');
  check(!byId(bobDevice, 'home-active').hidden, 'The game now appears in Bob\'s Active group');
  check(byId(bobDevice, 'home-invitations').hidden, "Bob's invitation list is empty again after accepting");

  printSummary();
  process.exitCode = testsFailed === 0 ? 0 : 1;
}

run().catch((err) => {
  console.error('\nTest run crashed:', err);
  process.exitCode = 1;
});