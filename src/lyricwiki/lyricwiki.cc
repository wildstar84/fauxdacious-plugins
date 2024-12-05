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

#include <iostream>
#include <vector>
#include <sstream>   // For std::istringstream
#include <regex>     // For std::regex and std::smatch
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <gtk/gtk.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/HTMLparser.h>
#include <libxml/xpath.h>
#include <pthread.h>

#ifdef _WIN32
#include <windows.h>
#include <winbase.h>
#endif

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
#include <libfauxdcore/playlist.h>
#include <libfauxdgui/gtk-compat.h>

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
    gint startlyrics;      /* JWT:OFFSET IN LYRICS WINDOW WHERE LYRIC TEXT ACTUALLY STARTS */
    bool Wasok2saveTag;    /* JWT:SET TO TRUE AFTER SAVE TO TAGS, FOR RESETTING IF USER EDITS THEM */
    bool force_refresh;    /* JWT:TRUE IF USER FORCED REFRESH VIA [Refresh] BUTTON (DON'T WAIT FOR ALBUMART! */
    bool synclyrics;       /* JWT:SCROLL LYRICS ON TIMESTAMPS (AS SET WHEN PLAYBACK STARTED) */
    String shotitle;       /* JWT:NEXT 3 FOR THREAD TO SAVE LYRIC DATA UNTIL MAIN THREAD CAN DISPLAY IT: */
    String shoartist;
    String sholyrics;
    int current_playlist;  /* JWT:SAVE THE PLAYLIST CURRENT WHEN ENTRY STARTS PLAYING! */
} LyricsState;

struct TimedLyricLine {
    int timestamp_ms;   // Timestamp in milliseconds
    String text;        // Lyric text at this timestamp
};

std::vector<TimedLyricLine> timed_lyrics;  // Stores parsed lyrics with timestamps

class LyricWiki : public GeneralPlugin
{
public:
    static const char * const defaults[];
    static const char about[];
    static const PreferencesWidget widgets[];
    static const PluginPreferences prefs;

    static constexpr PluginInfo info = {
        N_("Lyrics"),
        PACKAGE,
        about,   // about
        & prefs, // prefs
        PluginGLibOnly
    };

    constexpr LyricWiki () : GeneralPlugin (info, false) {}

    bool init ();
    void * get_gtk_widget ();
};

const char * const LyricWiki::defaults[] = {
    "search_internet", "TRUE",  // SEARCH FOR LYRICS FROM WEB (IF NOT FOUND LOCALLY)
    "sync_lyrics", "TRUE",      // ENABLE LYRIC SYNCHRONIZATION
    nullptr
};

const char LyricWiki::about[] =
    N_("Lyrics-display plugin for Fauxdacious\n"
    "\n"
    "from Audacious plugin by (c:2010): \n"
    "William Pitcock <nenolod@nenolod.net>\n"
    "\n"
    "Rewritten for Fauxdacious\n"
    "by (c:2020) Jim Turner <turnerjw784@yahoo.com>\n"
    "Scrolling lyrics by timestamp option\n"
    "by (c:2024) lecheel <lecheel@gmail.com>.");

EXPORT LyricWiki aud_plugin_instance;

const PreferencesWidget LyricWiki::widgets[] = {
    WidgetCheck (N_("Fetch lyrics from internet?"),
        WidgetBool ("lyricwiki", "search_internet")),
    WidgetCheck (N_("Enable scrolling lyric sync. by timestamps"),
        WidgetBool ("lyricwiki", "sync_lyrics")),
    WidgetCheck (N_("Cache (save) lyrics to disk?"),
        WidgetBool ("lyricwiki", "cache_lyrics")),
    WidgetCheck (N_("Try to save by song file-name first?"),
        WidgetBool ("lyricwiki", "save_by_songfile")),
    WidgetEntry (N_("Helper:"),
        WidgetString ("audacious", "lyric_helper"))
};

const PluginPreferences LyricWiki::prefs = {{widgets}};

static GtkTextView * textview;
static GtkTextBuffer * textbuffer;
static guint timer = 0;

static bool resetthreads = false;     // JWT:TRUE STOP ANY THREADS RUNNING ON SONG CHANGE OR SHUTDOWN.
static bool fromsongstartup = false;  // JWT:TRUE WHEN THREAD STARTED BY SONG CHANGE.
static LyricsState state;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static Index<String> extlist = str_list_to_index (".mp3,.ogg,.ogm,.oga,.flac,.fla,.wv", ",");

bool LyricWiki::init ()
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

                g_regex_unref (reg);
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
static void show_lyrics ();
static void lyricwiki_playback (bool force_refresh);
static void force_lyrics_refresh ();

// DEPRECIATED: static GtkWidget * edit_button;
static GtkWidget * refresh_button;
static GtkWidget * save_button;
static GtkWidget * tag_save_button;

static void save_lyrics_locally ();
static void save_lyrics_locally_fromscreen ();
gboolean update_lyrics_display (gpointer data);

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
        show_lyrics ();
