/**
 * widgets.cpp — VFD digits, bar meter, mini scope.
 *
 * Each helper reserves space with ImGui::Dummy() and paints via
 * ImDrawList. They sit inside any regular ImGui::Window / BeginChild.
 */

#include "widgets.h"

#include <cstdio>
#include <cstdint>
#include <cstring>

/* ---- 5x7 dot-matrix font (same patterns as legacy/main.c) -------------- */

static const uint8_t kDotMatrixFont[12][5] = {
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, /* 0 */
    {0x00, 0x42, 0x7F, 0x40, 0x00}, /* 1 */
    {0x42, 0x61, 0x51, 0x49, 0x46}, /* 2 */
    {0x21, 0x41, 0x45, 0x4B, 0x31}, /* 3 */
    {0x18, 0x14, 0x12, 0x7F, 0x10}, /* 4 */
    {0x27, 0x45, 0x45, 0x45, 0x39}, /* 5 */
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, /* 6 */
    {0x01, 0x71, 0x09, 0x05, 0x03}, /* 7 */
    {0x36, 0x49, 0x49, 0x49, 0x36}, /* 8 */
    {0x06, 0x49, 0x49, 0x29, 0x1E}, /* 9 */
    {0x00, 0x60, 0x60, 0x00, 0x00}, /* . (dot) */
    {0x08, 0x08, 0x08, 0x08, 0x08}, /* - (minus) */
};

static const float kDotGap   = 1.0f;
static const float kCharGap  = 5.0f;

static void vfd_dot(ImDrawList *dl, ImVec2 c, float r, ImU32 col, bool on) {
    if (!on) {
        /* Off pixel = barely-visible ghost. */
        ImU32 dim = (col & 0x00FFFFFFu) | (0x18u << 24);
        dl->AddCircleFilled(c, r, dim, 12);
        return;
    }
    /* Outer glow. */
    ImU32 glow = (col & 0x00FFFFFFu) | (0x40u << 24);
    dl->AddCircleFilled(c, r + 1.5f, glow, 16);
    /* Main pixel. */
    dl->AddCircleFilled(c, r, col, 16);
    /* Bright highlight. */
    dl->AddCircleFilled(ImVec2(c.x - r * 0.4f, c.y - r * 0.4f),
                        r * 0.4f, IM_COL32(220, 255, 230, 255), 12);
}

static float advance_for(int ch_idx, float dot_r) {
    float spacing = dot_r * 2 + kDotGap;
    if (ch_idx == 10) return spacing * 2 + kCharGap;   /* dot is narrower */
    return spacing * 5 + kCharGap;                     /* normal char width */
}

static void vfd_char(ImDrawList *dl, ImVec2 pos, int ch_idx,
                     float dot_r, ImU32 on_col, bool draw_off) {
    if (ch_idx < 0 || ch_idx > 11) return;
    const uint8_t *pat = kDotMatrixFont[ch_idx];
    float spacing = dot_r * 2 + kDotGap;
    for (int col = 0; col < 5; col++) {
        uint8_t coldata = pat[col];
        for (int row = 0; row < 7; row++) {
            bool on = (coldata >> row) & 1;
            ImVec2 c = ImVec2(pos.x + col * spacing + dot_r,
                              pos.y + row * spacing + dot_r);
            if (on)       vfd_dot(dl, c, dot_r, on_col, true);
            else if (draw_off) vfd_dot(dl, c, dot_r, on_col, false);
        }
    }
}

float vfd_string(ImDrawList *dl, ImVec2 pos, const char *str,
                 float dot_r, ImU32 on_col, bool draw_off) {
    if (!str) return 0;
    float x = pos.x;
    for (const char *p = str; *p; p++) {
        int idx = -1;
        if      (*p >= '0' && *p <= '9') idx = *p - '0';
        else if (*p == '.')              idx = 10;
        else if (*p == '-')              idx = 11;
        else if (*p == ' ')              { x += dot_r * 2 + kDotGap + kCharGap; continue; }
        if (idx < 0) continue;
        vfd_char(dl, ImVec2(x, pos.y), idx, dot_r, on_col, draw_off);
        x += advance_for(idx, dot_r);
    }
    return x - pos.x;
}

float vfd_digits(ImDrawList *dl, ImVec2 pos, const char *str, ImU32 on_col) {
    return vfd_string(dl, pos, str, 2.0f, on_col, true);
}

float vfd_width(const char *str, float dot_r) {
    if (!str) return 0;
    float w = 0;
    for (const char *p = str; *p; p++) {
        int idx = -1;
        if      (*p >= '0' && *p <= '9') idx = *p - '0';
        else if (*p == '.')              idx = 10;
        else if (*p == '-')              idx = 11;
        else if (*p == ' ')              { w += dot_r * 2 + kDotGap + kCharGap; continue; }
        if (idx < 0) continue;
        w += advance_for(idx, dot_r);
    }
    return w;
}

/* ---- bar meter -------------------------------------------------------- */

