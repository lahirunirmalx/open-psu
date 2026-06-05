/**
 * Open LabBench ImGui shell — C ABI for the C parts of the app.
 *
 * The shell hosts an SDL2 + Dear ImGui window, runs the main loop, draws
 * the launcher, and dispatches each open instance to its ImGui view.
 *
 * Pass a non-NULL driver_id + view_id to preload one instance at startup
 * (used by the CLI direct-launch path `psu_app --driver=… --view=… …`).
 */

#ifndef SHELL_SHELL_H
#define SHELL_SHELL_H

#ifdef __cplusplus
extern "C" {
#endif

int shell_run_launcher(const char *self_exe,
                       const char *preload_driver_id,   /* NULL = no preload */
                       const char *preload_view_id,
                       const char *preload_port,
                       int         preload_baud);       /* <= 0 → factory default */

#ifdef __cplusplus
}
#endif

#endif
