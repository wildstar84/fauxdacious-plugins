/*  BMP - Cross-platform multimedia player
 *  Copyright (C) 2003-2004  BMP development team.
 *
 *  Based on XMMS:
 *  Copyright (C) 1998-2003  XMMS development team.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; under version 3 of the License.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses>.
 *
 *  The Audacious team does not consider modular code linking to
 *  Audacious or using our public API to be a derived work.
 */

#ifndef SKINS_UI_PLAYLIST_H
#define SKINS_UI_PLAYLIST_H

class PlaylistWidget;
class TextBox;
class Window;

void playlistwin_create ();
void playlistwin_unhook ();
void playlistwin_hide_timer ();
void playlistwin_set_time (const char * minutes, const char * seconds);
void playlistwin_set_shaded_title (const char * title);

extern Window * playlistwin;
extern PlaylistWidget * playlistwin_list;
extern TextBox * playlistwin_sinfo;

#endif /* SKINS_UI_PLAYLIST_H */
