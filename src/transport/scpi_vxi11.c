/**
 * VXI-11 transport — ONC RPC (Sun RPC) over TCP.
 *
 * Port-spec:  vxi11:<host>[:<device-name>]
 *             default device-name = "inst0"
 *
 * Connection flow:
 *   1. portmap GETPORT (TCP:111) to discover DEVICE_CORE port
 *   2. open TCP to that port
 *   3. CREATE_LINK with the device name → link id
 *   4. scpi_send  → DEVICE_WRITE  with FLAGS_END
 *      scpi_recv  → DEVICE_READ
 *   5. close → DESTROY_LINK + TCP close
 *
 * XDR/RPC framing is hand-rolled (no rpc-glib dependency). Everything is
 * big-endian, 4-byte aligned per RFC 4506.
 */

#include "scpi.h"
#include "net_io.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- VXI-11 program numbers (per ANSI/VXI-11 Rev 1.0) ----------------- */
#define VXI11_DEVICE_CORE    0x0607AF
#define VXI11_DEVICE_VERSION 1

/* core procs we use */
#define VXI11_PROC_CREATE_LINK   10
#define VXI11_PROC_DEVICE_WRITE  11
#define VXI11_PROC_DEVICE_READ   12
#define VXI11_PROC_DESTROY_LINK  23

/* portmap (rpcbind v2) */
#define PMAP_PROG     100000u
#define PMAP_VERS     2u
#define PMAP_GETPORT  3u
#define PMAP_PORT     111
#define IPPROTO_RPC_TCP 6

/* DEVICE_WRITE flags */
#define VXI11_FLAG_WAITLOCK 0x01
#define VXI11_FLAG_END      0x08
#define VXI11_FLAG_TERMCHRS 0x80

/* DEVICE_READ reason bits */
#define VXI11_REASON_REQCNT 0x01
#define VXI11_REASON_CHR    0x02
#define VXI11_REASON_END    0x04

#define DEFAULT_DEVICE      "inst0"
#define CONNECT_TIMEOUT_MS  3000
#define DEFAULT_IO_TIMEOUT  3000   /* milliseconds per instrument I/O */
#define DEFAULT_LOCK_TIMEOUT 0
#define MAX_RECV_DATA       65536

typedef struct {
    int      sock;
    uint32_t link_id;
    uint32_t max_recv_size;
    uint32_t xid;
} vxi11_state_t;

/* ---- XDR helpers ------------------------------------------------------- */

static void put_u32(uint8_t **p, uint32_t v) {
    (*p)[0] = (uint8_t)(v >> 24);
    (*p)[1] = (uint8_t)(v >> 16);
    (*p)[2] = (uint8_t)(v >> 8);
    (*p)[3] = (uint8_t)(v);
    *p += 4;
}

static uint32_t get_u32(const uint8_t **p) {
    uint32_t v = ((uint32_t)(*p)[0] << 24)
               | ((uint32_t)(*p)[1] << 16)
               | ((uint32_t)(*p)[2] << 8)
               | ((uint32_t)(*p)[3]);
    *p += 4;
    return v;
}

static void put_opaque(uint8_t **p, const void *data, size_t len) {
    put_u32(p, (uint32_t)len);
    memcpy(*p, data, len);
    *p += len;
    /* pad to 4-byte boundary */
    size_t pad = (4 - (len & 3)) & 3;
    while (pad--) *(*p)++ = 0;
}

static void put_string(uint8_t **p, const char *s) {
    put_opaque(p, s, strlen(s));
}

/* ---- ONC RPC over TCP ------------------------------------------------- */

/* Build an RPC call header up to (but not including) the procedure args.
 * Returns the xid used. Writes into buf; **p is advanced past the header. */
static uint32_t build_rpc_call(uint8_t *buf, uint8_t **p,
                               uint32_t xid, uint32_t prog,
                               uint32_t vers, uint32_t proc) {
    *p = buf;
    /* Fragment header — patched in after we know total length. */
    put_u32(p, 0);
    /* RPC call message */
    put_u32(p, xid);          /* xid */
    put_u32(p, 0);            /* msg_type = CALL */
    put_u32(p, 2);            /* rpcvers = 2 */
    put_u32(p, prog);
    put_u32(p, vers);
    put_u32(p, proc);
    put_u32(p, 0); put_u32(p, 0);  /* cred: flavor=0, len=0  (AUTH_NONE) */
    put_u32(p, 0); put_u32(p, 0);  /* verf: flavor=0, len=0 */
    return xid;
}

