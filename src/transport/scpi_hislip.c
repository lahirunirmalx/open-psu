/**
 * HiSLIP transport (IVI-6.1) — binary-framed SCPI over TCP, port 4880.
 *
 * Port-spec:  hislip:<host>[:<port>][:<sub-address>]
 *             defaults: port=4880, sub-address="hislip0"
 *
 * v1 scope: basic synchronous channel only.
 *   - Initialize / InitializeResponse handshake
 *   - Data + DataEnd frames for send
 *   - Data + DataEnd frames for receive
 *   No async channel, no locking, no interrupts, no status queries —
 *   those aren't needed for plain SCPI send/query.
 *
 * Header layout (every frame, exactly 16 bytes, big-endian):
 *     +0  uint8[2]  magic       0x68 'h'  0x73 's'
 *     +2  uint8     msg_type
 *     +3  uint8     control_code
 *     +4  uint32    message_parameter
 *     +8  uint64    payload_length
 *    +16  bytes…    payload
 */

#include "scpi.h"
#include "net_io.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HISLIP_DEFAULT_PORT     4880
#define HISLIP_DEFAULT_SUBADDR  "hislip0"
#define HISLIP_VERSION_MAJOR    1
#define HISLIP_VERSION_MINOR    0
#define HISLIP_VENDOR_ID        0x4001    /* "LB" — Open LabBench. Just a tag for the server. */
#define CONNECT_TIMEOUT_MS      3000
#define IO_TIMEOUT_MS           3000
#define MAX_FRAME_PAYLOAD       65536

/* Message types we actually use (selection from the spec) */
enum {
    HS_INITIALIZE              = 0,
    HS_INITIALIZE_RESPONSE     = 1,
    HS_FATAL_ERROR             = 2,
    HS_ERROR                   = 3,
    HS_DATA                    = 6,
    HS_DATA_END                = 7,
};

typedef struct {
    int      sock;
    uint16_t session_id;
} hislip_state_t;

/* ---- frame helpers (big-endian put/get) ------------------------------ */

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static void put_be64(uint8_t *p, uint64_t v) {
    put_be32(p,     (uint32_t)(v >> 32));
    put_be32(p + 4, (uint32_t)(v));
}
static uint32_t get_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}
static uint64_t get_be64(const uint8_t *p) {
    return ((uint64_t)get_be32(p) << 32) | get_be32(p + 4);
}

static bool hs_send_frame(int sock, uint8_t msg_type, uint8_t ctrl,
                          uint32_t mparam,
                          const void *payload, size_t payload_len) {
    uint8_t hdr[16];
    hdr[0] = 'h'; hdr[1] = 's';
    hdr[2] = msg_type;
    hdr[3] = ctrl;
    put_be32(hdr + 4, mparam);
    put_be64(hdr + 8, (uint64_t)payload_len);
    if (!net_send_all(sock, hdr, sizeof(hdr))) return false;
    if (payload_len == 0) return true;
    return net_send_all(sock, payload, payload_len);
}

/* Read one frame header + drain its payload into `out` (caller-sized).
 * Returns the number of payload bytes that fit (truncating cleanly if the
 * payload is bigger than out_cap), -1 on error, or 0 on timeout. */
static int hs_recv_frame(int sock, uint8_t *msg_type, uint8_t *ctrl,
                         uint32_t *mparam,
                         void *out, size_t out_cap, int timeout_ms) {
    uint8_t hdr[16];
    if (!net_recv_exact(sock, hdr, sizeof(hdr), timeout_ms)) return -1;
    if (hdr[0] != 'h' || hdr[1] != 's') return -1;
    if (msg_type) *msg_type = hdr[2];
    if (ctrl)     *ctrl     = hdr[3];
    if (mparam)   *mparam   = get_be32(hdr + 4);
    uint64_t plen = get_be64(hdr + 8);
    if (plen > MAX_FRAME_PAYLOAD) return -1;
    if (plen == 0) return 0;
    size_t take = (plen < out_cap) ? (size_t)plen : out_cap;
    if (!net_recv_exact(sock, out, take, timeout_ms)) return -1;
    /* Discard any overflow bytes so the next frame parses cleanly. */
    if (take < plen) {
        size_t left = (size_t)plen - take;
        uint8_t junk[256];
        while (left > 0) {
            size_t want = (left < sizeof(junk)) ? left : sizeof(junk);
            if (!net_recv_exact(sock, junk, want, timeout_ms)) return -1;
            left -= want;
        }
    }
    return (int)take;
}

/* ---- handshake ------------------------------------------------------- */

