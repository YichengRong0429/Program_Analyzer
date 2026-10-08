/*
 * Runtime support for GotoCounter.
 *
 * Every instrumented module calls __gc_register() from a constructor before
 * main() runs, handing over its counter array and site tables. When the
 * program exits, report() prints how many branches of each kind executed,
 * in total and per function.
 *
 * Environment variables:
 *   GOTO_COUNTER_OUTPUT=<file>  write the report to <file> instead of stderr
 *   GOTO_COUNTER_SITES=1        also list every branch site that executed
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

enum { KIND_GOTO, KIND_COND_BR, KIND_SWITCH, KIND_INDIRECT_BR, NUM_KINDS };

static const char *const kind_names[NUM_KINDS] = {
    "goto (unconditional br)",
    "conditional br",
    "switch",
    "indirectbr (computed goto)",
};

struct gc_module {
  uint64_t *counters;
  const uint32_t *kinds;
  const uint32_t *func_idx;
  const uint32_t *lines;
  uint32_t n_sites;
  const char *const *func_names;
  uint32_t n_funcs;
  const char *module_name;
  struct gc_module *next;
};

static struct gc_module *modules;

static void report(void);

void __gc_register(uint64_t *counters, const uint32_t *kinds,
                   const uint32_t *func_idx, const uint32_t *lines,
                   uint32_t n_sites, const char *const *func_names,
                   uint32_t n_funcs, const char *module_name) {
  struct gc_module *m = malloc(sizeof *m);
  if (!m)
    return;
  *m = (struct gc_module){counters, kinds,   func_idx,    lines, n_sites,
                          func_names, n_funcs, module_name, modules};
  /* Register the exit handler once, as early as possible, so it runs after
   * any atexit handlers the program itself installs. */
  if (!modules)
    atexit(report);
  modules = m;
}

static void report_module(FILE *out, const struct gc_module *m) {
  uint64_t (*per_func)[NUM_KINDS] = calloc(m->n_funcs, sizeof *per_func);
  if (!per_func)
    return;
  for (uint32_t i = 0; i < m->n_sites; ++i)
    per_func[m->func_idx[i]][m->kinds[i]] += m->counters[i];

  fprintf(out, "\nModule: %s\n", m->module_name);
  fprintf(out, "  %-32s %12s %12s %8s %8s\n", "function", "goto", "cond_br",
          "switch", "indirbr");
  for (uint32_t f = 0; f < m->n_funcs; ++f) {
    uint64_t *c = per_func[f];
    if (!c[0] && !c[1] && !c[2] && !c[3])
      continue; /* never executed */
    fprintf(out, "  %-32s %12llu %12llu %8llu %8llu\n", m->func_names[f],
            (unsigned long long)c[0], (unsigned long long)c[1],
            (unsigned long long)c[2], (unsigned long long)c[3]);
  }
  free(per_func);

  const char *sites = getenv("GOTO_COUNTER_SITES");
  if (sites && *sites && *sites != '0') {
    fprintf(out, "  Executed branch sites (line 0 = no debug location):\n");
    for (uint32_t i = 0; i < m->n_sites; ++i)
      if (m->counters[i])
        fprintf(out, "    %-28s line %-5u %-26s x %llu\n",
                m->func_names[m->func_idx[i]], m->lines[i],
                kind_names[m->kinds[i]], (unsigned long long)m->counters[i]);
  }
}

static void report(void) {
  /* Let the program's own buffered output appear before the report. */
  fflush(stdout);
  FILE *out = stderr;
  const char *path = getenv("GOTO_COUNTER_OUTPUT");
  if (path && *path) {
    FILE *f = fopen(path, "w");
    if (f)
      out = f;
  }

  uint64_t total[NUM_KINDS] = {0};
  for (const struct gc_module *m = modules; m; m = m->next)
    for (uint32_t i = 0; i < m->n_sites; ++i)
      total[m->kinds[i]] += m->counters[i];

  fprintf(out, "\n================ GotoCounter report ================\n");
  fprintf(out, "Branches executed during this run:\n");
  for (int k = 0; k < NUM_KINDS; ++k)
    fprintf(out, "  %-28s : %llu\n", kind_names[k],
            (unsigned long long)total[k]);

  for (const struct gc_module *m = modules; m; m = m->next)
    report_module(out, m);
  fprintf(out, "====================================================\n");

  if (out != stderr)
    fclose(out);
}
