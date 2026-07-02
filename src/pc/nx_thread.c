// Isolated from the rest of the codebase on purpose: this file includes
// <switch.h> directly, which typedefs u64/s64 as `long` - conflicting with
// this project's own PR/ultratypes.h, which typedefs them as `long long`.
// Never include project headers here; communicate across the boundary only
// with plain C types (see src/pc/network/socket/socket_ldn.c for the same
// pattern).
#include <switch.h>

typedef void (*NxThreadEntry)(void*);

// nx-hbloader gives the process's initial thread a small stack that this
// codebase's rendering path (deep SM64 decomp call chains + the Mesa
// nouveau GL driver) overflows on the very first rendered frame. Run the
// caller-supplied entry point on a dedicated thread with a much larger,
// explicitly-sized stack instead, and block until it returns.
int nx_run_on_big_stack_thread(NxThreadEntry entry) {
    Thread t;
    Result rc = threadCreate(&t, entry, NULL, NULL, 8 * 1024 * 1024, 0x2C, -2);
    if (R_FAILED(rc)) {
        return 0;
    }
    threadStart(&t);
    threadWaitForExit(&t);
    threadClose(&t);
    return 1;
}

// If the HOME menu force-closes this app (or a title-override relaunch tool
// kills it) without going through SDL_QUIT, game_exit()/network_shutdown()
// never run - and if an LDN access point/network was open at the time, the
// ldn sysmodule's wireless session is left wedged, causing every subsequent
// ldnInitialize() in ANY later launch to fail until the console reboots.
// Hooking OnExitRequest gives us a chance to run cleanup before the OS tears
// the process down.
static AppletHookCookie sAppletHookCookie;
static void (*sExitCallback)(void) = NULL;

static void nx_applet_hook(AppletHookType hook, void* param) {
    (void)param;
    if (hook == AppletHookType_OnExitRequest && sExitCallback) {
        sExitCallback();
    }
}

void nx_register_exit_hook(void (*callback)(void)) {
    sExitCallback = callback;
    appletHook(&sAppletHookCookie, nx_applet_hook, NULL);
}
