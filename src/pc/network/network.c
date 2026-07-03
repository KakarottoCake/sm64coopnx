#include "socket/socket.h"
#include "coopnet/coopnet.h"
#include <stdio.h>
#include "network.h"
#include "object_fields.h"
#include "game/level_update.h"
#include "object_constants.h"
#include "behavior_table.h"
#include "pc/configfile.h"
#include "pc/djui/djui.h"
#include "pc/djui/djui_panel.h"
#include "pc/djui/djui_hud_utils.h"
#include "pc/djui/djui_panel_main.h"
#include "pc/utils/misc.h"
#include "pc/lua/smlua.h"
#include "pc/lua/utils/smlua_model_utils.h"
#include "pc/lua/utils/smlua_misc_utils.h"
#include "pc/lua/utils/smlua_camera_utils.h"
#include "pc/lua/utils/smlua_gfx_utils.h"
#include "pc/mods/mods.h"
#include "pc/crash_handler.h"
#include "pc/debuglog.h"
#include "pc/pc_main.h"
#include "pc/gfx/gfx_pc.h"
#include "pc/fs/fmem.h"
#include "game/hardcoded.h"
#include "game/scroll_targets.h"
#include "game/camera.h"
#include "game/skybox.h"
#include "game/object_list_processor.h"
#include "game/object_helpers.h"
#include "game/level_geo.h"
#include "menu/intro_geo.h"
#include "game/ingame_menu.h"
#include "game/first_person_cam.h"
#include "game/envfx_snow.h"
#include "game/mario.h"
#include "engine/math_util.h"
#include "engine/lighting_engine.h"
#include "audio/load.h"

#ifdef DISCORD_SDK
#include "pc/discord/discord.h"
#endif

// fix warnings when including rendering_graph_node
#undef near
#undef far
#include "game/rendering_graph_node.h"

// Mario 64 specific externs
extern s16 sCurrPlayMode;
extern s16 gCurrCourseNum, gCurrActStarNum, gCurrLevelNum, gCurrAreaIndex;

enum NetworkType gNetworkType = NT_NONE;
struct NetworkSystem* gNetworkSystem = &gNetworkSystemSocket;

#define LOADING_LEVEL_THRESHOLD 10
#define MAX_PACKETS_PER_SECOND_PER_PLAYER ((u16)100)

u16 networkLoadingLevel = 0;
bool gNetworkAreaLoaded = false;
bool gNetworkAreaSyncing = true;
u32 gNetworkAreaTimerClock = 0;
u32 gNetworkAreaTimer = 0;
void* gNetworkServerAddr = NULL;
bool gNetworkSentJoin = false;
u16 gNetworkRequestLocationTimer = 0;

u8 gDebugPacketIdBuffer[256] = { 0xFF };
u8 gDebugPacketSentBuffer[256] = { 0 };
u8 gDebugPacketOnBuffer = 0;

u32 gNetworkStartupTimer = 0;
u32 sNetworkReconnectTimer = 0;
u32 sNetworkRehostTimer = 0;
enum NetworkSystemType sNetworkReconnectType = NS_SOCKET;

struct ServerSettings gServerSettings = {
    .playerInteractions = PLAYER_INTERACTIONS_SOLID,
    .bouncyLevelBounds = BOUNCY_LEVEL_BOUNDS_OFF,
    .playerKnockbackStrength = 25,
    .skipIntro = FALSE,
    .bubbleDeath = TRUE,
    .enablePlayersInLevelDisplay = TRUE,
    .enablePlayerList = TRUE,
    .headlessServer = FALSE,
    .nametags = TRUE,
    .maxPlayers = MAX_PLAYERS,
    .pauseAnywhere = FALSE,
    .pvpType = PLAYER_PVP_CLASSIC,
};

struct NametagsSettings gNametagsSettings = {
    .showHealth = false,
    .showSelfTag = false,
};

void network_set_system(enum NetworkSystemType nsType) {
    network_forget_all_reliable();

    switch (nsType) {
        case NS_SOCKET:  gNetworkSystem = &gNetworkSystemSocket; break;
#ifdef COOPNET
        case NS_COOPNET: gNetworkSystem = &gNetworkSystemCoopNet; break;
#endif
#ifdef __SWITCH__
        case NS_LDN:     gNetworkSystem = &gNetworkSystemLdn; break;
#endif
        default: gNetworkSystem = &gNetworkSystemSocket; LOG_ERROR("Unknown network system: %d", nsType); break;
    }
}

