/**
 * Win32 native implementation of the serial_port.h API.
 *
 * Selected by the Makefile when building on Windows (MinGW-w64). Uses
 * CreateFileW + SetCommState + SetCommTimeouts + ReadFile/WriteFile.
 *
 * Device naming: pass "COM3" etc. Devices with a 2+ digit index
 * ("COM10") are silently rewritten to "\\\\.\\COM10" so Windows opens
 * them correctly.
 */

#include "serial_port.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define READ_BUF_SIZE 4096

struct serial_port {
    HANDLE          h;
    pthread_mutex_t write_lock;
    char            read_buf[READ_BUF_SIZE];
    size_t          read_pos;
};

/* Build a "\\.\COM<N>" path. Caller frees. */
static WCHAR *build_device_path(const char *device) {
    /* Accept either "COM3" or full "\\.\COM3" inputs. */
    bool need_prefix = !(device[0] == '\\' && device[1] == '\\');
    size_t bufsz = strlen(device) + (need_prefix ? 5 : 1);   /* "\\.\..." + NUL */
    char *path = (char *)malloc(bufsz);
    if (!path) return NULL;
    if (need_prefix) snprintf(path, bufsz, "\\\\.\\%s", device);
    else             snprintf(path, bufsz, "%s", device);

    int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    WCHAR *wpath = (WCHAR *)malloc((size_t)wlen * sizeof(WCHAR));
    if (wpath) MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, wlen);
    free(path);
    return wpath;
}

serial_port_t *serial_open(const char *device, int baud) {
    if (!device) return NULL;

    WCHAR *wpath = build_device_path(device);
    if (!wpath) return NULL;

    HANDLE h = CreateFileW(wpath, GENERIC_READ | GENERIC_WRITE,
                           0, NULL, OPEN_EXISTING, 0, NULL);
    free(wpath);
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "serial_open: CreateFileW failed for %s (err=%lu)\n",
                device, (unsigned long)GetLastError());
        return NULL;
    }

    DCB dcb = { .DCBlength = sizeof(dcb) };
    if (!GetCommState(h, &dcb)) {
        fprintf(stderr, "serial_open: GetCommState failed for %s\n", device);
        CloseHandle(h);
        return NULL;
    }
    dcb.BaudRate    = (DWORD)baud;
    dcb.ByteSize    = 8;
    dcb.Parity      = NOPARITY;
    dcb.StopBits    = ONESTOPBIT;
    dcb.fBinary     = TRUE;
    dcb.fParity     = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_DISABLE;
    dcb.fRtsControl = RTS_CONTROL_DISABLE;
    dcb.fOutX       = FALSE;
    dcb.fInX        = FALSE;
    if (!SetCommState(h, &dcb)) {
        fprintf(stderr, "serial_open: SetCommState failed for %s\n", device);
        CloseHandle(h);
        return NULL;
    }

    /* Non-blocking-ish reads: return immediately on any byte, timeout
     * after our caller-specified ms. Per-call adjustment happens in
     * serial_read_line / serial_read_bytes. */
    COMMTIMEOUTS to = {
        .ReadIntervalTimeout         = MAXDWORD,
        .ReadTotalTimeoutMultiplier  = MAXDWORD,
        .ReadTotalTimeoutConstant    = 0,
        .WriteTotalTimeoutMultiplier = 0,
        .WriteTotalTimeoutConstant   = 1000,
    };
    SetCommTimeouts(h, &to);

    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);

    serial_port_t *sp = (serial_port_t *)calloc(1, sizeof(*sp));
    if (!sp) { CloseHandle(h); return NULL; }
    sp->h = h;
    pthread_mutex_init(&sp->write_lock, NULL);
    return sp;
}

void serial_close(serial_port_t *sp) {
    if (!sp) return;
    if (sp->h && sp->h != INVALID_HANDLE_VALUE) CloseHandle(sp->h);
    pthread_mutex_destroy(&sp->write_lock);
    free(sp);
}

bool serial_is_open(serial_port_t *sp) {
    return sp && sp->h && sp->h != INVALID_HANDLE_VALUE;
}

/* Win32 doesn't have file descriptors in the POSIX sense. Drivers that
 * used to use serial_get_fd() for select() should use serial_read_bytes()
 * instead - that's the only portable path. */
int serial_get_fd(serial_port_t *sp) { (void)sp; return -1; }

bool serial_send_line(serial_port_t *sp, const char *line) {
    if (!serial_is_open(sp) || !line) return false;
    size_t n = strlen(line);

    pthread_mutex_lock(&sp->write_lock);
    DWORD written = 0;
    bool ok = (WriteFile(sp->h, line, (DWORD)n, &written, NULL) != 0)
              && written == n;
    if (ok) {
        char nl = '\n';
        ok = (WriteFile(sp->h, &nl, 1, &written, NULL) != 0)
              && written == 1;
    }
    pthread_mutex_unlock(&sp->write_lock);
    return ok;
}