// DEPRECIATED:         gtk_widget_set_sensitive (edit_button, true);
        return;
    }

    CharPtr lyrics = scrape_lyrics_from_lyricwiki_edit_page (buf.begin (), buf.len ());

    if (! lyrics)
    {
        update_lyrics (_("No lyrics Found"),
                (const char *) str_concat ({"Title: ", (const char *) state.title, "\nArtist: ",
                        (const char *) state.artist}),
                str_printf (_("Unable to parse(3) %s"), uri));
        show_lyrics ();
// DEPRECIATED:         gtk_widget_set_sensitive (edit_button, true);
        return;
    }

    update_lyrics (state.title, state.artist, lyrics);
    show_lyrics ();
    AUDINFO ("i:Lyrics came from old LyricWiki site!\n");
// DEPRECIATED:     gtk_widget_set_sensitive (edit_button, true);
    gtk_widget_set_sensitive (save_button, (timer == 0));
    if (aud_get_bool ("lyricwiki", "cache_lyrics"))
        save_lyrics_locally_fromscreen ();

    /* JWT:ALLOW 'EM TO EMBED IN TAG, IF POSSIBLE. */
    if (! strncmp ((const char *) state.filename, "file://", 7))
    {
        for (auto & ext : extlist)
        {
            if (str_has_suffix_nocase ((const char *) state.filename, (const char *) ext))
            {
                String error;
                VFSFile file (state.filename, "r");
                PluginHandle * decoder = aud_file_find_decoder (state.filename, true, file, & error);
                bool can_write = aud_file_can_write_tuple (state.filename, decoder);
                if (can_write)
                {
                    gtk_widget_set_sensitive (tag_save_button, true);
                    state.Wasok2saveTag = true;
                }
                break;
            }
        }
    }
}

/* CALLED BY g_idle_add() TO UPDATE LYRIC WIDGET FROM THREAD: */
static gboolean lyrics_ready (gpointer data)
{
    show_lyrics ();
    return false;
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
        show_lyrics ();
// DEPRECIATED:         gtk_widget_set_sensitive (edit_button, false);
        return;
    }

    String uri = scrape_uri_from_lyricwiki_search_result (buf.begin (), buf.len ());

    if (! uri)
    {
        update_lyrics (_("Error"), nullptr,
                str_printf (_("Unable to parse(2) %s"), uri1));
        show_lyrics ();
// DEPRECIATED:         gtk_widget_set_sensitive (edit_button, false);
        return;
    }
    else if (uri == String ("N/A"))
    {
        update_lyrics (state.title, state.artist, _("No lyrics available"));
        show_lyrics ();
// DEPRECIATED:         gtk_widget_set_sensitive (edit_button, false);
        return;
    }

    state.uri = uri;

    update_lyrics (state.title, state.artist, _("Looking for lyrics ..."));
    show_lyrics ();
// DEPRECIATED:     gtk_widget_set_sensitive (edit_button, true);
    vfs_async_file_get_contents (uri, get_lyrics_step_3, nullptr);
}

