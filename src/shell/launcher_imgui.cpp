/**
 * launcher_imgui.cpp — ImGui draw routine for the launcher.
 *
 * Phase A: same UX as the old launcher.c (driver list with PSU/DMM
 * sections, port field, view list, LAUNCH/QUIT) but rendered with ImGui
 * primitives. LAUNCH still fork/execs psu_app for now; views still come
 * from the existing SDL code path until Phase B replaces them.
 */

#include "launcher_imgui.h"
#include "instance.h"

extern "C" {
#include "drivers/registry.h"
#include "platform/platform.h"
}

#include "imgui.h"

#include <cstdio>
#include <cstring>

void launcher_imgui_init(launcher_imgui_state *st, const char *self_exe) {
    st->psu_drv  = psu_drivers_list(&st->n_psu_drv);
    st->dmm_drv  = dmm_drivers_list(&st->n_dmm_drv);
    st->psu_view = views_list      (&st->n_psu_view);
    st->dmm_view = dmm_views_list  (&st->n_dmm_view);
    st->sel_kind     = 0;
    st->sel_psu_drv  = st->n_psu_drv > 0 ? 0 : -1;
    st->sel_psu_view = st->n_psu_view > 0 ? 0 : -1;
    st->sel_dmm_drv  = -1;
    st->sel_dmm_view = -1;
    std::snprintf(st->port, sizeof(st->port), "-");
    std::snprintf(st->status, sizeof(st->status), "ready");
    std::snprintf(st->self_exe, sizeof(st->self_exe), "%s",
                  self_exe ? self_exe : "psu_app");
}

namespace {

void set_status(launcher_imgui_state *st, int kind, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(st->status, sizeof(st->status), fmt, ap);
    va_end(ap);
    st->status_kind = kind;
}

int driver_n_channels(launcher_imgui_state *st) {
    if (st->sel_kind == 0 && st->sel_psu_drv >= 0)
        return st->psu_drv[st->sel_psu_drv]->n_channels_hint;
    return 1;
}

bool can_launch(launcher_imgui_state *st) {
    if (st->port[0] == '\0') return false;
    if (st->sel_kind == 0) {
        if (st->sel_psu_drv < 0 || st->sel_psu_view < 0) return false;
        const view_def_t *v = st->psu_view[st->sel_psu_view];
        return v->min_channels <= driver_n_channels(st);
    }
    return st->sel_dmm_drv >= 0 && st->sel_dmm_view >= 0;
}

void do_launch(launcher_imgui_state *st) {
    const char *drv_id  = nullptr;
    const char *view_id = nullptr;
    int         baud    = 0;
    bool        is_dmm  = false;

    if (st->sel_kind == 0) {
        drv_id  = st->psu_drv [st->sel_psu_drv ]->id;
        view_id = st->psu_view[st->sel_psu_view]->id;
        baud    = st->psu_drv [st->sel_psu_drv ]->default_baud;
    } else {
        drv_id  = st->dmm_drv [st->sel_dmm_drv ]->id;
        view_id = st->dmm_view[st->sel_dmm_view]->id;
        baud    = st->dmm_drv [st->sel_dmm_drv ]->default_baud;
        is_dmm  = true;
    }

    if (!st->mgr) {
        set_status(st, 2, "internal: no instance manager attached");
        return;
    }

    const char *err = nullptr;
    if (instance_open(st->mgr, is_dmm, drv_id, view_id,
                      st->port[0] ? st->port : "-", baud, &err)) {
        set_status(st, 1, "opened %s + %s", drv_id, view_id);
    } else {
        set_status(st, 2, "%s", err ? err : "open failed");
    }
}

void draw_driver_list(launcher_imgui_state *st) {
    ImGui::TextDisabled("DRIVER");
    ImGui::BeginChild("##drivers", ImVec2(0, -190), true);

    ImGui::TextDisabled("PSU");
    ImGui::Separator();
    for (size_t i = 0; i < st->n_psu_drv; i++) {
        const psu_driver_factory_t *f = st->psu_drv[i];
        bool sel = (st->sel_kind == 0) && ((int)i == st->sel_psu_drv);
        if (ImGui::Selectable(f->display_name, sel,
                              ImGuiSelectableFlags_AllowDoubleClick)) {
            st->sel_kind     = 0;
            st->sel_psu_drv  = (int)i;
            /* If the previously-selected PSU view requires more channels
             * than the newly-picked driver provides, unset it. */
            if (st->sel_psu_view >= 0 &&
                st->psu_view[st->sel_psu_view]->min_channels > f->n_channels_hint)
                st->sel_psu_view = -1;
        }
        if (ImGui::IsItemHovered() && f->description)
            ImGui::SetTooltip("%s", f->description);
    }

    ImGui::Spacing();
    ImGui::TextDisabled("DMM");
    ImGui::Separator();
    for (size_t i = 0; i < st->n_dmm_drv; i++) {
        const dmm_driver_factory_t *f = st->dmm_drv[i];
        bool sel = (st->sel_kind == 1) && ((int)i == st->sel_dmm_drv);
        if (ImGui::Selectable(f->display_name, sel)) {
            st->sel_kind    = 1;
            st->sel_dmm_drv = (int)i;
        }
        if (ImGui::IsItemHovered() && f->description)
            ImGui::SetTooltip("%s", f->description);
    }

    ImGui::EndChild();
}

void draw_view_list(launcher_imgui_state *st) {
    ImGui::TextDisabled("VIEW");
    ImGui::BeginChild("##views", ImVec2(0, -120), true);

    int driver_ch = driver_n_channels(st);
    bool psu_active = (st->sel_kind == 0);

    ImGui::TextDisabled("PSU");
    ImGui::Separator();
    for (size_t i = 0; i < st->n_psu_view; i++) {
        const view_def_t *v = st->psu_view[i];
        bool fits    = v->min_channels <= driver_ch;
        bool enabled = psu_active && fits;
        bool sel     = enabled && ((int)i == st->sel_psu_view);
        char label[160];
        if (psu_active && !fits)
            std::snprintf(label, sizeof(label), "%s   [needs %d ch]",
                          v->display_name, v->min_channels);
        else
            std::snprintf(label, sizeof(label), "%s", v->display_name);

        if (!enabled) ImGui::BeginDisabled();
        if (ImGui::Selectable(label, sel)) {
            if (enabled) st->sel_psu_view = (int)i;
        }
        if (!enabled) ImGui::EndDisabled();
        if (ImGui::IsItemHovered() && v->description)
            ImGui::SetTooltip("%s", v->description);
    }

    ImGui::Spacing();
    ImGui::TextDisabled("DMM");
    ImGui::Separator();
    bool dmm_active = (st->sel_kind == 1);
    for (size_t i = 0; i < st->n_dmm_view; i++) {
        const dmm_view_def_t *v = st->dmm_view[i];
        bool enabled = dmm_active;
        bool sel = enabled && ((int)i == st->sel_dmm_view);
        if (!enabled) ImGui::BeginDisabled();
        if (ImGui::Selectable(v->display_name, sel)) {
            if (enabled) st->sel_dmm_view = (int)i;
        }
        if (!enabled) ImGui::EndDisabled();
        if (ImGui::IsItemHovered() && v->description)
            ImGui::SetTooltip("%s", v->description);
    }

    ImGui::EndChild();
}

void draw_port_and_buttons(launcher_imgui_state *st) {
    ImGui::TextDisabled("PORT");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##port", st->port, sizeof(st->port));

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    bool launchable = can_launch(st);
    if (!launchable) ImGui::BeginDisabled();
    if (ImGui::Button("LAUNCH", ImVec2(140, 32))) {
        do_launch(st);
    }
    if (!launchable) ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("QUIT", ImVec2(100, 32))) st->want_quit = true;

