#!/usr/bin/env python3
"""QEMU boot-assertion harness for talk-os.

Boots kernel.bin under QEMU once per case file, captures the serial console to
a scratch file, and asserts on it.  Exit status 0 = every case passed, 1 = at
least one failed.

    tests/qemu/run.sh                 run every case in tests/qemu/cases/
    tests/qemu/run.sh boot            run only cases whose name matches "boot"
    tests/qemu/run.sh -v              also print the serial log of passing cases

Adding a case is a new file in cases/; adding an assertion to a case is one
line in that file.  See cases/boot.case for the syntax, and CHECKS below for
the semantic (parse-the-numbers) assertions a case can invoke.

Environment:
    TALKOS_TEST_OFFLINE=1   skip network-dependent assertions (see "tags")
    TALKOS_TEST_ONLINE=1    force network assertions on (disable autodetect)
    TALKOS_TEST_TIMEOUT=N   override every case's timeout, in seconds
    TALKOS_TEST_QEMU=path   qemu binary (default qemu-system-x86_64)
    TALKOS_TEST_NIC=spec    override every case's -nic (e.g.
                            user,model=e1000,restrict=on to fake no internet)
    TALKOS_TEST_QEMU_EXTRA   extra QEMU arguments appended to every case, split
                            like a shell word list (see qemu_args)
    TALKOS_TEST_KEEPLOGS=1  keep the scratch dir even when everything passes
    TALKOS_TEST_PYTHON=path  interpreter run.sh uses to start this file
"""

import atexit
import os
import re
import shlex
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
OSDIR = os.path.abspath(os.path.join(HERE, "..", ".."))
CASEDIR = os.path.join(HERE, "cases")
KERNEL = os.path.join(OSDIR, "kernel.bin")

QEMU = os.environ.get("TALKOS_TEST_QEMU", "qemu-system-x86_64")
DEFAULT_TIMEOUT = 30.0
POLL = 0.1

# --------------------------------------------------------------------------
# tty
# --------------------------------------------------------------------------

_TTY = sys.stdout.isatty()


def _c(code, s):
    return "\033[%sm%s\033[0m" % (code, s) if _TTY else s


BOLD = lambda s: _c("1", s)
RED = lambda s: _c("31", s)
GREEN = lambda s: _c("32", s)
YELLOW = lambda s: _c("33", s)
DIM = lambda s: _c("2", s)


# --------------------------------------------------------------------------
# child-process bookkeeping — nothing may outlive this script
# --------------------------------------------------------------------------

_children = set()  # of subprocess.Popen, each its own process group


def _reap(proc, grace=2.0):
    """Kill a QEMU process group and wait for it. Safe to call repeatedly."""
    if proc is None:
        return
    _children.discard(proc)
    if proc.poll() is not None:
        return
    for sig in (signal.SIGTERM, signal.SIGKILL):
        try:
            os.killpg(proc.pid, sig)
        except (ProcessLookupError, PermissionError):
            try:
                proc.kill()
            except ProcessLookupError:
                pass
        deadline = time.time() + grace
        while time.time() < deadline:
            if proc.poll() is not None:
                return
            time.sleep(0.05)
    proc.poll()


def _reap_all():
    for proc in list(_children):
        _reap(proc, grace=1.0)


atexit.register(_reap_all)


def _on_signal(signum, _frame):
    _reap_all()
    # Re-raise with the default handler so the exit status is honest.
    signal.signal(signum, signal.SIG_DFL)
    os.kill(os.getpid(), signum)


for _s in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
    signal.signal(_s, _on_signal)


# --------------------------------------------------------------------------
# case files
# --------------------------------------------------------------------------

# "<directive> [tag,tag] : <argument>"
LINE_RE = re.compile(r"^([a-z][a-z0-9-]*)\s*(?:\[([^\]]*)\])?\s*:\s?(.*)$")

ASSERTIONS = {
    "expect",         # ordered literal substring
    "expect-re",      # ordered regex (MULTILINE)
    "expect-any",     # unordered literal substring
    "expect-any-re",  # unordered regex
    "forbid",         # literal substring that must not appear
    "forbid-re",      # regex that must not appear
    "check",          # named semantic check, see CHECKS
}
SETTINGS = {"name", "timeout", "ready", "nic", "qemu-extra", "description",
            "send"}

ESCAPES = {"\\n": "\n", "\\r": "\r", "\\t": "\t", "\\\\": "\\"}


def unescape(s):
    out, i = [], 0
    while i < len(s):
        if s[i] == "\\" and i + 1 < len(s) and s[i:i + 2] in ESCAPES:
            out.append(ESCAPES[s[i:i + 2]])
            i += 2
        else:
            out.append(s[i])
            i += 1
    return "".join(out)


def tag_active(tags, offline):
    """A bare directive always runs. [net] runs only with a network,
    [offline] only without one. Offline mode can therefore only relax."""
    if not tags:
        return True
    if "net" in tags and not offline:
        return True
    if "offline" in tags and offline:
        return True
    return False


