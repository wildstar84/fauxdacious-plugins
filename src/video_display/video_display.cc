/*
 *  Video Display visualization plugin for Fauxdacious
 *  Copyright (C) 2020 Jim Turner
 *
 *  Simply allow toggling of the ffaudio:play_video flag here
 *  NOTE:  User must restart currently-playing video in order for this
 #  flag change to take effect!
 *
 *  Based on XMMS:
 *  Copyright (C) 1998-2003  XMMS development team.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include <math.h>
#include <string.h>

#include <libfauxdcore/i18n.h>
#include <libfauxdcore/runtime.h>
#include <libfauxdcore/plugin.h>
#include <libfauxdcore/preferences.h>

static const char vid_about[] =
 N_("Video Display visualization plugin for Fauxdacious\n"
    "Copyright 2020 Jim Turner\n\n"
    "Toggles video display for the FFmpeg & DVD plugins:\n"
    "NOTE:  User must restart any currently-playing video\n"
    "or DVD in order for this change to take effect!");

class VidDisplay : public VisPlugin
{
public:
    static constexpr PluginInfo info = {
        N_("Video Display"),
        PACKAGE,
        vid_about,
        nullptr,
        0
    };

    constexpr VidDisplay () : VisPlugin (info, 0) {}

    bool init ();
    void clear ();
    void cleanup ();
};

EXPORT VidDisplay aud_plugin_instance;

bool VidDisplay::init ()
{
    aud_set_bool ("ffaudio", "play_video", true);
    aud_set_bool ("dvd", "play_video", true);
    AUDDBG ("i:Video Display Plugin turned ON!\n");

    return true;
}

void VidDisplay::clear ()  /* REQUIRED FOR VISUALIZATION PLUGINS. */
{
}

void VidDisplay::cleanup ()
{
    aud_set_bool ("ffaudio", "play_video", false);
    aud_set_bool ("dvd", "play_video", false);
    AUDDBG ("i:Video Display Plugin turned off!\n");
}
