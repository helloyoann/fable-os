// Server-rendered HTML for every page. All user-controlled values pass
// through esc() — there is no client-side templating and no inline script,
// which is what lets server.js send a strict CSP with a clear conscience.

import { JOB_CATEGORIES, REBATE_STATUSES } from './rebates.js';

export function esc(value) {
  return String(value ?? '')
    .replaceAll('&', '&amp;')
    .replaceAll('<', '&lt;')
    .replaceAll('>', '&gt;')
    .replaceAll('"', '&quot;')
    .replaceAll("'", '&#39;');
}

function nav(user, csrf) {
  if (!user) {
    return `<nav>
      <a href="/">Home</a>
      <a href="/login">Log in</a>
      <a class="btn btn-small" href="/signup">Join the network</a>
    </nav>`;
  }
  const admin = user.role === 'admin'
    ? `<a href="/admin/jobs">Admin</a>`
    : '';
  return `<nav>
    <a href="/dashboard">Dashboard</a>
    <a href="/jobs/new">Upload a job</a>
    <a href="/deals">Supplier deals</a>
    ${admin}
    <form method="post" action="/logout" class="inline">
      <input type="hidden" name="_csrf" value="${esc(csrf)}">
      <button class="linklike" type="submit">Log out</button>
    </form>
  </nav>`;
}

export function layout({ title, user = null, csrf = '', body, ok = '', err = '' }) {
  const flash =
    (ok ? `<div class="flash flash-ok">${esc(ok)}</div>` : '') +
    (err ? `<div class="flash flash-err">${esc(err)}</div>` : '');
  return `<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>${esc(title)} · Dots Services Network</title>
<link rel="stylesheet" href="/public/styles.css">
</head>
<body>
<header class="site-header">
  <a class="brand" href="/"><span class="brand-dot"></span>Dots <strong>Services</strong></a>
  ${nav(user, csrf)}
</header>
<main>
${flash}
${body}
</main>
<footer class="site-footer">
  <p>Dots Builders — services.dotsconstruction.com · Rebate amounts shown are indicative; every program is verified before filing.</p>
</footer>
</body>
</html>`;
}

// ---------------------------------------------------------------- public

export function landingPage() {
  return `
<section class="hero">
  <h1>The trade network for home-service pros</h1>
  <p class="lede">3,000+ contractors upload their completed jobs here. We hunt down every
  federal, state, and utility rebate the work qualifies for — and negotiate member
  pricing on the supplies you buy every week.</p>
  <p class="cta-row">
    <a class="btn" href="/signup">Join the network — it's free</a>
    <a class="btn btn-ghost" href="/login">Member log in</a>
  </p>
</section>
<section class="three-up">
  <div class="card">
    <h2>Upload your jobs</h2>
    <p>Log the install: category, equipment, completion date, ZIP. Two minutes per job,
    straight from your phone.</p>
  </div>
  <div class="card">
    <h2>We find the rebates</h2>
    <p>Every job is checked against federal tax credits, state electrification programs,
    and the homeowner's utility. You see the status on each job as we work it.</p>
  </div>
  <div class="card">
    <h2>Better prices on supplies</h2>
    <p>Network volume means supplier deals. Members see current negotiated pricing and
    discount codes on the deals board.</p>
  </div>
</section>
<section class="how">
  <h2>How it works</h2>
  <ol>
    <li><strong>Sign up</strong> with your trade, service area, and license.</li>
    <li><strong>Upload job info</strong> as you complete work — equipment model numbers and completion dates matter for rebates.</li>
    <li><strong>We review every job</strong> against the current rebate catalog and chase the paperwork.</li>
    <li><strong>You get the results</strong> — rebate status on every job, plus member supplier pricing.</li>
  </ol>
</section>`;
}

function tradeOptions(selected) {
  return Object.entries(JOB_CATEGORIES)
    .filter(([k]) => k !== 'other')
    .map(([k, label]) => `<option value="${k}"${k === selected ? ' selected' : ''}>${esc(label)}</option>`)
    .join('') + `<option value="other"${selected === 'other' ? ' selected' : ''}>Other / general</option>`;
}

