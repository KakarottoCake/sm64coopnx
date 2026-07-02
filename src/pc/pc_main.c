#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>

#include "sm64.h"

#include "pc/lua/smlua.h"
#include "pc/lua/utils/smlua_text_utils.h"
#include "game/memory.h"
#include "audio/data.h"
#include "audio/external.h"

#include "network/network.h"
#include "lua/smlua.h"

#include "rom_assets.h"
#include "rom_checker.h"
#include "pc_main.h"
#include "loading.h"
#include "cliopts.h"
#include "configfile.h"
#include "thread.h"
#include "controller/controller_api.h"
#include "controller/controller_keyboard.h"
#include "controller/controller_mouse.h"
#include "fs/fs.h"

#include "game/display.h" // for gGlobalTimer
#include "game/game_init.h"
#include "game/main.h"
#include "game/rumble_init.h"

#include "pc/lua/utils/smlua_audio_utils.h"

#include "pc/network/version.h"
#include "pc/network/socket/socket.h"
#include "pc/network/network_player.h"
#include "pc/update_checker.h"
#include "pc/djui/djui.h"
#include "pc/djui/djui_unicode.h"
#include "pc/djui/djui_panel.h"
#include "pc/djui/djui_panel_modlist.h"
#include "pc/djui/djui_ctx_display.h"
#include "pc/djui/djui_fps_display.h"
#include "pc/djui/djui_lua_profiler.h"
#include "pc/debuglog.h"
#include "pc/utils/misc.h"
#include "pc/mods/mods.h"

#include "debug_context.h"
#include "menu/intro_geo.h"

#include "gfx_dimensions.h"
#include "game/segment2.h"

#include "engine/math_util.h"

#ifdef DISCORD_SDK
#include "pc/discord/discord.h"
#endif

#include "pc/mumble/mumble.h"

#if defined(_WIN32)
#include <windows.h>
#endif

#ifndef __SWITCH__
#include <SDL2/SDL.h>
#endif

extern Vp gViewportFullscreen;

OSMesg D_80339BEC;
OSMesgQueue gSIEventMesgQueue;

s8 gResetTimer;
s8 D_8032C648;
s8 gDebugLevelSelect;
s8 gShowProfiler;
s8 gShowDebugText;

s32 gRumblePakPfs;
u32 gNumVblanks = 0;

u8 gRenderingInterpolated = 0;
f32 gRenderingDelta = 0;
f32 gFramePercentage = 0.f;

#define FRAMERATE 30
static const f64 sFrameTime = (1.0 / ((double)FRAMERATE));
static f64 sFpsTimeLast = 0;
static f64 sFrameTimeStart = 0;
static u32 sDrawnFrames = 0;

bool gGameInited = false;
bool gGfxInited = false;

f32 gMasterVolume;

u8 gLuaVolumeMaster = 127;
u8 gLuaVolumeLevel = 127;
u8 gLuaVolumeSfx = 127;
u8 gLuaVolumeEnv = 127;

#ifdef __SWITCH__
bool gNxLogGfxRunOnce = false;
#endif

struct AudioAPI* gAudioApi = &audio_null;
struct GfxWindowManagerAPI* gWindowApi = &gfx_dummy_wm_api;
struct GfxRenderingAPI* gRenderApi = &gfx_dummy_renderer_api;

extern void gfx_run(Gfx *commands);
extern void thread5_game_loop(void *arg);
extern void create_next_audio_buffer(s16 *samples, u32 num_samples);
void game_loop_one_iteration(void);

void dispatch_audio_sptask(UNUSED struct SPTask *spTask) {}
void set_vblank_handler(UNUSED s32 index, UNUSED struct VblankHandler *handler, UNUSED OSMesgQueue *queue, UNUSED OSMesg *msg) {}

void send_display_list(struct SPTask *spTask) {
    if (!gGameInited) { return; }
    gfx_run((Gfx *)spTask->task.t.data_ptr);
}

#ifdef VERSION_EU
#define SAMPLES_HIGH 560 // gAudioBufferParameters.maxAiBufferLength
#define SAMPLES_LOW 528 // gAudioBufferParameters.minAiBufferLength
#else
#define SAMPLES_HIGH 544
#define SAMPLES_LOW 528
#endif

