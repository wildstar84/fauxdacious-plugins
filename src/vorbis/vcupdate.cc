/* Audacious - Cross-platform multimedia player
 * Copyright (C) 2005-2022  Audacious development team
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 */

#include <ogg/ogg.h>
#include <vorbis/codec.h>
#include <vorbis/vorbisfile.h>

#include <glib.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define AUD_GLIB_INTEGRATION
#define WANT_AUD_BSWAP
#include <libfauxdcore/audstrings.h>
#include <libfauxdcore/i18n.h>
#include <libfauxdcore/multihash.h>
#include <libfauxdcore/runtime.h>

#include "vorbis.h"
#include "vcedit.h"

typedef SimpleHash<String, String> Dictionary;

static Dictionary dictionary_from_vorbis_comment (vorbis_comment * vc)
{
    Dictionary dict;

    for (int i = 0; i < vc->comments; i ++)
    {
        const char * s = vc->user_comments[i];
        AUDDBG("%s\n", s);

        const char * eq = strchr (s, '=');
        if (eq && eq > s && eq[1])
            dict.add (String (str_toupper (str_copy (s, eq - s))), String (eq + 1));
    }

    return dict;
}

static void dictionary_to_vorbis_comment (vorbis_comment * vc, Dictionary & dict)
{
    vorbis_comment_clear (vc);

    dict.iterate ([vc] (const String & key, String & field) {
        vorbis_comment_add_tag (vc, key, field);
    });
}

static void insert_str_tuple_field_to_dictionary (const Tuple & tuple,
 Tuple::Field field, Dictionary & dict, const char * key)
{
    String val = tuple.get_str (field);

    if (val && val[0])
        dict.add (String (key), std::move (val));
    else
        dict.remove (String (key));
}

/* JWT:ADDED FUNCTION TO WRITE A RAW (NON TUPLE-SUPPLIED VALUE) STRING: */
static void insert_string_to_dictionary (const char * val, Dictionary & dict, const char * key)
{
    if (val && val[0])
        dict.add (String (key), String (val));
    else
        dict.remove (String (key));
}

static void insert_int_tuple_field_to_dictionary (const Tuple & tuple,
 Tuple::Field field, Dictionary & dict, const char * key)
{
    int val = tuple.get_int (field);

    if (val > 0)
        dict.add (String (key), String (int_to_str (val)));
    else
        dict.remove (String (key));
}

/* JWT:  EMULATE: $>kid3-cli -c 'set picture:"<imagefid>" "front cover"' <songfile.ogg>
   SEE:  https://xiph.org/flac/format.html#metadata_block_picture
*/
static bool write_artimage_item (const Index<char> & data, const char * fileext,
 const char * key, Dictionary & dict)
{
    uint32_t int4bites;
    Index<char> buf;

    int4bites = TO_BE32 (3);  // JWT:"COVER FRONT"
    buf.insert ((const char *) & int4bites, 0, 4);

    if (strcmp_nocase (fileext, "jpg"))
    {
        int4bites = TO_BE32 (6 + strlen (fileext));
        buf.insert ((const char *) & int4bites, -1, 4);
        buf.insert ("image/", -1, 6);
        buf.insert (fileext, -1, strlen (fileext));
    }
    else
    {
        int4bites = TO_BE32 (10);
        buf.insert ((const char *) & int4bites, -1, 4);
        buf.insert ("image/jpeg", -1, 10);
    }

    int4bites = TO_BE32 (11);
    buf.insert ((const char *) & int4bites, -1, 4);
    buf.insert ("front cover", -1, 11);

    int4bites = TO_BE32 (32);  // JWT:WIDTH - DOESN'T SEEM TO BE USED BY US OR VLC AT LEAST (SO MADE UP)!
    buf.insert ((const char *) & int4bites, -1, 4);

    int4bites = TO_BE32 (32);  // JWT:HEIGHT - DOESN'T SEEM TO BE USED BY US OR VLC AT LEAST (SO MADE UP)!
    buf.insert ((const char *) & int4bites, -1, 4);

    int4bites = TO_BE32 (16);  // JWT:COLOR DEPTH - DOESN'T SEEM TO BE USED BY US OR VLC AT LEAST (SO SANE DEFAULT)!
    buf.insert ((const char *) & int4bites, -1, 4);

    int4bites = 0;
    buf.insert ((const char *) & int4bites, -1, 4);

    int4bites = TO_BE32 (data.len ());
    buf.insert ((const char *) & int4bites, -1, 4);

    buf.insert (data.begin (), -1, data.len ());
    AUDDBG ("Write: (len=%d) %s = %s.\n", buf.len (), key, buf.begin ());

    dict.add (String (key), String (g_base64_encode ((const guchar *) buf.begin (), buf.len ())));

    return true;
}