bool serial_write_bytes(serial_port_t *sp, const void *buf, size_t len) {
    if (!serial_is_open(sp) || !buf) return false;
    if (len == 0) return true;
    pthread_mutex_lock(&sp->write_lock);
    DWORD written = 0;
    bool ok = WriteFile(sp->h, buf, (DWORD)len, &written, NULL) != 0
              && written == len;
    pthread_mutex_unlock(&sp->write_lock);
    return ok;
}

/* Configure read timeouts so a single ReadFile returns within `timeout_ms`
 * regardless of how many bytes are available. */
static void set_read_timeout(serial_port_t *sp, int timeout_ms) {
    COMMTIMEOUTS to;
    GetCommTimeouts(sp->h, &to);
    if (timeout_ms < 0) {
        to.ReadIntervalTimeout        = 0;
        to.ReadTotalTimeoutMultiplier = 0;
        to.ReadTotalTimeoutConstant   = 0;          /* blocking */
    } else {
        to.ReadIntervalTimeout        = MAXDWORD;   /* return on first byte */
        to.ReadTotalTimeoutMultiplier = 0;
        to.ReadTotalTimeoutConstant   = (DWORD)timeout_ms;
    }
    SetCommTimeouts(sp->h, &to);
}

bool serial_read_line(serial_port_t *sp, char *buf, size_t buflen, int timeout_ms) {
    if (!serial_is_open(sp) || !buf || buflen == 0) return false;
    buf[0] = '\0';

    /* If we already have a complete line in the line-buffer, drain it. */
    for (size_t i = 0; i < sp->read_pos; i++) {
        if (sp->read_buf[i] == '\n') {
            size_t copy = (i < buflen - 1) ? i : buflen - 1;
            memcpy(buf, sp->read_buf, copy);
            buf[copy] = '\0';
            if (copy > 0 && buf[copy - 1] == '\r') buf[copy - 1] = '\0';
            memmove(sp->read_buf, sp->read_buf + i + 1, sp->read_pos - i - 1);
            sp->read_pos -= i + 1;
            return true;
        }
    }

    /* Read in chunks until we see '\n' or time out. */
    DWORD elapsed = 0;
    DWORD start   = GetTickCount();
    while (sp->read_pos < READ_BUF_SIZE - 1) {
        int remaining = (timeout_ms < 0) ? -1
                      : (int)(timeout_ms - (GetTickCount() - start));
        if (timeout_ms >= 0 && remaining <= 0) break;

        set_read_timeout(sp, remaining);
        DWORD got = 0;
        BOOL ok = ReadFile(sp->h, sp->read_buf + sp->read_pos,
                           (DWORD)(READ_BUF_SIZE - 1 - sp->read_pos), &got, NULL);
        if (!ok) return false;
        if (got == 0) break;

        /* Look for newline in the freshly-read bytes. */
        for (size_t i = sp->read_pos; i < sp->read_pos + got; i++) {
            if (sp->read_buf[i] == '\n') {
                size_t copy = (i < buflen - 1) ? i : buflen - 1;
                memcpy(buf, sp->read_buf, copy);
                buf[copy] = '\0';
                if (copy > 0 && buf[copy - 1] == '\r') buf[copy - 1] = '\0';
                size_t consumed = i + 1;
                size_t total = sp->read_pos + got;
                memmove(sp->read_buf, sp->read_buf + consumed, total - consumed);
                sp->read_pos = total - consumed;
                return true;
            }
        }
        sp->read_pos += got;
        elapsed = GetTickCount() - start;
        if (timeout_ms >= 0 && elapsed >= (DWORD)timeout_ms) break;
    }
    return false;
}

int serial_read_bytes(serial_port_t *sp, void *buf, size_t maxlen, int timeout_ms) {
    if (!serial_is_open(sp) || !buf || maxlen == 0) return -1;

    /* Drain the line-buffer first if it has anything. */
    if (sp->read_pos > 0) {
        size_t take = (sp->read_pos < maxlen) ? sp->read_pos : maxlen;
        memcpy(buf, sp->read_buf, take);
        if (take < sp->read_pos)
            memmove(sp->read_buf, sp->read_buf + take, sp->read_pos - take);
        sp->read_pos -= take;
        return (int)take;
    }

    set_read_timeout(sp, timeout_ms);
    DWORD got = 0;
    if (!ReadFile(sp->h, buf, (DWORD)maxlen, &got, NULL)) return -1;
    return (int)got;
}

bool serial_command(serial_port_t *sp, const char *cmd, char *resp, size_t resp_len, int timeout_ms) {
    if (!serial_send_line(sp, cmd)) return false;
    return serial_read_line(sp, resp, resp_len, timeout_ms);
}

void serial_flush(serial_port_t *sp) {
    if (!serial_is_open(sp)) return;
    PurgeComm(sp->h, PURGE_RXCLEAR);
    sp->read_pos = 0;
}
