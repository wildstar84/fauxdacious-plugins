/*
 * Copyright (c) 2010, 2014 William Pitcock <nenolod@dereferenced.org>
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

#include <QApplication>
#include <QContextMenuEvent>
#include <QDesktopServices>
#include <QMenu>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/HTMLparser.h>
#include <libxml/xpath.h>

#define AUD_GLIB_INTEGRATION
#include <libfauxdcore/drct.h>
#include <libfauxdcore/i18n.h>
#include <libfauxdcore/plugin.h>
#include <libfauxdcore/plugins.h>
#include <libfauxdcore/probe.h>
#include <libfauxdcore/audstrings.h>
#include <libfauxdcore/hook.h>
#include <libfauxdcore/vfs_async.h>
#include <libfauxdcore/runtime.h>

#include <libfauxdqt/libfauxdqt.h>

typedef struct {
    String filename; /* of song file */
    String title, artist;
    String uri; /* URI we are trying to retrieve */
    String local_filename; /* JWT:CALCULATED LOCAL FILENAME TO SAVE LYRICS TO */
    int startlyrics;       /* JWT:OFFSET IN LYRICS WINDOW WHERE LYRIC TEXT ACTUALLY STARTS */
    bool ok2save;          /* JWT:SET TO TRUE IF GOT LYRICS FROM LYRICWIKI (LOCAL FILE DOESN'T EXIST) */
    bool ok2saveTag;       /* JWT:SET TO TRUE IF GOT LYRICS FROM LYRICWIKI && LOCAL MP3 FILE */
} LyricsState;

static LyricsState state;

class TextEdit : public QTextEdit
{
public:
    TextEdit (QWidget * parent = nullptr) : QTextEdit (parent) {}

protected:
    void contextMenuEvent (QContextMenuEvent * event);
};

class LyricWikiQt : public GeneralPlugin
{
public:
    static constexpr PluginInfo info = {
        N_("LyricWiki Plugin"),
        PACKAGE,
        nullptr, // about
        nullptr, // prefs
        PluginQtOnly
    };

    constexpr LyricWikiQt () : GeneralPlugin (info, false) {}
    void * get_qt_widget ();
};

EXPORT LyricWikiQt aud_plugin_instance;

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

                g_match_info_free (match_info);
                g_regex_unref (reg);

                if (! ret)
                {
                    reg = g_regex_new
                     ("#REDIRECT \\[\\[([^:]*):(.*)]]",
                     (GRegexCompileFlags) (G_REGEX_MULTILINE | G_REGEX_DOTALL),
                     (GRegexMatchFlags) 0, nullptr);
                    if (g_regex_match (reg, (char *) lyric, G_REGEX_MATCH_NEWLINE_ANY, & match_info))
                    {
                        state.artist = String (g_match_info_fetch (match_info, 1));
                        state.title = String (g_match_info_fetch (match_info, 2));
                        state.uri = String ();
                    }

                    g_match_info_free (match_info);
                    g_regex_unref (reg);
                }
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
                    // if (slash && ! strstr (slash, "lyrics.wikia.com")) // JWT:FIXME?! - CHGD. TO MATCH GTK SIDE!:
                    if (slash)
                        uri = String (str_printf ("https://lyrics.fandom.com/index.php?"
                         "action=edit&title=%s", slash + 1));
                    else
                        uri = String ("N/A");
                }

                xmlFree ((xmlChar *) lyric);
            }
        }

        xmlFreeDoc (doc);
    }

    return uri;
}

static void update_lyrics_window (const char * title, const char * artist, const char * lyrics);

static void get_lyrics_step_1 ();

