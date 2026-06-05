/**
 * PSU full - single channel, ImGui port.
 *
 * Bench-instrument layout for one channel:
 *   - Dot-matrix VFD readout (V, A, W) via shared widgets
 *   - Big colored numeric setpoint inputs
 *   - OUTPUT toggle + status pill
 *   - V and A bar meters
 *   - Mini auto-scaling scope of recent (V, A) samples
 *
 * The keypad is replaced by direct ImGui::InputFloat fields - ImGui's
 * keyboard input handling is good enough that a phone-style keypad
 * isn't needed.
 */

#include "views_imgui.h"
#include "widgets.h"

#include "imgui.h"

#include <cstdio>
#include <cstdlib>
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

    /* On-screen keypad. kp_target: 0 = Set V, 1 = Set A.
     * kp_buf is what the user has typed so far (always parsable as float). */
    char    kp_buf[16] = "";
    int     kp_target  = 0;
    bool    kp_open    = true;
};

void push_sample(State *s, float v, float a) {
    s->trace_v[s->head] = v;
    s->trace_a[s->head] = a;
    s->head = (s->head + 1) % TRACE_CAP;
    if (s->count < TRACE_CAP) s->count++;
}

void kp_append(State *s, const char *piece) {
    size_t cur = std::strlen(s->kp_buf);
    /* Block second '.', leading 0 swallow, etc. - keep it simple. */
    if (*piece == '.' && std::strchr(s->kp_buf, '.')) return;
    if (cur + std::strlen(piece) + 1 >= sizeof(s->kp_buf)) return;
    std::strcat(s->kp_buf, piece);
}

void kp_backspace(State *s) {
    size_t n = std::strlen(s->kp_buf);
    if (n > 0) s->kp_buf[n - 1] = '\0';
}

void kp_toggle_sign(State *s) {
    if (s->kp_buf[0] == '-') {
        std::memmove(s->kp_buf, s->kp_buf + 1, std::strlen(s->kp_buf));
    } else if (std::strlen(s->kp_buf) + 1 < sizeof(s->kp_buf)) {
        std::memmove(s->kp_buf + 1, s->kp_buf, std::strlen(s->kp_buf) + 1);
        s->kp_buf[0] = '-';
    }
}

void kp_apply(State *s, instance_t *inst) {
    if (!s->kp_buf[0]) return;
    float val = (float)std::atof(s->kp_buf);
    float vmax = inst->psu_drv->caps.v_max;
    float imax = inst->psu_drv->caps.i_max;
    if (s->kp_target == 0) {
        if (val < 0) val = 0;
        if (val > vmax) val = vmax;
        s->set_v = val;
        inst->psu_drv->set_voltage(inst->psu_drv, 1, val);
        s->set_dirty_v = false;
    } else {
        if (val < 0) val = 0;
        if (val > imax) val = imax;
        s->set_a = val;
        inst->psu_drv->set_current(inst->psu_drv, 1, val);
        s->set_dirty_a = false;
    }
    s->kp_buf[0] = '\0';
}

