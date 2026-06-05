/**
 * Per-view ImGui draw functions. Each view exposes:
 *
 *   void*   <name>_state_new(void);     allocate per-instance state struct
 *   void    <name>_state_free(void*);   destroy it
 *   void    <name>_draw(instance_t*);   draw one frame inside ImGui::Begin()
 *
 * The instance manager dispatches by view_id string.
 */

#pragma once

#include "../instance.h"

/* ---- PSU toolbar (single channel) -------------------------------------- */
void *psu_toolbar_single_state_new(void);
void  psu_toolbar_single_state_free(void *st);
void  psu_toolbar_single_draw(instance_t *inst);

/* ---- PSU toolbar (dual channel) ---------------------------------------- */
void *psu_toolbar_dual_state_new(void);
void  psu_toolbar_dual_state_free(void *st);
void  psu_toolbar_dual_draw(instance_t *inst);

/* ---- PSU full (single channel) ----------------------------------------- */
void *psu_full_single_state_new(void);
void  psu_full_single_state_free(void *st);
void  psu_full_single_draw(instance_t *inst);

/* ---- PSU full (dual channel) ------------------------------------------- */
void *psu_full_dual_state_new(void);
void  psu_full_dual_state_free(void *st);
void  psu_full_dual_draw(instance_t *inst);

/* ---- DMM toolbar ------------------------------------------------------- */
void *dmm_toolbar_state_new(void);
void  dmm_toolbar_state_free(void *st);
void  dmm_toolbar_draw(instance_t *inst);

/* ---- DMM full ---------------------------------------------------------- */
void *dmm_full_state_new(void);
void  dmm_full_state_free(void *st);
void  dmm_full_draw(instance_t *inst);
