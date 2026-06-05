/**
 * psu_app — entry point.
 *
 * Every UI path now runs through the ImGui shell (src/shell/). The CLI
 * preload args open one instance at startup so script-style invocations
 * like
 *     psu_app --driver=demo --view=toolbar-single --port=-
 * still work: the launcher window comes up with that instance already
 * running; click LAUNCH on more drivers to add more windows.
 *
 * Usage:
 *   psu_app                                          # launcher only
 *   psu_app --list                                   # list drivers + views
 *   psu_app --driver=<id> --view=<id> [--port=<dev>] [--baud=<n>]
 */

#include "drivers/registry.h"
#include "dmm_driver.h"
#include "platform/platform.h"
#include "psu_driver.h"
#include "shell/shell.h"
#include "views/views.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *out, const char *prog) {
    fprintf(out,
        "Usage:\n"
        "  %s                                  # open the launcher\n"
        "  %s --list                           # list drivers + views\n"
        "  %s --driver=<id> --view=<id> [--port=<dev>] [--baud=<n>]\n"
        "      (launcher opens with that instance preloaded)\n"
        "\n"
        "Run \"%s --list\" to see available drivers and views.\n",
        prog, prog, prog, prog);
}

static void list_all(void) {
    size_t n;

    const psu_driver_factory_t *const *psu = psu_drivers_list(&n);
    printf("PSU drivers (%zu):\n", n);
    for (size_t i = 0; i < n; i++)
        printf("  --driver=%-18s %s\n      %s\n",
               psu[i]->id, psu[i]->display_name, psu[i]->description);

    const dmm_driver_factory_t *const *dmm = dmm_drivers_list(&n);
    printf("\nDMM drivers (%zu):\n", n);
    for (size_t i = 0; i < n; i++)
        printf("  --driver=%-18s %s\n      %s\n",
               dmm[i]->id, dmm[i]->display_name, dmm[i]->description);

    const view_def_t *const *psu_v = views_list(&n);
    printf("\nPSU views (%zu):\n", n);
    for (size_t i = 0; i < n; i++)
        printf("  --view=%-18s %s (needs %d ch)\n      %s\n",
               psu_v[i]->id, psu_v[i]->display_name,
               psu_v[i]->min_channels, psu_v[i]->description);

    const dmm_view_def_t *const *dmm_v = dmm_views_list(&n);
    printf("\nDMM views (%zu):\n", n);
    for (size_t i = 0; i < n; i++)
        printf("  --view=%-18s %s\n      %s\n",
               dmm_v[i]->id, dmm_v[i]->display_name, dmm_v[i]->description);
}

static const char *opt_value(const char *arg, const char *prefix) {
    size_t plen = strlen(prefix);
    if (strncmp(arg, prefix, plen) == 0) return arg + plen;
    return NULL;
}

static void resolve_self_exe(const char *argv0, char *out, size_t out_sz) {
    if (pl_self_exe(out, out_sz)) return;
    snprintf(out, out_sz, "%s", argv0 ? argv0 : "psu_app");
}

int main(int argc, char **argv) {
    const char *driver_id = NULL;
    const char *view_id   = NULL;
    const char *port      = NULL;
    int         baud      = 0;

    for (int i = 1; i < argc; i++) {
        const char *v;
        if (strcmp(argv[i], "--list") == 0 || strcmp(argv[i], "-l") == 0) {
            list_all();
            return 0;
        }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(stdout, argv[0]);
            return 0;
        }
        if ((v = opt_value(argv[i], "--driver="))) { driver_id = v; continue; }
        if ((v = opt_value(argv[i], "--view=")))   { view_id   = v; continue; }
        if ((v = opt_value(argv[i], "--port=")))   { port      = v; continue; }
        if ((v = opt_value(argv[i], "--baud=")))   { baud      = atoi(v); continue; }
        fprintf(stderr, "unknown argument: %s\n\n", argv[i]);
        usage(stderr, argv[0]);
        return 2;
    }

    /* --driver and --view must come together. */
    if ((driver_id != NULL) ^ (view_id != NULL)) {
        usage(stderr, argv[0]);
        return 2;
    }

    char self_exe[PATH_MAX];
    resolve_self_exe(argv[0], self_exe, sizeof(self_exe));
    return shell_run_launcher(self_exe, driver_id, view_id, port, baud);
}