bool network_init(enum NetworkType inNetworkType, bool reconnecting) {
    // reset override hide hud
    extern u8 gOverrideHideHud;
    gOverrideHideHud = 0;
    act_select_hud_show(ACT_SELECT_HUD_ALL);
    gNetworkStartupTimer = 5 * 30;

    // sanity check network system
    if (gNetworkSystem == NULL) {
        LOG_ERROR("no network system attached");
        return false;
    }

    network_forget_all_reliable();
    crash_handler_init();

    // set server settings
    gServerSettings.playerInteractions = configPlayerInteraction;
    gServerSettings.bouncyLevelBounds = configBouncyLevelBounds;
    gServerSettings.playerKnockbackStrength = configPlayerKnockbackStrength;
    gServerSettings.stayInLevelAfterStar = configStayInLevelAfterStar;
    gServerSettings.skipIntro = gCLIOpts.skipIntro ? TRUE : configSkipIntro;
    gServerSettings.bubbleDeath = configBubbleDeath;
    gServerSettings.enablePlayersInLevelDisplay = TRUE;
    gServerSettings.enablePlayerList = TRUE;
    gServerSettings.nametags = configNametags;
    gServerSettings.maxPlayers = configAmountOfPlayers;
    gServerSettings.pauseAnywhere = configPauseAnywhere;
    gServerSettings.pvpType = configPvpType;
    gServerSettings.headlessServer = gCLIOpts.headless && (inNetworkType == NT_SERVER);

    gNametagsSettings.showHealth = false;
    gNametagsSettings.showSelfTag = false;

    gPauseMenuHidden = false;

    // initialize the network system
    gNetworkSentJoin = false;
#ifdef __SWITCH__
    extern void nx_checkpoint(const char* label);
    nx_checkpoint("network_init: before gNetworkSystem->initialize");
#endif
    int rc = gNetworkSystem->initialize(inNetworkType, reconnecting);
#ifdef __SWITCH__
    nx_checkpoint("network_init: gNetworkSystem->initialize returned");
#endif
    if (!rc && inNetworkType != NT_NONE) {
        LOG_ERROR("failed to initialize network system");
        djui_popup_create(DLANG(NOTIF, DISCONNECT_CLOSED), 2);
        return false;
    }
    if (gNetworkServerAddr != NULL) {
        free(gNetworkServerAddr);
        gNetworkServerAddr = NULL;
    }

    // set network type
    gNetworkType = inNetworkType;

    if (gNetworkType == NT_SERVER) {
        extern s16 gCurrSaveFileNum;
        gCurrSaveFileNum = configHostSaveSlot;

        mods_activate(&gLocalMods);
#ifdef __SWITCH__
        nx_checkpoint("network_init: mods_activate done");
#endif
        smlua_init();
#ifdef __SWITCH__
        nx_checkpoint("network_init: smlua_init done");
#endif

        dynos_behavior_hook_all_custom_behaviors();
#ifdef __SWITCH__
        nx_checkpoint("network_init: dynos_behavior_hook_all_custom_behaviors done");
#endif

        network_player_connected(NPT_LOCAL, 0, configPlayerModel, &configPlayerPalette, configPlayerName, get_local_discord_id());
#ifdef __SWITCH__
        nx_checkpoint("network_init: network_player_connected done");
#endif
        extern u8* gOverrideEeprom;
        gOverrideEeprom = NULL;

        if (gCurrLevelNum != (s16)gLevelValues.entryLevel) {
            extern s16 gChangeLevelTransition;
            gChangeLevelTransition = gLevelValues.entryLevel;
        }

        djui_chat_box_create();
#ifdef __SWITCH__
        nx_checkpoint("network_init: djui_chat_box_create done");
#endif
    }

    configfile_save(configfile_name());
#ifdef __SWITCH__
    nx_checkpoint("network_init: configfile_save done");
#endif

#ifdef DISCORD_SDK
    if (gDiscordInitialized) {
        discord_activity_update();
    }
#endif

    djui_base_set_visible(&gDjuiModReload->base, network_allow_mod_dev_mode());

    LOG_INFO("initialized");

    return true;
}

void network_on_init_area(void) {
    // reset loading timer
    networkLoadingLevel = 0;
    gNetworkAreaLoaded = false;
    gNetworkAreaSyncing = true;
    gNetworkAreaTimer = 0;
    gNetworkAreaTimerClock = clock_elapsed_ticks();
}

