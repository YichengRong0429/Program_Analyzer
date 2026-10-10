#!/usr/bin/env bash
# Instrument a C/C++ program with GotoCounter, run it once, and print how many
# goto (and other branch) instructions it executed.
#
# Usage:
#   ./run.sh <source files...> [-- <program arguments...>]
#
# Examples:
#   ./run.sh tests/goto_loop.c -- 10
#   ./run.sh a.c b.c util.cpp -- input.txt
#
# Environment variables:
#   OPT_LEVEL=O0          optimization level used for compiling to IR (default O0)
#   SHOW_STATIC=1         also print static (compile-time) counts per function
#   KEEP=<dir>            keep the IR (.ll) and binary in <dir> for inspection
#   GOTO_COUNTER_SITES=1  list every executed branch site with its source line
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="$ROOT/build"
PLUGIN="$BUILD/pass/libGotoCounter.so"
RUNTIME="$BUILD/runtime/libgoto_runtime.a"
OPT="${OPT:-opt}"
OPT_LEVEL="${OPT_LEVEL:-O0}"

if [[ $# -eq 0 ]]; then
  sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'
  exit 1
fi

srcs=()
while [[ $# -gt 0 && "$1" != "--" ]]; do
  srcs+=("$1")
  shift
done
[[ "${1:-}" == "--" ]] && shift

if [[ ! -f "$PLUGIN" || ! -f "$RUNTIME" ]]; then
  echo "[run.sh] building GotoCounter..." >&2
  cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build "$BUILD" -j >/dev/null
fi

if [[ -n "${KEEP:-}" ]]; then
  WORK="$KEEP"
  mkdir -p "$WORK"
else
  WORK="$(mktemp -d)"
  trap 'rm -rf "$WORK"' EXIT
fi

linker=clang
instrumented=()
idx=0
for src in "${srcs[@]}"; do
  case "$src" in
    *.c) cc=clang ;;
    *.cc | *.cpp | *.cxx | *.C) cc=clang++; linker=clang++ ;;
    *) echo "[run.sh] unsupported source file: $src" >&2; exit 1 ;;
  esac
  # Index prefix keeps a/x.c and b/x.c from overwriting each other's files.
  idx=$((idx + 1))
  name="$idx-$(basename "${src%.*}")"
  # 1. C/C++ -> LLVM IR (with debug info so sites map back to source lines).
  "$cc" "-$OPT_LEVEL" -g -emit-llvm -c "$src" -o "$WORK/$name.bc"
  # 2. Run the GotoCounter pass: find every branch and insert a counter.
  "$OPT" -load-pass-plugin="$PLUGIN" -passes=goto-counter \
    ${SHOW_STATIC:+-goto-counter-static} "$WORK/$name.bc" -o "$WORK/$name.inst.bc"
  if [[ -n "${KEEP:-}" ]]; then
    # .bc -> readable .ll
    llvm-dis "$WORK/$name.bc" -o "$WORK/$name.ll"
    llvm-dis "$WORK/$name.inst.bc" -o "$WORK/$name.inst.ll"
  fi
  instrumented+=("$WORK/$name.inst.bc")
done

# 3. Link the instrumented IR with the runtime library that prints the report.
"$linker" "${instrumented[@]}" "$RUNTIME" -o "$WORK/program"

# 4. Run it once. The report is printed when the program exits.
set +e
"$WORK/program" "$@"
status=$?
set -e
exit "$status"
