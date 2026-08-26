/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (c) 2026 Cryptnox SA
 *
 * test_portal_render.js — headless test of the config portal page's render logic.
 *
 * `check_portal_page.py` proves the script parses and that its element ids exist.
 * This proves it shows the right thing: which sections are visible for each
 * (mode, step, authed, pending) the device can report. That logic is a hand-written
 * pile of booleans in `render()`, it decides whether the operator can complete
 * setup at all, and nothing else tests it.
 *
 * What it is specifically there to catch:
 *   - an unauthorised browser being shown anything it could submit;
 *   - the wizard showing two steps at once, or none;
 *   - a value waiting on the panel while the page still invites another;
 *   - the admin page growing a wizard Continue button, or vice versa.
 *
 * Run from the repo root (needs python for the extractor):
 *
 *   python tools/check_portal_page.py --emit-js build/portal_page.js
 *   node tools/test_portal_render.js build/portal_page.js
 */

'use strict';

const fs = require('fs');
const path = require('path');
const vm = require('vm');
const assert = require('assert');

const src = process.argv[2];
if (!src) {
  console.error('usage: node tools/test_portal_render.js <extracted page.js>');
  process.exit(2);
}

const ids = fs.readFileSync(src.replace(/\.js$/, '.ids'), 'utf8')
  .split('\n').map(s => s.trim()).filter(Boolean);

/* ── A DOM only as real as this page needs ──────────────────────────────────
 * getElementById throws on an unknown id rather than returning null: the page
 * would throw anyway on the next property access, and failing at the lookup names
 * the id in the stack trace. */
function makeEl(id) {
  return {
    id,
    hidden: false,
    textContent: '',
    value: '',
    type: '',
    disabled: false,
    onclick: null,
    files: [],
    children: [],
    attrs: {},
    appendChild(c) { this.children.push(c); return c; },
    setAttribute(k, v) { this.attrs[k] = String(v); },
  };
}

const els = new Map(ids.map(id => [id, makeEl(id)]));
const document = {
  getElementById(id) {
    if (!els.has(id)) { throw new Error(`page asked for unknown id '${id}'`); }
    return els.get(id);
  },
  createElement(tag) { return makeEl(`<${tag}>`); },
};

/* ── A fake terminal ────────────────────────────────────────────────────────
 * Starts inert — every fetch hangs — so the synchronous render scenarios below
 * cannot be raced by a resolving promise. The authorisation handshake test at the
 * end switches it live.
 *
 * It enforces the token exactly as the device does, which is the point: the real
 * `state_get()` answers an unauthorised request with a deliberately minimal body,
 * so a page that forgets the header on ONE of its requests can never observe that
 * it was authorised. That was a real bug and this is what catches it.
 */
const srv = {
  live: false,
  granted: false,   /* the operator has typed the code on the panel */
  pending: false,
  token: null,
  asks: 0,          /* times the panel was actually told to ask */
  seen: [],         /* {url, token} per request */
};

function reply(body) {
  const text = JSON.stringify(body);
  return Promise.resolve({
    ok: true, status: 200,
    json: () => Promise.resolve(body),
    text: () => Promise.resolve(text),
  });
}

