/**
 * PSU full - dual channel, ImGui port.
 *
 * Two channel panels side-by-side in one ImGui window, with a small
 * TRACKING toggle in the header that calls drv->set_tracking() when the
 * driver supports it. Same widget vocabulary as full_single.
 */

#include "views_imgui.h"
#include "widgets.h"

#include "imgui.h"

#include <cstdio>
#include <cstdlib>
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

    /* Shared keypad - drives whichever (channel, V|A) is selected. */
    char    kp_buf[16] = "";
    int     kp_channel = 0;   /* 0 or 1 */
    int     kp_target  = 0;   /* 0 = V, 1 = A */
    bool    kp_open    = true;
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

    /* VFD box - vertically centred per row */
    {
        float dot_r = 2.5f;
        float vfd_h = vfd_height(dot_r);
        float row_h = vfd_h + 10.0f;
        float pad_y = 8.0f;
        float box_h = pad_y * 2 + row_h * 3;
        float box_w = ImGui::GetContentRegionAvail().x;
        ImVec2 box_p = ImGui::GetCursorScreenPos();
        dl->AddRectFilled(box_p, ImVec2(box_p.x + box_w, box_p.y + box_h),
                          IM_COL32(8, 18, 12, 255), 4.0f);
        dl->AddRect(box_p, ImVec2(box_p.x + box_w, box_p.y + box_h),
                    IM_COL32(60, 60, 65, 255), 4.0f);
        ImU32 vfd_on = c->snap.out_on ? IM_COL32(0, 255, 120, 255)
                                      : IM_COL32(0, 80, 50, 255);
        ImU32 label_c = IM_COL32(200, 160, 60, 255);

        float text_h = ImGui::GetFontSize();
        struct Row { const char *label; const char *fmt; float val; };
        const Row rows_data[3] = {
            {"V", "% 7.3f", c->snap.out_v},
            {"A", "% 7.3f", c->snap.out_a},
            {"W", "% 7.2f", c->snap.out_p},
        };
        char buf[24];
        float left = box_p.x + 10.0f;
        float num_x = box_p.x + 70.0f;
        for (int i = 0; i < 3; i++) {
            float row_top = box_p.y + pad_y + i * row_h;
            float row_mid = row_top + row_h * 0.5f;
            float vfd_top = row_mid - vfd_h * 0.5f - dot_r;
            float text_y = row_mid - text_h * 0.5f;
            dl->AddText(ImVec2(left, text_y), label_c, rows_data[i].label);
            std::snprintf(buf, sizeof(buf), rows_data[i].fmt, rows_data[i].val);
            vfd_string(dl, ImVec2(num_x, vfd_top), buf, dot_r, vfd_on, true);
        }
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

void kp_append(State *s, const char *piece) {
    size_t cur = std::strlen(s->kp_buf);
    if (*piece == '.' && std::strchr(s->kp_buf, '.')) return;
    if (cur + std::strlen(piece) + 1 >= sizeof(s->kp_buf)) return;
    std::strcat(s->kp_buf, piece);
}

void kp_apply(State *s, instance_t *inst) {
    if (!s->kp_buf[0]) return;
    float val = (float)std::atof(s->kp_buf);
    float vmax = inst->psu_drv->caps.v_max;
    float imax = inst->psu_drv->caps.i_max;
    int ch = s->kp_channel + 1;
    ChState *c = &s->ch[s->kp_channel];
    if (s->kp_target == 0) {
        if (val < 0) val = 0;
        if (val > vmax) val = vmax;
        c->set_v = val;
        inst->psu_drv->set_voltage(inst->psu_drv, ch, val);
    } else {
        if (val < 0) val = 0;
        if (val > imax) val = imax;
        c->set_a = val;
        inst->psu_drv->set_current(inst->psu_drv, ch, val);
    }
    s->kp_buf[0] = '\0';
}

void draw_keypad(State *s, instance_t *inst) {
    if (!ImGui::CollapsingHeader("Keypad", &s->kp_open,
                                 ImGuiTreeNodeFlags_DefaultOpen))
        return;

    ImGui::Text("Channel:");
    ImGui::SameLine();
    if (ImGui::RadioButton("CH1", s->kp_channel == 0)) s->kp_channel = 0;
    ImGui::SameLine();
    if (ImGui::RadioButton("CH2", s->kp_channel == 1)) s->kp_channel = 1;
    ImGui::SameLine();
    ImGui::Text("  Target:");
    ImGui::SameLine();
    if (ImGui::RadioButton("Set V##kp", s->kp_target == 0)) {
        s->kp_target = 0;
        std::snprintf(s->kp_buf, sizeof(s->kp_buf), "%.3f",
                      s->ch[s->kp_channel].set_v);
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Set A##kp", s->kp_target == 1)) {
        s->kp_target = 1;
        std::snprintf(s->kp_buf, sizeof(s->kp_buf), "%.3f",
                      s->ch[s->kp_channel].set_a);
    }

    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##kp_buf_d", s->kp_buf, sizeof(s->kp_buf),
                     ImGuiInputTextFlags_CharsDecimal);

    const float bw = (ImGui::GetContentRegionAvail().x - 6 * 3) / 4.0f;
    const float bh = 28.0f;
    struct Btn { const char *lbl; int kind; const char *arg; };
    static const Btn rows[4][4] = {
        {{"7", 0, "7"}, {"8", 0, "8"}, {"9", 0, "9"}, {"BS", 1, nullptr}},
        {{"4", 0, "4"}, {"5", 0, "5"}, {"6", 0, "6"}, {"CLR", 2, nullptr}},
        {{"1", 0, "1"}, {"2", 0, "2"}, {"3", 0, "3"}, {".", 0, "."}},
        {{"0", 0, "0"}, {"00", 0, "00"}, {"000", 0, "000"}, {"ENTER", 4, nullptr}},
    };
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            const Btn &b = rows[r][c];
            if (c > 0) ImGui::SameLine(0, 6);
            ImGui::PushID(r * 4 + c);
            bool is_enter = (b.kind == 4);
            if (is_enter) ImGui::PushStyleColor(ImGuiCol_Button,
                                                ImVec4(0.20f, 0.55f, 0.30f, 1));
            if (ImGui::Button(b.lbl, ImVec2(bw, bh))) {
                switch (b.kind) {
                    case 0: kp_append(s, b.arg); break;
                    case 1: {
                        size_t n = std::strlen(s->kp_buf);
                        if (n > 0) s->kp_buf[n - 1] = '\0';
                        break;
                    }
                    case 2: s->kp_buf[0] = '\0'; break;
                    case 4: kp_apply(s, inst);   break;
                }
            }
            if (is_enter) ImGui::PopStyleColor();
            ImGui::PopID();
        }
    }
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
                  "PSU dual - %s##inst%d", inst->driver_id, inst->id);

    if (s->first_frame) {
        ImGui::SetNextWindowSize(ImVec2(1100, 760), ImGuiCond_Once);
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

    /* Reserve space at the bottom for the keypad - otherwise the
     * channel BeginChild(.., (0,0), ..) blocks fill the whole window
     * and the keypad gets pushed off-screen. */
    const float kp_reserved = 320.0f;
    float table_h = ImGui::GetContentRegionAvail().y - kp_reserved;
    if (table_h < 220.0f) table_h = 220.0f;

    if (ImGui::BeginTable("dual", 2, ImGuiTableFlags_BordersInnerV,
                           ImVec2(0, table_h))) {
        ImGui::TableSetupColumn("##c1", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("##c2", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); draw_channel(inst, 0, &s->ch[0]);
        ImGui::TableSetColumnIndex(1); draw_channel(inst, 1, &s->ch[1]);
        ImGui::EndTable();
    }

    ImGui::Spacing();
    draw_keypad(s, inst);

    ImGui::End();
}