static bool hislip_initialize(hislip_state_t *t, const char *sub_address) {
    /* message_parameter: high 16 = major.minor (each 8 bits), low 16 = vendor ID. */
    uint32_t mparam = ((HISLIP_VERSION_MAJOR & 0xFF) << 24) |
                      ((HISLIP_VERSION_MINOR & 0xFF) << 16) |
                      (HISLIP_VENDOR_ID & 0xFFFF);
    if (!hs_send_frame(t->sock, HS_INITIALIZE, 0, mparam,
                       sub_address, strlen(sub_address))) return false;

    uint8_t  msg_type = 0, ctrl = 0;
    uint32_t resp_mparam = 0;
    uint8_t  buf[64];
    int n = hs_recv_frame(t->sock, &msg_type, &ctrl, &resp_mparam,
                          buf, sizeof(buf), CONNECT_TIMEOUT_MS);
    if (n < 0) return false;
    if (msg_type == HS_FATAL_ERROR || msg_type == HS_ERROR) {
        fprintf(stderr, "hislip: server returned error msg_type=%u\n", msg_type);
        return false;
    }
    if (msg_type != HS_INITIALIZE_RESPONSE) return false;

    /* InitializeResponse mparam: high 16 = negotiated version, low 16 = session ID. */
    t->session_id = (uint16_t)(resp_mparam & 0xFFFF);
    return true;
}

/* ---- send: one DataEnd frame is sufficient for a complete SCPI cmd --- */

static bool hislip_send(hislip_state_t *t, const char *cmd) {
    /* SCPI conventionally ends with newline; some HiSLIP servers tolerate
     * its absence on DataEnd but Keysight's docs recommend it. */
    size_t n = strlen(cmd);
    size_t total = n + 1;
    char *line = (char *)malloc(total);
    if (!line) return false;
    memcpy(line, cmd, n);
    line[n] = '\n';
    bool ok = hs_send_frame(t->sock, HS_DATA_END, 0, /*mparam=msg-id=*/0,
                            line, total);
    free(line);
    return ok;
}

/* ---- recv: read frames until DataEnd, concatenate payloads ----------- */

static bool hislip_recv(hislip_state_t *t, char *out, size_t outlen, int timeout_ms) {
    if (!out || outlen == 0) return false;
    size_t total = 0;
    for (;;) {
        uint8_t msg_type = 0, ctrl = 0;
        uint32_t mparam = 0;
        size_t cap = (total < outlen - 1) ? (outlen - 1 - total) : 0;
        int n = hs_recv_frame(t->sock, &msg_type, &ctrl, &mparam,
                              out + total, cap, timeout_ms);
        if (n < 0) return false;
        if (msg_type == HS_DATA || msg_type == HS_DATA_END) {
            total += (size_t)n;
            if (msg_type == HS_DATA_END) break;
            if (total + 1 >= outlen) break;  /* output buffer full */
        } else if (msg_type == HS_FATAL_ERROR || msg_type == HS_ERROR) {
            return false;
        }
        /* Other message types (locks, interrupts, etc.) are ignored. */
    }
    out[total] = '\0';
    while (total > 0 && (out[total - 1] == '\n' || out[total - 1] == '\r')) {
        out[--total] = '\0';
    }
    return true;
}

/* ---- scpi_t vtable --------------------------------------------------- */

static hislip_state_t *st_of(scpi_t *s) { return (hislip_state_t *)s->state; }

static void v_close(scpi_t *s) {
    if (!s) return;
    hislip_state_t *t = st_of(s);
    if (t) {
        if (t->sock >= 0) net_close(t->sock);
        free(t);
    }
    s->state = NULL;
}

static bool v_send(scpi_t *s, const char *cmd) { return hislip_send(st_of(s), cmd); }
static bool v_recv(scpi_t *s, char *out, size_t outlen, int timeout_ms) {
    return hislip_recv(st_of(s), out, outlen, timeout_ms);
}

/* ---- factory --------------------------------------------------------- */

scpi_t *scpi_hislip_open(const char *host, int port, const char *sub_address) {
    if (!host || !*host) return NULL;
    if (port <= 0) port = HISLIP_DEFAULT_PORT;
    if (!sub_address || !*sub_address) sub_address = HISLIP_DEFAULT_SUBADDR;

    int sock = net_tcp_connect(host, port, CONNECT_TIMEOUT_MS);
    if (sock < 0) return NULL;

    scpi_t         *s = calloc(1, sizeof(*s));
    hislip_state_t *t = calloc(1, sizeof(*t));
    if (!s || !t) { free(s); free(t); net_close(sock); return NULL; }

    pthread_mutex_init(&s->lock, NULL);
    t->sock = sock;
    t->session_id = 0;

    if (!hislip_initialize(t, sub_address)) {
        fprintf(stderr, "hislip: Initialize handshake failed (host=%s port=%d sub=%s)\n",
                host, port, sub_address);
        net_close(sock);
        free(t); free(s);
        return NULL;
    }

    s->state      = t;
    s->close_impl = v_close;
    s->send_impl  = v_send;
    s->recv_impl  = v_recv;

    fprintf(stderr, "hislip: connected to %s:%d (sub-address='%s', session=%u)\n",
            host, port, sub_address, t->session_id);
    return s;
}
