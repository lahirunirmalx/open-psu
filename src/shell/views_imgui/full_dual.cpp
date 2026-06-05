/**
 * PSU full — dual channel, ImGui port.
 *
 * Two channel panels side-by-side in one ImGui window, with a small
 * TRACKING toggle in the header that calls drv->set_tracking() when the
 * driver supports it. Same widget vocabulary as full_single.
 */

#include "views_imgui.h"
#include "widgets.h"

#include "imgui.h"

#include <cstdio>
#include <cstring>

namespace {

#define TRACE_CAP 256

struct ChState {
    psu_channel_state_t snap{};
    float   set_v = 0;
    float   set_a = 0;
    float   trace_v[TRACE_CAP];
    float   trace_a[TRACE_CAP];
    size_t  head = 0;
    size_t  count = 0;
};

struct State {
    ChState ch[2];
    bool    tracking = false;
    bool    first_frame = true;
    bool    initialised = false;
};

void push_sample(ChState *c, float v, float a) {
    c->trace_v[c->head] = v;
    c->trace_a[c->head] = a;
    c->head = (c->head + 1) % TRACE_CAP;
    if (c->count < TRACE_CAP) c->count++;
}

void draw_channel(instance_t *inst, int ch_idx, ChState *c) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float vmax = inst->psu_drv->caps.v_max;
    float imax = inst->psu_drv->caps.i_max;

    char child_id[16];
    std::snprintf(child_id, sizeof(child_id), "##ch%d", ch_idx);
    ImGui::BeginChild(child_id, ImVec2(0, 0), true);

    ImGui::TextDisabled("CH%d", ch_idx + 1);
    ImGui::SameLine();
    ImGui::TextColored(c->snap.out_on ? ImVec4(0.2f, 0.95f, 0.4f, 1)
                                       : ImVec4(0.55f, 0.20f, 0.20f, 1),
                       c->snap.out_on ? "ON" : "OFF");
    ImGui::SameLine();
    ImGui::TextColored(c->snap.cv_mode ? ImVec4(0.4f, 0.95f, 0.55f, 1)
                                       : ImVec4(1.0f, 0.78f, 0.24f, 1),
                       c->snap.cv_mode ? "CV" : "CC");
    if (!c->snap.valid) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1, 0.27f, 0.27f, 1), "ERR");
    }

    /* VFD box */
    {
        ImVec2 box_p = ImGui::GetCursorScreenPos();
        float box_w = ImGui::GetContentRegionAvail().x;
        float box_h = 130.0f;
        dl->AddRectFilled(box_p, ImVec2(box_p.x + box_w, box_p.y + box_h),
                          IM_COL32(8, 18, 12, 255), 4.0f);
        dl->AddRect(box_p, ImVec2(box_p.x + box_w, box_p.y + box_h),
                    IM_COL32(60, 60, 65, 255), 4.0f);
        ImU32 vfd_on = c->snap.out_on ? IM_COL32(0, 255, 120, 255)
                                      : IM_COL32(0, 80, 50, 255);
        ImU32 label_c = IM_COL32(200, 160, 60, 255);
        float dot_r = 2.5f;
        float row_h = (box_h - 18) / 3.0f;
        float left = box_p.x + 10;
        float num_x = box_p.x + 75;
        char buf[24];

        float y1 = box_p.y + 6;
        dl->AddText(ImVec2(left, y1 + row_h * 0.5f - 6), label_c, "V");
        std::snprintf(buf, sizeof(buf), "% .3f", c->snap.out_v);
        vfd_string(dl, ImVec2(num_x, y1 + row_h * 0.5f - 11),
                   buf, dot_r, vfd_on, true);

        float y2 = y1 + row_h;
        dl->AddText(ImVec2(left, y2 + row_h * 0.5f - 6), label_c, "A");
        std::snprintf(buf, sizeof(buf), "% .3f", c->snap.out_a);
        vfd_string(dl, ImVec2(num_x, y2 + row_h * 0.5f - 11),
                   buf, dot_r, vfd_on, true);

        float y3 = y2 + row_h;
        dl->AddText(ImVec2(left, y3 + row_h * 0.5f - 6), label_c, "W");
        std::snprintf(buf, sizeof(buf), "% .2f", c->snap.out_p);
        vfd_string(dl, ImVec2(num_x, y3 + row_h * 0.5f - 11),
                   buf, dot_r, vfd_on, true);

        ImGui::Dummy(ImVec2(box_w, box_h));
    }

    /* OUTPUT */
    ImGui::Spacing();
    if (c->snap.out_on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0.55f, 0.30f, 1));
    char btn[16];
    std::snprintf(btn, sizeof(btn), "OUTPUT##%d", ch_idx);
    if (ImGui::Button(btn, ImVec2(110, 28))) {
        inst->psu_drv->set_output(inst->psu_drv, ch_idx + 1, !c->snap.out_on);
    }
    if (c->snap.out_on) ImGui::PopStyleColor();

    /* Setpoints */
    char id_v[8], id_a[8], ab_v[12], ab_a[12];
    std::snprintf(id_v, sizeof(id_v), "V##%d",  ch_idx);
    std::snprintf(id_a, sizeof(id_a), "A##%d",  ch_idx);
    std::snprintf(ab_v, sizeof(ab_v), "Apply##v%d", ch_idx);
    std::snprintf(ab_a, sizeof(ab_a), "Apply##a%d", ch_idx);

    ImGui::SetNextItemWidth(140);
    ImGui::InputFloat(id_v, &c->set_v, 0.1f, 1.0f, "%.4f");
    ImGui::SameLine();
    if (ImGui::Button(ab_v) && c->set_v >= 0 && c->set_v <= vmax)
        inst->psu_drv->set_voltage(inst->psu_drv, ch_idx + 1, c->set_v);

    ImGui::SetNextItemWidth(140);
    ImGui::InputFloat(id_a, &c->set_a, 0.01f, 0.1f, "%.4f");
    ImGui::SameLine();
    if (ImGui::Button(ab_a) && c->set_a >= 0 && c->set_a <= imax)
        inst->psu_drv->set_current(inst->psu_drv, ch_idx + 1, c->set_a);

    /* Bar meters */
    ImGui::Spacing();
    float w = ImGui::GetContentRegionAvail().x;
    bar_meter("V", c->snap.out_v, vmax, "V", w, 50);
    bar_meter("A", c->snap.out_a, imax, "A", w, 50);

    ImGui::EndChild();
}

}  // namespace