class Send:
    """One `send: <wait-regex> => <text>` step."""

    def __init__(self, tags, trigger, text, lineno):
        self.tags = tags
        self.trigger = trigger
        self.lineno = lineno
        # The kernel's line editor sees a terminal, so Enter is CR.
        self.payload = (text if text.endswith(("\n", "\r"))
                        else text + "\r").encode()

    def active(self, offline):
        return tag_active(self.tags, offline)


class Assertion:
    def __init__(self, kind, tags, arg, lineno):
        self.kind = kind
        self.tags = tags
        self.arg = arg
        self.lineno = lineno

    def active(self, offline):
        return tag_active(self.tags, offline)

    def __str__(self):
        tag = "[%s] " % ",".join(sorted(self.tags)) if self.tags else ""
        return "%s%s: %s" % (tag, self.kind, self.arg)


class Case:
    def __init__(self, path):
        self.path = path
        self.name = os.path.splitext(os.path.basename(path))[0]
        self.description = ""
        self.timeout = DEFAULT_TIMEOUT
        self.ready = []          # (tags, regex-source)
        self.nic = "user,model=e1000"
        self.qemu_extra = []
        self.assertions = []
        self.sends = []
        self._parse()

    def _parse(self):
        with open(self.path, "r", encoding="utf-8") as fh:
            for lineno, raw in enumerate(fh, 1):
                line = raw.rstrip("\n")
                if not line.strip() or line.lstrip().startswith("#"):
                    continue
                m = LINE_RE.match(line)
                if not m:
                    raise SyntaxError("%s:%d: cannot parse %r" %
                                      (self.path, lineno, line))
                kind, tagstr, arg = m.group(1), m.group(2), m.group(3)
                tags = {t.strip() for t in (tagstr or "").split(",") if t.strip()}
                bad = tags - {"net", "offline"}
                if bad:
                    raise SyntaxError("%s:%d: unknown tag(s) %s" %
                                      (self.path, lineno, ", ".join(sorted(bad))))
                if kind in ASSERTIONS:
                    if kind == "check":
                        cname = parse_check_args(arg)[0] if arg.strip() else ""
                        if cname not in CHECKS:
                            raise SyntaxError("%s:%d: unknown check %r (have: %s)"
                                              % (self.path, lineno, cname,
                                                 ", ".join(sorted(CHECKS))))
                    if kind.endswith("-re"):
                        try:
                            re.compile(arg)
                        except re.error as e:
                            raise SyntaxError("%s:%d: bad regex %r: %s"
                                              % (self.path, lineno, arg, e))
                    self.assertions.append(Assertion(kind, tags, arg, lineno))
                elif kind in SETTINGS:
                    self._setting(kind, tags, arg, lineno)
                else:
                    raise SyntaxError("%s:%d: unknown directive %r" %
                                      (self.path, lineno, kind))

    def _setting(self, kind, tags, arg, lineno):
        if kind == "name":
            self.name = arg.strip()
        elif kind == "description":
            self.description = arg.strip()
        elif kind == "timeout":
            # A typo like `timeout: 3o` must read as a case parse error with a
            # file and a line, not as a Python traceback from float().
            try:
                self.timeout = float(arg.strip())
            except ValueError:
                raise SyntaxError("%s:%d: timeout %r is not a number"
                                  % (self.path, lineno, arg.strip()))
        elif kind == "ready":
            try:
                re.compile(arg)
            except re.error as e:
                raise SyntaxError("%s:%d: bad ready regex %r: %s"
                                  % (self.path, lineno, arg, e))
            self.ready.append((tags, arg))
        elif kind == "nic":
            self.nic = arg.strip()
        elif kind == "qemu-extra":
            self.qemu_extra += shlex.split(arg)
        elif kind == "send":
            trig, sep, text = arg.partition("=>")
            if not sep:
                raise SyntaxError("%s:%d: send needs '<wait-regex> => <text>'"
                                  % (self.path, lineno))
            trig = trig.strip()
            try:
                re.compile(trig)
            except re.error as e:
                raise SyntaxError("%s:%d: bad send trigger %r: %s"
                                  % (self.path, lineno, trig, e))
            self.sends.append(Send(tags, trig, unescape(text.strip()), lineno))
        else:  # pragma: no cover
            raise SyntaxError("%s:%d: unhandled setting %r" %
                              (self.path, lineno, kind))

    def ready_res(self, offline):
        return [re.compile(src, re.MULTILINE) for tags, src in self.ready
                if tag_active(tags, offline)]


# --------------------------------------------------------------------------
# semantic checks — these parse numbers out of the log instead of
# substring-matching them, so a regression in the value actually fails.
# --------------------------------------------------------------------------

CHECKS = {}


def check(name):
    def deco(fn):
        CHECKS[name] = fn
        return fn
    return deco


