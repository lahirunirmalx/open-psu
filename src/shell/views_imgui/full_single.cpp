/**
 * PSU full — single channel, ImGui port.
 *
 * Bench-instrument layout for one channel:
 *   - Dot-matrix VFD readout (V, A, W) via shared widgets
 *   - Big colored numeric setpoint inputs
 *   - OUTPUT toggle + status pill
 *   - V and A bar meters
 *   - Mini auto-scaling scope of recent (V, A) samples
 *
 * The keypad is replaced by direct ImGui::InputFloat fields — ImGui's
 * keyboard input handling is good enough that a phone-style keypad
 * isn't needed.
 */

#include "views_imgui.h"
#include "widgets.h"

#include "imgui.h"

#include <cstdio>
#include <cstring>

namespace {

#define TRACE_CAP 256

struct State {
    psu_channel_state_t snap{};
    float   set_v = 0;
    float   set_a = 0;
    bool    set_dirty_v = false;
    bool    set_dirty_a = false;
    float   trace_v[TRACE_CAP];
    float   trace_a[TRACE_CAP];
    size_t  head = 0;
    size_t  count = 0;
    bool    first_frame = true;
    bool    initialised = false;
};

void push_sample(State *s, float v, float a) {
    s->trace_v[s->head] = v;
    s->trace_a[s->head] = a;
    s->head = (s->head + 1) % TRACE_CAP;
    if (s->count < TRACE_CAP) s->count++;
}

}  // namespace

void *psu_full_single_state_new(void)     { return new State(); }
void  psu_full_single_state_free(void *p) { delete static_cast<State*>(p); }

