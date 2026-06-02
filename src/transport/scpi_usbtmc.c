/**
 * USB-TMC transport — two backends share one factory function.
 *
 * Port-spec accepted forms:
 *
 *   usbtmc:/dev/usbtmc0            Linux kernel `usbtmc` driver — easy +
 *                                   stable; needs the udev rule
 *                                   SUBSYSTEM=="usbmisc", KERNEL=="usbtmc*",
 *                                   MODE="0660", GROUP="plugdev"
 *                                   for non-root access.
 *
 *   usbtmc:<vid>:<pid>             Userspace libusb backend with host-side
 *   usbtmc:<vid>:<pid>:<serial>    USB-TMC framing. Works on Linux, Windows
 *                                   (after Zadig installs the WinUSB driver
 *                                   on the instrument's TMC interface), and
 *                                   macOS. <vid>/<pid> in lowercase hex.
 *
 * On Windows the kernel-device path is unavailable, so users go through
 * the libusb path. On Linux either path works; the libusb path is useful
 * if the kernel module isn't loaded.
 */

#include "scpi.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- detect form of port spec ---------------------------------------- */

static bool looks_like_device_path(const char *s) {
    return s && (s[0] == '/' || s[0] == '.');
}

/* Forward decls of the two backends (each may compile to a stub). */
static scpi_t *open_kernel(const char *device);
static scpi_t *open_libusb(const char *device);

scpi_t *scpi_usbtmc_open(const char *device) {
    if (!device || !*device) return NULL;
    if (looks_like_device_path(device)) return open_kernel(device);
    return open_libusb(device);
}

/* ============== Linux kernel /dev/usbtmcN backend ===================== */

#ifdef __linux__

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

typedef struct {
    int fd;
} kdrv_state_t;

static kdrv_state_t *kdrv_st(scpi_t *s) { return (kdrv_state_t *)s->state; }

static void kdrv_close(scpi_t *s) {
    if (!s) return;
    kdrv_state_t *t = kdrv_st(s);
    if (t) {
        if (t->fd >= 0) close(t->fd);
        free(t);
    }
    s->state = NULL;
}

static bool kdrv_send(scpi_t *s, const char *cmd) {
    kdrv_state_t *t = kdrv_st(s);
    if (!t || t->fd < 0 || !cmd) return false;
    size_t n = strlen(cmd);
    char *line = (char *)malloc(n + 2);
    if (!line) return false;
    memcpy(line, cmd, n); line[n] = '\n'; line[n + 1] = '\0';
    ssize_t off = 0, need = (ssize_t)(n + 1);
    while (off < need) {
        ssize_t w = write(t->fd, line + off, (size_t)(need - off));
        if (w < 0) { if (errno == EINTR) continue; free(line); return false; }
        off += w;
    }
    free(line);
    return true;
}

static bool kdrv_recv(scpi_t *s, char *out, size_t outlen, int timeout_ms) {
    kdrv_state_t *t = kdrv_st(s);
    if (!t || t->fd < 0 || !out || outlen == 0) return false;
    fd_set rs;
    FD_ZERO(&rs); FD_SET(t->fd, &rs);
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    int rc = select(t->fd + 1, &rs, NULL, NULL, timeout_ms < 0 ? NULL : &tv);
    if (rc <= 0) return false;
    ssize_t n = read(t->fd, out, outlen - 1);
    if (n < 0) return false;
    out[n] = '\0';
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = '\0';
    return true;
}

static scpi_t *open_kernel(const char *device) {
    int fd = open(device, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "usbtmc(kernel): open('%s') failed: %s\n", device, strerror(errno));
        return NULL;
    }
    scpi_t       *s = calloc(1, sizeof(*s));
    kdrv_state_t *t = calloc(1, sizeof(*t));
    if (!s || !t) { free(s); free(t); close(fd); return NULL; }
    pthread_mutex_init(&s->lock, NULL);
    t->fd = fd;
    s->state = t;
    s->close_impl = kdrv_close;
    s->send_impl  = kdrv_send;
    s->recv_impl  = kdrv_recv;
    return s;
}

#else  /* non-Linux: kernel-driver path is unavailable */

static scpi_t *open_kernel(const char *device) {
    (void)device;
    fprintf(stderr,
        "usbtmc: kernel /dev/usbtmc* path is Linux-only. Use the libusb form\n"
        "        instead: usbtmc:<vid>:<pid>[:<serial>] (lowercase hex IDs).\n");
    return NULL;
}

