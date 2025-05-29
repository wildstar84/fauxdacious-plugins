/*
 * view.c
 * Copyright 2014-2015 John Lindgren
 *
 * This file is part of Audacious.
 *
 * Audacious is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, version 2 or version 3 of the License.
 *
 * Audacious is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * Audacious. If not, see <http://www.gnu.org/licenses/>.
 *
 * The Audacious team does not consider modular code linking to Audacious or
 * using our public API to be a derived work.
 */

#include "view.h"

#include <libfauxdcore/hook.h>
#include <libfauxdcore/mainloop.h>
#include <libfauxdcore/runtime.h>

#include <QWindow>

#include "plugin.h"
#include "plugin-window.h"
#include "skins_cfg.h"
#include "equalizer.h"
#include "main.h"
#include "vis-callbacks.h"
#include "playlistwin.h"
#include "button.h"
#include "eq-graph.h"
#include "textbox.h"
#include "menurow.h"
#include "window.h"
#include "vis.h"

void view_show_player (bool show)
{
    if (show)
    {
        /* JWT:NEXT 2 MOVED HERE FROM plugin.cc:skins_init_main().
           FOR BETTER COOPORATION BETWEEN AfterStep & statusicon PLUGIN: */
        view_apply_on_top_startup ();
        view_apply_sticky ();
        mainwin->show ();
        mainwin->activateWindow ();
        show_plugin_windows ();
    }
    else
    {
        mainwin->hide ();
        hide_plugin_windows ();
    }

    view_apply_show_playlist ();
    view_apply_show_equalizer ();

    start_stop_visual (false);
}

void view_set_show_playlist (bool show)
{
    aud_set_bool ("skins", "playlist_visible", show);
    hook_call ("skins set playlist_visible", nullptr);

    view_apply_show_playlist ();
}

void view_apply_show_playlist ()
{
    bool show = aud_get_bool ("skins", "playlist_visible");

    if (show && mainwin->isVisible ())
    {
        playlistwin->winId ();
        playlistwin->windowHandle ()->setTransientParent (mainwin->windowHandle ());
        playlistwin->show ();
        playlistwin->activateWindow ();
    }
    else
        playlistwin->hide ();

    mainwin_pl->set_active (show);
}

void view_set_show_equalizer (bool show)
{
    aud_set_bool ("skins", "equalizer_visible", show);
    hook_call ("skins set equalizer_visible", nullptr);

    view_apply_show_equalizer ();
}

void view_apply_show_equalizer ()
{
    bool show = aud_get_bool ("skins", "equalizer_visible");

    if (show && mainwin->isVisible ())
    {
        equalizerwin->winId ();
        equalizerwin->windowHandle ()->setTransientParent (mainwin->windowHandle ());
        equalizerwin->show ();
        equalizerwin->activateWindow ();
    }
    else
        equalizerwin->hide ();

    mainwin_eq->set_active (show);
}

void view_set_player_shaded (bool shaded)
{
    aud_set_bool ("skins", "player_shaded", shaded);
    hook_call ("skins set player_shaded", nullptr);

    view_apply_player_shaded ();
}

void view_apply_player_shaded ()
{
    bool shaded = aud_get_bool ("skins", "player_shaded");

    mainwin->set_shaded (shaded);

    int width = shaded ? MAINWIN_SHADED_WIDTH : skin.hints.mainwin_width;
    int height = shaded ? MAINWIN_SHADED_HEIGHT : skin.hints.mainwin_height;
    mainwin->resize (width, height);

    if (config.autoscroll)
        mainwin_info->set_scroll (! shaded);
}

void view_set_playlist_shaded (bool shaded)
{
    aud_set_bool ("skins", "playlist_shaded", shaded);
    hook_call ("skins set playlist_shaded", nullptr);

    view_apply_playlist_shaded ();
}

void view_apply_playlist_shaded ()
{
    bool shaded = aud_get_bool ("skins", "playlist_shaded");

    playlistwin->set_shaded (shaded);

    int height = shaded ? MAINWIN_SHADED_HEIGHT : config.playlist_height;
    playlistwin->resize (config.playlist_width, height);

    if (config.autoscroll)
        playlistwin_sinfo->set_scroll (shaded);
}

void view_set_equalizer_shaded (bool shaded)
{
    aud_set_bool ("skins", "equalizer_shaded", shaded);
    hook_call ("skins set equalizer_shaded", nullptr);

    view_apply_equalizer_shaded ();
}

void view_apply_equalizer_shaded ()
{
    bool shaded = aud_get_bool ("skins", "equalizer_shaded");

    /* do not allow shading the equalizer if eq_ex.bmp is missing */
    if (skin.pixmaps[SKIN_EQ_EX].isNull ())
        shaded = false;

    equalizerwin->set_shaded (shaded);
    equalizerwin->resize (275, shaded ? 14 : 116);
}

void view_set_double_size (bool double_size)
{
    aud_set_bool ("skins", "double_size", double_size);
    hook_call ("skins set double_size", nullptr);

    view_apply_double_size ();
}

