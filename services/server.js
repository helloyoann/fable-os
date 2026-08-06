// Dots Services Network — services.dotsconstruction.com
//
// A dependency-free Node.js web platform for the Dots Builders home-service
// professional network: member signup/login, job uploads, rebate matching
// against a program catalog, an admin rebate desk, and a supplier-deals
// board. Plain server-rendered HTML forms — no client framework, no npm
// installs, no build step. `node server.js` is the whole deployment.

import { createServer } from 'node:http';
import { readFileSync, existsSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

import { Store } from './lib/store.js';
import {
  hashPassword, verifyPassword,
  createSession, getSession, destroySession,
  loginAllowed, loginFailed, loginSucceeded,
} from './lib/auth.js';
import { JOB_CATEGORIES, REBATE_STATUSES, seedPrograms, matchJob } from './lib/rebates.js';
import * as views from './lib/views.js';

const MODULE_DIR = dirname(fileURLToPath(import.meta.url));
const MAX_BODY_BYTES = 100_000;

const STATIC_TYPES = { '.css': 'text/css', '.js': 'text/javascript', '.svg': 'image/svg+xml', '.png': 'image/png' };

export function createApp(options = {}) {
  const dataDir = options.dataDir ?? process.env.DATA_DIR ?? join(MODULE_DIR, 'data');
  const adminEmails = (options.adminEmails ??
    (process.env.ADMIN_EMAILS ?? 'yoann@dotsbuilders.com').split(','))
    .map((e) => e.trim().toLowerCase())
    .filter(Boolean);

  const store = new Store(dataDir);
  seedPrograms(store);

  const users = () => store.collection('users');
  const jobs = () => store.collection('jobs');
  const programs = () => store.collection('programs');
  const deals = () => store.collection('deals');

  const activePrograms = () => programs().find((p) => p.active !== false);
  const matchesFor = (job) => matchJob(job, activePrograms());

  // ------------------------------------------------------------ routing

  const routes = [];
  const route = (method, pattern, handler) => routes.push({ method, pattern: pattern.split('/'), handler });

  function matchRoute(patternParts, pathParts) {
    if (patternParts.length !== pathParts.length) return null;
    const params = {};
    for (let i = 0; i < patternParts.length; i++) {
      const p = patternParts[i];
      if (p.startsWith(':')) params[p.slice(1)] = decodeURIComponent(pathParts[i]);
      else if (p !== pathParts[i]) return null;
    }
    return params;
  }

  // ------------------------------------------------------------ helpers

  const isHttps = (req) => Boolean(req.socket.encrypted) || req.headers['x-forwarded-proto'] === 'https';

  function baseHeaders(extra = {}) {
    return {
      'X-Content-Type-Options': 'nosniff',
      'Referrer-Policy': 'no-referrer',
      ...extra,
    };
  }

  function sendHtml(ctx, html, status = 200) {
    ctx.res.writeHead(status, baseHeaders({
      'Content-Type': 'text/html; charset=utf-8',
      'Content-Security-Policy':
        "default-src 'self'; img-src 'self' data:; style-src 'self'; base-uri 'none'; form-action 'self'; frame-ancestors 'none'",
    }));
    ctx.res.end(html);
  }

  function redirect(ctx, location) {
    ctx.res.writeHead(303, baseHeaders({ Location: location }));
    ctx.res.end();
  }

  function page(ctx, title, body, status = 200) {
    sendHtml(ctx, views.layout({
      title,
      user: ctx.user,
      csrf: ctx.session?.csrf ?? '',
      body,
      ok: ctx.url.searchParams.get('ok') ?? '',
      err: ctx.url.searchParams.get('err') ?? '',
    }), status);
  }

  const notFound = (ctx) => page(ctx, 'Not found', views.notFoundPage(), 404);

  function readBody(req) {
    return new Promise((resolve, reject) => {
      const chunks = [];
      let size = 0;
      req.on('data', (chunk) => {
        size += chunk.length;
        if (size > MAX_BODY_BYTES) {
          reject(Object.assign(new Error('body too large'), { status: 413 }));
          req.destroy();
          return;
        }
        chunks.push(chunk);
      });
      req.on('end', () => resolve(Buffer.concat(chunks).toString('utf8')));
      req.on('error', reject);
    });
  }

  async function readForm(ctx) {
    const raw = await readBody(ctx.req);
    const params = new URLSearchParams(raw);
    const form = {};
    for (const [key, value] of params) {
      // Checkbox groups (name repeated) accumulate; scalars keep first value.
      if (key in form) {
        if (Array.isArray(form[key])) form[key].push(value);
        else form[key] = [form[key], value];
      } else {
        form[key] = value;
      }
    }
    return form;
  }

  function cookieToken(req) {
    const header = req.headers.cookie ?? '';
    for (const part of header.split(';')) {
      const [name, ...rest] = part.trim().split('=');
      if (name === 'sid') return rest.join('=');
    }
    return null;
  }

  function setSessionCookie(ctx, token) {
    const attrs = [`sid=${token}`, 'Path=/', 'HttpOnly', 'SameSite=Lax', `Max-Age=${30 * 24 * 60 * 60}`];
    if (isHttps(ctx.req)) attrs.push('Secure');
    ctx.res.setHeader('Set-Cookie', attrs.join('; '));
  }

  function clearSessionCookie(ctx) {
    ctx.res.setHeader('Set-Cookie', 'sid=; Path=/; HttpOnly; SameSite=Lax; Max-Age=0');
  }

  const requireUser = (handler) => (ctx) => {
    if (!ctx.user) return redirect(ctx, '/login?err=' + encodeURIComponent('Please log in first.'));
    return handler(ctx);
  };

  const requireAdmin = (handler) => requireUser((ctx) => {
    if (ctx.user.role !== 'admin') return notFound(ctx);
    return handler(ctx);
  });

  // Authenticated POSTs must echo the session's CSRF token. Signup/login are
  // exempt (no session yet); SameSite=Lax covers them.
  const withForm = (handler) => async (ctx) => {
    ctx.form = await readForm(ctx);
    if (ctx.session && ctx.form._csrf !== ctx.session.csrf) {
      return page(ctx, 'Forbidden', `<h1>Session expired</h1><p>Please go back, reload the page, and try again.</p>`, 403);
    }
    return handler(ctx);
  };

  const clean = (value, max = 240) => String(value ?? '').trim().slice(0, max);
  const cleanMoney = (value) => {
    const s = clean(value, 20).replace(/[$,\s]/g, '');
    if (s === '') return '';
    return /^\d+(\.\d{1,2})?$/.test(s) ? s : null;
  };
  const isDate = (s) => /^\d{4}-\d{2}-\d{2}$/.test(s);

  // ------------------------------------------------------------ public pages

  route('GET', '/', (ctx) => {
    if (ctx.user) return redirect(ctx, '/dashboard');
    return page(ctx, 'Home-service pro network', views.landingPage());
  });

  route('GET', '/healthz', (ctx) => {
    ctx.res.writeHead(200, baseHeaders({ 'Content-Type': 'application/json' }));
    ctx.res.end(JSON.stringify({ ok: true }));
  });

  route('GET', '/signup', (ctx) => page(ctx, 'Join the network', views.signupPage()));

  route('POST', '/signup', withForm((ctx) => {
    const f = ctx.form;
    const values = {
      name: clean(f.name, 120), company: clean(f.company, 120),
      email: clean(f.email, 200).toLowerCase(), phone: clean(f.phone, 40),
      trade: clean(f.trade, 40), zip: clean(f.zip, 5), license: clean(f.license, 80),
    };
    const password = String(f.password ?? '');
    const fail = (err) => page(ctx, 'Join the network', views.signupPage({ values, err }), 400);

    if (!values.name) return fail('Your name is required.');
    if (!/^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(values.email)) return fail('A valid email is required.');
    if (password.length < 8) return fail('Password must be at least 8 characters.');
    if (!(values.trade in JOB_CATEGORIES)) return fail('Pick your primary trade.');
    if (!/^\d{5}$/.test(values.zip)) return fail('ZIP must be 5 digits.');
    if (users().findOne((u) => u.email === values.email)) {
      return fail('An account with that email already exists — try logging in.');
    }

    const user = users().insert({
      ...values,
      password: hashPassword(password),
      role: adminEmails.includes(values.email) ? 'admin' : 'member',
    });
    const session = createSession(store, user.id);
    setSessionCookie(ctx, session.token);
    return redirect(ctx, '/dashboard?ok=' + encodeURIComponent('Welcome to the network! Upload your first job whenever you are ready.'));
  }));

  route('GET', '/login', (ctx) => page(ctx, 'Log in', views.loginPage()));

  route('POST', '/login', withForm((ctx) => {
    const email = clean(ctx.form.email, 200).toLowerCase();
    const password = String(ctx.form.password ?? '');
    const throttleKey = `${email}|${ctx.req.socket.remoteAddress ?? ''}`;
    const fail = (err) => page(ctx, 'Log in', views.loginPage({ err }), 401);

    if (!loginAllowed(throttleKey)) return fail('Too many attempts — wait 15 minutes and try again.');
    const user = users().findOne((u) => u.email === email);
    if (!user || !verifyPassword(password, user.password)) {
      loginFailed(throttleKey);
      return fail('Wrong email or password.');
    }
    loginSucceeded(throttleKey);
    const session = createSession(store, user.id);
    setSessionCookie(ctx, session.token);
    return redirect(ctx, '/dashboard');
  }));

  route('POST', '/logout', withForm((ctx) => {
    if (ctx.sessionToken) destroySession(store, ctx.sessionToken);
    clearSessionCookie(ctx);
    return redirect(ctx, '/?ok=' + encodeURIComponent('Logged out.'));
  }));

  // ------------------------------------------------------------ member pages

  route('GET', '/dashboard', requireUser((ctx) => {
    const mine = jobs().find((j) => j.user_id === ctx.user.id)
      .sort((a, b) => b.created_at.localeCompare(a.created_at));
    const matchCounts = new Map(mine.map((j) => [j.id, matchesFor(j).length]));
    const activeDeals = deals().find((d) => d.active !== false);
    return page(ctx, 'Dashboard', views.dashboardPage({ user: ctx.user, jobs: mine, matchCounts, deals: activeDeals }));
  }));

  route('GET', '/jobs/new', requireUser((ctx) =>
    page(ctx, 'Upload a job', views.jobFormPage({ csrf: ctx.session.csrf }))));

  route('POST', '/jobs/new', requireUser(withForm((ctx) => {
    const f = ctx.form;
    const values = {
      category: clean(f.category, 40), completed_on: clean(f.completed_on, 10),
      zip: clean(f.zip, 5), address: clean(f.address, 240), utility: clean(f.utility, 120),
      brand_model: clean(f.brand_model, 240), efficiency: clean(f.efficiency, 120),
      project_cost: clean(f.project_cost, 20), materials_cost: clean(f.materials_cost, 20),
      description: clean(f.description, 4000),
    };
    const fail = (err) => page(ctx, 'Upload a job', views.jobFormPage({ csrf: ctx.session.csrf, values, err }), 400);

    if (!(values.category in JOB_CATEGORIES)) return fail('Pick a job category.');
    if (!isDate(values.completed_on)) return fail('Completion date is required (YYYY-MM-DD).');
    if (!/^\d{5}$/.test(values.zip)) return fail('Job ZIP must be 5 digits.');
    const projectCost = cleanMoney(values.project_cost);
    const materialsCost = cleanMoney(values.materials_cost);
    if (projectCost === null || materialsCost === null) return fail('Costs must be plain dollar amounts, e.g. 12500 or 12500.50.');

    const job = jobs().insert({
      ...values,
      project_cost: projectCost,
      materials_cost: materialsCost,
      user_id: ctx.user.id,
      rebate_status: 'new',
      admin_note: '',
    });
    return redirect(ctx, `/jobs/${job.id}?ok=` + encodeURIComponent('Job uploaded — it will be checked against the rebate catalog.'));
  })));

  route('GET', '/jobs/:id', requireUser((ctx) => {
    const job = jobs().get(ctx.params.id);
    const isAdmin = ctx.user.role === 'admin';
    if (!job || (job.user_id !== ctx.user.id && !isAdmin)) return notFound(ctx);
    return page(ctx, 'Job details', views.jobDetailPage({
      user: ctx.user,
      csrf: ctx.session.csrf,
      job,
      owner: users().get(job.user_id),
      matches: matchesFor(job),
      isAdmin,
    }));
  }));

  route('POST', '/jobs/:id/delete', requireUser(withForm((ctx) => {
    const job = jobs().get(ctx.params.id);
    const isAdmin = ctx.user.role === 'admin';
    if (!job || (job.user_id !== ctx.user.id && !isAdmin)) return notFound(ctx);
    jobs().remove(job.id);
    return redirect(ctx, (isAdmin && job.user_id !== ctx.user.id ? '/admin/jobs' : '/dashboard') +
      '?ok=' + encodeURIComponent('Job deleted.'));
  })));

  route('GET', '/deals', requireUser((ctx) =>
    page(ctx, 'Supplier deals', views.dealsPage({ deals: deals().find((d) => d.active !== false) }))));

  // ------------------------------------------------------------ admin pages

  route('GET', '/admin', requireAdmin((ctx) => redirect(ctx, '/admin/jobs')));

  route('GET', '/admin/jobs', requireAdmin((ctx) => {
    const all = jobs().all().sort((a, b) => b.created_at.localeCompare(a.created_at));
    const filter = ctx.url.searchParams.get('status') ?? 'all';
    const counts = { all: all.length };
    for (const status of Object.keys(REBATE_STATUSES)) {
      counts[status] = all.filter((j) => j.rebate_status === status).length;
    }
    const visible = filter === 'all' ? all : all.filter((j) => j.rebate_status === filter);
    const rows = visible.map((job) => ({
      job,
      member: users().get(job.user_id),
      matchCount: matchesFor(job).length,
    }));
    return page(ctx, 'Rebate desk', views.adminJobsPage({ csrf: ctx.session.csrf, rows, filter, counts }));
  }));

  route('POST', '/admin/jobs/:id/status', requireAdmin(withForm((ctx) => {
    const job = jobs().get(ctx.params.id);
    if (!job) return notFound(ctx);
    const status = clean(ctx.form.rebate_status, 20);
    if (!(status in REBATE_STATUSES)) return notFound(ctx);
    jobs().update(job.id, { rebate_status: status, admin_note: clean(ctx.form.admin_note, 2000) });
    return redirect(ctx, `/jobs/${job.id}?ok=` + encodeURIComponent('Rebate status updated.'));
  })));

  route('GET', '/admin/jobs.csv', requireAdmin((ctx) => {
    const q = (v) => `"${String(v ?? '').replaceAll('"', '""')}"`;
    const header = ['job_id', 'uploaded', 'member', 'company', 'email', 'phone', 'category',
      'completed_on', 'zip', 'address', 'utility', 'equipment', 'efficiency',
      'project_cost', 'materials_cost', 'rebate_status', 'matched_programs'];
    const lines = [header.join(',')];
    for (const job of jobs().all()) {
      const member = users().get(job.user_id);
      lines.push([
        job.id, job.created_at, member?.name, member?.company, member?.email, member?.phone,
        JOB_CATEGORIES[job.category] ?? job.category, job.completed_on, job.zip, job.address,
        job.utility, job.brand_model, job.efficiency, job.project_cost, job.materials_cost,
        job.rebate_status, matchesFor(job).map((p) => p.name).join('; '),
      ].map(q).join(','));
    }
    ctx.res.writeHead(200, baseHeaders({
      'Content-Type': 'text/csv; charset=utf-8',
      'Content-Disposition': 'attachment; filename="dots-services-jobs.csv"',
    }));
    ctx.res.end(lines.join('\r\n') + '\r\n');
  }));

  route('GET', '/admin/members', requireAdmin((ctx) => {
    const members = users().all().sort((a, b) => a.created_at.localeCompare(b.created_at));
    const jobCounts = new Map();
    for (const job of jobs().all()) {
      jobCounts.set(job.user_id, (jobCounts.get(job.user_id) ?? 0) + 1);
    }
    return page(ctx, 'Members', views.adminMembersPage({ members, jobCounts }));
  }));

  route('GET', '/admin/programs', requireAdmin((ctx) =>
    page(ctx, 'Rebate programs', views.adminProgramsPage({ csrf: ctx.session.csrf, programs: programs().all() }))));

  route('POST', '/admin/programs', requireAdmin(withForm((ctx) => {
    const f = ctx.form;
    const categories = (Array.isArray(f.categories) ? f.categories : [f.categories])
      .filter((c) => c in JOB_CATEGORIES);
    const fail = (err) => page(ctx, 'Rebate programs',
      views.adminProgramsPage({ csrf: ctx.session.csrf, programs: programs().all(), err }), 400);

    const name = clean(f.name, 200);
    const amount = clean(f.amount, 200);
    if (!name || !amount) return fail('Name and amount are required.');
    if (categories.length === 0) return fail('Pick at least one job category.');
    const starts = clean(f.starts_on, 10);
    const ends = clean(f.ends_on, 10);
    if ((starts && !isDate(starts)) || (ends && !isDate(ends))) return fail('Dates must be YYYY-MM-DD.');

    programs().insert({
      slug: `custom-${Date.now()}`,
      name,
      amount,
      level: ['federal', 'state', 'utility', 'retail'].includes(f.level) ? f.level : 'utility',
      categories,
      starts_on: starts || null,
      ends_on: ends || null,
      requirements: clean(f.requirements, 2000),
      notes: clean(f.notes, 2000),
      active: true,
    });
    return redirect(ctx, '/admin/programs?ok=' + encodeURIComponent('Program added.'));
  })));

  route('POST', '/admin/programs/:id/toggle', requireAdmin(withForm((ctx) => {
    const program = programs().get(ctx.params.id);
    if (!program) return notFound(ctx);
    programs().update(program.id, { active: program.active === false });
    return redirect(ctx, '/admin/programs');
  })));

  route('GET', '/admin/deals', requireAdmin((ctx) =>
    page(ctx, 'Supplier deals', views.adminDealsPage({ csrf: ctx.session.csrf, deals: deals().all() }))));

  route('POST', '/admin/deals', requireAdmin(withForm((ctx) => {
    const supplier = clean(ctx.form.supplier, 120);
    const title = clean(ctx.form.title, 200);
    if (!supplier || !title) {
      return page(ctx, 'Supplier deals',
        views.adminDealsPage({ csrf: ctx.session.csrf, deals: deals().all(), err: 'Supplier and title are required.' }), 400);
    }
    deals().insert({
      supplier,
      title,
      description: clean(ctx.form.description, 2000),
      code: clean(ctx.form.code, 80),
      active: true,
    });
    return redirect(ctx, '/admin/deals?ok=' + encodeURIComponent('Deal posted.'));
  })));

  route('POST', '/admin/deals/:id/toggle', requireAdmin(withForm((ctx) => {
    const deal = deals().get(ctx.params.id);
    if (!deal) return notFound(ctx);
    deals().update(deal.id, { active: deal.active === false });
    return redirect(ctx, '/admin/deals');
  })));

  route('POST', '/admin/deals/:id/delete', requireAdmin(withForm((ctx) => {
    if (!deals().remove(ctx.params.id)) return notFound(ctx);
    return redirect(ctx, '/admin/deals?ok=' + encodeURIComponent('Deal deleted.'));
  })));

  // ------------------------------------------------------------ dispatch

  const staticCache = new Map();

  function serveStatic(ctx, pathname) {
    const name = pathname.slice('/public/'.length);
    if (!/^[\w.-]+$/.test(name)) return notFound(ctx); // no separators → no traversal
    const ext = name.slice(name.lastIndexOf('.'));
    const type = STATIC_TYPES[ext];
    const file = join(MODULE_DIR, 'public', name);
    if (!type || !existsSync(file)) return notFound(ctx);
    if (!staticCache.has(file)) staticCache.set(file, readFileSync(file));
    ctx.res.writeHead(200, baseHeaders({ 'Content-Type': type, 'Cache-Control': 'public, max-age=300' }));
    ctx.res.end(staticCache.get(file));
  }

  return createServer(async (req, res) => {
    const url = new URL(req.url, 'http://placeholder');
    const ctx = { req, res, url, params: {}, user: null, session: null, sessionToken: null, form: null };
    try {
      ctx.sessionToken = cookieToken(req);
      ctx.session = getSession(store, ctx.sessionToken);
      if (ctx.session) {
        ctx.user = users().get(ctx.session.user_id);
        if (!ctx.user) ctx.session = null;
      }

      if (req.method === 'GET' && url.pathname.startsWith('/public/')) {
        return serveStatic(ctx, url.pathname);
      }

      const pathParts = url.pathname.split('/');
      for (const r of routes) {
        if (r.method !== req.method) continue;
        const params = matchRoute(r.pattern, pathParts);
        if (!params) continue;
        ctx.params = params;
        return await r.handler(ctx);
      }
      return notFound(ctx);
    } catch (error) {
      const status = error.status ?? 500;
      if (status >= 500) console.error(error);
      if (!res.headersSent) {
        res.writeHead(status, baseHeaders({ 'Content-Type': 'text/plain; charset=utf-8' }));
      }
      res.end(status === 413 ? 'Request too large' : 'Internal error');
    }
  });
}

// Started directly (not imported by tests) → listen.
if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  const port = Number(process.env.PORT ?? 8080);
  const host = process.env.HOST ?? '0.0.0.0';
  createApp().listen(port, host, () => {
    console.log(`dots-services listening on http://${host}:${port}`);
  });
}