extern void patch_mtx_before(void);
extern void patch_screen_transition_before(void);
extern void patch_title_screen_before(void);
extern void patch_dialog_before(void);
extern void patch_hud_before(void);
extern void patch_paintings_before(void);
extern void patch_bubble_particles_before(void);
extern void patch_snow_particles_before(void);
extern void patch_djui_before(void);
extern void patch_djui_hud_before(void);
extern void patch_scroll_targets_before(void);

extern void patch_mtx_interpolated(f32 delta);
extern void patch_screen_transition_interpolated(f32 delta);
extern void patch_title_screen_interpolated(f32 delta);
extern void patch_dialog_interpolated(f32 delta);
extern void patch_hud_interpolated(f32 delta);
extern void patch_paintings_interpolated(f32 delta);
extern void patch_bubble_particles_interpolated(f32 delta);
extern void patch_snow_particles_interpolated(f32 delta);
extern void patch_djui_interpolated(f32 delta);
extern void patch_djui_hud(f32 delta);
extern void patch_scroll_targets_interpolated(f32 delta);

static void patch_interpolations_before(void) {
    patch_mtx_before();
    patch_screen_transition_before();
    patch_title_screen_before();
    patch_dialog_before();
    patch_hud_before();
    patch_paintings_before();
    patch_bubble_particles_before();
    patch_snow_particles_before();
    patch_djui_before();
    patch_djui_hud_before();
    patch_scroll_targets_before();
}

static inline void patch_interpolations(f32 delta) {
    patch_mtx_interpolated(delta);
    patch_screen_transition_interpolated(delta);
    patch_title_screen_interpolated(delta);
    patch_dialog_interpolated(delta);
    patch_hud_interpolated(delta);
    patch_paintings_interpolated(delta);
    patch_bubble_particles_interpolated(delta);
    patch_snow_particles_interpolated(delta);
    patch_djui_interpolated(delta);
    patch_djui_hud(delta);
    patch_scroll_targets_interpolated(delta);
}

static void compute_fps(f64 curTime) {
    u32 fps = round((f64) sDrawnFrames / MAX(0.001, curTime - sFpsTimeLast));
    djui_fps_display_update(fps);
    sFpsTimeLast = curTime;
    sDrawnFrames = 0;
}

static s32 get_num_frames_to_draw(f64 t, u32 frameLimit) {
    if (frameLimit % FRAMERATE == 0) {
        return frameLimit / FRAMERATE;
    }
    s64 numFramesCurr = (s64) (t * (f64) frameLimit);
    s64 numFramesNext = (s64) ((t + sFrameTime) * (f64) frameLimit);
    return (s32) MAX(1, numFramesNext - numFramesCurr);
}

static u32 get_display_refresh_rate(void) {
#ifdef __SWITCH__
    return 60;
#else
    static u32 refreshRate = 0;
    if (!refreshRate) {
        SDL_DisplayMode mode;
        if (SDL_GetCurrentDisplayMode(0, &mode) == 0) {
            if (mode.refresh_rate > 0) { refreshRate = (u32) mode.refresh_rate; }
        } else {
            refreshRate = 60;
        }
    }
    return refreshRate;
#endif
}

static u32 get_target_refresh_rate(void) {
    if (configFramerateMode == RRM_MANUAL) { return configFrameLimit; }
    if (configFramerateMode == RRM_UNLIMITED) { return 3000; } // Has no effect
    return get_display_refresh_rate();
}

static void select_graphics_backend(void) {
    if (gCLIOpts.headless) {
        return;
    }

#if defined(_WIN32)
    if (configGraphicsBackend == GAPI_GL && !gfx_sdl_check_opengl_compatibility()) {
        configGraphicsBackend = GAPI_D3D11;
    }
#endif
    int backend = configGraphicsBackend;
#if defined(_WIN32)
    if (gCLIOpts.backend != -1) { backend = gCLIOpts.backend; }
#endif

    switch (backend) {
        case GAPI_GL:
            gWindowApi = &gfx_sdl;
            gRenderApi = &gfx_opengl_api;
            gAudioApi  = &audio_sdl;
            break;
#if defined(_WIN32)
        case GAPI_D3D11:
            gWindowApi = &gfx_dxgi;
            gRenderApi = &gfx_direct3d11_api;
            gAudioApi  = &audio_sdl;
            break;
#endif
        default:
            gWindowApi = &gfx_sdl;
            gRenderApi = &gfx_opengl_api;
            gAudioApi  = &audio_sdl;
            break;
    }

    if (!gAudioApi->init()) {
        gAudioApi = &audio_null;
    }
}

