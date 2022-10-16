/*
 * albumart.cc
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

#include <QApplication>
#include <QLabel>
#include <QPixmap>
#include <QThread>
#include <QEvent>

#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <utime.h>

#ifdef _WIN32
#include <winbase.h>
#endif

#include <libfauxdcore/preferences.h>
#include <libfauxdcore/drct.h>
#include <libfauxdcore/i18n.h>
#include <libfauxdcore/plugin.h>
#include <libfauxdcore/hook.h>
#include <libfauxdcore/audstrings.h>
#include <libfauxdcore/runtime.h>

#include <libfauxdqt/libfauxdqt.h>

static bool fromsongstartup = false;  // TRUE WHEN THREAD STARTED BY SONG CHANGE (album_init()).
static bool skipArtReInit = false;    // JWT:TRUE:SKIP RESETTING ART (ALREADY RESET BY THREAD NOW SLEEPING).
static bool resetthreads = false;     // JWT:TRUE STOP ANY THREADS RUNNING ON SONG CHANGE OR SHUTDOWN.
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static int customType = QEvent::registerEventType();
static bool hide_dup_art_icon;   /* JWT:TOGGLE TO TRUE TO HIDE (DUPLICATE) ART ICON IN INFOBAR IF A WEB IMAGE FETCHED. */
static bool last_image_from_web; /* JWT:TRUE IF LAST IMAGE CAME FROM WEB ("Look for album art on the web" OPTION). */

/* JWT:SEPARATE THREAD TO CALL THE HELPER SO THAT THE "LONG" TIME IT TAKES DOESN'T FREEZE THE GUI
   DISPLAY WHILE ATTEMPTING TO FIND AND FETCH THE ALBUM-ART.  THIS THREAD MUST *NOT* CALL THE
   ART FUNCTIONS THOUGH - CAUSES GUI ISSUES!  WHEN STARTING A NEW SONG/STREAM, WE WAIT FOR 2
   SECONDS BEFORE FETCHING IMAGE TO ALLOW THE TUPLE TO CHANGE (IE. RESTARTING A STREAMING STATION
   LATER USUALLY MEANS A DIFFERENT SONG TITLE), OTHERWISE, WE'D CALL THE THREAD TWICE, ONE FOR THE
   PREV. SONG TITLE STILL DISPLAYED, THEN AGAIN WHEN THE TUPLE CHANGES (USUALLY, ALMOST IMMEDIATELY)!
*/

class AlbumArtQt : public GeneralPlugin {
public:
    static const char * const defaults[];
    static const PreferencesWidget widgets[];
    static const PluginPreferences prefs;
    static constexpr PluginInfo info = {
        N_("Album Art"),
        PACKAGE,
        nullptr, // about
        & prefs,
        PluginQtOnly
    };

    constexpr AlbumArtQt () : GeneralPlugin (info, false) {}

    bool init ();
    void * get_qt_widget ();
};

#define MARGIN 4

class ArtLabel : public QLabel {
public:
#if QT_VERSION >= QT_VERSION_CHECK(5, 6, 0)
    ArtLabel (QWidget * parent = nullptr, Qt::WindowFlags f = Qt::WindowFlags ()) : QLabel (parent, f)
#else
    ArtLabel (QWidget * parent = nullptr, Qt::WindowFlags f = 0) : QLabel (parent, f)
#endif
    {
        init ();
    }

#if QT_VERSION >= QT_VERSION_CHECK(5, 6, 0)
    ArtLabel (const QString & text, QWidget * parent = nullptr, Qt::WindowFlags f = Qt::WindowFlags ()) : QLabel (text, parent, f)
#else
    ArtLabel (const QString & text, QWidget * parent = nullptr, Qt::WindowFlags f = 0) : QLabel (text, parent, f)
#endif
    {
        init ();
    }

    bool event (QEvent*);