#endif

/* ============== libusb userspace backend ============================== */

#ifdef HAVE_LIBUSB

#include <libusb-1.0/libusb.h>
#include <pthread.h>

/* USB-TMC interface descriptor markers (USBTMC 1.0). */
#define USBTMC_IF_CLASS    0xFE   /* Application Specific */
#define USBTMC_IF_SUBCLASS 0x03   /* USB-TMC */

/* Bulk message IDs (USBTMC 1.0 §3.2). */
#define DEV_DEP_MSG_OUT            1
#define REQUEST_DEV_DEP_MSG_IN     2
#define DEV_DEP_MSG_IN             2

#define TIMEOUT_MS_DEFAULT 5000

typedef struct {
    libusb_device_handle *h;
    int   iface;
    uint8_t ep_out;
    uint8_t ep_in;
    uint8_t bTag;
    int   timeout_ms;
} lusb_state_t;

static lusb_state_t *lusb_st(scpi_t *s) { return (lusb_state_t *)s->state; }

/* Parse "<vid>:<pid>[:<serial>]" — returns true on success. */
static bool parse_vid_pid_serial(const char *spec,
                                 uint16_t *vid_out, uint16_t *pid_out,
                                 char *serial_buf, size_t serial_cap) {
    char vid_s[16] = {0}, pid_s[16] = {0};
    const char *p = spec;
    /* vid */
    const char *colon = strchr(p, ':');
    if (!colon || (size_t)(colon - p) >= sizeof(vid_s)) return false;
    memcpy(vid_s, p, (size_t)(colon - p));
    p = colon + 1;
    /* pid (may end the string or have :<serial>) */
    const char *colon2 = strchr(p, ':');
    if (colon2) {
        if ((size_t)(colon2 - p) >= sizeof(pid_s)) return false;
        memcpy(pid_s, p, (size_t)(colon2 - p));
        const char *s = colon2 + 1;
        if (serial_buf && serial_cap > 0)
            snprintf(serial_buf, serial_cap, "%s", s);
    } else {
        snprintf(pid_s, sizeof(pid_s), "%s", p);
        if (serial_buf && serial_cap > 0) serial_buf[0] = '\0';
    }
    unsigned vid = 0, pid = 0;
    if (sscanf(vid_s, "%x", &vid) != 1) return false;
    if (sscanf(pid_s, "%x", &pid) != 1) return false;
    *vid_out = (uint16_t)vid;
    *pid_out = (uint16_t)pid;
    return true;
}

/* Find the first USB-TMC interface on `dev` and its bulk endpoints. */
static bool find_tmc_interface(libusb_device *dev, int *iface_out,
                               uint8_t *ep_in_out, uint8_t *ep_out_out) {
    struct libusb_config_descriptor *cfg = NULL;
    if (libusb_get_active_config_descriptor(dev, &cfg) != 0 || !cfg) return false;
    bool found = false;
    for (int i = 0; i < cfg->bNumInterfaces && !found; i++) {
        const struct libusb_interface *iface = &cfg->interface[i];
        for (int a = 0; a < iface->num_altsetting && !found; a++) {
            const struct libusb_interface_descriptor *id = &iface->altsetting[a];
            if (id->bInterfaceClass    != USBTMC_IF_CLASS    ) continue;
            if (id->bInterfaceSubClass != USBTMC_IF_SUBCLASS ) continue;
            uint8_t ep_in = 0, ep_out = 0;
            for (int e = 0; e < id->bNumEndpoints; e++) {
                uint8_t addr = id->endpoint[e].bEndpointAddress;
                uint8_t attr = id->endpoint[e].bmAttributes & 0x03;
                if (attr != LIBUSB_TRANSFER_TYPE_BULK) continue;
                if (addr & 0x80) ep_in = addr;
                else             ep_out = addr;
            }
            if (ep_in && ep_out) {
                *iface_out  = id->bInterfaceNumber;
                *ep_in_out  = ep_in;
                *ep_out_out = ep_out;
                found = true;
            }
        }
    }
    libusb_free_config_descriptor(cfg);
    return found;
}