void produce_interpolation_frames_and_delay(void) {
    u32 refreshRate = get_target_refresh_rate();

    gRenderingInterpolated = true;

    u32 displayRefreshRate = get_display_refresh_rate();
    bool shouldDelay = configFramerateMode != RRM_UNLIMITED;
    if (configWindow.vsync && displayRefreshRate <= refreshRate) {
        shouldDelay = false;
        refreshRate = displayRefreshRate;
    }

    f64 targetTime = sFrameTimeStart + sFrameTime;
    s32 numFramesToDraw = get_num_frames_to_draw(sFrameTimeStart, refreshRate);

    f64 curTime = clock_elapsed_f64();
    f64 loopStartTime = curTime;
    f64 expectedTime = 0;
    u16 framesDrawn = 0;
    const f64 interpFrameTime = sFrameTime / (f64) numFramesToDraw;

#ifdef __SWITCH__
    void nx_checkpoint(const char* label);
    static bool sLoggedFirstInterp = false;
    bool logFirst = !sLoggedFirstInterp;
    sLoggedFirstInterp = true;
    if (logFirst) nx_checkpoint("produce_interpolation_frames_and_delay: start");
#endif

    // interpolate and render
    // make sure to draw at least one frame to prevent the game from freezing completely
    // (including inputs and window events) if the game update duration is greater than 33ms
    do {
        curTime = clock_elapsed_f64();
        ++framesDrawn;

        // when we know how many frames to draw, use a precise delta
        f64 idealTime = shouldDelay ? (sFrameTimeStart + interpFrameTime * framesDrawn) : curTime;
        f32 delta = clamp((idealTime - sFrameTimeStart) / sFrameTime, 0.f, 1.f);
        gFramePercentage = clamp((curTime - sFrameTimeStart) / sFrameTime, 0.f, 1.f);
        gRenderingDelta = delta;

        gfx_start_frame();
#ifdef __SWITCH__
        if (logFirst) nx_checkpoint("produce_interpolation_frames_and_delay: gfx_start_frame done");
#endif
        if (!gSkipInterpolationTitleScreen) { patch_interpolations(delta); }
#ifdef __SWITCH__
        if (logFirst) nx_checkpoint("produce_interpolation_frames_and_delay: patch_interpolations done");
        extern bool gNxLogGfxRunOnce;
        if (logFirst) gNxLogGfxRunOnce = true;
#endif
        send_display_list(gGfxSPTask);
#ifdef __SWITCH__
        if (logFirst) nx_checkpoint("produce_interpolation_frames_and_delay: send_display_list done");
#endif
        gfx_end_frame_render();
#ifdef __SWITCH__
        if (logFirst) nx_checkpoint("produce_interpolation_frames_and_delay: gfx_end_frame_render done");
#endif
        gfx_display_frame();
#ifdef __SWITCH__
        if (logFirst) nx_checkpoint("produce_interpolation_frames_and_delay: gfx_display_frame done");
#endif

        // delay if our framerate is capped
        if (shouldDelay) {
            expectedTime += (targetTime - curTime) / (f64) numFramesToDraw;
            f64 now = clock_elapsed_f64();
            f64 elapsedTime = now - loopStartTime;
            f64 delay = (expectedTime - elapsedTime);
            if (delay > 0.0) {
                precise_delay_f64(delay);
            }
        }

        sDrawnFrames++;
        if (shouldDelay) { numFramesToDraw--; }
    } while ((curTime = clock_elapsed_f64()) < targetTime && numFramesToDraw > 0);

    // compute and update the frame rate every second
    if ((curTime = clock_elapsed_f64()) >= sFpsTimeLast + 1.0) {
        compute_fps(curTime);
    }

    // advance frame start time
    if (curTime > sFrameTimeStart + 2 * sFrameTime) {
        sFrameTimeStart = curTime;
    } else {
        sFrameTimeStart += sFrameTime;
    }

    gRenderingInterpolated = false;
}

