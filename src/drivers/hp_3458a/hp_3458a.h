/**
 * HP 3458A — the 8½-digit GPIB reference DMM. Native HP command set
 * (not SCPI), so this driver doesn't ride the scpi_dmm profile table.
 *
 * The 3458A has no native USB; reach it via a Prologix GPIB-USB-HPIB
 * adapter:  --port=prologix:/dev/ttyUSB0:<gpib-addr>
 */

#ifndef DRIVERS_HP_3458A_H
#define DRIVERS_HP_3458A_H

#include "dmm_driver.h"

/* Shared driver core: HP DCL DMMs that speak DCV/ACV/OHM/OHMF/DCI/ACI/FREQ/PER
 * and trigger via TARM SGL. */
extern const dmm_driver_factory_t hp_3458a_factory;  /* 8½-digit reference */
extern const dmm_driver_factory_t hp_3457a_factory;  /* 6½-digit predecessor */

#endif
