/**
 * shell.cpp - Dear ImGui + SDL2 + OpenGL 3 host.
 *
 * Why OpenGL3 and not the SDL_Renderer backend? ImGui's SDL_Renderer
 * backend uses SDL_RenderGeometry which only landed in SDL 2.0.17, while
 * Ubuntu 20.04 (and the dev box this is being authored on) ships 2.0.10.
 * The OpenGL3 backend has no SDL-version floor - it just needs any GL
 * context the SDL_GL_* APIs can hand it.
 *
 * One SDL window, one GL context, an ImGui context with viewports
 * enabled so windows dragged out of the launcher become real OS-level
 * windows. The launcher draw routine runs every frame.
 */

#include "shell.h"
#include "instance.h"
#include "launcher_imgui.h"

#include "imgui.h"
#include "imgui_internal.h"   /* DockBuilder API */
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_opengl3.h"

extern "C" {
#include "drivers/registry.h"
}

#include <SDL.h>
#include <SDL_opengl.h>

#include <stdio.h>

namespace {

const char *kGlslVersion = "#version 130";

bool init_sdl_and_gl(SDL_Window **out_win, SDL_GLContext *out_ctx) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "shell: SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }

    /* OpenGL 3.0 with the compatibility-style #version 130 GLSL. ImGui's
     * loader handles whatever GL3.0+ context the driver hands back. */
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                        SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);

    SDL_WindowFlags flags = (SDL_WindowFlags)(SDL_WINDOW_OPENGL |
                                              SDL_WINDOW_RESIZABLE |
                                              SDL_WINDOW_ALLOW_HIGHDPI |
                                              SDL_WINDOW_MAXIMIZED);
    SDL_Window *win = SDL_CreateWindow("Open LabBench",
                                       SDL_WINDOWPOS_CENTERED,
                                       SDL_WINDOWPOS_CENTERED,
                                       1280, 800, flags);
    if (!win) {
        fprintf(stderr, "shell: SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return false;
    }

    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (!ctx) {
        fprintf(stderr, "shell: SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(win);
        SDL_Quit();
        return false;
    }
    SDL_GL_MakeCurrent(win, ctx);
    SDL_GL_SetSwapInterval(1);    /* request vsync; harmless if unavailable */

    *out_win = win;
    *out_ctx = ctx;
    return true;
}

bool init_imgui(SDL_Window *win, SDL_GLContext ctx) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    io.IniFilename = nullptr;

    ImGui::StyleColorsDark();
    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowRounding    = 4.0f;
    style.FrameRounding     = 3.0f;
    style.GrabRounding      = 3.0f;
    style.ScrollbarRounding = 3.0f;
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }

    if (!ImGui_ImplSDL2_InitForOpenGL(win, ctx)) return false;
    if (!ImGui_ImplOpenGL3_Init(kGlslVersion)) return false;
    return true;
}

void shutdown_imgui() {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
}

/**
 * Render the IDE-style dockspace host: a borderless window covering the
 * main viewport, containing one DockSpace. On the first frame we split
 * it into LEFT (the Launcher pane) + CENTER (where instrument windows
 * float by default). Returns the screen rect of the central node so
 * the instance manager knows where to place new windows.
 */
ImVec4 draw_ide_dockspace() {
    ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowViewport(vp->ID);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,   0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(0, 0));

    ImGuiWindowFlags host_flags = ImGuiWindowFlags_NoTitleBar
                                | ImGuiWindowFlags_NoCollapse
                                | ImGuiWindowFlags_NoResize
                                | ImGuiWindowFlags_NoMove
                                | ImGuiWindowFlags_NoBringToFrontOnFocus
                                | ImGuiWindowFlags_NoNavFocus
                                | ImGuiWindowFlags_NoDocking;
    ImGui::Begin("##DockHost", nullptr, host_flags);
    ImGui::PopStyleVar(3);

    ImGuiID dockspace_id = ImGui::GetID("OpenLabBenchDockSpace");

    static ImGuiID center_id = 0;
    static bool    built     = false;
    if (!built) {
        built = true;
        ImGui::DockBuilderRemoveNode(dockspace_id);
        ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspace_id, vp->WorkSize);

        ImGuiID left_id;
        ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Left, 0.22f,
                                    &left_id, &center_id);
        ImGui::DockBuilderDockWindow("Launcher", left_id);
        ImGui::DockBuilderFinish(dockspace_id);
    }

    ImGui::DockSpace(dockspace_id, ImVec2(0, 0), ImGuiDockNodeFlags_None);
    ImGui::End();

    /* Query the live central-node rect (changes when the user resizes
     * the launcher pane). Fallback: just use the main viewport. */
    ImGuiDockNode *central = ImGui::DockBuilderGetNode(center_id);
    if (central) {
        return ImVec4(central->Pos.x, central->Pos.y,
                      central->Size.x, central->Size.y);
    }
    return ImVec4(vp->WorkPos.x, vp->WorkPos.y,
                  vp->WorkSize.x, vp->WorkSize.y);
}

}  // namespace

extern "C" int shell_run_launcher(const char *self_exe,
                                  const char *preload_driver_id,
                                  const char *preload_view_id,
                                  const char *preload_port,
                                  int         preload_baud) {
    SDL_Window   *win = nullptr;
    SDL_GLContext ctx = nullptr;
    if (!init_sdl_and_gl(&win, &ctx)) return 1;

    if (!init_imgui(win, ctx)) {
        if (ctx) SDL_GL_DeleteContext(ctx);
        if (win) SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }

    instance_manager_t mgr;
    instance_manager_init(&mgr);

    /* Preload (CLI direct-launch path). Look up the driver id in both
     * registries to decide PSU vs DMM. Failures are reported but
     * non-fatal - the launcher window still comes up. */
    if (preload_driver_id && preload_view_id && *preload_driver_id && *preload_view_id) {
        bool is_dmm = (dmm_drivers_find(preload_driver_id) != nullptr);
        const char *err = nullptr;
        const char *port = (preload_port && *preload_port) ? preload_port : "-";
        if (!instance_open(&mgr, is_dmm,
                           preload_driver_id, preload_view_id,
                           port, preload_baud, &err)) {
            fprintf(stderr, "shell: preload failed: %s\n", err ? err : "(unknown)");
        }
    }

    launcher_imgui_state state{};
    state.mgr = &mgr;
    launcher_imgui_init(&state, self_exe ? self_exe : "psu_app");

    bool quit = false;
    while (!quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL2_ProcessEvent(&ev);
            if (ev.type == SDL_QUIT) quit = true;
            if (ev.type == SDL_WINDOWEVENT &&
                ev.window.event == SDL_WINDOWEVENT_CLOSE &&
                ev.window.windowID == SDL_GetWindowID(win)) {
                quit = true;
            }
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        ImVec4 c = draw_ide_dockspace();
        mgr.center_x = c.x; mgr.center_y = c.y;
        mgr.center_w = c.z; mgr.center_h = c.w;

        launcher_imgui_draw(&state);
        instance_draw_all(&mgr);
        if (state.want_quit) quit = true;

        ImGui::Render();
        ImGuiIO &io = ImGui::GetIO();
        glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
        glClearColor(0.12f, 0.12f, 0.14f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            SDL_Window   *bk_win = SDL_GL_GetCurrentWindow();
            SDL_GLContext bk_ctx = SDL_GL_GetCurrentContext();
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
            SDL_GL_MakeCurrent(bk_win, bk_ctx);
        }

        SDL_GL_SwapWindow(win);
    }

    instance_manager_destroy(&mgr);
    shutdown_imgui();
    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
