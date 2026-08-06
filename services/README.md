# Dots Services Network

The member platform for the Dots Builders home-service professional network,
served at **services.dotsconstruction.com**. Service pros sign up, upload the
jobs they complete, and the rebate desk checks every job against a catalog of
federal, state, and utility rebate programs. Members also see negotiated
supplier deals.

This directory is completely independent of the Fable OS build at the repo
root — nothing here touches the Makefile or the kernel.

## What it does

**For the ~3,000 network members (service professionals):**

- Sign up with trade, service area, and license → instant member account.
- Upload job info per completed job: category, completion date, ZIP, equipment
  model/efficiency, homeowner utility, and costs — the fields rebate filings
  actually need.
- See, per job, which rebate programs plausibly apply and the live status as
  the rebate desk works it (`new → potential → submitted → paid`).
- Browse member supplier deals (negotiated pricing and discount codes).

**For the network operator (admin):**

- Rebate desk: every uploaded job with member contact info, candidate program
  matches, filterable by status; set status + a note the member sees.
- One-click CSV export of all jobs (with matches) for offline rebate hunting.
- Member directory with job counts.
- Editable rebate program catalog — seeded with the federal 25C/25D credits
  (with their 2025 sunset dates), the state HEAR/HOMES electrification
  rebates, and generic utility/retail program entries; add or deactivate
  programs from the UI and matching updates immediately.
- Supplier deals board management.

Accounts whose email is listed in `ADMIN_EMAILS` (default
`yoann@dotsbuilders.com`) get the admin role at signup.

## Running it

No dependencies, no build step. Node 18+.

```sh
cd services
node server.js            # http://localhost:8080
node --test test/         # full end-to-end test suite
```

Configuration (environment variables):

| Variable       | Default                   | Meaning                          |
|----------------|---------------------------|----------------------------------|
| `PORT`         | `8080`                    | Listen port                      |
| `HOST`         | `0.0.0.0`                 | Listen address                   |
| `DATA_DIR`     | `services/data`           | Where JSON collections live      |
| `ADMIN_EMAILS` | `yoann@dotsbuilders.com`  | Comma-separated admin emails     |

Storage is one JSON file per collection, written atomically. That is
deliberate: at this network's scale it is plenty, and it keeps deployment to
a single `node server.js`. The `Store`/`Collection` interface in
`lib/store.js` is the seam to swap in SQLite/Postgres if volume ever demands
it. Back up by copying `DATA_DIR`.

## Deploying to services.dotsconstruction.com

1. **DNS** — at the `dotsconstruction.com` registrar/DNS host, add an
   `A` record: `services` → your server's public IP.
2. **Server** — any small VM (1 vCPU / 512 MB is plenty) with Docker and
   ports 80/443 open:

   ```sh
   git clone <this repo> && cd fable-os/services
   docker compose -f deploy/docker-compose.yml up -d
   ```

   Caddy obtains and renews the Let's Encrypt certificate automatically once
   DNS resolves to the machine.
3. **First login** — sign up with `yoann@dotsbuilders.com` (or whatever
   `ADMIN_EMAILS` is set to in `deploy/docker-compose.yml`); that account is
   the admin.

Alternatives in `deploy/`: `nginx-services.conf` if the box already runs
nginx (use certbot for TLS), and `dots-services.service` to run under systemd
without Docker.

## Security posture

- Passwords scrypt-hashed with per-user salts, constant-time comparison.
- Sessions are opaque server-side tokens in `HttpOnly; SameSite=Lax` cookies
  (`Secure` when behind TLS); authenticated POSTs require a per-session CSRF
  token.
- Every rendered value is HTML-escaped; the CSP disallows all script.
- Login throttling: 10 failures per email+IP per 15 minutes.
- Members can only see their own jobs; admin pages 404 for non-admins.

## Honest limitations (v0)

- Rebate matching is category + date-window against the catalog — it flags
  candidates for the rebate desk, it does not verify eligibility. Every
  seeded program says so.
- No email verification or password reset yet (the admin can be reached for
  resets; adding SMTP is the natural next step).
- No member approval gate — signups are active immediately.
- Single-process JSON storage (see above for the upgrade seam).