class CheckResult:
    def __init__(self):
        self.failures = []
        self.notes = []

    def note(self, msg):
        self.notes.append(msg)

    # The one way a check reports a failure. There used to be a public fail()
    # beside this whose only caller in the file was need() itself.
    def need(self, cond, msg):
        if not cond:
            self.failures.append(msg)
        return cond


HEAP_RE = re.compile(
    r"^heap: (\d+)/(\d+) KiB used \(peak (\d+) KiB\), "
    r"(\d+) live / (\d+) free blocks, (\d+) allocs (\d+) frees$", re.MULTILINE)


@check("heap_integrity")
def _heap_integrity(log, args, r):
    """Parse the heap:/ line and assert the allocator balanced.

    args: max_live=N max_used_kib=N min_allocs=N
    """
    max_live = int(args.get("max_live", 64))
    max_used = int(args.get("max_used_kib", 64))
    min_allocs = int(args.get("min_allocs", 1000))

    ms = HEAP_RE.findall(log)
    if not r.need(ms, "no parsable 'heap: ... allocs ... frees' line in the log"):
        return
    used, arena, peak, live, free, allocs, frees = (int(x) for x in ms[-1])
    r.note("heap: used=%dKiB arena=%dKiB peak=%dKiB live=%d free=%d "
           "allocs=%d frees=%d leaked=%d"
           % (used, arena, peak, live, free, allocs, frees, allocs - frees))

    leaked = allocs - frees
    r.need(allocs >= min_allocs,
           "only %d allocations — the TLS round-trip should churn >=%d; "
           "did the HTTPS self-test actually run?" % (allocs, min_allocs))
    r.need(frees <= allocs,
           "more frees (%d) than allocs (%d) — accounting is broken"
           % (frees, allocs))
    r.need(leaked == live,
           "allocs-frees (%d) != live blocks (%d) — heap accounting mismatch"
           % (leaked, live))
    r.need(0 <= leaked <= max_live,
           "%d allocations never freed (limit %d) — leak regression "
           "(%d allocs / %d frees)" % (leaked, max_live, allocs, frees))
    r.need(used <= max_used,
           "%d KiB still in use after the round-trip (limit %d KiB)"
           % (used, max_used))
    r.need(used <= peak <= arena,
           "nonsensical sizes: used=%d peak=%d arena=%d" % (used, peak, arena))
    r.need(free >= 1, "zero free blocks — the free list vanished")


PCI_DEV_RE = re.compile(r"^pci: [0-9a-f]{2}:[0-9a-f]{2}\.[0-9a-f]  "
                        r"[0-9a-f]{4}:[0-9a-f]{4}  class ", re.MULTILINE)
PCI_TOTAL_RE = re.compile(r"^pci: (\d+) device\(s\)$", re.MULTILINE)


@check("pci_enumeration")
def _pci_enumeration(log, args, r):
    """Cross-check the per-device pci: lines against the reported total.

    args: count=N (expected number of devices)
    """
    m = PCI_TOTAL_RE.search(log)
    if not r.need(m, "no 'pci: N device(s)' summary line"):
        return
    total = int(m.group(1))
    listed = len(PCI_DEV_RE.findall(log))
    r.note("pci: %d listed, summary says %d" % (listed, total))
    r.need(listed == total,
           "PCI enumeration inconsistent: %d device lines but summary says %d"
           % (listed, total))
    if "count" in args:
        want = int(args["count"])
        r.need(total == want,
               "expected %d PCI devices, found %d" % (want, total))


DEV_TOTAL_RE = re.compile(r"^devices: (\d+) registered$", re.MULTILINE)
DEV_ROW_RE = re.compile(r"^  \[\s*(\d+)\] (\S+)\s+(\S+)\s+(\S+)\s", re.MULTILINE)


@check("device_tree")
def _device_tree(log, args, r):
    """Cross-check the device-tree rows against the reported total, and verify
    the ids are a dense 1..N sequence.

    args: count=N  active=name,name,...  (devices that must be state 'active')
    """
    m = DEV_TOTAL_RE.search(log)
    if not r.need(m, "no 'devices: N registered' line"):
        return
    total = int(m.group(1))
    rows = DEV_ROW_RE.findall(log)
    r.note("device tree: %d rows, summary says %d" % (len(rows), total))
    r.need(len(rows) == total,
           "device tree inconsistent: %d rows but summary says %d"
           % (len(rows), total))
    ids = [int(x[0]) for x in rows]
    r.need(ids == list(range(1, len(rows) + 1)),
           "device ids are not a dense 1..N sequence: %s" % (ids,))
    if "count" in args:
        want = int(args["count"])
        r.need(total == want,
               "expected %d registered devices, found %d" % (want, total))
    if args.get("active"):
        states = {name: state for _, name, _, state in rows}
        for want in args["active"].split(","):
            want = want.strip()
            if not r.need(want in states,
                          "device %r missing from the device tree" % want):
                continue
            r.need(states[want] == "active",
                   "device %r is %r, expected 'active'" % (want, states[want]))


