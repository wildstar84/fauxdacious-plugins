/*
 * Copyright (c) 2010 William Pitcock <nenolod@dereferenced.org>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <gtk/gtk.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/HTMLparser.h>
#include <libxml/xpath.h>

#define AUD_GLIB_INTEGRATION
#include <libfauxdcore/drct.h>
#include <libfauxdcore/i18n.h>
#include <libfauxdcore/plugin.h>
#include <libfauxdcore/plugins.h>
#include <libfauxdcore/runtime.h>
#include <libfauxdcore/audstrings.h>
#include <libfauxdcore/hook.h>
#include <libfauxdcore/vfs_async.h>

class LyricWiki : public GeneralPlugin
{
public:
    static constexpr PluginInfo info = {
        N_("LyricWiki Plugin"),
        PACKAGE,
        nullptr, // about
        nullptr, // prefs
        PluginGLibOnly
    };

    constexpr LyricWiki () : GeneralPlugin (info, false) {}

    void * get_gtk_widget ();
};

EXPORT LyricWiki aud_plugin_instance;

typedef struct {
    String filename; /* of song file */
    String title, artist;
    String uri; /* URI we are trying to retrieve */
    String local_filename; /* JWT:CALCULATED LOCAL FILENAME TO SAVE LYRICS TO */
    gint startlyrics;      /* JWT:OFFSET IN LYRICS WINDOW WHERE LYRIC TEXT ACTUALLY STARTS */
} LyricsState;

static LyricsState state;

/*
 * Suppress libxml warnings, because lyricwiki does not generate anything near
 * valid HTML.
 */
static void libxml_error_handler (void * ctx, const char * msg, ...)
{
}

static CharPtr scrape_lyrics_from_lyricwiki_edit_page (const char * buf, int64_t len)
{
    xmlDocPtr doc;
    CharPtr ret;

    /*
     * temporarily set our error-handling functor to our suppression function,
     * but we have to set it back because other components of Audacious depend
     * on libxml and we don't want to step on their code paths.
     *
     * unfortunately, libxml is anti-social and provides us with no way to get
     * the previous error functor, so we just have to set it back to default after
     * parsing and hope for the best.
     */
    xmlSetGenericErrorFunc (nullptr, libxml_error_handler);
    doc = htmlReadMemory (buf, (int) len, nullptr, "utf-8", (HTML_PARSE_RECOVER | HTML_PARSE_NONET));
    xmlSetGenericErrorFunc (nullptr, nullptr);

    if (doc)
    {
        xmlXPathContextPtr xpath_ctx = nullptr;
        xmlXPathObjectPtr xpath_obj = nullptr;
        xmlNodePtr node = nullptr;

        xpath_ctx = xmlXPathNewContext (doc);
        if (! xpath_ctx)
            goto give_up;

        xpath_obj = xmlXPathEvalExpression ((xmlChar *) "//*[@id=\"wpTextbox1\"]", xpath_ctx);
        if (! xpath_obj)
            goto give_up;

        if (! xpath_obj->nodesetval->nodeMax)
            goto give_up;

        node = xpath_obj->nodesetval->nodeTab[0];
give_up:
        if (xpath_obj)
            xmlXPathFreeObject (xpath_obj);

        if (xpath_ctx)
            xmlXPathFreeContext (xpath_ctx);

        if (node)
        {
            xmlChar * lyric = xmlNodeGetContent (node);

            if (lyric)
            {
                GMatchInfo * match_info;
                GRegex * reg;

                reg = g_regex_new
                 ("<(lyrics?)>[[:space:]]*(.*?)[[:space:]]*</\\1>",
                 (GRegexCompileFlags) (G_REGEX_MULTILINE | G_REGEX_DOTALL),
                 (GRegexMatchFlags) 0, nullptr);
                g_regex_match (reg, (char *) lyric, G_REGEX_MATCH_NEWLINE_ANY, & match_info);

                ret.capture (g_match_info_fetch (match_info, 2));
                if (! strcmp_nocase (ret, "<!-- PUT LYRICS HERE (and delete this entire line) -->"))
                    ret.capture (g_strdup (_("No lyrics available")));

                g_regex_unref (reg);
            }

            xmlFree (lyric);
        }

        xmlFreeDoc (doc);
    }

    return ret;
}