/* Send an RPC call already assembled in buf (starting at the fragment
 * header). Patches the fragment header in place. */
static bool rpc_send(int sock, uint8_t *buf, uint8_t *end) {
    size_t total = (size_t)(end - buf);
    if (total < 4) return false;
    uint32_t frag_payload = (uint32_t)(total - 4);
    uint32_t frag_hdr = frag_payload | 0x80000000u;   /* MSB = last fragment */
    uint8_t *fp = buf;
    put_u32(&fp, frag_hdr);
    return net_send_all(sock, buf, total);
}

/* Read one full RPC record into out_buf (up to out_cap bytes). Returns the
 * number of payload bytes, or -1 on error. Strips fragment headers. */
static int rpc_recv(int sock, uint8_t *out_buf, size_t out_cap, int timeout_ms) {
    size_t off = 0;
    for (;;) {
        uint8_t hdr[4];
        if (!net_recv_exact(sock, hdr, sizeof(hdr), timeout_ms)) return -1;
        const uint8_t *p = hdr;
        uint32_t h = get_u32(&p);
        bool last = (h & 0x80000000u) != 0;
        uint32_t flen = h & 0x7fffffffu;
        if (off + flen > out_cap) return -1;
        if (!net_recv_exact(sock, out_buf + off, flen, timeout_ms)) return -1;
        off += flen;
        if (last) break;
    }
    return (int)off;
}

/* After reading an RPC reply, advance `p` past the RPC reply header to
 * the procedure-specific result body. Returns false on any RPC error. */
static bool parse_rpc_reply_header(const uint8_t **p, const uint8_t *end,
                                   uint32_t expected_xid) {
    if (end - *p < 6 * 4) return false;
    uint32_t xid = get_u32(p);
    if (xid != expected_xid) return false;
    if (get_u32(p) != 1) return false;        /* msg_type = REPLY */
    if (get_u32(p) != 0) return false;        /* reply_stat = MSG_ACCEPTED */
    /* verf: flavor, len */
    get_u32(p);
    uint32_t vlen = get_u32(p);
    if (*p + vlen > end) return false;
    *p += vlen;
    /* pad */
    *p += (4 - (vlen & 3)) & 3;
    if (end - *p < 4) return false;
    uint32_t accept_stat = get_u32(p);
    return accept_stat == 0;                  /* SUCCESS */
}

/* ---- portmap GETPORT -------------------------------------------------- */

static int portmap_getport(const char *host, uint32_t prog, uint32_t vers) {
    int sock = net_tcp_connect(host, PMAP_PORT, CONNECT_TIMEOUT_MS);
    if (sock < 0) return -1;

    uint8_t buf[64], *p;
    uint32_t xid = (uint32_t)((uintptr_t)host ^ prog);
    build_rpc_call(buf, &p, xid, PMAP_PROG, PMAP_VERS, PMAP_GETPORT);
    put_u32(&p, prog);
    put_u32(&p, vers);
    put_u32(&p, IPPROTO_RPC_TCP);
    put_u32(&p, 0);   /* port=0 (we want the answer, not to register) */

    if (!rpc_send(sock, buf, p)) { net_close(sock); return -1; }

    uint8_t reply[128];
    int rlen = rpc_recv(sock, reply, sizeof(reply), CONNECT_TIMEOUT_MS);
    net_close(sock);
    if (rlen < 0) return -1;

    const uint8_t *rp = reply, *rend = reply + rlen;
    if (!parse_rpc_reply_header(&rp, rend, xid)) return -1;
    if (rend - rp < 4) return -1;
    int port = (int)get_u32(&rp);
    return port > 0 ? port : -1;
}

/* ---- VXI-11 CREATE_LINK ---------------------------------------------- */

