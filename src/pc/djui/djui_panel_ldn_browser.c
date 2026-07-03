#ifdef __SWITCH__
#include "djui.h"
#include "djui_panel.h"
#include "djui_panel_menu.h"
#include "djui_paginated.h"
#include "pc/network/network.h"
#include "pc/configfile.h"
#include "pc/debuglog.h"

static struct DjuiPaginated* sLdnPaginated = NULL;
static struct DjuiFlowLayout* sLdnLayout = NULL;
static struct DjuiButton* sRefreshButton = NULL;
static bool sLdnConnecting = false;

static void djui_panel_ldn_connect(struct DjuiBase* caller) {
    if (sLdnConnecting) { return; }
    sLdnConnecting = true;

    s32 index = (s32)(intptr_t)caller->tag;
    LOG_INFO("LDN: connecting to network %d", index);

    // network_set_system()+network_init(NT_CLIENT,...) sets gNetworkType to
    // NT_CLIENT and runs the setup (network_forget_all_reliable, gServerSettings,
    // etc.) the rest of the join depends on. For the LDN client, network_init's
    // initialize() is deliberately a no-op (see ldn_initialize in network.c) -
    // all ldn work (init/station/scan/connect) is deferred to the async worker
    // so it all runs on one thread; splitting it makes ldnConnect fail 0x82cb.
    network_reset_reconnect_and_rehost();
    network_set_system(NS_LDN);
    network_init(NT_CLIENT, false);

    // Show "Joining..." immediately, then scan+connect on the worker thread so
    // the render loop keeps going. network_update() polls ldn_poll_connect()
    // and, on success, sends the mod-list request that finishes the join (on
    // failure it tears down). The worker re-scans, so no pre-scan is needed here.
    djui_connect_menu_open();
    ldn_begin_connect(index);

    sLdnConnecting = false;
}

static void djui_panel_ldn_browser_refresh(UNUSED struct DjuiBase* caller) {
    if (sLdnConnecting) { return; }
    ldn_refresh_scan();
    LOG_INFO("LDN: refreshing browser (found %d networks)", ldn_get_network_count());

    if (!sLdnLayout) { return; }
    if (!sLdnPaginated) { return; }

    djui_base_destroy_children(&sLdnLayout->base);

    s32 count = ldn_get_network_count();
    if (count <= 0) {
        struct DjuiText* text = djui_text_create(&sLdnLayout->base, "SCANNING FOR WIRELESS ROOMS...");
        djui_base_set_size_type(&text->base, DJUI_SVT_RELATIVE, DJUI_SVT_RELATIVE);
        djui_base_set_size(&text->base, 1, 1);
        djui_text_set_alignment(text, DJUI_HALIGN_CENTER, DJUI_VALIGN_CENTER);
    } else {
        s32 max = count > 4 ? 4 : count;
        for (s32 i = 0; i < max; i++) {
            char label[64];
            snprintf(label, sizeof(label), "%s  (%d/%d)",
                     ldn_get_network_name(i),
                     ldn_get_network_player_count(i),
                     ldn_get_network_max_players(i));

            struct DjuiButton* btn = djui_button_create(&sLdnLayout->base, label, DJUI_BUTTON_STYLE_NORMAL, djui_panel_ldn_connect);
            btn->base.tag = (s64)(intptr_t)(size_t)i;
        }
    }

    djui_paginated_update_page_buttons(sLdnPaginated);
}

static void djui_panel_ldn_browser_on_destroy(UNUSED struct DjuiBase* caller) {
    sRefreshButton = NULL;
    sLdnLayout = NULL;
    sLdnPaginated = NULL;
    sLdnConnecting = false;
}

void djui_panel_ldn_browser_create(struct DjuiBase* caller) {
    struct DjuiThreePanel* panel = djui_panel_menu_create("LOCAL NETWORKS", true);
    struct DjuiBase* body = djui_three_panel_get_body(panel);

    sLdnPaginated = djui_paginated_create(body, 10);
    sLdnLayout = sLdnPaginated->layout;
    djui_flow_layout_set_margin(sLdnLayout, 4);

    struct DjuiRect* rect = djui_rect_container_create(body, 64);
    {
        struct DjuiButton* back = djui_button_create(&rect->base, "BACK", DJUI_BUTTON_STYLE_BACK, djui_panel_menu_back);
        djui_base_set_size(&back->base, 0.485f, 64);
        djui_base_set_alignment(&back->base, DJUI_HALIGN_LEFT, DJUI_VALIGN_TOP);

        sRefreshButton = djui_button_create(&rect->base, "REFRESH", DJUI_BUTTON_STYLE_NORMAL, djui_panel_ldn_browser_refresh);
        djui_base_set_size(&sRefreshButton->base, 0.485f, 64);
        djui_base_set_alignment(&sRefreshButton->base, DJUI_HALIGN_RIGHT, DJUI_VALIGN_TOP);
    }

    struct DjuiPanel* p = djui_panel_add(caller, panel, NULL);
    if (p) {
        p->on_panel_destroy = djui_panel_ldn_browser_on_destroy;
    }

    djui_panel_ldn_browser_refresh(NULL);
}
#else
void djui_panel_ldn_browser_create(struct DjuiBase* caller) { (void)caller; }
#endif
