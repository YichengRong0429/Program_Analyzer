# GotoCounter: counting executed goto instructions with LLVM

GotoCounter is a small dynamic program analyzer for C/C++ built on LLVM. Given
a program, it reports **how many goto instructions were executed during one
run** of that program, broken down by function and, optionally, by source
line.

It is implemented as an LLVM pass plugin (instrumentation, at compile time)
plus a tiny runtime library (counting and reporting, at run time).

```
$ ./run.sh tests/goto_loop.c -- 10
sum of 0..9 = 45

================ GotoCounter report ================
Branches executed during this run:
  goto (unconditional br)      : 13
  conditional br               : 12
  switch                       : 0
  indirectbr (computed goto)   : 0

Module: tests/goto_loop.c
  function                                 goto      cond_br   switch  indirbr
  main                                       13           12        0        0
====================================================
```

## 1. What is a "goto instruction" in LLVM?

Clang first translates C/C++ into **LLVM IR**, a language-independent
intermediate representation. IR code is organized into *basic blocks*
(straight-line code), and every block ends with a *terminator* instruction
that transfers control to another block. There is no `goto` keyword in IR;
the control transfers are:

| Kind | IR | Typical source |
|---|---|---|
| **goto** | `br label %dest` (unconditional branch) | `goto`, `break`, `continue`, loop back-edges, end of an `if` branch |
| conditional branch | `br i1 %c, label %t, label %f` | `if`, loop conditions, `&&`, `\|\|`, `?:` |
| switch | `switch i32 %v, label %default [...]` | `switch` |
| indirect branch | `indirectbr ptr %addr, [...]` | GNU computed goto `goto *p` |

We define a goto instruction as an **unconditional `br`**. It is the IR
counterpart of `GotoStmt` in Soot's Jimple, the Java analogue of this task.
Notice that a program with no `goto` in its source still executes many of
them: every loop jumps back to its header with an unconditional `br`. The
other three kinds are reported as well for context.

## 2. Why the analysis has to be dynamic

Statically (without running the program), the pass can see **where** the
goto instructions are and how many appear in the code. The task asks how
many are **executed** in one run, and that depends on how many times each
loop iterates, which branches are taken, and how many times each function is
called, all of which may depend on the program's input. In general this
cannot be decided statically (it reduces to the halting problem), so we
observe an actual execution instead:

1. **At compile time**, the pass statically locates every branch site and
   inserts a counter increment right before it (*instrumentation*).
2. **At run time**, each executed branch increments its counter; when the
   program exits, the runtime prints the totals.

The pass can also print the static counts (`SHOW_STATIC=1`) so you can
compare "how many gotos are in the code" with "how many ran".

## 3. How it works

```
 prog.c ──clang -O0 -g -emit-llvm──▶ prog.bc
        ──opt -passes=goto-counter──▶ prog.inst.bc      (pass/GotoCounterPass.cpp)
        ──clang + libgoto_runtime.a──▶ executable        (runtime/runtime.c)
        ──run──▶ program output + GotoCounter report
```

**The pass** (`pass/GotoCounterPass.cpp`, a new-pass-manager module pass):

1. Visits every defined function and the terminator of every basic block, and
   classifies it as goto / conditional br / switch / indirectbr.
2. Creates one 64-bit counter per branch site in a global array
   `__gc_counters`, and constant tables recording each site's kind, function
   (demangled for C++), and source line (from debug info).
3. Inserts `atomicrmw add __gc_counters[i], 1` immediately before site `i`.
   The increment is atomic, so multi-threaded programs are counted correctly.
4. Adds a module constructor that calls `__gc_register(...)` before `main`,
   handing the tables to the runtime. Each source file is a separate module,
   so multi-file programs work: every module registers itself.

