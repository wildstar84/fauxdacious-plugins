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
#include <pthread.h>

#ifdef _WIN32
#include <windows.h>
#include <winbase.h>
#endif

#include <QApplication>
#include <QContextMenuEvent>
#include <QDesktopServices>
#include <QMenu>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QThread>
#include <QEventLoop>

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
#include <libfauxdcore/preferences.h>

#include <libfauxdqt/libfauxdqt.h>

#ifdef S_IRGRP
#define DIRMODE (S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH)
#else
#define DIRMODE (S_IRWXU)
#endif

typedef struct {
    String filename;       /* of song file */
    String title, artist, album; /* INPUT PARAMETERS, PASSED BETWEEN FUNCTIONS */
    String uri;            /* URI we are trying to retrieve */
    String local_filename; /* JWT:CALCULATED LOCAL FILENAME TO SAVE LYRICS TO */
    int startlyrics;       /* JWT:OFFSET IN LYRICS WINDOW WHERE LYRIC TEXT ACTUALLY STARTS */
    bool ok2save;          /* JWT:SET TO TRUE IF GOT LYRICS FROM LYRICWIKI (LOCAL FILE DOESN'T EXIST) */
    bool ok2saveTag;       /* JWT:SET TO TRUE IF GOT LYRICS FROM LYRICWIKI && LOCAL MP3 FILE */
    bool ok2edit;          /* JWT:SET TO TRUE IF USER CAN EDIT LYRICS */
    String shotitle;       /* JWT:NEXT 3 FOR THREAD TO SAVE LYRIC DATA UNTIL MAIN THREAD CAN DISPLAY IT: */
    String shoartist;
    String sholyrics;
} LyricsState;

static bool frominit = false;  // JWT:TRUE WHEN THREAD STARTED BY SONG CHANGE (album_init()).
static bool skipreset = false; // JWT:TRUE WHILE THREAD RUNNING AFTER STARTED BY SONG CHANGE (album_init()).
static bool skiplyricsupdate = false; // JWT:IF TRUE, THREAD YIELDED TO A LATER ONE, SO DON'T UPDATE LYRICS (YET)!
static QEventLoop q_eventloop; // JWT:LOCAL EVENT LOOP TO WAIT FOR THREAD (SINCE Qt WON'T ALLOW THREAD TO UPDATE WIDGET?!
static LyricsState state;      // GLOBAL VARIABLE STRUCT. */
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

class TextEdit : public QTextEdit
{
public:
    TextEdit (QWidget * parent = nullptr) : QTextEdit (parent) {}

    void show_lyrics ()
    {
        document ()->clear ();
        if (! state.shotitle)  return;

        QTextCursor cursor (document ());
        QString lyrichtml = QString ("<big><b>") + QString (state.shotitle) + QString ("</b></big>");

        if (state.shoartist)
            lyrichtml.append (QString ("<br><i>") + QString (state.shoartist) + QString ("</i>"));

        lyrichtml.append (QString ("<br><br>"));
        cursor.insertHtml (lyrichtml);
        QString prelyrics = toPlainText ();

        state.startlyrics = prelyrics.length ();
        if (state.sholyrics)
            cursor.insertText (QString (state.sholyrics));

        state.ok2save = false;
    }

protected:
    void contextMenuEvent (QContextMenuEvent * event);
};

class LyricWikiQt : public GeneralPlugin
{
public:
    static const char * const defaults[];
    static const PreferencesWidget widgets[];
    static const PluginPreferences prefs;

    static constexpr PluginInfo info = {
        N_("LyricWiki Plugin"),
        PACKAGE,
        nullptr, // about
        & prefs, // prefs
        PluginQtOnly
    };

    constexpr LyricWikiQt () : GeneralPlugin (info, false) {}

    bool init ();
    void * get_qt_widget ();
};

EXPORT LyricWikiQt aud_plugin_instance;

const char * const LyricWikiQt::defaults[] = {
    "search_internet", "TRUE",            // SEARCH FOR LYRICS FROM WEB (IF NOT FOUND LOCALLY)
    nullptr
};

