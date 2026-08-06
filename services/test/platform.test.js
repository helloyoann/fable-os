// End-to-end tests: boot the real server on an ephemeral port and drive it
// with fetch, cookies and all. No mocks — what passes here is what deploys.

import { test, before, after } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

import { createApp } from '../server.js';
import { matchJob, SEED_PROGRAMS } from '../lib/rebates.js';

let server;
let base;
let dataDir;

before(async () => {
  dataDir = mkdtempSync(join(tmpdir(), 'dots-services-test-'));
  server = createApp({ dataDir, adminEmails: ['boss@example.com'] });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  base = `http://127.0.0.1:${server.address().port}`;
});

after(async () => {
  await new Promise((resolve) => server.close(resolve));
  rmSync(dataDir, { recursive: true, force: true });
});

// Minimal cookie-jar client: keeps the sid cookie, never follows redirects.
function client() {
  let cookie = '';
  return async (path, { method = 'GET', form = null } = {}) => {
    const headers = {};
    if (cookie) headers.cookie = cookie;
    let body;
    if (form) {
      headers['content-type'] = 'application/x-www-form-urlencoded';
      body = new URLSearchParams(form).toString();
    }
    const res = await fetch(base + path, { method, headers, body, redirect: 'manual' });
    const setCookie = res.headers.get('set-cookie');
    if (setCookie) cookie = setCookie.split(';')[0];
    return res;
  };
}

async function csrfOf(http, path = '/dashboard') {
  const html = await (await http(path)).text();
  const m = html.match(/name="_csrf" value="([0-9a-f]+)"/);
  assert.ok(m, `no csrf token found on ${path}`);
  return m[1];
}

// ------------------------------------------------------------ matcher unit

test('matcher: heat pump completed in 2025 hits 25C, utility, and HEAR', () => {
  const job = { category: 'hvac_heat_pump', completed_on: '2025-06-01' };
  const slugs = matchJob(job, SEED_PROGRAMS).map((p) => p.slug);
  assert.ok(slugs.includes('federal-25c'));
  assert.ok(slugs.includes('utility-heat-pump'));
  assert.ok(slugs.includes('ira-hear'));
});

test('matcher: heat pump completed in 2026 misses the expired federal credit', () => {
  const job = { category: 'hvac_heat_pump', completed_on: '2026-06-01' };
  const slugs = matchJob(job, SEED_PROGRAMS).map((p) => p.slug);
  assert.ok(!slugs.includes('federal-25c'));
  assert.ok(slugs.includes('utility-heat-pump'));
});

test('matcher: category must match', () => {
  const job = { category: 'roofing', completed_on: '2025-06-01' };
  assert.equal(matchJob(job, SEED_PROGRAMS).length, 0);
});

test('matcher: inactive programs never match', () => {
  const job = { category: 'hvac_heat_pump', completed_on: '2025-06-01' };
  const inactive = SEED_PROGRAMS.map((p) => ({ ...p, active: false }));
  assert.equal(matchJob(job, inactive).length, 0);
});

// ------------------------------------------------------------ signup/login

const PRO = {
  name: 'Pat Fixer', company: 'Fixer HVAC', email: 'pat@example.com',
  password: 'hunter2hunter2', phone: '555-0100', trade: 'hvac_heat_pump', zip: '02139',
  license: 'HVAC-123',
};

test('signup creates an account and logs the member in', async () => {
  const http = client();
  const res = await http('/signup', { method: 'POST', form: PRO });
  assert.equal(res.status, 303);
  assert.match(res.headers.get('location'), /^\/dashboard/);
  const dash = await http('/dashboard');
  assert.equal(dash.status, 200);
  assert.match(await dash.text(), /Welcome back, Pat/);
});

test('duplicate email is rejected', async () => {
  const http = client();
  const res = await http('/signup', { method: 'POST', form: PRO });
  assert.equal(res.status, 400);
  assert.match(await res.text(), /already exists/);
});