// It's just better to have this off the stack, Because the size isn't small.
// It also may help static analysis and bug catching.
static s16 sAudioBuffer[SAMPLES_HIGH * 2 * 2] = { 0 };

inline static void buffer_audio(void) {
    bool shouldMute = (configMuteFocusLoss && !gWindowApi->has_focus()) || (gMasterVolume == 0);
    if (!shouldMute) {
        set_sequence_player_volume(SEQ_PLAYER_LEVEL, (f32)configMusicVolume / 127.0f * (f32)gLuaVolumeLevel / 127.0f);
        set_sequence_player_volume(SEQ_PLAYER_SFX,   (f32)configSfxVolume / 127.0f * (f32)gLuaVolumeSfx / 127.0f);
        set_sequence_player_volume(SEQ_PLAYER_ENV,   (f32)configEnvVolume / 127.0f * (f32)gLuaVolumeEnv / 127.0f);
    }

    int samplesLeft = gAudioApi->buffered();
    u32 numAudioSamples = samplesLeft < gAudioApi->get_desired_buffered() ? SAMPLES_HIGH : SAMPLES_LOW;
    for (s32 i = 0; i < 2; i++) {
        create_next_audio_buffer(sAudioBuffer + i * (numAudioSamples * 2), numAudioSamples);
    }

    if (!shouldMute) {
        for (u16 i=0; i < ARRAY_COUNT(sAudioBuffer); i++) {
            sAudioBuffer[i] *= gMasterVolume;
        }
        gAudioApi->play((u8 *)sAudioBuffer, 2 * numAudioSamples * 4);
    }
}

void *audio_thread(UNUSED void *arg) {
    // As long as we have an audio api and that we're threaded, Loop.
    while (gAudioApi) {
        f64 curTime = clock_elapsed_f64();

        // Buffer the audio.
        lock_mutex(&gAudioThread);
        buffer_audio();
        unlock_mutex(&gAudioThread);

        // Delay till the next frame for smooth audio at the correct speed.
        // delay
        f64 targetDelta = 1.0 / (f64)FRAMERATE;
        f64 now = clock_elapsed_f64();
        f64 actualDelta = now - curTime;
        if (actualDelta < targetDelta) {
            f64 delay = ((targetDelta - actualDelta) * 1000.0);
            gWindowApi->delay((u32)delay);
        }
    }

    // Exit the thread if our loop breaks.
    exit_thread();

    return NULL;
}

#ifdef __SWITCH__
// Diagnostic-only: used to trace a since-fixed freeze after hosting by
// logging N frames in detail (each nx_checkpoint() call opens/writes/closes
// a file on the SD card, so leaving this on is a serious perf hit - do not
// re-enable for routine use).
int gNxLogHostFrames = 0;
void nx_start_host_frame_logging(void) { gNxLogHostFrames = 0; }
#endif

void produce_one_frame(void) {
#ifdef __SWITCH__
    void nx_checkpoint(const char* label);
    static bool sLoggedFirstFrame = false;
    bool logFirst = !sLoggedFirstFrame || gNxLogHostFrames > 0;
    sLoggedFirstFrame = true;
    if (gNxLogHostFrames > 0) gNxLogHostFrames--;
    if (logFirst) nx_checkpoint("produce_one_frame: start");
#endif
    CTX_EXTENT(CTX_NETWORK, network_update);
#ifdef __SWITCH__
    if (logFirst) nx_checkpoint("produce_one_frame: network_update done");
#endif

    CTX_EXTENT(CTX_INTERP, patch_interpolations_before);
#ifdef __SWITCH__
    if (logFirst) nx_checkpoint("produce_one_frame: patch_interpolations_before done");
#endif

    CTX_EXTENT(CTX_GAME_LOOP, game_loop_one_iteration);
#ifdef __SWITCH__
    if (logFirst) nx_checkpoint("produce_one_frame: game_loop_one_iteration done");
#endif

    CTX_EXTENT(CTX_SMLUA, smlua_update);
#ifdef __SWITCH__
    if (logFirst) nx_checkpoint("produce_one_frame: smlua_update done");
#endif

    // If we aren't threaded
    if (gAudioThread.state == INVALID) {
        CTX_EXTENT(CTX_AUDIO, buffer_audio);
    }
#ifdef __SWITCH__
    if (logFirst) nx_checkpoint("produce_one_frame: buffer_audio done");
#endif

    CTX_EXTENT(CTX_RENDER, produce_interpolation_frames_and_delay);
#ifdef __SWITCH__
    if (logFirst) nx_checkpoint("produce_one_frame: produce_interpolation_frames_and_delay done");
#endif
}