static String scrape_uri_from_lyricwiki_search_result (const char * buf, int64_t len)
{
    xmlDocPtr doc;
    String uri;

    /*
     * workaround buggy lyricwiki search output where it cuts the lyrics
     * halfway through the UTF-8 symbol resulting in invalid XML.
     */
    GRegex * reg;

    reg = g_regex_new ("<(lyrics?)>.*</\\1>", (GRegexCompileFlags)
     (G_REGEX_MULTILINE | G_REGEX_DOTALL | G_REGEX_UNGREEDY),
     (GRegexMatchFlags) 0, nullptr);
    CharPtr newbuf (g_regex_replace_literal (reg, buf, len, 0, "", G_REGEX_MATCH_NEWLINE_ANY, nullptr));
    g_regex_unref (reg);

    /*
     * temporarily set our error-handling functor to our suppression function,
     * but we have to set it back because other components of Audacious depend
     * on libxml and we don't want to step on their code paths.
     *
     * unfortunately, libxml is anti-social and provides us with no way to get
     * the previous error functor, so we just have to set it back to default after
     * parsing and hope for the best.
     */
    xmlSetGenericErrorFunc (nullptr, libxml_error_handler);
    doc = xmlParseMemory (newbuf, strlen (newbuf));
    xmlSetGenericErrorFunc (nullptr, nullptr);

    if (doc != nullptr)
    {
        xmlNodePtr root, cur;

        root = xmlDocGetRootElement(doc);

        for (cur = root->xmlChildrenNode; cur; cur = cur->next)
        {
            if (xmlStrEqual(cur->name, (xmlChar *) "url"))
            {
                auto lyric = (char *) xmlNodeGetContent (cur);

                // If the lyrics are unknown, LyricWiki returns a broken link
                // to the edit page.  Extract the song ID (artist:title) from
                // the URI and recreate a working link.
                char * title = strstr (lyric, "title=");
                if (title)
                {
                    title += 6;

                    // Strip trailing "&action=edit"
                    char * amp = strchr (title, '&');
                    if (amp)
                        * amp = 0;

                    // Spaces get replaced with plus signs for some reason.
                    str_replace_char (title, '+', ' ');

                    // LyricWiki interprets UTF-8 as ISO-8859-1, then "converts"
                    // it to UTF-8 again.  Worse, it will sometimes corrupt only
                    // the song title in this way while leaving the artist name
                    // intact.  So we have to percent-decode the URI, split the
                    // two strings apart, repair them separately, and then
                    // rebuild the URI.
                    auto strings = str_list_to_index (str_decode_percent (title), ":");
                    for (String & s : strings)
                    {
                        StringBuf orig_utf8 = str_convert (s, -1, "UTF-8", "ISO-8859-1");
                        if (orig_utf8 && g_utf8_validate (orig_utf8, -1, nullptr))
                            s = String (orig_utf8);
                    }

                    uri = String (str_printf ("https://lyrics.fandom.com/index.php?"
                     "action=edit&title=%s", (const char *) str_encode_percent
                     (index_to_str_list (strings, ":"))));
                }
                else
                {
                    // Convert normal lyrics link to edit page link
                    char * slash = strrchr (lyric, '/');
                    if (slash)
                        uri = String (str_printf ("https://lyrics.fandom.com/index.php?"
                         "action=edit&title=%s", slash + 1));
                }

                xmlFree ((xmlChar *) lyric);
            }
        }

        xmlFreeDoc (doc);
    }

    return uri;
}

static void update_lyrics_window (const char * title, const char * artist,
 const char * lyrics, bool edit_enabled);

static GtkWidget * edit_button;
static GtkWidget * save_button;

static void get_lyrics_step_3 (const char * uri, const Index<char> & buf, void *)
{
    if (! state.uri || strcmp (state.uri, uri))
        return;

    if (! buf.len ())
    {
        update_lyrics_window (_("Error"), nullptr,
         str_printf (_("Unable to fetch %s"), uri), true);
        return;
    }

    CharPtr lyrics = scrape_lyrics_from_lyricwiki_edit_page (buf.begin (), buf.len ());

    if (! lyrics)
    {
        update_lyrics_window (_("No lyrics Found"),
                (const char *) str_concat ({"Title: ", (const char *) state.title, "\nArtist: ",
                        (const char *) state.artist}),
                str_printf (_("Unable to parse(3) %s"), uri), true);
        return;
    }

    update_lyrics_window (state.title, state.artist, lyrics, true);
    gtk_widget_set_sensitive (save_button, true);
}

