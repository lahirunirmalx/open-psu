/**
 * Tiny cross-platform TCP helper used by the LAN-class SCPI transports
 * (VXI-11 and HiSLIP). Intentionally minimal: blocking connect with a
 * timeout, exact-length read, all-or-nothing write. No DNS-async, no
 * IPv6 (instruments addressed by IPv4 dotted-quad or hostname).
 */

#ifndef TRANSPORT_NET_IO_H
#define TRANSPORT_NET_IO_H

#include <stdbool.h>
#include <stddef.h>

/** Per-process winsock init on Windows. No-op on POSIX. Safe to call
 *  repeatedly. */
void net_global_init(void);

/** Connect to host:port, timing out after timeout_ms. Returns a socket
 *  handle (Linux: file descriptor; Windows: SOCKET cast to int) or -1
 *  on failure. */
int  net_tcp_connect(const char *host, int port, int timeout_ms);

/** Send exactly `len` bytes; loops until done or error. */
bool net_send_all(int sock, const void *buf, size_t len);

/** Read exactly `len` bytes; loops until satisfied. Returns false on
 *  EOF / error / timeout. */
bool net_recv_exact(int sock, void *buf, size_t len, int timeout_ms);

/** Read up to `maxlen` bytes; returns the number of bytes read (0 on
 *  timeout, -1 on error). */
int  net_recv_some(int sock, void *buf, size_t maxlen, int timeout_ms);

/** Close (and on Windows, closesocket()). */
void net_close(int sock);

#endif