    /* JWT:CALLED BY ALBUMART FETCHING THREAD WHEN IT HAS FINISHED SEARCHING/FETCHING ALBUM-COVER IMAGE */
    void ready_art ()
    {
        String coverart_file;
        Index<String> extlist = str_list_to_index ("jpg,png,gif,jpeg,webp", ",");

        for (auto & ext : extlist)
        {
            coverart_file = String (str_concat ({aud_get_path (AudPath::UserDir), "/_tmp_albumart.", (const char *) ext}));
            const char * filenamechar = coverart_file;
            struct stat statbuf;
            if (stat (filenamechar, &statbuf) >= 0)  // ART IMAGE FILE EXISTS:
            {
                coverart_file = String (filename_to_uri (filenamechar));
                origPixmap = QPixmap (audqt::art_request ((const char *) coverart_file, 0, 0));
                origSize = origPixmap.size ();
                if (origSize.width () > 10)
                {
                    drawArt ();

                    /* INFOBAR ICON WAS HIDDEN BY HIDE DUP. OPTION, SO TOGGLE IT BACK OFF ("SHOW" IN INFOBAR): */
                    aud_set_bool ("albumart", "_infoarea_hide_art", false);
                    hook_call ("qtui toggle infoarea_art", nullptr);
                    last_image_from_web = true;

                    return;
                }
                else
                    break;
            }
        }
        return;
    }