void network_on_loaded_area(void) {
    area_remove_sync_ids_clear();
    struct NetworkPlayer* np = gNetworkPlayerLocal;
    if (np != NULL) {
        bool levelMatch = (np->currCourseNum == gCurrCourseNum
                           && np->currActNum == gCurrActStarNum
                           && np->currLevelNum == gCurrLevelNum);
        if (np->currLevelSyncValid && levelMatch && np->currAreaIndex != gCurrAreaIndex) {
            network_send_change_area();
        } else {
            network_send_change_level();
        }
    }
}

static void network_remember_debug_packet(u8 id, bool sent) {
    if (id == PACKET_ACK) { return; }
    if (id == PACKET_KEEP_ALIVE) { return; }
    if (id == PACKET_DEBUG_SYNC) { return; }
    if (id == PACKET_PLAYER && id == gDebugPacketIdBuffer[gDebugPacketOnBuffer]) { return; }
    if (id == PACKET_OBJECT && id == gDebugPacketIdBuffer[gDebugPacketOnBuffer]) { return; }
    gDebugPacketOnBuffer++;
    gDebugPacketIdBuffer[gDebugPacketOnBuffer] = id;
    gDebugPacketSentBuffer[gDebugPacketOnBuffer] = sent;
}

bool network_allow_unknown_local_index(enum PacketType packetType) {
    return (packetType == PACKET_JOIN_REQUEST)
        || (packetType == PACKET_KICK)
        || (packetType == PACKET_ACK)
        || (packetType == PACKET_MOD_LIST_REQUEST)
        || (packetType == PACKET_MOD_LIST)
        || (packetType == PACKET_MOD_LIST_ENTRY)
        || (packetType == PACKET_MOD_LIST_FILE)
        || (packetType == PACKET_MOD_LIST_DONE)
        || (packetType == PACKET_MOD_LIST_FILE_BATCH)
        || (packetType == PACKET_DOWNLOAD_REQUEST)
        || (packetType == PACKET_DOWNLOAD)
        || (packetType == PACKET_KEEP_ALIVE)
        || (packetType == PACKET_DEBUG_SYNC)
        || (packetType == PACKET_PING)
        || (packetType == PACKET_PONG);
}

