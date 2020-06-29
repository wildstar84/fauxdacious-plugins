/*
 * song-info.cc
 * Copyright 2014 William Pitcock
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

#include <libfauxdcore/drct.h>
#include <libfauxdcore/hook.h>
#include <libfauxdcore/i18n.h>
#include <libfauxdcore/plugin.h>
#include <libfauxdcore/runtime.h>
#include <libfauxdcore/playlist.h>

#include <libfauxdqt/info-widget.h>

class SongInfo : public GeneralPlugin {
public:
    static constexpr PluginInfo info = {
        N_("Song Info"),
        PACKAGE,
        nullptr, // about
        nullptr, // prefs
        PluginQtOnly
    };

    constexpr SongInfo () : GeneralPlugin (info, false) {}
    void * get_qt_widget ();
};

class SongInfoWidget : public audqt::InfoWidget
{
public:
    SongInfoWidget() { update(); }

private:
    const HookReceiver<SongInfoWidget>
        change_hook{"tuple change", this, &SongInfoWidget::update},
        update_hook{"playback ready", this, &SongInfoWidget::update},
        clear_hook{"playback stop", this, &SongInfoWidget::update};

    void update()
    {
        int playlist = aud_playlist_get_playing ();
        int position;

        if (playlist == -1)
            playlist = aud_playlist_get_active ();

        position = aud_playlist_get_position (playlist);

        if (position >= 0)
        fillInfo(playlist, position, aud_drct_get_filename(), aud_drct_get_tuple(), nullptr, false);
    }
};

void * SongInfo::get_qt_widget ()
{
    return new SongInfoWidget;
}

EXPORT SongInfo aud_plugin_instance;
