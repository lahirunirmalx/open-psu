/**
 * DMM toolbar — ImGui port.
 *
 * One compact window: large primary reading with overload handling,
 * mode label, rate selector. No mode/range buttons here (those live in
 * dmm_full); the toolbar is meant to be a glance widget you tile in a
 * corner of the screen.
 */

#include "views_imgui.h"
#include "widgets.h"

#include "imgui.h"

#include <cmath>
#include <cstdio>

namespace {

struct State {
    dmm_reading_t r{};
    bool          first_frame = true;
};

const char *rate_str(dmm_rate_t r) {
    switch (r) {
        case DMM_RATE_SLOW:   return "SLOW";
        case DMM_RATE_FAST:   return "FAST";
        case DMM_RATE_MEDIUM:
        default:              return "MED";
    }
}

}  // namespace

void *dmm_toolbar_state_new(void)        { return new State(); }
void  dmm_toolbar_state_free(void *p)    { delete static_cast<State*>(p); }

void  dmm_toolbar_draw(instance_t *inst) {
    if (!inst || !inst->dmm_drv || !inst->view_state) return;
    State *s = static_cast<State*>(inst->view_state);

    inst->dmm_drv->read(inst->dmm_drv, &s->r);

    char title[96];
    std::snprintf(title, sizeof(title),
                  "DMM — %s##inst%d", inst->driver_id, inst->id);

    if (s->first_frame) {
        ImGui::SetNextWindowSize(ImVec2(540, 140), ImGuiCond_Once);
        s->first_frame = false;
    }
    if (!ImGui::Begin(title, &inst->open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    /* Header */
    bool connected = inst->dmm_drv->is_connected(inst->dmm_drv);
    ImGui::TextDisabled("%s · %s", inst->port, dmm_mode_label(s->r.mode));
    ImGui::SameLine(ImGui::GetWindowWidth() - 28);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddCircleFilled(ImVec2(p.x + 6, p.y + 8),
                        5,
                        connected ? IM_COL32(50, 220, 100, 255)
                                  : IM_COL32(240, 80, 80, 255));
    ImGui::NewLine();

    /* Big primary reading */
    ImGui::SetWindowFontScale(3.0f);
    ImVec4 col = s->r.valid ? ImVec4(0.0f, 1.0f, 0.55f, 1)
                            : ImVec4(0.45f, 0.45f, 0.50f, 1);
    char buf[32];
    if (s->r.overload) {
        std::snprintf(buf, sizeof(buf), "OL");
        col = ImVec4(1, 0.27f, 0.27f, 1);
    } else {
        std::snprintf(buf, sizeof(buf), "% .5f", s->r.value);
    }
    ImGui::TextColored(col, "%s", buf);
    ImGui::SameLine();
    ImGui::SetWindowFontScale(1.4f);
    ImGui::TextColored(col, "%s", dmm_mode_unit(s->r.mode));
    ImGui::SetWindowFontScale(1.0f);

    /* Rate selector along the bottom */
    ImGui::Spacing();
    dmm_rate_t cur = s->r.rate;
    if (ImGui::SmallButton("SLOW")) inst->dmm_drv->set_rate(inst->dmm_drv, DMM_RATE_SLOW);
    ImGui::SameLine();
    if (ImGui::SmallButton("MED"))  inst->dmm_drv->set_rate(inst->dmm_drv, DMM_RATE_MEDIUM);
    ImGui::SameLine();
    if (ImGui::SmallButton("FAST")) inst->dmm_drv->set_rate(inst->dmm_drv, DMM_RATE_FAST);
    ImGui::SameLine();
    ImGui::TextDisabled("rate: %s", rate_str(cur));

    ImGui::End();
}