void network_send_to(u8 localIndex, struct Packet* p) {
    if (p == NULL) {
        LOG_ERROR("no data to send");
        return;
    }
#ifdef __SWITCH__
    extern void nx_packet_log(const char* fmt, ...);
    nx_packet_log("[TX] network_send_to: requestedLocalIndex=%d packetType=%d p->localIndex=%d gNetworkType=%d",
                  localIndex, p->buffer[0], p->localIndex, gNetworkType);
#endif

    // set destination
    if (localIndex == PACKET_DESTINATION_SERVER) {
        packet_set_destination(p, 0);
        localIndex = (gNetworkPlayerServer != NULL) ? gNetworkPlayerServer->localIndex : 0;
    } else {
        u8 idx = (localIndex == 0) ? p->localIndex : localIndex;
        if (idx >= MAX_PLAYERS) {
            LOG_ERROR("Could not set destination to %u", idx);
            return;
        }
        packet_set_destination(p, p->requestBroadcast
                                ? PACKET_DESTINATION_BROADCAST
                                : gNetworkPlayers[idx].globalIndex);
    }

    // sanity checks
    if (gNetworkType == NT_NONE) { LOG_ERROR("network type error none!"); return; }
    if (p->error) { LOG_ERROR("packet error!"); return; }
    if (gNetworkSystem == NULL) { LOG_ERROR("no network system attached"); return; }
    if (localIndex == 0 && !network_allow_unknown_local_index(p->buffer[0])) {
        LOG_ERROR("\n####################\nsending to myself, packetType: %d\n####################\n", p->packetType);
#ifdef __SWITCH__
        nx_packet_log("[TX] network_send_to: REFUSED (sending to myself), packetType=%d", p->packetType);
#endif
        // SOFT_ASSERT(false); - Crash?
        return;
    }

    if (gNetworkType == NT_SERVER) {
        if (localIndex >= MAX_PLAYERS) {
            LOG_ERROR("Could not get network player %u", localIndex);
            return;
        }
        struct NetworkPlayer* np = &gNetworkPlayers[localIndex];
        // don't send a packet to a player that can't receive it
        if (p->levelAreaMustMatch) {
            if (p->courseNum != np->currCourseNum) { return; }
            if (p->actNum    != np->currActNum)    { return; }
            if (p->levelNum  != np->currLevelNum)  { return; }
            if (p->areaIndex != np->currAreaIndex) { return; }
        } else if (p->levelMustMatch) {
            if (p->courseNum != np->currCourseNum) { return; }
            if (p->actNum    != np->currActNum)    { return; }
            if (p->levelNum  != np->currLevelNum)  { return; }
        }
    }

    // set the flags again
    packet_set_flags(p);

    p->localIndex = localIndex;

    // set ordered data (MUST BE IMMEDITAELY BEFORE network_remember_reliable())
    if (p->orderedGroupId != 0 && !p->sent) {
        packet_set_ordered_data(p);
    }

    // remember reliable packets
    network_remember_reliable(p);

    // save inside packet buffer
    u32 hash = packet_hash(p);
    memcpy(&p->buffer[p->dataLength], &hash, sizeof(u32));

    // redirect to server if required
    if (localIndex != 0 && gNetworkType != NT_SERVER && gNetworkSystem->requireServerBroadcast && gNetworkPlayerServer != NULL) {
        localIndex = gNetworkPlayerServer->localIndex;
    }

    SOFT_ASSERT(p->dataLength < PACKET_LENGTH);

    // rate limit packets
    bool tooManyPackets = false;
    s32 maxPacketsPerSecond = (gNetworkType == NT_SERVER) ? (MAX_PACKETS_PER_SECOND_PER_PLAYER * (u16)network_player_connected_count()) : MAX_PACKETS_PER_SECOND_PER_PLAYER;
    static s32 sPacketsPerSecond[MAX_PLAYERS] = { 0 };
    static f32 sPacketsPerSecondTime[MAX_PLAYERS] = { 0 };
    f32 currentTime = clock_elapsed();
    if ((currentTime - sPacketsPerSecondTime[localIndex]) > 0) {
        if (sPacketsPerSecond[localIndex] > maxPacketsPerSecond) {
            LOG_ERROR("Too many packets sent to localIndex %d! Attempted %d. Connected count %d.", localIndex, sPacketsPerSecond[localIndex], network_player_connected_count());
        }
        sPacketsPerSecondTime[localIndex] = currentTime;
        sPacketsPerSecond[localIndex] = 1;
    } else {
        sPacketsPerSecond[localIndex]++;
        if (sPacketsPerSecond[localIndex] > maxPacketsPerSecond) {
            tooManyPackets = true;
        }
    }

    // send
    if (!tooManyPackets) {
        if (p->keepSendingAfterDisconnect) {
            localIndex = 0; // Force this type of packet to use the saved addr
        }
        u8* buffer = NULL;
        u32 len = 0;
        packet_compress(p, &buffer, &len);
        if (!buffer || len == 0) {
            LOG_ERROR("Failed to compress!");
#ifdef __SWITCH__
            nx_packet_log("[TX] network_send_to: packet_compress FAILED, packetType=%d", p->packetType);
#endif
        } else {
#ifdef __SWITCH__
            nx_packet_log("[TX] network_send_to: calling gNetworkSystem->send localIndex=%d len=%u packetType=%d", localIndex, len, p->packetType);
#endif
            int rc = gNetworkSystem->send(localIndex, p->addr, buffer, len);
#ifdef __SWITCH__
            nx_packet_log("[TX] network_send_to: send rc=%d", rc);
#endif
            if (rc == SOCKET_ERROR) { LOG_ERROR("send error %d", rc); return; }
        }
    }
#ifdef __SWITCH__
    else {
        nx_packet_log("[TX] network_send_to: DROPPED (too many packets), packetType=%d", p->packetType);
    }
#endif
    p->sent = true;

    network_remember_debug_packet(p->packetType, true);

    if (localIndex < MAX_PLAYERS) {
        gNetworkPlayers[localIndex].lastSent = clock_elapsed();
    }
}

void network_send(struct Packet* p) {
    if (p == NULL) {
        LOG_ERROR("no data to send");
        return;
    }
    // prevent errors during writing from propagating
    if (p->writeError) {
        LOG_ERROR("packet has write error: %u", p->packetType);
        return;
    }

    // set the flags again
    packet_set_flags(p);

    if (gNetworkType != NT_SERVER) {
        p->requestBroadcast = TRUE;
        if (gNetworkSystem != NULL && gNetworkSystem->requireServerBroadcast && gNetworkPlayerServer != NULL) {
            int i = gNetworkPlayerServer->localIndex;
            p->localIndex = i;
            p->sent = false;
            network_send_to(i, p);
            return;
        }
    }

    for (s32 i = 1; i < MAX_PLAYERS; i++) {
        struct NetworkPlayer* np = &gNetworkPlayers[i];
        if (!np->connected) { continue; }

        // don't send a packet to a player that can't receive it
        if (p->levelAreaMustMatch) {
            if (p->courseNum != np->currCourseNum) { continue; }
            if (p->actNum    != np->currActNum)    { continue; }
            if (p->levelNum  != np->currLevelNum)  { continue; }
            if (p->areaIndex != np->currAreaIndex) { continue; }
        } else if (p->levelMustMatch) {
            if (p->courseNum != np->currCourseNum) { continue; }
            if (p->actNum    != np->currActNum)    { continue; }
            if (p->levelNum  != np->currLevelNum)  { continue; }
        }

        p->localIndex = i;
        p->sent = false;
        network_send_to(i, p);
    }
}

