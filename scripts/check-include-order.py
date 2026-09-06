#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Checks that every first-party source orders its includes the way
.clang-format says to -- this project's own headers first.

Why a script rather than a clang-tidy check: `llvm-include-order` only verifies
that includes are sorted *within* a block. It does not read IncludeCategories and
cannot see a block that is in the wrong place, which is exactly the mistake this
guards against. Checked, after assuming the opposite.

Why not `clang-format --dry-run --Werror` on whole files: the project is not
clang-format-clean for anything else -- ColumnLimit was inherited along with the
rest of the config and disagrees with how 178 lines are wrapped. Reformatting all
of that is a separate decision from keeping includes in order, so this looks at
the include region and nothing else.

Exits 0 when every file agrees, 1 when one does not, and 4 -- CTest's skip code
here -- when clang-format is not installed.
"""

import pathlib
import shutil
import subprocess
import sys


def include_region(text: str) -> tuple[int, int] | None:
    """The first and last line numbers holding an #include, 1-based."""
    lines = text.splitlines()
    numbers = [i + 1 for i, line in enumerate(lines) if line.startswith("#include")]
    return (numbers[0], numbers[-1]) if numbers else None


def main() -> int:
    formatter = shutil.which("clang-format")
    if formatter is None:
        print("clang-format is not installed, so include order is not checked")
        return 4

    root = pathlib.Path(__file__).resolve().parent.parent
    sources = subprocess.run(
        ["git", "-C", str(root), "ls-files", "src/**/*.cpp", "src/**/*.hpp"],
        capture_output=True, text=True, check=True,
    ).stdout.split()

    wrong = []
    for name in sources:
        path = root / name
        text = path.read_text()
        region = include_region(text)
        if region is None:
            continue

        first, last = region
        formatted = subprocess.run(
            [formatter, f"--lines={first}:{last}", f"--assume-filename={path}"],
            input=text, capture_output=True, text=True, check=True,
        ).stdout
        if formatted != text:
            wrong.append(name)

    if not wrong:
        print(f"include order is right in all {len(sources)} files")
        return 0

    print("These files order their includes differently from .clang-format:")
    for name in wrong:
        print(f"  {name}")
    print()
    print("This project's own headers go first. To fix, for each file above:")
    print("  clang-format --lines=<first include>:<last include> -i <file>")
    print("or run this script's sibling: scripts/fix-include-order.py")
    return 1


if __name__ == "__main__":
    sys.exit(main())
