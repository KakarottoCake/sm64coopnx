#include <stdio.h>
#include "../network.h"
#include "pc/mods/mods.h"
#include "pc/mods/mods_utils.h"
#include "pc/djui/djui.h"
#include "pc/djui/djui_panel_join_message.h"
#include "pc/debuglog.h"
#include "pc/mods/mod_cache.h"
#include "pc/utils/misc.h"

#ifdef __SWITCH__
extern void nx_packet_log(const char* fmt, ...);
#define MODLIST_LOG(...) nx_packet_log("[MODLIST] " __VA_ARGS__)
#else
#define MODLIST_LOG(...)
#endif

void network_send_mod_list_request(void) {
    SOFT_ASSERT(gNetworkType == NT_CLIENT);
    MODLIST_LOG("network_send_mod_list_request: start, gNetworkType=%d", gNetworkType);
    mods_clear(&gActiveMods);
    mods_clear(&gRemoteMods);

    if (!mods_generate_remote_base_path()) {
        LOG_ERROR("Failed to generate remote base path!");
        MODLIST_LOG("network_send_mod_list_request: mods_generate_remote_base_path FAILED");
        return;
    }

    struct Packet p = { 0 };
    packet_init(&p, PACKET_MOD_LIST_REQUEST, true, PLMT_NONE);
    char version[MAX_VERSION_LENGTH] = { 0 };
    snprintf(version, MAX_VERSION_LENGTH, "%s", get_version());
    packet_write(&p, &version, sizeof(u8) * MAX_VERSION_LENGTH);

    network_send_to(PACKET_DESTINATION_SERVER, &p);
    LOG_INFO("sending mod list request");
    MODLIST_LOG("network_send_mod_list_request: sent PACKET_MOD_LIST_REQUEST");
    gAllowOrderedPacketClear = 0;
}

void network_receive_mod_list_request(UNUSED struct Packet* p) {
    MODLIST_LOG("network_receive_mod_list_request: called, gNetworkType=%d", gNetworkType);
    if (gNetworkType != NT_SERVER) {
        LOG_ERROR("Network type should be server!");
        MODLIST_LOG("network_receive_mod_list_request: WRONG gNetworkType, bailing");
        return;
    }
    LOG_INFO("received mod list request");

    network_send_mod_list();
    MODLIST_LOG("network_receive_mod_list_request: network_send_mod_list() returned");
}

