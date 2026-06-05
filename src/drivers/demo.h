/**
 * Synthetic "demo" driver - pretends to be a 2-channel PSU and produces
 * plausible-looking moving values. Useful when no real hardware is attached
 * and replaces the per-view demo paths that lived in the old code.
 */

#ifndef DRIVERS_DEMO_H
#define DRIVERS_DEMO_H

#include "psu_driver.h"

extern const psu_driver_factory_t demo_factory;

#endif
