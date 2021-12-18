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

#define AUD_GLIB_INTEGRATION
#include <libfauxdcore/drct.h>
#include <libfauxdcore/hook.h>
#include <libfauxdcore/i18n.h>
#include <libfauxdcore/plugin.h>
#include <libfauxdcore/runtime.h>
#include <libfauxdcore/playlist.h>
#include <libfauxdcore/hook.h>

#include <libfauxdgui/ui_infoarea.h>

class InfoBarPlugin : public GeneralPlugin {
public:
    static const char about[];

    static constexpr PluginInfo info = {
        N_("Mini-Fauxdacious"),
        PACKAGE,
        about, // about
        nullptr, // prefs
        PluginGLibOnly
    };

    constexpr InfoBarPlugin () : GeneralPlugin (info, false) {}
    void * get_gtk_widget ();
};

const char InfoBarPlugin::about[] =
 N_("Mini-Fauxdacious Plugin for Fauxdacious\n\n"
    "Copyright (C) 2021 Jim Turner <turnerjw784@yahoo.com>.\n\n"
    "Creates an InfoBar as a dockable window, which when main-\n"
    "window is minimized, provides a minimalist Fauxdacious!");

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
//                do_seek (aud_drct_get_time () - aud_get_int (0, "step_size") * 1000);
                aud_drct_seek (aud_drct_get_time () - aud_get_int (0, "step_size") * 1000);
            break;
        case GDK_KEY_Right:
            if (aud_drct_get_playing ())
//                do_seek (aud_drct_get_time () + aud_get_int (0, "step_size") * 1000);
                aud_drct_seek (aud_drct_get_time () + aud_get_int (0, "step_size") * 1000);
            break;
        case GDK_KEY_Up:
            aud_drct_set_volume_main (aud_drct_get_volume_main () + aud_get_int (0, "volume_delta"));
            break;
        case GDK_KEY_Down:
            aud_drct_set_volume_main (aud_drct_get_volume_main () - aud_get_int (0, "volume_delta"));
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
        case 'q':
            aud_quit ();
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
                aud_plugin_enable (plugin, (! aud_plugin_get_enabled (plugin)));
                break;
              case 'g':
                plugin = aud_plugin_lookup_basename ("gnomeshortcuts");
                aud_plugin_enable (plugin, (! aud_plugin_get_enabled (plugin)));
                break;
              case 'l':
                plugin = aud_plugin_lookup_basename ("lyricwiki");
                aud_plugin_enable (plugin, (! aud_plugin_get_enabled (plugin)));
                break;
              case 'm':
                plugin = aud_plugin_lookup_basename ("info-bar-plugin-gtk");
                aud_plugin_enable (plugin, (! aud_plugin_get_enabled (plugin)));
                break;
              case 'p':
                plugin = aud_plugin_lookup_basename ("playlist-manager");
                aud_plugin_enable (plugin, (! aud_plugin_get_enabled (plugin)));
                break;
              case 's':
                plugin = aud_plugin_lookup_basename ("search-tool");
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
    gtk_widget_destroy (widget);
    widget = nullptr;
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
    GtkWidget * widget = ui_infoarea_new ();

    g_signal_connect (widget, "destroy", (GCallback) infobar_cleanup, nullptr);
    g_signal_connect (widget, "key-press-event", (GCallback) infobar_keypress_cb, nullptr);
    gtk_widget_show_all (widget);

    show_hide_infoarea_art ();
    show_hide_infoarea_vis ();

    gtk_widget_set_can_focus (widget, true);
    gtk_widget_set_tooltip_text (widget, "Space: pause\nEsc: close\nUp|Down: volume\nLeft|Right: seek\nQ: Quit\nB: next\nC: pause\nX: play\nZ: prev");

    return widget;
}

EXPORT InfoBarPlugin aud_plugin_instance;
