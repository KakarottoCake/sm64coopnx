#ifdef __SWITCH__

// Isolated on purpose: this file includes ONLY <switch.h> (libnx) and
// standard/socket headers, never any project header. <switch.h>'s u64/s64
// typedefs (long) conflict with this project's own PR/ultratypes.h u64/s64
// (long long) - same width, but C treats them as distinct types, so the
// two cannot be included in the same translation unit. network.c's LDN
// glue (struct NetworkSystem gNetworkSystemLdn, etc.) calls into the
// plain-C-typed (bool/int/u8/u16/u32, never u64/s64) functions below
// instead of touching libnx types directly. Peer addresses cross that
// boundary as opaque void* pointing at struct in_addr (a plain C socket
// type, not a project or libnx type), so both sides can handle them.

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <switch.h>

// TRANSPORT: game packets travel as ordinary UDP datagrams over the LDN
// network, NOT as ldn action frames. The LDN service assigns every joined
// console a real IPv4 address (LdnNodeInfo.ip_addr - usable with standard
// sockets), and the bsd socket service handles the data path in the kernel
// the same way it does for real games. The earlier action-frame transport
// was a dead end (per-packet IPC into the ldn sysmodule, silent multi-second
// drops, 0x400 size limit, and hard console-freezing wedges), so ldn* calls
// are now only used for creating/scanning/joining the network itself.
#define LDN_PACKET_LENGTH 3000
// matches network_player.h's UNKNOWN_LOCAL_INDEX ((u8)-1)
#define LDN_UNKNOWN_LOCAL_INDEX ((unsigned char)-1)
// matches include/types.h's MAX_PLAYERS
#define LDN_MAX_PLAYERS 16
// UDP port for game traffic on the LDN network (mirrors the PC default port)
#define LDN_UDP_PORT 7777

extern void network_receive(unsigned char localIndex, void* addr, unsigned char* data, unsigned short dataLength);
extern char configPlayerName[];

static bool sLdnInitialized = false;
static bool sLdnAccessPointOpen = false;
static bool sLdnStationOpen = false;
static bool sLdnConnected = false;
static bool sIsServer = false;
static LdnNetworkInfo sLdnNetworkInfo[4];
static int sLdnNetworkCount = 0;

// UDP transport + per-player address tracking. This mirrors socket.c's
// sAddr[] model exactly: index 0 doubles as a scratch "address of the most
// recently received packet" AND (on the client) the server's address;
// indices 1..MAX-1 hold each connected player's bound address, set via
// ldn_save_id_impl() when they join. Matching an incoming packet's source
// address against this table is what lets the server tell which player sent
// a packet - without it every packet looks like it's from an unknown player,
// which made the server assign a fresh globalIndex to every (duplicated)
// join request and produce phantom players.
static int sUdpSocket = -1;
static struct in_addr sOwnAddr;
static struct in_addr sLdnAddr[LDN_MAX_PLAYERS];

// periodic activity summary (see ldn_update_impl)
static int sSendOkCount = 0;
static int sSendFailCount = 0;
static int sRecvOkCount = 0;
static int sConnectedCount = 0;
static u64 sLastHeartbeatTick = 0;

void ldn_shutdown_impl(void);