// used for rendering 2D scenes fullscreen like the loading or crash screens
void produce_one_dummy_frame(void (*callback)(), u8 clearColorR, u8 clearColorG, u8 clearColorB) {
    // measure frame start time
    f64 frameStart = clock_elapsed_f64();
    f64 targetFrameTime = 1.0 / 60.0; // update at 60fps

    // start frame
    gfx_start_frame();
    config_gfx_pool();
    init_render_image();
    create_dl_ortho_matrix();
    djui_gfx_displaylist_begin();

    // fix scaling issues
    gSPViewport(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(&gViewportFullscreen));
    gDPSetScissor(gDisplayListHead++, G_SC_NON_INTERLACE, 0, BORDER_HEIGHT, SCREEN_WIDTH, SCREEN_HEIGHT - BORDER_HEIGHT);

    // clear screen
    create_dl_translation_matrix(MENU_MTX_PUSH, GFX_DIMENSIONS_FROM_LEFT_EDGE(0), 240.f, 0.f);
    create_dl_scale_matrix(MENU_MTX_NOPUSH, (GFX_DIMENSIONS_ASPECT_RATIO * SCREEN_HEIGHT) / 130.f, 3.f, 1.f);
    gDPSetEnvColor(gDisplayListHead++, clearColorR, clearColorG, clearColorB, 0xFF);
    gSPDisplayList(gDisplayListHead++, dl_draw_text_bg_box);
    gSPPopMatrix(gDisplayListHead++, G_MTX_MODELVIEW);

    // call the callback
    callback();

    // render frame
    djui_gfx_displaylist_end();
    end_master_display_list();
    alloc_display_list(0);
    gfx_run((Gfx*) gGfxSPTask->task.t.data_ptr); // send_display_list
    display_and_vsync();

    // delay to go easy on the cpu
    f64 frameEnd = clock_elapsed_f64();
    f64 elapsed = frameEnd - frameStart;
    f64 remaining = targetFrameTime - elapsed;
    if (remaining > 0) {
        gWindowApi->delay((u32)(remaining * 1000.0));
    }

    gfx_end_frame();
}

void audio_shutdown(void) {
    if (gAudioApi) {
        if (gAudioApi->shutdown) gAudioApi->shutdown();
        gAudioApi = NULL;
    }
}

void game_deinit(void) {
    if (gGameInited) { configfile_save(configfile_name()); }
    controller_shutdown();
    audio_shutdown();
    network_shutdown(true, true, false, false);
    smlua_text_utils_shutdown();
    smlua_shutdown();
    mods_shutdown();
    djui_shutdown();
    gfx_shutdown();
    gGameInited = false;
}

void game_exit(void) {
    LOG_INFO("exiting cleanly");
    game_deinit();
    exit(0);
}

#ifdef __SWITCH__
void nx_checkpoint(const char* label);
#endif

