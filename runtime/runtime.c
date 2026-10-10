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

/* Must match enum BranchKind in pass/GotoCounterPass.cpp. */
enum
{
    KIND_GOTO,
    KIND_COND_BR,
    KIND_SWITCH,
    KIND_INDIRECT_BR,
    NUM_KINDS
};

static const char *const kind_names[NUM_KINDS] =
{
    "goto (unconditional br)",
    "conditional br",
    "switch",
    "indirectbr (computed goto)",
};

/* Everything one instrumented module (source file) hands to the runtime. */
struct gc_module
{
    uint64_t *counters;            /* counters[i]: times site i executed */
    const uint32_t *kinds;         /* kinds[i]: KIND_* of site i */
    const uint32_t *func_idx;      /* func_idx[i]: index into func_names */
    const uint32_t *lines;         /* lines[i]: source line of site i */
    uint32_t n_sites;
    const char *const *func_names;
    uint32_t n_funcs;
    const char *module_name;
    struct gc_module *next;        /* linked list of all modules */
};

/* One node per instrumented source file. */
static struct gc_module *modules;

static void report(void);

/* Called once per source file, before main(). */
void __gc_register(uint64_t *counters,
                   const uint32_t *kinds,
                   const uint32_t *func_idx,
                   const uint32_t *lines,
                   uint32_t n_sites,
                   const char *const *func_names,
                   uint32_t n_funcs,
                   const char *module_name)
{
    struct gc_module *m = malloc(sizeof *m);
    if (!m)
    {
        return;
    }
    m->counters = counters;
    m->kinds = kinds;
    m->func_idx = func_idx;
    m->lines = lines;
    m->n_sites = n_sites;
    m->func_names = func_names;
    m->n_funcs = n_funcs;
    m->module_name = module_name;
    m->next = modules;

    /* Register the exit handler once, as early as possible, so it runs after
     * any atexit handlers the program itself installs. */
    if (!modules)
    {
        atexit(report);
    }
    modules = m;
}

static void report_module(FILE *out, const struct gc_module *m)
{
    /* per_func[f][k]: how many branches of kind k function f executed. */
    uint64_t (*per_func)[NUM_KINDS] = calloc(m->n_funcs, sizeof *per_func);
    if (!per_func)
    {
        return;
    }
    for (uint32_t i = 0; i < m->n_sites; ++i)
    {
        per_func[m->func_idx[i]][m->kinds[i]] += m->counters[i];
    }

    fprintf(out, "\nModule: %s\n", m->module_name);
    fprintf(out, "  %-32s %12s %12s %8s %8s\n",
            "function", "goto", "cond_br", "switch", "indirbr");
    for (uint32_t f = 0; f < m->n_funcs; ++f)
    {
        uint64_t *c = per_func[f];
        if (!c[0] && !c[1] && !c[2] && !c[3])
        {
            continue; /* never executed */
        }
        fprintf(out, "  %-32s %12llu %12llu %8llu %8llu\n",
                m->func_names[f],
                (unsigned long long)c[KIND_GOTO],
                (unsigned long long)c[KIND_COND_BR],
                (unsigned long long)c[KIND_SWITCH],
                (unsigned long long)c[KIND_INDIRECT_BR]);
    }
    free(per_func);

    const char *sites = getenv("GOTO_COUNTER_SITES");
    if (sites && *sites && *sites != '0')
    {
        fprintf(out, "  Executed branch sites (line 0 = no debug location):\n");
        for (uint32_t i = 0; i < m->n_sites; ++i)
        {
            if (m->counters[i])
            {
                fprintf(out, "    %-28s line %-5u %-26s x %llu\n",
                        m->func_names[m->func_idx[i]],
                        m->lines[i],
                        kind_names[m->kinds[i]],
                        (unsigned long long)m->counters[i]);
            }
        }
    }
}

/* Runs at exit (see atexit above). */
static void report(void)
{
    /* Let the program's own buffered output appear before the report. */
    fflush(stdout);
    FILE *out = stderr;
    const char *path = getenv("GOTO_COUNTER_OUTPUT");
    if (path && *path)
    {
        FILE *f = fopen(path, "w");
        if (f)
        {
            out = f;
        }
    }

    uint64_t total[NUM_KINDS] = {0};
    for (const struct gc_module *m = modules; m; m = m->next)
    {
        for (uint32_t i = 0; i < m->n_sites; ++i)
        {
            total[m->kinds[i]] += m->counters[i];
        }
    }

    fprintf(out, "\n================ GotoCounter report ================\n");
    fprintf(out, "Branches executed during this run:\n");
    for (int k = 0; k < NUM_KINDS; ++k)
    {
        fprintf(out, "  %-28s : %llu\n", kind_names[k], (unsigned long long)total[k]);
    }

    for (const struct gc_module *m = modules; m; m = m->next)
    {
        report_module(out, m);
    }
    fprintf(out, "====================================================\n");

    if (out != stderr)
    {
        fclose(out);
    }
}