static void ldn_log(const char* fmt, ...) {
    static int sCallsRemaining = 2000;
    if (sCallsRemaining <= 0) { return; }
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

// The libnx header notes LdnIpv4Address is "essentially the same as struct
// in_addr ... (byteswap required)" - it stores the address in the opposite
// byte order from the network order sockets expect.
static struct in_addr ldn_to_in_addr(LdnIpv4Address ldnAddr) {
    struct in_addr a;
    a.s_addr = __builtin_bswap32(ldnAddr.addr);
    return a;
}

// Re-reads the node list from the ldn service. Used to (a) log how many
// nodes are connected, and (b) on the client, learn the host's (AccessPoint
// node[0]) address so we know where to send. Directed per-player addressing
// otherwise comes from sLdnAddr[] (learned as packets arrive), not from here.
static void ldn_refresh_nodes(void) {
    LdnNetworkInfo netInfo;
    Result rc = ldnGetNetworkInfo(&netInfo);
    if (R_FAILED(rc)) {
        ldn_log("[LDN] ldn_refresh_nodes: ldnGetNetworkInfo FAILED 0x%x (mod=%04x desc=%04x)", rc, R_MODULE(rc), R_DESCRIPTION(rc));
        return;
    }

    int connectedCount = 0;
    for (s32 i = 0; i < netInfo.node_count && i < 8; i++) {
        if (netInfo.nodes[i].is_connected) { connectedCount++; }
    }
    if (connectedCount != sConnectedCount) {
        ldn_log("[LDN] ldn_refresh_nodes: connected node count %d -> %d", sConnectedCount, connectedCount);
        sConnectedCount = connectedCount;
    }

    // node[0] is always the AccessPoint (host). On the client, that's the
    // server we send to - bind it to index 0.
    if (!sIsServer && netInfo.node_count >= 1 && netInfo.nodes[0].is_connected) {
        sLdnAddr[0] = ldn_to_in_addr(netInfo.nodes[0].ip_addr);
    }
}

static bool ldn_udp_open(void) {
    static bool sSocketServiceUp = false;
    if (!sSocketServiceUp) {
        Result rc = socketInitializeDefault();
        ldn_log("[LDN] socketInitializeDefault: 0x%x", rc);
        if (R_FAILED(rc)) { return false; }
        sSocketServiceUp = true;
    }

    LdnIpv4Address ownLdnAddr;
    LdnSubnetMask mask;
    Result rc = ldnGetIpv4Address(&ownLdnAddr, &mask);
    if (R_FAILED(rc)) {
        ldn_log("[LDN] ldnGetIpv4Address FAILED 0x%x (mod=%04x desc=%04x)", rc, R_MODULE(rc), R_DESCRIPTION(rc));
        return false;
    }
    sOwnAddr = ldn_to_in_addr(ownLdnAddr);
    unsigned int ip = sOwnAddr.s_addr;
    ldn_log("[LDN] own ip: %u.%u.%u.%u",
            ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);

    sUdpSocket = socket(AF_INET, SOCK_DGRAM, 0);
    if (sUdpSocket < 0) {
        ldn_log("[LDN] socket() failed: errno=%d", errno);
        return false;
    }

    int flags = fcntl(sUdpSocket, F_GETFL, 0);
    fcntl(sUdpSocket, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in bindAddr;
    memset(&bindAddr, 0, sizeof(bindAddr));
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_port = htons(LDN_UDP_PORT);
    bindAddr.sin_addr.s_addr = INADDR_ANY;
    if (bind(sUdpSocket, (struct sockaddr*)&bindAddr, sizeof(bindAddr)) < 0) {
        ldn_log("[LDN] bind() failed: errno=%d", errno);
        close(sUdpSocket);
        sUdpSocket = -1;
        return false;
    }

    memset(sLdnAddr, 0, sizeof(sLdnAddr));
    sConnectedCount = 0;
    ldn_refresh_nodes();
    sLastHeartbeatTick = svcGetSystemTick();
    sSendOkCount = sSendFailCount = sRecvOkCount = 0;
    ldn_log("[LDN] udp transport up on port %d (role=%s)", LDN_UDP_PORT, sIsServer ? "host" : "client");
    return true;
}

static void ldn_udp_close(void) {
    if (sUdpSocket >= 0) {
        close(sUdpSocket);
        sUdpSocket = -1;
    }
    memset(sLdnAddr, 0, sizeof(sLdnAddr));
}

bool ldn_initialize_impl(bool isServer) {
    ldn_log("[LDN] ldn_initialize_impl: isServer=%d initialized=%d connected=%d stationOpen=%d",
            isServer, sLdnInitialized, sLdnConnected, sLdnStationOpen);

    if (sLdnInitialized && sLdnConnected) { return true; }
    sIsServer = isServer;

    // Switching roles without a clean shutdown leaves us in the wrong open
    // state; only reset on an actual role mismatch (not just because a
    // station is already open, which is the normal client re-entry state).
    if (sLdnInitialized && ((isServer && sLdnStationOpen) || (!isServer && sLdnAccessPointOpen))) {
        ldn_log("[LDN] ldn_initialize_impl: stale role state, resetting first");
        ldn_shutdown_impl();
    }

    if (!sLdnInitialized) {
        Result rc;
        for (int attempt = 0; attempt < 5; attempt++) {
            rc = ldnInitialize(LdnServiceType_User);
            ldn_log("[LDN] ldnInitialize attempt=%d: 0x%x (mod=%04x desc=%04x)", attempt, rc, R_MODULE(rc), R_DESCRIPTION(rc));
            if (R_SUCCEEDED(rc)) { break; }
            svcSleepThread(300000000ULL); // 300ms
        }
        if (R_FAILED(rc)) { return false; }
        sLdnInitialized = true;
    }

    if (isServer) {
        if (!sLdnAccessPointOpen) {
            Result rc = ldnOpenAccessPoint();
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

        Result rc = ldnCreateNetwork(&sec, &user, &cfg);
        ldn_log("[LDN] ldnCreateNetwork: 0x%x (mod=%04x desc=%04x)", rc, R_MODULE(rc), R_DESCRIPTION(rc));
        if (R_FAILED(rc)) {
            ldnCloseAccessPoint();
            sLdnAccessPointOpen = false;
            return false;
        }

        sLdnConnected = true;
        ldnSetStationAcceptPolicy(LdnAcceptPolicy_AlwaysAccept);
        if (!ldn_udp_open()) {
            ldn_log("[LDN] ldn_initialize_impl: udp transport failed to open");
            ldn_shutdown_impl();
            return false;
        }
    } else if (!sLdnStationOpen) {
        Result rc = ldnOpenStation();
        ldn_log("[LDN] ldnOpenStation: 0x%x (mod=%04x desc=%04x)", rc, R_MODULE(rc), R_DESCRIPTION(rc));
        if (R_FAILED(rc)) { return false; }
        sLdnStationOpen = true;
    }

    return true;
}

void ldn_update_impl(void) {
    if (!sLdnInitialized || !sLdnConnected || sUdpSocket < 0) { return; }

    u64 nowTick = svcGetSystemTick();
    u64 tickFreq = armGetSystemTickFreq();

    if ((nowTick - sLastHeartbeatTick) >= (tickFreq * 2)) {
        ldn_refresh_nodes(); // pick up joins/leaves + client's server addr
        ldn_log("[LDN] heartbeat: sendOk=%d sendFail=%d recvOk=%d connected=%d", sSendOkCount, sSendFailCount, sRecvOkCount, sConnectedCount);
        sSendOkCount = sSendFailCount = sRecvOkCount = 0;
        sLastHeartbeatTick = nowTick;
    }

    // drain everything that's arrived since last frame (non-blocking)
    unsigned char recvBuf[LDN_PACKET_LENGTH];
    for (;;) {
        struct sockaddr_in from;
        socklen_t fromLen = sizeof(from);
        ssize_t len = recvfrom(sUdpSocket, recvBuf, sizeof(recvBuf), 0, (struct sockaddr*)&from, &fromLen);
        if (len <= 0) { break; }
        sRecvOkCount++;

        // stash the sender in scratch index 0, then match it against the
        // bound player addresses so the server knows which player this is
        // (exactly how socket.c's socket_receive assigns localIndex).
        sLdnAddr[0] = from.sin_addr;
        unsigned char localIndex = LDN_UNKNOWN_LOCAL_INDEX;
        for (int i = 1; i < LDN_MAX_PLAYERS; i++) {
            if (sLdnAddr[i].s_addr != 0 && sLdnAddr[i].s_addr == from.sin_addr.s_addr) {
                localIndex = (unsigned char)i;
                break;
            }
        }
        network_receive(localIndex, &sLdnAddr[0], recvBuf, (unsigned short)len);
    }
}

// Sends one packet to a specific player. Mirrors socket.c's ns_socket_send:
// normally the destination is the bound address for localIndex, except
// localIndex 0 with an explicit address (the reply-to-sender convention)
// sends to that address instead.
int ldn_send_impl(unsigned char localIndex, void* address, unsigned char* data, unsigned short dataLength) {
    if (!sLdnInitialized || !sLdnConnected || sUdpSocket < 0) { return -1; }
    if (localIndex >= LDN_MAX_PLAYERS) { return -1; }

    struct in_addr dest = sLdnAddr[localIndex];
    if (localIndex == 0 && address != NULL) {
        dest = *(struct in_addr*)address;
    }
    if (dest.s_addr == 0) {
        // no known address for this target yet - nothing to send to
        return -1;
    }

    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(LDN_UDP_PORT);
    to.sin_addr = dest;

    ssize_t sent = sendto(sUdpSocket, data, dataLength, 0, (struct sockaddr*)&to, sizeof(to));
    if (sent < 0) {
        sSendFailCount++;
        return -1;
    }
    sSendOkCount++;
    return dataLength;
}

// --- per-player address glue, called from network.c's NetworkSystem ---

void* ldn_dup_addr_impl(unsigned char localIndex) {
    if (localIndex >= LDN_MAX_PLAYERS) { localIndex = 0; }
    struct in_addr* copy = malloc(sizeof(struct in_addr));
    if (copy) { *copy = sLdnAddr[localIndex]; }
    return copy;
}

bool ldn_match_addr_impl(void* a, void* b) {
    if (a == NULL || b == NULL) { return false; }
    return ((struct in_addr*)a)->s_addr == ((struct in_addr*)b)->s_addr;
}

// bind the current sender (scratch index 0) to a player slot on join
void ldn_save_id_impl(unsigned char localIndex) {
    if (localIndex == 0 || localIndex >= LDN_MAX_PLAYERS) { return; }
    sLdnAddr[localIndex] = sLdnAddr[0];
    unsigned int ip = sLdnAddr[localIndex].s_addr;
    ldn_log("[LDN] bound player %d to %u.%u.%u.%u", localIndex,
            ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
}

void ldn_clear_id_impl(unsigned char localIndex) {
    if (localIndex == 0 || localIndex >= LDN_MAX_PLAYERS) { return; }
    sLdnAddr[localIndex].s_addr = 0;
}

void ldn_shutdown_impl(void) {
    ldn_log("[LDN] ldn_shutdown_impl: initialized=%d connected=%d stationOpen=%d accessPointOpen=%d",
            sLdnInitialized, sLdnConnected, sLdnStationOpen, sLdnAccessPointOpen);
    ldn_udp_close();
    if (!sLdnInitialized) { return; }

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
    ldnExit();
    sLdnInitialized = false;
    sLdnNetworkCount = 0;
}

bool ldn_connect_to_index(int index) {
    ldn_log("[LDN] ldn_connect_to_index: index=%d initialized=%d connected=%d stationOpen=%d accessPointOpen=%d",
            index, sLdnInitialized, sLdnConnected, sLdnStationOpen, sLdnAccessPointOpen);

    if (index < 0 || index >= sLdnNetworkCount) { return false; }

    if (sLdnAccessPointOpen) {
        ldn_log("[LDN] ldn_connect_to_index: was in AccessPoint state, resetting first");
        ldn_shutdown_impl();
    }

    sIsServer = false;
    if (!sLdnInitialized) {
        if (!ldn_initialize_impl(false)) { return false; }
    } else if (!sLdnStationOpen) {
        Result rc = ldnOpenStation();
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

    Result rc = ldnConnect(&sec, &user, 0, 0, target);
    ldn_log("[LDN] ldnConnect: 0x%x (mod=%04x desc=%04x)", rc, R_MODULE(rc), R_DESCRIPTION(rc));
    if (R_FAILED(rc)) { return false; }

    sLdnConnected = true;
    sLdnStationOpen = false;
    if (!ldn_udp_open()) {
        ldn_log("[LDN] ldn_connect_to_index: udp transport failed to open");
        ldn_shutdown_impl();
        return false;
    }
    return true;
}

bool ldn_refresh_scan(void) {
    if (sLdnConnected) { return true; }

    if (!sLdnInitialized) {
        if (!ldn_initialize_impl(false)) { return false; }
    }

    sLdnNetworkCount = 0;
    LdnScanFilter filter;
    memset(&filter, 0, sizeof(filter));
    filter.network_id.intent_id.local_communication_id = -1;
    filter.flags = LdnScanFilterFlag_LocalCommunicationId;

    Result rc = ldnScan(0, &filter, sLdnNetworkInfo, 4, &sLdnNetworkCount);
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

// --- Async join -----------------------------------------------------------
// ldnScan()/ldnConnect() are blocking IPC calls into the ldn sysmodule that
// can take hundreds of ms; running them from the button callback froze the
// render loop for the whole join. Instead the heavy scan+connect runs on a
// worker thread while the main thread keeps drawing the "Joining..." menu and
// polls ldn_poll_connect() each frame. All djui/network follow-up (mod list
// request, popups) stays on the main thread via the poll result - the worker
// touches ONLY the ldn/socket state in this file. ldn_update_impl() does no
// concurrent ldn IPC during the connect because sLdnConnected is still false
// until the worker finishes.
static Thread sConnectThread;
static bool sConnectThreadActive = false;
static int sConnectIndex = -1;
// 0 = idle, 1 = running, 2 = done ok, 3 = done fail
static volatile int sConnectState = 0;

static void ldn_connect_worker(void* arg) {
    (void)arg;
    ldn_refresh_scan();
    bool ok = ldn_connect_to_index(sConnectIndex);
    sConnectState = ok ? 2 : 3;
}

// Starts the async scan+connect for the given scan index. Returns immediately.
void ldn_begin_connect(int index) {
    if (sConnectState == 1) { return; } // already connecting
    if (sConnectThreadActive) {
        threadWaitForExit(&sConnectThread);
        threadClose(&sConnectThread);
        sConnectThreadActive = false;
    }
    sConnectIndex = index;
    sConnectState = 1;
    Result rc = threadCreate(&sConnectThread, ldn_connect_worker, NULL, NULL, 128 * 1024, 0x2C, -2);
    if (R_FAILED(rc)) { sConnectState = 3; return; }
    rc = threadStart(&sConnectThread);
    if (R_FAILED(rc)) { threadClose(&sConnectThread); sConnectState = 3; return; }
    sConnectThreadActive = true;
}

// Poll from the main thread each frame. Returns 0 while idle/running, 1 once
// on success, -1 once on failure (then latches back to idle).
int ldn_poll_connect(void) {
    int s = sConnectState;
    if (s != 2 && s != 3) { return 0; }
    if (sConnectThreadActive) {
        threadWaitForExit(&sConnectThread);
        threadClose(&sConnectThread);
        sConnectThreadActive = false;
    }
    sConnectState = 0;
    return (s == 2) ? 1 : -1;
}

#endif // __SWITCH__
