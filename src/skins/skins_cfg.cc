/*
 * Audacious - a cross-platform multimedia player
 * Copyright (c) 2008 Tomasz Moń
 * Copyright (c) 2011 John Lindgren
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; under version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses>.
 *
 * The Audacious team does not consider modular code linking to
 * Audacious or using our public API to be a derived work.
 */

#include <string.h>
#include <gtk/gtk.h>

#include <libfauxdcore/i18n.h>
#include <libfauxdcore/runtime.h>
#include <libfauxdcore/preferences.h>
#include <libfauxdcore/audstrings.h>
#include <libfauxdgui/libfauxdgui-gtk.h>

#include "dnd.h"
#include "skins_cfg.h"
#include "main.h"
#include "vis-callbacks.h"
#include "playlistwin.h"
#include "skin.h"
#include "playlist-widget.h"
#include "textbox.h"
#include "skinselector.h"
#include "vis.h"
#include "skins_util.h"
#include "view.h"

static const char * const skins_defaults[] = {
    /* general */
    "autoscroll_songname", "TRUE",
    "mainwin_font", "Sans Bold 9",
    "mainwin_use_bitmapfont", "TRUE",
    "playlist_font", "Sans Bold 8",
    "show_remaining_time", "FALSE",
    "twoway_scroll", "FALSE",
    "use_random_skins", "FALSE",
    "oaidu_menustyle", "0",
    "try_classic_hints", "FALSE",

    /* visualizer */
    "analyzer_falloff", aud::numeric_string<FALLOFF_FAST>::str,
    "analyzer_mode", aud::numeric_string<ANALYZER_NORMAL>::str,
    "analyzer_peaks", "TRUE",
    "analyzer_type", aud::numeric_string<ANALYZER_BARS>::str,
    "peaks_falloff", aud::numeric_string<FALLOFF_SLOW>::str,
    "scope_mode", aud::numeric_string<SCOPE_DOT>::str,
    "vis_type", aud::numeric_string<VIS_ANALYZER>::str,
    "voiceprint_mode", aud::numeric_string<VOICEPRINT_NORMAL>::str,
    "vu_mode", aud::numeric_string<VU_SMOOTH>::str,

    /* windows */
    "active_titlebar_any", "FALSE",
    "always_on_top", "FALSE",
    "double_size", "FALSE",
    "equalizer_shaded", "FALSE",
    "equalizer_visible", "FALSE",
    "equalizer_x", "20",
    "equalizer_y", "136",
    "player_shaded", "FALSE",
    "player_x", "20",
    "player_y", "20",
    "playlist_shaded", "FALSE",
    "playlist_visible", "FALSE",
    "playlist_x", "295",
    "playlist_y", "20",
    "playlist_width", "275",
    "playlist_height", "232",
    "sticky", "FALSE",

    nullptr
};

skins_cfg_t config;

static GtkWidget * skin_view;

static const struct skins_cfg_boolent_t {
    const char * name;
    bool * ptr;
} skins_boolents[] =
{
    /* general */
    {"autoscroll_songname", & config.autoscroll},
    {"mainwin_use_bitmapfont", & config.mainwin_use_bitmapfont},
    {"active_titlebar_any", & config.active_titlebar_any},
    {"twoway_scroll", & config.twoway_scroll},
    {"use_random_skins", & config.use_random_skins},
    {"try_classic_hints", & config.try_classic_hints},

    /* visualizer */
    {"analyzer_peaks", & config.analyzer_peaks}
};

static const struct {
    const char * name;
    int * ptr;
} skins_numents[] =
{
    /* visualizer */
    {"analyzer_falloff", & config.analyzer_falloff},
    {"analyzer_mode", & config.analyzer_mode},
    {"analyzer_type", & config.analyzer_type},
    {"peaks_falloff", & config.peaks_falloff},
    {"scope_mode", & config.scope_mode},
    {"vis_type", & config.vis_type},
    {"voiceprint_mode", & config.voiceprint_mode},
    {"vu_mode", & config.vu_mode},

    /* windows */
    {"equalizer_x", & config.equalizer_x},
    {"equalizer_y", & config.equalizer_y},
    {"player_x", & config.player_x},
    {"player_y", & config.player_y},
    {"playlist_x", & config.playlist_x},
    {"playlist_y", & config.playlist_y},
    {"playlist_width", & config.playlist_width},
    {"playlist_height", & config.playlist_height},
    {"oaidu_menustyle", & config.oaidu_menustyle}
};

void skins_cfg_load ()
{
    aud_config_set_defaults ("skins", skins_defaults);

    for (auto & boolent : skins_boolents)
        * boolent.ptr = aud_get_bool ("skins", boolent.name);

    for (auto & nument : skins_numents)
        * nument.ptr = aud_get_int ("skins", nument.name);
}