void network_send_mod_list(void) {
    SOFT_ASSERT(gNetworkType == NT_SERVER);
    MODLIST_LOG("network_send_mod_list: start, gActiveMods.entryCount=%d", gActiveMods.entryCount);

    packet_ordered_begin();

    struct Packet p = { 0 };
    packet_init(&p, PACKET_MOD_LIST, true, PLMT_NONE);

    char version[MAX_VERSION_LENGTH] = { 0 };
    snprintf(version, MAX_VERSION_LENGTH, "%s", get_version());
    LOG_INFO("sending version: %s", version);
    packet_write(&p, &version, sizeof(u8) * MAX_VERSION_LENGTH);
    packet_write(&p, &gActiveMods.entryCount, sizeof(u16));
    network_send_to(0, &p);

    LOG_INFO("sent mod list (%u):", gActiveMods.entryCount);
    for (u16 i = 0; i < gActiveMods.entryCount; i++) {
        struct Mod* mod = gActiveMods.entries[i];

        u16 nameLength = strlen(mod->name);
        if (nameLength > MOD_NAME_MAX_LENGTH) { nameLength = MOD_NAME_MAX_LENGTH; }

        u16 incompatibleLength = 0;
        if (mod->incompatible) {
            incompatibleLength = strlen(mod->incompatible);
            if (incompatibleLength > MOD_INCOMPATIBLE_MAX_LENGTH) { incompatibleLength = MOD_INCOMPATIBLE_MAX_LENGTH; }
        }

        u16 relativePathLength = strlen(mod->relativePath);
        u64 modSize = mod->size;

        struct Packet p = { 0 };
        packet_init(&p, PACKET_MOD_LIST_ENTRY, true, PLMT_NONE);
        packet_write(&p, &i, sizeof(u16));
        packet_write(&p, &nameLength, sizeof(u16));
        packet_write(&p, mod->name, sizeof(u8) * nameLength);
        packet_write(&p, &incompatibleLength, sizeof(u16));
        if (mod->incompatible) {
            packet_write(&p, mod->incompatible, sizeof(u8) * incompatibleLength);
        } else {
            packet_write(&p, "", 0);
        }
        packet_write(&p, &relativePathLength, sizeof(u16));
        packet_write(&p, mod->relativePath, sizeof(u8) * relativePathLength);
        packet_write(&p, &modSize, sizeof(u64));
        packet_write(&p, &mod->isDirectory, sizeof(u8));
        packet_write(&p, &mod->pausable, sizeof(u8));
        packet_write(&p, &mod->ignoreScriptWarnings, sizeof(u8));
        packet_write(&p, &mod->fileCount, sizeof(u16));
        network_send_to(0, &p);
        LOG_INFO("    '%s': %llu", mod->name, (u64)mod->size);

#ifdef __SWITCH__
        // Switch/LDN never transfers file contents and doesn't need
        // per-file hashes either - it only ever asks "do you already have a
        // mod with this name", answered entirely from the PACKET_MOD_LIST_ENTRY
        // sent above (see network_ldn_check_mods_available()). Skipping this
        // avoids exchanging per-file metadata for every file in the mod
        // (previously hundreds of round trips for a mod like Star Road, and
        // before that, oversized batched packets that crashed the ldn
        // sysmodule outright).
#else
        for (u16 j = 0; j < mod->fileCount; j++) {
            struct Packet p = { 0 };
            packet_init(&p, PACKET_MOD_LIST_FILE, true, PLMT_NONE);
            struct ModFile* file = &mod->files[j];
            u16 relativePathLength = strlen(file->relativePath);
            u64 fileSize = file->size;
            packet_write(&p, &i, sizeof(u16));
            packet_write(&p, &j, sizeof(u16));
            packet_write(&p, &relativePathLength, sizeof(u16));
            packet_write(&p, file->relativePath, sizeof(u8) * relativePathLength);
            packet_write(&p, &fileSize, sizeof(u64));
            packet_write(&p, &file->dataHash[0], sizeof(u8) * 16);
            network_send_to(0, &p);
            LOG_INFO("      '%s': %llu", file->relativePath, (u64)file->size);
        }
#endif
    }

    struct Packet p2 = { 0 };
    packet_init(&p2, PACKET_MOD_LIST_DONE, true, PLMT_NONE);
    network_send_to(0, &p2);
    MODLIST_LOG("network_send_mod_list: sent PACKET_MOD_LIST + %d entries + PACKET_MOD_LIST_DONE", gActiveMods.entryCount);

    packet_ordered_end();

}

void network_receive_mod_list(struct Packet* p) {
    SOFT_ASSERT(gNetworkType == NT_CLIENT);
    MODLIST_LOG("network_receive_mod_list: called, p->localIndex=%d gNetworkType=%d", p->localIndex, gNetworkType);

    if (p->localIndex != UNKNOWN_LOCAL_INDEX) {
        if (gNetworkPlayerServer == NULL || gNetworkPlayerServer->localIndex != p->localIndex) {
            LOG_ERROR("Received mod list from known local index '%d'", p->localIndex);
            MODLIST_LOG("network_receive_mod_list: REFUSED unknown localIndex=%d (gNetworkPlayerServer=%p)", p->localIndex, gNetworkPlayerServer);
            return;
        }
    }

    if (gRemoteMods.entries != NULL) {
        LOG_INFO("received mod list after allocating");
        MODLIST_LOG("network_receive_mod_list: gRemoteMods.entries already allocated, ignoring duplicate");
        return;
    }

    if (gNetworkServerAddr == NULL) {
        gNetworkServerAddr = network_duplicate_address(0);
    }

    char version[MAX_VERSION_LENGTH] = { 0 };
    snprintf(version, MAX_VERSION_LENGTH, "%s", get_version());
    LOG_INFO("client has version: %s", version);

    // verify version
    char remoteVersion[MAX_VERSION_LENGTH] = { 0 };
    packet_read(p, &remoteVersion, sizeof(u8) * MAX_VERSION_LENGTH);
    LOG_INFO("server has version: %s", version);
    MODLIST_LOG("network_receive_mod_list: clientVersion='%s' serverVersion='%s'", version, remoteVersion);
    if (memcmp(version, remoteVersion, MAX_VERSION_LENGTH) != 0) {
        network_shutdown(true, false, false, false);
        LOG_ERROR("version mismatch");
        MODLIST_LOG("network_receive_mod_list: VERSION MISMATCH, aborting");
        char mismatchMessage[256] = { 0 };
        snprintf(mismatchMessage, 256, "\\#ffa0a0\\Error:\\#dcdcdc\\ Version mismatch.\n\nYour version: \\#a0a0ff\\%s\\#dcdcdc\\\nTheir version: \\#a0a0ff\\%s\\#dcdcdc\\\n\nSomeone is out of date!\n", version, remoteVersion);
        djui_panel_join_message_error(mismatchMessage);
        return;
    }

    packet_read(p, &gRemoteMods.entryCount, sizeof(u16));
    MODLIST_LOG("network_receive_mod_list: gRemoteMods.entryCount=%d", gRemoteMods.entryCount);
    gRemoteMods.entries = calloc(gRemoteMods.entryCount, sizeof(struct Mod*));
    if (gRemoteMods.entries == NULL) {
        LOG_ERROR("Failed to allocate remote mod entries");
        return;
    }

    LOG_INFO("received mod list (%u):", gRemoteMods.entryCount);
}

