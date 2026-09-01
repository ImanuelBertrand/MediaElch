#!/usr/bin/env python3

# Checks the `File.cpp:12-34` citations in a design document by comparing the
# *text* each one resolves to, at two revisions.
#
# Usage:
#  ./scripts/check_doc_citations.py docs/concepts/movie-sets.md [base-rev]
#
# Why this rather than a list of citations:
#
# The documents under docs/concepts/ cite source by line number, and a branch
# that edits those sources silently breaks the citations -- including ones the
# branch never went near, because inserting a line moves everything below it.
# Enumerating the citations and eyeballing them does not catch that: a citation
# that now points at a blank line, or at the next statement down, still looks
# exactly like a citation.  Whether it is still right is a judgement against the
# prose, which is why three review rounds in a row missed some.
#
# So this resolves every citation twice -- against the document and tree at a
# base revision, and against the document and tree now -- and reports the ones
# whose cited *text* changed, whether or not the citation itself was edited.
# That is mechanical, and it is the part a reader cannot do reliably.
#
# It reports three buckets, and only the first is unambiguously a defect:
#
#   1. untouched citations whose text changed -- the citation is now wrong
#   2. citations resolving to a blank or missing line -- almost always wrong
#   3. citations that were edited and do not match any text previously cited
#      for that file -- expected for genuinely new citations, so each one still
#      has to be read; a repointed citation that landed correctly does *not*
#      appear here, because its new text matches what the old one used to say.
#
# A citation may be marked as belonging to another revision by prefixing it,
# e.g. ``master`'s `File.cpp:12` ``; those are resolved against that revision in
# both snapshots.  Only `master` is understood.

import io
import os
import re
import subprocess
import sys

CITE = re.compile(r'`([A-Za-z0-9_/]+\.(?:cpp|h))[:](\d+)(?:-(\d+))?`' r'|`[:](\d+)(?:-(\d+))?`')
# The marker sits in its own backtick span, outside the citation, so it cannot be
# part of the pattern above.  Missing it is not cosmetic: a citation into another
# revision, checked against this tree, reports a difference that is not there --
# and the question that matters, whether it still resolves there, is never asked.
MARKED = re.compile(r"(?:`master`|master)'s\s*$")
OTHER_REV = "origin/master"


def at_revision(rev, path):
    result = subprocess.run(["git", "show", f"{rev}:{path}"], capture_output=True, text=True)
    if result.returncode != 0:
        return None
    return result.stdout.split("\n")


def in_worktree(path):
    if not os.path.exists(path):
        return None
    return io.open(path, encoding="utf-8").read().split("\n")


def resolve(name):
    """A bare basename in the document means the file last named in full."""
    if os.path.exists(name):
        return name
    base = os.path.basename(name)
    for root, _, files in os.walk("src"):
        if base in files:
            return os.path.join(root, base)
    return None


def citations(doc_lines):
    """(marked, path, first, last) for every citation in the document.

    The last fully-named file is carried forward so that a bare `:123`
    continuation resolves, and a citation may be marked on the previous line
    because the document wraps.
    """
    found = []
    last_path = None
    for index, line in enumerate(doc_lines):
        for match in CITE.finditer(line):
            before = line[: match.start()] or (doc_lines[index - 1] if index else "")
            marked = bool(MARKED.search(before.rstrip()))
            if match.group(1):
                path = last_path = match.group(1)
                first, last = int(match.group(2)), match.group(3)
            else:
                if last_path is None:
                    continue
                path = last_path
                first, last = int(match.group(4)), match.group(5)
            found.append((marked, path, first, int(last) if last else first))
    return found


def cited_text(lines, first, last):
    if lines is None or first - 1 >= len(lines):
        return None
    return "\n".join(line.strip() for line in lines[first - 1 : last])


def snapshot(doc_lines, rev):
    """{(path, first, last, marked): the text that citation resolves to}

    Citations into sources that are not in this repository -- the documents cite
    Kodi and Ember heavily -- are skipped rather than reported: nothing here can
    check them, and listing them every run would bury the ones it can.
    """
    out = {}
    skipped = 0
    for marked, path, first, last in citations(doc_lines):
        real = resolve(path)
        if real is None:
            skipped += 1
            continue
        if marked:
            lines = at_revision(OTHER_REV, real)
        else:
            lines = at_revision(rev, real) if rev else in_worktree(real)
        out[(real, first, last, marked)] = cited_text(lines, first, last)
    return out, skipped


def describe(key):
    real, first, last, marked = key
    prefix = f"{OTHER_REV}'s " if marked else ""
    span = f"{first}-{last}" if last != first else f"{first}"
    return f"{prefix}{real}:{span}"


def first_line(text):
    return (text or "<nothing>").splitlines()[0][:78] if text else "<nothing>"


def main():
    if len(sys.argv) < 2:
        sys.exit(f"usage: {sys.argv[0]} <document> [base-rev]")
    doc = sys.argv[1]
    base = sys.argv[2] if len(sys.argv) > 2 else "HEAD"

    base_doc = at_revision(base, doc)
    now_doc = in_worktree(doc)
    if base_doc is None:
        sys.exit(f"cannot read {doc} at {base}")
    if now_doc is None:
        sys.exit(f"cannot read {doc}")

    before, _ = snapshot(base_doc, base)
    after, skipped = snapshot(now_doc, None)

    broken = [(k, before[k], after[k]) for k in after if k in before and before[k] != after[k]]
    empty = [k for k, text in after.items() if not (text or "").strip()]
    edited = []
    for key in after:
        if key in before:
            continue
        real, _, _, marked = key
        previously = [v for (p, _, _, m), v in before.items() if p == real and m == marked]
        if after[key] not in previously:
            edited.append(key)

    print(f"{doc}: {len(before)} citations at {base}, {len(after)} now")
    print(f"({skipped} into sources outside this repository, which cannot be checked here)\n")

    print(f"== UNTOUCHED CITATIONS WHOSE TEXT CHANGED ({len(broken)}) ==")
    print("   every one of these is wrong now")
    for key, was, now in broken:
        print(f"  {describe(key)}")
        print(f"      was: {first_line(was)}")
        print(f"      now: {first_line(now)}")
    if not broken:
        print("  none")

    print(f"\n== CITATIONS RESOLVING TO A BLANK OR MISSING LINE ({len(empty)}) ==")
    for key in empty:
        print(f"  {describe(key)}")
    if not empty:
        print("  none")

    print(f"\n== EDITED CITATIONS NOT MATCHING ANY TEXT PREVIOUSLY CITED ({len(edited)}) ==")
    print("   expected for genuinely new citations; read each one")
    for key in edited:
        print(f"  {describe(key)}  ->  {first_line(after[key])}")
    if not edited:
        print("  none")

    return 1 if broken else 0


if __name__ == "__main__":
    sys.exit(main())