void network_receive(u8 localIndex, void* addr, u8* data, u16 dataLength) {
#ifdef __SWITCH__
    extern void nx_packet_log(const char* fmt, ...);
    nx_packet_log("[RX] network_receive: localIndex=%d addr=%p dataLength=%d rawType=%d gNetworkType=%d",
                  localIndex, addr, dataLength, dataLength > 0 ? data[0] : -1, gNetworkType);
#endif

    // receive packet
    struct Packet p = {
        .localIndex = localIndex,
        .cursor = 3,
        .addr = addr,
        .buffer = { 0 },
        .dataLength = dataLength,
    };
    if (!packet_decompress(&p, data, dataLength)) {
        LOG_ERROR("Failed to decompress!");
#ifdef __SWITCH__
        nx_packet_log("[RX] network_receive: packet_decompress FAILED");
#endif
        return;
    }

    if (localIndex != UNKNOWN_LOCAL_INDEX && localIndex != 0) {
        gNetworkPlayers[localIndex].lastReceived = clock_elapsed();
    }

    // subtract and check hash
    if (!packet_check_hash(&p)) {
        LOG_ERROR("invalid packet hash!");
#ifdef __SWITCH__
        nx_packet_log("[RX] network_receive: packet_check_hash FAILED, packetType=%d", p.buffer[0]);
#endif
        return;
    }

#ifdef __SWITCH__
    nx_packet_log("[RX] network_receive: decompressed OK, packetType=%d dataLength=%d", p.buffer[0], p.dataLength);
#endif

    network_remember_debug_packet(p.buffer[0], false);

    // execute packet
    packet_receive(&p);
}

void* network_duplicate_address(u8 localIndex) {
    assert(localIndex < MAX_PLAYERS);
    return gNetworkSystem->dup_addr(localIndex);
}

void network_reset_reconnect_and_rehost(void) {
    gNetworkStartupTimer = 0;
    sNetworkReconnectTimer = 0;
    sNetworkRehostTimer = 0;
    sNetworkReconnectType = NS_SOCKET;
}

void network_reconnect_begin(void) {
    if (sNetworkReconnectTimer > 0) {
        return;
    }

    sNetworkReconnectTimer = 2 * 30;

#ifdef COOPNET
    sNetworkReconnectType = (gNetworkSystem == &gNetworkSystemCoopNet)
                          ? NS_COOPNET
                          : NS_SOCKET;
#else
    sNetworkReconnectType = NS_SOCKET;
#endif

    network_shutdown(false, false, false, true);

    djui_connect_menu_open();
}

static void network_reconnect_update(void) {
    if (sNetworkReconnectTimer <= 0) { return; }
    if (--sNetworkReconnectTimer != 0) { return; }

    if (sNetworkReconnectType == NS_SOCKET) {
        network_set_system(NS_SOCKET);
    } else if (sNetworkReconnectType == NS_COOPNET) {
        network_set_system(NS_COOPNET);
    }

    network_init(NT_CLIENT, true);

    network_send_mod_list_request();
}

bool network_is_reconnecting(void) {
    return sNetworkReconnectTimer > 0;
}

void network_rehost_begin(void) {
    for (int i = 1; i < MAX_PLAYERS; i++) {
        struct NetworkPlayer* np = &gNetworkPlayers[i];
        if (!np->connected) { continue; }

        network_send_kick(i, EKT_REJOIN);
        network_player_disconnected(i);
    }

    network_shutdown(false, false, false, true);

    sNetworkRehostTimer = 2;
}

extern void djui_panel_do_host(bool reconnecting, bool playSound);
static void network_rehost_update(void) {
    if (sNetworkRehostTimer <= 0) { return; }
    if (--sNetworkRehostTimer != 0) { return; }

    djui_panel_do_host(true, true);
}