void  psu_full_single_draw(instance_t *inst) {
    if (!inst || !inst->psu_drv || !inst->view_state) return;
    State *s = static_cast<State*>(inst->view_state);

    inst->psu_drv->get_channel(inst->psu_drv, 1, &s->snap);
    if (!s->initialised) {
        s->set_v = s->snap.set_v;
        s->set_a = s->snap.set_a;
        s->initialised = true;
    }
    if (s->snap.valid && (s->snap.out_v > 0.001f || s->snap.out_a > 0.0001f))
        push_sample(s, s->snap.out_v, s->snap.out_a);

    char title[96];
    std::snprintf(title, sizeof(title),
                  "PSU full — %s##inst%d", inst->driver_id, inst->id);

    if (s->first_frame) {
        ImGui::SetNextWindowSize(ImVec2(620, 540), ImGuiCond_Once);
        s->first_frame = false;
    }
    if (!ImGui::Begin(title, &inst->open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    /* Header */
    bool connected = inst->psu_drv->is_connected(inst->psu_drv);
    ImGui::TextDisabled("CH1  ·  %s", inst->port);
    ImGui::SameLine(ImGui::GetWindowWidth() - 28);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 hp = ImGui::GetCursorScreenPos();
    dl->AddCircleFilled(ImVec2(hp.x + 6, hp.y + 8), 5,
                        connected ? IM_COL32(50, 220, 100, 255)
                                  : IM_COL32(240, 80, 80, 255));
    ImGui::NewLine();
    ImGui::Separator();

    /* ---- VFD readout (V, A, W) ---- */
    {
        ImVec2 box_p = ImGui::GetCursorScreenPos();
        float box_w = ImGui::GetContentRegionAvail().x;
        float box_h = 160.0f;
        dl->AddRectFilled(box_p, ImVec2(box_p.x + box_w, box_p.y + box_h),
                          IM_COL32(8, 18, 12, 255), 4.0f);
        dl->AddRect(box_p, ImVec2(box_p.x + box_w, box_p.y + box_h),
                    IM_COL32(60, 60, 65, 255), 4.0f);

        ImU32 vfd_on  = s->snap.out_on ? IM_COL32(0, 255, 120, 255)
                                       : IM_COL32(0, 80, 50, 255);
        ImU32 label_c = IM_COL32(200, 160, 60, 255);

        float dot_r = 3.0f;
        float row_h = (box_h - 24) / 3.0f;
        float left = box_p.x + 14;
        float num_x = box_p.x + 95;
        char buf[24];

        /* Row 1: VOLTAGE */
        float y1 = box_p.y + 8;
        dl->AddText(ImVec2(left, y1 + row_h * 0.5f - 6), label_c, "VOLTAGE");
        std::snprintf(buf, sizeof(buf), "% .3f", s->snap.out_v);
        vfd_string(dl, ImVec2(num_x, y1 + row_h * 0.5f - 14),
                   buf, dot_r, vfd_on, true);
        dl->AddText(ImVec2(box_p.x + box_w - 32, y1 + row_h * 0.5f - 6),
                    vfd_on, "V");

        /* Row 2: CURRENT */
        float y2 = y1 + row_h;
        dl->AddText(ImVec2(left, y2 + row_h * 0.5f - 6), label_c, "CURRENT");
        std::snprintf(buf, sizeof(buf), "% .3f", s->snap.out_a);
        vfd_string(dl, ImVec2(num_x, y2 + row_h * 0.5f - 14),
                   buf, dot_r, vfd_on, true);
        dl->AddText(ImVec2(box_p.x + box_w - 32, y2 + row_h * 0.5f - 6),
                    vfd_on, "A");

        /* Row 3: POWER */
        float y3 = y2 + row_h;
        dl->AddText(ImVec2(left, y3 + row_h * 0.5f - 6), label_c, "POWER");
        std::snprintf(buf, sizeof(buf), "% .2f", s->snap.out_p);
        vfd_string(dl, ImVec2(num_x, y3 + row_h * 0.5f - 14),
                   buf, dot_r, vfd_on, true);
        dl->AddText(ImVec2(box_p.x + box_w - 32, y3 + row_h * 0.5f - 6),
                    vfd_on, "W");

        ImGui::Dummy(ImVec2(box_w, box_h));
    }

    /* ---- Output toggle + status ---- */
    ImGui::Spacing();
    if (s->snap.out_on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0.55f, 0.30f, 1));
    if (ImGui::Button("OUTPUT", ImVec2(120, 32))) {
        inst->psu_drv->set_output(inst->psu_drv, 1, !s->snap.out_on);
    }
    if (s->snap.out_on) ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextColored(s->snap.out_on ? ImVec4(0.2f, 0.95f, 0.4f, 1)
                                      : ImVec4(0.6f, 0.25f, 0.25f, 1),
                       s->snap.out_on ? "  ON " : "  OFF");
    ImGui::SameLine();
    ImGui::TextColored(s->snap.cv_mode ? ImVec4(0.4f, 0.95f, 0.55f, 1)
                                       : ImVec4(1.0f, 0.78f, 0.24f, 1),
                       s->snap.cv_mode ? "  CV" : "  CC");
    if (!s->snap.valid) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1, 0.27f, 0.27f, 1), "  ERR");
    }

    /* ---- Setpoint inputs ---- */
    ImGui::Spacing();
    ImGui::Separator();
    float vmax = inst->psu_drv->caps.v_max;
    float imax = inst->psu_drv->caps.i_max;
    ImGui::SetNextItemWidth(180);
    if (ImGui::InputFloat("Set V", &s->set_v, 0.1f, 1.0f, "%.4f"))
        s->set_dirty_v = true;
    ImGui::SameLine();
    if (ImGui::Button("Apply V")) {
        if (s->set_v >= 0 && s->set_v <= vmax)
            inst->psu_drv->set_voltage(inst->psu_drv, 1, s->set_v);
        s->set_dirty_v = false;
    }

    ImGui::SetNextItemWidth(180);
    if (ImGui::InputFloat("Set A", &s->set_a, 0.01f, 0.1f, "%.4f"))
        s->set_dirty_a = true;
    ImGui::SameLine();
    if (ImGui::Button("Apply A")) {
        if (s->set_a >= 0 && s->set_a <= imax)
            inst->psu_drv->set_current(inst->psu_drv, 1, s->set_a);
        s->set_dirty_a = false;
    }

    /* ---- Bar meters ---- */
    ImGui::Spacing();
    float half_w = ImGui::GetContentRegionAvail().x * 0.5f - 6;
    bar_meter("VOLTAGE", s->snap.out_v, vmax, "V", half_w, 56);
    ImGui::SameLine();
    bar_meter("CURRENT", s->snap.out_a, imax, "A", half_w, 56);

    /* ---- Scope ---- */
    ImGui::Spacing();
    size_t oldest = s->count < TRACE_CAP
                    ? 0
                    : (s->head + TRACE_CAP - s->count) % TRACE_CAP;
    mini_scope("V (green)  /  A (yellow)", s->trace_v, oldest, s->count, TRACE_CAP,
               IM_COL32(80, 255, 120, 255),
               ImGui::GetContentRegionAvail().x, 100);
    mini_scope(nullptr, s->trace_a, oldest, s->count, TRACE_CAP,
               IM_COL32(255, 200, 80, 255),
               ImGui::GetContentRegionAvail().x, 70);

    ImGui::End();
}