export function signupPage({ values = {}, err = '' } = {}) {
  const v = (k) => esc(values[k] ?? '');
  return `
<section class="form-page">
  <h1>Join the Dots Services Network</h1>
  <p>Free for service professionals. Get job leads, member supplier pricing, and rebate
  hunting on every job you upload.</p>
  ${err ? `<div class="flash flash-err">${esc(err)}</div>` : ''}
  <form method="post" action="/signup">
    <label>Your name *<input name="name" required maxlength="120" value="${v('name')}"></label>
    <label>Company<input name="company" maxlength="120" value="${v('company')}"></label>
    <label>Email *<input name="email" type="email" required maxlength="200" value="${v('email')}"></label>
    <label>Password * <span class="hint">(at least 8 characters)</span><input name="password" type="password" required minlength="8" maxlength="200"></label>
    <label>Phone<input name="phone" maxlength="40" value="${v('phone')}"></label>
    <label>Primary trade *<select name="trade" required>${tradeOptions(values.trade)}</select></label>
    <label>Home-base ZIP *<input name="zip" required pattern="\\d{5}" maxlength="5" inputmode="numeric" value="${v('zip')}"></label>
    <label>License # <span class="hint">(if applicable)</span><input name="license" maxlength="80" value="${v('license')}"></label>
    <button class="btn" type="submit">Create my account</button>
  </form>
  <p>Already a member? <a href="/login">Log in</a>.</p>
</section>`;
}

export function loginPage({ err = '', ok = '' } = {}) {
  return `
<section class="form-page">
  <h1>Member log in</h1>
  ${ok ? `<div class="flash flash-ok">${esc(ok)}</div>` : ''}
  ${err ? `<div class="flash flash-err">${esc(err)}</div>` : ''}
  <form method="post" action="/login">
    <label>Email<input name="email" type="email" required maxlength="200"></label>
    <label>Password<input name="password" type="password" required maxlength="200"></label>
    <button class="btn" type="submit">Log in</button>
  </form>
  <p>New here? <a href="/signup">Join the network</a>.</p>
</section>`;
}

// ---------------------------------------------------------------- member

function statusBadge(status) {
  const label = REBATE_STATUSES[status] ?? status;
  return `<span class="badge badge-${esc(status)}">${esc(label)}</span>`;
}

export function dashboardPage({ user, jobs, matchCounts, deals }) {
  const rows = jobs.map((j) => `
    <tr>
      <td><a href="/jobs/${esc(j.id)}">${esc(JOB_CATEGORIES[j.category] ?? j.category)}</a></td>
      <td>${esc(j.completed_on)}</td>
      <td>${esc(j.zip)}</td>
      <td>${matchCounts.get(j.id) ?? 0} program(s)</td>
      <td>${statusBadge(j.rebate_status)}</td>
    </tr>`).join('');
  const table = jobs.length
    ? `<table>
        <thead><tr><th>Job</th><th>Completed</th><th>ZIP</th><th>Possible rebates</th><th>Status</th></tr></thead>
        <tbody>${rows}</tbody>
      </table>`
    : `<p class="empty">No jobs yet. <a href="/jobs/new">Upload your first job</a> — completed installs
       from 2025 onward can still qualify for rebates.</p>`;
  const dealsTeaser = deals.length
    ? `<ul class="deal-list">${deals.slice(0, 3).map((d) => `<li><strong>${esc(d.supplier)}</strong> — ${esc(d.title)}</li>`).join('')}</ul>
       <p><a href="/deals">See all supplier deals →</a></p>`
    : `<p class="empty">Supplier deals are being negotiated — check back soon.</p>`;
  return `
<h1>Welcome back, ${esc(user.name.split(' ')[0])}</h1>
<section>
  <div class="section-head">
    <h2>Your jobs</h2>
    <a class="btn btn-small" href="/jobs/new">Upload a job</a>
  </div>
  ${table}
</section>
<section>
  <h2>Supplier deals</h2>
  ${dealsTeaser}
</section>`;
}

function categoryOptions(selected) {
  return Object.entries(JOB_CATEGORIES)
    .map(([k, label]) => `<option value="${k}"${k === selected ? ' selected' : ''}>${esc(label)}</option>`)
    .join('');
}