function fakeFetch(url, opts) {
  if (!srv.live) { return new Promise(() => {}); }
  const hdr = (opts && opts.headers) || {};
  const token = hdr['X-Prov-Token'];
  srv.seen.push({ url, token });

  if (url === '/api/state') {
    /* Mirrors state_get(): two different bodies, and which one you get depends
     * entirely on whether you presented the token. */
    if (srv.granted && token === srv.token) {
      return reply({
        mode: 'admin', step: 'admin', authed: true, auth_pending: false,
        version: '1.0.0', pay_eth: '0xAAA', pay_trx: 'TBBB',
        ct_eth: '0xCCC', ct_trx: 'TDDD', ssid: 'Lucky_2.4G',
        pending: '', note: '', scan_gen: 0, win: 15,
      });
    }
    return reply({
      mode: 'admin', step: 'admin', authed: false,
      auth_pending: srv.pending, version: '1.0.0',
    });
  }
  if (url === '/api/auth') {
    /* Mirrors auth_post(): already granted hands the token back WITHOUT asking
     * the panel again, which is why "Ask again" is silent once authorised. */
    if (srv.granted) { return Promise.resolve({ ok: true, status: 200, text: () => Promise.resolve(srv.token) }); }
    srv.token = 'a'.repeat(32);
    srv.pending = true;
    srv.asks++;
    /* Where the token starts existing. Requests before this point cannot carry one
     * and must not be held to it; everything after must. */
    srv.tokenAt = srv.seen.length;
    return Promise.resolve({ ok: true, status: 200, text: () => Promise.resolve(srv.token) });
  }
  if (url === '/api/scan') { return reply({ aps: [] }); }
  return Promise.resolve({ ok: true, status: 200, text: () => Promise.resolve('') });
}

const sandbox = {
  document,
  fetch: fakeFetch,
  XMLHttpRequest: function () { this.open = this.setRequestHeader = this.send = () => {}; this.upload = {}; },
  setInterval: () => 0,
  Promise, Math, JSON, String, Number, encodeURIComponent, console,
};

/* Exported by appending to the extracted source: `render` and `S` are module-scope
 * in the page, so the epilogue has to be evaluated in the same scope. */
const code = fs.readFileSync(src, 'utf8') +
  '\n;__out.render=render;__out.poll=poll;' +
  '__out.setS=function(v){S=v};__out.getS=function(){return S};';
sandbox.__out = {};
vm.createContext(sandbox);
vm.runInContext(code, sandbox, { filename: path.basename(src) });
const page = sandbox.__out;

/* ── The contract ──────────────────────────────────────────────────────────
 * `on` lists the sections that must be visible; every other section must be
 * hidden. Listing only the positives and deriving the negatives is deliberate: a
 * new section added to the page with no case here fails every scenario at once,
 * rather than silently defaulting to visible.
 */
const SECTIONS = ['s_auth', 's_pend', 's_addr', 's_ct', 's_wifi',
  's_fw', 's_done', 'nav'];

const CASES = [
  {
    name: 'wizard, not yet authorised',
    state: { mode: 'wizard', step: 'auth', authed: false, version: '1.0.0' },
    on: ['s_auth'],
    also: s => assert.strictEqual(els.get('waiting').hidden, true,
      'the waiting box should appear only once the terminal has been asked'),
  },
  {
    name: 'wizard, asked, waiting for the code on the panel',
    state: { mode: 'wizard', step: 'auth', authed: false, auth_pending: true },
    on: ['s_auth'],
    also: () => assert.strictEqual(els.get('waiting').hidden, false),
  },
  {
    name: 'wizard, authorised, address step',
    state: { mode: 'wizard', step: 'addr', authed: true, pay_eth: '', pay_trx: '' },
    on: ['s_addr', 'nav'],
  },
  {
    /* No Continue here: joining is what ends the wizard, so a button that only
     * repaints the panel would be the one thing on screen doing nothing. */
    name: 'wizard, authorised, Wi-Fi step',
    state: { mode: 'wizard', step: 'wifi', authed: true },
    on: ['s_wifi'],
  },
  {
    name: 'wizard, finished',
    state: { mode: 'wizard', step: 'done', authed: true },
    on: ['s_done'],
  },
  {
    name: 'wizard, a value waiting on the panel',
    state: { mode: 'wizard', step: 'addr', authed: true, pending: 'Ethereum payout address' },
    on: ['s_pend'],
    also: () => assert.strictEqual(els.get('pend').textContent, 'Ethereum payout address'),
  },
  {
    name: 'admin, not authorised',
    state: { mode: 'admin', step: 'admin', authed: false },
    on: ['s_auth'],
  },
  {
    name: 'admin, authorised',
    state: {
      mode: 'admin', step: 'admin', authed: true,
      pay_eth: '0xAAA', pay_trx: 'TBBB', ct_eth: '0xCCC', ct_trx: 'TDDD',
      ssid: 'My Cafe', version: '1.0.1', scan_gen: 0,
    },
    on: ['s_addr', 's_ct', 's_wifi', 's_fw'],
    also: () => {
      assert.strictEqual(els.get('cur_eth').textContent, '0xAAA');
      assert.strictEqual(els.get('cur_trx').textContent, 'TBBB');
      assert.strictEqual(els.get('cur_cte').textContent, '0xCCC');
      assert.strictEqual(els.get('cur_ctt').textContent, 'TDDD');
      assert.strictEqual(els.get('cur_ssid').textContent, 'My Cafe');
      assert.strictEqual(els.get('ver').textContent, '1.0.1');
    },
  },
  {
    name: 'admin, a value waiting on the panel',
    state: { mode: 'admin', step: 'admin', authed: true, pending: 'TRC-20 token contract' },
    on: ['s_pend'],
  },
];