    ImGui::SameLine();
    ImVec4 col;
    switch (st->status_kind) {
        case 1:  col = ImVec4(0.4f, 0.85f, 0.45f, 1); break;
        case 2:  col = ImVec4(0.95f, 0.35f, 0.35f, 1); break;
        default: col = ImVec4(0.7f, 0.7f, 0.72f, 1); break;
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(col, "  status:  %s", st->status);
}

}  // namespace

void launcher_imgui_draw(launcher_imgui_state *st) {
    /* Fill the whole main viewport. */
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar
                           | ImGuiWindowFlags_NoCollapse
                           | ImGuiWindowFlags_NoResize
                           | ImGuiWindowFlags_NoMove
                           | ImGuiWindowFlags_NoBringToFrontOnFocus
                           | ImGuiWindowFlags_NoNavFocus;

    ImGui::Begin("Open LabBench", nullptr, flags);

    ImGui::Text("Open LabBench");
    ImGui::SameLine();
    ImGui::TextDisabled("— pick driver + view, click LAUNCH to open an instrument window.");
    ImGui::Separator();

    /* Two-column layout: drivers on the left, port + views on the right. */
    if (ImGui::BeginTable("layout", 2, ImGuiTableFlags_BordersInnerV |
                                        ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("##l", ImGuiTableColumnFlags_WidthStretch, 0.45f);
        ImGui::TableSetupColumn("##r", ImGuiTableColumnFlags_WidthStretch, 0.55f);

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        draw_driver_list(st);

        ImGui::TableSetColumnIndex(1);
        draw_view_list(st);

        ImGui::EndTable();
    }

    ImGui::Spacing();
    draw_port_and_buttons(st);

    ImGui::End();
}
