/**
 * DMM full - ImGui port.
 *
 * Big primary reading + mode buttons + range cycle + rate selector +
 * mini auto-scaling scope of recent samples.
 */

#include "views_imgui.h"
#include "widgets.h"

#include "imgui.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

#define TRACE_CAP 256

struct State {
    dmm_reading_t r{};
    float         trace[TRACE_CAP];
    size_t        head = 0;
    size_t        count = 0;
    /* Current range cycle index per mode - we cycle through a tiny table. */
    int           range_idx = 0;
    bool          first_frame = true;
};

const struct {
    dmm_mode_t  mode;
    const char *label;
} kModeButtons[] = {
    { DMM_MODE_DC_VOLTS, "DCV"   },
    { DMM_MODE_AC_VOLTS, "ACV"   },
    { DMM_MODE_DC_AMPS,  "DCI"   },
    { DMM_MODE_AC_AMPS,  "ACI"   },
    { DMM_MODE_OHMS_2W,  "Ω"     },
    { DMM_MODE_OHMS_4W,  "Ω4W"   },
    { DMM_MODE_CAPACITANCE, "CAP" },
    { DMM_MODE_FREQUENCY,"FREQ"  },
    { DMM_MODE_PERIOD,   "PER"   },
    { DMM_MODE_DIODE,    "DIOD"  },
    { DMM_MODE_CONTINUITY,"CONT" },
    { DMM_MODE_TEMPERATURE,"TEMP"},
};

const float kRangeTableV[] = { 0, 0.1f, 1, 10, 100, 1000 };
const float kRangeTableA[] = { 0, 0.01f, 0.1f, 1, 10 };
const float kRangeTableOHM[]= { 0, 100, 1e3, 1e4, 1e5, 1e6, 1e7 };

void push_sample(State *s, float v) {
    s->trace[s->head] = v;
    s->head = (s->head + 1) % TRACE_CAP;
    if (s->count < TRACE_CAP) s->count++;
}

}  // namespace

void *dmm_full_state_new(void)        { return new State(); }
void  dmm_full_state_free(void *p)    { delete static_cast<State*>(p); }

void  dmm_full_draw(instance_t *inst) {
    if (!inst || !inst->dmm_drv || !inst->view_state) return;
    State *s = static_cast<State*>(inst->view_state);

    inst->dmm_drv->read(inst->dmm_drv, &s->r);
    if (s->r.valid && !s->r.overload) push_sample(s, s->r.value);

    char title[96];
    std::snprintf(title, sizeof(title),
                  "DMM - %s##inst%d", inst->driver_id, inst->id);

    if (s->first_frame) {
        ImGui::SetNextWindowSize(ImVec2(720, 420), ImGuiCond_Once);
        s->first_frame = false;
    }
    if (!ImGui::Begin(title, &inst->open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    bool connected = inst->dmm_drv->is_connected(inst->dmm_drv);
    ImGui::TextDisabled("%s · %s",
                        inst->port, dmm_mode_label(s->r.mode));
    ImGui::SameLine(ImGui::GetWindowWidth() - 28);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 hp = ImGui::GetCursorScreenPos();
    dl->AddCircleFilled(ImVec2(hp.x + 6, hp.y + 8), 5,
                        connected ? IM_COL32(50, 220, 100, 255)
                                  : IM_COL32(240, 80, 80, 255));
    ImGui::NewLine();
    ImGui::Separator();

    /* Big primary reading + unit. */
    ImGui::SetWindowFontScale(3.6f);
    ImVec4 col = s->r.valid ? ImVec4(0.0f, 1.0f, 0.55f, 1)
                            : ImVec4(0.45f, 0.45f, 0.50f, 1);
    char buf[32];
    if (s->r.overload) {
        std::snprintf(buf, sizeof(buf), "OL");
        col = ImVec4(1, 0.27f, 0.27f, 1);
    } else {
        std::snprintf(buf, sizeof(buf), "% .6f", s->r.value);
    }
    ImGui::TextColored(col, "%s", buf);
    ImGui::SameLine();
    ImGui::SetWindowFontScale(1.5f);
    ImGui::TextColored(col, "%s", dmm_mode_unit(s->r.mode));
    ImGui::SetWindowFontScale(1.0f);

    ImGui::Spacing();
    ImGui::Separator();

    /* Mode buttons - wrap to fit. */
    ImGui::TextDisabled("MODE");
    int per_row = 6;
    int n = (int)(sizeof(kModeButtons) / sizeof(kModeButtons[0]));
    for (int i = 0; i < n; i++) {
        bool supported =
            inst->dmm_drv->caps.supports_mode[kModeButtons[i].mode];
        if (!supported) ImGui::BeginDisabled();
        bool cur = (s->r.mode == kModeButtons[i].mode);
        if (cur) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0.50f, 0.66f, 1));
        if (ImGui::Button(kModeButtons[i].label, ImVec2(60, 0))) {
            if (supported) {
                inst->dmm_drv->set_mode(inst->dmm_drv, kModeButtons[i].mode);
                s->head = 0; s->count = 0;
            }
        }
        if (cur) ImGui::PopStyleColor();
        if (!supported) ImGui::EndDisabled();
        if ((i + 1) % per_row != 0) ImGui::SameLine();
    }
    ImGui::NewLine();

    /* Range cycle (mode-dependent). */
    ImGui::TextDisabled("RANGE");
    if (ImGui::Button("AUTO", ImVec2(70, 0))) {
        s->range_idx = 0;
        inst->dmm_drv->set_range(inst->dmm_drv, 0);
    }
    ImGui::SameLine();
    if (ImGui::Button("RANGE >", ImVec2(90, 0))) {
        const float *tbl = kRangeTableV; int sz = (int)(sizeof(kRangeTableV)/sizeof(kRangeTableV[0]));
        if (s->r.mode == DMM_MODE_DC_AMPS || s->r.mode == DMM_MODE_AC_AMPS) {
            tbl = kRangeTableA; sz = (int)(sizeof(kRangeTableA)/sizeof(kRangeTableA[0]));
        } else if (s->r.mode == DMM_MODE_OHMS_2W || s->r.mode == DMM_MODE_OHMS_4W) {
            tbl = kRangeTableOHM; sz = (int)(sizeof(kRangeTableOHM)/sizeof(kRangeTableOHM[0]));
        }
        s->range_idx = (s->range_idx + 1) % sz;
        inst->dmm_drv->set_range(inst->dmm_drv, tbl[s->range_idx]);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(cur %.4g)", s->r.range);

    /* Rate selector. */
    ImGui::Spacing();
    ImGui::TextDisabled("RATE");
    if (ImGui::Button("SLOW")) inst->dmm_drv->set_rate(inst->dmm_drv, DMM_RATE_SLOW);
    ImGui::SameLine();
    if (ImGui::Button("MED"))  inst->dmm_drv->set_rate(inst->dmm_drv, DMM_RATE_MEDIUM);
    ImGui::SameLine();
    if (ImGui::Button("FAST")) inst->dmm_drv->set_rate(inst->dmm_drv, DMM_RATE_FAST);

    ImGui::Spacing();
    ImGui::Separator();

    /* Auto-scaling trace of recent samples. */
    size_t oldest = s->count < TRACE_CAP
                    ? 0
                    : (s->head + TRACE_CAP - s->count) % TRACE_CAP;
    float w = ImGui::GetContentRegionAvail().x;
    mini_scope("recent samples", s->trace, oldest, s->count, TRACE_CAP,
               IM_COL32(80, 255, 120, 255),
               w, 110.0f);

    ImGui::End();
}