export function jobFormPage({ csrf, values = {}, err = '' } = {}) {
  const v = (k) => esc(values[k] ?? '');
  return `
<section class="form-page form-wide">
  <h1>Upload a job</h1>
  <p>The more detail you give — especially equipment model numbers, the completion date,
  and the homeowner's utility — the more rebates we can chase for you.</p>
  ${err ? `<div class="flash flash-err">${esc(err)}</div>` : ''}
  <form method="post" action="/jobs/new">
    <input type="hidden" name="_csrf" value="${esc(csrf)}">
    <label>Job category *<select name="category" required>${categoryOptions(values.category)}</select></label>
    <label>Completion date *<input name="completed_on" type="date" required value="${v('completed_on')}"></label>
    <label>Job ZIP code *<input name="zip" required pattern="\\d{5}" maxlength="5" inputmode="numeric" value="${v('zip')}"></label>
    <label>Job address <span class="hint">(street / city — needed for most rebate filings)</span><input name="address" maxlength="240" value="${v('address')}"></label>
    <label>Homeowner's utility <span class="hint">(electric or gas provider)</span><input name="utility" maxlength="120" value="${v('utility')}"></label>
    <label>Equipment brand &amp; model <span class="hint">(AHRI # if you have it)</span><input name="brand_model" maxlength="240" value="${v('brand_model')}"></label>
    <label>Efficiency ratings <span class="hint">(SEER2 / HSPF2 / UEF / R-value…)</span><input name="efficiency" maxlength="120" value="${v('efficiency')}"></label>
    <label>Total project cost ($)<input name="project_cost" inputmode="decimal" maxlength="20" value="${v('project_cost')}"></label>
    <label>Materials cost ($)<input name="materials_cost" inputmode="decimal" maxlength="20" value="${v('materials_cost')}"></label>
    <label>Notes<textarea name="description" rows="4" maxlength="4000">${v('description')}</textarea></label>
    <button class="btn" type="submit">Submit job</button>
  </form>
</section>`;
}

export function jobDetailPage({ user, csrf, job, owner, matches, isAdmin }) {
  const fields = [
    ['Category', JOB_CATEGORIES[job.category] ?? job.category],
    ['Completed', job.completed_on],
    ['ZIP', job.zip],
    ['Address', job.address],
    ['Homeowner utility', job.utility],
    ['Equipment', job.brand_model],
    ['Efficiency', job.efficiency],
    ['Project cost', job.project_cost ? `$${job.project_cost}` : ''],
    ['Materials cost', job.materials_cost ? `$${job.materials_cost}` : ''],
    ['Notes', job.description],
  ].filter(([, val]) => val)
    .map(([k, val]) => `<tr><th>${esc(k)}</th><td>${esc(val)}</td></tr>`)
    .join('');

  const matchList = matches.length
    ? `<ul class="program-list">${matches.map((p) => `
        <li>
          <strong>${esc(p.name)}</strong> <span class="level level-${esc(p.level)}">${esc(p.level)}</span>
          <p class="amount">${esc(p.amount)}</p>
          ${p.requirements ? `<p>${esc(p.requirements)}</p>` : ''}
          ${p.notes ? `<p class="hint">${esc(p.notes)}</p>` : ''}
        </li>`).join('')}</ul>`
    : `<p class="empty">No catalog programs match this job's category and completion date.
       It will still be reviewed by hand.</p>`;

  const adminNote = job.admin_note
    ? `<div class="admin-note"><strong>Note from the rebate desk:</strong> ${esc(job.admin_note)}</div>`
    : '';

  const adminPanel = isAdmin
    ? `<section class="admin-panel">
        <h2>Rebate desk (admin)</h2>
        <p>Member: <strong>${esc(owner?.name ?? 'unknown')}</strong>${owner?.company ? ` — ${esc(owner.company)}` : ''}
        · ${esc(owner?.email ?? '')} · ${esc(owner?.phone ?? '')}</p>
        <form method="post" action="/admin/jobs/${esc(job.id)}/status">
          <input type="hidden" name="_csrf" value="${esc(csrf)}">
          <label>Rebate status<select name="rebate_status">${Object.entries(REBATE_STATUSES)
            .map(([k, label]) => `<option value="${k}"${k === job.rebate_status ? ' selected' : ''}>${esc(label)}</option>`)
            .join('')}</select></label>
          <label>Note to member<textarea name="admin_note" rows="3" maxlength="2000">${esc(job.admin_note ?? '')}</textarea></label>
          <button class="btn btn-small" type="submit">Update</button>
        </form>
      </section>`
    : '';

  const deleteForm = (user.id === job.user_id || isAdmin)
    ? `<form method="post" action="/jobs/${esc(job.id)}/delete" class="inline">
        <input type="hidden" name="_csrf" value="${esc(csrf)}">
        <button class="linklike danger" type="submit">Delete this job</button>
      </form>`
    : '';

  return `
<h1>${esc(JOB_CATEGORIES[job.category] ?? job.category)} — ${esc(job.completed_on)}</h1>
<p>${statusBadge(job.rebate_status)}</p>
${adminNote}
<section><h2>Job details</h2><table class="kv">${fields}</table>${deleteForm}</section>
<section><h2>Possible rebate programs</h2>${matchList}</section>
${adminPanel}`;
}

