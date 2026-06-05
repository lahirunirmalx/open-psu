/**
 * psu_app — single binary that runs one view against one driver.
 *
 * No arguments: open the GUI launcher (pick driver/view/port, click LAUNCH
 * which fork/execs psu_app again with the chosen flags — so each window is
 * its own process and several can run side by side).
 *
 * With --driver and --view: run that combination directly (this is what the
 * launcher invokes in its children). The driver id selects between the PSU
 * and DMM registries; the view id is looked up in the matching view list.
 *
 * Usage:
 *   psu_app                                          # launcher GUI
 *   psu_app --list                                   # CLI: list drivers + views
 *   psu_app --driver=<id> --view=<id> [--port=<dev>] [--baud=<n>]
 */

#include "drivers/registry.h"
#include "dmm_driver.h"
#include "launcher.h"
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
        "  %s                                  # open GUI launcher\n"
        "  %s --list                           # list drivers + views\n"
        "  %s --driver=<id> --view=<id> [--port=<dev>] [--baud=<n>]\n"
        "\n"
        "Run \"%s --list\" to see available drivers and views.\n",
        prog, prog, prog, prog);
}

static void list_all(void) {
    size_t n;

    const psu_driver_factory_t *const *psu = psu_drivers_list(&n);
    printf("PSU drivers (%zu):\n", n);
    for (size_t i = 0; i < n; i++) {
        printf("  --driver=%-18s %s\n      %s\n",
               psu[i]->id, psu[i]->display_name, psu[i]->description);
    }

    const dmm_driver_factory_t *const *dmm = dmm_drivers_list(&n);
    printf("\nDMM drivers (%zu):\n", n);
    for (size_t i = 0; i < n; i++) {
        printf("  --driver=%-18s %s\n      %s\n",
               dmm[i]->id, dmm[i]->display_name, dmm[i]->description);
    }

    const view_def_t *const *psu_v = views_list(&n);
    printf("\nPSU views (%zu):\n", n);
    for (size_t i = 0; i < n; i++) {
        const char *status = psu_v[i]->run ? "" : "  [not yet ported]";
        printf("  --view=%-18s %s (needs %d ch)%s\n      %s\n",
               psu_v[i]->id, psu_v[i]->display_name,
               psu_v[i]->min_channels, status, psu_v[i]->description);
    }

    const dmm_view_def_t *const *dmm_v = dmm_views_list(&n);
    printf("\nDMM views (%zu):\n", n);
    for (size_t i = 0; i < n; i++) {
        printf("  --view=%-18s %s\n      %s\n",
               dmm_v[i]->id, dmm_v[i]->display_name, dmm_v[i]->description);
    }
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

/* Run a PSU driver + view. */
static int run_psu(const psu_driver_factory_t *fac,
                   const char *driver_id, const char *view_id,
                   const char *port, int baud) {
    const view_def_t *view = views_find(view_id);
    if (!view) {
        fprintf(stderr, "PSU driver '%s' selected but view '%s' isn't a PSU view\n",
                driver_id, view_id);
        return 2;
    }
    if (!view->run) {
        fprintf(stderr, "view '%s' is not yet ported\n", view_id);
        return 2;
    }
    if (baud <= 0) baud = fac->default_baud;

    psu_driver_t *drv = fac->open(port, baud);
    if (!drv) {
        fprintf(stderr, "failed to open PSU driver '%s' on %s @ %d baud\n",
                driver_id, port ? port : "(none)", baud);
        return 1;
    }
    if (drv->caps.n_channels < view->min_channels) {
        fprintf(stderr,
                "driver '%s' has %d channel(s) but view '%s' needs %d\n",
                driver_id, drv->caps.n_channels, view_id, view->min_channels);
        drv->close(drv);
        return 2;
    }
    int rc = view->run(drv);
    drv->close(drv);
    return rc;
}

/* Run a DMM driver + view. */
static int run_dmm(const dmm_driver_factory_t *fac,
                   const char *driver_id, const char *view_id,
                   const char *port, int baud) {
    const dmm_view_def_t *view = dmm_views_find(view_id);
    if (!view) {
        fprintf(stderr, "DMM driver '%s' selected but view '%s' isn't a DMM view\n",
                driver_id, view_id);
        return 2;
    }
    if (baud <= 0) baud = fac->default_baud;

    dmm_driver_t *drv = fac->open(port, baud);
    if (!drv) {
        fprintf(stderr, "failed to open DMM driver '%s' on %s @ %d baud\n",
                driver_id, port ? port : "(none)", baud);
        return 1;
    }
    int rc = view->run(drv);
    drv->close(drv);
    return rc;
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

    /* No driver/view picked from the CLI → show launcher.
     *
     * Phase A: the new ImGui-based shell is the default. Set
     * OPENBENCH_LEGACY_LAUNCHER=1 in the env to fall back to the
     * original SDL-native launcher (kept around until Phase B+ ports
     * everything). */
    if (!driver_id && !view_id) {
        char self_exe[PATH_MAX];
        resolve_self_exe(argv[0], self_exe, sizeof(self_exe));
        const char *legacy = getenv("OPENBENCH_LEGACY_LAUNCHER");
        if (legacy && *legacy && *legacy != '0')
            return launcher_run(self_exe);
        return shell_run_launcher(self_exe);
    }

    if (!driver_id || !view_id) {
        usage(stderr, argv[0]);
        return 2;
    }

    /* Driver id is globally unique across PSU + DMM registries; try both. */
    const psu_driver_factory_t *psu_fac = psu_drivers_find(driver_id);
    if (psu_fac) return run_psu(psu_fac, driver_id, view_id, port, baud);

    const dmm_driver_factory_t *dmm_fac = dmm_drivers_find(driver_id);
    if (dmm_fac) return run_dmm(dmm_fac, driver_id, view_id, port, baud);

    fprintf(stderr, "unknown driver: %s\n", driver_id);
    return 2;
}
