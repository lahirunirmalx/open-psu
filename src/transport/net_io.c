/**
 * net_io.c — POSIX socket calls on Linux/macOS, Winsock2 on Windows.
 * Compiled into the build regardless of platform; the Windows path
 * activates via _WIN32.
 */

#include "net_io.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef int socklen_t_compat;
  #define close_socket(s) closesocket((SOCKET)(s))
  #define socket_errno()  WSAGetLastError()
  #define SK_EAGAIN  WSAEWOULDBLOCK
  #define SK_EINPROG WSAEWOULDBLOCK     /* connect() in non-blocking returns WSAEWOULDBLOCK */
  #define SK_EINTR   WSAEINTR
#else
  #include <sys/socket.h>
  #include <sys/select.h>
  #include <sys/types.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <fcntl.h>
  #include <unistd.h>
  #define close_socket(s) close(s)
  #define socket_errno()  errno
  #define SK_EAGAIN  EAGAIN
  #define SK_EINPROG EINPROGRESS
  #define SK_EINTR   EINTR
#endif

void net_global_init(void) {
#ifdef _WIN32
    static bool done = false;
    if (done) return;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0) done = true;
#endif
}

static void set_nonblock(int sock, bool nb) {
#ifdef _WIN32
    u_long mode = nb ? 1 : 0;
    ioctlsocket((SOCKET)sock, FIONBIO, &mode);
#else
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0) return;
    fcntl(sock, F_SETFL, nb ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
#endif
}

int net_tcp_connect(const char *host, int port, int timeout_ms) {
    net_global_init();
    if (!host || port <= 0 || port > 65535) return -1;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port);

    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        fprintf(stderr, "net_io: cannot resolve %s\n", host);
        return -1;
    }

    int sock = (int)socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) { freeaddrinfo(res); return -1; }

    /* Disable Nagle for SCPI: latency matters more than throughput here. */
    int one = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));

    set_nonblock(sock, true);
    int rc = connect(sock, res->ai_addr, (socklen_t)res->ai_addrlen);
    int err = socket_errno();
    freeaddrinfo(res);

    if (rc != 0 && err != SK_EINPROG && err != SK_EAGAIN) {
        fprintf(stderr, "net_io: connect(%s:%d) failed (err=%d)\n", host, port, err);
        close_socket(sock);
        return -1;
    }

    /* Wait for writability up to timeout_ms; then re-check SO_ERROR. */
    fd_set ws;
    FD_ZERO(&ws);
    FD_SET(sock, &ws);
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    rc = select(sock + 1, NULL, &ws, NULL, &tv);
    if (rc <= 0) {
        fprintf(stderr, "net_io: connect(%s:%d) timed out\n", host, port);
        close_socket(sock);
        return -1;
    }
    int so_err = 0;
    socklen_t so_err_len = sizeof(so_err);
    getsockopt(sock, SOL_SOCKET, SO_ERROR, (char *)&so_err, &so_err_len);
    if (so_err != 0) {
        fprintf(stderr, "net_io: connect(%s:%d) refused (err=%d)\n", host, port, so_err);
        close_socket(sock);
        return -1;
    }

    set_nonblock(sock, false);
    return sock;
}

bool net_send_all(int sock, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    while (len > 0) {
        int n = (int)send(sock, p, (int)len, 0);
        if (n < 0) {
            int e = socket_errno();
            if (e == SK_EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        p += n; len -= (size_t)n;
    }
    return true;
}

static bool wait_readable(int sock, int timeout_ms) {
    fd_set rs;
    FD_ZERO(&rs);
    FD_SET(sock, &rs);
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    int rc = select(sock + 1, &rs, NULL, NULL, timeout_ms < 0 ? NULL : &tv);
    return rc > 0;
}

int net_recv_some(int sock, void *buf, size_t maxlen, int timeout_ms) {
    if (!wait_readable(sock, timeout_ms)) return 0;
    int n = (int)recv(sock, (char *)buf, (int)maxlen, 0);
    if (n < 0) return -1;
    return n;
}

bool net_recv_exact(int sock, void *buf, size_t len, int timeout_ms) {
    char *p = (char *)buf;
    while (len > 0) {
        if (!wait_readable(sock, timeout_ms)) return false;
        int n = (int)recv(sock, p, (int)len, 0);
        if (n <= 0) return false;
        p += n; len -= (size_t)n;
    }
    return true;
}

void net_close(int sock) {
    if (sock >= 0) close_socket(sock);
}