export function dealsPage({ deals }) {
  const list = deals.length
    ? `<ul class="deal-list deal-cards">${deals.map((d) => `
        <li class="card">
          <h2>${esc(d.supplier)}</h2>
          <p><strong>${esc(d.title)}</strong></p>
          ${d.description ? `<p>${esc(d.description)}</p>` : ''}
          ${d.code ? `<p class="deal-code">Member code: <code>${esc(d.code)}</code></p>` : ''}
        </li>`).join('')}</ul>`
    : `<p class="empty">Deals are being negotiated with suppliers now — the more jobs the
       network uploads, the more volume we can bring to the table.</p>`;
  return `
<h1>Supplier deals</h1>
<p>Negotiated for the network. Mention Dots Services (or use the code) when ordering.</p>
${list}`;
}

// ---------------------------------------------------------------- admin

export function adminJobsPage({ csrf, rows, filter, counts }) {
  const tabs = ['all', ...Object.keys(REBATE_STATUSES)]
    .map((s) => {
      const label = s === 'all' ? `All (${counts.all})` : `${REBATE_STATUSES[s]} (${counts[s] ?? 0})`;
      const cls = s === filter ? 'tab tab-active' : 'tab';
      return `<a class="${cls}" href="/admin/jobs${s === 'all' ? '' : `?status=${s}`}">${esc(label)}</a>`;
    })
    .join('');
  const body = rows.length
    ? `<table>
        <thead><tr><th>Job</th><th>Completed</th><th>ZIP</th><th>Member</th><th>Matches</th><th>Status</th></tr></thead>
        <tbody>${rows.map(({ job, member, matchCount }) => `
          <tr>
            <td><a href="/jobs/${esc(job.id)}">${esc(JOB_CATEGORIES[job.category] ?? job.category)}</a></td>
            <td>${esc(job.completed_on)}</td>
            <td>${esc(job.zip)}</td>
            <td>${esc(member?.name ?? '?')}${member?.company ? ` (${esc(member.company)})` : ''}</td>
            <td>${matchCount}</td>
            <td>${statusBadge(job.rebate_status)}</td>
          </tr>`).join('')}</tbody>
      </table>`
    : `<p class="empty">No jobs in this bucket.</p>`;
  return `
<h1>Rebate desk — all jobs</h1>
<p class="admin-links"><a href="/admin/members">Members</a> · <a href="/admin/programs">Rebate programs</a> ·
<a href="/admin/deals">Supplier deals</a> · <a href="/admin/jobs.csv">Export CSV</a></p>
<div class="tabs">${tabs}</div>
${body}`;
}

export function adminMembersPage({ members, jobCounts }) {
  const rows = members.map((m) => `
    <tr>
      <td>${esc(m.name)}</td>
      <td>${esc(m.company)}</td>
      <td>${esc(JOB_CATEGORIES[m.trade] ?? m.trade)}</td>
      <td>${esc(m.zip)}</td>
      <td><a href="mailto:${esc(m.email)}">${esc(m.email)}</a></td>
      <td>${esc(m.phone)}</td>
      <td>${esc(m.license)}</td>
      <td>${jobCounts.get(m.id) ?? 0}</td>
      <td>${m.role === 'admin' ? '<span class="badge badge-paid">admin</span>' : ''}</td>
    </tr>`).join('');
  return `
<h1>Network members (${members.length})</h1>
<p class="admin-links"><a href="/admin/jobs">Jobs</a> · <a href="/admin/programs">Rebate programs</a> · <a href="/admin/deals">Supplier deals</a></p>
<table>
  <thead><tr><th>Name</th><th>Company</th><th>Trade</th><th>ZIP</th><th>Email</th><th>Phone</th><th>License</th><th>Jobs</th><th></th></tr></thead>
  <tbody>${rows}</tbody>
</table>`;
}

