/**
 * SCPI PSU driver - one implementation parameterised by a per-model profile.
 *
 * Compiled-in models export their factory below. The driver speaks to the
 * instrument through scpi_t, so any model works over either direct USB
 * serial (port="serial:/dev/ttyUSB0") or a Prologix GPIB-USB controller
 * (port="prologix:/dev/ttyUSB0:<gpib-addr>") - see transport/scpi.h for the
 * full port-spec grammar.
 */

#ifndef DRIVERS_SCPI_PSU_H
#define DRIVERS_SCPI_PSU_H

#include "psu_driver.h"

/* Siglent SPD-series (SPD3303C/X/etc.) - 2-channel + fixed 5V (we expose 2ch). */
extern const psu_driver_factory_t siglent_spd_factory;

/* Keysight / Agilent / HP E3631A - triple output (+6V/+25V/-25V).
 * Exposed as 3 channels. */
extern const psu_driver_factory_t keysight_e3631a_factory;

/* Keysight / Agilent / HP E3633A - single output (8V/20A or 20V/10A). */
extern const psu_driver_factory_t keysight_e3633a_factory;

/* Keysight / Agilent / HP E3634A - single output (8V/7A or 25V/4A). */
extern const psu_driver_factory_t keysight_e3634a_factory;

/* Keysight / Agilent / HP E3645A - single output (8V/5A or 20V/2.2A). */
extern const psu_driver_factory_t keysight_e3645a_factory;

/* Rigol DP800 family. */
extern const psu_driver_factory_t rigol_dp832_factory;
extern const psu_driver_factory_t rigol_dp832a_factory;
extern const psu_driver_factory_t rigol_dp811_factory;
extern const psu_driver_factory_t rigol_dp711_factory;

/* Rohde & Schwarz HMP / NGE families. */
extern const psu_driver_factory_t rs_hmp4040_factory;
extern const psu_driver_factory_t rs_hmp4030_factory;
extern const psu_driver_factory_t rs_hmp2030_factory;
extern const psu_driver_factory_t rs_nge103b_factory;

/* Keithley / Tektronix triple-channel supplies. */
extern const psu_driver_factory_t keithley_2230g_factory;
extern const psu_driver_factory_t keithley_2231a_factory;

/* HP / Agilent legacy single-output (GPIB-friendly). */
extern const psu_driver_factory_t hp_6632a_factory;
extern const psu_driver_factory_t hp_6633a_factory;
extern const psu_driver_factory_t hp_6634a_factory;

#endif