HTTP_RE = re.compile(r"^\[http\] HTTP/1\.(\d) (\d{3}) (.*)$", re.MULTILINE)


@check("http_response")
def _http_response(log, args, r):
    """Parse the status line of the boot HTTPS self-test.

    args: allow=401,200 (comma-separated status codes considered a pass)
    """
    m = HTTP_RE.search(log)
    if not r.need(m, "no '[http] HTTP/1.x NNN' status line — the HTTPS "
                     "self-test produced no response"):
        return
    code = int(m.group(2))
    r.note("http: HTTP/1.%s %d %s" % (m.group(1), code, m.group(3).strip()))
    r.need(100 <= code <= 599, "implausible HTTP status %d" % code)
    if args.get("allow"):
        ok = [int(x) for x in args["allow"].split(",")]
        r.need(code in ok,
               "HTTP status %d not in the allowed set %s "
               "(401 = TLS worked but no API key, which is the default)"
               % (code, ok))


TOOLS_RE = re.compile(
    r"^\s*tools\s*: (\d+) registered \((\d+) bytes of schema\)$", re.MULTILINE)

# net/chat.c: CHAT_TOOLS_BYTES. The whole schema has to fit this buffer or
# chat.c throws it away and offers the model no tools at all.
CHAT_TOOLS_BYTES = 32768