export function adminProgramsPage({ csrf, programs, err = '' }) {
  const rows = programs.map((p) => `
    <tr class="${p.active === false ? 'row-inactive' : ''}">
      <td><strong>${esc(p.name)}</strong><br><span class="hint">${esc(p.amount)}</span></td>
      <td>${esc(p.level)}</td>
      <td>${(p.categories ?? []).map((c) => esc(JOB_CATEGORIES[c] ?? c)).join(', ')}</td>
      <td>${esc(p.ends_on ?? '—')}</td>
      <td>
        <form method="post" action="/admin/programs/${esc(p.id)}/toggle" class="inline">
          <input type="hidden" name="_csrf" value="${esc(csrf)}">
          <button class="linklike" type="submit">${p.active === false ? 'Activate' : 'Deactivate'}</button>
        </form>
      </td>
    </tr>`).join('');
  const categoryChecks = Object.entries(JOB_CATEGORIES)
    .map(([k, label]) => `<label class="check"><input type="checkbox" name="categories" value="${k}"> ${esc(label)}</label>`)
    .join('');
  return `
<h1>Rebate program catalog</h1>
<p class="admin-links"><a href="/admin/jobs">Jobs</a> · <a href="/admin/members">Members</a> · <a href="/admin/deals">Supplier deals</a></p>
${err ? `<div class="flash flash-err">${esc(err)}</div>` : ''}
<table>
  <thead><tr><th>Program</th><th>Level</th><th>Categories</th><th>Ends</th><th></th></tr></thead>
  <tbody>${rows}</tbody>
</table>
<section class="form-page form-wide">
  <h2>Add a program</h2>
  <form method="post" action="/admin/programs">
    <input type="hidden" name="_csrf" value="${esc(csrf)}">
    <label>Name *<input name="name" required maxlength="200"></label>
    <label>Level *<select name="level">
      <option value="federal">federal</option><option value="state">state</option>
      <option value="utility" selected>utility</option><option value="retail">retail</option>
    </select></label>
    <label>Amount (text) *<input name="amount" required maxlength="200"></label>
    <fieldset><legend>Job categories *</legend>${categoryChecks}</fieldset>
    <label>Starts on <span class="hint">(YYYY-MM-DD, optional)</span><input name="starts_on" type="date"></label>
    <label>Ends on <span class="hint">(YYYY-MM-DD, optional)</span><input name="ends_on" type="date"></label>
    <label>Requirements<textarea name="requirements" rows="3" maxlength="2000"></textarea></label>
    <label>Notes<textarea name="notes" rows="2" maxlength="2000"></textarea></label>
    <button class="btn" type="submit">Add program</button>
  </form>
</section>`;
}

export function adminDealsPage({ csrf, deals, err = '' }) {
  const rows = deals.map((d) => `
    <tr class="${d.active === false ? 'row-inactive' : ''}">
      <td><strong>${esc(d.supplier)}</strong></td>
      <td>${esc(d.title)}</td>
      <td>${esc(d.code)}</td>
      <td>
        <form method="post" action="/admin/deals/${esc(d.id)}/toggle" class="inline">
          <input type="hidden" name="_csrf" value="${esc(csrf)}">
          <button class="linklike" type="submit">${d.active === false ? 'Activate' : 'Deactivate'}</button>
        </form>
        <form method="post" action="/admin/deals/${esc(d.id)}/delete" class="inline">
          <input type="hidden" name="_csrf" value="${esc(csrf)}">
          <button class="linklike danger" type="submit">Delete</button>
        </form>
      </td>
    </tr>`).join('');
  return `
<h1>Supplier deals</h1>
<p class="admin-links"><a href="/admin/jobs">Jobs</a> · <a href="/admin/members">Members</a> · <a href="/admin/programs">Rebate programs</a></p>
${err ? `<div class="flash flash-err">${esc(err)}</div>` : ''}
<table>
  <thead><tr><th>Supplier</th><th>Deal</th><th>Code</th><th></th></tr></thead>
  <tbody>${rows}</tbody>
</table>
<section class="form-page form-wide">
  <h2>Post a deal</h2>
  <form method="post" action="/admin/deals">
    <input type="hidden" name="_csrf" value="${esc(csrf)}">
    <label>Supplier *<input name="supplier" required maxlength="120"></label>
    <label>Deal title *<input name="title" required maxlength="200"></label>
    <label>Description<textarea name="description" rows="3" maxlength="2000"></textarea></label>
    <label>Member code<input name="code" maxlength="80"></label>
    <button class="btn" type="submit">Post deal</button>
  </form>
</section>`;
}

export function notFoundPage() {
  return `<section class="form-page"><h1>Not found</h1><p>That page doesn't exist. <a href="/">Back home</a>.</p></section>`;
}
