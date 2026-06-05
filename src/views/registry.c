#include "views.h"

#include <string.h>

static const view_def_t k_views[] = {
    {
        .id           = "toolbar-single",
        .display_name = "Toolbar — single channel",
        .description  = "Compact strip: V/A readout, ON/OFF, SET popup. One channel.",
        .min_channels = 1,
    },
    {
        .id           = "toolbar-dual",
        .display_name = "Toolbar — dual channel",
        .description  = "Compact strip: CH1+CH2 side by side, large V/A, SET popup per channel.",
        .min_channels = 2,
    },
    {
        .id           = "full-single",
        .display_name = "Full GUI — single channel",
        .description  = "VFD readouts, bar meters, temperature, scope, collapsible keypad.",
        .min_channels = 1,
    },
    {
        .id           = "full-dual",
        .display_name = "Full GUI — dual channel",
        .description  = "Dual VFD/bars/scope, shared keypad, TRACKING (driver permitting).",
        .min_channels = 2,
    },
};

static const view_def_t *k_view_ptrs[] = {
    &k_views[0], &k_views[1], &k_views[2], &k_views[3],
};

const view_def_t *const *views_list(size_t *count) {
    if (count) *count = sizeof(k_view_ptrs) / sizeof(k_view_ptrs[0]);
    return k_view_ptrs;
}

const view_def_t *views_find(const char *id) {
    if (!id) return NULL;
    size_t n = sizeof(k_view_ptrs) / sizeof(k_view_ptrs[0]);
    for (size_t i = 0; i < n; i++) {
        if (strcmp(k_view_ptrs[i]->id, id) == 0) return k_view_ptrs[i];
    }
    return NULL;
}

/* ---- DMM views ---- */

static const dmm_view_def_t k_dmm_views[] = {
    {
        .id           = "dmm-toolbar",
        .display_name = "DMM toolbar — compact readout",
        .description  = "Big primary reading, mode label, rate indicator. One row.",
    },
    {
        .id           = "dmm-full",
        .display_name = "DMM full — mode/range/rate + trace",
        .description  = "Big readout, mode buttons, range cycle, rate selector, recent-trace.",
    },
};

static const dmm_view_def_t *k_dmm_view_ptrs[] = {
    &k_dmm_views[0], &k_dmm_views[1],
};

const dmm_view_def_t *const *dmm_views_list(size_t *count) {
    if (count) *count = sizeof(k_dmm_view_ptrs) / sizeof(k_dmm_view_ptrs[0]);
    return k_dmm_view_ptrs;
}

const dmm_view_def_t *dmm_views_find(const char *id) {
    if (!id) return NULL;
    size_t n = sizeof(k_dmm_view_ptrs) / sizeof(k_dmm_view_ptrs[0]);
    for (size_t i = 0; i < n; i++) {
        if (strcmp(k_dmm_view_ptrs[i]->id, id) == 0) return k_dmm_view_ptrs[i];
    }
    return NULL;
}