/* Send a Bulk-OUT DEV_DEP_MSG_OUT containing the SCPI command. */
static bool lusb_send(scpi_t *s, const char *cmd) {
    lusb_state_t *t = lusb_st(s);
    if (!t || !t->h || !cmd) return false;

    size_t n = strlen(cmd);
    /* Newline-terminate to match what most SCPI instruments expect. */
    size_t payload_len = n + 1;
    size_t pad = (4 - (payload_len & 3)) & 3;
    size_t total = 12 + payload_len + pad;

    uint8_t *buf = (uint8_t *)calloc(1, total);
    if (!buf) return false;

    t->bTag = (uint8_t)((t->bTag % 255) + 1);  /* 1..255, never 0 */
    buf[0] = DEV_DEP_MSG_OUT;
    buf[1] = t->bTag;
    buf[2] = (uint8_t)~t->bTag;
    buf[3] = 0;
    buf[4] = (uint8_t)(payload_len);
    buf[5] = (uint8_t)(payload_len >> 8);
    buf[6] = (uint8_t)(payload_len >> 16);
    buf[7] = (uint8_t)(payload_len >> 24);
    buf[8] = 0x01;        /* TransferAttributes: bit0 = EOM */
    buf[9] = buf[10] = buf[11] = 0;
    memcpy(buf + 12, cmd, n);
    buf[12 + n] = '\n';

    int xferred = 0;
    int rc = libusb_bulk_transfer(t->h, t->ep_out, buf, (int)total, &xferred, t->timeout_ms);
    free(buf);
    if (rc != 0) {
        fprintf(stderr, "usbtmc(libusb): bulk OUT failed (%s)\n", libusb_error_name(rc));
        return false;
    }
    return (size_t)xferred == total;
}

/* Issue REQUEST_DEV_DEP_MSG_IN then read DEV_DEP_MSG_IN payload. */
static bool lusb_recv(scpi_t *s, char *out, size_t outlen, int timeout_ms) {
    lusb_state_t *t = lusb_st(s);
    if (!t || !t->h || !out || outlen == 0) return false;

    uint32_t want = (uint32_t)(outlen > 65536 ? 65536 : outlen);
    uint8_t req[12] = {0};
    t->bTag = (uint8_t)((t->bTag % 255) + 1);
    req[0] = REQUEST_DEV_DEP_MSG_IN;
    req[1] = t->bTag;
    req[2] = (uint8_t)~t->bTag;
    req[3] = 0;
    req[4] = (uint8_t)(want);
    req[5] = (uint8_t)(want >> 8);
    req[6] = (uint8_t)(want >> 16);
    req[7] = (uint8_t)(want >> 24);
    req[8] = 0x02;          /* TransferAttributes: bit1 = TermChar enabled */
    req[9] = '\n';          /* TermChar */
    req[10] = req[11] = 0;

    int xferred = 0;
    int rc = libusb_bulk_transfer(t->h, t->ep_out, req, sizeof(req), &xferred,
                                  timeout_ms > 0 ? timeout_ms : t->timeout_ms);
    if (rc != 0) {
        fprintf(stderr, "usbtmc(libusb): REQUEST_MSG_IN failed (%s)\n", libusb_error_name(rc));
        return false;
    }

    /* IN transfer: 12-byte header + payload. Round up alloc to a generous
     * boundary so a longer response than the caller asked for still reads
     * cleanly into our buffer before we truncate to `outlen`. */
    size_t cap = 12 + (size_t)want + 4;
    uint8_t *rxbuf = (uint8_t *)malloc(cap);
    if (!rxbuf) return false;
    rc = libusb_bulk_transfer(t->h, t->ep_in, rxbuf, (int)cap, &xferred,
                              timeout_ms > 0 ? timeout_ms : t->timeout_ms);
    if (rc != 0) {
        fprintf(stderr, "usbtmc(libusb): bulk IN failed (%s)\n", libusb_error_name(rc));
        free(rxbuf);
        return false;
    }
    if (xferred < 12) { free(rxbuf); return false; }

    uint32_t plen = (uint32_t)rxbuf[4]
                  | ((uint32_t)rxbuf[5] << 8)
                  | ((uint32_t)rxbuf[6] << 16)
                  | ((uint32_t)rxbuf[7] << 24);
    int payload_avail = xferred - 12;
    if (payload_avail < 0) payload_avail = 0;
    size_t take = (size_t)((plen < (uint32_t)payload_avail) ? plen : (uint32_t)payload_avail);
    if (take > outlen - 1) take = outlen - 1;
    memcpy(out, rxbuf + 12, take);
    out[take] = '\0';
    while (take > 0 && (out[take - 1] == '\n' || out[take - 1] == '\r'))
        out[--take] = '\0';
    free(rxbuf);
    return true;
}