bool VorbisPlugin::write_tuple (const char * filename, VFSFile & file, const Tuple & tuple)
{
    VCEdit edit;
    if (! edit.open (file))
        return false;

    Dictionary dict = dictionary_from_vorbis_comment (& edit.vc);

    insert_str_tuple_field_to_dictionary (tuple, Tuple::Title, dict, "TITLE");
    insert_str_tuple_field_to_dictionary (tuple, Tuple::Artist, dict, "ARTIST");
    insert_str_tuple_field_to_dictionary (tuple, Tuple::Album, dict, "ALBUM");
    insert_str_tuple_field_to_dictionary (tuple, Tuple::AlbumArtist, dict, "ALBUMARTIST");
//    insert_str_tuple_field_to_dictionary (tuple, Tuple::Comment, dict, "COMMENT");
    insert_str_tuple_field_to_dictionary (tuple, Tuple::Genre, dict, "GENRE");
    insert_str_tuple_field_to_dictionary (tuple, Tuple::Description, dict, "DESCRIPTION");
    insert_str_tuple_field_to_dictionary (tuple, Tuple::Lyrics, dict, "LYRICS");
    insert_str_tuple_field_to_dictionary (tuple, Tuple::MusicBrainzID, dict, "musicbrainz_trackid");

    insert_int_tuple_field_to_dictionary (tuple, Tuple::Year, dict, "DATE");
    insert_int_tuple_field_to_dictionary (tuple, Tuple::Track, dict, "TRACKNUMBER");
    insert_str_tuple_field_to_dictionary (tuple, Tuple::Publisher, dict, "publisher");
    insert_str_tuple_field_to_dictionary (tuple, Tuple::CatalogNum, dict, "CATALOGNUMBER");
    insert_str_tuple_field_to_dictionary (tuple, Tuple::Performer, dict, "PERFORMER");

    String comment = tuple.get_str (Tuple::Comment);
    bool wrote_art = false;
    if (comment && comment[0] && ! strncmp ((const char *) comment, "file://", 7))
    {
        const char * comment_ptr = (const char *) comment;
        const char * sep = strstr (comment_ptr, ";file://");
        String main_imageuri = sep ? String (str_printf ("%.*s", (int)(sep - comment_ptr), comment_ptr))
                : comment;
        VFSFile file (main_imageuri, "r");  /* JWT:ASSUME COMMENT IS AN IMAGE FILE FROM SONG-INFO EDITS!: */
        if (file)
        {
            Index<char> data = file.read_all ();
            if (data.len () > 499)  /* JWT:SANITY-CHECK: ANY VALID ART IMAGE SHOULD BE BIGGER THAN THIS! */
            {
                wrote_art = write_artimage_item (data, uri_get_extension (comment),
                        "METADATA_BLOCK_PICTURE", dict);
                if (wrote_art)
                {
                    if (sep)  // WE MAY HAVE A SECOND (CHANNEL) ART IMAGE, WRITE THAT TO COMMENT FIELD (";file:.."):
                        insert_string_to_dictionary (sep, dict, "COMMENT");

                    aud_set_bool (nullptr, "_user_tag_skipthistime", true);  /* JWT:SKIP DUP. TO user_tag_data. */
                }
            }
        }
    }
    if (! wrote_art)
        insert_str_tuple_field_to_dictionary (tuple, Tuple::Comment, dict, "COMMENT");

    dictionary_to_vorbis_comment (& edit.vc, dict);

    auto temp_vfs = VFSFile::tmpfile ();
    if (! temp_vfs)
        return false;

    if (! edit.write (file, temp_vfs))
    {
        AUDERR ("Tag update failed: %s.\n", edit.lasterror);
        return false;
    }

    return file.replace_with (temp_vfs);
}
