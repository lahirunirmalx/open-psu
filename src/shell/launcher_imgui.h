/**
 * ImGui-based launcher state + draw entry-point. C++ only; consumed by
 * shell.cpp.
 */

#pragma once

#include <stddef.h>

/* The factory / view typedefs are anonymous-struct-with-typedef in the
 * C headers, so they can't be forward-declared in C++. Pull them in via
 * extern "C". */
extern "C" {
#include "psu_driver.h"
#include "dmm_driver.h"
#include "views/views.h"
}

struct instance_manager_t;  /* opaque to the launcher; defined in instance.h */

struct launcher_imgui_state {
    /* The shell creates one of these and hands the launcher a pointer
     * so LAUNCH can spawn windows in-process. */
    instance_manager_t *mgr = nullptr;

    /* Cached registry pointers + sizes. */
    const psu_driver_factory_t *const *psu_drv = nullptr;
    size_t                             n_psu_drv = 0;
    const dmm_driver_factory_t *const *dmm_drv = nullptr;
    size_t                             n_dmm_drv = 0;
    const view_def_t           *const *psu_view = nullptr;
    size_t                             n_psu_view = 0;
    const dmm_view_def_t       *const *dmm_view = nullptr;
    size_t                             n_dmm_view = 0;

    /* Selection state. */
    int  sel_kind = 0;          /* 0 = PSU, 1 = DMM */
    int  sel_psu_drv = -1;
    int  sel_dmm_drv = -1;
    int  sel_psu_view = -1;
    int  sel_dmm_view = -1;
    char port[256] = "-";

    /* Status line shown at the bottom. */
    char status[256] = "ready";
    /* 0 = neutral grey, 1 = success green, 2 = error red. */
    int  status_kind = 0;

    /* Set by the QUIT button so shell.cpp exits the main loop. */
    bool want_quit = false;

    /* Absolute path to psu_app for fork+exec. */
    char self_exe[1024] = {};
};

void launcher_imgui_init(launcher_imgui_state *st, const char *self_exe);
void launcher_imgui_draw(launcher_imgui_state *st);