let failures = 0;
for (const c of CASES) {
  /* Reset every section, so a case can only pass by being shown by render(). */
  for (const id of SECTIONS.concat(['waiting'])) { els.get(id).hidden = null; }
  page.setS(c.state);
  try {
    page.render();
    for (const id of SECTIONS) {
      const want = c.on.includes(id);
      assert.strictEqual(els.get(id).hidden, !want,
        `${id} should be ${want ? 'visible' : 'hidden'}`);
    }
    if (c.also) { c.also(c.state); }
    console.log(`  ok    ${c.name}`);
  } catch (e) {
    console.log(`  FAIL  ${c.name}\n        ${e.message}`);
    failures++;
  }
}

/* render() must not *write* address data for an unauthorised session. The device
 * already declines to send any (state_get has two different response bodies), so
 * this is the belt to that braces: if the two ever disagree, the page must not be
 * the thing that puts a payout address on screen.
 *
 * Asserted as "did not write" rather than "is empty", because a session that was
 * authorised and then dropped leaves its last values in the DOM — inside a section
 * render() has just hidden, which is not a leak and not worth clearing. */
try {
  const SENTINEL = '<<untouched>>';
  for (const id of ['cur_eth', 'cur_trx', 'cur_cte', 'cur_ctt', 'cur_ssid']) {
    els.get(id).textContent = SENTINEL;
  }
  page.setS({
    mode: 'wizard', step: 'auth', authed: false,
    /* A device that wrongly volunteered them anyway. */
    pay_eth: '0xLEAK', pay_trx: 'TLEAK', ssid: 'Leaky Cafe',
  });
  page.render();
  for (const id of ['cur_eth', 'cur_trx', 'cur_cte', 'cur_ctt', 'cur_ssid']) {
    assert.strictEqual(els.get(id).textContent, SENTINEL,
      `${id} was written for an unauthorised session`);
  }
  console.log('  ok    unauthorised session renders no addresses');
} catch (e) {
  console.log(`  FAIL  unauthorised session renders no addresses\n        ${e.message}`);
  failures++;
}

/* ── The typed send-to fields are always there ──────────────────────────────
 * They used to be hidden behind a "Manual input" button, which left the card
 * read looking like the only way to set a payout address — and that sets it to
 * the tapped card. So they are open on arrival, and render() (every 1.5 s) must
 * never close them on whoever is typing into one.
 */
try {
  page.setS({ mode: 'admin', step: 'admin', authed: true, scan_gen: 0 });
  page.render();
  els.get('in_eth').value = '0xhalf-typed';
  els.get('in_trx').value = 'Thalf-typed';
  page.render();
  assert.strictEqual(els.get('s_addr').hidden, false,
    'render() hid the send-to card on an authorised admin page');
  assert.strictEqual(els.get('in_eth').value, '0xhalf-typed',
    'a poll cleared a half-typed Ethereum address');
  assert.strictEqual(els.get('in_trx').value, 'Thalf-typed',
    'a poll cleared a half-typed Tron address');
  console.log('  ok    the typed send-to fields stay open');
} catch (e) {
  console.log(`  FAIL  the typed send-to fields stay open\n        ${e.message}`);
  failures++;
}

