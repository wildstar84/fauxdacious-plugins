/*
 * info-bar-plugin.cc (aka. "Mini-Fauxdacious")
 * Copyright 2021 Jim Turner
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

#include <glib.h>
#include <gdk/gdkkeysyms.h>
#include <gtk/gtk.h>
#include <string.h>

#define AUD_GLIB_INTEGRATION
#include <libfauxdcore/audstrings.h>
#include <libfauxdcore/drct.h>
#include <libfauxdcore/hook.h>
#include <libfauxdcore/i18n.h>
#include <libfauxdcore/plugin.h>
#include <libfauxdcore/runtime.h>
#include <libfauxdcore/playlist.h>
#include <libfauxdcore/hook.h>
#include <libfauxdcore/preferences.h>

#include <libfauxdgui/gtk-compat.h>
#include <libfauxdgui/libfauxdgui.h>
#include <libfauxdgui/libfauxdgui-gtk.h>
#include <libfauxdgui/ui_infoarea.h>

// #include "../ui-common/menu-ops.h"

static bool plugin_running = false;
static bool show_toolbar;
static bool show_volume = false;
static bool show_slider = false;
static bool show_label = false;
static bool slider_is_moving = false;
static bool moonstone_toolbarstyle;
static int slider_seek_time = -1;
static bool volume_slider_is_moving = false;
static unsigned long volume_change_handler_id;
static GtkWidget * main_window;
static GtkWidget * toolbar;
static GtkWidget * slider, * label_time, * volume, * volume_popup;
static GtkToolItem * button_open, * button_add, * button_opendir, * button_adddir,
        * button_openurl, * button_addurl, * button_settings,
        * button_prev, * button_play,
        * button_stop, * button_next, * button_record, * button_repeat,
        * button_shuffle, * button_quit;

static void update_toggles (void *, void *);

static bool is_floating ()
{
    bool floating = false;
    GtkWidget * parent_window = gtk_widget_get_parent (main_window);
    if (parent_window)
    {
        GdkWindow * gdktop = gtk_widget_get_window (main_window);
        if (gdktop)
        {
            /* WE'RE SURE THAT WE'RE FLOATING, ALLOW TOOLBAR TOGGLE!: */
            GdkWindow * gtkfloating = gdk_window_get_toplevel (gdktop);
            if (gtkfloating && gtkfloating == gdktop)
                floating = true;
        }
    }
    return floating;
}

/* JWT:CALLED TO TOGGLE THE TOOLBAR (BY 2 OTHER FUNCTIONS) ONLY IF FLOATING!: */
static void do_toolbar_toggle_fn ()
{
    int w = 0;
    int h = 0;
    int fixed_height = 0;
    GtkWidget * plugin_window = gtk_widget_get_toplevel (main_window);

    if (GTK_IS_WINDOW (plugin_window))
    {
        gtk_window_get_size ((GtkWindow *) plugin_window, & w, & h);
        fixed_height = 102;  /* JWT:FIXME SOMEHOW, SOMEDAY! */
    }
    gtk_window_get_size ((GtkWindow *) plugin_window, & w, & h);
    if (show_toolbar)  /* UNSHADE: */
    {
        gtk_widget_set_no_show_all (toolbar, false);
        gtk_widget_show_all (toolbar);
        gtk_widget_show (toolbar);

        GtkAllocation alloc;
        gtk_widget_get_allocation (toolbar, & alloc);
        if (alloc.height <= 1)
            alloc.height = aud_get_int ("minifauxdacious-gtk", "toolbar_height");

        aud_set_int ("gtkui-layout", "mini_fauxdacious_w", w);
        if (alloc.height > 1)
        {
            aud_set_int ("minifauxdacious-gtk", "toolbar_height", alloc.height);
            if (fixed_height > 0)
            {
                int wtb = aud_get_int ("gtkui-layout", "mini_fauxdacious_w_tb");
                if (wtb > 100)
                    w = wtb;

                gtk_window_resize ((GtkWindow *) plugin_window, w, fixed_height + alloc.height);
            }
        }
    }
    else  /* SHADE: */
    {
        gtk_widget_hide (toolbar);

        aud_set_int ("gtkui-layout", "mini_fauxdacious_w_tb", w);
        if (fixed_height > 0)
        {
            int wtb = aud_get_int ("gtkui-layout", "mini_fauxdacious_w");
            if (wtb > 100)
                w = wtb;

            gtk_window_resize ((GtkWindow *) plugin_window, w, fixed_height);
        }
    }

    aud_set_bool ("minifauxdacious-gtk", "show_toolbar", show_toolbar);
}

/* CALLED INTERNALLY BY PLUGIN-OPTIONS CHECKBOX (WHICH ALREADY PRETOGGLES B4 CALLING THIS): */
static void show_toolbar_toggled_fn ()
{
    /* JWT:ONLY ALLOW TOOLBAR TOGGLE IF FLOATING (UNDOCKED) - MESSES UP LAYOUT IF DOCKED!: */
    if (is_floating ())
        do_toolbar_toggle_fn ();
    else  /* SO WE MUST INSTEAD PUT BACK HOW IT WAS SET (NO CHANGE)! */
    {
        show_toolbar = ! show_toolbar;  // PUT IT BACK (WE COULDN'T DO IT!
        AUDWARN ("w:Mini-Fauxdacious must be undocked to toggle toolbar!\n");
    }
}

