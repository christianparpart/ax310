#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Reorders the includes in every first-party source, and touches nothing else.

The companion to check-include-order.py. It formats only the include region, so
running it does not drag in the rest of the formatting drift the project carries.
"""

import pathlib
import shutil
import subprocess
import sys


def main() -> int:
    formatter = shutil.which("clang-format")
    if formatter is None:
        print("clang-format is not installed", file=sys.stderr)
        return 1

    root = pathlib.Path(__file__).resolve().parent.parent
    sources = subprocess.run(
        ["git", "-C", str(root), "ls-files", "src/**/*.cpp", "src/**/*.hpp"],
        capture_output=True, text=True, check=True,
    ).stdout.split()

    fixed = 0
    for name in sources:
        path = root / name
        text = path.read_text()
        numbers = [i + 1 for i, line in enumerate(text.splitlines())
                   if line.startswith("#include")]
        if not numbers:
            continue

        subprocess.run(
            [formatter, f"--lines={numbers[0]}:{numbers[-1]}", "-i", str(path)],
            check=True,
        )
        if path.read_text() != text:
            fixed += 1
            print(f"  reordered {name}")

    print(f"{fixed} file(s) changed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