void skins_cfg_save ()
{
    for (auto & boolent : skins_boolents)
        aud_set_bool ("skins", boolent.name, * boolent.ptr);

    for (auto & nument : skins_numents)
        aud_set_int ("skins", nument.name, * nument.ptr);
}

static void mainwin_font_set_cb ()
{
    if (! config.mainwin_use_bitmapfont)
        mainwin_info->set_font (aud_get_str ("skins", "mainwin_font"));
    else
        mainwin_info->set_font (nullptr);
}

static void playlist_font_set_cb ()
{
    playlistwin_list->set_font (aud_get_str ("skins", "playlist_font"));
}

static void autoscroll_set_cb ()
{
    if (! aud_get_bool ("skins", "mainwin_shaded"))
        mainwin_info->set_scroll (config.autoscroll);
    if (aud_get_bool ("skins", "playlist_shaded"))
        playlistwin_sinfo->set_scroll (config.autoscroll);
}

void random_skins_set_cb ()
{
    aud_set_bool ("skins", "use_random_skins", config.use_random_skins);
}

static void vis_reset_cb ()
{
    mainwin_vis->clear ();
    mainwin_svis->clear ();
    start_stop_visual (false);
}

static void try_classic_hints_cb ()
{
    aud_set_bool ("skins", "try_classic_hints", config.try_classic_hints);
    String path = aud_get_str ("skins", "skin");
    if (path && path[0])
    {
        skin_load (path);
        view_apply_skin ();
    }
}

static void oaidu_menustyle_cb ()
{
    view_apply_skin ();
}

static void * create_skin_view ();

static const PreferencesWidget font_table_elements[] = {
    WidgetFonts (N_("Player:"),
        WidgetString ("skins", "mainwin_font", mainwin_font_set_cb),
        {N_("Select main player window font:")}),
    WidgetFonts (N_("Playlist:"),
        WidgetString ("skins", "playlist_font", playlist_font_set_cb),
        {N_("Select playlist font:")})
};

static const ComboItem oaidu_menustyle[] = {
    ComboItem (N_("Xmms"), 0),
    ComboItem (N_("Legacy"), 1)
};

static const PreferencesWidget skins_widgets_general[] = {
    WidgetLabel (N_("<b>Skin</b>")),
    WidgetCustomGTK (create_skin_view),
    WidgetCheck (N_("Display active titlebar when any window is focused"),
        WidgetBool (config.active_titlebar_any, nullptr)),
    WidgetLabel (N_("<b>Fonts</b>")),
    WidgetTable ({{font_table_elements}}),
    WidgetCheck (N_("Use bitmap fonts (supports ASCII only)"),
        WidgetBool (config.mainwin_use_bitmapfont, mainwin_font_set_cb)),
    WidgetCheck (N_("Scroll song title"),
        WidgetBool (config.autoscroll, autoscroll_set_cb)),
    WidgetCheck (N_("Scroll song title in both directions"),
        WidgetBool (config.twoway_scroll, autoscroll_set_cb)),
    WidgetLabel (N_("<b>Fauxdacious Extras</b>")),
    WidgetCheck (N_("Random skin on play"),
        WidgetBool (config.use_random_skins, random_skins_set_cb)),
    WidgetCheck (N_("Use Classic skin hints (skin-classic.hints) if available)"),
        WidgetBool (config.try_classic_hints, try_classic_hints_cb)),
    WidgetCombo (N_("Winamp [OAIDU] menu style:"),
        WidgetInt (config.oaidu_menustyle, oaidu_menustyle_cb),
        {{oaidu_menustyle}})
};

static ComboItem vis_mode_elements[] = {
    ComboItem (N_("Analyzer"), VIS_ANALYZER),
    ComboItem (N_("Scope"), VIS_SCOPE),
    ComboItem (N_("Voiceprint / VU meter"), VIS_VOICEPRINT),
    ComboItem (N_("Off"), VIS_OFF)
};

static ComboItem analyzer_mode_elements[] = {
    ComboItem (N_("Normal"), ANALYZER_NORMAL),
    ComboItem (N_("Fire"), ANALYZER_FIRE),
    ComboItem (N_("Vertical lines"), ANALYZER_VLINES)
};

static ComboItem analyzer_type_elements[] = {
    ComboItem (N_("Lines"), ANALYZER_LINES),
    ComboItem (N_("Bars"), ANALYZER_BARS)
};

static ComboItem falloff_elements[] = {
    ComboItem (N_("Slowest"), FALLOFF_SLOWEST),
    ComboItem (N_("Slow"), FALLOFF_SLOW),
    ComboItem (N_("Medium"), FALLOFF_MEDIUM),
    ComboItem (N_("Fast"), FALLOFF_FAST),
    ComboItem (N_("Fastest"), FALLOFF_FASTEST)
};