For example, `goto loop;` in `tests/goto_loop.c` (the loop back-edge, counter
#6 of 7) becomes:

```llvm
  ; before
  br label %19, !dbg !50
  ; after
  %36 = atomicrmw add ptr getelementptr inbounds ([7 x i64], ptr @__gc_counters, i64 0, i64 6), i64 1 monotonic, align 8, !dbg !50
  br label %23, !dbg !50
```

(Block numbers shift after instrumentation because LLVM renumbers unnamed
values; the target is the same `loop:` block.)

**The runtime** (`runtime/runtime.c`) keeps a list of registered modules and
uses `atexit` to print the report when the program exits: totals per kind,
then per function (functions that never executed a branch are omitted).

## 4. Building

Requirements: LLVM/Clang (tested with 18.1.3), CMake ≥ 3.20 and a C/C++
compiler. On Ubuntu 24.04:

```bash
sudo apt install -y clang llvm-dev cmake build-essential
```

Build:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

This produces `build/pass/libGotoCounter.so` (the pass plugin) and
`build/runtime/libgoto_runtime.a` (the runtime). `run.sh` builds them
automatically if they are missing.

## 5. Usage

```bash
./run.sh <source files...> [-- <program arguments...>]

./run.sh tests/goto_loop.c -- 1000          # program arguments after --
./run.sh tests/shapes.cpp                   # C++ works too
./run.sh main.c util.c                      # multi-file programs
```

Options (environment variables):

| Variable | Effect |
|---|---|
| `GOTO_COUNTER_SITES=1` | also list every executed branch site with its source line and count |
| `SHOW_STATIC=1` | print the static per-function branch counts at compile time |
| `OPT_LEVEL=O2` | compile at another optimization level (default `O0`, see below) |
| `KEEP=<dir>` | keep the IR before/after instrumentation (`*.ll`) and the binary in `<dir>` |
| `GOTO_COUNTER_OUTPUT=<file>` | write the report to a file instead of stderr |

Per-site example:

```
$ GOTO_COUNTER_SITES=1 ./run.sh tests/goto_loop.c -- 10
...
  Executed branch sites (line 0 = no debug location):
    main                         line 7     conditional br             x 1
    main                         line 7     goto (unconditional br)    x 1
    main                         line 9     goto (unconditional br)    x 1
    main                         line 12    conditional br             x 11
    main                         line 13    goto (unconditional br)    x 1
    main                         line 16    goto (unconditional br)    x 10
```

The plugin can also be used without `run.sh`:

```bash
# with opt, as run.sh does
clang -O0 -g -emit-llvm -c prog.c -o prog.bc
opt -load-pass-plugin=build/pass/libGotoCounter.so -passes=goto-counter prog.bc -o prog.inst.bc
clang prog.inst.bc build/runtime/libgoto_runtime.a -o prog && ./prog

# or directly inside clang (the pass runs at the end of the pipeline)
clang -O0 -g -fpass-plugin=build/pass/libGotoCounter.so prog.c build/runtime/libgoto_runtime.a -o prog
```

## 6. Validation

`tests/run_tests.sh` checks the analyzer against counts derived by hand:

```bash
tests/run_tests.sh
```

| Test | What it exercises | Expected (derivation) |
|---|---|---|
| `goto_loop.c` | explicit `goto` loop; `n` from argv | **n + 3** gotos: `?:` in line 7 (1), fall into `loop:` (1), `goto done` (1), `goto loop` (n). **n + 2** conditional: `?:` (1) and `if (i >= n)` (n + 1). Checked for n = 0, 1, 10, 1000. |
| `recursion.c` | `fib(15)`, recursion | `fib(15)` makes 2·fib(16) − 1 = **1973** calls, and each call executes exactly one conditional branch and one goto. |
| `switch_computed_goto.c` | `switch` and GNU computed goto | the bytecode has 7 instructions → **7** `switch` and **7** `indirectbr` executions |
| `shapes.cpp` | C++ classes and virtual calls | names are demangled; regression value only |
| `loops.c` | `for`, `while`, `do-while`, `break` | demo: no `goto` in the source, but 946 gotos are executed |

## 7. Limitations and notes

- **Results depend on the optimization level.** The count is measured on the
  IR that is actually compiled. At `-O0` (default) the IR closely follows the
  source; at `-O2` the optimizer merges and removes many branches
  (`goto_loop.c` with n = 10: 13 gotos at `-O0`, 1 at `-O2`).
- **Only code compiled with the pass is counted.** Branches inside libc or
  other prebuilt libraries (e.g. inside `printf`) are not counted.
- **Other terminators are not counted**: `ret`, `invoke` (C++ calls that may
  throw), `callbr`, `resume`, `unreachable`.
- **The report is printed at normal exit** (return from `main` or `exit()`).
  If the program crashes, is killed, or calls `_exit()`, no report is printed.
- Counting costs one atomic add per executed branch, so instrumented
  programs run somewhat slower.

## 8. Project layout

```
pass/GotoCounterPass.cpp   LLVM pass: finds branches and inserts counters
runtime/runtime.c          runtime: collects counters, prints the report at exit
run.sh                     compile -> instrument -> link -> run, in one command
tests/                     example programs and run_tests.sh
CMakeLists.txt             build configuration
```

## References

- LLVM Language Reference, terminator instructions: https://llvm.org/docs/LangRef.html#terminator-instructions
- Writing an LLVM pass (new pass manager): https://llvm.org/docs/WritingAnLLVMNewPMPass.html
- Profiling code with LLVM: https://medium.com/delta-force/profiling-code-with-llvm-f32c5292750a
- Soot tutorials (the Java version of this exercise): https://github.com/Sable/soot/wiki/Tutorials
- A. Møller and M. I. Schwartzbach, *Static Program Analysis*: https://cs.au.dk/~amoeller/spa/
