/*
 * Audacious: A cross-platform multimedia player
 * Copyright (c) 2006-2010 William Pitcock, Tony Vroon, George Averill, Giacomo
 *  Lozito, Derek Pomery and Yoshiki Yazawa, and John Lindgren.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include <stdlib.h>
#include <string.h>
#include <glib.h>  /* for g_get_current_dir, g_path_is_absolute */

#include <libfauxdcore/preferences.h>
#include <libfauxdcore/audstrings.h>
#include <libfauxdcore/i18n.h>
#include <libfauxdcore/plugin.h>
#include <libfauxdcore/runtime.h>

static const char * const m3u_exts[] = {"m3u", "m3u8", "txt", "ls"};  // JWT:ACCEPT .txt & "ls * |"

class M3ULoader : public PlaylistPlugin
{
public:
    static const PreferencesWidget widgets[];
    static const PluginPreferences prefs;
    static constexpr PluginInfo info = {
        N_("M3U Playlists"),
        PACKAGE,
        nullptr,
        & prefs
    };

    constexpr M3ULoader () : PlaylistPlugin (info, m3u_exts, true) {}

    bool load (const char * filename, VFSFile & file, String & title,
     Index<PlaylistAddItem> & items);
    bool save (const char * filename, VFSFile & file, const char * title,
     const Index<PlaylistAddItem> & items);

private:
    bool Extended_m3u = false;
    Tuple tuple;
};

EXPORT M3ULoader aud_plugin_instance;

static char * split_line (char * line)
{
    char * feed = strchr (line, '\n');
    if (! feed)
        return nullptr;

    if (feed > line && feed[-1] == '\r')
        feed[-1] = 0;
    else
        feed[0] = 0;

    return feed + 1;
}

bool M3ULoader::load (const char * filename, VFSFile & file, String & title,
 Index<PlaylistAddItem> & items)
{
    Extended_m3u = false;
    bool HLS_firstentryonly = aud_get_bool ("m3u", "HLS_firstentryonly");
    tuple = Tuple ();

    Index<char> text = file.read_all ();
    if (! text.len ())
        return false;

    text.append (0);  /* null-terminate */

    char * parse = text.begin ();
    if (! strncmp (parse, "\xef\xbb\xbf", 3)) /* byte order mark */
        parse += 3;

    bool firstline = true;
    while (parse)
    {
        char * next = split_line (parse);

        while (* parse == ' ' || * parse == '\t')
            parse ++;

        if (* parse)
        {
            if (* parse != '#')
            {
                String s;
                if (! strncmp (filename, "stdin://", 8))  // WE'RE PIPING IN FROM STDIN:
                {
                    char * cur = g_get_current_dir ();
                    String cur_path = String (filename_to_uri (filename_build ({cur, filename+8})));
                    s = String (uri_construct (parse, cur_path));
                    g_free (cur);
                }
                else
                    s = String (uri_construct (parse, filename));

                if (s && s[0])
                {
                    if (! strncmp (s, "file://", 7) && strstr (parse, "cue?"))  // CUESHEET:
                        s = String (str_decode_percent (s));  // UNESCAPE THE "?" ESCAPED BY uri_construct ().

                    if (Extended_m3u)
                    {
                        tuple.set_filename (s);
                        items.append (String (s), std::move (tuple));  // NOTE:NEVER SET TUPLE VALID (FORCE RESCAN)!
                        if (HLS_firstentryonly && strstr_nocase (s, ".ts"))
                            break;
                    }
                    else
                        items.append (String (s));
                }
            }
            else if (Extended_m3u && ! strncmp (parse, "#EXTINF", 7))  // WE'RE A TITLE LINE (EXTENDED M3U):
            {
                tuple = Tuple ();
                parse += 7;
                if (parse < next && * parse == ':')
                {
                    ++parse;
                    while (parse < next && * parse == ' ')
                        ++parse;

                    if (* parse && parse < next)
                    {
                        Index<String> headerparts = str_list_to_index (parse, ",");
                        if (headerparts.len () > 1)
                        {
                            int tlen = atoi (headerparts[0]) * 1000;
                            if (tlen <= 0)
                                tuple.unset (Tuple::Length);
                            else
                                tuple.set_int (Tuple::Length, tlen);

                            // FIND THE TITLE AND MOVE PAST ANY LEADING SPACES IN IT:
                            char * c = parse;
                            while (c < next && * c != ',')
                                ++c;
                            if (c < next && * c)
                                ++c;
                            while (c < next && * c == ' ')
                                ++c;
                            if (* c)
                                tuple.set_str (Tuple::Title, c);
                        }
                        else if (headerparts.len () > 0)
                        {
                            tuple.unset (Tuple::Length);
                            tuple.set_str (Tuple::Title, headerparts[0]);
                        }
                    }
                }
            }
            else if (firstline && ! strncmp (parse, "#EXTM3U", 7))  // WE'RE AN EXTENDED M3U:
                Extended_m3u = true;
        }

        firstline = false;
        parse = next;
    }

    return true;
}

bool M3ULoader::save (const char * filename, VFSFile & file, const char * title,
 const Index<PlaylistAddItem> & items)
{
    Extended_m3u = aud_get_bool ("m3u", "saveas_extended_m3u");
    if (Extended_m3u && file.fwrite (str_copy("#EXTM3U\n"), 1, 8) != 8)
        return false;

    for (auto & item : items)
    {
        StringBuf path = uri_deconstruct (item.filename, filename);
        if (Extended_m3u && item.tuple.state () == Tuple::Valid)
        {
            int tuplen = item.tuple.get_int (Tuple::Length);
            if (tuplen >= 0)
                tuplen /= 1000;
            String tupstr = item.tuple.get_str (Tuple::Title);
            if (!tupstr)
                tupstr = String (filename_get_base (item.filename));
            StringBuf line = str_printf ("#EXTINF:%d, %s\n", tuplen, (const char *) tupstr);
            if (file.fwrite (line, 1, line.len ()) != line.len ())
                return false;
        }
        StringBuf line = str_concat ({path, "\n"});
        if (file.fwrite (line, 1, line.len ()) != line.len ())
            return false;
    }

    return true;
}

const PreferencesWidget M3ULoader::widgets[] = {
    WidgetLabel(N_("<b>M3U Configuration</b>")),
    WidgetCheck(N_("Save in Extended M3U format?"), WidgetBool("m3u", "saveas_extended_m3u")),
    WidgetCheck(N_("Only 1st ts entry for HLS streams?"), WidgetBool("m3u", "HLS_firstentryonly")),
};

const PluginPreferences M3ULoader::prefs = {{widgets}};