static void get_lyrics_step_3 (const char * uri, const Index<char> & buf, void *)
{
    if (! state.uri || strcmp (state.uri, uri))
        return;

    if (! buf.len ())
    {
        update_lyrics_window (_("Error"), nullptr,
         str_printf (_("Unable to fetch %s"), uri));
        return;
    }

    CharPtr lyrics = scrape_lyrics_from_lyricwiki_edit_page (buf.begin (), buf.len ());

    if (! state.uri)
    {
        get_lyrics_step_1 ();
        return;
    }

    if (! lyrics)
    {
        update_lyrics_window (_("No lyrics Found"),
                (const char *) str_concat ({"Title: ", (const char *) state.title, "\nArtist: ",
                        (const char *) state.artist}),
                str_printf (_("Unable to parse(3) %s"), uri));
        return;
    }

    update_lyrics_window (state.title, state.artist, lyrics);
    state.ok2save = true;
    if (! strncmp ((const char *) state.filename, "file://", 7)
            && str_has_suffix_nocase ((const char *) state.filename, ".mp3"))
    {
        String error;
        VFSFile file (state.filename, "r");
        PluginHandle * decoder = aud_file_find_decoder (state.filename, true, file, & error);
        bool can_write = aud_file_can_write_tuple (state.filename, decoder);
        if (can_write)
            state.ok2saveTag = true;
    }
}

static void get_lyrics_step_2 (const char * uri1, const Index<char> & buf, void *)
{
    if (! state.uri || strcmp (state.uri, uri1))
        return;

    if (! buf.len ())
    {
        update_lyrics_window (_("Error"), nullptr,
         str_printf (_("Unable to fetch %s"), uri1));
        return;
    }

    String uri = scrape_uri_from_lyricwiki_search_result (buf.begin (), buf.len ());

    if (! uri)
    {
        update_lyrics_window (_("Error"), nullptr,
         str_printf (_("Unable to parse(2) %s"), uri1));
        return;
    }
    else if (uri == String ("N/A"))
    {
        update_lyrics_window (state.title, state.artist,
         _("No lyrics available"));
        return;
    }

    state.uri = uri;

    update_lyrics_window (state.title, state.artist, _("Looking for lyrics ..."));
    vfs_async_file_get_contents (uri, get_lyrics_step_3, nullptr);
}

static void get_lyrics_step_1 ()
{
    if (! state.artist || ! state.title)
    {
        update_lyrics_window (_("Error"), nullptr, _("Missing title and/or artist"));
        return;
    }

    StringBuf title_buf = str_encode_percent (state.title);
    StringBuf artist_buf = str_encode_percent (state.artist);

    state.uri = String (str_printf ("https://lyrics.fandom.com/api.php?"
     "action=lyrics&artist=%s&song=%s&fmt=xml", (const char *) artist_buf,
     (const char *) title_buf));

    update_lyrics_window (state.title, state.artist, _("Connecting to lyrics.fandom.com ..."));
    vfs_async_file_get_contents (state.uri, get_lyrics_step_2, nullptr);
}

static void get_lyrics_step_0 (const char * uri, const Index<char> & buf, void *)
{
    if (! buf.len ())
    {
        update_lyrics_window (_("Error"), nullptr,
         str_printf (_("Unable to fetch file %s"), uri));
        return;
    }

    StringBuf nullterminated_buf = str_copy (buf.begin (), buf.len ());
    update_lyrics_window (state.title, state.artist, (const char *) nullterminated_buf);

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
    }
}

static QTextEdit * textedit;

static void save_lyrics_locally ()
{
    if (state.local_filename)
    {
        QString lyrics = textedit->toPlainText ();
        if (! lyrics.isNull() && ! lyrics.isEmpty())
        {
            if (state.startlyrics > 0)
                lyrics.remove(0, state.startlyrics);

            int sz = lyrics.length ();
            if (sz > 0)
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
                    if (file.fwrite (lyrics.toUtf8().constData(), 1, sz) == sz)
                        AUDINFO ("i:Successfully saved %d bytes of lyrics locally to (%s).\n", sz,
                                (const char *) state.local_filename);

                    state.ok2save = false;
                }
            }
        }
    }
}