static void network_update_area_timer(void) {
    bool brokenClock = false;
#ifdef DEVELOPMENT
    static u16 skipClockCount = 0;
    static u16 updateClockCount = 1;
    if (updateClockCount > 0) {
        updateClockCount--;
        if (updateClockCount <= 0 || updateClockCount > 120) {
            skipClockCount = rand() % 30;
        }
    }
    else {
        skipClockCount--;
        if (skipClockCount <= 0 || skipClockCount > 60) {
            updateClockCount = rand() % 120;
        }
    }
    //brokenClock = (skipClockCount > 0);
#endif
    if (!brokenClock) {
        if (network_check_singleplayer_pause()) {
            gNetworkAreaTimerClock++;
        }
        // update network area timer
        u32 desiredNAT = gNetworkAreaTimer + 1;
        gNetworkAreaTimer = (clock_elapsed_ticks() - gNetworkAreaTimerClock);
        if (gNetworkAreaTimer < desiredNAT) {
            gNetworkAreaTimer++;
        }
        else if (gNetworkAreaTimer > desiredNAT) {
            gNetworkAreaTimer--;
        }
    }
}

#ifdef COOPNET
void network_update_coopnet(void) {
    if (gNetworkType != NT_NONE) { return; }
    if (!ns_coopnet_is_connected()) { return; }
    ns_coopnet_update();
}
#endif

void network_update(void) {
    if (gNetworkStartupTimer > 0) {
        gNetworkStartupTimer--;
    }

    network_rehost_update();
    network_reconnect_update();

#ifdef COOPNET
    network_update_coopnet();
#endif

    // check for level loaded event
    if (networkLoadingLevel < LOADING_LEVEL_THRESHOLD) {
        networkLoadingLevel++;
        if (!gNetworkAreaLoaded && networkLoadingLevel >= LOADING_LEVEL_THRESHOLD) {
            gNetworkAreaLoaded = true;
            network_on_loaded_area();
        }
    }

    // update network area timer
    network_update_area_timer();

    // send out update packets
    if (gNetworkType != NT_NONE) {
        network_player_update();
        if (sCurrPlayMode == PLAY_MODE_NORMAL || sCurrPlayMode == PLAY_MODE_PAUSED) {
            network_update_player();
            network_update_objects();
        }
    }

    // receive packets
    if (gNetworkSystem != NULL) {
        gNetworkSystem->update();
    }

    // update reliable and ordered packets
    if (gNetworkType != NT_NONE) {
        network_update_reliable();
        packet_ordered_update();
    }

    sync_objects_update();

    // update level/area request timers
    /*struct NetworkPlayer* np = gNetworkPlayerLocal;
    if (np != NULL && !np->currLevelSyncValid) {
        gNetworkRequestLocationTimer++;
        if (gNetworkRequestLocationTimer > 30 * 10) {
            // find a NetworkPlayer around that location
            struct NetworkPlayer *npLevelAreaMatch = get_network_player_from_area(np->currCourseNum, np->currActNum, np->currLevelNum, np->currAreaIndex);
            struct NetworkPlayer *npLevelMatch     = get_network_player_from_level(np->currCourseNum, np->currActNum, np->currLevelNum);
            struct NetworkPlayer *npAny = (npLevelAreaMatch == NULL) ? npLevelMatch : npLevelAreaMatch;

            bool inCredits = (np->currActNum == 99);
            if (gNetworkType == NT_SERVER && (npAny == NULL || inCredits)) {
                // no NetworkPlayer in the level
                network_send_sync_valid(np, np->currCourseNum, np->currActNum, np->currLevelNum, np->currAreaIndex, false);
                return;
            }

            // matching NetworkPlayer is client
            if (npAny == npLevelAreaMatch) {
                network_send_level_area_request(np, npAny);
            } else {
                network_send_level_request(np, npAny);
            }
        }
    }*/

    // Kick the player back to the Main Menu if network init failed
    if ((gNetworkType == NT_NONE) && !gDjuiInMainMenu) {
        network_reset_reconnect_and_rehost();
        network_shutdown(true, false, false, false);
    }
}

static inline void color_set(Color color, u8 r, u8 g, u8 b) {
    color[0] = r;
    color[1] = g;
    color[2] = b;
}

bool network_allow_mod_dev_mode(void) {
    return (configModDevMode && gNetworkSystem == &gNetworkSystemSocket && gNetworkType == NT_SERVER);
}

void network_mod_dev_mode_reload(void) {
    network_rehost_begin();

    for (int i = 0; i < gLocalMods.entryCount; i++) {
        struct Mod* mod = gLocalMods.entries[i];
        if (mod->enabled) {
            mod_refresh_files(mod);
        }
    }

    djui_lua_error_clear();

    LOG_CONSOLE(" ");
    LOG_CONSOLE("===================================================");
    LOG_CONSOLE("===================================================");
    LOG_CONSOLE("===================================================");
    LOG_CONSOLE("===================== REFRESH =====================");
    LOG_CONSOLE("===================================================");
    LOG_CONSOLE("===================================================");
    LOG_CONSOLE("===================================================");
}