void view_apply_double_size ()
{
    static QueuedFunc restart;
    restart.queue (skins_restart);
}

void view_set_on_top (bool on_top)
{
    aud_set_bool ("skins", "always_on_top", on_top);
    hook_call ("skins set always_on_top", nullptr);

    view_apply_on_top ();
}

void view_apply_on_top_startup ()
{
#if QT_VERSION >= QT_VERSION_CHECK(5, 9, 0)
    bool on_top = aud_get_bool ("skins", "always_on_top");
    bool mainwin_visible = mainwin->isVisible ();
    bool equalizer_visible = equalizerwin->isVisible ();
    bool playlist_visible = playlistwin->isVisible ();

    mainwin->setWindowFlag (Qt::WindowStaysOnTopHint, on_top);
    if (mainwin_visible)
        mainwin->show ();


    equalizerwin->setWindowFlag (Qt::WindowStaysOnTopHint, on_top);
    if (equalizer_visible)
        equalizerwin->show ();

    playlistwin->setWindowFlag (Qt::WindowStaysOnTopHint, on_top);
    if (playlist_visible)
        playlistwin->show ();
#endif

    mainwin_menurow->refresh ();
}

void view_apply_on_top ()
{
#if QT_VERSION >= QT_VERSION_CHECK(5, 9, 0)
    if (aud_get_bool ("audacious", "afterstep2"))
        /* JWT:SPECIAL CASE JUST FOR OUR DBUS-LESS VSN. OF AfterStep WINDOW-MANAGER!: */
        system ("WinCommand -pattern \"^\" ontop");
    else
    {
        bool on_top = aud_get_bool ("skins", "always_on_top");
        bool mainwin_visible = mainwin->isVisible ();
        bool equalizer_visible = equalizerwin->isVisible ();
        bool playlist_visible = playlistwin->isVisible ();

        mainwin->setWindowFlag (Qt::WindowStaysOnTopHint, on_top);
        if (mainwin_visible)
            mainwin->show ();


        equalizerwin->setWindowFlag (Qt::WindowStaysOnTopHint, on_top);
        if (equalizer_visible)
            equalizerwin->show ();

        playlistwin->setWindowFlag (Qt::WindowStaysOnTopHint, on_top);
        if (playlist_visible)
            playlistwin->show ();
    }
#else
    if (aud_get_bool ("audacious", "afterstep2"))
        /* JWT:SPECIAL CASE JUST FOR OUR DBUS-LESS VSN. OF AfterStep WINDOW-MANAGER!: */
        system ("WinCommand -pattern \"^\" ontop");
#endif

    mainwin_menurow->refresh ();
}

void view_set_sticky (bool sticky)
{
    aud_set_bool ("skins", "sticky", sticky);
    hook_call ("skins set sticky", nullptr);

    view_apply_sticky ();
}

void view_apply_sticky ()
{
#if 0
    bool sticky = aud_get_bool ("skins", "sticky");

    if (sticky)
    {
        gtk_window_stick ((GtkWindow *) mainwin->gtk ());
        gtk_window_stick ((GtkWindow *) equalizerwin->gtk ());
        gtk_window_stick ((GtkWindow *) playlistwin->gtk ());
    }
    else
    {
        gtk_window_unstick ((GtkWindow *) mainwin->gtk ());
        gtk_window_unstick ((GtkWindow *) equalizerwin->gtk ());
        gtk_window_unstick ((GtkWindow *) playlistwin->gtk ());
    }
#endif
}

void view_set_show_remaining (bool remaining)
{
    aud_set_bool ("skins", "show_remaining_time", remaining);
    hook_call ("skins set show_remaining_time", nullptr);

    view_apply_show_remaining ();
}

void view_apply_show_remaining ()
{
    mainwin_update_song_info ();
}

static QRegion * scale_mask (const Index<QRect> & mask, int scale)
{
    QRegion * region = nullptr;

    for (auto & rect : mask)
    {
        QRect scaled (
            rect.x () * scale,
            rect.y () * scale,
            rect.width () * scale,
            rect.height () * scale
        );

        if (region)
            * region |= scaled;
        else
            region = new QRegion (scaled);
    }

    return region;
}

void view_apply_skin ()
{
    mainwin->set_shapes
     (scale_mask (skin.masks[SKIN_MASK_MAIN], config.scale),
      scale_mask (skin.masks[SKIN_MASK_MAIN_SHADE], config.scale));
    equalizerwin->set_shapes
     (scale_mask (skin.masks[SKIN_MASK_EQ], config.scale),
      scale_mask (skin.masks[SKIN_MASK_EQ_SHADE], config.scale));

    mainwin_refresh_hints ();
    view_apply_equalizer_shaded ();
    TextBox::update_all ();

    mainwin->queue_draw ();
    equalizerwin->queue_draw ();
    playlistwin->queue_draw ();
}

void view_redraw_main ()
{
    mainwin->queue_draw ();
}
