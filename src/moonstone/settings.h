/*
 * settings.h
 * Copyright 2024 Jim Turner
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

#ifndef SETTINGS_H
#define SETTINGS_H

#define DEFAULT_COLUMNS "playing artist title"

/* JWT:NOTE - THIS IS DUPLICATED IN playlist_tabs.cc DUE TO NAMESPACE ISSUES, INCLUDE ANY CHANGES THERE TOO! */
enum PlaylistTabVisibility {
    Always,
    AutoHide,
    Never
};

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

#endif

