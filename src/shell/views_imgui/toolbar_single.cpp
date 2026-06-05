/**
 * PSU toolbar — single channel, ImGui port.
 *
 * One compact ImGui window per instance: big V/A readout, status flags
 * (ON/OFF, CV/CC, ERR-if-stale), SET button that opens an inline modal
 * for setpoints, OUT button that toggles output.
 *
 * Layout uses regular ImGui widgets where they fit and drops to
 * SetWindowFontScale for the big readouts. Custom dot-matrix VFD comes
 * later in the full views — toolbar wants the compact look.
 */

#include "views_imgui.h"

#include "imgui.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

struct State {
    psu_channel_state_t snap{};
    bool   popup_open    = false;
    int    popup_focus   = 0;   /* 0 = V field, 1 = A field */
    char   popup_v[16]   = "";
    char   popup_a[16]   = "";
    bool   first_frame   = true;
};

ImU32 to_u32(ImVec4 c) { return ImGui::GetColorU32(c); }

void draw_inline_popup(instance_t *inst, State *s) {
    if (!s->popup_open) return;

    ImGui::OpenPopup("##set_popup");
    if (ImGui::BeginPopup("##set_popup")) {
        ImGui::Text("SET CH1");
        ImGui::Separator();

        ImGui::AlignTextToFramePadding();
        ImGui::Text("V");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140);
        ImGui::SetKeyboardFocusHere(s->popup_focus == 0 ? 0 : -1);
        ImGui::InputText("##v", s->popup_v, sizeof(s->popup_v),
                         ImGuiInputTextFlags_CharsDecimal);

        ImGui::AlignTextToFramePadding();
        ImGui::Text("A");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140);
        ImGui::SetKeyboardFocusHere(s->popup_focus == 1 ? 0 : -1);
        ImGui::InputText("##a", s->popup_a, sizeof(s->popup_a),
                         ImGuiInputTextFlags_CharsDecimal);

        if (ImGui::Button("Apply", ImVec2(80, 0))) {
            float v = std::strtof(s->popup_v, nullptr);
            float a = std::strtof(s->popup_a, nullptr);
            if (v >= 0 && v <= inst->psu_drv->caps.v_max)
                inst->psu_drv->set_voltage(inst->psu_drv, 1, v);
            if (a >= 0 && a <= inst->psu_drv->caps.i_max)
                inst->psu_drv->set_current(inst->psu_drv, 1, a);
            s->popup_open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(80, 0))) {
            s->popup_open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    } else {
        /* User clicked outside — treat as cancel. */
        s->popup_open = false;
    }
}

}  // namespace

void *psu_toolbar_single_state_new(void) { return new State(); }
void  psu_toolbar_single_state_free(void *st) { delete static_cast<State*>(st); }

void psu_toolbar_single_draw(instance_t *inst) {
    if (!inst || !inst->psu_drv || !inst->view_state) return;
    State *s = static_cast<State*>(inst->view_state);

    /* Poll latest state every frame. */
    inst->psu_drv->get_channel(inst->psu_drv, 1, &s->snap);

    char title[96];
    std::snprintf(title, sizeof(title),
                  "PSU toolbar — %s##inst%d", inst->driver_id, inst->id);

    if (s->first_frame) {
        ImGui::SetNextWindowSize(ImVec2(540, 130), ImGuiCond_Once);
        s->first_frame = false;
    }
    if (!ImGui::Begin(title, &inst->open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    /* ---- header strip: small label + status dot ---- */
    bool connected = inst->psu_drv->is_connected(inst->psu_drv);
    ImVec4 dot = connected ? ImVec4(0.20f, 0.85f, 0.40f, 1)
                           : ImVec4(0.95f, 0.30f, 0.30f, 1);
    ImGui::TextDisabled("CH1");
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", inst->port);
    ImGui::SameLine(ImGui::GetWindowWidth() - 28);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddCircleFilled(ImVec2(p.x + 6, p.y + 8), 5, to_u32(dot));
    ImGui::NewLine();

    /* ---- big V / A readout ---- */
    ImGui::SetWindowFontScale(2.4f);
    ImVec4 vcol = s->snap.valid ? ImVec4(0.0f, 1.0f, 0.55f, 1)
                                : ImVec4(0.45f, 0.45f, 0.50f, 1);
    ImVec4 acol = s->snap.valid ? ImVec4(0.47f, 0.86f, 1.00f, 1)
                                : ImVec4(0.45f, 0.45f, 0.50f, 1);
    char vbuf[24], abuf[24];
    std::snprintf(vbuf, sizeof(vbuf), "%6.3f", s->snap.out_v);
    std::snprintf(abuf, sizeof(abuf), "%6.3f", s->snap.out_a);
    ImGui::TextColored(vcol, "%s", vbuf);
    ImGui::SameLine();
    ImGui::SetWindowFontScale(1.0f);
    ImGui::TextColored(vcol, "V");
    ImGui::SameLine();
    ImGui::Dummy(ImVec2(16, 0));
    ImGui::SameLine();
    ImGui::SetWindowFontScale(2.4f);
    ImGui::TextColored(acol, "%s", abuf);
    ImGui::SameLine();
    ImGui::SetWindowFontScale(1.0f);
    ImGui::TextColored(acol, "A");

    /* ---- status row ---- */
    ImGui::Spacing();
    ImVec4 on_c = s->snap.out_on ? ImVec4(0.20f, 0.95f, 0.40f, 1)
                                 : ImVec4(0.55f, 0.20f, 0.20f, 1);
    ImGui::TextColored(on_c, s->snap.out_on ? "ON " : "OFF");
    ImGui::SameLine();
    ImVec4 mode_c = s->snap.cv_mode ? ImVec4(0.40f, 0.95f, 0.55f, 1)
                                    : ImVec4(1.00f, 0.78f, 0.24f, 1);
    ImGui::TextColored(mode_c, s->snap.cv_mode ? "CV" : "CC");
    if (!s->snap.valid) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1, 0.27f, 0.27f, 1), "ERR");
    }

    /* ---- right-aligned SET / OUT buttons ---- */
    float btn_w = 60.0f, gap = 8.0f;
    float row_w = ImGui::GetContentRegionAvail().x;
    ImGui::SameLine(row_w - 2 * btn_w - gap);
    if (ImGui::Button("SET", ImVec2(btn_w, 0))) {
        std::snprintf(s->popup_v, sizeof(s->popup_v), "%.3f", s->snap.set_v);
        std::snprintf(s->popup_a, sizeof(s->popup_a), "%.3f", s->snap.set_a);
        s->popup_focus = 0;
        s->popup_open = true;
    }
    ImGui::SameLine();
    bool out_on = s->snap.out_on;
    if (out_on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0.50f, 0.66f, 1));
    if (ImGui::Button("OUT", ImVec2(btn_w, 0))) {
        inst->psu_drv->set_output(inst->psu_drv, 1, !out_on);
    }
    if (out_on) ImGui::PopStyleColor();

    draw_inline_popup(inst, s);

    ImGui::End();
}