static void lusb_close(scpi_t *s) {
    if (!s) return;
    lusb_state_t *t = lusb_st(s);
    if (t) {
        if (t->h) {
            libusb_release_interface(t->h, t->iface);
            libusb_close(t->h);
        }
        free(t);
    }
    s->state = NULL;
    libusb_exit(NULL);   /* matches libusb_init in open_libusb */
}

static scpi_t *open_libusb(const char *spec) {
    uint16_t vid = 0, pid = 0;
    char want_serial[64] = {0};
    if (!parse_vid_pid_serial(spec, &vid, &pid, want_serial, sizeof(want_serial))) {
        fprintf(stderr, "usbtmc: bad spec '%s' — expected <vid>:<pid>[:<serial>]\n", spec);
        return NULL;
    }

    if (libusb_init(NULL) != 0) {
        fprintf(stderr, "usbtmc(libusb): libusb_init failed\n");
        return NULL;
    }

    libusb_device **list = NULL;
    ssize_t cnt = libusb_get_device_list(NULL, &list);
    if (cnt < 0) {
        fprintf(stderr, "usbtmc(libusb): get_device_list failed\n");
        libusb_exit(NULL);
        return NULL;
    }

    libusb_device_handle *h = NULL;
    int iface = -1;
    uint8_t ep_in = 0, ep_out = 0;
    for (ssize_t i = 0; i < cnt; i++) {
        struct libusb_device_descriptor d;
        if (libusb_get_device_descriptor(list[i], &d) != 0) continue;
        if (d.idVendor != vid || d.idProduct != pid) continue;
        if (!find_tmc_interface(list[i], &iface, &ep_in, &ep_out)) continue;

        if (libusb_open(list[i], &h) != 0) { h = NULL; continue; }

        if (want_serial[0]) {
            unsigned char serial[128];
            int len = libusb_get_string_descriptor_ascii(h, d.iSerialNumber,
                                                        serial, sizeof(serial));
            if (len < 0 || strcmp((const char *)serial, want_serial) != 0) {
                libusb_close(h);
                h = NULL;
                continue;
            }
        }
        break;
    }
    libusb_free_device_list(list, 1);

    if (!h) {
        fprintf(stderr, "usbtmc(libusb): no USB-TMC device matching %04x:%04x%s%s\n",
                vid, pid, want_serial[0] ? " serial=" : "", want_serial);
        libusb_exit(NULL);
        return NULL;
    }

    libusb_set_auto_detach_kernel_driver(h, 1);
    int rc = libusb_claim_interface(h, iface);
    if (rc != 0) {
        fprintf(stderr, "usbtmc(libusb): claim_interface(%d) failed (%s)\n",
                iface, libusb_error_name(rc));
        libusb_close(h);
        libusb_exit(NULL);
        return NULL;
    }

    scpi_t       *s = calloc(1, sizeof(*s));
    lusb_state_t *t = calloc(1, sizeof(*t));
    if (!s || !t) { free(s); free(t); libusb_close(h); libusb_exit(NULL); return NULL; }
    pthread_mutex_init(&s->lock, NULL);
    t->h          = h;
    t->iface      = iface;
    t->ep_in      = ep_in;
    t->ep_out     = ep_out;
    t->bTag       = 0;
    t->timeout_ms = TIMEOUT_MS_DEFAULT;
    s->state      = t;
    s->close_impl = lusb_close;
    s->send_impl  = lusb_send;
    s->recv_impl  = lusb_recv;
    fprintf(stderr, "usbtmc(libusb): connected to %04x:%04x (iface=%d ep_in=0x%02x ep_out=0x%02x)\n",
            vid, pid, iface, ep_in, ep_out);
    return s;
}

#else  /* HAVE_LIBUSB not defined */

static scpi_t *open_libusb(const char *spec) {
    (void)spec;
    fprintf(stderr,
        "usbtmc: this build has no libusb backend. Rebuild with libusb-1.0\n"
        "        installed (Linux: libusb-1.0-0-dev; MSYS2: mingw-w64-x86_64-libusb)\n"
        "        and re-run `make`.\n");
    return NULL;
}

#endif
