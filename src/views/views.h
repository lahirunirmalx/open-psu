/**
 * Views - the catalogue of UI layouts the launcher can offer.
 *
 * Post-Phase-C every view runs in-process via the ImGui shell
 * (src/shell/views_imgui/*); the per-entry .run field that used to
 * point at an SDL-native view function is gone. The launcher and the
 * instance manager dispatch by the stable `id` string.
 */

#ifndef VIEWS_VIEWS_H
#define VIEWS_VIEWS_H

#include "psu_driver.h"
#include "dmm_driver.h"
#include <stddef.h>

/* ---- PSU views ---- */

typedef struct {
    const char *id;            /* stable id, used on CLI: "toolbar-single" */
    const char *display_name;  /* shown in launcher */
    const char *description;   /* one-line tooltip */
    int         min_channels;  /* view requires >= this many driver channels */
} view_def_t;

const view_def_t *const *views_list(size_t *count);
const view_def_t *views_find(const char *id);

/* ---- DMM views ---- */

typedef struct {
    const char *id;            /* stable id, used on CLI: "dmm-toolbar" / "dmm-full" */
    const char *display_name;
    const char *description;
} dmm_view_def_t;

const dmm_view_def_t *const *dmm_views_list(size_t *count);
const dmm_view_def_t *dmm_views_find(const char *id);

#endif
