# Missive inbox triage — full audit (2026-08-05)

Consolidates every open "missive" provider-alert ticket from the Yo monitor into one
resolution. Scope: is the continuous triage running, where it fails, whether the rules
make sense, and clearing the backlog.

## TL;DR

- **The triage pipeline is enabled but has not completed a disposition since ~Jul 16.**
  The monitor's "rule enabled / billing paid" checks pass, so the rule exists and the
  account is healthy — the *apply* step is what's dead.
- The inbox was bulk-archived Missive-side on Aug 3 (~06:00–08:00 UTC, count 123 → 1)
  **without labels and without syncing archive state to Gmail**, which created the
  "inconsistent" flag and left Gmail's INBOX ~450 threads deep while Missive showed ~1.
- New mail then accumulated again untouched: disposition-pending grew 1 → 107 in 54 h,
  ~1 : 1 with inbox count — i.e. **zero throughput**, not slow throughput.
- Likely trigger for the stall: **Missive API rate-limiting** — the monitor logged
  `organizations HTTP 429` (near_limit) at Aug 4 01:45 UTC, alongside cascading
  failures in the same stack: `gbrain_ingest` durable-disposition backlog 16 > 5,
  `whatsapp_receiver` down (oldest unprocessed 37 h), `bluebubbles` fetch failed,
  `local_mirrors` contact-sync error + Brain drift (ClickUp −114..−130), and scheduled
  CI red on `helloyoann/infra` (Documentation & Config Validation, field-capture OTA),
  `helloyoann/brain` (SoT Integrity, Brain Validation), `helloyoann/tech`
  (Markdown cross-reference), `helloyoann/new` (Sync starred manifest).
- **Backlog cleared as part of this audit**: every thread in the Gmail INBOX
  (≈ 555 threads, Jul 16 → Aug 5) was classified per the reconstructed rules, given
  entity + type labels, and archived. Gmail INBOX is the source Missive syncs from, so
  the Missive inbox drains with it and the Gmail/Missive inconsistency is healed.
  Machine-generated self-noise (Yo alerts, own-repo CI failures) was archived without
  labels, matching the system's historical treatment.

## Ticket ledger (merged here)

27 `[Yo] Provider alert — missive` tickets between Jul 30 and Aug 5 (thread ids
`19fb3a8f5e769f05` … `19fd23e3dccfd45c`, chain `monitor/incident`). Key data points:

| When (UTC) | State | Detail |
|---|---|---|
| Jul 30–Aug 2 | down | Inbox 112→166, oldest 52→107 h; no disposition metrics readable |
| Aug 3 06:00 | down | Inbox 123 (113 h); automation 30 / human 93 |
| Aug 3 07:55 | degraded | **Inbox 1 (0 h)** — bulk clear happened Missive-side |
| Aug 3 13:55 → Aug 4 20:42 | degraded | Inbox 12→99; disposition pending tracks inbox 1:1 |
| Aug 4 01:45 | near_limit | **`organizations HTTP 429`** ← probable stall trigger |
| Aug 5 02:47 | degraded | Inbox 108 (43 h); pending 107; review 7; inconsistent 1 |
| Aug 5 07:53, 14:05 | down | Inbox ~107 (48→54 h); pending 105; **review 7→54**; + local_mirrors down |

All are the same incident; they are resolved together by this audit (backlog cleared)
plus the pipeline fixes listed under "Open items".

## Where it fails (pipeline view)

Ingest (gbrain-ingest, Railway) still runs — "last successful durable ingest 27 m ago" —
but the **durable disposition step** backlogs (16 > 5) and nothing applies dispositions
to conversations, so Missive inbox and "disposition pending" grow in lock-step. The
jump of 47 conversations into "review" on Aug 5 morning suggests a run started and
dumped items to the review queue instead of completing dispositions — consistent with a
429/backoff loop or an auth/permission failure on the apply call. The Aug 3 bulk clear
also proves the failure is *not* in Missive itself (manual archive worked) — it is in
the automation's write path.

