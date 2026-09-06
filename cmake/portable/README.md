# Portable CMake modules

Everything in this directory is written to be copied. Each module uses nothing
but stock CMake, names no target of the project it sits in, and depends on no
other file here — so another project can vendor one and have it work without
having heard of this one. That is the whole reason the directory exists: `../`
holds the modules that only make sense while building ax310, and this holds the
ones that travel.

Anything added here has to keep that property, and nothing checks it
automatically: a module here that quietly grew a dependency on something in `../`
would still build fine in this repository and break in every other one, so it is
worth looking for when reviewing a change to this directory.

| Module | What it does |
| --- | --- |
| `ClangTidy.cmake` | `-DENABLE_TIDY=ON` runs clang-tidy as part of the C++ compile. |
| `PedanticCompiler.cmake` | Turns on a wide warning set, each flag probed for support first, and optionally makes warnings errors. |
| `Sanitizers.cmake` | `-DENABLE_SANITIZER_{ADDRESS,UNDEFINED,THREAD}=ON` wires up the matching sanitizer. |

All three are ordinary build configuration that happens to have no tie to this
project. They are here because they are copyable, not because anything about them
is promised to stay still.

This directory arrived with a fourth, `CompileCache.cmake`, which selected a
compiler-cache launcher and could install one. It was 1390 lines, nothing here
included it, and the machine this is developed on already reaches ccache through
`/usr/lib64/ccache/` on `PATH`. It was removed rather than kept as documentation
for a feature the build does not have. It is not recoverable from this
repository's history, which was squashed before publication — it came from
[fastcached](https://github.com/LASTRADA-Software/fastcached), which is where to
look for it.
