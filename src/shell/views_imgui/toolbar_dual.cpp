/**
 * PSU toolbar - dual channel, ImGui port.
 *
 * Same widget vocabulary as toolbar_single (big V/A readouts, status,
 * SET popup, OUT button) but two channels side by side in one window.
 * Each channel has its own popup; both pop into the same window without
 * resizing it.
 */

#include "views_imgui.h"

#include "imgui.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

struct ChState {
    psu_channel_state_t snap{};
    bool   popup_open  = false;
    int    popup_focus = 0;
    char   popup_v[16] = "";
    char   popup_a[16] = "";
};

struct State {
    ChState ch[2];
    bool    first_frame = true;
};

ImU32 to_u32(ImVec4 c) { return ImGui::GetColorU32(c); }

void draw_ch_set_popup(instance_t *inst, ChState *c, int ch_idx) {
    char pop_id[32];
    std::snprintf(pop_id, sizeof(pop_id), "##set_pop_%d", ch_idx);
    if (c->popup_open) ImGui::OpenPopup(pop_id);

    if (ImGui::BeginPopup(pop_id)) {
        ImGui::Text("SET CH%d", ch_idx + 1);
        ImGui::Separator();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("V"); ImGui::SameLine();
        ImGui::SetNextItemWidth(140);
        ImGui::SetKeyboardFocusHere(c->popup_focus == 0 ? 0 : -1);
        ImGui::InputText("##v", c->popup_v, sizeof(c->popup_v),
                         ImGuiInputTextFlags_CharsDecimal);
        ImGui::AlignTextToFramePadding();
        ImGui::Text("A"); ImGui::SameLine();
        ImGui::SetNextItemWidth(140);
        ImGui::SetKeyboardFocusHere(c->popup_focus == 1 ? 0 : -1);
        ImGui::InputText("##a", c->popup_a, sizeof(c->popup_a),
                         ImGuiInputTextFlags_CharsDecimal);
        if (ImGui::Button("Apply", ImVec2(80, 0))) {
            float v = std::strtof(c->popup_v, nullptr);
            float a = std::strtof(c->popup_a, nullptr);
            if (v >= 0 && v <= inst->psu_drv->caps.v_max)
                inst->psu_drv->set_voltage(inst->psu_drv, ch_idx + 1, v);
            if (a >= 0 && a <= inst->psu_drv->caps.i_max)
                inst->psu_drv->set_current(inst->psu_drv, ch_idx + 1, a);
            c->popup_open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(80, 0))) {
            c->popup_open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    } else {
        c->popup_open = false;
    }
}

