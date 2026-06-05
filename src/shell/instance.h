/**
 * Instance manager - owns the live driver + view-state pair for each
 * window the user has launched from the shell.
 *
 * Each ImGui frame the shell calls instance_draw_all() which:
 *   1. polls the driver for fresh state,
 *   2. draws an ImGui::Begin() block for the instance,
 *   3. removes any instances whose window was closed by the user.
 *
 * With viewports enabled in shell.cpp, the user can drag any instance
 * window out and it becomes a real top-level OS window - that's the
 * Phase B replacement for the fork+exec multi-process model.
 */

#pragma once

#include <cstddef>

extern "C" {
#include "psu_driver.h"
#include "dmm_driver.h"
}

struct instance_t {
    int            id;            /* monotonic, used as part of the window label */
    bool           open;          /* user can flip this false via the window's X */
    bool           is_dmm;
    psu_driver_t  *psu_drv;       /* exactly one of these is non-null */
    dmm_driver_t  *dmm_drv;
    char           driver_id[32];
    char           view_id[32];
    char           port[160];
    void          *view_state;    /* per-view state struct, type depends on view_id */
};

#define INSTANCE_MAX 32

struct instance_manager_t {
    instance_t   list[INSTANCE_MAX];
    int          count;
    int          next_id;
    /* Screen-space rect of the central dock node, refreshed each frame by
     * the shell. Instance windows place themselves inside this rect on
     * first use (cascaded), but are NOT docked to it - they appear as
     * free-floating draggable windows. */
    float        center_x, center_y, center_w, center_h;
};

void instance_manager_init   (instance_manager_t *mgr);
void instance_manager_destroy(instance_manager_t *mgr);

/**
 * Open a fresh instance. Looks the driver factory up by id, opens it
 * against `port` + `baud`, allocates the view's per-instance state.
 *
 * Returns true on success; false on driver-open failure (with an English
 * message stashed into *error_out if non-null - pointer to static
 * storage, no free needed).
 */
bool instance_open(instance_manager_t *mgr,
                   bool is_dmm,
                   const char *driver_id,
                   const char *view_id,
                   const char *port,
                   int baud,
                   const char **error_out);

/** Draw every live instance for this frame; reaps closed ones. */
void instance_draw_all(instance_manager_t *mgr);
