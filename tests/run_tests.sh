#!/usr/bin/env bash
# Checks GotoCounter against counts derived by hand.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPORT="$(mktemp)"
trap 'rm -f "$REPORT"' EXIT
failures=0

# count <report label> -> number on that line of the last report
count() {
  grep -F "  $1" "$REPORT" | head -1 | awk -F: '{gsub(/ /, "", $2); print $2}'
}

# expect <name> <label> <expected>
expect() {
  local got
  got="$(count "$2")"
  if [[ "$got" == "$3" ]]; then
    echo "  PASS  $1: $2 = $got"
  else
    echo "  FAIL  $1: $2 = $got (expected $3)"
    failures=$((failures + 1))
  fi
}

run() {
  GOTO_COUNTER_OUTPUT="$REPORT" "$ROOT/run.sh" "$@" >/dev/null
}

echo "goto_loop.c: n iterations -> n+3 gotos, n+2 conditional branches"
for n in 0 1 10 1000; do
  run "$ROOT/tests/goto_loop.c" -- "$n"
  expect "goto_loop n=$n" "goto (unconditional br)" "$((n + 3))"
  expect "goto_loop n=$n" "conditional br" "$((n + 2))"
done

echo "recursion.c: fib(15) makes 1973 calls, one goto + one cond br each"
run "$ROOT/tests/recursion.c"
expect "recursion" "goto (unconditional br)" 1973
expect "recursion" "conditional br" 1973

echo "switch_computed_goto.c: 7 bytecodes dispatched by each interpreter"
run "$ROOT/tests/switch_computed_goto.c"
expect "switch_computed_goto" "switch" 7
expect "switch_computed_goto" "indirectbr (computed goto)" 7

echo "shapes.cpp: C++ input (regression value, not hand-derived)"
run "$ROOT/tests/shapes.cpp"
expect "shapes" "goto (unconditional br)" 11

if [[ $failures -eq 0 ]]; then
  echo "All tests passed."
else
  echo "$failures test(s) failed."
  exit 1
fi
