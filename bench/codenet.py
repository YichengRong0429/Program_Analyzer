#!/usr/bin/env python3
"""Run GotoCounter over Project CodeNet submissions at scale.

For every sampled Accepted C/C++ submission this script:
  1. compiles it twice: plain (baseline) and with the GotoCounter pass;
  2. runs both on the problem's sample input (derived/input_output);
  3. checks that instrumentation did not change the program's stdout;
  4. records the executed branch counts from the GotoCounter report.

Each row of the output CSV is one submission. The `result` column is:
  ok              instrumented output == baseline output, report parsed
  skip_compile    baseline does not compile with clang (not our problem)
  skip_run        baseline times out / crashes / exits non-zero on the input
  inst_compile    baseline compiles but the instrumented build fails   <- bug
  inst_run        instrumented binary crashes / times out              <- bug
  mismatch        instrumented stdout differs from baseline stdout      <- bug
  no_report       instrumented binary ran but wrote no report           <- bug

`expected_ok` additionally says whether the baseline output matches the
sample output shipped with CodeNet (whitespace-insensitive); it is for
information only, since sample outputs sometimes allow several answers.

Get CodeNet from https://github.com/IBM/Project_CodeNet (the full tarball,
or a benchmark subset that keeps the metadata/ and derived/ folders).

Note: submissions are untrusted code from online judges. They run with CPU,
memory and file-size limits, but not in a sandbox; use a VM or container.

Usage:
  bench/codenet.py --codenet ~/Project_CodeNet --per-problem 5 --limit 2000 \
      --jobs 8 --out results.csv
"""

import argparse
import concurrent.futures as cf
import csv
import os
import random
import re
import resource
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PLUGIN = ROOT / "build" / "pass" / "libGotoCounter.so"
RUNTIME = ROOT / "build" / "runtime" / "libgoto_runtime.a"

KINDS = [
    ("goto", "goto (unconditional br)"),
    ("cond_br", "conditional br"),
    ("switch", "switch"),
    ("indirectbr", "indirectbr (computed goto)"),
]

# CodeNet language name -> (compiler, extra flags). Old judge code often uses
# implicit int / implicit declarations, which clang 16+ rejects by default.
COMPILERS = {
    "C": ("clang", ["-std=gnu11", "-Wno-error=implicit-int",
                    "-Wno-error=implicit-function-declaration",
                    "-Wno-error=int-conversion"]),
    "C++": ("clang++", ["-std=gnu++17"]),
}

FIELDS = ["submission_id", "problem_id", "language", "result", "expected_ok",
          "goto", "cond_br", "switch", "indirectbr", "cpu_time", "code_size",
          "source", "detail"]


def limits(timeout):
    """preexec_fn: CPU, memory and output-file limits for one program run."""
    def apply():
        resource.setrlimit(resource.RLIMIT_CPU, (timeout, timeout + 1))
        resource.setrlimit(resource.RLIMIT_AS, (2 << 30, 2 << 30))
        resource.setrlimit(resource.RLIMIT_FSIZE, (64 << 20, 64 << 20))
    return apply


def compile_one(cc, flags, src, out, instrument, opt_level):
    cmd = [cc, f"-{opt_level}", "-w", *flags, str(src), "-o", str(out), "-lm"]
    if instrument:
        cmd[1:1] = ["-g", f"-fpass-plugin={PLUGIN}"]
        cmd.insert(-3, str(RUNTIME))
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    except subprocess.TimeoutExpired:
        return False, "compile timeout"
    return p.returncode == 0, " | ".join(p.stderr[-300:].splitlines())


def run_one(exe, stdin_path, timeout, env=None):
    """Returns (ok, stdout bytes, detail)."""
    with open(stdin_path, "rb") as fin:
        try:
            p = subprocess.run([str(exe)], stdin=fin, capture_output=True,
                               timeout=timeout * 2, env=env,
                               preexec_fn=limits(timeout), cwd=exe.parent)
        except subprocess.TimeoutExpired:
            return False, b"", "timeout"
    if p.returncode != 0:
        return False, p.stdout, f"exit {p.returncode}"
    return True, p.stdout, ""


def parse_report(path):
    try:
        text = Path(path).read_text()
    except OSError:
        return None
    counts = {}
    for key, label in KINDS:
        m = re.search(rf"^  {re.escape(label)}\s*: (\d+)$", text, re.M)
        if not m:
            return None
        counts[key] = int(m.group(1))
    return counts


