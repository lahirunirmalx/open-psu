/**
 * Win32 backend for the platform shim. Selected by the Makefile when
 * compiling under MinGW-w64 on Windows (also works in MSYS2 MINGW64).
 */

#include "platform.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void pl_sleep_ms(unsigned ms) {
    Sleep(ms);
}

uint64_t pl_now_ms(void) {
    /* GetTickCount64 has ~15 ms resolution but is good enough for the
     * coarse polling intervals we use throughout the codebase. */
    return (uint64_t)GetTickCount64();
}

const char *pl_find_monospace_font(void) {
    /* Probe %WINDIR%/Fonts/<file> for the three monospace fonts that ship
     * with every modern Windows install. Fall back to the literal
     * C:/Windows/Fonts/... path if WINDIR isn't set for some reason. */
    static char chosen[MAX_PATH];
    const char *windir = getenv("WINDIR");
    if (!windir) windir = "C:\\Windows";
    static const char *const names[] = {
        "Fonts\\consola.ttf",      /* Consolas - preinstalled, monospace */
        "Fonts\\cour.ttf",         /* Courier New */
        "Fonts\\lucon.ttf",        /* Lucida Console */
        NULL,
    };
    for (int i = 0; names[i]; i++) {
        snprintf(chosen, sizeof(chosen), "%s\\%s", windir, names[i]);
        FILE *f = fopen(chosen, "rb");
        if (f) { fclose(f); return chosen; }
    }
    return NULL;
}

bool pl_self_exe(char *out, size_t outlen) {
    if (!out || outlen == 0) return false;
    WCHAR wbuf[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, wbuf, MAX_PATH);
    if (n == 0 || n == MAX_PATH) return false;
    int u = WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, out, (int)outlen, NULL, NULL);
    return u > 0;
}

/* Build a single command-line string from exe + argv, quoting any value
 * that contains a space. The launcher's argv values are simple
 * (--driver=…, --view=…, --port=…, --baud=N) so heavy escaping is unneeded. */
static char *join_argv(const char *exe, char *const argv[]) {
    /* Worst-case capacity: each arg can be quoted (+2) and gets a leading
     * space (+1). exe gets quoted (+2). +1 for the trailing NUL. */
    size_t cap = strlen(exe) + 3;
    for (int i = 1; argv[i]; i++) cap += strlen(argv[i]) + 4;
    char *cmd = (char *)malloc(cap);
    if (!cmd) return NULL;

    size_t pos = 0;
    cmd[pos++] = '"';
    pos += (size_t)snprintf(cmd + pos, cap - pos, "%s\"", exe);

    for (int i = 1; argv[i]; i++) {
        bool need_q = strchr(argv[i], ' ') != NULL;
        cmd[pos++] = ' ';
        if (need_q) cmd[pos++] = '"';
        size_t alen = strlen(argv[i]);
        memcpy(cmd + pos, argv[i], alen);
        pos += alen;
        if (need_q) cmd[pos++] = '"';
    }
    cmd[pos] = '\0';
    return cmd;
}

bool pl_spawn(const char *exe, char *const argv[]) {
    if (!exe || !argv) return false;

    char *cmdline = join_argv(exe, argv);
    if (!cmdline) return false;

    int wlen = MultiByteToWideChar(CP_UTF8, 0, cmdline, -1, NULL, 0);
    WCHAR *wcmd = (WCHAR *)malloc((size_t)wlen * sizeof(WCHAR));
    if (!wcmd) { free(cmdline); return false; }
    MultiByteToWideChar(CP_UTF8, 0, cmdline, -1, wcmd, wlen);

    STARTUPINFOW si = { .cb = sizeof(si) };
    PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessW(NULL, wcmd, NULL, NULL, FALSE,
                             CREATE_NEW_CONSOLE, NULL, NULL, &si, &pi);
    if (ok) {
        /* Detach: the parent (launcher) never waits for the child. */
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    free(wcmd);
    free(cmdline);
    return ok != 0;
}