static void get_lyrics_step_2 (const char * uri1, const Index<char> & buf, void *)
{
    if (! state.uri || strcmp (state.uri, uri1))
        return;

    if (! buf.len ())
    {
        update_lyrics_window (_("Error"), nullptr,
         str_printf (_("Unable to fetch %s"), uri1), false);
        return;
    }

    String uri = scrape_uri_from_lyricwiki_search_result (buf.begin (), buf.len ());

    if (! uri)
    {
        update_lyrics_window (_("Error"), nullptr,
         str_printf (_("Unable to parse(2) %s"), uri1), false);
        return;
    }

    state.uri = uri;

    update_lyrics_window (state.title, state.artist, _("Looking for lyrics ..."), true);
    vfs_async_file_get_contents (uri, get_lyrics_step_3, nullptr);
}

static void get_lyrics_step_1 ()
{
    if (! state.artist || ! state.title)
    {
        update_lyrics_window (_("Error"), nullptr, _("Missing title and/or artist"), false);
        return;
    }

    StringBuf title_buf = str_encode_percent (state.title);
    StringBuf artist_buf = str_encode_percent (state.artist);

    state.uri = String (str_printf ("https://lyrics.fandom.com/api.php?"
     "action=lyrics&artist=%s&song=%s&fmt=xml", (const char *) artist_buf,
     (const char *) title_buf));

    update_lyrics_window (state.title, state.artist, _("Connecting to lyrics.fandom.com ..."), false);
    vfs_async_file_get_contents (state.uri, get_lyrics_step_2, nullptr);
}

static void get_lyrics_step_0 (const char * uri, const Index<char> & buf, void *)
{
    if (! buf.len ())
    {
        update_lyrics_window (_("Error"), nullptr,
         str_printf (_("Unable to fetch file %s"), uri), true);
        return;
    }

    StringBuf nullterminated_buf = str_copy (buf.begin (), buf.len ());
    update_lyrics_window (state.title, state.artist, (const char *) nullterminated_buf, false);

    /* JWT:ALLOW 'EM TO EDIT LYRICWIKI, EVEN IF LYRICS ARE LOCAL, IF THEY HAVE BOTH REQUIRED FIELDS: */
    if (state.artist && state.title)
    {
        StringBuf title_buf = str_copy (state.title);
        str_replace_char (title_buf, ' ', '_');
        title_buf = str_encode_percent (title_buf, -1);
        StringBuf artist_buf = str_copy (state.artist);
        str_replace_char (artist_buf, ' ', '_');
        artist_buf = str_encode_percent (artist_buf, -1);
        state.uri = String (str_printf ("https://lyrics.fandom.com/index.php?action=edit&title=%s:%s",
                (const char *) artist_buf, (const char *) title_buf));
        gtk_widget_set_sensitive (edit_button, true);
    }
}

static GtkTextView * textview;
static GtkTextBuffer * textbuffer;

static void launch_edit_page ()
{
    if (state.uri)
        gtk_show_uri (nullptr, state.uri, GDK_CURRENT_TIME, nullptr);
}

static void save_lyrics_locally ()
{
    if (state.local_filename && textbuffer)
    {
        GtkTextIter start_iter;
        GtkTextIter end_iter;
        gtk_text_buffer_get_start_iter (textbuffer, & start_iter);
        gtk_text_buffer_get_end_iter (textbuffer, & end_iter);
        gint sz = gtk_text_iter_get_offset (& end_iter) - gtk_text_iter_get_offset (& start_iter);
        sz -= state.startlyrics;
        if (sz > 0)
        {
            gchar * lyrics = gtk_text_buffer_get_slice (textbuffer, & start_iter,
                  & end_iter, false);
            if (lyrics)
            {
                if (strstr ((const char *) state.local_filename, "/lyrics/"))
                {
                    GStatBuf statbuf;
                    StringBuf path = filename_get_parent ((const char *) state.local_filename);
                    if (g_stat ((const char *) path, & statbuf)
                            && g_mkdir ((const char *) path, 0755))
                    {
                        AUDERR ("e:Could not create missing lyrics directory (%s)!\n", (const char *) path);
                        return;
                    }
                }
                VFSFile file (state.local_filename, "w");
                if (file)
                {
                    if (file.fwrite (lyrics + state.startlyrics, 1, sz) == sz)
                        AUDINFO ("i:Successfully saved %d bytes of lyrics locally to (%s).\n", sz,
                                (const char *) state.local_filename);

                    gtk_widget_set_sensitive (save_button, false);
                }
                g_free (lyrics);
            }
        }
    }
}