void draw_channel_strip(instance_t *inst, ChState *c, int ch_idx) {
    char child_id[32];
    std::snprintf(child_id, sizeof(child_id), "##ch%d", ch_idx);
    ImGui::BeginChild(child_id, ImVec2(0, 110), true);

    ImGui::TextDisabled("CH%d", ch_idx + 1);

    ImGui::SetWindowFontScale(2.2f);
    ImVec4 vcol = c->snap.valid ? ImVec4(0.0f, 1.0f, 0.55f, 1)
                                : ImVec4(0.45f, 0.45f, 0.50f, 1);
    ImVec4 acol = c->snap.valid ? ImVec4(0.47f, 0.86f, 1.00f, 1)
                                : ImVec4(0.45f, 0.45f, 0.50f, 1);
    char vbuf[24], abuf[24];
    std::snprintf(vbuf, sizeof(vbuf), "%6.3f", c->snap.out_v);
    std::snprintf(abuf, sizeof(abuf), "%6.3f", c->snap.out_a);
    ImGui::TextColored(vcol, "%s", vbuf); ImGui::SameLine();
    ImGui::SetWindowFontScale(1.0f);
    ImGui::TextColored(vcol, "V"); ImGui::SameLine();
    ImGui::Dummy(ImVec2(8, 0)); ImGui::SameLine();
    ImGui::SetWindowFontScale(2.2f);
    ImGui::TextColored(acol, "%s", abuf); ImGui::SameLine();
    ImGui::SetWindowFontScale(1.0f);
    ImGui::TextColored(acol, "A");

    ImGui::Spacing();
    ImVec4 on_c = c->snap.out_on ? ImVec4(0.20f, 0.95f, 0.40f, 1)
                                 : ImVec4(0.55f, 0.20f, 0.20f, 1);
    ImGui::TextColored(on_c, c->snap.out_on ? "ON " : "OFF");
    ImGui::SameLine();
    ImVec4 mode_c = c->snap.cv_mode ? ImVec4(0.40f, 0.95f, 0.55f, 1)
                                    : ImVec4(1.00f, 0.78f, 0.24f, 1);
    ImGui::TextColored(mode_c, c->snap.cv_mode ? "CV" : "CC");
    if (!c->snap.valid) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1, 0.27f, 0.27f, 1), "ERR");
    }

    float btn_w = 56.0f;
    float row_w = ImGui::GetContentRegionAvail().x;
    ImGui::SameLine(row_w - 2 * btn_w - 8);
    char setlbl[8], outlbl[8];
    std::snprintf(setlbl, sizeof(setlbl), "SET##%d", ch_idx);
    std::snprintf(outlbl, sizeof(outlbl), "OUT##%d", ch_idx);
    if (ImGui::Button(setlbl, ImVec2(btn_w, 0))) {
        std::snprintf(c->popup_v, sizeof(c->popup_v), "%.3f", c->snap.set_v);
        std::snprintf(c->popup_a, sizeof(c->popup_a), "%.3f", c->snap.set_a);
        c->popup_focus = 0;
        c->popup_open = true;
    }
    ImGui::SameLine();
    bool out_on = c->snap.out_on;
    if (out_on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0.50f, 0.66f, 1));
    if (ImGui::Button(outlbl, ImVec2(btn_w, 0))) {
        inst->psu_drv->set_output(inst->psu_drv, ch_idx + 1, !out_on);
    }
    if (out_on) ImGui::PopStyleColor();

    draw_ch_set_popup(inst, c, ch_idx);

    ImGui::EndChild();
}

}  // namespace

void *psu_toolbar_dual_state_new(void) { return new State(); }
void  psu_toolbar_dual_state_free(void *st) { delete static_cast<State*>(st); }

void psu_toolbar_dual_draw(instance_t *inst) {
    if (!inst || !inst->psu_drv || !inst->view_state) return;
    State *s = static_cast<State*>(inst->view_state);

    inst->psu_drv->get_channel(inst->psu_drv, 1, &s->ch[0].snap);
    inst->psu_drv->get_channel(inst->psu_drv, 2, &s->ch[1].snap);

    char title[96];
    std::snprintf(title, sizeof(title),
                  "PSU toolbar dual - %s##inst%d", inst->driver_id, inst->id);

    if (s->first_frame) {
        ImGui::SetNextWindowSize(ImVec2(940, 150), ImGuiCond_Once);
        s->first_frame = false;
    }
    if (!ImGui::Begin(title, &inst->open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    bool connected = inst->psu_drv->is_connected(inst->psu_drv);
    ImVec4 dot = connected ? ImVec4(0.20f, 0.85f, 0.40f, 1)
                           : ImVec4(0.95f, 0.30f, 0.30f, 1);
    ImGui::TextDisabled("%s", inst->port);
    ImGui::SameLine(ImGui::GetWindowWidth() - 28);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddCircleFilled(ImVec2(p.x + 6, p.y + 8), 5, to_u32(dot));
    ImGui::NewLine();

    if (ImGui::BeginTable("tlbl_dual", 2, ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("##c1", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("##c2", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); draw_channel_strip(inst, &s->ch[0], 0);
        ImGui::TableSetColumnIndex(1); draw_channel_strip(inst, &s->ch[1], 1);
        ImGui::EndTable();
    }

    ImGui::End();
}