void network_receive_mod_list_entry(struct Packet* p) {
    SOFT_ASSERT(gNetworkType == NT_CLIENT);
    MODLIST_LOG("network_receive_mod_list_entry: called, p->localIndex=%d", p->localIndex);

    // make sure it was sent by the server
    if (p->localIndex != UNKNOWN_LOCAL_INDEX) {
        if (gNetworkPlayerServer == NULL || gNetworkPlayerServer->localIndex != p->localIndex) {
            LOG_ERROR("Received download from known local index '%d'", p->localIndex);
            MODLIST_LOG("network_receive_mod_list_entry: REFUSED unknown localIndex");
            return;
        }
    }

    // get mod index
    u16 modIndex = 0;
    packet_read(p, &modIndex, sizeof(u16));
    MODLIST_LOG("network_receive_mod_list_entry: modIndex=%d entryCount=%d", modIndex, gRemoteMods.entryCount);
    if (modIndex >= gRemoteMods.entryCount) {
        LOG_ERROR("Received mod outside of known range");
        MODLIST_LOG("network_receive_mod_list_entry: modIndex OUT OF RANGE");
        return;
    }

    // allocate mod entry
    gRemoteMods.entries[modIndex] = calloc(1, sizeof(struct Mod));
    struct Mod* mod = gRemoteMods.entries[modIndex];
    if (mod == NULL) {
        LOG_ERROR("Failed to allocate remote mod!");
        return;
    }

    // get name length
    u16 nameLength = 0;
    packet_read(p, &nameLength, sizeof(u16));
    if (nameLength > MOD_NAME_MAX_LENGTH) {
        LOG_ERROR("Received name with invalid length!");
        return;
    }

    // get name
    packet_read(p, mod->name, nameLength * sizeof(u8));
    mod->name[nameLength] = 0;

    // get incompatible length
    u16 incompatibleLength = 0;
    packet_read(p, &incompatibleLength, sizeof(u16));
    if (incompatibleLength > MOD_INCOMPATIBLE_MAX_LENGTH) {
        LOG_ERROR("Received name with invalid length!");
        return;
    }

    // get incompatible
    if (incompatibleLength > 0) {
        char incompatible[MOD_INCOMPATIBLE_SIZE] = { 0 };
        packet_read(p, incompatible, incompatibleLength * sizeof(u8));
        mod->incompatible = strdup(incompatible);
    } else {
        packet_read(p, 0, 0);
    }

    // get other fields
    u16 relativePathLength = 0;
    packet_read(p, &relativePathLength, sizeof(u16));
    packet_read(p, mod->relativePath, relativePathLength * sizeof(u8));
    packet_read(p, &mod->size, sizeof(u64));
    packet_read(p, &mod->isDirectory, sizeof(u8));
    packet_read(p, &mod->pausable, sizeof(u8));
    packet_read(p, &mod->ignoreScriptWarnings, sizeof(u8));
    normalize_path(mod->relativePath);
    LOG_INFO("    '%s': %llu", mod->name, (u64)mod->size);

    // figure out base path
    if (mod->isDirectory) {
        if (snprintf(mod->basePath, SYS_MAX_PATH - 1, "%s/%s", gRemoteModsBasePath, mod->relativePath) < 0) {
            LOG_ERROR("Failed save remote base path!");
            return;
        }
        normalize_path(mod->basePath);
    } else {
        if (snprintf(mod->basePath, SYS_MAX_PATH - 1, "%s", gRemoteModsBasePath) < 0) {
            LOG_ERROR("Failed save remote base path!");
            return;
        }
    }

    // sanity check mod size
    if (mod->size >= MAX_MOD_SIZE) {
        djui_popup_create(DLANG(NOTIF, DISCONNECT_BIG_MOD), 4);
        network_shutdown(false, false, false, false);
        return;
    }

    // get file count and allocate them
    packet_read(p, &mod->fileCount, sizeof(u16));
    mod->files = calloc(mod->fileCount, sizeof(struct ModFile));
    if (mod->files == NULL) {
        LOG_ERROR("Failed to allocate mod files!");
        return;
    }
}