static GtkWidget * build_widget ()
{
    textview = (GtkTextView *) gtk_text_view_new ();
    gtk_text_view_set_editable (textview, false);
    gtk_text_view_set_cursor_visible (textview, false);
    gtk_text_view_set_left_margin (textview, 4);
    gtk_text_view_set_right_margin (textview, 4);
    gtk_text_view_set_wrap_mode (textview, GTK_WRAP_WORD);
    textbuffer = gtk_text_view_get_buffer (textview);

    GtkWidget * scrollview = gtk_scrolled_window_new (nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type ((GtkScrolledWindow *) scrollview, GTK_SHADOW_IN);
    gtk_scrolled_window_set_policy ((GtkScrolledWindow *) scrollview, GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    GtkWidget * vbox = gtk_vbox_new (false, 6);

    gtk_container_add ((GtkContainer *) scrollview, (GtkWidget *) textview);
    gtk_box_pack_start ((GtkBox *) vbox, scrollview, true, true, 0);

    gtk_widget_show_all (vbox);

    gtk_text_buffer_create_tag (textbuffer, "weight_bold", "weight", PANGO_WEIGHT_BOLD, nullptr);
    gtk_text_buffer_create_tag (textbuffer, "size_x_large", "scale", PANGO_SCALE_X_LARGE, nullptr);
    gtk_text_buffer_create_tag (textbuffer, "style_italic", "style", PANGO_STYLE_ITALIC, nullptr);

    GtkWidget * hbox = gtk_hbox_new (false, 6);
    gtk_box_pack_start ((GtkBox *) vbox, hbox, false, false, 0);

    edit_button = gtk_button_new_with_mnemonic (_("Edit Lyricwiki"));
    gtk_widget_set_sensitive (edit_button, false);
    gtk_box_pack_end ((GtkBox *) hbox, edit_button, false, false, 0);

    save_button = gtk_button_new_with_mnemonic (_("Save Locally"));
    gtk_widget_set_sensitive (save_button, false);
    gtk_box_pack_end ((GtkBox *) hbox, save_button, false, false, 0);

    g_signal_connect (edit_button, "clicked", (GCallback) launch_edit_page, nullptr);
    g_signal_connect (save_button, "clicked", (GCallback) save_lyrics_locally, nullptr);

    return vbox;
}

static void update_lyrics_window (const char * title, const char * artist,
 const char * lyrics, bool edit_enabled)
{
    GtkTextIter iter, startlyrics;

    if (! textbuffer)
        return;

    gtk_text_buffer_set_text (textbuffer, "", -1);

    gtk_text_buffer_get_start_iter (textbuffer, & iter);

    gtk_text_buffer_insert_with_tags_by_name (textbuffer, & iter, title, -1,
     "weight_bold", "size_x_large", nullptr);

    if (artist)
    {
        gtk_text_buffer_insert (textbuffer, & iter, "\n", -1);
        gtk_text_buffer_insert_with_tags_by_name (textbuffer, & iter, artist, -1,
         "style_italic", nullptr);
    }

    gtk_text_buffer_insert (textbuffer, & iter, "\n\n", -1);
    gtk_text_buffer_get_end_iter (textbuffer, & startlyrics);
    state.startlyrics = gtk_text_iter_get_offset (& startlyrics);
    gtk_text_buffer_insert (textbuffer, & iter, lyrics, -1);

    gtk_text_buffer_get_start_iter (textbuffer, & iter);
    gtk_text_view_scroll_to_iter (textview, & iter, 0, true, 0, 0);

    gtk_widget_set_sensitive (edit_button, edit_enabled);
}

static void lyricwiki_playback_began ()
{
    /* FIXME: cancel previous VFS requests (not possible with current API) */

    bool have_valid_filename = false;
    bool found_lyricfile = false;
    GStatBuf statbuf;
    String lyricStr = String ("");
    StringBuf path = StringBuf ();

    state.filename = aud_drct_get_filename ();
    state.uri = String ();

    /* JWT: EXTRACT JUST THE "NAME" PART TO USE TO NAME THE LYRICS FILE: */
    const char * slash = state.filename ? strrchr (state.filename, '/') : nullptr;
    const char * base = slash ? slash + 1 : nullptr;

    if (base && base[0] != '\0' && strncmp (base, "-.", 2))  // NOT AN EMPTY "NAME" OR stdin!
        have_valid_filename = true;

    state.local_filename = String ("");
    if (have_valid_filename)  // WE KNOW THAT base IS NOT NULL, IF TRUE!
    {
        /* JWT:IF WE'RE A "FILE", FIRST CHECK LOCAL DIRECTORY FOR A LYRICS FILE MATCHING FILE-NAME: */
        const char * dot = strrchr (base, '.');
        int ln = (dot && ! strstr (dot, ".cue?")) ? (dot - base) : -1;  // SET TO FULL LENGTH(-1) IF NO EXTENSION OR NOT A CUESHEET.
        if (! strncmp ((const char *) state.filename, "file://", 7))
        {
            path = filename_get_parent (uri_to_filename (state.filename));
            lyricStr = String (str_concat ({path, "/", str_decode_percent (base, ln), ".lrc"}));
            found_lyricfile = ! (g_stat ((const char *) lyricStr, & statbuf));
            state.local_filename = lyricStr;
        }
        if (! found_lyricfile)
        {
            /* JWT:LOCAL LYRIC FILE NOT FOUND, SO CHECK THE GLOBAL CONFIG PATH FOR A MATCHING LYRICS FILE: */
            lyricStr = String (str_concat ({aud_get_path (AudPath::UserDir), "/lyrics/",
                    str_decode_percent (base, ln), ".lrc"}));
            found_lyricfile = ! (g_stat ((const char *) lyricStr, & statbuf));
        }
    }
    if (! found_lyricfile)
    {
        /* JWT:NO LYRICS FILE: IF WE'RE PLAYING A DISK, CHECK FOR LYRIC FILE BY DISK-ID / DVD TITLE: */
        if (! strncmp (state.filename, "cdda://?", 8))  // FOR CDs, LOOK FOR DIRECTORY WITH TRACK LYRIC FILES:
        {
            /* FIRST LOOK FOR SEPARATE CD TRACK LYRIC FILE (~/.config/fauxdacious[_?]/<CD-ID>_tracks/track_#.lrc): */
            int track;
            const char * trackstr = state.filename + 8;

            if (sscanf (trackstr, "%d", & track) == 1)
            {
                String playingdiskid = aud_get_str (nullptr, "playingdiskid");
                if (playingdiskid[0])
                {
                    lyricStr = String (str_concat ({aud_get_path (AudPath::UserDir), "/lyrics/",
                            (const char *) playingdiskid, "/track_", trackstr, ".lrc"}));
                    found_lyricfile = ! (g_stat ((const char *) lyricStr, & statbuf));
                }
            }
        }
        if (! found_lyricfile && (! strncmp (state.filename, "cdda://", 7) || ! strncmp (state.filename, "dvd://", 6)))
        {
            /* NOW LOOK FOR CD/DVD COMBINED LYRIC FILE (~/.config/fauxdacious[_?]/<CD-ID|DVD-TITLE>.lrc):
               (IF DVD OR NO INDIVIDUAL CD TRACK LYRIC FILE FOUND) */
            String playingdiskid = aud_get_str (nullptr, "playingdiskid");
            if (playingdiskid[0])
            {
                lyricStr = String (str_concat ({aud_get_path (AudPath::UserDir), "/lyrics/",
                        (const char *) playingdiskid, ".lrc"}));
                found_lyricfile = ! (g_stat ((const char *) lyricStr, & statbuf));
            }
        }
    }

    Tuple tuple = aud_drct_get_tuple ();
    state.title = tuple.get_str (Tuple::Title);
    state.artist = tuple.get_str (Tuple::Artist);
    gtk_widget_set_sensitive (save_button, false);

    if (found_lyricfile)  // JWT:WE HAVE LYRICS STORED IN A LOCAL FILE MATCHING FILE NAME!:
    {
        AUDINFO ("i:Local lyric file found (%s).\n", (const char *) lyricStr);
        vfs_async_file_get_contents (lyricStr, get_lyrics_step_0, nullptr);
    }
    else  // NO LOCAL LYRICS FILE FOUND, SO CHECK FOR LYRIC FILE MATCHING TITLE:
    {
        if (state.title)
        {
            /* JWT:MANY STREAMS & SOME FILES FORMAT THE TITLE FIELD AS:
               "<artist> - <title> [<other-stuff>?]".  IF SO, THEN PARSE OUT THE
               ARTIST AND TITLE COMPONENTS FROM THE TITLE FOR SEARCHING LYRICWIKI:
            */
            const char * ttlstart = (const char *) state.title;
            const char * ttloffset = ttlstart ? strstr (ttlstart, " - ") : nullptr;
            if (ttloffset)
            {
                state.artist = String (str_copy (ttlstart, (ttloffset-ttlstart)));
                ttloffset += 3;
                const char * ttlend = ttloffset ? strstr (ttloffset, " - ") : nullptr;
                if (ttlend)
                    state.title = String (str_copy (ttloffset, ttlend-ttloffset));
                else
                {
                    auto split = str_list_to_index (ttloffset, "|/");
                    for (auto & str : split)
                    {
                        int ttllen_1 = strlen (str) - 1;  // "CHOMP" ANY TRAILING SPACES:
                        while (ttllen_1 >= 0 && str[ttllen_1] == ' ')
                            ttllen_1--;

                        if (ttllen_1 >= 0)
                        {
                            StringBuf titleBuf = str_copy (str);
                            titleBuf.resize (ttllen_1+1);
                            state.title = String (titleBuf);
                        }
                        break;
                    }
                }
            }
            if (! state.local_filename || ! state.local_filename[0])
            {
                /* JWT:NO LOCAL LYRIC FILE, SO TRY SEARCH FOR LYRIC FILE BY TITLE: */
                StringBuf titleBuf = str_copy (state.title);
                /* DON'T %-ENCODE SPACES BY CONVERTING TO A "LEGAL" CHAR. NOT (LIKELY) IN FILE-NAMES/TITLES: */
                str_replace_char (titleBuf, ' ', '~');
                titleBuf = str_encode_percent ((const char *) titleBuf, -1);
                str_replace_char (titleBuf, '~', ' ');  // (THEN CONVERT 'EM BACK TO SPACES)
                if (path)
                {
                    /* ENTRY IS A LOCAL FILE, SO FIRST CHECK DIRECTORY THE FILE IS IN: */
                    lyricStr = String (str_concat ({path, "/", titleBuf, ".lrc"}));
                    found_lyricfile = ! (g_stat ((const char *) lyricStr, & statbuf));
                    state.local_filename = lyricStr;
                    if (found_lyricfile)
                    {
                        AUDINFO ("i:Local lyric file found by title (%s).\n", (const char *) lyricStr);
                        vfs_async_file_get_contents (lyricStr, get_lyrics_step_0, nullptr);
                        return;
                    }
                }
                /* OTHERWISE (STREAM, ETC.), CHECK THE GLOBAL CONFIG PATH FOR LYRICS FILE MATCHING TITLE: */
                lyricStr = String (str_concat ({aud_get_path (AudPath::UserDir),
                        "/lyrics/", titleBuf, ".lrc"}));
                found_lyricfile = ! (g_stat ((const char *) lyricStr, & statbuf));
                state.local_filename = lyricStr;
                if (found_lyricfile)
                {
                    AUDINFO ("i:Global lyric file found by title (%s).\n", (const char *) lyricStr);
                    vfs_async_file_get_contents (lyricStr, get_lyrics_step_0, nullptr);
                    return;
                }
            }
        }
        /* IF HERE, NO LOCAL LYRICS FILE BY FILENAME OR TITLE, SEARCH LYRICWIKI: */
        AUDINFO ("i:No Local lyric file found, try fetching from lyricwiki...\n");
        get_lyrics_step_1 ();
    }
    lyricStr = String ();
}

static void destroy_cb ()
{
    state.filename = String ();
    state.title = String ();
    state.artist = String ();
    state.uri = String ();
    state.local_filename = String ();

    hook_dissociate ("tuple change", (HookFunction) lyricwiki_playback_began);
    hook_dissociate ("playback ready", (HookFunction) lyricwiki_playback_began);

    textview = nullptr;
    textbuffer = nullptr;
    save_button = nullptr;
    edit_button = nullptr;
}

void * LyricWiki::get_gtk_widget ()
{
    GtkWidget * vbox = build_widget ();

    hook_associate ("tuple change", (HookFunction) lyricwiki_playback_began, nullptr);
    hook_associate ("playback ready", (HookFunction) lyricwiki_playback_began, nullptr);

    if (aud_drct_get_ready ())
        lyricwiki_playback_began ();

    g_signal_connect (vbox, "destroy", destroy_cb, nullptr);

    return vbox;
}