/* ── The Wi-Fi reveal eye ───────────────────────────────────────────────────
 * A venue passphrase typed blind on a phone and rejected tells the operator
 * nothing about which of the two got it wrong. Both directions, because a toggle
 * that only unmasks is a password left on a screen facing a room.
 */
try {
  const eye = els.get('eye');
  const p = els.get('wpass');
  p.type = 'password';
  eye.onclick.call(eye);
  assert.strictEqual(p.type, 'text', 'the eye did not reveal the password');
  assert.strictEqual(eye.attrs['aria-pressed'], 'true');
  eye.onclick.call(eye);
  assert.strictEqual(p.type, 'password', 'the eye did not mask it again');
  assert.strictEqual(eye.attrs['aria-pressed'], 'false');
  console.log('  ok    the reveal eye masks and unmasks the Wi-Fi password');
} catch (e) {
  console.log(`  FAIL  the reveal eye\n        ${e.message}`);
  failures++;
}

/* ── The authorisation handshake, end to end ────────────────────────────────
 * The regression test for a bug that shipped: the page polled /api/state without
 * the session token, so after the operator typed the admin code on the panel the
 * browser went on reporting authed:false forever and sat on "Authorise this
 * browser" with no way forward. Nothing in the static checks could see it — the
 * script parsed, every id was wired, render() was correct. Only driving the real
 * request sequence against a server that enforces the token catches it.
 */
const flush = async () => { for (let i = 0; i < 20; i++) { await new Promise(r => setImmediate(r)); } };

(async () => {
  srv.live = true;

  try {
    /* 1. Arrive. The page should ask to be authorised on its own — the operator
     *    should not have to press anything before the panel starts asking. */
    await page.poll();
    await flush();
    assert.strictEqual(srv.asks, 1, 'the page should ask the terminal on arrival');
    assert.strictEqual(els.get('s_auth').hidden, false, 'auth section should show');
    assert.strictEqual(els.get('waiting').hidden, false,
      'the waiting box should show once the terminal has been asked');
    console.log('  ok    arriving asks the terminal by itself');

    /* 2. The operator types the code on the panel. */
    srv.granted = true;
    srv.pending = false;

    /* 3. The very next poll must notice. This is the assertion that fails on the
     *    shipped bug: without the header the body stays minimal and authed:false. */
    await page.poll();
    await flush();
    assert.strictEqual(page.getS().authed, true,
      'the page never saw the grant — is the token being sent on /api/state?');
    assert.strictEqual(els.get('s_auth').hidden, true, 'auth section should be gone');
    for (const id of ['s_addr', 's_ct', 's_wifi', 's_fw']) {
      assert.strictEqual(els.get(id).hidden, false, `${id} should be visible in admin mode`);
    }
    assert.strictEqual(els.get('cur_ssid').textContent, 'Lucky_2.4G');
    console.log('  ok    the grant is picked up on the next poll');

    /* 4. And nothing the page sends the device once it holds a token may omit it.
     *    Stated over the whole sequence rather than per call, so an endpoint added
     *    later without the header fails here too. */
    const naked = srv.seen.slice(srv.tokenAt)
      .filter(r => r.url.startsWith('/api/') && !r.token);
    assert.deepStrictEqual(naked.map(r => r.url), [],
      'these requests went out with no session token');
    console.log('  ok    every /api/ request carries the token once one exists');
  } catch (e) {
    console.log(`  FAIL  authorisation handshake\n        ${e.message}`);
    failures++;
  }

  if (failures) {
    console.error(`portal render test: ${failures} failure(s)`);
    process.exit(1);
  }
  console.log(`portal render test OK (${CASES.length + 3} render scenarios + handshake)`);
})();
