/**
 * Korad KA-series PSU driver - and the many Chinese clones that speak the
 * same wire protocol (TENMA 72-25xx, Velleman LABPS-3005, Hanmatek HM-3xxP,
 * RND 320-KAxxxxP, Stamos, Korad-USB-style branded units).
 *
 * Single output channel by default; dual variants (KA3305P, …) can be added
 * later as separate factories on top of the same wire layer.
 */

#ifndef DRIVERS_KORAD_H
#define DRIVERS_KORAD_H

#include "psu_driver.h"

extern const psu_driver_factory_t korad_ka_factory;

#endif
