/**
 * SCPI client — thread-safe send/query against a SCPI-speaking instrument.
 *
 * The wire transport is selected by the port spec string passed to
 * scpi_open():
 *
 *   "serial:/dev/ttyUSB0"            direct SCPI-over-serial
 *   "serial:/dev/ttyUSB0:9600"       explicit baud override
 *   "prologix:/dev/ttyUSB0:5"        Prologix GPIB-USB adapter, GPIB addr 5
 *   "prologix:/dev/ttyUSB0:5:9600"   with explicit Prologix-port baud
 *   "usbtmc:/dev/usbtmc0"            Linux USB-TMC kernel driver
 *   "vxi11:192.168.1.10"             VXI-11 LAN (default device "inst0")
 *   "vxi11:scope.local:gpib0,5"      VXI-11, custom device name
 *   "hislip:192.168.1.10"            HiSLIP LAN (port 4880, sub-addr "hislip0")
 *   "hislip:192.168.1.10:4880:hislip0"
 *                                    fully-specified HiSLIP
 *   "<naked path>"                   shorthand for "serial:<path>"
 *
 * Drivers (e.g. Siglent SPD) talk only to scpi_t — they don't know whether
 * they're going over USB-serial directly or through a GPIB controller.
 */

#ifndef TRANSPORT_SCPI_H
#define TRANSPORT_SCPI_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct scpi scpi_t;

struct scpi {
    void           *state;          /* transport-private */
    pthread_mutex_t lock;

    /* Vtable filled by the chosen transport during scpi_open(). */
    void (*close_impl)(scpi_t *);
    bool (*send_impl) (scpi_t *, const char *cmd);
    bool (*recv_impl) (scpi_t *, char *out, size_t outlen, int timeout_ms);
};

/**
 * Open a SCPI connection described by port_spec. default_baud is used when
 * the spec doesn't carry an explicit baud rate.
 *
 * Returns NULL on failure.
 */
scpi_t *scpi_open(const char *port_spec, int default_baud);

void scpi_close(scpi_t *s);

/* Send a command line (e.g. "OUTP CH1,ON"). No '?' expected. Thread-safe. */
bool scpi_send(scpi_t *s, const char *cmd);

/*
 * Send a query (e.g. "MEAS:VOLT? CH1") and capture the response line.
 * Returns false on transport error or timeout. Thread-safe.
 */
bool scpi_query(scpi_t *s, const char *cmd, char *out, size_t outlen,
                int timeout_ms);

/* ---- factory hooks (called by scpi_open) — not for direct use --- */

/* Open a direct-serial SCPI transport. */
scpi_t *scpi_serial_open(const char *device, int baud);

/* Open a Prologix-GPIB-over-USB SCPI transport on the given serial device,
 * talking to the instrument at GPIB primary address `gpib_addr`. */
scpi_t *scpi_prologix_open(const char *device, int baud, int gpib_addr);

/* Open a USB-TMC transport (Linux: /dev/usbtmc*). Returns NULL on Windows
 * for now — the host-side TMC framing isn't implemented there. */
scpi_t *scpi_usbtmc_open(const char *device);

/* Open a VXI-11 LAN transport. host = IP or hostname; device_name =
 * VXI-11 logical device (typically "inst0"; pass NULL for the default). */
scpi_t *scpi_vxi11_open(const char *host, const char *device_name);

/* Open a HiSLIP LAN transport. port defaults to 4880 if <= 0;
 * sub_address defaults to "hislip0" if NULL/empty. */
scpi_t *scpi_hislip_open(const char *host, int port, const char *sub_address);

#endif
