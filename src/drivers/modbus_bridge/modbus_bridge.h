/**
 * Modbus-bridge driver - talks to the existing ESP32 firmware that fronts
 * Riden / DPS-style Modbus PSUs over a text protocol on USB serial.
 *
 * The factory exposed below is registered with the global driver registry;
 * views never include this header directly.
 */

#ifndef DRIVERS_MODBUS_BRIDGE_H
#define DRIVERS_MODBUS_BRIDGE_H

#include "psu_driver.h"

extern const psu_driver_factory_t modbus_bridge_factory;

#endif
