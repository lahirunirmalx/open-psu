/**
 * Driver registry - single point that knows every compiled-in driver
 * factory (both PSU and DMM). Adding a new model is a one-line change to
 * registry.c.
 */

#ifndef DRIVERS_REGISTRY_H
#define DRIVERS_REGISTRY_H

#include "psu_driver.h"
#include "dmm_driver.h"
#include <stddef.h>

/* ---- PSU side ---- */

const psu_driver_factory_t *const *psu_drivers_list(size_t *count);
const psu_driver_factory_t        *psu_drivers_find(const char *id);

/* ---- DMM side ---- */

const dmm_driver_factory_t *const *dmm_drivers_list(size_t *count);
const dmm_driver_factory_t        *dmm_drivers_find(const char *id);

#endif
