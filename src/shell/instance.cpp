/**
 * Instance manager — see instance.h.
 */

#include "instance.h"
#include "views_imgui/views_imgui.h"

#include "imgui.h"

extern "C" {
#include "drivers/registry.h"
}

#include <cstdio>
#include <cstring>

void instance_manager_init(instance_manager_t *mgr) {
    mgr->count = 0;
    mgr->next_id = 1;
    for (int i = 0; i < INSTANCE_MAX; i++) mgr->list[i] = {};
}

void instance_manager_destroy(instance_manager_t *mgr) {
    for (int i = 0; i < mgr->count; i++) {
        instance_t *inst = &mgr->list[i];
        if (inst->view_state) {
            /* Dispatch by view_id to free the right struct. */
            if (std::strcmp(inst->view_id, "toolbar-single") == 0)
                psu_toolbar_single_state_free(inst->view_state);
            else if (std::strcmp(inst->view_id, "toolbar-dual") == 0)
                psu_toolbar_dual_state_free(inst->view_state);
            inst->view_state = nullptr;
        }
        if (inst->psu_drv && inst->psu_drv->close)
            inst->psu_drv->close(inst->psu_drv);
        if (inst->dmm_drv && inst->dmm_drv->close)
            inst->dmm_drv->close(inst->dmm_drv);
        inst->psu_drv = nullptr;
        inst->dmm_drv = nullptr;
    }
    mgr->count = 0;
}

static void reap_closed(instance_manager_t *mgr) {
    for (int i = 0; i < mgr->count; ) {
        if (mgr->list[i].open) { i++; continue; }
        instance_t *inst = &mgr->list[i];
        if (inst->view_state) {
            if (std::strcmp(inst->view_id, "toolbar-single") == 0)
                psu_toolbar_single_state_free(inst->view_state);
            else if (std::strcmp(inst->view_id, "toolbar-dual") == 0)
                psu_toolbar_dual_state_free(inst->view_state);
            inst->view_state = nullptr;
        }
        if (inst->psu_drv && inst->psu_drv->close) inst->psu_drv->close(inst->psu_drv);
        if (inst->dmm_drv && inst->dmm_drv->close) inst->dmm_drv->close(inst->dmm_drv);
        /* Shift left. */
        for (int j = i; j < mgr->count - 1; j++) mgr->list[j] = mgr->list[j + 1];
        mgr->count--;
        mgr->list[mgr->count] = {};
    }
}

/* Is this view_id one of the ones already ported to ImGui? */
static bool view_is_imgui(const char *view_id) {
    return std::strcmp(view_id, "toolbar-single") == 0 ||
           std::strcmp(view_id, "toolbar-dual")   == 0;
}

bool instance_open(instance_manager_t *mgr,
                   bool is_dmm,
                   const char *driver_id,
                   const char *view_id,
                   const char *port,
                   int baud,
                   const char **error_out) {
    static char err[128];

    if (mgr->count >= INSTANCE_MAX) {
        std::snprintf(err, sizeof(err), "too many instances (%d open)", mgr->count);
        if (error_out) *error_out = err;
        return false;
    }
    if (!view_is_imgui(view_id)) {
        std::snprintf(err, sizeof(err),
                      "view '%s' not yet ported to ImGui — use the legacy launcher",
                      view_id);
        if (error_out) *error_out = err;
        return false;
    }

    instance_t *inst = &mgr->list[mgr->count];
    *inst = {};
    inst->id      = mgr->next_id++;
    inst->open    = true;
    inst->is_dmm  = is_dmm;
    std::snprintf(inst->driver_id, sizeof(inst->driver_id), "%s", driver_id);
    std::snprintf(inst->view_id,   sizeof(inst->view_id),   "%s", view_id);
    std::snprintf(inst->port,      sizeof(inst->port),      "%s", port ? port : "-");

    if (is_dmm) {
        const dmm_driver_factory_t *f = dmm_drivers_find(driver_id);
        if (!f) {
            std::snprintf(err, sizeof(err), "unknown DMM driver '%s'", driver_id);
            if (error_out) *error_out = err;
            return false;
        }
        if (baud <= 0) baud = f->default_baud;
        inst->dmm_drv = f->open(port, baud);
        if (!inst->dmm_drv) {
            std::snprintf(err, sizeof(err), "failed to open DMM '%s' on %s",
                          driver_id, port ? port : "(none)");
            if (error_out) *error_out = err;
            return false;
        }
    } else {
        const psu_driver_factory_t *f = psu_drivers_find(driver_id);
        if (!f) {
            std::snprintf(err, sizeof(err), "unknown PSU driver '%s'", driver_id);
            if (error_out) *error_out = err;
            return false;
        }
        if (baud <= 0) baud = f->default_baud;
        inst->psu_drv = f->open(port, baud);
        if (!inst->psu_drv) {
            std::snprintf(err, sizeof(err), "failed to open PSU '%s' on %s",
                          driver_id, port ? port : "(none)");
            if (error_out) *error_out = err;
            return false;
        }
    }

    if (std::strcmp(view_id, "toolbar-single") == 0)      inst->view_state = psu_toolbar_single_state_new();
    else if (std::strcmp(view_id, "toolbar-dual") == 0)   inst->view_state = psu_toolbar_dual_state_new();

    mgr->count++;
    return true;
}

void instance_draw_all(instance_manager_t *mgr) {
    for (int i = 0; i < mgr->count; i++) {
        instance_t *inst = &mgr->list[i];
        if (!inst->open) continue;
        if (std::strcmp(inst->view_id, "toolbar-single") == 0)      psu_toolbar_single_draw(inst);
        else if (std::strcmp(inst->view_id, "toolbar-dual") == 0)   psu_toolbar_dual_draw(inst);
    }
    reap_closed(mgr);
}