void network_shutdown(bool sendLeaving, bool exiting, bool popup, bool reconnecting) {
    smlua_call_event_hooks(HOOK_ON_EXIT);

    if (gDjuiChatBox != NULL) {
        djui_base_destroy(&gDjuiChatBox->base);
        gDjuiChatBox = NULL;
    }

    gNetworkSentJoin = false;

    network_forget_all_reliable();
    if (gNetworkSystem == NULL) {
        LOG_ERROR("no network system attached");
    } else {
        if (gNetworkPlayerLocal != NULL && sendLeaving) { network_send_leaving(gNetworkPlayerLocal->globalIndex); }
        network_player_shutdown(popup);
        gNetworkSystem->shutdown(reconnecting);
    }
    if (gNetworkServerAddr != NULL) {
        free(gNetworkServerAddr);
        gNetworkServerAddr = NULL;
    }
    gNetworkPlayerServer = NULL;

    if (sNetworkReconnectTimer <= 0 || sNetworkReconnectType != NS_COOPNET) {
        gNetworkType = NT_NONE;
    }

    if (exiting) { return; }

    dynos_model_clear_pool(MODEL_POOL_SESSION);

    // reset other stuff
    extern u8* gOverrideEeprom;
    gOverrideEeprom = NULL;
    extern u8 gOverrideFreezeCamera;
    gOverrideFreezeCamera = false;
    gDjuiHudLockMouse = false;
    gOverrideNear = 0;
    gOverrideFar = 0;
    gOverrideFOV = 0;
    gRoomOverride = -1;
    gOverrideBank = -1;
    gCurrActStarNum = 0;
    gCurrActNum = 0;
    gCurrCreditsEntry = NULL;
    vec3f_set(gLightingDir, 0, 0, 0);
    color_set(gLightingColor[0], 0xFF, 0xFF, 0xFF);
    color_set(gLightingColor[1], 0xFF, 0xFF, 0xFF);
    color_set(gVertexColor, 0xFF, 0xFF, 0xFF);
    color_set(gSkyboxColor, 0xFF, 0xFF, 0xFF);
    color_set(gFogColor, 0xFF, 0xFF, 0xFF);
    gFogIntensity = 1.0f;
    gFullbright = false;
    clear_all_shader_flags();
    gOverrideBackground = -1;
    gOverrideEnvFx = ENVFX_MODE_NO_OVERRIDE;
    gRomhackCameraSettings.switchable = FALSE;
    gOverrideAllowToxicGasCamera = FALSE;
    gRomhackCameraSettings.dpad = FALSE;
    camera_reset_overrides();
    romhack_camera_reset_settings();
    free_vtx_scroll_targets();
    dynos_mod_shutdown();
    mods_clear(&gActiveMods);
    mods_clear(&gRemoteMods);
    smlua_shutdown();
    extern s16 gChangeLevel;
    gChangeLevel = LEVEL_CASTLE_GROUNDS;
    network_player_init();
    gMarioStates[0].cap = 0;
    gMarioStates[0].input = 0;
    extern s16 gTTCSpeedSetting;
    gTTCSpeedSetting = 0;
    gOverrideDialogPos = 0;
    gOverrideDialogColor = 0;
    gDialogMinWidth = 0;
    gOverrideAllowToxicGasCamera = FALSE;
    gLuaVolumeMaster = 127;
    gLuaVolumeLevel = 127;
    gLuaVolumeSfx = 127;
    gLuaVolumeEnv = 127;

    struct Controller* cnt = gPlayer1Controller;
    cnt->rawStickX = 0;
    cnt->rawStickY = 0;
    cnt->stickX = 0;
    cnt->stickY = 0;
    cnt->stickMag = 0;
    cnt->buttonDown = 0;
    cnt->buttonPressed = 0;
    cnt->buttonReleased = 0;
    cnt->extStickX = 0;
    cnt->extStickY = 0;

    gFirstPersonCamera.enabled = false;
    gFirstPersonCamera.forcePitch = false;
    gFirstPersonCamera.forceYaw = false;
    gFirstPersonCamera.forceRoll = true;
    gFirstPersonCamera.centerL = true;
    gFirstPersonCamera.fov = FIRST_PERSON_DEFAULT_FOV;
    vec3f_set(gFirstPersonCamera.offset, 0, 0, 0);
    first_person_reset();

    le_shutdown();

    extern void save_file_load_all(UNUSED u8 reload);
    save_file_load_all(TRUE);
    extern void save_file_set_using_backup_slot(bool usingBackupSlot);
    save_file_set_using_backup_slot(false);
    f_shutdown();

    extern s16 gMenuMode;
    gMenuMode = -1;

    reset_window_title();

    init_mario_from_save_file();

    djui_panel_shutdown();
    extern bool gDjuiInMainMenu;
    if (!gDjuiInMainMenu) {
        gDjuiInMainMenu = true;
        djui_panel_main_create(NULL);
    }
    djui_lua_error_clear();

#ifdef DISCORD_SDK
    if (gDiscordInitialized) {
        discord_activity_update();
    }
#endif
    packet_ordered_clear_all();

    djui_reset_popup_disabled_override();
}