/* (SEPARATE THREAD) HANDLE FETCHING LYRICS FROM WEB VIA HELPER (1-STEP) OR fandom.com? (STEP 1 OF 3): */
/* NOTE:  WHEN STARTING PLAY, WE WAIT 2 SEC. B/C NORMALLY, A STREAM STILL HAS IT'S LAST-PLAYED
   TITLE-TUPLE AND, QUICKLY AFTER STARTING PLAY, A TUPLE-CHANGE WILL OCCUR BEFORE THE FIRST THREAD CAN
   LOOK UP THE "OLD" LYRICS (IF NOT CACHED), IF SO WE WANT TO ABORT THE FIRST THREAD SO THAT ONLY THE
   NEW THREAD WILL LOOK UP THE CURRENT LYRICS FOR THE NOW-CHANGED TUPLE (TITLE)!  WE STILL HAVE TO
   INITIATE A LOOKUP ON PLAY-START SINCE OTHERWISE FILES (WHICH HAVE NO TUPLE-CHANGES) WOULD NEVER
   HAVE THEIR LYRICS LOOKED UP!
*/
static void * lyric_helper_thread_fn (void * data)
{
    bool abortthisthread = resetthreads;
    if (abortthisthread)
    {
        pthread_exit (nullptr);
        return nullptr;
    }
    if (fromsongstartup)  // TRUE IF SONG-START, FALSE ON TUPLE-CHANGE!
    {
        if (! strcmp_nocase (state.filename, "https://", 8) || ! strcmp_nocase (state.filename, "http://", 7))
        {
            int sleep_msec = aud_get_int ("lyricwiki", "sleep_msec");
            if (sleep_msec < 1)  sleep_msec = 1600;
            g_usleep (sleep_msec * 1000);  // SLEEP 2" TO ALLOW FOR ANY IMMEDIATE TUPLE CHANGE TO OVERRIDE! */
            if (! fromsongstartup || resetthreads)  // CHGD. BY ANOTHER THREAD WHILST WE WERE SLEEPING!
            {
                /* ANOTHER THREAD HAS BEEN STARTED BY TUPLE-CHANGE, WHILE WE SLEPT, SO ABORT THIS
                   THREAD AND LET THE LATTER (TUPLE-CHANGE) THREAD UPDATE THE LYRICS!
                */
                pthread_exit (nullptr);
                return nullptr;
            }
        }
    }

    pthread_mutex_lock (& mutex);

    String lyric_helper = aud_get_str ("audacious", "lyric_helper");

    if (resetthreads)
        goto THREAD_EXIT;

    if (lyric_helper[0])  //JWT:WE HAVE A PERL HELPER, LESSEE IF IT CAN FIND/DOWNLOAD LYRICS FOR US:
    {
        bool lyrics_found = false;
        int helper_returncode = 0;
        GStatBuf statbuf;
        if (! state.album)
            state.album = String ("_");

        /* JWT:DON'T WANT HELPER WAITING FOR ALBUMART UNLESS ALBUMART ACTIVE AND NOT A USER-FORCED REFRESH: */
        const char * flags = (! state.force_refresh && aud_get_bool ("albumart", "_isactive")
                && aud_get_bool ("albumart", "internet_coverartlookup")) ? "ALBUMART" : "none";

        AUDINFO ("i:HELPER FOUND: WILL DO (%s)\n", (const char *) str_concat ({lyric_helper, " \"",
                (const char *) state.artist, "\" \"",
                (const char *) state.title, "\" ", aud_get_path (AudPath::UserDir),
                " \"", (const char *) state.album, "\" '", flags, "' "}));
#ifdef _WIN32
        WinExec ((const char *) str_concat ({lyric_helper, " \"", (const char *) state.artist, "\" \"",
                (const char *) state.title, "\" ", aud_get_path (AudPath::UserDir),
                " \"", (const char *) state.album, "\" '", flags, "' "}),
                SW_HIDE);
#else
        helper_returncode = system ((const char *) str_concat ({lyric_helper, " \"", (const char *) state.artist, "\" \"",
                (const char *) state.title, "\" ", aud_get_path (AudPath::UserDir),
                " \"", (const char *) state.album, "\" '", flags, "' "}));
#endif
        String lyric_fid = String (str_concat ({aud_get_path (AudPath::UserDir), "/_tmp_lyrics.txt"}));

        if (! resetthreads)
        {
// DEPRECIATED:             gtk_widget_set_sensitive (edit_button, false);  /* NO EDITING LYRICS ON HELPER-SERVED SITES! */
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
                        gtk_widget_set_sensitive (save_button, (timer == 0));
                        if (aud_get_bool ("lyricwiki", "cache_lyrics"))
                        {
                            save_lyrics_locally ();
                            if (aud_get_bool ("lyricwiki", "search_internet"))
                                gtk_widget_set_sensitive (refresh_button, true);
                        }
                        AUDINFO ("i:Lyrics came from HELPER!\n");
                        /* JWT:ALLOW 'EM TO EMBED IN TAG, IF POSSIBLE. */
                        if (! strncmp ((const char *) state.filename, "file://", 7))
                        {
                            for (auto & ext : extlist)
                            {
                                if (str_has_suffix_nocase ((const char *) state.filename, (const char *) ext))
                                {
                                    String error;
                                    VFSFile file (state.filename, "r");
                                    PluginHandle * decoder = aud_file_find_decoder (state.filename, true, file, & error);
                                    bool can_write = aud_file_can_write_tuple (state.filename, decoder);
                                    if (can_write)
                                    {
                                        gtk_widget_set_sensitive (tag_save_button, (timer == 0));
                                        state.Wasok2saveTag = true;
                                    }
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        }
        if (! lyrics_found && ! resetthreads)
        {
            /* HELPER RETURNS 4 (1024) IF WEB-SEARCH SKIPPED DUE TO USER-CONFIG. */
            update_lyrics (_("No lyrics Found"),
                    (const char *) str_concat ({"Title: ", (const char *) state.title, "\nArtist: ",
                    (const char *) state.artist}),
                    ((helper_returncode == 1024)
                            ? str_printf (_("Lyrics fetch skipped (helper config)."))
                            : str_printf (_("Unable to fetch lyrics."))));

            goto THREAD_EXIT;
        }
    }
    else /* DEPRECIATED:NO HELPER, TRY THE OLD SCHOOL "3-STEP C" WAY: (MAYBE fandom.com CAME BACK OR AUDACIOUS FIXED?) */
    {
        StringBuf title_buf = str_encode_percent (state.title);
        StringBuf artist_buf = str_encode_percent (state.artist);

        state.uri = String (str_printf ("https://lyrics.fandom.com/api.php?"
                "action=lyrics&artist=%s&song=%s&fmt=xml", (const char *) artist_buf,
                (const char *) title_buf));

        update_lyrics (state.title, state.artist, _("Connecting to lyrics.fandom.com ..."));
// DEPRECIATED:         gtk_widget_set_sensitive (edit_button, false);
        vfs_async_file_get_contents (state.uri, get_lyrics_step_2, nullptr);
    }

THREAD_EXIT:
    lyric_helper = String ();

    if (! abortthisthread && ! resetthreads)
        g_idle_add (lyrics_ready, nullptr);

    pthread_mutex_unlock (& mutex);

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
        show_lyrics ();
// DEPRECIATED:         gtk_widget_set_sensitive (edit_button, false);
        return;
    }

    StringBuf nullterminated_buf = str_copy (buf.begin (), buf.len ());
    update_lyrics (state.title, state.artist, (const char *) nullterminated_buf);
    show_lyrics ();

    /* JWT:(DEPRECIATED):  ALLOW 'EM TO EDIT LYRICWIKI, EVEN IF LYRICS ARE LOCAL, IF THEY HAVE */
    /* BOTH REQUIRED FIELDS:BUT ONLY IF USING OLD SITE (*NOT* USING THE PERL "HELPER")! */
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
// DEPRECIATED:         gtk_widget_set_sensitive (edit_button, true);
    }
// DEPRECIATED:     else
// DEPRECIATED:         gtk_widget_set_sensitive (edit_button, false);

    AUDINFO ("i:Lyrics came from local file: (%s)!\n", (const char *) state.local_filename);
    /* JWT:ALLOW 'EM TO EMBED IN TAG, IF POSSIBLE, EVEN IF LYRICS ARE FROM LOCAL FILE. */
    if (! strncmp ((const char *) state.filename, "file://", 7))
    {
        for (auto & ext : extlist)
        {
            if (str_has_suffix_nocase ((const char *) state.filename, (const char *) ext))
            {
                String error;
                VFSFile file (state.filename, "r");
                PluginHandle * decoder = aud_file_find_decoder (state.filename, true, file, & error);
                bool can_write = aud_file_can_write_tuple (state.filename, decoder);
                if (can_write)
                {
                    gtk_widget_set_sensitive (tag_save_button, (timer == 0));
                    state.Wasok2saveTag = true;
                }
                break;
            }
        }
    }
}

/* DEPRECIATED!:
static void launch_edit_page ()
{
    if (state.uri)
        gtk_show_uri (nullptr, state.uri, GDK_CURRENT_TIME, nullptr);
}
*/

/* CALLED WHEN USER SELECTS "SAVE LOCALLY" TO CACHE (BEFORE LYRICS ARE SHOWN ONSCREEN): */
static void save_lyrics_locally ()
{
    if (state.local_filename && state.local_filename[0] && textbuffer)
    {
        AUDINFO ("i:Caching lyrics locally to (%s)!\n", (const char *) state.local_filename);
        if (state.sholyrics && state.sholyrics[0])
        {
            const char * lyrics = (const char *) state.sholyrics;
            int sz = strlen (lyrics);
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
                if (file.fwrite (lyrics, 1, sz) == sz)
                {
                    AUDINFO ("i:Successfully cached %d bytes of lyrics locally to (%s).\n", sz,
                            (const char *) state.local_filename);

                    gtk_widget_set_sensitive (save_button, false);
                }
            }
        }
    }
}

