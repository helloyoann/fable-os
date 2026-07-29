#!/usr/bin/env python3
"""Regenerate the APP_CALCULATOR_JSON block in calculator_json.h from the .json.

The kernel cannot read host files, so the reference app has to exist twice: once
as apps/examples/calculator.json (the artifact a human — or a model being shown
what good looks like — reads) and once as a C string literal inside the kernel.
This script derives the second from the first so the two cannot drift, and
tests/host/test_app.c reads the .json at run time and fails if they ever differ.

    python3 apps/examples/gen_header.py        # run from os/, or from anywhere
"""

import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "calculator.json")
HDR = os.path.join(HERE, "calculator_json.h")

BEGIN = "/* --- BEGIN GENERATED (do not edit by hand) --- */ \\\n"
END = "/* --- END GENERATED --- */"


def main():
    with open(SRC, "r", encoding="utf-8") as fh:
        text = fh.read()

    # A malformed example would be a rejected app and a very confusing test
    # failure, so the generator refuses to embed one.
    try:
        json.loads(text)
    except ValueError as exc:
        print("gen_header: %s is not valid JSON: %s" % (SRC, exc), file=sys.stderr)
        return 1

    with open(HDR, "r", encoding="utf-8") as fh:
        hdr = fh.read()

    lines = text.split("\n")
    if lines and lines[-1] == "":
        lines.pop()                      # trailing newline, not a real line
    body = "".join(
        '    "%s\\n"%s\n' % (l.replace("\\", "\\\\").replace('"', '\\"'),
                             " \\" if i + 1 < len(lines) else "")
        for i, l in enumerate(lines))

    i = hdr.find(BEGIN)
    j = hdr.find(END)
    if i < 0 or j < 0 or j < i:
        print("gen_header: markers not found in %s" % HDR, file=sys.stderr)
        return 1
    out = hdr[:i + len(BEGIN)] + body + hdr[j:]
    if out == hdr:
        print("gen_header: already up to date (%d bytes of document)" % len(text))
        return 0
    with open(HDR, "w", encoding="utf-8") as fh:
        fh.write(out)
    print("gen_header: rewrote %d lines (%d bytes) into %s"
          % (len(lines), len(text), HDR))
    return 0


if __name__ == "__main__":
    sys.exit(main())
