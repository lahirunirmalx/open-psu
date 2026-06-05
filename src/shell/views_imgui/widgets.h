/**
 * Custom-rendered widgets used by the bench-instrument views. All draw
 * via ImDrawList against the current ImGui window, so they live inside
 * regular ImGui windows / BeginChild blocks alongside ordinary widgets.
 */

#pragma once

#include "imgui.h"

#include <cstddef>

/* ---- VFD-style dot-matrix digits --------------------------------------
 *
 * 5x7 pixel font, glowing green-on-black, matches the look of the
 * legacy/main.c VFD digits.
 *
 * Returns the width (px) used by the rendered string so callers can lay
 * out subsequent text after it.
 */
float vfd_string(ImDrawList *dl, ImVec2 pos, const char *str,
                 float dot_radius, ImU32 on_col, bool draw_off_pixels);

/* Convenience: same as above but at fixed dot radius 2 — what most
 * legacy callers used. */
float vfd_digits(ImDrawList *dl, ImVec2 pos, const char *str, ImU32 on_col);

/* Approximate width of a vfd_string rendered at the given dot radius. */
float vfd_width(const char *str, float dot_radius);

/* ---- Bar meter --------------------------------------------------------
 *
 * Horizontal bar with a green→yellow→red gradient fill, tick marks,
 * value-vs-max label. value is auto-clamped to [0, max_val].
 */
void bar_meter(const char *label, float value, float max_val,
               const char *unit, float width, float height);

/* ---- Mini scope -------------------------------------------------------
 *
 * Single-line trace with auto-scaling. samples is a ring buffer head
 * pointer; pass count = how many of the last samples to draw, capacity =
 * total ring size. head_idx = index of the OLDEST sample within `samples`.
 */
void mini_scope(const char *label, const float *samples,
                size_t head_idx, size_t count, size_t capacity,
                ImU32 line_col, float width, float height);