test('signup validates zip, trade, and password length', async () => {
  const http = client();
  for (const bad of [
    { ...PRO, email: 'a@b.co', zip: 'abcde' },
    { ...PRO, email: 'a@b.co', trade: 'nonsense' },
    { ...PRO, email: 'a@b.co', password: 'short' },
  ]) {
    const res = await http('/signup', { method: 'POST', form: bad });
    assert.equal(res.status, 400);
  }
});

test('login rejects a wrong password and accepts the right one', async () => {
  const http = client();
  const bad = await http('/login', { method: 'POST', form: { email: PRO.email, password: 'wrong-password' } });
  assert.equal(bad.status, 401);
  const good = await http('/login', { method: 'POST', form: { email: PRO.email, password: PRO.password } });
  assert.equal(good.status, 303);
});

test('logged-out visitors are redirected away from member pages', async () => {
  const http = client();
  for (const path of ['/dashboard', '/jobs/new', '/deals']) {
    const res = await http(path);
    assert.equal(res.status, 303);
    assert.match(res.headers.get('location'), /^\/login/);
  }
});

// ------------------------------------------------------------ jobs & rebates

test('member uploads a job and sees rebate matches on it', async () => {
  const http = client();
  await http('/login', { method: 'POST', form: { email: PRO.email, password: PRO.password } });
  const csrf = await csrfOf(http, '/jobs/new');

  const res = await http('/jobs/new', {
    method: 'POST',
    form: {
      _csrf: csrf, category: 'hvac_heat_pump', completed_on: '2025-06-15', zip: '02139',
      address: '12 Main St, Cambridge MA', utility: 'Eversource',
      brand_model: 'Mitsubishi MXZ-SM42', efficiency: 'SEER2 21', project_cost: '$14,500',
      materials_cost: '9000', description: 'Full 3-zone mini-split install.',
    },
  });
  assert.equal(res.status, 303);
  const jobUrl = res.headers.get('location').split('?')[0];

  const detail = await (await http(jobUrl)).text();
  assert.match(detail, /Federal 25C/);
  assert.match(detail, /Electric utility heat pump/);
  assert.match(detail, /Eversource/);
  assert.doesNotMatch(detail, /Rebate desk \(admin\)/);
});

test('job upload requires a valid CSRF token', async () => {
  const http = client();
  await http('/login', { method: 'POST', form: { email: PRO.email, password: PRO.password } });
  const res = await http('/jobs/new', {
    method: 'POST',
    form: { _csrf: 'forged', category: 'roofing', completed_on: '2025-01-01', zip: '02139' },
  });
  assert.equal(res.status, 403);
});

test('a member cannot see another member\'s job', async () => {
  const other = client();
  await other('/signup', {
    method: 'POST',
    form: { name: 'Riley Roofer', email: 'riley@example.com', password: 'longpassword1', trade: 'roofing', zip: '02140' },
  });
  const csrf = await csrfOf(other, '/jobs/new');
  const created = await other('/jobs/new', {
    method: 'POST',
    form: { _csrf: csrf, category: 'roofing', completed_on: '2026-03-01', zip: '02140' },
  });
  const jobUrl = created.headers.get('location').split('?')[0];

  const pat = client();
  await pat('/login', { method: 'POST', form: { email: PRO.email, password: PRO.password } });
  assert.equal((await pat(jobUrl)).status, 404);
});

// ------------------------------------------------------------ admin

test('admin email gets the admin role and the rebate desk', async () => {
  const admin = client();
  const res = await admin('/signup', {
    method: 'POST',
    form: { name: 'Yoann Admin', email: 'boss@example.com', password: 'adminpassword1', trade: 'other', zip: '02138' },
  });
  assert.equal(res.status, 303);

  const jobsPage = await admin('/admin/jobs');
  assert.equal(jobsPage.status, 200);
  const html = await jobsPage.text();
  assert.match(html, /Rebate desk/);
  assert.match(html, /Pat Fixer/);

  const membersPage = await (await admin('/admin/members')).text();
  assert.match(membersPage, /pat@example\.com/);
  assert.match(membersPage, /Riley Roofer/);
});

test('regular members cannot reach admin pages', async () => {
  const http = client();
  await http('/login', { method: 'POST', form: { email: PRO.email, password: PRO.password } });
  for (const path of ['/admin/jobs', '/admin/members', '/admin/programs', '/admin/deals', '/admin/jobs.csv']) {
    assert.equal((await http(path)).status, 404, path);
  }
});