/* CALLED WHEN USER SELECTS "SAVE LOCALLY" FROM MENU: */
static void save_lyrics_locally_fromscreen ()
{
    if (state.local_filename && state.local_filename[0] && textbuffer)
    {
        AUDINFO ("i:Saving lyrics locally to (%s)!\n", (const char *) state.local_filename);
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
                    if (file.fwrite (lyrics + state.startlyrics, 1, sz) == sz)
                    {
                        AUDINFO ("i:Successfully saved %d bytes of lyrics locally to (%s).\n", sz,
                                (const char *) state.local_filename);

                        gtk_widget_set_sensitive (save_button, false);
                    }
                }
                g_free (lyrics);
            }
        }
    }
}

/* CALLED WHEN USER SELECTS "Refresh" FROM MENU: */
static void force_lyrics_refresh ()
{
    fromsongstartup = false;
    lyricwiki_playback (true);
}

/* CALLED WHEN USER SELECTS "SAVE IN ID3 TAG/VORBIS COMMENT" FROM MENU (LOCAL SONG FILES ONLY): */
static void save_lyrics_in_embedded_tag ()
{
    if (textbuffer)
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
                String error;
                /* JWT:EMPTY TUPLE, FETCH "CURRENT"/LAST-PLAYING ENTRY'S TUPLE (SHOULD "ALWAYS" WORK): */
                Tuple tuple = aud_playlist_entry_get_tuple (state.current_playlist, aud_playlist_get_position (state.current_playlist));
                tuple.set_str (Tuple::Lyrics, String (str_copy (lyrics + state.startlyrics, sz)));
                g_free (lyrics);
                VFSFile file (state.filename, "r");
                PluginHandle * decoder = aud_file_find_decoder (state.filename, true, file, & error);
                bool success = aud_file_write_tuple (state.filename, decoder, tuple);
                if (success)
                    AUDINFO ("i:Successfully saved %d bytes of lyrics to embedded tag.\n", sz);
                else
                    AUDERR ("e:Could not save lyrics to embedded tag.\n");

                state.Wasok2saveTag = true;
                gtk_widget_set_sensitive (tag_save_button, false);
            }
        }
    }
}

