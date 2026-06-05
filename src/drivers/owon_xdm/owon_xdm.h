/**
 * OWON XDM bench-DMM driver (XDM1041, XDM1241, XDM2041 - and likely
 * XDM3041/3051/3000-series for what works via the documented SCPI commands).
 *
 * Talks SCPI over USB-serial. Tested commands match the rusty_meter
 * (markusdd/rusty_meter) implementation: MEAS?, FUNC?, CONF:..., RATE,
 * SYST:REM/LOC.
 */

#ifndef DRIVERS_OWON_XDM_H
#define DRIVERS_OWON_XDM_H

#include "dmm_driver.h"

extern const dmm_driver_factory_t owon_xdm_factory;

#endif
