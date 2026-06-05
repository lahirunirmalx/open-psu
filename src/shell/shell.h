/**
 * Open LabBench ImGui shell — C ABI for the C parts of the app.
 *
 * The shell hosts an SDL2 + Dear ImGui window, runs the main loop, and
 * draws the ImGui-based launcher. Phase A: the launcher LAUNCH button
 * still fork/execs psu_app for each opened view (same as before).
 */

#ifndef SHELL_SHELL_H
#define SHELL_SHELL_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Run the ImGui-based launcher. Blocks until the user quits.
 *
 * @param self_exe absolute path of the running psu_app binary, used for
 *                 fork+exec when LAUNCH is clicked.
 * @return 0 on clean exit, non-zero on init failure.
 */
int shell_run_launcher(const char *self_exe);

#ifdef __cplusplus
}
#endif

#endif
