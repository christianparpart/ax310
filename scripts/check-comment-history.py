#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Checks that comments describe the code as it is, not as it was.

A comment saying what a thing used to be called, or what an earlier version of
itself claimed, describes a state of the repository that git already records --
and it goes stale in a way nothing detects, because it was never true of the code
in front of you. This project has grown several of them, and a written convention
did not prevent it. A check does.

**Refuted hardware readings are a different thing and are not caught here.** In a
project that reverse-engineers an undocumented protocol, "byte 4 is not brightness,
and here is the capture that looked like it was" stops the next reader deriving the
same wrong answer from the same evidence. Those live in
.agent/rules/hardware-facts.md, where a reader looking for what the deck does will
find them, rather than in a header where the reader is looking for what the code
does. Whichever file they land in, this checker only reads source.

Exits 0 when no source narrates its own past, 1 when one does. There is nothing
external to invoke, so unlike its sibling there is no skip path.
"""

import pathlib
import re
import sys

# Phrasings that only ever introduce the history of the code. Deliberately
# narrow: each one was in a comment this project actually shipped, and a wider
# net catches sentences about the hardware, which are wanted.
PATTERNS = [
    r"\ban earlier version\b",
    r"\ban earlier reading\b",
    r"\ban earlier entry\b",
    r"\bused to be\b",
    r"\bit was called\b",
    r"\bwas once named\b",
    r"\bwe used to\b",
    r"\bis no longer written\b",
    r"\bfor a long time\b",
    r"\bhistorical note\b",
]

SOURCE_SUFFIXES = {".hpp", ".cpp"}
SKIP_DIRECTORIES = {"out", "build", ".git", "_deps"}


def sources(root: pathlib.Path) -> list[pathlib.Path]:
    """@return Every first-party source file, walked rather than asked of git.

    Asking git fails inside the CI container with the dubious-ownership error,
    which is a scar its sibling already carries.
    """
    found = []
    for path in sorted(root.rglob("*")):
        if path.suffix not in SOURCE_SUFFIXES or not path.is_file():
            continue
        if SKIP_DIRECTORIES & set(path.parts):
            continue
        found.append(path)
    return found


def main() -> int:
    root = pathlib.Path(__file__).resolve().parent.parent
    expression = re.compile("|".join(PATTERNS), re.IGNORECASE)

    # Only comment text. A string literal quoting one of these is somebody's
    # narration to a user, which is not this checker's business -- and the
    # patterns above are English, so they turn up in prose the code prints.
    comment = re.compile(r"//.*|/\*.*?\*/", re.DOTALL)

    found = []
    checked = sources(root)
    for path in checked:
        text = path.read_text(encoding="utf-8", errors="replace")
        for block in comment.finditer(text):
            hit = expression.search(block.group())
            if hit:
                line = text.count("\n", 0, block.start() + hit.start()) + 1
                found.append((path.relative_to(root).as_posix(), line, hit.group()))

    if not found:
        print(f"no comment narrates its own past, in {len(checked)} files")
        return 0

    print("These comments describe the code's history rather than the code:")
    for name, line, phrase in found:
        print(f"  {name}:{line}  \"{phrase}\"")
    print()
    print("Say what the code is. Git records what it was.")
    print("A refuted reading of the *hardware* is worth keeping -- put it in")
    print(".agent/rules/hardware-facts.md, next to the evidence it warns about.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
