#!/usr/bin/env python3
"""lint_printf.py — find format strings the KERNEL's own formatter cannot render.

    tests/qemu/lint_printf.py            # scan what a default build compiles
    tests/qemu/lint_printf.py --all      # scan every first-party source
    tests/qemu/lint_printf.py --json

WHY THIS EXISTS, and why it is a source lint rather than a test.

The kernel does not use the host C library. `lib/libc_shim.c` provides a small
bounded vsnprintf, and its parser accepts only DIGITS after '.' and only digits
for a width. It has no star width and no star precision.

The failure mode is NOT a truncated string, which is what everyone assumes when
they read "no %*d support". Because the '*' is never consumed, the formatter
emits the LITERAL FOUR CHARACTERS "%*s" into the output AND LEAVES THE ARGUMENT
LIST MISALIGNED, so every conversion after it reads the wrong argument. Measured
against the real shim:

    format : "%s: register \"%.*s\" does not exist (this machine has r0-r%d)"
    args   : "ld", 3, "r6b", 15
    host   : ld: register "r6b" does not exist (this machine has r0-r15)
    KERNEL : ld: register "%*s" does not exist (this machine has r0-r3)

Two separate harms in one line. The token the model needs is replaced by
punctuation, and the register count becomes a CONFIDENT LIE assembled from the
token's length. A model reading that is told this machine has four registers.

WHY `make test-host` CANNOT FIND THESE. Host suites link the real libc, where
every one of these format strings works perfectly. The bug exists only in the
binary that boots. That asymmetry is the whole reason for a lint: it reads the
source, so it does not care which formatter is linked.

WHAT IS AND IS NOT A FINDING. Third-party trees (lwip, mbedtls) are skipped:
they ship their own formatters and are not printed through libc_shim. Host test
sources under tests/ are skipped for the same reason — they really do run
against the host libc. Everything a default `make` compiles is in scope.

EXIT STATUS: 0 if clean, 1 if any offender is found, 2 on usage error. Findings
are printed as `file:line: text`, so an editor can jump to them.

NOT WIRED INTO `make test-qemu`, deliberately. At the time of writing the only
offenders are in a file this script's author does not own, and a new red test
that nobody is allowed to fix teaches people to ignore red tests. It is a
one-command reproduction for whoever does own it, and it belongs in the suite
the moment the count reaches zero.
"""

import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))        # os/tests/qemu
OSDIR = os.path.dirname(os.path.dirname(HERE))           # os/

# Third-party trees bring their own printf. tests/ links the host libc.
SKIP_PREFIX = ("lwip/", "mbedtls/", "tests/", "port/")

# A conversion whose width or precision is taken from an argument. Matches
# `%*d`, `%.*s`, `%-*s`, `%0.*x`, `%.*S` ... but not `%.3s`, and not a literal
# `%%*` (an escaped percent followed by a star, which is just text).
STAR = re.compile(r"%[-+ #0]*(?:\*|[0-9]*\.\*)")


def sources_from_depfiles():
    """Every first-party .c/.h a default build actually compiled.

    Read from the compiler's own -MMD output, so the set is what the build
    really pulled in rather than what a directory walk guesses. Returns None if
    the tree has not been built, so the caller can fall back.
    """
    found = set()
    seen_any = False
    for root, dirs, files in os.walk(OSDIR):
        dirs[:] = [d for d in dirs if d not in (".git", "tests/build")]
        for fn in files:
            if not fn.endswith(".d"):
                continue
            seen_any = True
            path = os.path.join(root, fn)
            try:
                with open(path) as f:
                    text = f.read()
            except OSError:
                continue
            # Both the prerequisite list and the phony per-header targets name
            # files; a bare token ending in .c/.h is all we need.
            for tok in re.split(r"[\s\\:]+", text):
                if tok.endswith((".c", ".h")):
                    found.add(os.path.normpath(tok))
    return found if seen_any else None


def sources_all():
    out = set()
    for root, dirs, files in os.walk(OSDIR):
        dirs[:] = [d for d in dirs if d != ".git"]
        for fn in files:
            if fn.endswith((".c", ".h")):
                rel = os.path.relpath(os.path.join(root, fn), OSDIR)
                out.add(os.path.normpath(rel))
    return out


def in_scope(rel):
    rel = rel.replace(os.sep, "/")
    if rel.startswith("/"):
        return False
    return not any(rel.startswith(p) for p in SKIP_PREFIX)


def scan(rel):
    """Offending lines in one file: (line_no, stripped_text, [conversions])."""
    path = os.path.join(OSDIR, rel)
    try:
        with open(path, "r", errors="replace") as f:
            lines = f.readlines()
    except OSError:
        return []
    hits = []
    for i, line in enumerate(lines, 1):
        # Skip comment lines. This file's own explanation of the bug quotes the
        # broken format string verbatim, and a lint that reports the description
        # of a fix as an instance of the bug is a lint people switch off. The
        # test is deliberately crude — leading `*`, `//` or `/*`, which is every
        # comment in this tree's house style — because the alternative is a C
        # parser. A format string sharing a line with the end of a block comment
        # would be missed; nothing in this tree does that.
        stripped = line.lstrip()
        if stripped.startswith(("*", "//", "/*")):
            continue
        # Only look inside string literals: a bare `a %* b` in prose, or an
        # expression like `x = y %*p`, is not a format string. Cheap and good
        # enough — every real offender is a literal on the line.
        for lit in re.findall(r'"((?:[^"\\]|\\.)*)"', line):
            m = STAR.findall(lit.replace("%%", ""))
            if m:
                hits.append((i, line.strip(), m))
                break
    return hits


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--all", action="store_true",
                    help="scan every first-party source, not just what was built")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args(argv[1:])

    srcs = None if args.all else sources_from_depfiles()
    how = "compiled by the current build (from -MMD .d files)"
    if srcs is None:
        srcs = sources_all()
        how = "every first-party source (tree was not built, or --all)"

    srcs = sorted(s for s in srcs if in_scope(s))

    findings = []
    for rel in srcs:
        for lineno, text, convs in scan(rel):
            findings.append({"file": rel, "line": lineno,
                             "conversions": convs, "text": text})

    if args.json:
        print(json.dumps({"scanned": len(srcs), "scope": how,
                          "findings": findings}, indent=1))
        return 1 if findings else 0

    print("lint_printf: scanned %d file(s): %s" % (len(srcs), how))
    print("looking for star width/precision, which lib/libc_shim.c renders as")
    print("the literal text \"%*s\" while misaligning every later argument.")
    print()
    if not findings:
        print("CLEAN: no kernel format string uses an argument-supplied width")
        print("or precision. This lint belongs in make test-qemu now.")
        return 0

    byfile = {}
    for f in findings:
        byfile.setdefault(f["file"], []).append(f)
    for rel in sorted(byfile):
        print("%s  (%d)" % (rel, len(byfile[rel])))
        for f in byfile[rel]:
            print("  %s:%d: %s" % (rel, f["line"], f["text"][:96]))
    print()
    print("%d offending line(s) in %d file(s)." % (len(findings), len(byfile)))
    print("Each one reaches a model as punctuation plus a wrong number.")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
