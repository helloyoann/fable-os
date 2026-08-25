# FABLE-001 — Evaluate Fable OS on a spare Intel Mac

Status: blocked pending exact device identity and a later action-time destructive approval.

Migrated from the wrong status plane:
[`helloyoann/tech#163`](https://github.com/helloyoann/tech/issues/163).
This file is now the canonical product ticket; the Tech issue is retained only
as a cross-linked migration receipt.

## Outcome

Determine whether Fable OS can run safely and usefully on Yoann's exact spare
Intel Mac. Only after a reversible external-boot canary, a verified recovery
path, and a separate action-time approval naming the device may its internal
disk be erased and configured for Fable OS.

## Verified starting point

- `helloyoann/fable-os` is an intentional public research project and remains
  separate from Dots repositories.
- The documented path builds and runs on macOS through QEMU.
- The repository can produce a GRUB ISO, but bare-metal compatibility with the
  exact target Mac has not been proven.
- Fable OS has never booted on physical hardware; QEMU success is not a
  bare-metal receipt.
- Actual cost: `MISSING`.

## Next gate

Read-only identify the exact spare Mac model, CPU, RAM, storage, firmware/boot
constraints, and current disk/recovery state. Then determine whether the pinned
repository revision supports a reversible external boot on that model. No disk
write or erase occurs at this gate.

## Plan

1. Record the exact spare Mac identity and recoverable data state.
2. Verify the pinned Fable OS revision, dependencies, license, networking,
   API-key handling, persistence, and bare-metal limitations.
3. Build and test in QEMU using synthetic inputs and no production credential.
4. Create and boot a reversible external USB/ISO canary without erasing the
   internal disk.
5. Verify keyboard, display, storage, networking, reboot/shutdown, persistence,
   and recovery; record unsupported hardware.
6. Produce a wipe/install runbook with a verified macOS recovery installer or
   full backup, rollback steps, costs, and stop conditions.
7. Request a separate action-time owner approval naming the exact Mac and
   accepting the destructive erase.
8. Only after that approval, perform the install and independently cold-boot
   and read back the result.

## Done criteria

- Exact target Mac identity and hardware compatibility are recorded.
- Pinned source revision, build instructions, dependencies, and license are
  verified.
- QEMU tests pass on the target Mac.
- A reversible external-boot canary passes before any internal-disk erase.
- A recoverable backup or macOS recovery route is independently verified.
- Ring-0 model/network control, credential, persistence, and unsupported
  hardware risks are documented.
- Actual cost is recorded or remains literally `MISSING`.
- Separate action-time approval identifies the exact destructive target.
- Final cold boot, required hardware/use cases, and rollback route are verified.

## Hard stops

- Do not erase, repartition, format, or overwrite a disk during evaluation.
- Do not touch a primary Mac, Cloud Mac, production system, or company data.
- Do not store live API credentials in an image, repository, ticket, or log.
- Do not treat QEMU boot as proof of bare-metal compatibility.
- Do not perform a destructive wipe without fresh approval naming the device.
- Stop on unclear device identity, missing recovery proof, unsupported required
  hardware, or irreversible ambiguity.