void draw_keypad(State *s, instance_t *inst) {
    if (!ImGui::CollapsingHeader("Keypad", &s->kp_open,
                                 ImGuiTreeNodeFlags_DefaultOpen))
        return;

    /* Target selector. */
    ImGui::Text("Target:");
    ImGui::SameLine();
    if (ImGui::RadioButton("Set V", s->kp_target == 0)) {
        s->kp_target = 0;
        std::snprintf(s->kp_buf, sizeof(s->kp_buf), "%.3f", s->set_v);
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Set A", s->kp_target == 1)) {
        s->kp_target = 1;
        std::snprintf(s->kp_buf, sizeof(s->kp_buf), "%.3f", s->set_a);
    }

    /* Live display (also editable). */
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##kp_buf", s->kp_buf, sizeof(s->kp_buf),
                     ImGuiInputTextFlags_CharsDecimal);

    /* 4x4 button grid. */
    const float bw = (ImGui::GetContentRegionAvail().x - 6 * 3) / 4.0f;
    const float bh = 30.0f;
    struct Btn { const char *lbl; int kind; const char *arg; };
    static const Btn rows[4][4] = {
        {{"7", 0, "7"}, {"8", 0, "8"}, {"9", 0, "9"}, {"BS", 1, nullptr}},
        {{"4", 0, "4"}, {"5", 0, "5"}, {"6", 0, "6"}, {"CLR", 2, nullptr}},
        {{"1", 0, "1"}, {"2", 0, "2"}, {"3", 0, "3"}, {"+/-", 3, nullptr}},
        {{"0", 0, "0"}, {".", 0, "."}, {"00", 0, "00"}, {"ENTER", 4, nullptr}},
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
                    case 1: kp_backspace(s);     break;
                    case 2: s->kp_buf[0] = '\0'; break;
                    case 3: kp_toggle_sign(s);   break;
                    case 4: kp_apply(s, inst);   break;
                }
            }
            if (is_enter) ImGui::PopStyleColor();
            ImGui::PopID();
        }
    }
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
                  "PSU full - %s##inst%d", inst->driver_id, inst->id);

    if (s->first_frame) {
        ImGui::SetNextWindowSize(ImVec2(640, 780), ImGuiCond_Once);
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

    /* ---- VFD readout (V, A, W) - vertically centred per row ---- */
    {
        float dot_r  = 3.0f;
        float vfd_h  = vfd_height(dot_r);
        float row_h  = vfd_h + 14.0f;                 /* row padding */
        float pad_y  = 10.0f;
        float box_h  = pad_y * 2 + row_h * 3;
        float box_w  = ImGui::GetContentRegionAvail().x;
        ImVec2 box_p = ImGui::GetCursorScreenPos();

        dl->AddRectFilled(box_p, ImVec2(box_p.x + box_w, box_p.y + box_h),
                          IM_COL32(8, 18, 12, 255), 4.0f);
        dl->AddRect(box_p, ImVec2(box_p.x + box_w, box_p.y + box_h),
                    IM_COL32(60, 60, 65, 255), 4.0f);

        ImU32 vfd_on  = s->snap.out_on ? IM_COL32(0, 255, 120, 255)
                                       : IM_COL32(0, 80, 50, 255);
        ImU32 label_c = IM_COL32(200, 160, 60, 255);

        float text_h = ImGui::GetFontSize();
        struct Row { const char *label; const char *unit; const char *fmt; float val; };
        const Row rows_data[3] = {
            {"VOLTAGE", "V", "% 7.3f", s->snap.out_v},
            {"CURRENT", "A", "% 7.3f", s->snap.out_a},
            {"POWER",   "W", "% 7.2f", s->snap.out_p},
        };
        char buf[24];
        float num_x = box_p.x + 100.0f;
        float left  = box_p.x + 14.0f;
        float unit_x = box_p.x + box_w - 24.0f;

        for (int i = 0; i < 3; i++) {
            float row_top = box_p.y + pad_y + i * row_h;
            float row_mid = row_top + row_h * 0.5f;
            float vfd_top = row_mid - vfd_h * 0.5f - dot_r;  /* top edge of topmost dot */
            float text_y = row_mid - text_h * 0.5f;

            dl->AddText(ImVec2(left, text_y), label_c, rows_data[i].label);
            std::snprintf(buf, sizeof(buf), rows_data[i].fmt, rows_data[i].val);
            vfd_string(dl, ImVec2(num_x, vfd_top), buf, dot_r, vfd_on, true);
            dl->AddText(ImVec2(unit_x, text_y), vfd_on, rows_data[i].unit);
        }

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

    /* ---- On-screen keypad ---- */
    ImGui::Spacing();
    draw_keypad(s, inst);

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
