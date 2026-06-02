/**
 * HP 3478A — 5½-digit GPIB DMM, native HP F-command protocol.
 *
 * No SCPI, no *IDN?. Function and range are selected with single-letter
 * codes (F1..F7, R1..R7). Reach it via a Prologix GPIB-USB-HPIB adapter:
 *
 *     --port=prologix:/dev/ttyUSB0:<gpib-addr>
 */

#ifndef DRIVERS_HP_3478A_H
#define DRIVERS_HP_3478A_H

#include "dmm_driver.h"

extern const dmm_driver_factory_t hp_3478a_factory;

#endif