/* CALLED BY info_bar (PRESSING "T" or DOUBLE-CLICK) TO TOGGLE TOOLBAR SHADE: */
static void show_toolbar_toggle_fn ()
{
    /* JWT:ONLY ALLOW TOOLBAR TOGGLE IF FLOATING (UNDOCKED) - MESSES UP LAYOUT IF DOCKED!: */
    if (is_floating ())
    {
        show_toolbar = ! show_toolbar;
        do_toolbar_toggle_fn ();
    }
    else
        AUDWARN ("w:Mini-Fauxdacious must be undocked to toggle toolbar!\n");
}

static void toolbarstyle_init_fn ()
{
#ifdef USE_GTK3
    GtkStyleContext * context = gtk_widget_get_style_context (toolbar);
    GtkCssProvider * provider = gtk_css_provider_new ();
    gtk_css_provider_load_from_data (provider,
            "#minifauxd_toolbar {"
            " padding-top: 0px;"
            " padding-bottom: 0px;"
            " padding-left: 0px;"
            " padding-right: 0px; }"
            ".Moonstone {"
            " background-color: #999999; }",
            -1, nullptr);
    gtk_style_context_add_provider (context,
            GTK_STYLE_PROVIDER (provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref (provider);
#endif
}

static void toolbarstyle_toggled_fn ()
{
#ifdef USE_GTK3
    GtkStyleContext * context = gtk_widget_get_style_context (toolbar);
    if (moonstone_toolbarstyle)
        gtk_style_context_add_class (context, "Moonstone");
    else
        gtk_style_context_remove_class (context, "Moonstone");

    aud_set_bool ("minifauxdacious-gtk", "moonstone_toolbarstyle", moonstone_toolbarstyle);
#endif
}

static void append_str (char * buf, int bufsize, const char * str)
{
    snprintf (buf + strlen (buf), bufsize - strlen (buf), "%s", str);
}

static void set_time_label (int time, int len)
{
    if (! plugin_running)
        return;

    char s[128] = "<b>";

    if (len > 0 && aud_get_bool ("gtkui", "show_remaining_time"))
        append_str (s, sizeof s, str_format_time (len - time));
    else
        append_str (s, sizeof s, str_format_time (time));

    if (len > 0)
    {
        append_str (s, sizeof s, " / ");
        append_str (s, sizeof s, str_format_time (len));

        int a, b;
        aud_drct_get_ab_repeat (a, b);

        if (a >= 0)
        {
            append_str (s, sizeof s, " A=");
            append_str (s, sizeof s, str_format_time (a));
        }

        if (b >= 0)
        {
            append_str (s, sizeof s, " B=");
            append_str (s, sizeof s, str_format_time (b));
        }
    }

    append_str (s, sizeof s, "</b>");

    /* only update label if necessary */
    if (strcmp (gtk_label_get_label ((GtkLabel *) label_time), s))
        gtk_label_set_markup ((GtkLabel *) label_time, s);
}

static void set_slider (int time)
{
    if (! plugin_running)
        return;

    gtk_range_set_value ((GtkRange *) slider, time);
}

static void time_counter_cb (void * = nullptr)
{
    if (! show_toolbar || slider_is_moving || ! plugin_running)
        return;

    slider_seek_time = -1;  // delayed reset to avoid seeking twice

    int time = aud_drct_get_time ();
    int length = aud_drct_get_length ();

    if (length > 0 && show_slider)
        set_slider (time);

    if (show_label)
        set_time_label (time, length);
}

static void do_seek (int time)
{
    if (! plugin_running)
        return;

    aud_drct_seek (time);
    if (show_slider || show_label)
        time_counter_cb ();
}

static gboolean ui_slider_change_value_cb (GtkRange *, GtkScrollType, double value)
{
    if (! plugin_running)
        return false;

    int length = aud_drct_get_length ();
    int time = aud::clamp ((int) value, 0, length);

    if (slider_is_moving)
    {
        slider_seek_time = time;
        if (show_label)
            set_time_label (time, length);
    }
    else if (time != slider_seek_time)  // avoid seeking twice
        do_seek (time);

    return false;
}

/* return an object property if it exists, otherwise false */
static bool get_boolean_prop (void * obj, const char * prop)
{
    gboolean value = false;
    if (g_object_class_find_property (G_OBJECT_GET_CLASS (obj), prop))
        g_object_get (obj, prop, & value, nullptr);

    return value;
}

static gboolean ui_slider_button_press_cb (GtkWidget * widget, GdkEventButton * event)
{
    bool primary_warps = get_boolean_prop (gtk_widget_get_settings (widget),
            "gtk-primary-button-warps-slider");

    if (event->button == 1 && ! primary_warps)
        event->button = 2;

    slider_is_moving = true;
    return false;
}

static gboolean ui_slider_button_release_cb (GtkWidget * widget, GdkEventButton * event)
{
    bool primary_warps = get_boolean_prop (gtk_widget_get_settings (widget),
            "gtk-primary-button-warps-slider");

    if (event->button == 1 && ! primary_warps)
        event->button = 2;

    if (slider_seek_time != -1)
        do_seek (slider_seek_time);

    slider_is_moving = false;
    return false;
}

static gboolean ui_volume_value_changed_cb (GtkButton *, double volume)
{
    if (! plugin_running)
        return false;

    aud_drct_set_volume_main (volume);
    return true;
}

static void toggle_mute ()
{
    int current_volume = aud_drct_get_volume_main ();
    if (current_volume)
    {
        aud_set_int ("audacious", "_premuted_volume", current_volume);
        aud_drct_set_volume_main (0);
    }
    else
        aud_drct_set_volume_main (aud_get_int ("audacious", "_premuted_volume"));
}

static gboolean ui_volume_button_press_cb (GtkWidget *, GdkEvent * event)
{
    GdkEventButton * button_event = (GdkEventButton *) event;

    /* ignore double and triple clicks */
    if (button_event->type != GDK_BUTTON_PRESS)
        return false;
    else if (button_event->button == 1)
        /* handle left mouse button */
        volume_slider_is_moving = true;
    else if (button_event->button == 2)
        /* (un)mute with middle mouse button */
        toggle_mute ();

    return false;
}

/* JWT:THIS KLUDGE NEEDED TO RETAIN KEYBOARD FOCUS AFTER CLOSING VOLUME SLIDER "WINDOW"!: */
static void ui_volume_unmap_cb (GtkButton *)
{
    gtk_widget_grab_focus (main_window);
}

static gboolean ui_volume_button_release_cb (GtkWidget *, GdkEvent * event)
{
    GdkEventButton * button_event = (GdkEventButton *) event;

    if (button_event->button == 1)
        volume_slider_is_moving = false;

    return false;
}

static void ui_volume_slider_update (void * button)
{
    if (! show_toolbar || volume_slider_is_moving || ! plugin_running)
        return;

    int volume = aud_drct_get_volume_main ();

    if (volume == (int) gtk_scale_button_get_value ((GtkScaleButton *) button))
        return;

    g_signal_handler_block (button, volume_change_handler_id);
    gtk_scale_button_set_value ((GtkScaleButton *) button, volume);
    g_signal_handler_unblock (button, volume_change_handler_id);
}

static void set_slider_length (int length)
{
    if (length > 0)
    {
        gtk_range_set_range ((GtkRange *) slider, 0, length);
        gtk_widget_show (slider);
    }
    else
        gtk_widget_hide (slider);
}

static void update_step_size ()
{
    if (plugin_running && show_slider)
    {
        int step_size = aud_get_int (0, "step_size");
        // set half the step size because GTK doubles it for scroll events
        gtk_range_set_increments ((GtkRange *) slider, step_size * 500, step_size * 500);
    }
}

static void update_volume_delta ()
{
    if (plugin_running && show_volume)
    {
        int volume_delta = aud_get_int (0, "volume_delta");
        GtkAdjustment * adjustment = gtk_scale_button_get_adjustment ((GtkScaleButton *) volume);
        gtk_adjustment_set_step_increment (adjustment, volume_delta);
        gtk_adjustment_set_page_increment (adjustment, volume_delta);
    }
}

static void set_button_icon (GtkToolButton * button, const char * icon)
{
    if (aud_get_bool ("gtkui", "symbolic_icons"))
        gtk_tool_button_set_icon_name (button, str_concat ({icon, "-symbolic"}));
    else
        gtk_tool_button_set_icon_name (button, icon);
}

static void pause_cb ()
{
    if (plugin_running && aud_get_bool ("minifauxdacious-gtk", "playback-btns"))
    {
        bool paused = aud_drct_get_paused ();
        set_button_icon ((GtkToolButton *) button_play,
                paused ? "media-playback-start" : "media-playback-pause");
        gtk_tool_item_set_tooltip_text (button_play, paused ? _("Play") : _("Pause"));
    }
}

static void ui_playback_begin ()
{
    pause_cb ();
    gtk_widget_set_sensitive ((GtkWidget *) button_stop, true);
}

static void ui_playback_ready ()
{
    if (show_slider || show_label)
    {
        if (show_slider)
            set_slider_length (aud_drct_get_length ());

        time_counter_cb ();

        /* update time counter 4 times a second */
        timer_add (TimerRate::Hz4, time_counter_cb);

        if (show_label)
            gtk_widget_show (label_time);
    }
}

static void ui_playback_stop ()
{
    if (show_slider || show_label)
        timer_remove (TimerRate::Hz4, time_counter_cb);

    set_button_icon ((GtkToolButton *) button_play, "media-playback-start");
    gtk_tool_item_set_tooltip_text (button_play, _("Play"));
    gtk_widget_set_sensitive ((GtkWidget *) button_stop, false);
    if (show_slider)
        gtk_widget_hide (slider);
    if (show_label)
        gtk_widget_hide (label_time);
}

static void ui_hooks_associate ()
{
    hook_associate ("playback begin", (HookFunction) ui_playback_begin, nullptr);
    hook_associate ("playback ready", (HookFunction) ui_playback_ready, nullptr);
    hook_associate ("playback pause", (HookFunction) pause_cb, nullptr);
    hook_associate ("playback unpause", (HookFunction) pause_cb, nullptr);
    hook_associate ("playback stop", (HookFunction) ui_playback_stop, nullptr);
//?    hook_associate ("set stop_after_current_song", (HookFunction) update_toggles, nullptr);
    hook_associate ("enable record", (HookFunction) update_toggles, nullptr);
    hook_associate ("set record", (HookFunction) update_toggles, nullptr);
    hook_associate ("set repeat", (HookFunction) update_toggles, nullptr);
    hook_associate ("set shuffle", (HookFunction) update_toggles, nullptr);
    hook_associate ("set step_size", (HookFunction) update_step_size, nullptr);
    hook_associate ("set volume_delta", (HookFunction) update_volume_delta, nullptr);
}

static void ui_hooks_disassociate ()
{
    hook_dissociate ("set volume_delta", (HookFunction) update_volume_delta, nullptr);
    hook_dissociate ("set step_size", (HookFunction) update_step_size, nullptr);
    hook_dissociate ("set shuffle", (HookFunction) update_toggles);
    hook_dissociate ("set repeat", (HookFunction) update_toggles);
    hook_dissociate ("set record", (HookFunction) update_toggles);
    hook_dissociate ("enable record", (HookFunction) update_toggles);
//?    hook_dissociate ("set stop_after_current_song", (HookFunction) update_toggles, nullptr);
    hook_dissociate ("playback stop", (HookFunction) ui_playback_stop);
    hook_dissociate ("playback unpause", (HookFunction) pause_cb);
    hook_dissociate ("playback pause", (HookFunction) pause_cb);
    hook_dissociate ("playback ready", (HookFunction) ui_playback_ready);
    hook_dissociate ("playback begin", (HookFunction) ui_playback_begin);
}

static GtkToolItem * toolbar_button_add (GtkWidget * toolbar,
 void (* callback) (), const char * icon, const char * tooltip)
{
    GtkToolItem * item = gtk_tool_button_new (nullptr, nullptr);
    set_button_icon ((GtkToolButton *) item, icon);
    gtk_tool_item_set_tooltip_text (item, tooltip);
    gtk_toolbar_insert ((GtkToolbar *) toolbar, item, -1);
    g_signal_connect (item, "clicked", callback, nullptr);
    return item;
}

static GtkToolItem * toggle_button_new (const char * icon, const char * tooltip,
 void (* toggled) (GtkToggleToolButton *))
{
    GtkToolItem * item = gtk_toggle_tool_button_new ();
    set_button_icon ((GtkToolButton *) item, icon);
    gtk_tool_item_set_tooltip_text (item, tooltip);
    g_signal_connect (item, "toggled", (GCallback) toggled, nullptr);
    return item;
}

static GtkWidget * markup_label_new (const char * str)
{
    GtkWidget * label = gtk_label_new (str);
    gtk_label_set_use_markup ((GtkLabel *) label, true);
    return label;
}

static void button_open_pressed ()
{
    audgui_run_filebrowser (true);
}

static void button_add_pressed ()
{
    audgui_run_filebrowser (false);
}

static void button_openfolder_pressed ()
{
    audgui_run_filebrowser (true);
}

static void button_addfolder_pressed ()
{
    audgui_run_filebrowser (false);
}

static void button_openurl_pressed ()
{
    audgui_show_add_url_window (true);
}

static void button_addurl_pressed ()
{
    audgui_show_add_url_window (false);
}

static void update_toggles (void * = nullptr, void * = nullptr)
{
    if (aud_get_bool ("minifauxdacious-gtk", "record-btn"))
    {
        gtk_widget_set_visible ((GtkWidget *) button_record, aud_drct_get_record_enabled ());

        gtk_toggle_tool_button_set_active ((GtkToggleToolButton *) button_record,
               aud_get_bool (nullptr, "record"));
    }
    if (aud_get_bool ("minifauxdacious-gtk", "repeat-btn"))
        gtk_toggle_tool_button_set_active ((GtkToggleToolButton *) button_repeat,
                aud_get_bool (nullptr, "repeat"));
    if (aud_get_bool ("minifauxdacious-gtk", "shuffle-btn"))
        gtk_toggle_tool_button_set_active ((GtkToggleToolButton *) button_shuffle,
                aud_get_bool (nullptr, "shuffle"));
}

static void toggle_repeat (GtkToggleToolButton * button)
{
    aud_set_bool (nullptr, "repeat", gtk_toggle_tool_button_get_active (button));
}

static void toggle_shuffle (GtkToggleToolButton * button)
{
    aud_set_bool (nullptr, "shuffle", gtk_toggle_tool_button_get_active (button));
}

static void toggle_record (GtkToggleToolButton * button)
{
    aud_set_bool (nullptr, "record", gtk_toggle_tool_button_get_active (button));
}

class InfoBarPlugin : public GeneralPlugin {
public:
    static const char about[];
    static const char * const defaults[];
    static const PreferencesWidget widgets[];
    static const PluginPreferences prefs;
    static constexpr PluginInfo info = {
        N_("Mini-Fauxdacious"),
        PACKAGE,
        about,   // about
        & prefs, // prefs
        PluginGLibOnly
    };

    constexpr InfoBarPlugin () : GeneralPlugin (info, false) {}

    bool init ();
    void * get_gtk_widget ();
};

const char InfoBarPlugin::about[] =
 N_("Mini-Fauxdacious Plugin for Fauxdacious\n\n"
    "Copyright (C) 2021 Jim Turner <turnerjw784@yahoo.com>.\n\n"
    "Creates an InfoBar as a dockable window, which when main-\n"
    "window is minimized, provides a minimalist Fauxdacious!");

const char * const InfoBarPlugin::defaults[] = {
    "show_toolbar",           "FALSE",
    "moonstone_toolbarstyle", "FALSE",

    "openfiles-btn",      "FALSE",
    "addfiles-btn",       "TRUE",
    "openfolder-btn",     "FALSE",
    "addfolder-btn",      "FALSE",
    "openurl-btn",        "FALSE",
    "addurl-btn",         "FALSE",
    "settings-btn",       "FALSE",
    "playback-btns",      "TRUE",
    "record-btn",         "TRUE",
    "slider-btn",         "TRUE",
    "time-label",         "TRUE",
    "repeat-btn",         "TRUE",
    "shuffle-btn",        "TRUE",
    "volume-btn",         "TRUE",
    "quit-btn",           "FALSE",
    "separators",         "TRUE",
    nullptr
};

const PreferencesWidget InfoBarPlugin::widgets[] = {
    WidgetLabel(N_("<b>Mini-Fauxdacious Configuration</b>")),
    WidgetCheck (N_("Show toolbar under info-bar?"),
        WidgetBool (show_toolbar, show_toolbar_toggled_fn)),
#ifdef USE_GTK3
    WidgetCheck (N_("Audacious Moonstone-themed toolbar? (GTK-3 only)"),
        WidgetBool (moonstone_toolbarstyle, toolbarstyle_toggled_fn)),
#endif
    WidgetLabel (N_("<b>Select toolbar-buttons:</b>")),
    WidgetCheck (N_("Open Files"),
        WidgetBool ("minifauxdacious-gtk", "openfiles-btn")),
    WidgetCheck (N_("Add Files"),
        WidgetBool ("minifauxdacious-gtk", "addfiles-btn")),
    WidgetCheck (N_("Open Folder"),
        WidgetBool ("minifauxdacious-gtk", "openfolder-btn")),
    WidgetCheck (N_("Add Folder"),
        WidgetBool ("minifauxdacious-gtk", "addfolder-btn")),
    WidgetCheck (N_("Open URL"),
        WidgetBool ("minifauxdacious-gtk", "openurl-btn")),
    WidgetCheck (N_("Add URL"),
        WidgetBool ("minifauxdacious-gtk", "addurl-btn")),
    WidgetCheck (N_("Settings"),
        WidgetBool ("minifauxdacious-gtk", "settings-btn")),
    WidgetCheck (N_("Playback controls (4 buttons)"),
        WidgetBool ("minifauxdacious-gtk", "playback-btns")),
    WidgetCheck (N_("Record"),
        WidgetBool ("minifauxdacious-gtk", "record-btn")),
    WidgetCheck (N_("Time Slider"),
        WidgetBool ("minifauxdacious-gtk", "slider-btn")),
    WidgetCheck (N_("Time Display"),
        WidgetBool ("minifauxdacious-gtk", "time-label")),
    WidgetCheck (N_("Repeat"),
        WidgetBool ("minifauxdacious-gtk", "repeat-btn")),
    WidgetCheck (N_("Shuffle"),
        WidgetBool ("minifauxdacious-gtk", "shuffle-btn")),
    WidgetCheck (N_("Volume"),
        WidgetBool ("minifauxdacious-gtk", "volume-btn")),
    WidgetCheck (N_("Quit"),
        WidgetBool ("minifauxdacious-gtk", "quit-btn")),
    WidgetCheck (N_("Separators"),
        WidgetBool ("minifauxdacious-gtk", "separators")),
};

const PluginPreferences InfoBarPlugin::prefs = {{widgets}};

void show_hide_infoarea_art ()
{
    bool show = aud_get_bool ("gtkui", "infoarea_show_art");

    ui_infoarea_show_art (show);
    aud_set_bool ("albumart", "_infoarea_show_art_saved", show);
}

void show_hide_infoarea_vis ()
{
    /* only turn on visualization if interface is shown */
    ui_infoarea_show_vis (aud_get_bool ("gtkui", "infoarea_show_vis"));
}

static gboolean infobar_btnpress_cb (GtkWidget * widget, GdkEventKey * event)
{
    if (event->type == GDK_2BUTTON_PRESS)
        show_toolbar_toggle_fn ();

    return false;
}

static gboolean infobar_keypress_cb (GtkWidget *, GdkEventKey * event)
{
    switch (event->state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK | GDK_MOD1_MASK))
    {
      case 0:
      {
        switch (event->keyval)
        {
        case GDK_KEY_Left:
            if (aud_drct_get_playing ())
                aud_drct_seek (aud_drct_get_time () - aud_get_int (0, "step_size") * 1000);
            break;
        case GDK_KEY_Right:
            if (aud_drct_get_playing ())
                aud_drct_seek (aud_drct_get_time () + aud_get_int (0, "step_size") * 1000);
            break;
        case GDK_KEY_Up:
            aud_drct_set_volume_main (aud_drct_get_volume_main () + aud_get_int (0, "volume_delta"));
            break;
        case GDK_KEY_Down:
            aud_drct_set_volume_main (aud_drct_get_volume_main () - aud_get_int (0, "volume_delta"));
            break;
        case 't':
            show_toolbar_toggle_fn ();
            break;
        case 'z':
            aud_drct_pl_prev ();
            break;
        case 'x':
            aud_drct_play ();
            break;
        case 'c':
        case ' ':
            aud_drct_pause ();
            break;
        case 'v':
            aud_drct_stop ();
            break;
        case 'b':
            aud_drct_pl_next ();
            break;
        case 'm':
            toggle_mute ();
            break;
        }

        break;
      }
      case GDK_MOD1_MASK:        // [Alt]
        {
            PluginHandle * plugin;

            switch (event->keyval)
            {
              case 'a':
                plugin = aud_plugin_lookup_basename ("albumart");
                if (plugin)
                    aud_plugin_enable (plugin, (! aud_plugin_get_enabled (plugin)));
                break;
              case 'b':
                plugin = aud_plugin_lookup_basename ("blur_scope");
                if (plugin)
                    aud_plugin_enable (plugin, (! aud_plugin_get_enabled (plugin)));
                break;
              case 'c':
                plugin = aud_plugin_lookup_basename ("cairo-spectrum");
                if (plugin)
                    aud_plugin_enable (plugin, (! aud_plugin_get_enabled (plugin)));
                break;
              case 'g':
                plugin = aud_plugin_lookup_basename ("gnomeshortcuts");
                if (plugin)
                    aud_plugin_enable (plugin, (! aud_plugin_get_enabled (plugin)));
                break;
              case 'l':
                plugin = aud_plugin_lookup_basename ("lyricwiki");
                if (plugin)
                    aud_plugin_enable (plugin, (! aud_plugin_get_enabled (plugin)));
                break;
              case 'm':
                plugin = aud_plugin_lookup_basename ("info-bar-plugin-gtk");
                if (plugin)
                    aud_plugin_enable (plugin, (! aud_plugin_get_enabled (plugin)));
                break;
              case 'o':
                plugin = aud_plugin_lookup_basename ("gl-spectrum");
                if (plugin)
                    aud_plugin_enable (plugin, (! aud_plugin_get_enabled (plugin)));
                break;
              case 'p':
                plugin = aud_plugin_lookup_basename ("playlist-manager");
                if (plugin)
                    aud_plugin_enable (plugin, (! aud_plugin_get_enabled (plugin)));
                break;
              case 'q':
                aud_quit ();
                break;
              case 's':
                plugin = aud_plugin_lookup_basename ("search-tool");
                if (plugin)
                    aud_plugin_enable (plugin, (! aud_plugin_get_enabled (plugin)));
                break;
              case 'v':
                plugin = aud_plugin_lookup_basename ("video_display");
                if (plugin)
                    aud_plugin_enable (plugin, (! aud_plugin_get_enabled (plugin)));
                break;
              default:
                return false;
            }
        }
      default:
        return false;
    }

    return true;
}

/* CALLED ON SHUTDOWN TO CLEAN UP: */
static void infobar_cleanup (GtkWidget * widget)
{
    if (show_slider || show_label)
        timer_remove (TimerRate::Hz4, time_counter_cb);
    if (show_volume)
        timer_remove (TimerRate::Hz4, ui_volume_slider_update);

    plugin_running = false;
    show_slider = false;
    show_label = false;
    show_volume = false;
    ui_hooks_disassociate ();

    gtk_widget_destroy (widget);
    widget = nullptr;
}

bool InfoBarPlugin::init ()
{
    aud_config_set_defaults ("minifauxdacious-gtk", defaults);
    show_toolbar = aud_get_bool ("minifauxdacious-gtk", "show_toolbar");
    moonstone_toolbarstyle = aud_get_bool ("minifauxdacious-gtk", "moonstone_toolbarstyle");
    return true;
}

/* CALLED ON STARTUP (WIDGET CREATION): */
void * InfoBarPlugin::get_gtk_widget ()
{
    /* JWT:HIDE EMBEDDED (CLASSIC) INFOBAR WHILE THIS PLUGIN IS ACTIVE: */
    bool show = aud_get_bool ("gtkui", "infoarea_visible");
    aud_set_bool ("gtkui", "_infoarea_was_visible", show);
    if (show)
    {
        aud_set_bool ("gtkui", "infoarea_visible", false);
        hook_call ("gtkui toggle infoarea", nullptr);
    }
    GtkWidget * mouse_eventbox = gtk_event_box_new ();
    GtkWidget * widget = ui_infoarea_new ();
    gtk_container_add ((GtkContainer *) mouse_eventbox, widget);
    GtkWidget * vbox_outer = audgui_vbox_new (0);
    main_window = vbox_outer;
    toolbar = gtk_toolbar_new ();

    GtkAllocation alloc;
    gtk_widget_get_allocation (toolbar, & alloc);
    if (alloc.height > 1)
        aud_set_int ("minifauxdacious-gtk", "toolbar_height", alloc.height);

    gtk_widget_set_name (toolbar, "minifauxd_toolbar");
    gtk_toolbar_set_style ((GtkToolbar *) toolbar, GTK_TOOLBAR_ICONS);
    gtk_toolbar_set_show_arrow ((GtkToolbar *) toolbar, false);
    gtk_box_pack_start ((GtkBox *) vbox_outer, mouse_eventbox, false, false, 0);
    gtk_box_pack_end ((GtkBox *) vbox_outer, toolbar, false, false, 0);

#ifdef USE_GTK3
    GtkStyleContext * context = gtk_widget_get_style_context (toolbar);
    gtk_style_context_add_class (context, GTK_STYLE_CLASS_PRIMARY_TOOLBAR);
#endif

    /* open/add buttons */
    if (aud_get_bool ("minifauxdacious-gtk", "openfiles-btn"))
        button_open = toolbar_button_add (toolbar, button_open_pressed, "document-open", _("Open Files"));
    if (aud_get_bool ("minifauxdacious-gtk", "addfiles-btn"))
        button_add = toolbar_button_add (toolbar, button_add_pressed, "list-add", _("Add Files"));
    if (aud_get_bool ("minifauxdacious-gtk", "openfolder-btn"))
        button_opendir = toolbar_button_add (toolbar, button_openfolder_pressed, "document-open", _("Open Folder"));
    if (aud_get_bool ("minifauxdacious-gtk", "addfolder-btn"))
        button_adddir = toolbar_button_add (toolbar, button_addfolder_pressed, "list-add", _("Add Folder"));
    if (aud_get_bool ("minifauxdacious-gtk", "openurl-btn"))
        button_openurl = toolbar_button_add (toolbar, button_openurl_pressed, "folder-remote", _("Open URL"));
    if (aud_get_bool ("minifauxdacious-gtk", "addurl-btn"))
        button_addurl = toolbar_button_add (toolbar, button_addurl_pressed, "folder-remote", _("Add URL"));
    if (aud_get_bool ("minifauxdacious-gtk", "settings-btn"))
        button_settings = toolbar_button_add (toolbar, audgui_show_prefs_window, "preferences-system", N_("Settings"));

    /* playback buttons (all 4 or none): */
    if (aud_get_bool ("minifauxdacious-gtk", "playback-btns"))
    {
        button_prev = toolbar_button_add (toolbar, aud_drct_pl_prev, "media-skip-backward", _("Previous"));
        button_play = toolbar_button_add (toolbar, aud_drct_play_pause, "media-playback-start", _("Play"));
        button_stop = toolbar_button_add (toolbar, aud_drct_stop, "media-playback-stop", _("Stop"));
        button_next = toolbar_button_add (toolbar, aud_drct_pl_next, "media-skip-forward", _("Next"));
    }
    if (aud_get_bool ("minifauxdacious-gtk", "record-btn"))
    {
        button_record = toggle_button_new ("media-record", _("Record Stream"), toggle_record);
        gtk_widget_set_no_show_all ((GtkWidget *) button_record, true);
        gtk_toolbar_insert ((GtkToolbar *) toolbar, button_record, -1);
    }

    if (aud_get_bool ("minifauxdacious-gtk", "separators"))
        gtk_toolbar_insert ((GtkToolbar *) toolbar, gtk_separator_tool_item_new (), -1);

    show_slider = aud_get_bool ("minifauxdacious-gtk", "slider-btn");
    show_label = aud_get_bool ("minifauxdacious-gtk", "time-label");
    if (show_slider || show_label)
    {
        GtkToolItem * boxitem1 = gtk_tool_item_new ();
        GtkWidget * box1 = audgui_hbox_new (0);
        gtk_tool_item_set_expand (boxitem1, true);  /* FORCES RIGHT-JUSTIFICATION FROM HERE-ON: */
        gtk_toolbar_insert ((GtkToolbar *) toolbar, boxitem1, -1);
        gtk_container_add ((GtkContainer *) boxitem1, box1);

        if (show_slider)
        {
            /* time slider */
            show_slider = true;
            slider = audgui_scale_new (GTK_ORIENTATION_HORIZONTAL, nullptr);
            gtk_scale_set_draw_value ((GtkScale *) slider, false);
            gtk_widget_set_size_request (slider, audgui_get_dpi () * 5 / 4, -1);
            gtk_widget_set_can_focus (slider, false);
            gtk_box_pack_start ((GtkBox *) box1, slider, true, true, 6);

            update_step_size ();
            gtk_widget_set_no_show_all (slider, true);
        }
        if (show_label)
        {
            /* time label */
            show_label = true;
            label_time = markup_label_new (nullptr);
            gtk_box_pack_end ((GtkBox *) box1, label_time, false, false, 6);

            gtk_widget_set_no_show_all (label_time, true);
        }
        if (aud_get_bool ("minifauxdacious-gtk", "separators"))
            gtk_toolbar_insert ((GtkToolbar *) toolbar, gtk_separator_tool_item_new (), -1);
    }

    /* repeat and shuffle buttons */
    if (aud_get_bool ("minifauxdacious-gtk", "repeat-btn"))
    {
        button_repeat = toggle_button_new ("media-playlist-repeat", _("Repeat"), toggle_repeat);
        gtk_toolbar_insert ((GtkToolbar *) toolbar, button_repeat, -1);
    }
    if (aud_get_bool ("minifauxdacious-gtk", "shuffle-btn"))
    {
        button_shuffle = toggle_button_new ("media-playlist-shuffle", _("Shuffle"), toggle_shuffle);
        gtk_toolbar_insert ((GtkToolbar *) toolbar, button_shuffle, -1);
    }

    /* volume button */
    if (aud_get_bool ("minifauxdacious-gtk", "volume-btn"))
    {
        show_volume = true;
        GtkToolItem * boxitem2 = gtk_tool_item_new ();
        gtk_toolbar_insert ((GtkToolbar *) toolbar, boxitem2, -1);

        GtkWidget * box2 = audgui_hbox_new (0);
        gtk_container_add ((GtkContainer *) boxitem2, box2);

        volume = gtk_volume_button_new ();
        GtkIconSize icon_size = gtk_tool_shell_get_icon_size ((GtkToolShell *) toolbar);
        g_object_set ((GObject *) volume, "size", icon_size, nullptr);
        gtk_button_set_relief ((GtkButton *) volume, GTK_RELIEF_NONE);
        int delta = aud_get_int (0, "volume_delta");
        gtk_scale_button_set_adjustment ((GtkScaleButton *) volume,
                (GtkAdjustment *) gtk_adjustment_new (0, 0, 100, delta, delta, 0));
        gtk_widget_set_can_focus (volume, false);

        gtk_scale_button_set_value ((GtkScaleButton *) volume, aud_drct_get_volume_main ());

        gtk_box_pack_start ((GtkBox *) box2, volume, false, false, 0);
        volume_popup = gtk_scale_button_get_popup ((GtkScaleButton *) volume);
    }
    else
        show_volume = false;

    if (aud_get_bool ("minifauxdacious-gtk", "quit-btn"))
        button_quit = toolbar_button_add (toolbar, aud_quit, "application-exit", N_("Quit"));

    gtk_widget_set_can_focus (toolbar, false);

    if (! show_toolbar)
        gtk_widget_set_no_show_all (toolbar, true);

    gtk_widget_set_events (mouse_eventbox, GDK_BUTTON_PRESS_MASK);
    g_signal_connect (widget, "destroy", (GCallback) infobar_cleanup, nullptr);
    g_signal_connect (main_window, "key-press-event", (GCallback) infobar_keypress_cb, nullptr);
    g_signal_connect (mouse_eventbox, "button_press_event", (GCallback) infobar_btnpress_cb, nullptr);

    gtk_widget_show (widget);
    plugin_running = true;

    show_hide_infoarea_art ();
    show_hide_infoarea_vis ();

    toolbarstyle_init_fn ();
    toolbarstyle_toggled_fn ();

// NO IMPROVEMENT - JUST ADDS PADDING: gtk_toolbar_set_icon_size ((GtkToolbar *) toolbar, GTK_ICON_SIZE_SMALL_TOOLBAR);
    ui_hooks_associate ();

    if (show_slider)
    {
        g_signal_connect (slider, "change-value", (GCallback) ui_slider_change_value_cb , nullptr);
        g_signal_connect (slider, "button-press-event", (GCallback) ui_slider_button_press_cb, nullptr);
        g_signal_connect (slider, "button-release-event", (GCallback) ui_slider_button_release_cb, nullptr);
    }

    if (show_volume)
    {
        volume_change_handler_id = g_signal_connect (volume, "value-changed", (GCallback) ui_volume_value_changed_cb, nullptr);
        g_signal_connect (volume, "button-press-event", (GCallback) ui_volume_button_press_cb, nullptr);
        g_signal_connect (volume, "button-release-event", (GCallback) ui_volume_button_release_cb, nullptr);
        if (volume_popup)  /* JWT:THIS KLUDGE NEEDED TO RETAIN KEYBOARD FOCUS AFTER CLOSING VOLUME SLIDER "WINDOW"!: */
            g_signal_connect (volume_popup, "hide", (GCallback) ui_volume_unmap_cb, nullptr);

        timer_add (TimerRate::Hz4, ui_volume_slider_update, volume);
    }


    update_toggles ();

    gtk_widget_set_can_focus (main_window, true);
    gtk_widget_set_tooltip_text (main_window, "Space: pause\nEsc: close\nUp|Down: volume\nLeft|Right: seek\nAlt-Q: Quit\nB: next\nC: pause\nM: mute\nT: toggle toolbar\nV: stop\nX: play\nZ: previous");
    gtk_widget_grab_focus (main_window);

    ui_playback_ready ();

    return main_window;
}

EXPORT InfoBarPlugin aud_plugin_instance;