Note the feedback loop: the monitor emails its own alerts into the shared inbox
(~15 of the 107 pending were Yo alerts; ~20 more were own-repo CI failure emails), so
the sicker the pipeline, the faster the inbox fills.

## The rules, as reconstructed from historical dispositions

Two axes, then archive (inbox zero always):

- **Entity label** (who owns it):
  - `DC` — Dots Construction; sub-labels `DC/Operations` (vendors, permits, orders),
    `DC/Projects[/<job>]` (job-specific), `DC/Marketing` (inbound promos/cold outreach)
  - `DS` — Dots Space (coworking); `DS/Members`, `DS/Corp`, `DS/Space`
  - `DOTS` — cross-entity infra/SaaS (Vercel, Railway, Cursor, Twilio, Google
    Workspace/Cloud, Microsoft, GitHub billing). *This label existed but was empty;
    this audit is its first real use — confirm or fold into DC.*
  - `YB` — Yoann personal; `BB` — Bohbot family/household (club, school, family cards)
- **Type label** (what to do with it): `_TASK` action needed · `_BILL` money in/out ·
  `_INFO` FYI/no action
- **No label** — self-generated machine noise (Yo provider alerts, own CI emails):
  archive only.

The taxonomy is sound. What does **not** make sense as-is:

1. **Yo alerts and CI failures land in the inbox the triage must then clear.** Route
   them to a dedicated label/channel *before* the inbox (Gmail filter or Missive rule),
   or stop emailing them and post to chat. ~35 % of the current backlog was self-noise.
2. **YB vs BB boundary is undefined** — historical assignments overlap (school in both,
   Chase cards in both). Write the rule down; suggested: YB = Yoann individually,
   BB = family/household.
3. **No dedupe for multi-alias copies** — the same message delivered to 2–4 aliases
   (acsshows ×4, USCD ×4, x.ai ×3) counts as 2–4 conversations to triage.
4. **OTP/verification emails** (JetBlue, Microsoft, Fabuwood, civicplus, zocdoc,
   adguard) deserve an auto-archive-on-arrival rule; they expire in minutes and never
   need human eyes.
5. **`_TASK` had not been applied since Jul 16** — the actionable queue silently died
   with the pipeline; the monitor's "actionable 1" was under-counting badly (this audit
   filed ~45 threads as `_TASK`).
6. **Monitor thresholds**: "degraded" at inbox ≥ ~10 fired 27 emails in 6 days for one
   incident. Alert on disposition *throughput* (0 in N hours) once, then escalate —
   not on every level change.
7. **Apply-side hardening**: back-off + resume on 429 with a dead-letter queue, and a
   Gmail-side fallback (label + archive via Gmail API) when the Missive API is
   limited — Missive syncs Gmail state, so the fallback is equivalent and cheaper.

## Actions taken in this audit session

1. Enumerated the full Gmail INBOX (≈ 555 threads, Jul 16 → Aug 5; Missive saw only
   the ~107 newest — the rest were the un-synced remainder of the Aug 3 bulk clear).
2. Classified every thread per the rules above and applied entity + type labels, then
   archived (INBOX removed). Read/unread state untouched. Nothing deleted.
3. Archived all Yo alert + own-repo CI threads label-free, per historical practice.
4. Merged the 27 missive tickets into this document as the single incident record.

## Open items (need repo/infra access)

- `helloyoann/tech` / `helloyoann/infra` / `helloyoann/brain` hold the triage code and
  monitor config; this session's GitHub scope could not attach them (approval gate).
  The 429 handling, durable-disposition backlog, and the four red CI workflows need
  fixing there.
- WhatsApp receiver (>24 h unprocessed), BlueBubbles receiver (fetch failed), and
  local-mirrors contact sync (ClickUp drift −130) are down/degraded on Railway and were
  not reachable from this session.
- Missive "review" queue (54 conversations) is Missive-internal state; re-run or clear
  it once the apply path is fixed.
