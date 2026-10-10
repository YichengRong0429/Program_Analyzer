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
tests/run_tests.sh   # checks counts against hand-derived values
```

## 3. How it works

LLVM IR has no `goto` keyword. Every basic block ends with a terminator, and
the pass counts four kinds:

| Kind | IR | Typical source |
|---|---|---|
| **goto** | `br label %dest` | `goto`, `break`, `continue`, loop back-edges |
| conditional br | `br i1 %c, label %t, label %f` | `if`, loop conditions, `&&`, `\|\|` |
| switch | `switch` | `switch` |
| indirectbr | `indirectbr` | GNU computed goto `goto *p` |

The analysis has to be dynamic, because the executed count depends on the
input. The work is split between a compile-time pass and a small runtime
library:

![GotoCounter internals](docs/internals.png)

### Compile time: `GotoCounterPass::run()` (`pass/GotoCounterPass.cpp`)

The pass runs once per module (`sum.bc`). `run.sh` runs it inside `opt`;
with `clang -fpass-plugin` it runs at the end of clang's optimization
pipeline.

1. **Find branch sites.** For every defined function, `classify()` looks at
   the terminator of each basic block and records it in `Sites[]` with its
   kind, function index and source line (from debug info, 0 if missing).
   With `-goto-counter-static` (`SHOW_STATIC=1`) the per-function static
   counts are printed here.
2. **Build tables.** Adds global arrays to the module: `__gc_counters[N]`
   (one zero-initialized 64-bit counter per site), and constant tables
   `__gc_kinds`, `__gc_func_idx`, `__gc_lines` and `__gc_fnames` that
   describe each site.
3. **Instrument.** Inserts `atomicrmw add __gc_counters[i], 1` right before
   site `i`. The increment is atomic, so multithreaded programs are counted
   correctly.
4. **Add constructor.** Creates `__gc_module_ctor`, which passes the
   pointers to these tables to `__gc_register()`, and adds it to
   `llvm.global_ctors`.

The output `sum.inst.bc` is linked with `libgoto_runtime.a` into the final
program.

### Run time: `./program` (`runtime/runtime.c`)

1. **`__gc_module_ctor()`** runs automatically before `main`, once per
   instrumented source file.
2. **`__gc_register()`** stores the module's table pointers in a linked
   list. The first call also registers `report()` with `atexit`. Because it
   is registered this early, it runs after any `atexit` handlers the program
   itself installs.
3. **`main()`** runs as usual. Every executed branch increments its counter.
4. **`report()`** runs at exit. It sums the counters by kind (overall and per
   function) and prints the report. `GOTO_COUNTER_SITES=1` also lists every
   executed site, and `GOTO_COUNTER_OUTPUT=<file>` writes the report to a
   file instead of stderr.

## 4. Notes

- Counts depend on the optimization level (`goto_loop.c`, n = 10: 13 gotos at `-O0`, 1 at `-O2`).
- Only code compiled with the pass is counted, so branches inside libc are not.
- The report is printed only on normal exit (return from `main` or `exit()`), not on a crash or `_exit()`.