def evaluate(task, args):
    row = {k: "" for k in FIELDS}
    row.update({k: task[k] for k in ("submission_id", "problem_id", "language",
                                      "cpu_time", "code_size")})
    row["source"] = str(task["src"])
    cc, flags = COMPILERS[task["language"]]

    work = Path(tempfile.mkdtemp(prefix="gc_codenet_"))
    try:
        # Copy the source so `#include "..."` or writes stay inside work/.
        src = work / task["src"].name
        shutil.copy(task["src"], src)
        base, inst = work / "base", work / "inst"

        ok, err = compile_one(cc, flags, src, base, False, args.opt_level)
        if not ok:
            row.update(result="skip_compile", detail=err)
            return row
        ok, err = compile_one(cc, flags, src, inst, True, args.opt_level)
        if not ok:
            row.update(result="inst_compile", detail=err)
            return row

        ok, base_out, err = run_one(base, task["input"], args.timeout)
        if not ok:
            row.update(result="skip_run", detail=err)
            return row
        expected = Path(task["output"]).read_bytes() if task["output"] else None
        if expected is not None:
            row["expected_ok"] = int(base_out.split() == expected.split())

        report = work / "report.txt"
        env = dict(os.environ, GOTO_COUNTER_OUTPUT=str(report))
        ok, inst_out, err = run_one(inst, task["input"], args.timeout, env)
        if not ok:
            row.update(result="inst_run", detail=err)
            return row
        if inst_out != base_out:
            row.update(result="mismatch", detail="stdout differs from baseline")
            return row
        counts = parse_report(report)
        if counts is None:
            row.update(result="no_report")
            return row
        row.update(result="ok", **counts)
        return row
    finally:
        shutil.rmtree(work, ignore_errors=True)


def collect_tasks(args):
    codenet = Path(args.codenet).expanduser()
    io_dir = codenet / "derived" / "input_output" / "data"
    rng = random.Random(args.seed)
    tasks = []
    for meta in sorted((codenet / "metadata").glob("p*.csv")):
        pid = meta.stem
        inp = io_dir / pid / "input.txt"
        if not inp.is_file():
            continue
        out = io_dir / pid / "output.txt"
        with open(meta, newline="") as f:
            rows = [r for r in csv.DictReader(f)
                    if r["status"] == "Accepted" and r["language"] in args.langs]
        rng.shuffle(rows)
        for r in rows[:args.per_problem]:
            src = (codenet / "data" / pid / r["language"]
                   / f"{r['submission_id']}.{r['filename_ext']}")
            if src.is_file():
                tasks.append(dict(r, src=src, input=inp,
                                  output=out if out.is_file() else None))
    rng.shuffle(tasks)
    return tasks[:args.limit] if args.limit else tasks


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--codenet", required=True, help="Project_CodeNet directory")
    ap.add_argument("--out", default="codenet_results.csv")
    ap.add_argument("--langs", nargs="+", default=["C", "C++"],
                    choices=sorted(COMPILERS))
    ap.add_argument("--per-problem", type=int, default=5,
                    help="Accepted submissions sampled per problem (default 5)")
    ap.add_argument("--limit", type=int, default=0,
                    help="max submissions in total (0 = no limit)")
    ap.add_argument("--jobs", type=int, default=os.cpu_count())
    ap.add_argument("--timeout", type=int, default=10, help="CPU seconds per run")
    ap.add_argument("--opt-level", default="O0")
    ap.add_argument("--seed", type=int, default=0)
    args = ap.parse_args()

    if not PLUGIN.is_file() or not RUNTIME.is_file():
        sys.exit(f"build GotoCounter first: cmake -S {ROOT} -B {ROOT}/build "
                 f"&& cmake --build {ROOT}/build -j")

    tasks = collect_tasks(args)
    print(f"{len(tasks)} submissions to test", file=sys.stderr)

    tally = {}
    with open(args.out, "w", newline="") as f, \
            cf.ThreadPoolExecutor(args.jobs) as pool:
        w = csv.DictWriter(f, FIELDS)
        w.writeheader()
        futures = [pool.submit(evaluate, t, args) for t in tasks]
        for i, fut in enumerate(cf.as_completed(futures), 1):
            row = fut.result()
            w.writerow(row)
            tally[row["result"]] = tally.get(row["result"], 0) + 1
            if i % 100 == 0 or i == len(tasks):
                f.flush()
                print(f"[{i}/{len(tasks)}] {tally}", file=sys.stderr)

    print(f"\nresults written to {args.out}")
    for result, n in sorted(tally.items(), key=lambda kv: -kv[1]):
        print(f"  {result:14s} {n}")
    bugs = sum(tally.get(k, 0) for k in
               ("inst_compile", "inst_run", "mismatch", "no_report"))
    sys.exit(1 if bugs else 0)


if __name__ == "__main__":
    main()