void* main_game_init(UNUSED void* dummy) {
#ifdef __SWITCH__
    nx_checkpoint("main_game_init: start");
#endif
    // load language
    if (!djui_language_init(configLanguage)) { snprintf(configLanguage, MAX_CONFIG_STRING, "%s", ""); }
#ifdef __SWITCH__
    nx_checkpoint("main_game_init: djui_language_init done");
#endif

    LOADING_SCREEN_MUTEX(loading_screen_set_segment_text("Loading"));
    dynos_gfx_init();
#ifdef __SWITCH__
    nx_checkpoint("main_game_init: dynos_gfx_init done");
#endif
    enable_queued_dynos_packs();
#ifdef __SWITCH__
    nx_checkpoint("main_game_init: enable_queued_dynos_packs done");
#endif
    sync_objects_init_system();
#ifdef __SWITCH__
    nx_checkpoint("main_game_init: sync_objects_init_system done");
#endif

#ifndef __SWITCH__
    // no self-update mechanism on Switch - the .nro is built locally, and
    // there's nothing to check/download over the network for it
    if (gCLIOpts.network != NT_SERVER && !gCLIOpts.skipUpdateCheck) {
        check_for_updates();
    }
#endif
#ifdef __SWITCH__
    nx_checkpoint("main_game_init: check_for_updates done");
#endif

    LOADING_SCREEN_MUTEX(loading_screen_set_segment_text("Loading ROM Assets"));
    rom_assets_load();
#ifdef __SWITCH__
    nx_checkpoint("main_game_init: rom_assets_load done");
#endif
    smlua_text_utils_init();
#ifdef __SWITCH__
    nx_checkpoint("main_game_init: smlua_text_utils_init done");
#endif

    mods_init();
#ifdef __SWITCH__
    nx_checkpoint("main_game_init: mods_init done");
#endif
    enable_queued_mods();
#ifdef __SWITCH__
    nx_checkpoint("main_game_init: enable_queued_mods done");
#endif
    LOADING_SCREEN_MUTEX(
        gCurrLoadingSegment.percentage = 0;
        loading_screen_set_segment_text("Starting Game");
    );

    audio_init();
#ifdef __SWITCH__
    nx_checkpoint("main_game_init: audio_init done");
#endif
    sound_init();
#ifdef __SWITCH__
    nx_checkpoint("main_game_init: sound_init done");
#endif
    network_player_init();
#ifdef __SWITCH__
    nx_checkpoint("main_game_init: network_player_init done");
#endif
    mumble_init();
#ifdef __SWITCH__
    nx_checkpoint("main_game_init: mumble_init done");
#endif

    gGameInited = true;
    return NULL;
}

#ifdef __SWITCH__
// bisecting where boot stops; each checkpoint appends one line, so the log
// itself shows exactly how far execution got before stopping
void nx_checkpoint(const char* label) {
    static bool sOpened = false;
    FILE* f = fopen("sdmc:/sm64coopdx_boot.log", sOpened ? "a" : "w");
    sOpened = true;
    if (!f) { return; }
    fprintf(f, "[boot] %s\n", label);
    fclose(f);
}

// Separate from nx_checkpoint/ldn_log - this traces every packet in/out plus
// the join/mod-list state machine, to diagnose exactly where an LDN session
// stalls after the radio-level connection succeeds.
#include <stdarg.h>
void nx_packet_log(const char* fmt, ...) {
    // Once actually connected, normal gameplay sends/receives a packet
    // almost every frame - logging each one (a full SD-card file
    // open/write/close per call) was fine for tracing the one-time connect/
    // mod-list/join handshake, but under sustained gameplay traffic this
    // I/O volume is enough to freeze the game. Cap it: plenty to capture a
    // full join sequence, cuts off well before steady-state traffic begins.
    static int sCallsRemaining = 2000;
    if (sCallsRemaining <= 0) { return; }
    static bool sOpened = false;
    FILE* f = fopen("sdmc:/sm64coopdx_packet.log", sOpened ? "a" : "w");
    sOpened = true;
    sCallsRemaining--;

    if (!f) { return; }
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fprintf(f, "\n");
    fclose(f);
}
#endif

#ifdef __SWITCH__
// nx-hbloader gives the process's initial thread a small stack, which this
// codebase's rendering path (deep SM64 decomp call chains + the Mesa
// nouveau GL driver) overflows on the very first rendered frame - every
// test run reached the exact same point (right before
// produce_interpolation_frames_and_delay's first GL calls) and died with no
// checkpoint reached inside it. Fix: run the entire game on a dedicated
// thread with a much larger, explicitly-sized stack instead of the default
// one; the original hbloader-provided thread just waits on it.
static int sm64_main(int argc, char *argv[]);
extern int nx_run_on_big_stack_thread(void (*entry)(void*));

static int sArgc;
static char** sArgv;

static void nx_main_thread_entry(void* arg) {
    (void)arg;
    sm64_main(sArgc, sArgv);
}