// shared by network_receive_mod_list_file (one entry per packet) and
// network_receive_mod_list_file_batch (many entries per packet) - reads
// exactly one file entry from the packet's current cursor position.
static void network_receive_mod_list_file_entry(struct Packet* p) {
    // get mod index
    u16 modIndex = 0;
    packet_read(p, &modIndex, sizeof(u16));
    if (modIndex >= gRemoteMods.entryCount) {
        LOG_ERROR("Received mod outside of known range");
        return;
    }
    struct Mod* mod = gRemoteMods.entries[modIndex];
    if (mod == NULL) {
        LOG_ERROR("Received mod file for null mod");
        return;
    }

    // get file index
    u16 fileIndex = 0;
    packet_read(p, &fileIndex, sizeof(u16));
    if (fileIndex >= mod->fileCount) {
        LOG_ERROR("Received mod file outside of known range");
        return;
    }
    struct ModFile* file = &mod->files[fileIndex];
    if (mod == NULL) {
        LOG_ERROR("Received null mod file");
        return;
    }

    u16 relativePathLength = 0;
    packet_read(p, &relativePathLength, sizeof(u16));
    packet_read(p, file->relativePath, relativePathLength * sizeof(u8));
    packet_read(p, &file->size, sizeof(u64));
    packet_read(p, &file->dataHash, sizeof(u8) * 16);
    file->fp = NULL;
    LOG_INFO("      '%s': %llu", file->relativePath, (u64)file->size);

    struct ModCacheEntry* cache = mod_cache_get_from_hash(file->dataHash);
    if (cache != NULL) {
        LOG_INFO("Found file in cache: %s -> %s", file->relativePath, cache->path);
        if (file->cachedPath != NULL) {
            free((char*)file->cachedPath);
        }
        file->cachedPath = strdup(cache->path);
        normalize_path(file->cachedPath);
    }
}

void network_receive_mod_list_file(struct Packet* p) {
    SOFT_ASSERT(gNetworkType == NT_CLIENT);

    if (p->localIndex != UNKNOWN_LOCAL_INDEX) {
        if (gNetworkPlayerServer == NULL || gNetworkPlayerServer->localIndex != p->localIndex) {
            LOG_ERROR("Received download from known local index '%d'", p->localIndex);
            return;
        }
    }

    network_receive_mod_list_file_entry(p);
}

// Sent only by network_send_mod_list()'s Switch/LDN path - many file entries
// packed into one reliable packet instead of one packet per file. A mod
// with hundreds of files (e.g. a big level hack) took thousands of
// individually-acknowledged round trips over LDN's small, non-blocking
// action-frame transport, which looked like a freeze but was really just
// grinding through the file list one entry at a time.
void network_receive_mod_list_file_batch(struct Packet* p) {
    SOFT_ASSERT(gNetworkType == NT_CLIENT);

    if (p->localIndex != UNKNOWN_LOCAL_INDEX) {
        if (gNetworkPlayerServer == NULL || gNetworkPlayerServer->localIndex != p->localIndex) {
            LOG_ERROR("Received download from known local index '%d'", p->localIndex);
            return;
        }
    }

    u16 entryCount = 0;
    packet_read(p, &entryCount, sizeof(u16));
    for (u16 i = 0; i < entryCount; i++) {
        network_receive_mod_list_file_entry(p);
    }
}