const PreferencesWidget LyricWikiQt::widgets[] = {
    WidgetCheck (N_("Fetch lyrics from internet?"),
        WidgetBool ("lyricwiki", "search_internet")),
    WidgetCheck (N_("Cache (save) lyrics to disk?"),
        WidgetBool ("lyricwiki", "cache_lyrics")),
    WidgetCheck (N_("Try to save by song file-name first?"),
        WidgetBool ("lyricwiki", "save_by_songfile")),
    WidgetEntry (N_("Helper:"),
        WidgetString ("audacious", "lyric_helper"))
};

const PluginPreferences LyricWikiQt::prefs = {{widgets}};

static TextEdit * textedit;

bool LyricWikiQt::init ()
{
    aud_config_set_defaults ("lyricwiki", defaults);

    return true;
}

/*
 * DEPRECIATED: Suppress libxml warnings, because lyricwiki does not generate anything near
 * valid HTML.
 */
static void libxml_error_handler (void * ctx, const char * msg, ...)
{
}

/* DEPRECIATED: CALLED FOR FETCHING LYRICS THE OLD-SCHOOL AUDACIOUS WAY (NO PERL HELPER): */
static CharPtr scrape_lyrics_from_lyricwiki_edit_page (const char * buf, int64_t len)
{
    xmlDocPtr doc;
    CharPtr ret;

    /*
     * temporarily set our error-handling function to our suppression function,
     * but we have to set it back because other components of Audacious depend
     * on libxml and we don't want to step on their code paths.
     *
     * unfortunately, libxml is anti-social and provides us with no way to get
     * the previous error function, so we just have to set it back to default after
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

/* DEPRECIATED: CALLED FOR FETCHING LYRICS THE OLD-SCHOOL AUDACIOUS WAY (NO PERL HELPER): */
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
     * temporarily set our error-handling function to our suppression function,
     * but we have to set it back because other components of Audacious depend
     * on libxml and we don't want to step on their code paths.
     *
     * unfortunately, libxml is anti-social and provides us with no way to get
     * the previous error function, so we just have to set it back to default after
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

static void update_lyrics (const char * title, const char * artist, const char * lyrics);

static void save_lyrics_locally (bool haveonscreen);

/* DEPRECIATED: STEP 3 OF 3 (FOR FETCHING LYRICS THE OLD-SCHOOL AUDACIOUS WAY (NO HELPER)): */
static void get_lyrics_step_3 (const char * uri, const Index<char> & buf, void *)
{
    /* JWT:WE'RE ONLY HERE IF *NOT* USING THE PERL HELPER (THE OLD-SCHOOL WAY)! */
    if (! state.uri || strcmp (state.uri, uri))
        return;

    if (! buf.len ())
    {
        update_lyrics (_("Error"), nullptr,
                str_printf (_("Unable to fetch %s"), uri));
        textedit->show_lyrics ();
        state.ok2edit = true;
        return;
    }

    CharPtr lyrics = scrape_lyrics_from_lyricwiki_edit_page (buf.begin (), buf.len ());

    if (! lyrics)
    {
        update_lyrics (_("No lyrics Found"),
                (const char *) str_concat ({"Title: ", (const char *) state.title, "\nArtist: ",
                        (const char *) state.artist}),
                str_printf (_("Unable to parse(3) %s"), uri));
        textedit->show_lyrics ();
        state.ok2edit = true;
        return;
    }

    update_lyrics (state.title, state.artist, lyrics);
    textedit->show_lyrics ();
    AUDINFO ("i:Lyrics came from old LyricWiki site!\n");
    state.ok2edit = true;
    state.ok2save = true;
    if (aud_get_bool ("lyricwiki", "cache_lyrics"))
        save_lyrics_locally (true);

    /* JWT:ALLOW 'EM TO EMBED IN TAG, IF POSSIBLE. */
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

/* DEPRECIATED: STEP 2 OF 3 (FOR FETCHING LYRICS THE OLD-SCHOOL AUDACIOUS WAY (NO HELPER)): */
static void get_lyrics_step_2 (const char * uri1, const Index<char> & buf, void *)
{
    if (! state.uri || strcmp (state.uri, uri1))
        return;

    if (! buf.len ())
    {
        update_lyrics (_("Error"), nullptr,
                str_printf (_("Unable to fetch %s"), uri1));
        textedit->show_lyrics ();
        state.ok2edit = false;
        return;
    }

    String uri = scrape_uri_from_lyricwiki_search_result (buf.begin (), buf.len ());

    if (! uri)
    {
        update_lyrics (_("Error"), nullptr,
                str_printf (_("Unable to parse(2) %s"), uri1));
        textedit->show_lyrics ();
        state.ok2edit = false;
        return;
    }
    else if (uri == String ("N/A"))
    {
        update_lyrics (state.title, state.artist, _("No lyrics available"));
        textedit->show_lyrics ();
        state.ok2edit = false;
        return;
    }

    state.uri = uri;

    update_lyrics (state.title, state.artist, _("Looking for lyrics ..."));
    textedit->show_lyrics ();
    state.ok2edit = true;
    vfs_async_file_get_contents (uri, get_lyrics_step_3, nullptr);
}

/* (SEPARATE THREAD) HANDLE FETCHING LYRICS FROM WEB VIA HELPER (1-STEP) OR fandom.com? (STEP 1 OF 3): */
static void * helper_thread_fn (void * data)
{
    if (frominit)  // TRUE IF SONG-START, FALSE ON TUPLE-CHANGE!
    {
        skipreset = true;
        QThread::usleep (2000000);  // SLEEP 2" TO ALLOW FOR ANY TUPLE CHANGE TO OVERRIDE! */
        if (! frominit)
        {
            skipreset = false;
            skiplyricsupdate = true;
            pthread_exit (nullptr);
            return nullptr;
        }
    }

    pthread_mutex_lock (& mutex);

    String lyric_helper = aud_get_str ("audacious", "lyric_helper");

    if (lyric_helper[0])  //JWT:WE HAVE A PERL HELPER, LESSEE IF IT CAN FIND/DOWNLOAD LYRICS FOR US:
    {
        bool lyrics_found = false;
        GStatBuf statbuf;
        if (! state.album)
            state.album = String ("_");

        AUDINFO ("i:HELPER FOUND: WILL DO (%s)\n", (const char *) str_concat ({lyric_helper, " \"",
                (const char *) state.artist, "\" \"",
                (const char *) state.title, "\" ", aud_get_path (AudPath::UserDir),
                " \"", (const char *) state.album, "\" "}));
#ifdef _WIN32
        WinExec ((const char *) str_concat ({lyric_helper, " \"", (const char *) state.artist, "\" \"",
                (const char *) state.title, "\" ", aud_get_path (AudPath::UserDir),
                " \"", (const char *) state.album, "\" "}),
                SW_HIDE);
#else
        system ((const char *) str_concat ({lyric_helper, " \"", (const char *) state.artist, "\" \"",
                (const char *) state.title, "\" ", aud_get_path (AudPath::UserDir),
                " \"", (const char *) state.album, "\" "}));
#endif
        String lyric_fid = String (str_concat ({aud_get_path (AudPath::UserDir), "/_tmp_lyrics.txt"}));

        state.ok2edit = false;  /* NO EDITING LYRICS ON HELPER-SERVED SITES! */
        if (g_stat ((const char *) lyric_fid, & statbuf) == 0)
        {
            VFSFile lyrics_file ((const char *) lyric_fid, "r");
            if (lyrics_file)
            {
                Index<char> lyrics = lyrics_file.read_all ();
                if (lyrics.len () > 1)
                {
                    lyrics.resize (lyrics.len ()+1);
                    lyrics[lyrics.len ()-1] = '\0';
                    lyrics_found = true;
                    update_lyrics (state.title, state.artist, (const char *) lyrics.begin ());
                    state.ok2save = true;
                    if (aud_get_bool ("lyricwiki", "cache_lyrics"))
                        save_lyrics_locally (false);

                    AUDINFO ("i:Lyrics came from HELPER!\n");
                    /* JWT:ALLOW 'EM TO EMBED IN TAG, IF POSSIBLE. */
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
            }
        }
        if (! lyrics_found)
        {
            update_lyrics (_("No lyrics Found"),
                    (const char *) str_concat ({"Title: ", (const char *) state.title, "\nArtist: ",
                    (const char *) state.artist}),
                    str_printf (_("Unable to fetch lyrics.")));

            pthread_mutex_unlock (& mutex);
            goto THREAD_EXIT;
        }
    }
    else /* NO HELPER, TRY THE OLD SCHOOL "3-STEP C" WAY: (MAYBE fandom.com CAME BACK OR AUDACIOUS FIXED?) */
    {
        StringBuf title_buf = str_encode_percent (state.title);
        StringBuf artist_buf = str_encode_percent (state.artist);

        state.uri = String (str_printf ("https://lyrics.fandom.com/api.php?"
                "action=lyrics&artist=%s&song=%s&fmt=xml", (const char *) artist_buf,
                (const char *) title_buf));

        update_lyrics (state.title, state.artist, _("Connecting to lyrics.fandom.com ..."));
        state.ok2edit = false;
        vfs_async_file_get_contents (state.uri, get_lyrics_step_2, nullptr);
    }

    pthread_mutex_unlock (& mutex);

THREAD_EXIT:
    skiplyricsupdate = false;
    skipreset = false;
    q_eventloop.exit ();
    pthread_exit (nullptr);
    return nullptr;
}

/* JWT:HANDLE LYRICS FROM LOCAL LYRICS FILES: */
static void get_lyrics_step_0 (const char * uri, const Index<char> & buf, void *)
{
    if (! buf.len ())
    {
        update_lyrics (_("Error"), nullptr,
                str_printf (_("Unable to fetch file %s"), uri));
        textedit->show_lyrics ();
        state.ok2edit = false;
        return;
    }

    StringBuf nullterminated_buf = str_copy (buf.begin (), buf.len ());
    update_lyrics (state.title, state.artist, (const char *) nullterminated_buf);
    textedit->show_lyrics ();

    /* JWT:ALLOW 'EM TO EDIT LYRICWIKI, EVEN IF LYRICS ARE LOCAL, IF THEY HAVE BOTH REQUIRED FIELDS: */
    /* BUT ONLY IF USING OLD SITE (*NOT* USING THE PERL "HELPER")! */
    String lyric_helper = aud_get_str ("audacious", "lyric_helper");
    if (! lyric_helper[0] && state.artist && state.title)
    {
        StringBuf title_buf = str_copy (state.title);
        str_replace_char (title_buf, ' ', '_');
        title_buf = str_encode_percent (title_buf, -1);
        StringBuf artist_buf = str_copy (state.artist);
        str_replace_char (artist_buf, ' ', '_');
        artist_buf = str_encode_percent (artist_buf, -1);
        state.uri = String (str_printf ("https://lyrics.fandom.com/index.php?action=edit&title=%s:%s",
                (const char *) artist_buf, (const char *) title_buf));
        state.ok2edit = true;
    }
    else
        state.ok2edit = false;

    AUDINFO ("i:Lyrics came from local file: (%s)!\n", (const char *) state.local_filename);
    /* JWT:ALLOW 'EM TO EMBED IN TAG, IF POSSIBLE, EVEN IF LYRICS ARE FROM LOCAL FILE. */
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

/* CALLED WHEN USER SELECTS "SAVE LOCALLY" FROM MENU: */
static void save_lyrics_locally (bool haveonscreen)
{
    // haveonscreen = false MEANS CACHING FROM THREAD (*BEFORE* LYRICS COPIED TO SCREEN WIDGET)!
    if (state.local_filename && state.local_filename[0])
    {
        AUDINFO ("i:Saving lyrics locally to (%s)!\n", (const char *) state.local_filename);
        QString lyrics = haveonscreen ? textedit->toPlainText () : QString (state.sholyrics);
        if (! lyrics.isNull() && ! lyrics.isEmpty())
        {
            if (state.startlyrics > 0)
                lyrics.remove(0, state.startlyrics);

            int sz = lyrics.length ();
            if (sz > 0)
            {
                String user_dir = String (aud_get_path (AudPath::UserDir));
                if (strstr ((const char *) state.local_filename, (const char *) user_dir))
                {
                    /* WE'RE STORING IN GLOBAL LYRICS CACHE, CREATE DIRECTORIES AS NEEDED: */
                    StringBuf artist_path = filename_get_parent (state.local_filename);
                    if (g_mkdir_with_parents (artist_path, DIRMODE) < 0)
                    {
                        AUDERR ("e:Could not create missing lyrics directory (%s): %s!\n", (const char *) artist_path, strerror (errno));
                        return;
                    }
                }
                /* WRITE THE LYRICS FILE (WHETHER LOCAL OR GLOBAL LYRICS CACHE: */
                VFSFile file (state.local_filename, "w");
                if (file)
                {
                    if (file.fwrite (lyrics.toUtf8().constData(), 1, sz) == sz)
                    {
                        AUDINFO ("i:Successfully saved %d bytes of lyrics locally to (%s).\n", sz,
                                (const char *) state.local_filename);

                        state.ok2save = false;
                    }
                }
            }
        }
    }
}

/* CALLED WHEN USER SELECTS "SAVE IN ID3 TAG" FROM MENU (LOCAL SONG FILES ONLY): */
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

/* CALLED BY BOTH MAIN AND THREAD TO UPDATE LYRICS (FOR LATER DISPLAYING IN THE WIDGET): */
static void update_lyrics (const char * title, const char * artist, const char * lyrics)
{
    state.shotitle = title ? String (title) : String ("");
    state.shoartist = artist ? String (artist) : String ("");
    state.sholyrics = lyrics ? String (lyrics) : String ("");
}

/* CALLED ON PLAYBACK START OR TUPLE-CHANGE (WHEN WE NEED LYRICS): */
static void lyricwiki_playback ()
{
    /* FIXME: cancel previous VFS requests (not possible with current API) */

    bool found_lyricfile = false;
    GStatBuf statbuf;
    String lyricStr = String ("");
    StringBuf path = StringBuf ();

    state.filename = aud_drct_get_filename ();
    state.uri = String ();
    state.ok2edit = false;
    state.local_filename = String ("");

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
                state.local_filename = lyricStr;
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
            if (found_lyricfile || (! state.local_filename || ! state.local_filename[0]))
                state.local_filename = lyricStr;  // SET FOUND CD-WIDE LYRICS FOUND OR NO TRACK FILE SET (DVD).
        }
    }
    if (! found_lyricfile)
    {
        /* JWT: EXTRACT JUST THE "NAME" PART TO USE TO NAME THE LYRICS FILE: */
        const char * slash = state.filename ? strrchr (state.filename, '/') : nullptr;
        const char * base = slash ? slash + 1 : nullptr;

        if (base && base[0] != '\0' && base[0] != '?' && strncmp (base, "-.", 2))  // WE KNOW THAT base IS NOT NULL, IF TRUE!
        {
            /* JWT:IF WE'RE A "FILE", FIRST CHECK LOCAL DIRECTORY FOR A LYRICS FILE MATCHING FILE-NAME: */
            const char * dot = strrchr (base, '.');
            int ln = (dot && ! strstr_nocase (dot, ".cue?")) ? (dot - base) : -1;  // SET TO FULL LENGTH(-1) IF NO EXTENSION OR NOT A CUESHEET.
            if (! strncmp ((const char *) state.filename, "file://", 7))
            {
                /* WE ARE A VALID LOCAL SONG FILE NAME! */
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
                /* NOTE: REGARDLESS OF WHETHER OR NOT WE FIND LYRICS IN THE GLOBAL DIR, WE KEEP state.local_filename==LOCAL FILENAME.lrc! */
                /* (WE'LL OVERRIDE IT LATER (ARTIST NAME/TITLE) IF WE HAVE BOTH, UNLESS save_by_songfile(name) IS SET). */
            }
        }
    }

    Tuple tuple = aud_drct_get_tuple ();
    state.title = tuple.get_str (Tuple::Title);
    state.artist = tuple.get_str (Tuple::Artist);
    state.album = tuple.get_str (Tuple::Album);
    state.ok2save = false;
    state.ok2saveTag = false;

    if (found_lyricfile)  // JWT:WE HAVE LYRICS STORED IN A LOCAL FILE MATCHING FILE NAME!:
    {
        AUDINFO ("i:Local lyric file found (%s).\n", (const char *) lyricStr);
        vfs_async_file_get_contents (lyricStr, get_lyrics_step_0, nullptr);
    }
    else  // NO LOCAL LYRICS FILE FOUND, SO CHECK FOR ID3 LYRICS TAGS, THEN GLOBAL LYRIC FILE MATCHING ARTIST/TITLE:
    {
        bool need_lyrics = true;
        String lyricsFromTuple = tuple.get_str (Tuple::Lyrics);

        if (lyricsFromTuple && lyricsFromTuple[0])
        {
            AUDDBG ("i:Lyrics found in ID3 tag.\n");
            update_lyrics (state.title, state.artist, (const char *) lyricsFromTuple);
            textedit->show_lyrics ();
            state.ok2edit = false;
            state.ok2save = true;
            AUDINFO ("i:Lyrics came from id3 tag!\n");
            need_lyrics = false;
        }
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
            if (state.artist)
            {
                /* JWT:NO LOCAL LYRIC FILE, SO TRY SEARCH FOR LYRIC FILE BY ARTIST/TITLE: */
                /* CHECK THE GLOBAL CONFIG PATH FOR LYRICS FILE MATCHING ARTIST/TITLE: */
                bool save_by_songfile = aud_get_bool ("lyricwiki", "save_by_songfile");
                auto user_dir = aud_get_path (AudPath::UserDir);
                StringBuf base_path = filename_build ({user_dir, "lyrics"});
                StringBuf artist_path = filename_build ({base_path, state.artist});
                lyricStr = String (str_concat({filename_build({artist_path, state.title}), ".lrc"}));
                found_lyricfile = ! (g_stat ((const char *) lyricStr, & statbuf));

                /* local_filename := (GLOBAL) ARTIST NAME/TITLE, IF NOT ALREADY SET *OR* save_by_songfile NOT SET. */
                if (! state.local_filename || ! state.local_filename[0] || ! save_by_songfile)
                    state.local_filename = lyricStr;  /* THE FILE NAME WE'LL CACHE OR "SAVE LOCALLY" TO. */

                if (need_lyrics && found_lyricfile)
                {
                    AUDINFO ("i:Global lyric file found by artist/title (%s).\n", (const char *) lyricStr);
                    vfs_async_file_get_contents (lyricStr, get_lyrics_step_0, nullptr);
                    lyricStr = String ();
                    if (save_by_songfile)
                        state.ok2save = true;

                    return;
                }
            }
        }
        /* IF HERE, NO LOCAL LYRICS FILE BY FILENAME OR TITLE, SEARCH LYRICWIKI: */
        if (need_lyrics)
        {
            AUDINFO ("i:No Local lyric file found, try fetching from lyricwiki...\n");
            update_lyrics ("Searching web for lyrics...", nullptr, nullptr);
            textedit->show_lyrics ();
            if (! state.artist || ! state.title)
            {
                update_lyrics (_("Error"), nullptr, _("Missing title and/or artist"));
                textedit->show_lyrics ();
                state.ok2edit = false;
                return;
            }
            if (! aud_get_bool ("lyricwiki", "search_internet"))
            {
                update_lyrics (_("No lyrics Found locally"),
                        (const char *) str_concat ({"Title: ", (const char *) state.title, "\nArtist: ",
                        (const char *) state.artist}),
                        str_printf (_("Unable to fetch lyrics (Fetch lyrics from internet option not enabled).")));
                textedit->show_lyrics ();
                return;
            }

            if (! skipreset)
                textedit->show_lyrics ();

            pthread_attr_t thread_attrs;
            if (! pthread_attr_init (& thread_attrs))
            {
                if (! pthread_attr_setdetachstate (& thread_attrs, PTHREAD_CREATE_DETACHED)
                        || ! pthread_attr_setscope (& thread_attrs, PTHREAD_SCOPE_SYSTEM))
                {
                    pthread_t helper_thread;

                    if (pthread_create (&helper_thread, nullptr, helper_thread_fn, nullptr))
                        AUDERR ("s:Error creating helper thread: %s - Expect Delays!...\n", strerror (errno));
                    else  // THREAD STARTED!...
                    {
                        if (! q_eventloop.isRunning ())
                            q_eventloop.exec ();   // WAIT HERE UNTIL THREAD IS DONE, BUT KEEP GUI WORKING!

                        if (! skiplyricsupdate)
                            textedit->show_lyrics ();
                    }
                }
                else
                    AUDERR ("s:Error detatching helper thread: %s!\n", strerror (errno));

                if (pthread_attr_destroy (& thread_attrs))
                    AUDERR ("s:Error destroying helper thread attributes: %s!\n", strerror (errno));
            }
            else
                AUDERR ("s:Error initializing helper thread attributes: %s!\n", strerror (errno));
        }
    }
    lyricStr = String ();
}

/* CALLED WHEN PLAYBACK STARTS: */
static void lyricwiki_playback_began ()
{
    skipreset = false;
    frominit = true;
    lyricwiki_playback ();
}

/* CALLED WHEN TUPLE CHANGES (STREAMS CHANGE TITLE, ETC. WHILE PLAYING:  */
static void lyricwiki_playback_changed ()
{
    frominit = false;
    lyricwiki_playback ();
}

/* CALLED WHEN PLAYBACK IS STOPPED, MAKE SURE NO THREADS HAVE THE LOCAL EVENT LOOP RUNNING!: */
static void kill_thread_eventloop ()
{
    skipreset = false;
    if (q_eventloop.isRunning ())
        q_eventloop.exit ();
}

/* CALLED WHEN USER CHANGES LYRICS TEXT IN WIDGET, MAKE [SAVE] OPTION VISIBLE: */
static void allow_usersave ()
{
    state.ok2save = true;
}

/* CALLED ON SHUTDOWN TO CLEAN UP: */
static void lw_cleanup (QObject * object = nullptr)
{
    state.filename = String ();
    state.title = String ();
    state.artist = String ();
    state.uri = String ();
    state.local_filename = String ();

    hook_dissociate ("playback ready", (HookFunction) lyricwiki_playback_began);
    hook_dissociate ("tuple change", (HookFunction) lyricwiki_playback_changed);
    hook_dissociate ("playback stop", (HookFunction) kill_thread_eventloop);

    textedit = nullptr;
}

/* CALLED ON STARTUP (WIDGET CREATION): */
void * LyricWikiQt::get_qt_widget ()
{
    textedit = new TextEdit;
    textedit->setReadOnly (false);

#ifdef Q_OS_MAC  // Mac-specific font tweaks
    textedit->document ()->setDefaultFont (QApplication::font ("QTipLabel"));
#endif

    hook_associate ("playback stop", (HookFunction) kill_thread_eventloop, nullptr);
    hook_associate ("tuple change", (HookFunction) lyricwiki_playback_changed, nullptr);
    hook_associate ("playback ready", (HookFunction) lyricwiki_playback_began, nullptr);

    if (aud_drct_get_ready ())
        lyricwiki_playback_began ();

    QObject::connect (textedit, & QObject::destroyed, lw_cleanup);
    QObject::connect (textedit, & QTextEdit::textChanged, allow_usersave);

    return textedit;
}

/* CALLED ON RIGHT-CLICK IN WIDGET TO POPUP MENU: */
void TextEdit::contextMenuEvent (QContextMenuEvent * event)
{
    QMenu * menu = createStandardContextMenu ();
    menu->addSeparator ();

    if (state.ok2edit)
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
            save_lyrics_locally (true);
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
