#ifdef __SWITCH__

// Isolated on purpose: this file includes ONLY <switch.h> (libnx) and
// standard headers, never any project header. <switch.h>'s u64/s64
// typedefs (long) conflict with this project's own PR/ultratypes.h u64/s64
// (long long) - same width, but C treats them as distinct types, so the
// two cannot be included in the same translation unit. network.c's LDN
// glue (struct NetworkSystem gNetworkSystemLdn, etc.) calls into the
// plain-C-typed (bool/int/u8/u16/u32, never u64/s64) functions below
// instead of touching libnx types directly.

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <switch.h>

// matches network.h's PACKET_LENGTH; duplicated here rather than included,
// to keep this file's only dependency on libnx headers
#define LDN_PACKET_LENGTH 3000
// matches network_player.h's UNKNOWN_LOCAL_INDEX
#define LDN_UNKNOWN_LOCAL_INDEX ((unsigned char)-1)

extern void network_receive(unsigned char localIndex, void* addr, unsigned char* data, unsigned short dataLength);
extern char configPlayerName[];

static bool sLdnInitialized = false;
static bool sLdnAccessPointOpen = false;
static bool sLdnStationOpen = false;
static bool sLdnConnected = false;
static bool sLdnActionFrameEnabled = false;
static LdnNetworkInfo sLdnNetworkInfo[4];
static int sLdnNetworkCount = 0;

void ldn_shutdown_impl(void);
static void ldn_thread_start(void);
static void ldn_thread_stop(void);

// Every ldnSendActionFrame/ldnRecvActionFrame/ldnGetNetworkInfo call is a
// kernel IPC round trip to the ldn sysmodule. Doing these directly from
// ldn_update_impl()/ldn_send_impl() (called once per game frame, and once
// per outgoing packet, from the main thread) meant every frame paid that
// IPC latency before rendering could continue - this is what caused the
// lag/desync once real gameplay traffic started flowing continuously.
// Instead, a dedicated background thread owns all the actual radio I/O;
// the main thread only ever touches small mutex-protected queues, which is
// cheap. bit0=1 (non-blocking) is no longer needed for the recv call since
// blocking here no longer stalls anything the player can see - but it's
// kept anyway so the thread can still notice sLdnThreadShouldStop promptly
// instead of being stuck inside a blocking syscall.
// A burst right after connecting (radio-level retransmits of the same
// action frame, or several frames' worth of already-queued gameplay traffic
// arriving back to back) can hand the recv thread many packets within a
// single frame's worth of time - a 64-slot queue overflowed within
// milliseconds of connecting in testing, silently dropping the mod-list/join
// handshake and leading to a spurious player-timeout disconnect shortly
// after. 512 slots gives a lot more headroom to absorb a burst before the
// main thread's once-per-frame drain catches up.
#define LDN_QUEUE_CAPACITY 512
// Matches LDN_ACTION_FRAME_MAX_SIZE below (real action frames can never
// exceed this) - much smaller than LDN_PACKET_LENGTH, so bumping
// LDN_QUEUE_CAPACITY way up here doesn't blow up memory usage.
#define LDN_QUEUE_ITEM_MAX_SIZE 0x400

typedef struct {
    unsigned char data[LDN_QUEUE_ITEM_MAX_SIZE];
    unsigned short length;
} LdnQueueItem;

static LdnQueueItem sRecvQueue[LDN_QUEUE_CAPACITY];
static int sRecvHead = 0, sRecvTail = 0, sRecvCount = 0;
static Mutex sRecvMutex;

static LdnQueueItem sSendQueue[LDN_QUEUE_CAPACITY];
static int sSendHead = 0, sSendTail = 0, sSendCount = 0;
static Mutex sSendMutex;

static Thread sLdnThread;
static bool sLdnThreadRunning = false;
static bool sLdnThreadShouldStop = false;