#ifdef __SWITCH__
// LDN's transport isn't proven for bulk file transfer, and re-sending mod
// files over local wireless every join is wasteful even if it worked - so
// on Switch, joining just asks "do you already have a mod with this name"
// against your own local mods (gLocalMods, populated at boot regardless of
// network role) and enables it if so. No file lists, no hashes, nothing
// transferred - the host says "this mod is enabled", the client says
// "got it" or reports it missing.
static bool network_ldn_check_mods_available(void) {
    bool allAvailable = true;
    for (u16 i = 0; i < gRemoteMods.entryCount; i++) {
        struct Mod* mod = gRemoteMods.entries[i];
        struct Mod* localMatch = NULL;
        for (u16 j = 0; j < gLocalMods.entryCount; j++) {
            if (strcmp(gLocalMods.entries[j]->name, mod->name) == 0) {
                localMatch = gLocalMods.entries[j];
                break;
            }
        }
        MODLIST_LOG("network_ldn_check_mods_available: mod[%d]='%s' localMatch=%d", i, mod->name, localMatch != NULL);
        if (localMatch != NULL) {
            mod->enabled = true;
            // point at the mod we actually already have, not the empty
            // remote-download staging path - we never transferred any files
            snprintf(mod->basePath, SYS_MAX_PATH - 1, "%s", localMatch->basePath);
            snprintf(mod->relativePath, SYS_MAX_PATH - 1, "%s", localMatch->relativePath);
            mod->isDirectory = localMatch->isDirectory;
            // mod->files is still the empty stub array from the network
            // entry (fileCount>0 but every relativePath is "") since we
            // never send file data - mod_refresh_files() throws that away
            // and rescans mod->basePath on disk, same as how gLocalMods
            // itself was populated, giving mod_activate() a real file list
            // (with correct cachedPath) instead of empty entries that would
            // silently activate nothing (or, before the mod_activate fix,
            // crash on a null cachedPath).
            // mod_refresh_files() rescans every file in the mod directory
            // from disk and (for any not already in the in-memory cache from
            // this mod's own boot-time scan) MD5-hashes it - a cache miss
            // here means re-hashing every file in the mod from SD card,
            // which for a large pack could plausibly take many seconds and
            // stall the main thread (no rendering, no network_update) the
            // whole time. Timed to confirm/rule this out as the cause of the
            // multi-second freeze seen joining "Star Road".
            float refreshStart = clock_elapsed();
            bool refreshOk = mod_refresh_files(mod);
            MODLIST_LOG("network_ldn_check_mods_available: mod_refresh_files('%s') took %.2fs, fileCount=%d", mod->name, clock_elapsed() - refreshStart, mod->fileCount);
            if (!refreshOk) {
                LOG_ERROR("Failed to refresh local files for matched mod '%s'", mod->name);
                allAvailable = false;
                mod->enabled = false;
            }
        } else {
            allAvailable = false;
        }
    }
    MODLIST_LOG("network_ldn_check_mods_available: allAvailable=%d", allAvailable);
    return allAvailable;
}
#endif

void network_receive_mod_list_done(struct Packet* p) {
    SOFT_ASSERT(gNetworkType == NT_CLIENT);
    MODLIST_LOG("network_receive_mod_list_done: called, p->localIndex=%d", p->localIndex);

    if (p->localIndex != UNKNOWN_LOCAL_INDEX) {
        if (gNetworkPlayerServer == NULL || gNetworkPlayerServer->localIndex != p->localIndex) {
            LOG_ERROR("Received download from known local index '%d'", p->localIndex);
            MODLIST_LOG("network_receive_mod_list_done: REFUSED unknown localIndex");
            return;
        }
    }

    size_t totalSize = 0;
    for (u16 i = 0; i < gRemoteMods.entryCount; i++) {
        struct Mod* mod = gRemoteMods.entries[i];
        totalSize += mod->size;
    }
    gRemoteMods.size = totalSize;

#ifdef __SWITCH__
    if (network_ldn_check_mods_available()) {
        MODLIST_LOG("network_receive_mod_list_done: all mods available, sending join request");
        network_send_join_request();
        return;
    }
    MODLIST_LOG("network_receive_mod_list_done: mods missing, showing error and aborting");

    char missingMessage[1024] = { 0 };
    int len = snprintf(missingMessage, sizeof(missingMessage), "\\#ffa0a0\\Missing mods:\\#dcdcdc\\\n\n");
    for (u16 i = 0; i < gRemoteMods.entryCount && len < (int)sizeof(missingMessage); i++) {
        struct Mod* mod = gRemoteMods.entries[i];
        if (!mod->enabled) {
            len += snprintf(missingMessage + len, sizeof(missingMessage) - len, "\\#a0a0ff\\%s\\#dcdcdc\\\n", mod->name);
        }
    }
    snprintf(missingMessage + len, sizeof(missingMessage) - len, "\nInstall these mods locally to join this room.");
    network_shutdown(true, false, false, false);
    djui_panel_join_message_error(missingMessage);
    return;
#else
    network_start_download_requests();
#endif
}