test('admin sets a rebate status and the member sees it', async () => {
  const pat = client();
  await pat('/login', { method: 'POST', form: { email: PRO.email, password: PRO.password } });
  const patDash = await (await pat('/dashboard')).text();
  const jobId = patDash.match(/\/jobs\/([0-9a-f-]{36})/)[1];

  const admin = client();
  await admin('/login', { method: 'POST', form: { email: 'boss@example.com', password: 'adminpassword1' } });
  const csrf = await csrfOf(admin, `/jobs/${jobId}`);

  const res = await admin(`/admin/jobs/${jobId}/status`, {
    method: 'POST',
    form: { _csrf: csrf, rebate_status: 'potential', admin_note: 'Filing with Eversource this week.' },
  });
  assert.equal(res.status, 303);

  const owner = client();
  await owner('/login', { method: 'POST', form: { email: PRO.email, password: PRO.password } });
  const dash = await (await owner('/dashboard')).text();
  assert.match(dash, /Potential rebate identified/);
});

test('admin CSV export includes member and match data', async () => {
  const admin = client();
  await admin('/login', { method: 'POST', form: { email: 'boss@example.com', password: 'adminpassword1' } });
  const res = await admin('/admin/jobs.csv');
  assert.equal(res.status, 200);
  assert.match(res.headers.get('content-type'), /text\/csv/);
  const csv = await res.text();
  assert.match(csv, /Pat Fixer/);
  assert.match(csv, /Federal 25C/);
});

test('admin posts a supplier deal and members see it', async () => {
  const admin = client();
  await admin('/login', { method: 'POST', form: { email: 'boss@example.com', password: 'adminpassword1' } });
  const csrf = await csrfOf(admin, '/admin/deals');
  const res = await admin('/admin/deals', {
    method: 'POST',
    form: { _csrf: csrf, supplier: 'Ferguson', title: '12% off PEX and fittings', code: 'DOTS12' },
  });
  assert.equal(res.status, 303);

  const member = client();
  await member('/login', { method: 'POST', form: { email: PRO.email, password: PRO.password } });
  const dealsPage = await (await member('/deals')).text();
  assert.match(dealsPage, /Ferguson/);
  assert.match(dealsPage, /DOTS12/);
});

test('admin adds a custom rebate program and it matches jobs', async () => {
  const admin = client();
  await admin('/login', { method: 'POST', form: { email: 'boss@example.com', password: 'adminpassword1' } });
  const csrf = await csrfOf(admin, '/admin/programs');
  const res = await admin('/admin/programs', {
    method: 'POST',
    form: {
      _csrf: csrf, name: 'Mass Save roofing pilot', level: 'utility',
      amount: 'Up to $500', categories: 'roofing',
    },
  });
  assert.equal(res.status, 303);

  const adminJobs = await (await admin('/admin/jobs?status=all')).text();
  assert.match(adminJobs, /Roofing/);
});

test('XSS in user input is escaped on render', async () => {
  const http = client();
  await http('/signup', {
    method: 'POST',
    form: {
      name: '<script>alert(1)</script>', email: 'xss@example.com',
      password: 'longpassword1', trade: 'plumbing', zip: '02141',
    },
  });
  const dash = await (await http('/dashboard')).text();
  assert.doesNotMatch(dash, /<script>alert/);
  assert.match(dash, /&lt;script&gt;/);
});

test('logout destroys the session', async () => {
  const http = client();
  await http('/login', { method: 'POST', form: { email: PRO.email, password: PRO.password } });
  const csrf = await csrfOf(http);
  await http('/logout', { method: 'POST', form: { _csrf: csrf } });
  const res = await http('/dashboard');
  assert.equal(res.status, 303);
  assert.match(res.headers.get('location'), /^\/login/);
});

test('healthcheck responds', async () => {
  const res = await fetch(base + '/healthz');
  assert.equal(res.status, 200);
  assert.deepEqual(await res.json(), { ok: true });
});
