/**
 * Profile-driven SCPI DMM driver - one implementation, many models.
 *
 * Like scpi_psu, the wire transport is selected by the port-spec string the
 * factory receives, so every model below works over either USB-serial or a
 * Prologix GPIB-USB-HPIB adapter:
 *
 *   --port=serial:/dev/ttyUSB0
 *   --port=prologix:/dev/ttyUSB0:<gpib-addr>
 *
 * Adding another SCPI DMM is one scpi_dmm_profile_t entry + a thin factory
 * symbol + one line in the driver registry.
 */

#ifndef DRIVERS_SCPI_DMM_H
#define DRIVERS_SCPI_DMM_H

#include "dmm_driver.h"

/* Keysight/Agilent/HP - classic 34401A and its Truevolt successors. */
extern const dmm_driver_factory_t keysight_34401a_factory;   /* 6½ digit */
extern const dmm_driver_factory_t keysight_34461a_factory;   /* 6½ digit Truevolt */
extern const dmm_driver_factory_t keysight_34465a_factory;   /* 6½ digit Truevolt mid */
extern const dmm_driver_factory_t keysight_34470a_factory;   /* 7½ digit Truevolt top */

/* Fluke - 884x family is 34401A-compatible SCPI by default. */
extern const dmm_driver_factory_t fluke_8845a_factory;       /* 6½ digit */
extern const dmm_driver_factory_t fluke_8846a_factory;       /* 6½ digit, +4W ohms */

/* Keithley / Tektronix. */
extern const dmm_driver_factory_t keithley_2000_factory;     /* legacy 6½ digit, GPIB-class */
extern const dmm_driver_factory_t keithley_dmm6500_factory;  /* 6½ digit Touch DMM (SCPI mode) */

#endif