// The ldn sysmodule's IPC session isn't safe for concurrent calls from two
// threads - the background thread (send/recv/getNetworkInfo) and the main
// thread (scan/connect/initialize/shutdown, e.g. the browser panel still
// polling ldn_refresh_scan right after a connect) racing on the same session
// crashed the sysmodule itself (Atmosphere fatal report, Process Name "ldn").
// Every actual ldn* call, on either thread, must hold this while calling.
static Mutex sLdnIpcMutex;

static void ldn_log(const char* fmt, ...) {
    // Per-packet send/recv tracing floods this once real gameplay traffic
    // starts (every frame, x2 for the per-node send loop) - each call is a
    // full SD-card file open/write/close, enough sustained I/O to freeze
    // the game. Cap it: plenty to capture the connect handshake, cuts off
    // before steady-state traffic dominates.
    static int sCallsRemaining = 2000;
    if (sCallsRemaining <= 0) { return; }
    // truncate on this process's first write so stale data from a previous
    // launch doesn't eat into this run's budget before real data appears
    static bool sOpened = false;
    FILE* f = fopen("sdmc:/sm64coopdx_ldn.log", sOpened ? "a" : "w");
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

bool ldn_initialize_impl(bool isServer) {
    ldn_log("[LDN] ldn_initialize_impl: isServer=%d initialized=%d connected=%d stationOpen=%d",
            isServer, sLdnInitialized, sLdnConnected, sLdnStationOpen);

    if (sLdnInitialized && sLdnConnected) { return true; }

    // Switching roles (e.g. tried to Join, then went back and pressed Host,
    // or vice versa) without a clean shutdown in between leaves us in the
    // wrong open state for ldnOpenAccessPoint()/ldnOpenStation() to succeed.
    // Only reset when the role actually mismatches the currently-open state -
    // NOT just because a station happens to already be open, since that's
    // the normal, correct state when re-entering this function as a client
    // (e.g. network_init(NT_CLIENT,...) called right after a scan already
    // opened it) and resetting here would wipe the scan results out from
    // under the caller.
    if (sLdnInitialized && ((isServer && sLdnStationOpen) || (!isServer && sLdnAccessPointOpen))) {
        ldn_log("[LDN] ldn_initialize_impl: stale role state, resetting first");
        ldn_shutdown_impl();
    }

    if (!sLdnInitialized) {
        // If a previous run of this app was killed abruptly (title-override
        // relaunch, HOME-closed without reaching game_exit()'s SDL_QUIT ->
        // network_shutdown() path), the ldn sysmodule's own access-point
        // state can still be winding down even though the kernel already
        // closed our old session's handles. ldnInitialize can transiently
        // fail while that happens - retry briefly instead of giving up
        // immediately.
        Result rc;
        for (int attempt = 0; attempt < 5; attempt++) {
            mutexLock(&sLdnIpcMutex);
            rc = ldnInitialize(LdnServiceType_User);
            mutexUnlock(&sLdnIpcMutex);
            ldn_log("[LDN] ldnInitialize attempt=%d: 0x%x (mod=%04x desc=%04x)", attempt, rc, R_MODULE(rc), R_DESCRIPTION(rc));
            if (R_SUCCEEDED(rc)) { break; }
            svcSleepThread(300000000ULL); // 300ms
        }
        if (R_FAILED(rc)) { return false; }
        sLdnInitialized = true;

        // ldnSendActionFrame/ldnRecvActionFrame (how actual game packets are
        // exchanged once connected) silently do nothing useful without this
        // being enabled first - it must happen here, while still in
        // LdnState_Initialized, before OpenAccessPoint/OpenStation. Missing
        // this was why the radio-level connect could succeed (ldnConnect
        // returning 0x0) while the client sat on "Joining..." forever: the
        // TCP/game-data channel was never actually active.
        LdnActionFrameSettings afSettings;
        memset(&afSettings, 0, sizeof(afSettings));
        afSettings.local_communication_id = -1;
        afSettings.security_mode = (u16)LdnSecurityMode_Product;
        afSettings.passphrase_size = 0x10;
        memset(afSettings.passphrase, 0x42, 0x10);
        mutexLock(&sLdnIpcMutex);
        Result afRc = ldnEnableActionFrame(&afSettings);
        mutexUnlock(&sLdnIpcMutex);
        ldn_log("[LDN] ldnEnableActionFrame: 0x%x (mod=%04x desc=%04x)", afRc, R_MODULE(afRc), R_DESCRIPTION(afRc));
        sLdnActionFrameEnabled = R_SUCCEEDED(afRc);
    }

    if (isServer) {
        // ldnCreateNetwork requires LdnState_AccessPoint, which is only
        // reached via ldnOpenAccessPoint() - calling it straight after
        // ldnInitialize() (still LdnState_Initialized) fails with
        // "Invalid State or state field" (module 203/ldn, description 39).
        if (!sLdnAccessPointOpen) {
            mutexLock(&sLdnIpcMutex);
            Result rc = ldnOpenAccessPoint();
            mutexUnlock(&sLdnIpcMutex);
            ldn_log("[LDN] ldnOpenAccessPoint: 0x%x (mod=%04x desc=%04x)", rc, R_MODULE(rc), R_DESCRIPTION(rc));
            if (R_FAILED(rc)) { return false; }
            sLdnAccessPointOpen = true;
        }

        LdnSecurityConfig sec;
        memset(&sec, 0, sizeof(sec));
        sec.security_mode = (LdnSecurityMode)0;
        sec.passphrase_size = 0x10;
        memset(sec.passphrase, 0x42, 0x10);

        LdnUserConfig user;
        memset(&user, 0, sizeof(user));
        snprintf(user.user_name, sizeof(user.user_name), "%s", configPlayerName);

        LdnNetworkConfig cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.intent_id.local_communication_id = -1;
        cfg.channel = 0;
        cfg.node_count_max = 4;

        mutexLock(&sLdnIpcMutex);
        Result rc = ldnCreateNetwork(&sec, &user, &cfg);
        mutexUnlock(&sLdnIpcMutex);
        ldn_log("[LDN] ldnCreateNetwork: 0x%x (mod=%04x desc=%04x)", rc, R_MODULE(rc), R_DESCRIPTION(rc));
        if (R_FAILED(rc)) {
            // roll back to LdnState_Initialized so a retry starts clean
            mutexLock(&sLdnIpcMutex);
            ldnCloseAccessPoint();
            mutexUnlock(&sLdnIpcMutex);
            sLdnAccessPointOpen = false;
            return false;
        }

        sLdnConnected = true;
        mutexLock(&sLdnIpcMutex);
        ldnSetStationAcceptPolicy(LdnAcceptPolicy_AlwaysAccept);
        mutexUnlock(&sLdnIpcMutex);
        ldn_thread_start();
    } else if (!sLdnStationOpen) {
        mutexLock(&sLdnIpcMutex);
        Result rc = ldnOpenStation();
        mutexUnlock(&sLdnIpcMutex);
        ldn_log("[LDN] ldnOpenStation: 0x%x (mod=%04x desc=%04x)", rc, R_MODULE(rc), R_DESCRIPTION(rc));
        if (R_FAILED(rc)) { return false; }
        sLdnStationOpen = true;
    }

    return true;
}

// The related lp2p service's equivalent send command documents a 0x400
// (1024 byte) max action frame size. Sending an oversized frame previously
// crashed the "ldn" sysmodule itself (Data Abort, confirmed via an
// Atmosphere crash report naming process "ldn" as the crashed process) -
// this is a hard safety net so no caller can ever repeat that, regardless
// of how the data got this large.
#define LDN_ACTION_FRAME_MAX_SIZE 0x400

// Tracks the last seen connected-node count so a change (radio-level link
// loss, detected by the ldn sysmodule itself) gets logged once instead of
// either flooding the log or being invisible - the player-timeout kick in
// network_player.c only logs the eventual symptom (~15-22s later), not
// whether the underlying radio link was still up at the time.
static int sLastConnectedCount = -1;

// A player-timeout kick (network_player.c) only reveals that data stopped
// arriving ~15-22s after the fact, not whether ldnSendActionFrame/
// ldnRecvActionFrame were themselves still succeeding during that window.
// These counters + a periodic heartbeat let us tell apart "the sysmodule
// stopped accepting our IPC calls" from "the IPC calls kept succeeding but
// the other console just never got the radio frames" (the latter would mean
// a real hardware-level drop, not a bug here).
static int sSendOkCount = 0;
static int sSendFailCount = 0;
static int sRecvOkCount = 0;

// Runs on the dedicated LDN thread only. Does the actual ldnGetNetworkInfo +
// per-node ldnSendActionFrame IPC calls for one queued packet.
static void ldn_thread_do_send(unsigned char* data, unsigned short dataLength) {
    LdnNetworkInfo netInfo;
    mutexLock(&sLdnIpcMutex);
    Result infoRc = ldnGetNetworkInfo(&netInfo);
    mutexUnlock(&sLdnIpcMutex);
    if (R_FAILED(infoRc)) {
        ldn_log("[LDN] ldn_thread_do_send: ldnGetNetworkInfo FAILED 0x%x (mod=%04x desc=%04x)", infoRc, R_MODULE(infoRc), R_DESCRIPTION(infoRc));
        return;
    }

    int connectedCount = 0;
    for (s32 i = 0; i < netInfo.node_count; i++) {
        if (netInfo.nodes[i].is_connected) { connectedCount++; }
    }
    if (connectedCount != sLastConnectedCount) {
        ldn_log("[LDN] ldn_thread_do_send: connected node count changed %d -> %d (node_count=%d)", sLastConnectedCount, connectedCount, netInfo.node_count);
        sLastConnectedCount = connectedCount;
    }

    int sentTo = 0;
    for (s32 i = 0; i < netInfo.node_count; i++) {
        if (netInfo.nodes[i].is_connected) {
            // channel MUST be the network's actual operating channel (from
            // ldnGetNetworkInfo, NOT a hardcoded value) - action frames sent
            // on the wrong channel are silently never seen by the other side.
            // flags=0 (blocking) is fine now - this runs on the LDN thread,
            // not the main thread, so blocking here no longer stalls frames.
            mutexLock(&sLdnIpcMutex);
            Result sendRc = ldnSendActionFrame(data, dataLength, netInfo.nodes[i].mac_addr, netInfo.common.bssid, netInfo.common.channel, 0);
            mutexUnlock(&sLdnIpcMutex);
            if (R_FAILED(sendRc)) {
                ldn_log("[LDN] ldn_thread_do_send: ldnSendActionFrame to node[%d] FAILED 0x%x (mod=%04x desc=%04x)", i, sendRc, R_MODULE(sendRc), R_DESCRIPTION(sendRc));
                sSendFailCount++;
            } else {
                sSendOkCount++;
            }
            sentTo++;
        }
    }
    if (sentTo == 0) {
        ldn_log("[LDN] ldn_thread_do_send: WARNING - no connected nodes to send to! node_count=%d", netInfo.node_count);
    }
}

static void ldn_thread_func(void* arg) {
    (void)arg;
    ldn_log("[LDN] ldn_thread_func: started");
    sSendOkCount = 0;
    sSendFailCount = 0;
    sRecvOkCount = 0;
    u64 lastHeartbeatTick = svcGetSystemTick();
    while (!sLdnThreadShouldStop) {
        // every ~2s, log a summary of actual IPC-level send/recv activity -
        // low enough volume to stay within the log budget for a long
        // session, but frequent enough to pinpoint exactly when real IPC
        // traffic stops (vs. keeps succeeding while the other side just
        // never receives it - a real radio-level drop, not a code bug)
        u64 nowTick = svcGetSystemTick();
        if ((nowTick - lastHeartbeatTick) >= (armGetSystemTickFreq() * 2)) {
            ldn_log("[LDN] heartbeat: sendOk=%d sendFail=%d recvOk=%d", sSendOkCount, sSendFailCount, sRecvOkCount);
            sSendOkCount = 0;
            sSendFailCount = 0;
            sRecvOkCount = 0;
            lastHeartbeatTick = nowTick;
        }
        // drain the whole send queue first (each item is a quick IPC call)
        for (;;) {
            if (sLdnThreadShouldStop) { break; }
            LdnQueueItem item;
            bool has = false;
            mutexLock(&sSendMutex);
            if (sSendCount > 0) {
                item = sSendQueue[sSendHead];
                sSendHead = (sSendHead + 1) % LDN_QUEUE_CAPACITY;
                sSendCount--;
                has = true;
            }
            mutexUnlock(&sSendMutex);
            if (!has) { break; }
            ldn_thread_do_send(item.data, item.length);
        }

        // one non-blocking receive attempt per loop iteration, then a short
        // sleep - avoids a tight busy-spin while still checking often enough
        // (this thread is off the render path, so this delay doesn't cost
        // any visible frame time)
        unsigned char recvBuf[LDN_PACKET_LENGTH];
        LdnMacAddress addr0, addr1;
        s16 channel;
        u32 outSize;
        s32 linkLevel;
        mutexLock(&sLdnIpcMutex);
        Result rc = ldnRecvActionFrame(recvBuf, sizeof(recvBuf), &addr0, &addr1, &channel, &outSize, &linkLevel, 1);
        mutexUnlock(&sLdnIpcMutex);
        if (R_SUCCEEDED(rc) && outSize > 0 && outSize <= LDN_QUEUE_ITEM_MAX_SIZE && channel > 0) {
            sRecvOkCount++;
            mutexLock(&sRecvMutex);
            if (sRecvCount < LDN_QUEUE_CAPACITY) {
                LdnQueueItem* slot = &sRecvQueue[sRecvTail];
                memcpy(slot->data, recvBuf, outSize);
                slot->length = (unsigned short)outSize;
                sRecvTail = (sRecvTail + 1) % LDN_QUEUE_CAPACITY;
                sRecvCount++;
            } else {
                ldn_log("[LDN] ldn_thread_func: recv queue full, dropping packet");
            }
            mutexUnlock(&sRecvMutex);
        } else {
            svcSleepThread(1000000ULL); // 1ms
        }
    }
    ldn_log("[LDN] ldn_thread_func: stopped");
}

static void ldn_thread_start(void) {
    if (sLdnThreadRunning) { return; }
    sLdnThreadShouldStop = false;
    mutexInit(&sRecvMutex);
    mutexInit(&sSendMutex);
    sRecvHead = sRecvTail = sRecvCount = 0;
    sSendHead = sSendTail = sSendCount = 0;
    sLastConnectedCount = -1;
    Result rc = threadCreate(&sLdnThread, ldn_thread_func, NULL, NULL, 32 * 1024, 0x2C, -2);
    if (R_FAILED(rc)) {
        ldn_log("[LDN] ldn_thread_start: threadCreate FAILED 0x%x", rc);
        return;
    }
    threadStart(&sLdnThread);
    sLdnThreadRunning = true;
}

static void ldn_thread_stop(void) {
    if (!sLdnThreadRunning) { return; }
    sLdnThreadShouldStop = true;
    threadWaitForExit(&sLdnThread);
    threadClose(&sLdnThread);
    sLdnThreadRunning = false;
}

void ldn_update_impl(void) {
    if (!sLdnInitialized || !sLdnConnected) { return; }

    // drain whatever the background thread has received since last frame -
    // network_receive() touches game state, so it must run on the main
    // thread, but the actual radio recv IPC already happened off-thread.
    for (;;) {
        LdnQueueItem item;
        bool has = false;
        mutexLock(&sRecvMutex);
        if (sRecvCount > 0) {
            item = sRecvQueue[sRecvHead];
            sRecvHead = (sRecvHead + 1) % LDN_QUEUE_CAPACITY;
            sRecvCount--;
            has = true;
        }
        mutexUnlock(&sRecvMutex);
        if (!has) { break; }
        network_receive(LDN_UNKNOWN_LOCAL_INDEX, NULL, item.data, item.length);
    }
}

int ldn_send_impl(unsigned char* data, unsigned short dataLength) {
    if (!sLdnInitialized || !sLdnConnected) {
        return -1;
    }

    if (dataLength > LDN_ACTION_FRAME_MAX_SIZE) {
        ldn_log("[LDN] ldn_send_impl: REFUSING oversized payload dataLength=%d (max=%d) - this previously crashed the ldn sysmodule", dataLength, LDN_ACTION_FRAME_MAX_SIZE);
        return -1;
    }

    // just hand it to the LDN thread - the actual IPC calls happen there
    mutexLock(&sSendMutex);
    bool queued = false;
    if (sSendCount < LDN_QUEUE_CAPACITY) {
        LdnQueueItem* slot = &sSendQueue[sSendTail];
        memcpy(slot->data, data, dataLength);
        slot->length = dataLength;
        sSendTail = (sSendTail + 1) % LDN_QUEUE_CAPACITY;
        sSendCount++;
        queued = true;
    }
    mutexUnlock(&sSendMutex);

    if (!queued) {
        ldn_log("[LDN] ldn_send_impl: send queue full, dropping packet");
        return -1;
    }
    return dataLength;
}

void ldn_shutdown_impl(void) {
    ldn_log("[LDN] ldn_shutdown_impl: initialized=%d connected=%d stationOpen=%d accessPointOpen=%d",
            sLdnInitialized, sLdnConnected, sLdnStationOpen, sLdnAccessPointOpen);
    // stop touching the ldn service from the background thread before we
    // start tearing the session down on this one, or the two threads could
    // race on the same session state
    ldn_thread_stop();
    if (!sLdnInitialized) { return; }

    mutexLock(&sLdnIpcMutex);
    if (sLdnConnected) {
        ldnDisconnect();
        sLdnConnected = false;
    }
    if (sLdnStationOpen) {
        ldnCloseStation();
        sLdnStationOpen = false;
    }
    if (sLdnAccessPointOpen) {
        ldnDestroyNetwork();
        ldnCloseAccessPoint();
        sLdnAccessPointOpen = false;
    }
    if (sLdnActionFrameEnabled) {
        ldnDisableActionFrame();
        sLdnActionFrameEnabled = false;
    }
    ldnExit();
    mutexUnlock(&sLdnIpcMutex);
    sLdnInitialized = false;
    sLdnNetworkCount = 0;
}

bool ldn_connect_to_index(int index) {
    ldn_log("[LDN] ldn_connect_to_index: index=%d initialized=%d connected=%d stationOpen=%d accessPointOpen=%d",
            index, sLdnInitialized, sLdnConnected, sLdnStationOpen, sLdnAccessPointOpen);

    if (index < 0 || index >= sLdnNetworkCount) { return false; }

    // If a previous attempt on this process left us in AccessPoint state
    // (e.g. switched from hosting to joining without a clean shutdown),
    // ldnOpenStation() below would fail with Invalid State. Reset to a known
    // clean state first.
    if (sLdnAccessPointOpen) {
        ldn_log("[LDN] ldn_connect_to_index: was in AccessPoint state, resetting first");
        ldn_shutdown_impl();
    }

    if (!sLdnInitialized) {
        if (!ldn_initialize_impl(false)) { return false; }
    } else if (!sLdnStationOpen) {
        mutexLock(&sLdnIpcMutex);
        Result rc = ldnOpenStation();
        mutexUnlock(&sLdnIpcMutex);
        ldn_log("[LDN] ldnOpenStation (reopen): 0x%x", rc);
        if (R_FAILED(rc)) { return false; }
        sLdnStationOpen = true;
    }

    LdnSecurityConfig sec;
    memset(&sec, 0, sizeof(sec));
    sec.security_mode = (LdnSecurityMode)0;
    sec.passphrase_size = 0x10;
    memset(sec.passphrase, 0x42, 0x10);

    LdnUserConfig user;
    memset(&user, 0, sizeof(user));
    snprintf(user.user_name, sizeof(user.user_name), "%s", configPlayerName);

    LdnNetworkInfo* target = &sLdnNetworkInfo[index];
    ldn_log("[LDN] ldn_connect_to_index: target ssid_len=%d channel=%d link_level=%d node_count=%d/%d version=%d security_mode=%d bssid=%02x:%02x:%02x:%02x:%02x:%02x",
            target->common.ssid.len, target->common.channel, target->common.link_level,
            target->node_count, target->node_count_max, target->version, target->security_mode,
            target->common.bssid.addr[0], target->common.bssid.addr[1], target->common.bssid.addr[2],
            target->common.bssid.addr[3], target->common.bssid.addr[4], target->common.bssid.addr[5]);

    mutexLock(&sLdnIpcMutex);
    Result rc = ldnConnect(&sec, &user, 0, 0, target);
    mutexUnlock(&sLdnIpcMutex);
    ldn_log("[LDN] ldnConnect: 0x%x (mod=%04x desc=%04x)", rc, R_MODULE(rc), R_DESCRIPTION(rc));
    if (R_FAILED(rc)) { return false; }

    sLdnConnected = true;
    sLdnStationOpen = false;
    ldn_thread_start();
    return true;
}

bool ldn_refresh_scan(void) {
    // Scanning while already connected is pointless (and racy - the
    // background thread may be mid ldnSendActionFrame/ldnRecvActionFrame on
    // this session right now) - this is what let the browser panel's
    // still-polling refresh crash the ldn sysmodule right after a successful
    // connect. Once connected there's nothing left to discover.
    if (sLdnConnected) { return true; }

    if (!sLdnInitialized) {
        if (!ldn_initialize_impl(false)) { return false; }
    }

    sLdnNetworkCount = 0;
    LdnScanFilter filter;
    memset(&filter, 0, sizeof(filter));
    // Unfiltered, ldnScan() returns every LDN network in range - other
    // people's unrelated games included, not just ours (this is why the
    // browser kept finding "4 networks" even with no host of ours running,
    // and why connecting to the wrong entry failed). -1 here resolves to
    // this process's own real LocalCommunicationId (the title we're
    // overridden as, e.g. Smash Ultimate), same as ldnCreateNetwork/
    // ldnConnect, so this only matches networks hosted by the same game.
    filter.network_id.intent_id.local_communication_id = -1;
    filter.flags = LdnScanFilterFlag_LocalCommunicationId;

    mutexLock(&sLdnIpcMutex);
    Result rc = ldnScan(0, &filter, sLdnNetworkInfo, 4, &sLdnNetworkCount);
    mutexUnlock(&sLdnIpcMutex);
    ldn_log("[LDN] ldnScan: 0x%x (mod=%04x desc=%04x), networks=%d",
            rc, R_MODULE(rc), R_DESCRIPTION(rc), sLdnNetworkCount);

    if (R_FAILED(rc)) {
        sLdnNetworkCount = 0;
        return false;
    }

    return true;
}

int ldn_get_network_count(void) {
    return sLdnNetworkCount;
}

const char* ldn_get_network_name(int index) {
    if (index < 0 || index >= sLdnNetworkCount) { return ""; }
    return sLdnNetworkInfo[index].nodes[0].user_name;
}

int ldn_get_network_player_count(int index) {
    if (index < 0 || index >= sLdnNetworkCount) { return 0; }
    return sLdnNetworkInfo[index].node_count;
}

int ldn_get_network_max_players(int index) {
    if (index < 0 || index >= sLdnNetworkCount) { return 0; }
    return sLdnNetworkInfo[index].node_count_max;
}

#endif // __SWITCH__