static ComboItem scope_mode_elements[] = {
    ComboItem (N_("Dots"), SCOPE_DOT),
    ComboItem (N_("Line"), SCOPE_LINE),
    ComboItem (N_("Solid"), SCOPE_SOLID)
};

static ComboItem voiceprint_mode_elements[] = {
    ComboItem (N_("Normal"), VOICEPRINT_NORMAL),
    ComboItem (N_("Fire"), VOICEPRINT_FIRE),
    ComboItem (N_("Ice"), VOICEPRINT_ICE)
};

static ComboItem vu_mode_elements[] = {
    ComboItem (N_("Normal"), VU_NORMAL),
    ComboItem (N_("Smooth"), VU_SMOOTH)
};

static const PreferencesWidget analyzer_table[] = {
    WidgetCombo (N_("Coloring:"),
        WidgetInt (config.analyzer_mode, vis_reset_cb),
        {{analyzer_mode_elements}}),
    WidgetCombo (N_("Style:"),
        WidgetInt (config.analyzer_type, vis_reset_cb),
        {{analyzer_type_elements}}),
    WidgetCombo (N_("Falloff:"),
        WidgetInt (config.analyzer_falloff, vis_reset_cb),
        {{falloff_elements}}),
    WidgetCombo (N_("Peak falloff:"),
        WidgetInt (config.peaks_falloff, vis_reset_cb),
        {{falloff_elements}})
};

static const PreferencesWidget misc_table[] = {
    WidgetCombo (N_("Scope Style:"),
        WidgetInt (config.scope_mode, vis_reset_cb),
        {{scope_mode_elements}}),
    WidgetCombo (N_("Voiceprint Coloring:"),
        WidgetInt (config.voiceprint_mode, vis_reset_cb),
        {{voiceprint_mode_elements}}),
    WidgetCombo (N_("VU Meter Style:"),
        WidgetInt (config.vu_mode, vis_reset_cb),
        {{vu_mode_elements}})
};

static const PreferencesWidget skins_widgets_vis[] = {
    WidgetLabel (N_("<b>Type</b>")),
    WidgetCombo (N_("Visualization type:"),
        WidgetInt (config.vis_type, vis_reset_cb),
        {{vis_mode_elements}}),
    WidgetLabel (N_("<b>Analyzer</b>")),
    WidgetCheck (N_("Show peaks"),
        WidgetBool (config.analyzer_peaks, vis_reset_cb)),
    WidgetTable ({{analyzer_table}}),
    WidgetLabel (N_("<b>Miscellaneous</b>")),
    WidgetTable ({{misc_table}})
};

static const NotebookTab skins_notebook_tabs[] = {
    {N_("General"), {skins_widgets_general}},
    {N_("Visualization"), {skins_widgets_vis}}
};

static const PreferencesWidget skins_widgets[] = {
    WidgetNotebook ({{skins_notebook_tabs}})
};

const PluginPreferences skins_prefs = {{skins_widgets}};

void on_skin_view_drag_data_received (GtkWidget * widget, GdkDragContext * context,
 int x, int y, GtkSelectionData * selection_data, unsigned info, unsigned time, void *)
{
    const char * data = (const char *) gtk_selection_data_get_data (selection_data);
    g_return_if_fail (data);

    const char * end = strchr (data, '\r');
    if (! end)
        end = strchr (data, '\n');
    if (! end)
        end = data + strlen (data);

    StringBuf path = uri_to_filename (str_copy (data, end - data));
    if (path && file_is_archive (path))
    {
        if (! skin_load (path))
            return;

        view_apply_skin ();
        skin_install_skin (path);

        if (skin_view)
            skin_view_update ((GtkTreeView *) skin_view);
    }
}

static void * create_skin_view ()
{
    GtkWidget * scrolled = gtk_scrolled_window_new (nullptr, nullptr);
    gtk_scrolled_window_set_policy ((GtkScrolledWindow *) scrolled,
     GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);
    gtk_scrolled_window_set_shadow_type ((GtkScrolledWindow *) scrolled, GTK_SHADOW_IN);
    gtk_widget_set_size_request (scrolled, -1, audgui_get_dpi () * 3 / 2);

    skin_view = gtk_tree_view_new ();
    skin_view_realize ((GtkTreeView *) skin_view);
    skin_view_update ((GtkTreeView *) skin_view);
    gtk_container_add ((GtkContainer *) scrolled, skin_view);

    drag_dest_set (skin_view);

    g_signal_connect (skin_view, "drag-data-received",
     (GCallback) on_skin_view_drag_data_received, nullptr);
    g_signal_connect (skin_view, "destroy", (GCallback) gtk_widget_destroyed, & skin_view);

    return scrolled;
}
