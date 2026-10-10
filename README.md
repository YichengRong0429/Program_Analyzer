# GotoCounter

An LLVM pass that counts how many goto instructions (unconditional `br` in
LLVM IR) a C/C++ program executes in one run.

## 1. Build and run

![GotoCounter build and run pipeline](docs/pipeline.png)

Requirements: LLVM/Clang 18, CMake ≥ 3.20. On Ubuntu 24.04:

```bash
sudo apt install -y clang llvm-dev cmake build-essential
```

Build:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Run a program (arguments go after `--`):

```bash
./run.sh tests/goto_loop.c -- 10
```

```
================ GotoCounter report ================
Branches executed during this run:
  goto (unconditional br)      : 13
  conditional br               : 12
  switch                       : 0
  indirectbr (computed goto)   : 0
...
```

Options (environment variables):

| Variable | Effect |
|---|---|
| `GOTO_COUNTER_SITES=1` | list every executed branch site with its source line |
| `GOTO_COUNTER_OUTPUT=<file>` | write the report to a file instead of stderr |
| `SHOW_STATIC=1` | print static per-function branch counts at compile time |
| `OPT_LEVEL=O2` | compile at another optimization level (default `O0`) |
| `KEEP=<dir>` | keep the IR (`*.ll`) and binary in `<dir>` |

Without `run.sh`:

```bash
clang -O0 -g -fpass-plugin=build/pass/libGotoCounter.so prog.c build/runtime/libgoto_runtime.a -o prog
./prog
```

## 2. Tests

```bash
tests/run_tests.sh       # checks counts against hand-derived values
```

Large-scale check on [Project CodeNet](https://github.com/IBM/Project_CodeNet).
This compiles each Accepted C/C++ submission with and without the pass, runs
both on the sample input, and checks that the output is unchanged:

```bash
bench/codenet.py --codenet ~/Project_CodeNet --per-problem 5 --limit 2000 --jobs 8 --out results.csv
```

The submissions are untrusted code, so run it in a VM or container.

## 3. How it works

LLVM IR has no `goto` keyword. Every basic block ends with a terminator, and
the pass counts four kinds:

| Kind | IR | Typical source |
|---|---|---|
| **goto** | `br label %dest` | `goto`, `break`, `continue`, loop back-edges |
| conditional br | `br i1 %c, label %t, label %f` | `if`, loop conditions, `&&`, `\|\|` |
| switch | `switch` | `switch` |
| indirectbr | `indirectbr` | GNU computed goto `goto *p` |

The pass (`pass/GotoCounterPass.cpp`) inserts an atomic counter increment
before every branch. The runtime (`runtime/runtime.c`) prints the totals
when the program exits. The analysis has to be dynamic, because the
executed count depends on the input.

## 4. Notes

- Counts depend on the optimization level (`goto_loop.c`, n = 10: 13 gotos at `-O0`, 1 at `-O2`).
- Only code compiled with the pass is counted, so branches inside libc are not.
- The report is printed only on normal exit (return from `main` or `exit()`), not on a crash or `_exit()`.