/* CALLED WHEN USER CHANGES LYRICS TEXT IN WIDGET, MAKE [SAVE] OPTION VISIBLE: */
static void allow_usersave (GtkTextBuffer *textbuffer,
               char        * preedit,
               gpointer     user_data)
{
    if (! gtk_widget_get_sensitive (save_button))
        gtk_widget_set_sensitive (save_button, (timer == 0));

    gtk_widget_set_sensitive (tag_save_button, (state.Wasok2saveTag && timer == 0));
}

static GtkWidget * build_widget ()
{
    textview = (GtkTextView *) gtk_text_view_new ();
    gtk_text_view_set_editable (textview, true);
    gtk_text_view_set_cursor_visible (textview, true);
    gtk_text_view_set_left_margin (textview, 4);
    gtk_text_view_set_right_margin (textview, 4);
    gtk_text_view_set_wrap_mode (textview, GTK_WRAP_WORD);
    textbuffer = gtk_text_view_get_buffer (textview);

    GtkWidget * scrollview = gtk_scrolled_window_new (nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type ((GtkScrolledWindow *) scrollview, GTK_SHADOW_IN);
    gtk_scrolled_window_set_policy ((GtkScrolledWindow *) scrollview, GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    GtkWidget * vbox = audgui_vbox_new (6);

    gtk_container_add ((GtkContainer *) scrollview, (GtkWidget *) textview);
    gtk_box_pack_start ((GtkBox *) vbox, scrollview, true, true, 0);

    gtk_widget_show_all (vbox);

    gtk_text_buffer_create_tag (textbuffer, "weight_bold", "weight", PANGO_WEIGHT_BOLD, nullptr);
    gtk_text_buffer_create_tag (textbuffer, "size_x_large", "scale", PANGO_SCALE_X_LARGE, nullptr);
    gtk_text_buffer_create_tag (textbuffer, "style_italic", "style", PANGO_STYLE_ITALIC, nullptr);

    GtkWidget * hbox = audgui_hbox_new (6);
    gtk_box_pack_start ((GtkBox *) vbox, hbox, false, false, 0);

    // DEPRECIATED: edit_button = gtk_button_new_with_mnemonic (_("Edit Lyricwiki"));
    // DEPRECIATED: gtk_widget_set_sensitive (edit_button, false);
    // DEPRECIATED: gtk_box_pack_end ((GtkBox *) hbox, edit_button, false, false, 0);

    refresh_button = gtk_button_new_with_mnemonic (_("Refresh"));
    gtk_widget_set_sensitive (refresh_button, false);
    gtk_box_pack_end ((GtkBox *) hbox, refresh_button, false, false, 0);

#ifdef USE_GTK3
    save_button = gtk_button_new_with_mnemonic (_("Save"));
    tag_save_button = gtk_button_new_with_mnemonic (_("Embed"));
#else
    save_button = gtk_button_new_with_mnemonic (_("Save Locally"));
    tag_save_button = gtk_button_new_with_mnemonic (_("Save Embedded"));
#endif
    gtk_widget_set_sensitive (save_button, false);
    gtk_box_pack_end ((GtkBox *) hbox, save_button, false, false, 0);

    gtk_widget_set_sensitive (tag_save_button, false);
    state.Wasok2saveTag = false;

    gtk_box_pack_end ((GtkBox *) hbox, tag_save_button, false, false, 0);

// DEPRECIATED:     g_signal_connect (edit_button, "clicked", (GCallback) launch_edit_page, nullptr);
    g_signal_connect (refresh_button, "clicked", (GCallback) force_lyrics_refresh, nullptr);
    g_signal_connect (save_button, "clicked", (GCallback) save_lyrics_locally_fromscreen, nullptr);
    g_signal_connect (tag_save_button, "clicked", (GCallback) save_lyrics_in_embedded_tag, nullptr);
    g_signal_connect (textbuffer, "changed", (GCallback) allow_usersave, nullptr);

    return vbox;
}

/* CALLED BY BOTH MAIN AND THREAD TO UPDATE LYRICS (FOR LATER DISPLAYING IN THE WIDGET): */
static void update_lyrics (const char * title, const char * artist, const char * lyrics)
{
    state.shotitle = title ? String (title) : String ("");
    state.shoartist = artist ? String (artist) : String ("");
    state.sholyrics = lyrics ? String (lyrics) : String ("");
}

static void show_lyrics ()
{
    GtkTextIter iter, startlyrics;

    if (! textbuffer)
        return;

    bool ok2save_was = gtk_widget_get_sensitive (save_button);  // OK2SAVE GETS SET TO TRUE BY allow_usersave() CALLBACK IN HERE!:

    gtk_text_buffer_set_text (textbuffer, "", -1);
    gtk_text_buffer_get_start_iter (textbuffer, & iter);
    gtk_text_buffer_insert_with_tags_by_name (textbuffer, & iter, str_to_utf8 (state.shotitle, -1), -1,
     "weight_bold", "size_x_large", nullptr);

    if (state.shoartist)
    {
        gtk_text_buffer_insert (textbuffer, & iter, "\n", -1);
        gtk_text_buffer_insert_with_tags_by_name (textbuffer, & iter, str_to_utf8 (state.shoartist, -1), -1,
         "style_italic", nullptr);
    }

    gtk_text_buffer_insert (textbuffer, & iter, "\n\n", -1);
    gtk_text_buffer_get_end_iter (textbuffer, & startlyrics);
    state.startlyrics = gtk_text_iter_get_offset (& startlyrics);
    gtk_text_buffer_insert (textbuffer, & iter, str_to_utf8 (state.sholyrics, -1), -1);

    gtk_text_buffer_get_start_iter (textbuffer, & iter);
    gtk_text_view_scroll_to_iter (textview, & iter, 0, true, 0, 0);
    gtk_widget_set_sensitive (save_button, (ok2save_was && timer == 0));

    if (! state.synclyrics)
        return;

    bool have_timestamps = false;
    // Parse the lyrics and populate timed_lyrics
    timed_lyrics.clear ();
    std::istringstream iss (static_cast<std::string>(state.sholyrics)); // Assuming String can be cast to std::string
    std::string line;

    // Add a dummy timestamp line at the beginning to prevent title from being highlighted
    TimedLyricLine dummy_line;
    dummy_line.timestamp_ms = 0;
    dummy_line.text = String ("");
    timed_lyrics.push_back (dummy_line);

    while (std::getline (iss, line))
    {
        // Sanitize the line: remove leading/trailing spaces and carriage return
        line.erase (0, line.find_first_not_of (" \t\r"));  // Remove leading whitespace and \r
        line.erase (line.find_last_not_of (" \t\r") + 1);  // Remove trailing whitespace and \r

        // Updated regex to handle various whitespaces around the timestamp and lyric text
        std::regex re (R"(\[\s*(\d+)\s*:\s*(\d+\.\d{2,3})\s*\]\s*(.*))");
        std::smatch match;

        if (std::regex_match (line, match, re))
        {
            int minutes = std::stoi (match[1].str ());    // Convert minutes
            float seconds = std::stof (match[2].str ());  // Convert seconds
            int timestamp_ms = static_cast<int>((minutes * 60 + seconds) * 1000);

            have_timestamps = true;
            TimedLyricLine timed_line;
            timed_line.timestamp_ms = timestamp_ms;
            timed_line.text = String (match[3].str ().c_str ());  // Lyric text

            timed_lyrics.push_back (timed_line);
        }
    }
    if (! have_timestamps)
    {
        if (timer > 0)
        {
            g_source_remove (timer);  // Stop the sync timer
            timer = 0;
        }
        state.synclyrics = false;
    }
}

/* CALLED WHENEVER WE NEED LYRICS: */
static void lyricwiki_playback (bool force_refresh)
{
    /* FIXME: cancel previous VFS requests (not possible with current API) */

    bool found_lyricfile = false;
    GStatBuf statbuf;
    String lyricStr = String ("");
    StringBuf path = StringBuf ();

    state.filename = aud_drct_get_filename ();
    state.uri = String ();
// DEPRECIATED:     gtk_widget_set_sensitive (edit_button, false);
    gtk_widget_set_sensitive (refresh_button, false);
    state.local_filename = String ("");
    state.force_refresh = force_refresh;
    state.synclyrics = aud_get_bool ("lyricwiki", "sync_lyrics");

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
    state.current_playlist = aud_playlist_get_active ();
    state.title = tuple.get_str (Tuple::Title);
    state.artist = tuple.get_str (Tuple::Artist);
    state.album = tuple.get_str (Tuple::Album);
    gtk_widget_set_sensitive (save_button, false);
    gtk_widget_set_sensitive (tag_save_button, false);
    state.Wasok2saveTag = false;

    if (found_lyricfile)  // JWT:WE HAVE LYRICS STORED IN A LOCAL FILE MATCHING FILE NAME!:
    {
        AUDINFO ("i:Local lyric file found (%s).\n", (const char *) lyricStr);
        vfs_async_file_get_contents (lyricStr, get_lyrics_step_0, nullptr);
    }
    else  // NO LOCAL LYRICS FILE FOUND, SO CHECK FOR EMBEDDED LYRICS TAGS, THEN GLOBAL LYRIC FILE MATCHING ARTIST/TITLE:
    {
        bool need_lyrics = true;
        String lyricsFromTuple = tuple.get_str (Tuple::Lyrics);

        if (! force_refresh && lyricsFromTuple && lyricsFromTuple[0])
        {
            AUDDBG ("i:Lyrics found in embedded tag.\n");
            update_lyrics (state.title, state.artist, (const char *) lyricsFromTuple);
            show_lyrics ();
// DEPRECIATED:             gtk_widget_set_sensitive (edit_button, false);
            gtk_widget_set_sensitive (save_button, (timer == 0));
            state.Wasok2saveTag = true;
            AUDINFO ("i:Lyrics came from embedded tag!\n");
            need_lyrics = false;
            gtk_widget_set_sensitive (refresh_button, true);
        }
        if (state.title)
        {
            /* JWT:MANY STREAMS & SOME FILES FORMAT THE TITLE FIELD AS:
               "<artist> - <title> [<other-stuff>]".  IF SO, THEN PARSE OUT THE
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
                found_lyricfile = force_refresh ? false : ! (g_stat ((const char *) lyricStr, & statbuf));

                /* local_filename := (GLOBAL) ARTIST NAME/TITLE, IF NOT ALREADY SET *OR* save_by_songfile NOT SET. */
                if (! state.local_filename || ! state.local_filename[0] || ! save_by_songfile)
                    state.local_filename = lyricStr;  /* THE FILE NAME WE'LL CACHE OR "SAVE LOCALLY" TO. */

                if (need_lyrics && found_lyricfile)
                {
                    AUDINFO ("i:Global lyric file found by artist/title (%s).\n", (const char *) lyricStr);
                    vfs_async_file_get_contents (lyricStr, get_lyrics_step_0, nullptr);
                    lyricStr = String ();
                    if (aud_get_bool ("lyricwiki", "cache_lyrics")
                            && aud_get_bool ("lyricwiki", "search_internet"))
                        gtk_widget_set_sensitive (refresh_button, true);
                    if (save_by_songfile)
                        gtk_widget_set_sensitive (save_button, (timer == 0));

                    return;
                }
            }
        }
        /* IF HERE, NO LOCAL LYRICS FILE BY FILENAME OR TITLE, SEARCH LYRICWIKI: */
        if (need_lyrics)
        {
            AUDINFO ("i:No Local lyric file found, try fetching from lyricwiki...\n");
            update_lyrics ("Searching web for lyrics...", nullptr, nullptr);
            show_lyrics ();
            if (! state.artist || ! state.title)
            {
                update_lyrics (_("Error"), nullptr, _("Missing title and/or artist"));
                show_lyrics ();
// DEPRECIATED:                 gtk_widget_set_sensitive (edit_button, false);  /* NO EDITING LYRICS ON HELPER-SERVED SITES! */
                return;
            }
            if (! aud_get_bool ("lyricwiki", "search_internet"))
            {
                update_lyrics (_("No lyrics Found locally"),
                        (const char *) str_concat ({"Title: ", (const char *) state.title, "\nArtist: ",
                        (const char *) state.artist}),
                        str_printf (_("Unable to fetch lyrics (Fetch lyrics from internet option not enabled).")));
                show_lyrics ();
                return;
            }

            pthread_attr_t thread_attrs;
            if (! pthread_attr_init (& thread_attrs))
            {
                if (! pthread_attr_setdetachstate (& thread_attrs, PTHREAD_CREATE_DETACHED)
                        || ! pthread_attr_setscope (& thread_attrs, PTHREAD_SCOPE_SYSTEM))
                {
                    pthread_t lyric_helper_thread;

                    resetthreads = false;
                    if (pthread_create (&lyric_helper_thread, nullptr, lyric_helper_thread_fn, nullptr))
                        AUDERR ("s:Error creating helper thread: %s - Expect Delays!...\n", strerror (errno));
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
    if (state.synclyrics && timer == 0)  // Start the sync timer
        timer = g_timeout_add (200, update_lyrics_display, nullptr);  // Call every 200 ms

    gtk_widget_set_sensitive (save_button, (timer == 0));  // NO SAVING (PARTIAL) SYNCED LYRICS!

    lyricStr = String ();
}

/* CALLED WHEN PLAYBACK STARTS: */
static void lyricwiki_playback_began ()
{
    resetthreads = true;
    fromsongstartup = true;
    lyricwiki_playback (false);
}

/* CALLED WHEN TUPLE CHANGES (STREAMS CHANGE TITLE, ETC. WHILE PLAYING:  */
static void lyricwiki_playback_changed ()
{
    fromsongstartup = false;
    lyricwiki_playback (false);
}

/* CALLED WHEN PLAYBACK IS STOPPED, MAKE SURE NO DANGLING THREADS HAVE LOCAL EVENT LOOP RUNNING!: */
static void kill_thread_eventloop ()
{
    if (state.synclyrics && timer > 0)
    {
        g_source_remove (timer);  // Stop the sync timer
        timer = 0;
    }
    gtk_widget_set_sensitive (refresh_button, false);
    resetthreads = true;
}

/* CALLED ON SHUTDOWN TO CLEAN UP: */
static void destroy_cb ()
{
    kill_thread_eventloop ();

    hook_dissociate ("playback stop", (HookFunction) kill_thread_eventloop);
    hook_dissociate ("tuple change", (HookFunction) lyricwiki_playback_changed);
    hook_dissociate ("playback ready", (HookFunction) lyricwiki_playback_began);

    state.filename = String ();
    state.title = String ();
    state.artist = String ();
    state.uri = String ();
    state.local_filename = String ();
    if (timer > 0)
    {
        g_source_remove (timer);  // Stop the sync timer
        timer = 0;
    }
    textview = nullptr;
    textbuffer = nullptr;
    save_button = nullptr;
    tag_save_button = nullptr;
// DEPRECIATED:     edit_button = nullptr;
}

void highlight_lyrics(int current_time_ms) {
    if (!textbuffer)
        return;

    // Clear the text buffer
    gtk_text_buffer_set_text (textbuffer, "", -1);
    gtk_widget_set_sensitive (save_button, false);  // NO SAVING (PARTIAL) SYNCED LYRICS!

    // Find the 4 lines closest to the current timestamp (current + 3 neighbors)
    std::vector<TimedLyricLine> lines_to_display;

    // Store up to 4 lines starting from the current one
    for (size_t i = 0; i < timed_lyrics.size (); ++i)
    {
        if (timed_lyrics[i].timestamp_ms >= current_time_ms)
        {
            // Ensure we have the current line and its neighbors (3 more lines)
            size_t start_index = (i > 1) ? i - 2 : 0;  // Start 2 lines before the current line
            size_t end_index = std::min (i + 2, timed_lyrics.size () - 1);  // Limit to 3 more lines after

            // Ensure no more than 4 lines are selected
            size_t line_count = 0;

            for (size_t j = start_index; j <= end_index && line_count < 4; ++j)
            {
                lines_to_display.push_back (timed_lyrics[j]);
                line_count++;
            }
            break;
        }
    }

    // Retrieve the tag table
    GtkTextTagTable* tag_table = gtk_text_buffer_get_tag_table (textbuffer);

    // Check if the enlarge tag exists, and create it if not
    GtkTextTag* enlarge_tag = gtk_text_tag_table_lookup (tag_table, "enlarge_tag");
    if (!enlarge_tag) {
        enlarge_tag = gtk_text_tag_new ("enlarge_tag");
        g_object_set (enlarge_tag, "scale", 1.5, NULL); // Enlarge by 1.5 times (adjust as needed)
        gtk_text_tag_table_add (tag_table, enlarge_tag);
    }

    // Insert the selected lines into the text buffer
    GtkTextIter iter;
    gtk_text_buffer_get_start_iter (textbuffer, &iter);

    for (size_t i = 0; i < lines_to_display.size (); ++i) {
        const TimedLyricLine &line = lines_to_display[i];
        std::string text_with_newline = std::string (line.text);

        // Skip empty lines (like our dummy timestamp)
        if (text_with_newline.empty ())
            continue;

        // Apply the enlarge tag to the second line
        if (i == 1)  // Second line (index 1)
            gtk_text_buffer_insert_with_tags_by_name (textbuffer, &iter,
                    text_with_newline.c_str (), -1, "enlarge_tag", NULL);
        else
            gtk_text_buffer_insert(textbuffer, &iter, text_with_newline.c_str (), -1);

        gtk_text_buffer_insert (textbuffer, &iter, "\n", -1);
    }

    // After inserting lines, force scroll to the last line
    GtkTextIter end_iter;
    gtk_text_buffer_get_end_iter (textbuffer, &end_iter);
    gtk_text_view_scroll_to_iter (textview, &end_iter, 0, TRUE, 0, 0);
}

gboolean update_lyrics_display (gpointer data)
{
    int current_time_ms = aud_drct_get_time ();  // Get current time from player in ms
    highlight_lyrics (current_time_ms);

    return G_SOURCE_CONTINUE;  // Continue calling this function
}

/* CALLED ON STARTUP (WIDGET CREATION): */
void * LyricWiki::get_gtk_widget ()
{
    GtkWidget * vbox = build_widget ();

    hook_associate ("playback ready", (HookFunction) lyricwiki_playback_began, nullptr);
    hook_associate ("tuple change", (HookFunction) lyricwiki_playback_changed, nullptr);
    hook_associate ("playback stop", (HookFunction) kill_thread_eventloop, nullptr);

    g_signal_connect (vbox, "destroy", destroy_cb, nullptr);

    if (aud_drct_get_ready ())
        lyricwiki_playback_began ();

    return vbox;
}