#ifdef __SWITCH__

// Local Data Network (LDN) ad-hoc wireless multiplayer. Coexists with
// NS_SOCKET (real internet sockets, see socket.c) as an additional
// network backend, selected via the LOCAL WIRELESS buttons in
// djui_panel_host.c and the network picker in djui_panel_ldn_browser.c.
//
// The actual libnx Ldn* API calls live in socket/socket_ldn.c, not here:
// <switch.h>'s u64/s64 typedefs (long) conflict with this project's own
// PR/ultratypes.h u64/s64 (long long) - same width, but C treats them as
// distinct types, so the two headers cannot coexist in one translation
// unit. socket_ldn.c includes only <switch.h> (no project headers) and
// exposes a plain-C-typed (bool/int/u8/u16/u32/s32, never u64/s64) boundary
// that this file calls into instead.

// implemented in socket_ldn.c (isolated from this project's u64/s64 types).
// Peer addresses are opaque void* pointing at a struct in_addr on the other
// side of the boundary; this file only passes them around, never derefs them.
bool ldn_initialize_impl(bool isServer);
void ldn_update_impl(void);
int ldn_send_impl(u8 localIndex, void* addr, u8* data, u16 dataLength);
void ldn_shutdown_impl(void);
void* ldn_dup_addr_impl(u8 localIndex);
bool ldn_match_addr_impl(void* addr1, void* addr2);
void ldn_save_id_impl(u8 localIndex);
void ldn_clear_id_impl(u8 localIndex);

static s64 ldn_get_id(UNUSED u8 localIndex) { return 0; }
static char* ldn_get_id_str(UNUSED u8 localIndex) { return "ldn"; }
static void ldn_get_lobby_id(UNUSED char* destination, UNUSED u32 destLength) {}
static void ldn_get_lobby_secret(UNUSED char* destination, UNUSED u32 destLength) {}

// address tracking: forward to socket_ldn.c so the server can match incoming
// packets to the right player. Without this every packet looked like it came
// from an unknown player, so duplicate join requests spawned phantom players.
static void ldn_save_id(u8 localIndex, UNUSED s64 networkId) { ldn_save_id_impl(localIndex); }
static void ldn_clear_id(u8 localIndex) { ldn_clear_id_impl(localIndex); }
static void* ldn_dup_addr(u8 localIndex) { return ldn_dup_addr_impl(localIndex); }
static bool ldn_match_addr(void* addr1, void* addr2) { return ldn_match_addr_impl(addr1, addr2); }

static bool ldn_initialize(enum NetworkType networkType, UNUSED bool reconnecting) {
    return ldn_initialize_impl(networkType == NT_SERVER);
}

static void ldn_update(void) {
    ldn_update_impl();
}

static int ldn_send(u8 localIndex, void* addr, u8* data, u16 dataLength) {
    return ldn_send_impl(localIndex, addr, data, dataLength);
}

static void ldn_shutdown(UNUSED bool reconnecting) {
    ldn_shutdown_impl();
}

struct NetworkSystem gNetworkSystemLdn = {
    .initialize       = ldn_initialize,
    .get_id           = ldn_get_id,
    .get_id_str       = ldn_get_id_str,
    .save_id          = ldn_save_id,
    .clear_id         = ldn_clear_id,
    .dup_addr         = ldn_dup_addr,
    .match_addr       = ldn_match_addr,
    .update           = ldn_update,
    .send             = ldn_send,
    .get_lobby_id     = ldn_get_lobby_id,
    .get_lobby_secret = ldn_get_lobby_secret,
    .shutdown         = ldn_shutdown,
    .requireServerBroadcast = false,
    .name             = "LDN",
};

#endif // __SWITCH__