void bar_meter(const char *label, float value, float max_val,
               const char *unit, float width, float height) {
    if (max_val <= 0) max_val = 1.0f;
    float frac = value / max_val;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;

    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImVec2 sz(width, height);

    /* Background */
    dl->AddRectFilled(p0, ImVec2(p0.x + sz.x, p0.y + sz.y),
                      IM_COL32(35, 35, 38, 255), 3.0f);
    dl->AddRect(p0, ImVec2(p0.x + sz.x, p0.y + sz.y),
                IM_COL32(80, 80, 88, 255), 3.0f);

    /* Bar region */
    float bar_x  = p0.x + 8.0f;
    float bar_y  = p0.y + 22.0f;
    float bar_w  = sz.x - 16.0f;
    float bar_h  = sz.y - 32.0f;
    if (bar_h < 8.0f) bar_h = 8.0f;

    dl->AddRectFilled(ImVec2(bar_x, bar_y),
                      ImVec2(bar_x + bar_w, bar_y + bar_h),
                      IM_COL32(20, 20, 22, 255));
    float fill_w = bar_w * frac;
    for (int i = 0; i < (int)fill_w; i++) {
        float f = (float)i / bar_w;
        int red, green;
        if (f < 0.5f) { red = (int)(f * 2 * 200); green = 200; }
        else          { red = 200; green = (int)((1.0f - (f - 0.5f) * 2) * 200); }
        dl->AddLine(ImVec2(bar_x + i, bar_y + 1),
                    ImVec2(bar_x + i, bar_y + bar_h - 1),
                    IM_COL32(red, green, 50, 255));
    }
    dl->AddRect(ImVec2(bar_x, bar_y),
                ImVec2(bar_x + bar_w, bar_y + bar_h),
                IM_COL32(80, 80, 85, 255));

    /* Label + value */
    dl->AddText(ImVec2(p0.x + 8, p0.y + 4),
                IM_COL32(160, 160, 168, 255), label);
    char vbuf[24];
    std::snprintf(vbuf, sizeof(vbuf), "%.2f %s", value, unit ? unit : "");
    ImVec2 tsz = ImGui::CalcTextSize(vbuf);
    dl->AddText(ImVec2(p0.x + sz.x - 8 - tsz.x, p0.y + 4),
                IM_COL32(200, 200, 205, 255), vbuf);

    ImGui::Dummy(sz);
}

/* ---- mini scope ------------------------------------------------------- */

void mini_scope(const char *label, const float *samples,
                size_t head_idx, size_t count, size_t capacity,
                ImU32 line_col, float width, float height) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImVec2 sz(width, height);

    /* Background */
    dl->AddRectFilled(p0, ImVec2(p0.x + sz.x, p0.y + sz.y),
                      IM_COL32(10, 20, 15, 255), 3.0f);
    dl->AddRect(p0, ImVec2(p0.x + sz.x, p0.y + sz.y),
                IM_COL32(60, 60, 65, 255), 3.0f);

    if (label) {
        dl->AddText(ImVec2(p0.x + 6, p0.y + 4),
                    IM_COL32(120, 120, 128, 255), label);
    }

    if (count < 2 || !samples || capacity == 0) {
        dl->AddText(ImVec2(p0.x + sz.x / 2 - 28, p0.y + sz.y / 2 - 6),
                    IM_COL32(120, 120, 128, 255), "NO DATA");
        ImGui::Dummy(sz);
        return;
    }

    /* Auto-scale. */
    float mn = samples[0], mx = samples[0];
    for (size_t i = 0; i < count; i++) {
        size_t idx = (head_idx + i) % capacity;
        float v = samples[idx];
        if (v < mn) mn = v;
        if (v > mx) mx = v;
    }
    float range = mx - mn;
    if (range < 1e-4f) { range = 1.0f; mn -= 0.5f; }

    /* Grid. */
    for (int i = 1; i < 4; i++) {
        float gy = p0.y + sz.y * i / 4.0f;
        dl->AddLine(ImVec2(p0.x, gy), ImVec2(p0.x + sz.x, gy),
                    IM_COL32(30, 50, 35, 255));
    }

    /* Polyline. Build into a thread_local static buffer to avoid heap. */
    static ImVec2 pts[1024];
    size_t draw_count = count < 1024 ? count : 1024;
    for (size_t i = 0; i < draw_count; i++) {
        size_t idx = (head_idx + i) % capacity;
        float v = samples[idx];
        float x = p0.x + (float)i * sz.x / (float)(draw_count - 1);
        float y = p0.y + sz.y - ((v - mn) / range) * sz.y;
        if (y < p0.y) y = p0.y;
        if (y > p0.y + sz.y - 1) y = p0.y + sz.y - 1;
        pts[i] = ImVec2(x, y);
    }
    dl->AddPolyline(pts, (int)draw_count, line_col, ImDrawFlags_None, 1.5f);

    /* Range labels. */
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%.2f", mx);
    dl->AddText(ImVec2(p0.x + 4, p0.y + 16), line_col, buf);
    std::snprintf(buf, sizeof(buf), "%.2f", mn);
    dl->AddText(ImVec2(p0.x + 4, p0.y + sz.y - 14), line_col, buf);

    ImGui::Dummy(sz);
}