static void save_lyrics_in_id3tag ()
{
    QString lyrics = textedit->toPlainText ();
    if (! lyrics.isNull() && ! lyrics.isEmpty())
    {
        if (state.startlyrics > 0)
            lyrics.remove(0, state.startlyrics);

        int sz = lyrics.length ();
        if (sz > 0)
        {
            String error;
            Tuple tuple = aud_drct_get_tuple ();
            tuple.set_str (Tuple::Lyrics, String (str_copy (lyrics.toUtf8().constData(), sz)));
            VFSFile file (state.filename, "r");
            PluginHandle * decoder = aud_file_find_decoder (state.filename, true, file, & error);
            bool success = aud_file_write_tuple (state.filename, decoder, tuple);
            if (success)
                AUDINFO ("i:Successfully saved %d bytes of lyrics to Id3 tag.\n", sz);
            else
                AUDERR ("e:Could not save lyrics to id3 tag.\n");

            state.ok2saveTag = false;
        }
    }
}

static void update_lyrics_window (const char * title, const char * artist, const char * lyrics)
{
    if (! textedit)
        return;

    textedit->document ()->clear ();

    QTextCursor cursor (textedit->document ());
    cursor.insertHtml (QString ("<big><b>") + QString (title) + QString ("</b></big>"));

    if (artist)
    {
        cursor.insertHtml (QString ("<br><i>") + QString (artist) + QString ("</i>"));
    }

    cursor.insertHtml ("<br><br>");
    QString prelyrics = textedit->toPlainText ();
    state.startlyrics = prelyrics.length ();
    cursor.insertText (lyrics);
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
    state.ok2save = false;
    state.ok2saveTag = false;

    if (found_lyricfile)  // JWT:WE HAVE LYRICS STORED IN A LOCAL FILE MATCHING FILE NAME!:
    {
        AUDINFO ("i:Local lyric file found (%s).\n", (const char *) lyricStr);
        vfs_async_file_get_contents (lyricStr, get_lyrics_step_0, nullptr);
    }
    else  // NO LOCAL LYRICS FILE FOUND, SO CHECK FOR LYRIC FILE MATCHING TITLE:
    {
        String lyricsFromTuple = tuple.get_str (Tuple::Lyrics);
        if (lyricsFromTuple && lyricsFromTuple[0])
        {
            AUDDBG ("i:Lyrics found in ID3 tag.\n");
            update_lyrics_window (state.title, state.artist, (const char *) lyricsFromTuple);
        }
        else
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
    }
    lyricStr = String ();
}

static void lw_cleanup (QObject * object = nullptr)
{
    state.filename = String ();
    state.title = String ();
    state.artist = String ();
    state.uri = String ();
    state.local_filename = String ();

    hook_dissociate ("tuple change", (HookFunction) lyricwiki_playback_began);
    hook_dissociate ("playback ready", (HookFunction) lyricwiki_playback_began);

    textedit = nullptr;
}

void * LyricWikiQt::get_qt_widget ()
{
    textedit = new TextEdit;
    textedit->setReadOnly (true);

#ifdef Q_OS_MAC  // Mac-specific font tweaks
    textedit->document ()->setDefaultFont (QApplication::font ("QTipLabel"));
#endif

    hook_associate ("tuple change", (HookFunction) lyricwiki_playback_began, nullptr);
    hook_associate ("playback ready", (HookFunction) lyricwiki_playback_began, nullptr);

    if (aud_drct_get_ready ())
        lyricwiki_playback_began ();

    QObject::connect (textedit, & QObject::destroyed, lw_cleanup);

    return textedit;
}

void TextEdit::contextMenuEvent (QContextMenuEvent * event)
{
    QMenu * menu = createStandardContextMenu ();
    menu->addSeparator ();

    if (state.uri)
    {
        QAction * edit = menu->addAction (_("Edit Lyricwiki"));
        QObject::connect (edit, & QAction::triggered, [] () {
            QDesktopServices::openUrl (QUrl ((const char *) state.uri));
        });
    }
    if (state.ok2save)
    {
        QAction * save_button = menu->addAction (_("Save Locally"));
        QObject::connect (save_button, & QAction::triggered, [] () {
            save_lyrics_locally ();
        });
    }
    if (state.ok2saveTag)
    {
        QAction * tuple_save_button = menu->addAction (_("Save ID3"));
        QObject::connect (tuple_save_button, & QAction::triggered, [] () {
            save_lyrics_in_id3tag ();
        });
    }
    menu->exec (event->globalPos ());
    delete menu;
}