void *psu_full_dual_state_new(void)     { return new State(); }
void  psu_full_dual_state_free(void *p) { delete static_cast<State*>(p); }

void  psu_full_dual_draw(instance_t *inst) {
    if (!inst || !inst->psu_drv || !inst->view_state) return;
    State *s = static_cast<State*>(inst->view_state);

    inst->psu_drv->get_channel(inst->psu_drv, 1, &s->ch[0].snap);
    inst->psu_drv->get_channel(inst->psu_drv, 2, &s->ch[1].snap);
    if (!s->initialised) {
        s->ch[0].set_v = s->ch[0].snap.set_v;
        s->ch[0].set_a = s->ch[0].snap.set_a;
        s->ch[1].set_v = s->ch[1].snap.set_v;
        s->ch[1].set_a = s->ch[1].snap.set_a;
        s->initialised = true;
    }
    for (int i = 0; i < 2; i++) {
        ChState *c = &s->ch[i];
        if (c->snap.valid && (c->snap.out_v > 0.001f || c->snap.out_a > 0.0001f))
            push_sample(c, c->snap.out_v, c->snap.out_a);
    }

    char title[96];
    std::snprintf(title, sizeof(title),
                  "PSU dual — %s##inst%d", inst->driver_id, inst->id);

    if (s->first_frame) {
        ImGui::SetNextWindowSize(ImVec2(1100, 600), ImGuiCond_Once);
        s->first_frame = false;
    }
    if (!ImGui::Begin(title, &inst->open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    /* Header + TRACKING button if the driver supports it. */
    bool connected = inst->psu_drv->is_connected(inst->psu_drv);
    ImGui::TextDisabled("Dual output  ·  %s", inst->port);
    ImGui::SameLine();
    bool can_track = (inst->psu_drv->set_tracking != nullptr)
                  && inst->psu_drv->caps.supports_tracking;
    if (!can_track) ImGui::BeginDisabled();
    if (s->tracking) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0.50f, 0.66f, 1));
    if (ImGui::Button("TRACKING", ImVec2(110, 0))) {
        s->tracking = !s->tracking;
        if (inst->psu_drv->set_tracking)
            inst->psu_drv->set_tracking(inst->psu_drv, s->tracking);
    }
    if (s->tracking) ImGui::PopStyleColor();
    if (!can_track) ImGui::EndDisabled();

    ImGui::SameLine(ImGui::GetWindowWidth() - 28);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 hp = ImGui::GetCursorScreenPos();
    dl->AddCircleFilled(ImVec2(hp.x + 6, hp.y + 8), 5,
                        connected ? IM_COL32(50, 220, 100, 255)
                                  : IM_COL32(240, 80, 80, 255));
    ImGui::NewLine();
    ImGui::Separator();

    if (ImGui::BeginTable("dual", 2, ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("##c1", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("##c2", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); draw_channel(inst, 0, &s->ch[0]);
        ImGui::TableSetColumnIndex(1); draw_channel(inst, 1, &s->ch[1]);
        ImGui::EndTable();
    }

    ImGui::End();
}