static bool vxi11_create_link(vxi11_state_t *t, const char *device) {
    uint8_t buf[256], *p;
    uint32_t xid = ++t->xid;
    build_rpc_call(buf, &p, xid, VXI11_DEVICE_CORE,
                   VXI11_DEVICE_VERSION, VXI11_PROC_CREATE_LINK);
    put_u32(&p, 0);                 /* clientID */
    put_u32(&p, 0);                 /* lockDevice = false */
    put_u32(&p, DEFAULT_LOCK_TIMEOUT);
    put_string(&p, device);

    if (!rpc_send(t->sock, buf, p)) return false;

    uint8_t reply[64];
    int rlen = rpc_recv(t->sock, reply, sizeof(reply), CONNECT_TIMEOUT_MS);
    if (rlen < 0) return false;

    const uint8_t *rp = reply, *rend = reply + rlen;
    if (!parse_rpc_reply_header(&rp, rend, xid)) return false;
    if (rend - rp < 4 * 4) return false;
    uint32_t err  = get_u32(&rp);
    if (err != 0) {
        fprintf(stderr, "vxi11: CREATE_LINK failed (err=%u)\n", err);
        return false;
    }
    t->link_id        = get_u32(&rp);
    /* uint32_t abort_port = */ get_u32(&rp);
    t->max_recv_size  = get_u32(&rp);
    if (t->max_recv_size == 0 || t->max_recv_size > MAX_RECV_DATA)
        t->max_recv_size = MAX_RECV_DATA;
    return true;
}

static void vxi11_destroy_link(vxi11_state_t *t) {
    if (t->sock < 0) return;
    uint8_t buf[64], *p;
    uint32_t xid = ++t->xid;
    build_rpc_call(buf, &p, xid, VXI11_DEVICE_CORE,
                   VXI11_DEVICE_VERSION, VXI11_PROC_DESTROY_LINK);
    put_u32(&p, t->link_id);
    rpc_send(t->sock, buf, p);
    /* Best-effort: read and discard the reply. */
    uint8_t reply[64];
    rpc_recv(t->sock, reply, sizeof(reply), 500);
}

/* ---- DEVICE_WRITE ---------------------------------------------------- */

static bool vxi11_write(vxi11_state_t *t, const char *cmd) {
    size_t n = strlen(cmd);
    /* The instrument expects a newline terminator on each command. */
    size_t bufsz = 64 + n + 4;
    uint8_t *buf = (uint8_t *)malloc(bufsz);
    if (!buf) return false;

    uint8_t *p;
    uint32_t xid = ++t->xid;
    build_rpc_call(buf, &p, xid, VXI11_DEVICE_CORE,
                   VXI11_DEVICE_VERSION, VXI11_PROC_DEVICE_WRITE);
    put_u32(&p, t->link_id);
    put_u32(&p, DEFAULT_IO_TIMEOUT);    /* io_timeout (ms) */
    put_u32(&p, DEFAULT_LOCK_TIMEOUT);  /* lock_timeout */
    put_u32(&p, VXI11_FLAG_END);
    /* data: command + '\n' as opaque */
    char *line = (char *)malloc(n + 2);
    if (!line) { free(buf); return false; }
    memcpy(line, cmd, n);
    line[n]     = '\n';
    line[n + 1] = '\0';
    put_opaque(&p, line, n + 1);
    free(line);

    bool ok = rpc_send(t->sock, buf, p);
    free(buf);
    if (!ok) return false;

    uint8_t reply[64];
    int rlen = rpc_recv(t->sock, reply, sizeof(reply), DEFAULT_IO_TIMEOUT + 1000);
    if (rlen < 0) return false;
    const uint8_t *rp = reply, *rend = reply + rlen;
    if (!parse_rpc_reply_header(&rp, rend, xid)) return false;
    if (rend - rp < 4 * 2) return false;
    uint32_t err = get_u32(&rp);
    /* uint32_t size = */ get_u32(&rp);
    return err == 0;
}

/* ---- DEVICE_READ ----------------------------------------------------- */

