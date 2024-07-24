/*
 * settings.cc
 * Copyright 2015 Eugene Paskevich
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions, and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions, and the following disclaimer in the documentation
 *    provided with the distribution.
 *
 * This software is provided "as is" and without any warranty, express or
 * implied. In no event shall the authors be liable for any damages arising from
 * the use of this software.
 */

#include "settings.h"

#include <libfauxdcore/hook.h>
#include <libfauxdcore/i18n.h>
#include <libfauxdcore/preferences.h>

const char * const moonstone_defaults[] = {
    "infoarea_show_vis", "TRUE",
    "infoarea_show_art", "TRUE",
    "_infoarea_show_art_saved", "TRUE",
    "infoarea_hide_fallback_art", "FALSE",
    "_infoarea_hide_fallback_art_saved", "FALSE",
    "infoarea_visible", "TRUE",
    "menu_visible", "TRUE",
    "playlist_tabs_visible", aud::numeric_string<PlaylistTabVisibility::AutoHide>::str,
    "entry_count_visible", "FALSE",
    "close_button_visible", "TRUE",
    "tabheight", "20",
    "autoscroll", "TRUE",
    "playlist_columns", DEFAULT_COLUMNS,
    "playlist_headers", "TRUE",
    "playlist_headers_bold", "TRUE",
    "show_remaining_time", "FALSE",
    "show_filepopup_for_tuple", "TRUE",
    "shaded", "FALSE",
    "window_w", "768",
    "window_h", "480",
    nullptr
};

static void moonstone_update_playlist_settings ()
{
    hook_call ("moonstone update playlist settings", nullptr);
}

static void moonstone_update_playlist_headers ()
{
    hook_call("moonstone update playlist headers", nullptr);
}

static const ComboItem playlist_tabs_options[] = {
    ComboItem (N_("Always"), PlaylistTabVisibility::Always),
    ComboItem (N_("Auto-hide"), PlaylistTabVisibility::AutoHide),
    ComboItem (N_("Never"), PlaylistTabVisibility::Never)
};

static const PreferencesWidget moonstone_widgets[] = {
    WidgetLabel (N_("<b>Playlist Tabs</b>")),
    WidgetCombo (N_("Show playlist tabs:"),
        WidgetInt ("moonstone", "playlist_tabs_visible", moonstone_update_playlist_settings),
        {{playlist_tabs_options}}),
    WidgetCheck (N_("Show entry counts"),
        WidgetBool ("moonstone", "entry_count_visible", moonstone_update_playlist_settings)),
    WidgetCheck (N_("Show close buttons"),
        WidgetBool ("moonstone", "close_button_visible", moonstone_update_playlist_settings)),
    WidgetLabel (N_("<b>Playlist Columns</b>")),
    WidgetCheck (N_("Show column headers"),
        WidgetBool ("moonstone", "playlist_headers", moonstone_update_playlist_settings)),
    WidgetCheck(N_("Use bold font for column headers"),
        WidgetBool("moonstone", "playlist_headers_bold", moonstone_update_playlist_headers)),
    WidgetLabel (N_("<b>Miscellaneous</b>")),
    WidgetCheck (N_("Scroll on song change"),
        WidgetBool ("moonstone", "autoscroll"))
};

const PluginPreferences moonstone_prefs = {{moonstone_widgets}};
