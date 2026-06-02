/**
 * HP / Agilent 6620-series "System DC Sources" — multi-output GPIB PSUs.
 *
 * Native HP DCL command set (predates SCPI on this family):
 *     VSET <ch>,<v>      ISET <ch>,<a>      OUT <ch>,<0|1>
 *     VOUT? <ch>         IOUT? <ch>         VSET? <ch>     ISET? <ch>
 *     STS? <ch>          ID?
 *
 * Reach the instrument via Prologix GPIB-USB-HPIB:
 *     --port=prologix:/dev/ttyUSB0:<gpib-addr>
 *
 * One driver, five factories — each is a different physical model in the
 * 6620-series family with its own channel count and per-channel ranges.
 */

#ifndef DRIVERS_HP_6620_FAMILY_H
#define DRIVERS_HP_6620_FAMILY_H

#include "psu_driver.h"

extern const psu_driver_factory_t hp_6622a_factory;   /* 2-output */
extern const psu_driver_factory_t hp_6623a_factory;   /* 3-output */
extern const psu_driver_factory_t hp_6624a_factory;   /* 4-output */
extern const psu_driver_factory_t hp_6625a_factory;   /* 2-output, high-power */
extern const psu_driver_factory_t hp_6627a_factory;   /* 4-output */

#endif