@check("tool_registry")
def _tool_registry(log, args, r):
    """Parse the ready banner's `tools : N registered (M bytes of schema)` line
    and assert the tool registry actually produced a usable schema.

    Deliberately a *lower bound* on N, not an equality: tools get added all the
    time and a hardcoded count would fail every time one did. What must not
    regress is (a) tools exist, (b) they serialised, (c) the serialisation is
    big enough to be more than a stub, and (d) it fits the request buffer.

    M == 0 with N > 0 is the exact signature of chat.c's tools_load() rejecting
    the schema (`the tool schema does not fit ... the model will be offered no
    tools at all`) — the machine boots and talks but can never act.

    args: min_tools=N  max_tools=N  min_schema_bytes=N  max_schema_bytes=N
          min_bytes_per_tool=N
    """
    min_tools = int(args.get("min_tools", 1))
    max_tools = int(args.get("max_tools", 4096))
    min_bytes = int(args.get("min_schema_bytes", 1))
    max_bytes = int(args.get("max_schema_bytes", CHAT_TOOLS_BYTES))
    min_per   = int(args.get("min_bytes_per_tool", 0))

    ms = TOOLS_RE.findall(log)
    if not r.need(ms, "no parsable 'tools : N registered (M bytes of schema)' "
                      "line — the ready banner never printed"):
        return
    ntools, nbytes = int(ms[-1][0]), int(ms[-1][1])
    r.note("tools: %d registered, %d bytes of schema (%d bytes/tool)"
           % (ntools, nbytes, nbytes // ntools if ntools else 0))

    r.need(ntools >= min_tools,
           "only %d tools registered, expected at least %d — a whole tool "
           "family (filesystem / hardware / memory) probably failed to "
           "register" % (ntools, min_tools))
    r.need(ntools <= max_tools,
           "%d tools registered, more than the sane ceiling of %d — the "
           "registry is probably counting garbage" % (ntools, max_tools))
    if ntools == 0:
        return                       # min_tools above already said everything
    if not r.need(nbytes > 0,
                  "%d tools are registered but the schema is 0 bytes — "
                  "chat.c rejected it, so the model is offered no tools at "
                  "all and this machine cannot act" % ntools):
        return
    r.need(nbytes >= min_bytes,
           "the tool schema is only %d bytes (expected at least %d) — it is "
           "a stub, not %d real tool definitions" % (nbytes, min_bytes, ntools))
    r.need(nbytes <= max_bytes,
           "the tool schema is %d bytes, over the %d-byte request buffer — "
           "it cannot be sent" % (nbytes, max_bytes))
    if min_per and ntools:
        r.need(nbytes // ntools >= min_per,
               "%d bytes of schema for %d tools is %d bytes each (expected "
               ">=%d) — the definitions look truncated"
               % (nbytes, ntools, nbytes // ntools, min_per))


@check("occurrences")
def _occurrences(log, args, r):
    """Assert a pattern appears between `min` and `max` times.

    max=1 catches reboot loops (the boot banner would print twice); min=2
    pins down "the boot self-test AND the REPL turn both did an HTTP request".

    args: pattern=<regex> min=N max=N
    """
    pat = args.get("pattern")
    if not r.need(pat, "occurrences: missing pattern=<regex>"):
        return
    lo = int(args.get("min", 0))
    hi = int(args.get("max", 1 << 30))
    n = len(re.findall(pat, log, re.MULTILINE))
    r.note("%r matched %d time(s) (want %d..%s)"
           % (pat, n, lo, "inf" if hi == 1 << 30 else hi))
    r.need(n >= lo, "%r appeared %d times, expected at least %d" % (pat, n, lo))
    r.need(n <= hi, "%r appeared %d times, expected at most %d "
                    "(a repeated boot banner means the machine reset)"
                    % (pat, n, hi))


def parse_check_args(arg):
    """`heap_integrity max_live=64 pattern="a b"` -> ("heap_integrity", {...})"""
    parts = shlex.split(arg)
    name = parts[0]
    args = {}
    for p in parts[1:]:
        k, _, v = p.partition("=")
        args[k] = v
    return name, args


# --------------------------------------------------------------------------
# running one case
# --------------------------------------------------------------------------

class Failure:
    def __init__(self, summary, detail=""):
        self.summary = summary
        self.detail = detail


def read_log(path):
    try:
        with open(path, "rb") as fh:
            raw = fh.read()
    except FileNotFoundError:
        return ""
    return raw.decode("utf-8", "replace").replace("\r\n", "\n").replace("\r", "\n")


class SerialSocket:
    """Bidirectional COM1 over a unix socket, tee'd into the log file.

    Only used by cases with `send:` steps; the plain read-only cases keep the
    simpler `-serial file:` path. The kernel registers COM1 as an input source
    (drivers/input/serial_input.c), so writing here is the same as typing.
    """

    def __init__(self, sockpath, logpath):
        self.path = sockpath
        self.fh = open(logpath, "wb")
        self.sock = None
        self.buf = bytearray()
        self.lock = threading.Lock()
        self.stop = threading.Event()

    def connect(self, deadline):
        while time.time() < deadline:
            # Closed on every failed attempt: the retry loop runs up to ~200
            # times in the 10 s window, and leaking a descriptor per attempt (per
            # interactive case) left reclamation to the garbage collector.
            s = socket.socket(socket.AF_UNIX)
            try:
                s.connect(self.path)
            except (FileNotFoundError, ConnectionRefusedError, OSError):
                s.close()
                time.sleep(0.05)
                continue
            s.setblocking(False)
            self.sock = s
            threading.Thread(target=self._pump, daemon=True).start()
            return True
        return False

    def _pump(self):
        while not self.stop.is_set():
            try:
                data = self.sock.recv(65536)
                if not data:
                    return
            except BlockingIOError:
                time.sleep(0.03)
                continue
            except OSError:
                return
            with self.lock:
                self.buf += data
                self.fh.write(data)
                self.fh.flush()

    def text(self):
        with self.lock:
            return (bytes(self.buf).decode("utf-8", "replace")
                    .replace("\r\n", "\n").replace("\r", "\n"))

    def write(self, data):
        try:
            self.sock.sendall(data)
        except OSError:
            pass

    def close(self):
        self.stop.set()
        try:
            if self.sock:
                self.sock.close()
        except OSError:
            pass
        self.fh.close()


def boot(case, logpath, timeout, offline):
    """Boot QEMU, wait for a ready pattern (or timeout), always kill it.
    Returns (log_text, waited_seconds, hit_ready, qemu_stderr)."""
    if case.sends:
        return boot_interactive(case, logpath, timeout, offline)
    errpath = logpath + ".qemu-stderr"
    cmd = qemu_args(case, "file:" + logpath)
    readies = case.ready_res(offline)
    errfh = open(errpath, "wb")
    proc = subprocess.Popen(cmd, stdout=errfh, stderr=subprocess.STDOUT,
                            stdin=subprocess.DEVNULL, start_new_session=True)
    _children.add(proc)
    start = time.time()
    hit = False
    early = None
    try:
        while True:
            elapsed = time.time() - start
            text = read_log(logpath)
            if readies and any(rx.search(text) for rx in readies):
                hit = True
                # Small grace so anything printed alongside the ready marker
                # lands in the file before we pull the plug.
                time.sleep(0.25)
                break
            if proc.poll() is not None:
                # QEMU exited on its own (crash, or killed from outside).
                early = proc.returncode
                time.sleep(0.15)
                break
            if elapsed >= timeout:
                break
            time.sleep(POLL)
    finally:
        _reap(proc)
        errfh.close()
    return (read_log(logpath), time.time() - start, hit,
            qemu_diagnosis(errpath, early, hit))


SIGNOTE_RE = re.compile(r"terminating on signal (\d+) from pid (\d+)")

FRATRICIDE = ("QEMU was killed from outside (signal %s from pid %s) before "
              "the test finished — something else is running "
              "`pkill -f qemu-system-x86_64` (capture.sh only does that with "
              "KILL_OTHERS=1). Re-run when it is done.")


def qemu_diagnosis(errpath, early, hit):
    """Turn QEMU's stderr + exit status into a note, or "" if everything went
    as expected.

    We always SIGTERM our own QEMU, so "terminating on signal 15 from pid
    <us>" is normal and gets filtered.  The same message naming a *different*
    pid means someone else killed our VM, which is the single most confusing
    way for this suite to fail — call it out by name.
    """
    external = None
    lines = []
    for l in read_log(errpath).split("\n"):
        if not l.strip():
            continue
        m = SIGNOTE_RE.search(l)
        if m:
            if int(m.group(2)) != os.getpid():
                external = (m.group(1), m.group(2))
            continue                       # our own SIGTERM: expected, drop it
        lines.append(l)
    if external and not hit:
        lines.append(FRATRICIDE % external)
    elif early is not None and not hit:
        if early < 0:
            lines.append(FRATRICIDE % (-early, "unknown"))
        elif early != 0:
            lines.append("QEMU exited with status %d" % early)
        else:
            lines.append("QEMU exited cleanly before the test finished — a "
                         "guest reset with -no-reboot looks like this.")
    return "\n".join(lines).strip()


def qemu_args(case, serial):
    """`serial` is a -serial chardev spec: file:<path> or unix:<sock>,...

    TALKOS_TEST_NIC overrides the NIC of networked cases — handy for
    reproducing a network-less CI box locally:
        TALKOS_TEST_NIC=user,model=e1000,restrict=on
    Cases that deliberately run with `nic: none` keep their own setting.
    """
    nic = case.nic or "none"
    if nic != "none":
        nic = os.environ.get("TALKOS_TEST_NIC") or nic
    return ([QEMU, "-kernel", KERNEL, "-display", "none", "-no-reboot",
             "-serial", serial, "-nic", nic] + case.qemu_extra +
            shlex.split(os.environ.get("TALKOS_TEST_QEMU_EXTRA", "")))


def boot_interactive(case, logpath, timeout, offline):
    """Same as boot(), but drives the REPL: wait for a prompt, type a line,
    wait for the next one. Serial goes over a unix socket instead of a file."""
    errpath = logpath + ".qemu-stderr"
    # Short path: macOS caps AF_UNIX paths at ~104 bytes, and $TMPDIR is long.
    sockdir = tempfile.mkdtemp(prefix="tqs", dir="/tmp")
    sockpath = os.path.join(sockdir, "com1")
    cmd = qemu_args(case, "unix:%s,server=on,wait=off" % sockpath)

    errfh = open(errpath, "wb")
    proc = subprocess.Popen(cmd, stdout=errfh, stderr=subprocess.STDOUT,
                            stdin=subprocess.DEVNULL, start_new_session=True)
    _children.add(proc)
    ser = SerialSocket(sockpath, logpath)
    readies = case.ready_res(offline)
    sends = [s for s in case.sends if s.active(offline)]
    start = time.time()
    hit = False
    early = None
    note = ""
    try:
        if not ser.connect(start + min(10.0, timeout)):
            note = "could not connect to the QEMU serial socket %s" % sockpath
        else:
            step = 0
            scan = 0                       # only look for a trigger after the
            while True:                    # text that satisfied the last one
                text = ser.text()
                if step < len(sends):
                    trig, payload = sends[step].trigger, sends[step].payload
                    m = re.compile(trig, re.MULTILINE).search(text, scan)
                    if m:
                        time.sleep(0.2)    # let the prompt settle
                        ser.write(payload)
                        scan = m.end()
                        step += 1
                elif readies and any(rx.search(text, scan) for rx in readies):
                    hit = True
                    time.sleep(0.35)
                    break
                if proc.poll() is not None:
                    early = proc.returncode
                    time.sleep(0.15)
                    break
                if time.time() - start >= timeout:
                    if step < len(sends):
                        note = ("timed out waiting for send-step %d/%d "
                                "trigger /%s/" % (step + 1, len(sends),
                                                  sends[step].trigger))
                    break
                time.sleep(POLL)
    finally:
        ser.close()
        _reap(proc)
        errfh.close()
        shutil.rmtree(sockdir, ignore_errors=True)
    qerr = qemu_diagnosis(errpath, early, hit)
    if note:
        qerr = (qerr + "\n" + note).strip()
    return read_log(logpath), time.time() - start, hit, qerr


def context(log, pos, before=6, after=4):
    """Render the serial log around a character offset, with a marker."""
    lines = log.split("\n")
    # character offset -> line index
    idx, acc = 0, 0
    for i, ln in enumerate(lines):
        if acc + len(ln) + 1 > pos:
            idx = i
            break
        acc += len(ln) + 1
    else:
        idx = len(lines) - 1
    lo, hi = max(0, idx - before), min(len(lines), idx + after + 1)
    out = []
    for i in range(lo, hi):
        mark = ">>" if i == idx else "  "
        out.append("      %s %4d | %s" % (mark, i + 1, lines[i]))
    return "\n".join(out)


def tail(log, n=20):
    lines = log.split("\n")[-n:]
    return "\n".join("         | " + l for l in lines)


def evaluate(case, log, offline, waited, hit_ready):
    """Run every active assertion against the captured log."""
    failures, notes, skipped = [], [], 0
    pos = 0                       # ordered-expect cursor
    last_ok = "(start of log)"
    shown_ctx = set()             # positions we've already printed context for

    if not log.strip():
        # The REAL command, not a plausible-looking one. This is the failure
        # where the reader most needs to paste it and see QEMU's own complaint —
        # a bad `qemu-extra`, a -device this QEMU does not have, an unknown NIC
        # model — and a command missing -display/-no-reboot/-serial/-nic and the
        # case's own extras would behave differently from the one that failed.
        failures.append(Failure(
            "the kernel produced no serial output at all in %.1fs" % waited,
            "      QEMU command was:\n         | " + " ".join(
                shlex.quote(x) for x in qemu_args(case, "file:<log>"))))
        return failures, notes, skipped

    for a in case.assertions:
        if not a.active(offline):
            skipped += 1
            continue

        if a.kind in ("expect", "expect-re"):
            if a.kind == "expect":
                found = log.find(a.arg, pos)
                m_end = found + len(a.arg) if found >= 0 else -1
            else:
                m = re.compile(a.arg, re.MULTILINE).search(log, pos)
                found = m.start() if m else -1
                m_end = m.end() if m else -1
            if found < 0:
                anywhere = (a.arg in log) if a.kind == "expect" else bool(
                    re.search(a.arg, log, re.MULTILINE))
                why = ("it DOES appear earlier in the log — out of order"
                       if anywhere else "not found anywhere in the log")
                if pos in shown_ctx:
                    # Everything after the first miss stalls at the same spot;
                    # don't print the same ten lines of serial over and over.
                    detail = ("      %s:%d — %s (log position unchanged since "
                              "the failure above)" % (case.path, a.lineno, why))
                else:
                    shown_ctx.add(pos)
                    detail = ("      %s:%d\n      %s\n"
                              "      last match before this: %s\n"
                              "      serial log from where the search "
                              "resumed:\n%s"
                              % (case.path, a.lineno, why, last_ok,
                                 context(log, pos)))
                failures.append(Failure(
                    "expected (in order) %s" % _q(a.kind, a.arg), detail))
                # Don't cascade: keep the cursor, report the rest too.
            else:
                pos = m_end
                last_ok = _q(a.kind, a.arg)

        elif a.kind in ("expect-any", "expect-any-re"):
            ok = (a.arg in log) if a.kind == "expect-any" else bool(
                re.search(a.arg, log, re.MULTILINE))
            if not ok:
                failures.append(Failure(
                    "expected (anywhere) %s" % _q(a.kind, a.arg),
                    "      %s:%d\n      last 20 lines of serial:\n%s"
                    % (case.path, a.lineno, tail(log))))

        elif a.kind in ("forbid", "forbid-re"):
            if a.kind == "forbid":
                at = log.find(a.arg)
            else:
                m = re.search(a.arg, log, re.MULTILINE | re.IGNORECASE)
                at = m.start() if m else -1
            if at >= 0:
                failures.append(Failure(
                    "forbidden %s appeared" % _q(a.kind, a.arg),
                    "      %s:%d\n      serial log around it:\n%s"
                    % (case.path, a.lineno, context(log, at))))

        elif a.kind == "check":
            name, args = parse_check_args(a.arg)
            r = CheckResult()
            try:
                CHECKS[name](log, args, r)
            except (ValueError, KeyError) as e:
                # `check: heap_integrity max_live=x` used to raise out of here
                # as a traceback, after a VM had already booted. The check name
                # is validated at parse time; its arguments cannot be, because
                # only the check knows which of them are numbers — so a bad one
                # becomes an ordinary failure that names the file and the line.
                failures.append(Failure(
                    "check %s has a bad argument (%s: %s)"
                    % (BOLD(name), type(e).__name__, e),
                    "      %s:%d" % (case.path, a.lineno)))
                continue
            notes += ["%s: %s" % (name, n) for n in r.notes]
            for f in r.failures:
                failures.append(Failure(
                    "check %s failed: %s" % (BOLD(name), f),
                    "      %s:%d" % (case.path, a.lineno)))

    if not hit_ready and case.ready:
        want = ", ".join("/%s/" % s for _, s in case.ready)
        failures.append(Failure(
            "timed out after %.1fs without reaching a ready marker (%s)"
            % (waited, want),
            "      last 20 lines of serial:\n%s" % tail(log)))
    return failures, notes, skipped


def _q(kind, arg):
    return ("/%s/" % arg) if kind.endswith("-re") else repr(arg)


# --------------------------------------------------------------------------
# offline detection
# --------------------------------------------------------------------------

def env_bool(name):
    return os.environ.get(name, "").strip().lower() in ("1", "true", "yes", "on")


def network_available(host="api.anthropic.com", timeout=4.0):
    """Best-effort: can this machine resolve *and* reach the API host?"""
    result = {}

    def probe():
        try:
            infos = socket.getaddrinfo(host, 443, socket.AF_INET,
                                       socket.SOCK_STREAM)
            # `with` so the offline path — the one taken on every CI box without
            # internet, i.e. the common one — closes the socket too.
            with socket.socket() as s:
                s.settimeout(timeout)
                s.connect(infos[0][4])
            result["ok"] = True
        except Exception as e:      # noqa: BLE001 - any failure means offline
            result["ok"] = False
            result["err"] = "%s: %s" % (type(e).__name__, e)

    t = threading.Thread(target=probe, daemon=True)
    t.start()
    t.join(timeout + 1.0)
    if not result:
        return False, "probe timed out after %.0fs" % timeout
    return result.get("ok", False), result.get("err", "")


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------

def main(argv):
    verbose = False
    filters = []
    for a in argv:
        if a in ("-v", "--verbose"):
            verbose = True
        elif a in ("-h", "--help"):
            print(__doc__)
            return 0
        else:
            filters.append(a)

    if shutil.which(QEMU) is None:
        print(RED("error: %s not found on PATH" % QEMU))
        return 1
    if not os.path.exists(KERNEL):
        print(RED("error: %s missing — run `make` first" % KERNEL))
        return 1

    # ---- mode --------------------------------------------------------
    reason = ""
    if env_bool("TALKOS_TEST_OFFLINE"):
        offline, reason = True, "TALKOS_TEST_OFFLINE=1"
    elif env_bool("TALKOS_TEST_ONLINE"):
        offline, reason = False, "TALKOS_TEST_ONLINE=1"
    else:
        ok, err = network_available()
        offline = not ok
        reason = ("autodetected: api.anthropic.com reachable" if ok else
                  "autodetected: api.anthropic.com unreachable (%s)" % err)

    override_timeout = os.environ.get("TALKOS_TEST_TIMEOUT")

    # ---- cases -------------------------------------------------------
    paths = sorted(os.path.join(CASEDIR, f) for f in os.listdir(CASEDIR)
                   if f.endswith(".case"))
    cases = []
    for p in paths:
        try:
            cases.append(Case(p))
        except (SyntaxError, ValueError) as e:
            # ValueError too: a malformed setting deep in _setting() should read
            # as a case parse error, not as an interpreter traceback.
            print(RED("case parse error: %s" % e))
            return 1
    if filters:
        cases = [c for c in cases
                 if any(f in c.name or f in os.path.basename(c.path)
                        for f in filters)]
    if not cases:
        print(RED("no cases matched %s in %s" % (filters, CASEDIR)))
        return 1

    workdir = tempfile.mkdtemp(prefix="talkos-qemu-%d-" % os.getpid())

    print(BOLD("talk-os QEMU boot tests"))
    print("  kernel : %s (%d bytes)" % (KERNEL, os.path.getsize(KERNEL)))
    print("  qemu   : %s" % shutil.which(QEMU))
    print("  mode   : %s   (%s)" % (
        YELLOW("OFFLINE — network assertions skipped") if offline
        else GREEN("ONLINE — network assertions enforced"), reason))
    print("  logs   : %s" % workdir)
    print("  cases  : %d" % len(cases))
    print()

    results = []
    for case in cases:
        timeout = float(override_timeout) if override_timeout else case.timeout
        slug = re.sub(r"[^A-Za-z0-9._-]+", "-", case.name).strip("-") or "case"
        logpath = os.path.join(workdir, slug + ".log")
        sys.stdout.write("  %-28s " % case.name)
        sys.stdout.flush()
        # `description:` was parsed and stored by every case file and read by
        # nothing, which made it decoration that looked like configuration. It is
        # the one line that says what the case is FOR, so -v prints it.
        if verbose and case.description:
            sys.stdout.write("\n      %s\n  %-28s " % (DIM(case.description), ""))
            sys.stdout.flush()

        t0 = time.time()
        log, waited, hit_ready, qerr = boot(case, logpath, timeout, offline)
        failures, notes, skipped = evaluate(case, log, offline, waited,
                                            hit_ready)
        dt = time.time() - t0

        if failures:
            print("%s  (%.1fs)" % (RED("FAIL"), dt))
        else:
            extra = DIM(" %d skipped" % skipped) if skipped else ""
            print("%s  (%.1fs)%s" % (GREEN("PASS"), dt, extra))
        for n in notes:
            print("      %s" % DIM(n))
        for f in failures:
            print("      %s %s" % (RED("x"), f.summary))
            if f.detail:
                print(f.detail)
        if qerr:
            print("      %s" % DIM("qemu stderr: " + qerr.replace("\n", " | ")))
        if failures or verbose:
            print("      %s" % DIM("full serial log: " + logpath))
        results.append((case, failures))

    npass = sum(1 for _, f in results if not f)
    nfail = len(results) - npass
    print()
    print(BOLD("  %d passed, %d failed") % (npass, nfail))
    for case, f in results:
        if f:
            print("    %s %s (%d assertion%s)"
                  % (RED("FAIL"), case.name, len(f), "" if len(f) == 1 else "s"))
    if nfail == 0 and not env_bool("TALKOS_TEST_KEEPLOGS"):
        shutil.rmtree(workdir, ignore_errors=True)
    else:
        print("    serial logs kept in %s" % workdir)
    print()
    return 1 if nfail else 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    finally:
        _reap_all()