extern void nx_register_exit_hook(void (*callback)(void));

int main(int argc, char *argv[]) {
    sArgc = argc;
    sArgv = argv;

    if (!nx_run_on_big_stack_thread(nx_main_thread_entry)) {
        nx_checkpoint("FATAL: big-stack thread creation failed");
    }
    return 0;
}

static int sm64_main(int argc, char *argv[]) {
#else
int main(int argc, char *argv[]) {
#endif

#ifdef __SWITCH__
    // NOTE: do NOT use libnx's consoleInit()/printf-to-screen debug console
    // here or anywhere else in this codebase - consoleInit() hijacks
    // stdout globally via newlib's device table, and consoleExit() doesn't
    // fully undo that, so any later printf() (this codebase calls it
    // constantly, e.g. configfile_save(), LOG_INFO/LOG_ERROR) crashes into
    // freed console state. Confirmed root cause of an earlier crash here.
    nx_checkpoint("main() entered");
    // must happen before anything calls sys_user_path()/sys_exe_path_dir()
    // (fs_init below is the first such call)
    sys_switch_set_argv0(argc > 0 ? argv[0] : NULL);
    nx_checkpoint("argv0 set");
    nx_register_exit_hook(game_exit);
    nx_checkpoint("exit hook registered");
#endif

    // handle terminal arguments
    if (!parse_cli_opts(argc, argv)) { return 0; }
#ifdef __SWITCH__
    nx_checkpoint("cli opts parsed");
#endif

#ifdef _WIN32
    // handle Windows console
    if (gCLIOpts.console || gCLIOpts.headless) {
        SetConsoleOutputCP(CP_UTF8);
    } else {
        FreeConsole();
        freopen("NUL", "w", stdout);
    }
#endif

#ifdef _WIN32
    if (gCLIOpts.savePath[0]) {
        char portable_path[SYS_MAX_PATH] = {};
        sys_windows_short_path_from_mbs(portable_path, SYS_MAX_PATH, gCLIOpts.savePath);
        fs_init(portable_path);
    } else {
        fs_init(sys_user_path());
    }
#else
    fs_init(gCLIOpts.savePath[0] ? gCLIOpts.savePath : sys_user_path());
#endif
#ifdef __SWITCH__
    nx_checkpoint("fs_init done");
#endif

    configfile_load();
#ifdef __SWITCH__
    nx_checkpoint("configfile_load done");
#endif

    legacy_folder_handler();
#ifdef __SWITCH__
    nx_checkpoint("legacy_folder_handler done");
#endif

    select_graphics_backend();
#ifdef __SWITCH__
    nx_checkpoint("select_graphics_backend done");
#endif

#ifdef __SWITCH__
    nx_checkpoint("before gfx_init");
#endif
    // create the window almost straight away
    if (!gGfxInited) {
        gfx_init(gWindowApi, gRenderApi, TITLE);
#ifdef __SWITCH__
        nx_checkpoint("gfx_init returned");
#endif
        gWindowApi->set_keyboard_callbacks(keyboard_on_key_down, keyboard_on_key_up, keyboard_on_all_keys_up,
            keyboard_on_text_input, keyboard_on_text_editing);
        gWindowApi->set_scroll_callback(mouse_on_scroll);
    }

#ifdef __SWITCH__
    // rom_assets_load() (called later from main_game_init) loads textures,
    // vertex data, sounds, animations, etc. at runtime from gRomFilename -
    // that file is never baked in, so we still need a real baserom on the
    // SD card. main_rom_handler() scans the user save path and the exe's
    // own directory (sys_exe_path_dir()) for a valid, MD5-verified .z64 and
    // sets gRomFilename/gRomIsValid. There's no GUI/mouse-driven setup
    // screen on Switch, so just fail loudly to the boot log if none found.
    if (!main_rom_handler()) {
        nx_checkpoint("FATAL: no valid baserom.*.z64 found next to the .nro or in save dir");
        return 0;
    }
#else
    // render the rom setup screen
    if (!main_rom_handler()) {
        if (!gCLIOpts.hideLoadingScreen) {
            render_rom_setup_screen(); // holds the game load until a valid rom is provided
        } else {
            printf("ERROR: could not find valid vanilla us sm64 rom in game's user folder\n");
            return 0;
        }
    }
#endif

    // start the thread for setting up the game
#ifdef __SWITCH__
    nx_checkpoint("main(): before starting loading thread");
#endif
    bool threadSuccess = false;
    if (!gCLIOpts.hideLoadingScreen && !gCLIOpts.headless) {
        if (init_thread_handle(&gLoadingThread, main_game_init, NULL, NULL, 0) == 0) {
#ifdef __SWITCH__
            nx_checkpoint("main(): loading thread started, entering render_loading_screen");
#endif
            render_loading_screen(); // render the loading screen while the game is setup
            threadSuccess = true;
            destroy_mutex(&gLoadingThread);
        }
    }
#ifdef __SWITCH__
    nx_checkpoint("main(): threadSuccess check done");
#endif
    if (!threadSuccess) {
        main_game_init(NULL); // failsafe incase threading doesn't work
    }
#ifdef __SWITCH__
    nx_checkpoint("main(): main_game_init returned");
#endif

    // initialize sm64 data and controllers
    thread5_game_loop(NULL);
#ifdef __SWITCH__
    nx_checkpoint("main(): thread5_game_loop done");
#endif

    // Initialize the audio thread if possible.
    // init_thread_handle(&gAudioThread, audio_thread, NULL, NULL, 0);

    loading_screen_reset();
#ifdef __SWITCH__
    nx_checkpoint("main(): loading_screen_reset done");
#endif

    // initialize djui
    djui_init();
#ifdef __SWITCH__
    nx_checkpoint("main(): djui_init done");
#endif
    djui_unicode_init();
#ifdef __SWITCH__
    nx_checkpoint("main(): djui_unicode_init done");
#endif
    djui_init_late();
#ifdef __SWITCH__
    nx_checkpoint("main(): djui_init_late done");
#endif
    djui_console_message_dequeue();
#ifdef __SWITCH__
    nx_checkpoint("main(): djui_console_message_dequeue done");
#endif

#ifndef __SWITCH__
    show_update_popup();

    if (can_update_game()) {
        djui_open_update_panel();
    }
#endif
#ifdef __SWITCH__
    nx_checkpoint("main(): show_update_popup done");
#endif
#ifdef __SWITCH__
    nx_checkpoint("main(): can_update_game/djui_open_update_panel done");
#endif

    // initialize network
    if (gCLIOpts.network == NT_CLIENT) {
        network_set_system(NS_SOCKET);
        snprintf(gGetHostName, MAX_CONFIG_STRING, "%s", gCLIOpts.joinIp);
        snprintf(configJoinIp, MAX_CONFIG_STRING, "%s", gCLIOpts.joinIp);
        configJoinPort = gCLIOpts.networkPort;
        network_init(NT_CLIENT, false);
    } else if (gCLIOpts.network == NT_SERVER || gCLIOpts.coopnet) {
        if (gCLIOpts.network == NT_SERVER) {
            configNetworkSystem = NS_SOCKET;
            configHostPort = gCLIOpts.networkPort;
        } else {
            configNetworkSystem = NS_COOPNET;
            snprintf(configPassword, MAX_CONFIG_STRING, "%s", gCLIOpts.coopnetPassword);
        }

        // horrible, hacky fix for mods that access marioObj straight away
        // best fix: host with the standard main menu method
        static struct Object sHackyObject = { 0 };
        gMarioStates[0].marioObj = &sHackyObject;

        extern void djui_panel_do_host(bool reconnecting, bool playSound);
        djui_panel_do_host(NULL, false);
    } else {
        network_init(NT_NONE, false);
    }
#ifdef __SWITCH__
    nx_checkpoint("main(): network_init done");
#endif

    // main loop
    while (true) {
        debug_context_reset();
        CTX_BEGIN(CTX_TOTAL);
        gWindowApi->main_loop(produce_one_frame);
#ifdef DISCORD_SDK
        discord_update();
#endif
        mumble_update();
#ifdef DEBUG
        fflush(stdout);
        fflush(stderr);
#endif
        CTX_END(CTX_TOTAL);

#ifdef DEVELOPMENT
        djui_ctx_display_update();
#endif
        djui_lua_profiler_update();
    }

    return 0;
}
