/**
 * Serial port abstraction for Linux.
 * Thread-safe with non-blocking read support.
 */

#ifndef SERIAL_PORT_H
#define SERIAL_PORT_H

#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>

typedef struct serial_port serial_port_t;

/**
 * Open serial port.
 * @param device e.g. "/dev/ttyUSB0"
 * @param baud   e.g. 115200
 * @return handle or NULL on failure
 */
serial_port_t *serial_open(const char *device, int baud);

/**
 * Close serial port.
 */
void serial_close(serial_port_t *sp);

/**
 * Check if port is open and valid.
 */
bool serial_is_open(serial_port_t *sp);

/**
 * Get file descriptor for select/poll.
 */
int serial_get_fd(serial_port_t *sp);

/**
 * Send a line (appends \n). Thread-safe.
 * @return true on success
 */
bool serial_send_line(serial_port_t *sp, const char *line);

/**
 * Write raw bytes - no line terminator added. For wire protocols that
 * don't use line termination (e.g. Korad text protocol). Thread-safe.
 */
bool serial_write_bytes(serial_port_t *sp, const void *buf, size_t len);

/**
 * Read a line (non-blocking).
 * @param buf    output buffer
 * @param buflen buffer size
 * @param timeout_ms  timeout in ms (0 = non-blocking, -1 = blocking)
 * @return true if line received, false on timeout/error
 */
bool serial_read_line(serial_port_t *sp, char *buf, size_t buflen, int timeout_ms);

/**
 * Read up to `maxlen` raw bytes with an overall timeout. Returns
 * immediately when at least one byte arrives, or after `timeout_ms`
 * milliseconds of silence. Used by drivers whose wire protocol is not
 * line-terminated (e.g. Korad).
 *
 * @return number of bytes read (0 on timeout, -1 on error).
 */
int  serial_read_bytes(serial_port_t *sp, void *buf, size_t maxlen, int timeout_ms);

/**
 * Send command and read response (blocking with timeout).
 * @return true if response received
 */
bool serial_command(serial_port_t *sp, const char *cmd, char *resp, size_t resp_len, int timeout_ms);

/**
 * Flush input buffer.
 */
void serial_flush(serial_port_t *sp);

#endif /* SERIAL_PORT_H */