    /* JWT:UPDATE THE ALBUM-COVER IMAGE (CALL THREAD IF DYNAMIC ALBUM-ART OPTION IN EFFECT): */
    void update_art ()
    {
        bool haveartalready = false;
        bool have_channel_art = aud_get_bool ("albumart", "_have_channel_art");
        bool skip_web_art_search = aud_get_bool (nullptr, "_skip_web_art_search");
        String filename = aud_drct_get_filename ();

        if (skip_web_art_search)
        aud_set_bool (nullptr, "_skip_web_art_search", false);

        if (skipArtReInit)
            skipArtReInit = false;
        else
        {
            origPixmap = QPixmap (audqt::art_request_current (0, 0));
            if (origPixmap.isNull ())
            {
                bool have_dir_icon_art = false;
                if (! strncmp (filename, "file://", 7)
                        && aud_get_bool ("albumart", "seek_directory_channel_art"))
                {
                    /* FOR LOCAL FILES W/O CHANNEL ART, LOOK FOR A DIRECTORY CHANNEL ART ICON FILE: */
                    String dir_channel_icon = aud_get_str ("albumart", "directory_channel_art");
                    if (dir_channel_icon && dir_channel_icon[0])
                    {
                        struct stat statbuf;
                        StringBuf icon_path = str_concat ({filename_get_parent (uri_to_filename (filename)), "/"});
                        StringBuf icon_fid = str_concat ({icon_path, dir_channel_icon});
                        const char * filename;
                        const char * ext;
                        int isub_p;
                        uri_parse (icon_fid, & filename, & ext, nullptr, & isub_p);
                        if (! ext || ! ext[0])
                        {
                            Index<String> extlist = str_list_to_index ("jpg,png,jpeg", ",");
                            for (auto & ext : extlist)
                            {
                                dir_channel_icon = String (str_concat ({icon_fid, ".", (const char *) ext}));
                                struct stat statbuf;
                                if (stat ((const char *) dir_channel_icon, &statbuf) < 0)  // ART IMAGE FILE DOESN'T EXIST:
                                    dir_channel_icon = String ("");
                                else
                                    break;
                            }
                        }
                        else
                            dir_channel_icon = String (icon_fid);

                        if (dir_channel_icon && dir_channel_icon[0] && stat ((const char *) dir_channel_icon, & statbuf) == 0)
                        {
                            origPixmap = QPixmap (audqt::art_request ((const char *) dir_channel_icon, 0, 0));
                            if (! origPixmap.isNull ())
                            {
                                have_dir_icon_art = true;
                                have_channel_art = false;
                            }
                        }
                    }
                }
                if (! have_dir_icon_art)
                    origPixmap = QPixmap (audqt::art_request_fallback (0, 0));
            }
            else
                haveartalready = true;

            origSize = origPixmap.size ();
            drawArt ();
        }

        if (aud_get_bool ("albumart", "hide_dup_art_icon"))
            aud_set_bool ("albumart", "_infoarea_hide_art", ! have_channel_art);

        last_image_from_web = false;
        if (haveartalready)  /* JWT:IF SONG IS A FILE & ALREADY HAVE ART IMAGE, SKIP INTERNET ART SEARCH! */
        {
            if (! strncmp (filename, "file://", 7)
                    || (! strncmp (filename, "cdda://", 7) && ! aud_get_bool ("CDDA", "seek_albumart_for_cds"))
                    || (! strncmp (filename, "dvd://", 6) && aud_get_bool ("dvd", "skip_coverartlookup")))
                return;
        }

        /* JWT:NOW CHECK THE ALBUM-ART CACHE: */
        Tuple tuple = aud_drct_get_tuple ();
        String Title = tuple.get_str (Tuple::Title);
        String Artist = tuple.get_str (Tuple::Artist);
        String Album = tuple.get_str (Tuple::Album);
        String audio_fn = tuple.get_str (Tuple::AudioFile);
        if (! audio_fn || ! audio_fn[0])
            audio_fn = aud_drct_get_filename ();

        const char * album = (const char *) Album;

        if (Title && Title[0])
        {
            bool split_titles = aud_get_bool (nullptr, "split_titles");
            bool skipweb = false;
            if (album && album[0])  // ALBUM FIELD NOT BLANK AND NOT A FILE/URL:
            {
                const char * album_uri = strstr (album, "://");  // FOR URI, WE'LL ASSUME LONGEST IS "stdin" (5 chars)
                if (album_uri && (album_uri-album) < 6)  // ALBUM FIELD IS A URI (PBLY A PODCAST/VIDEO FROM STREAMFINDER!):
                {
                    Album = String ("_");
                    String s = aud_get_str (nullptr, "_cover_art_link");
                    if (! s || ! s[0])
                        skipweb = true;
                }
                else if (split_titles)
                {
                    /* ALBUM MAY ALSO CONTAIN THE STREAM NAME (IE. "<ALBUM> - <STREAM NAME>"): STRIP THAT OFF: */
                    const char * throwaway = strstr (album, " - ");
                    int albumlen = throwaway ? throwaway - album : -1;
                    Album = String (str_copy (album, albumlen));
                }
            }
            else
                Album = String ("_");

            if (! split_titles)
            {
                /* ARTIST MAY BE IN TITLE INSTEAD (IE. "<ARTIST> - <TITLE>"): IF SO, USE THAT FOR ARTIST: */
                const char * title = (const char *) Title;
                if (title)
                {
                    const char * artistlen = strstr (title, " - ");
                    if (artistlen)
                    {
                        Artist = String (str_copy (title, artistlen - title));
                        const char * titleoffset = artistlen+3;
                        if (titleoffset)
                            Title = String (str_copy (artistlen+3, -1));
                    }
                }
            }

            StringBuf albart_FN;
            StringBuf album_buf = str_copy (Album);
            str_replace_char (album_buf, ' ', '~');  // JWT:PROTECT SPACES, WHICH I STUPIDLY DIDN'T ENCODE IN ALBUMART!
            if (Artist && Artist[0])
            {
                StringBuf artist_buf = str_copy (Artist);
                str_replace_char (artist_buf, ' ', '~');
                albart_FN = str_concat ({(const char *) str_encode_percent (album_buf), "__",
                        (const char *) str_encode_percent (artist_buf)});
            }
            else
            {
                if (Album == String ("_"))
                    return;   /* JWT:NO ALBUM OR ARTIST, PUNT! */
                else if (Title && Title[0])
                {
                    StringBuf title_buf = str_copy (Title);
                    str_replace_char (title_buf, ' ', '~');
                    albart_FN = str_concat ({(const char *) str_encode_percent (album_buf), "__",
                            (const char *) str_encode_percent (title_buf)});
                }
                else
                    albart_FN = str_encode_percent (album_buf);
            }
            str_replace_char (albart_FN, '~', ' ');  // JWT:UNPROTECT SPACES!

            String coverart_file;
            Index<String> extlist = str_list_to_index ("jpg,png,gif,jpeg", ",");
            for (auto & ext : extlist)
            {
                coverart_file = String (str_concat ({aud_get_path (AudPath::UserDir),
                        "/albumart/", (const char *) albart_FN, ".", (const char *) ext}));
                const char * filenamechar = coverart_file;
                struct stat statbuf;
                if (stat (filenamechar, &statbuf) >= 0)  // ART IMAGE FILE EXISTS:
                {
                    String coverart_uri = String (filename_to_uri (filenamechar));
                    origPixmap = QPixmap (audqt::art_request ((const char *) coverart_uri, 0, 0));
                    origSize = origPixmap.size ();
                    if (origSize.width () > 10)  /* FOUND ART IN CACHE, RETURN. */
                    {
                        drawArt ();
                        /* MAKE FILE NEWEST FOR EASIER USER-LOOKUP IN SONG-EDIT!: */
                        if (utime (filenamechar, nullptr) < 0)
                            AUDWARN ("i:Failed to update art-file time (for easier user-lookup)!");

                        /* INFOBAR ICON WAS HIDDEN BY HIDE DUP. OPTION, SO TOGGLE IT BACK OFF ("SHOW" IN INFOBAR): */
                        aud_set_bool ("albumart", "_infoarea_hide_art", false);
                        hook_call ("qtui toggle infoarea_art", nullptr);
                        last_image_from_web = true;

                        return;
                    }
                    else
                        break;
                }
            }

            /* JWT:NO CACHED ART, CALL HELPER: */
            if (! skip_web_art_search && ! skipweb && aud_get_bool ("albumart", "internet_coverartlookup")
                    && aud_get_str ("audacious", "cover_helper"))
            {
                pthread_attr_t thread_attrs;
                if (! pthread_attr_init (& thread_attrs))
                {
                    if (! pthread_attr_setdetachstate (& thread_attrs, PTHREAD_CREATE_DETACHED)
                            || ! pthread_attr_setscope (& thread_attrs, PTHREAD_SCOPE_PROCESS))
                    {
                        pthread_t album_helper_thread;

                        resetthreads = false;
                        if (pthread_create (&album_helper_thread, nullptr, album_helper_thread_fn, this))
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
    }

    void init_update_art ()
    {
        resetthreads = true;
        fromsongstartup = true;
        update_art ();  // JWT:CHECK FILES & DISKS (TUPLE DOESN'T CHANGE IN THESE) ONCE NOW ON PLAY START!
    }

    void clear ()
    {
        QLabel::clear ();
        origPixmap = QPixmap ();
    }

protected:
    virtual void resizeEvent (QResizeEvent * event)
    {
        QLabel::resizeEvent (event);

#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
        QPixmap pm = pixmap (Qt::ReturnByValue);
        if (! origPixmap.isNull () && ! pm.isNull () &&
                (size ().width () <= origSize.width () + MARGIN ||
                 size ().height () <= origSize.height () + MARGIN ||
                 pm.size ().width () != origSize.width () ||
                 pm.size ().height () != origSize.height ()))
#else
        const QPixmap * pm = pixmap ();
        if (! origPixmap.isNull () && pm && ! pm->isNull () &&
                (size ().width () <= origSize.width () + MARGIN ||
                 size ().height () <= origSize.height () + MARGIN ||
                 pm->size ().width () != origSize.width () ||
                 pm->size ().height () != origSize.height ()))
#endif
            drawArt ();
    }

private:
    QPixmap origPixmap;
    QSize origSize;

    void init ()
    {
        clear ();
        setMinimumSize (MARGIN + 1, MARGIN + 1);
        setAlignment (Qt::AlignCenter);
    }

    void drawArt ()
    {
        if (! aud_get_bool ("albumart", "scale_to_fill")
                && origSize.width () <= size ().width () - MARGIN
                && origSize.height () <= size ().height () - MARGIN)
            setPixmap (origPixmap);
        else
            setPixmap (origPixmap.scaled (size ().width () - MARGIN, size ().height () - MARGIN,
                        Qt::KeepAspectRatio, Qt::SmoothTransformation));

#ifdef Q_OS_MAC
        repaint ();
#endif
    }

    static void * album_helper_thread_fn (void * data)
    {
        bool abortthisthread = resetthreads;
        if (abortthisthread)
        {
            pthread_exit (nullptr);
            return nullptr;
        }
        if (fromsongstartup)
        {
            String filename = aud_drct_get_filename ();
            if (! strcmp_nocase (filename, "https://", 8) || ! strcmp_nocase (filename, "http://", 7))
            {
                int sleep_msec = aud_get_int ("albumart", "sleep_msec");
                if (sleep_msec < 1)  sleep_msec = 1500;
                skipArtReInit = true;
                QThread::usleep (sleep_msec * 1000);  // SLEEP 2" TO ALLOW FOR ANY TUPLE CHANGE TO OVERRIDE! */
                if (! fromsongstartup || resetthreads)
                {
                    /* ANOTHER THREAD HAS BEEN STARTED BY TUPLE-CHANGE, WHILE WE SLEPT, SO ABORT THIS
                       THREAD AND LET THE LATTER (TUPLE-CHANGE) THREAD UPDATE THE LYRICS!
                    */
                    filename = String ();
                    pthread_exit (nullptr);
                    return nullptr;
                }
            }
        }

        pthread_mutex_lock (& mutex);

        String cover_helper = aud_get_str ("audacious", "cover_helper");
        if (! resetthreads && cover_helper && cover_helper[0]) //JWT:WE HAVE A PERL HELPER TO LOOK UP COVER ART.
        {
            Tuple tuple = aud_drct_get_tuple ();
            String Title = tuple.get_str (Tuple::Title);
            String Artist = tuple.get_str (Tuple::Artist);
            String Album = tuple.get_str (Tuple::Album);
            String audio_fn = tuple.get_str (Tuple::AudioFile);
            if (! audio_fn || ! audio_fn[0])
                audio_fn = aud_drct_get_filename ();

            const char * album = (const char *) Album;
            ((ArtLabel *) data)->origPixmap = QPixmap ();

            if (Title && Title[0])
            {
                if (album && album[0])  // ALBUM FIELD NOT BLANK AND NOT A FILE/URL:
                {
                    const char * album_uri = strstr (album, "://");  // FOR URI, WE'LL ASSUME LONGEST IS "stdin" (5 chars)
                    if (album_uri && (album_uri-album) < 6)  // ALBUM FIELD IS A URI (PBLY A PODCAST/VIDEO FROM STREAMFINDER!):
                        Album = String ("_");
                    else if (aud_get_bool (nullptr, "split_titles"))
                    {
                        /* ALBUM MAY ALSO CONTAIN THE STREAM NAME (IE. "<ALBUM> - <STREAM NAME>"): STRIP THAT OFF: */
                        const char * throwaway = strstr (album, " - ");
                        int albumlen = throwaway ? throwaway - album : -1;
                        Album = String (str_copy (album, albumlen));
                    }
                }
                else
                    Album = String ("_");

                const char * webfetch = (! strncmp (audio_fn, "file://", 7)
                                && aud_get_bool ("albumart", "save_by_songfile"))
                                ? audio_fn : aud_get_str (nullptr, "_cover_art_link");

                if (! aud_get_bool (nullptr, "split_titles"))
                {
                    /* ARTIST MAY BE IN TITLE INSTEAD (IE. "<ARTIST> - <TITLE>"): IF SO, USE THAT FOR ARTIST: */
                    const char * title = (const char *) Title;
                    if (title)
                    {
                        const char * artistlen = strstr (title, " - ");
                        if (artistlen)
                        {
                            Artist = String (str_copy (title, artistlen - title));
                            const char * titleoffset = artistlen+3;
                            if (titleoffset)
                                Title = String (str_copy (artistlen+3, -1));
                        }
                    }
                }
                if (!Artist || !Artist[0])
                    Artist = String ("_");

                StringBuf album_buf = str_encode_percent (Album);
                StringBuf artist_buf = str_encode_percent (Artist);
                StringBuf title_buf = str_encode_percent (Title);

#ifdef _WIN32
                WinExec ((const char *) str_concat ({cover_helper, " ALBUM '",
                        (const char *) album_buf, "' ", aud_get_path (AudPath::UserDir), " '",
                        (const char *) artist_buf, "' '", (const char *) title_buf, "' ",
                        webfetch}), SW_HIDE);
#else
                system ((const char *) str_concat ({cover_helper, " ALBUM '",
                        (const char *) album_buf, "' ", aud_get_path (AudPath::UserDir), " '",
                        (const char *) artist_buf, "' '", (const char *) title_buf, "' ",
                        webfetch}));
#endif
            }
        }
        else
            abortthisthread = true;

        cover_helper = String ();

        if (! abortthisthread && ! resetthreads)
        {
            skipArtReInit = false;
            QApplication::postEvent((ArtLabel *) data, new QEvent((QEvent::Type) customType));
        }

        pthread_mutex_unlock (& mutex);

        pthread_exit (nullptr);

        return nullptr;
    }
};

#undef MARGIN

/* JWT:EMULATE g_idle_add() IN Qt - FROM:  https://stackoverflow.com/questions/30031230/qt-equivalent-to-gobject-idle-add */
bool ArtLabel::event(QEvent* event) {
    if (event->type() == (QEvent::Type) customType) {
        // Do whatever your idle callback would do
        // Optional: Post event again to self if this should be repeated
        //           implementing behaviour of returning TRUE from g_idle_add callback
        ready_art ();

        return true;
    }

    // Else default handler, may want to replace QWidget with actual base of "MyWidget"
    return QLabel::event(event);
}

/* JWT:CALLED WHEN SONG ENTRY CHANGES: */
static void init_update (void *, ArtLabel * widget)
{
    aud_set_str (nullptr, "_cover_art_link", "");  // JWT:MAKE SURE THIS IS CLEARED, AS THREAD DOESN'T ALWAYS SEEM TO DO SO?!:
    widget->init_update_art ();
}

static void tuple_update (void *, ArtLabel * widget)
{
    fromsongstartup = false;
    widget->update_art ();
}

/* JWT:CALLED WHEN PLAY IS STOPPED (BUT NOT WHEN JUMPING BETWEEN ENTRIES: */
static void clear (void *, ArtLabel * widget)
{
    resetthreads = true;
    widget->clear ();
}

/* JWT:CALLED WHEN USER TOOGLES THE hide_dup_art_icon CHECKBOX: */
/* IF ON, WE HIDE THE "DUPLICATE" IMG. IN INFOBAR, UNLESS WE FETCHED AN IMG. FROM THE WEB! */
/* (THIS OPTION HAS NO EFFECT UNLESS BOTH THE "VIEW - SHOW INFOBAR ALBUM ART" -AND THE - */
/* THE PLUGIN'S "LOOK FOR ALBUM ART ON THE WEB" OPTIONS ARE BOTH ON)! */
static void hide_dup_art_icon_toggle_fn ()
{
    aud_set_bool ("albumart", "hide_dup_art_icon", hide_dup_art_icon);
}

static void widget_cleanup (QObject * widget)
{
    resetthreads = true;
    aud_set_bool ("albumart", "_isactive", false);
    hook_dissociate ("playback stop", (HookFunction) clear, widget);
    hook_dissociate ("tuple change", (HookFunction) tuple_update, widget);
    hook_dissociate ("playback ready", (HookFunction) init_update, widget);
}

void * AlbumArtQt::get_qt_widget ()
{
    ArtLabel * widget = new ArtLabel;

    QObject::connect (widget, &QObject::destroyed, widget_cleanup);

    hook_associate ("playback ready", (HookFunction) init_update, widget);
    hook_associate ("tuple change", (HookFunction) tuple_update, widget);
    hook_associate ("playback stop", (HookFunction) clear, widget);

    aud_set_bool ("albumart", "_isactive", true);

    if (aud_drct_get_ready ())
        widget->update_art ();

    return widget;
}

EXPORT AlbumArtQt aud_plugin_instance;

const char * const AlbumArtQt::defaults[] = {
    "internet_coverartlookup", "FALSE",
    "scale_to_fill", "FALSE",
    "save_by_songfile", "FALSE",
    nullptr
};

bool AlbumArtQt::init ()
{
    aud_config_set_defaults ("albumart", defaults);
    hide_dup_art_icon = aud_get_bool ("albumart", "hide_dup_art_icon");
    return true;
}

const PreferencesWidget AlbumArtQt::widgets[] = {
    WidgetLabel(N_("<b>Albumart Configuration</b>")),
    WidgetCheck (N_("Look for album art on the web."),
        WidgetBool ("albumart", "internet_coverartlookup")),
    WidgetCheck (N_("Hide info bar art icon unless separate album cover fetched."),
        WidgetBool (hide_dup_art_icon, hide_dup_art_icon_toggle_fn)),
    WidgetCheck (N_("Scale small images to fill."),
        WidgetBool ("albumart", "scale_to_fill")),
    WidgetCheck (N_("Try to save by song file-name first?"),
        WidgetBool ("albumart", "save_by_songfile")),
};

const PluginPreferences AlbumArtQt::prefs = {{widgets}};