static bool vxi11_read(vxi11_state_t *t, char *out, size_t outlen, int timeout_ms) {
    uint8_t buf[64], *p;
    uint32_t xid = ++t->xid;
    build_rpc_call(buf, &p, xid, VXI11_DEVICE_CORE,
                   VXI11_DEVICE_VERSION, VXI11_PROC_DEVICE_READ);
    put_u32(&p, t->link_id);
    put_u32(&p, (uint32_t)(outlen > t->max_recv_size ? t->max_recv_size : outlen));
    put_u32(&p, (uint32_t)timeout_ms);
    put_u32(&p, DEFAULT_LOCK_TIMEOUT);
    put_u32(&p, VXI11_FLAG_TERMCHRS);      /* request termchar reason */
    put_u32(&p, '\n');                     /* termchar */
    if (!rpc_send(t->sock, buf, p)) return false;

    /* Generous reply buffer — the data field is variable-length. */
    size_t reply_cap = 32 + outlen + 16;
    uint8_t *reply = (uint8_t *)malloc(reply_cap);
    if (!reply) return false;
    int rlen = rpc_recv(t->sock, reply, reply_cap, timeout_ms + 1000);
    if (rlen < 0) { free(reply); return false; }

    const uint8_t *rp = reply, *rend = reply + rlen;
    if (!parse_rpc_reply_header(&rp, rend, xid)) { free(reply); return false; }
    if (rend - rp < 4 * 3) { free(reply); return false; }
    uint32_t err    = get_u32(&rp);
    /* uint32_t reason = */ get_u32(&rp);
    uint32_t dlen   = get_u32(&rp);
    if (err != 0 || (uintptr_t)(rend - rp) < dlen) {
        free(reply);
        return false;
    }
    size_t take = (dlen < outlen - 1) ? dlen : outlen - 1;
    memcpy(out, rp, take);
    out[take] = '\0';
    /* Strip the trailing termchar(s). */
    while (take > 0 && (out[take - 1] == '\n' || out[take - 1] == '\r')) {
        out[--take] = '\0';
    }
    free(reply);
    return true;
}

/* ---- scpi_t vtable wrappers ----------------------------------------- */

static vxi11_state_t *st_of(scpi_t *s) { return (vxi11_state_t *)s->state; }

static void vxi11_close(scpi_t *s) {
    if (!s) return;
    vxi11_state_t *t = st_of(s);
    if (t) {
        vxi11_destroy_link(t);
        if (t->sock >= 0) net_close(t->sock);
        free(t);
    }
    s->state = NULL;
}

static bool vxi11_send_impl(scpi_t *s, const char *cmd) {
    return vxi11_write(st_of(s), cmd);
}

static bool vxi11_recv_impl(scpi_t *s, char *out, size_t outlen, int timeout_ms) {
    return vxi11_read(st_of(s), out, outlen, timeout_ms);
}

/* ---- factory --------------------------------------------------------- */

scpi_t *scpi_vxi11_open(const char *host, const char *device_name) {
    if (!host || !*host) return NULL;
    if (!device_name || !*device_name) device_name = DEFAULT_DEVICE;

    int core_port = portmap_getport(host, VXI11_DEVICE_CORE, VXI11_DEVICE_VERSION);
    if (core_port < 0) {
        fprintf(stderr, "vxi11: portmap lookup on %s failed — is rpcbind reachable?\n", host);
        return NULL;
    }
    int sock = net_tcp_connect(host, core_port, CONNECT_TIMEOUT_MS);
    if (sock < 0) return NULL;

    scpi_t       *s = calloc(1, sizeof(*s));
    vxi11_state_t *t = calloc(1, sizeof(*t));
    if (!s || !t) { free(s); free(t); net_close(sock); return NULL; }

    pthread_mutex_init(&s->lock, NULL);
    t->sock    = sock;
    t->link_id = 0;
    t->xid     = (uint32_t)((uintptr_t)host & 0xffffu);

    if (!vxi11_create_link(t, device_name)) {
        net_close(sock);
        free(t); free(s);
        return NULL;
    }

    s->state      = t;
    s->close_impl = vxi11_close;
    s->send_impl  = vxi11_send_impl;
    s->recv_impl  = vxi11_recv_impl;

    fprintf(stderr, "vxi11: connected to %s (port %d, link %u, device '%s')\n",
            host, core_port, t->link_id, device_name);
    return s;
}
