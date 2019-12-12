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

#include <QLabel>
#include <QPixmap>

#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* JWT:IT'S POSSIBLE FOR A 2ND INSTANCE OF THE HELPER THREAD TO BE SPAWNED AND COMPLETE AND SUCCEED
    BEFORE THE FIRST ONE, IF THIS HAPPENS, DON'T LET THE FIRST ONE (FINISHING LATER) OVERWRITE THE
    IMAGE RETURNED BY THE 2ND ONE, AS THE 1ST ONE IS LIKELY FOR THE STATION'S *PREVIOUS* SONG TUPLE!
    * THE CASE THAT USUALLY CAUSES THIS IS WHEN THERE WAS NO IMAGE FOR THE PREV. SONG TUPLE (WHICH
    REMAINS DISPLAYED UNTIL YOU RESTART PLAY ON THE STATION LATER), SINCE STARTING PLAY WILL FORCE A
    FETCH FOR *THAT* TITLE (FOR WHICH THERE WAS NO IMAGE FOUND, INITIATING A FULL SEARCH (SLOW), AND,
    WHILST SEARCHING, THE NEW TITLE TUPLE ARRIVES AND INITIATES A SEARCH (A 2ND THREAD) FOR THAT AND
    IT HAPPENS TO ALREADY BE ON DISK (FAST), SO WE WANT TO KEEP THAT (GOOD) RESULT AND *NOT* LET THE
    FIRST SEARCH (WHICH WILL LIKELY *STILL* FAIL AND RETURN LATER) TO THEN RESET THE IMAGE TO THE
    DEFAULT!  NOTE:  IF WE DIDN'T INITIATE THE FIRST SEARCH (START OF PLAY) (album_init), BUT ONLY
    ON TUPLE-CHANGE, WE WOULD NEVER GET ART FOR THE FIRST SONG STREAMING WHEN STARTING PLAY!
    THIS VARIABLE SHOULD PREVENT THIS! (STARTING A NEW UPDATE RESETS IT): */
static bool album_art_found;

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
    ArtLabel (QWidget * parent = 0, Qt::WindowFlags f = 0) : QLabel(parent, f)
    {
        init ();
    }

    ArtLabel (const QString & text, QWidget * parent = 0, Qt::WindowFlags f = 0) : QLabel (text, parent, f)
    {
        init ();
    }

    void update_art ()
    {
        pthread_t helper_thread;
        album_art_found = false;
        if (pthread_create (& helper_thread, nullptr, helper_thread_fn, this))
        {
            AUDERR ("s:Error creating helper thread: %s - Expect Delay!...\n", strerror(errno));
            helper_thread_fn (this);
        }
        else if (pthread_detach (helper_thread))
            AUDERR ("s:Error detaching helper thread: %s!\n", strerror(errno));
    }

    void init_update_art ()
    {
        if (aud_get_bool ("albumart", "internet_coverartlookup"))
            update_art ();  // JWT:CHECK FILES & DISKS (TUPLE DOESN'T CHANGE IN THESE) ONCE NOW ON PLAY START!
        else
        {
            origPixmap = QPixmap (audqt::art_request_current (0, 0));

            origSize = origPixmap.size ();
            drawArt ();
        }
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
        const QPixmap * pm = pixmap ();

        if ( ! origPixmap.isNull () && pm && ! pm->isNull () &&
                (size ().width () <= origSize.width () + MARGIN ||
                 size ().height () <= origSize.height () + MARGIN ||
                 pm->size ().width () != origSize.width () ||
                 pm->size ().height () != origSize.height ()))
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
        if (origSize.width () <= size ().width () - MARGIN &&
            origSize.height () <= size ().height() - MARGIN)
            setPixmap (origPixmap);
        else
            setPixmap (origPixmap.scaled (size ().width () - MARGIN, size ().height () - MARGIN,
                        Qt::KeepAspectRatio, Qt::SmoothTransformation));

#ifdef Q_OS_MAC
	    repaint();
#endif
    }

    static void * helper_thread_fn (void * data)
    {
        String cover_helper = aud_get_str ("audacious", "cover_helper");
        ((ArtLabel *) data)->origPixmap = QPixmap ();
        if (cover_helper && cover_helper[0]
                && aud_get_bool ("albumart", "internet_coverartlookup")) //JWT:WE HAVE A PERL HELPER TO LOOK UP COVER ART.
        {
            Tuple tuple = aud_drct_get_tuple ();
            String Title = tuple.get_str (Tuple::Title);
            String Artist = tuple.get_str (Tuple::Artist);
            String Album = tuple.get_str (Tuple::Album);
            const char * album = (const char *) Album;
            if (Title && Title[0])
            {
                if (album && ! strstr (album, "://"))  // ALBUM FIELD NOT BLANK AND NOT A FILE/URL:
                {
                    if (aud_get_bool (nullptr, "split_titles"))
                    {
                        /* ALBUM MAY ALSO CONTAIN THE STREAM NAME (IE. "<ALBUM> - <STREAM NAME>"): STRIP THAT OFF: */
                        const char * throwaway = strstr (album, " - ");
                        int albumlen = throwaway ? throwaway - album : -1;
                        Album = String (str_copy (album, albumlen));
                    }
                }
                else
                    Album = String ("_");

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
                String cover_helper = aud_get_str ("audacious", "cover_helper");
                StringBuf album_buf = str_encode_percent (Album);
                StringBuf artist_buf = str_encode_percent (Artist);
                StringBuf title_buf = str_encode_percent (Title);
                String coverart_file;
                Index<String> extlist = str_list_to_index ("jpg,png,jpeg,gif", ",");
                if (! album_art_found)  /* CAN BE SET BY ANOTHER THREAD-INSTANCE IN THE MEAN TIME! */
#ifdef _WIN32
                    WinExec ((const char *) str_concat ({cover_helper, " ALBUM '",
                            (const char *) album_buf, "' ", aud_get_path (AudPath::UserDir), " '",
                            (const char *) artist_buf, "' '", (const char *) title_buf, "' "}),
                            SW_HIDE);
#else
                    system ((const char *) str_concat ({cover_helper, " ALBUM '",
                            (const char *) album_buf, "' ", aud_get_path (AudPath::UserDir), " '",
                            (const char *) artist_buf, "' '", (const char *) title_buf, "' "}));
#endif

                if (! album_art_found)  /* CAN BE SET BY ANOTHER THREAD-INSTANCE IN THE MEAN TIME! */
                {
                    for (auto & ext : extlist)
                    {
                        coverart_file = String (str_concat ({"file://", aud_get_path (AudPath::UserDir), "/_tmp_albumart.", (const char *) ext}));
                        const char * filenamechar = coverart_file + 7;
                        struct stat statbuf;
                        if (stat (filenamechar, &statbuf) < 0)  // ART IMAGE FILE DOESN'T EXIST:
                            coverart_file = String (_(""));
                        else
                        {
                            coverart_file = String (filename_to_uri (filenamechar));
                            break;
                        }
                    }
                    if (coverart_file && coverart_file[0])
                        ((ArtLabel *) data)->origPixmap = QPixmap (audqt::art_request ((const char *) coverart_file, 0, 0));
                }
                else
                    return nullptr;  /* DON'T OVERWRITE - WE ALREADY HAVE IT FROM ANOTHER THREAD-INSTANCE! */
            }
        }

        if (! ((ArtLabel *) data)->origPixmap)
            ((ArtLabel *) data)->origPixmap = QPixmap (audqt::art_request_current (0, 0));

        ((ArtLabel *) data)->origSize = ((ArtLabel *) data)->origPixmap.size ();
        ((ArtLabel *) data)->drawArt ();

        return nullptr;
    }

};

#undef MARGIN

static void init_update (void *, ArtLabel * widget)
{
    widget->init_update_art ();
}

static void update (void *, ArtLabel * widget)
{
    widget->update_art ();
}

static void clear (void *, ArtLabel * widget)
{
    widget->clear ();
}

static void widget_cleanup (QObject * widget)
{
    //x hook_dissociate ("playback ready", (HookFunction) update, widget);
    hook_dissociate ("playback stop", (HookFunction) clear, widget);
    hook_dissociate ("playback ready", (HookFunction) init_update, widget);
    hook_dissociate ("tuple change", (HookFunction) update, widget);
}

void * AlbumArtQt::get_qt_widget ()
{
    ArtLabel * widget = new ArtLabel;

    QObject::connect (widget, &QObject::destroyed, widget_cleanup);

    hook_associate ("tuple change", (HookFunction) update, widget);
    hook_associate ("playback ready", (HookFunction) init_update, widget);
    hook_associate ("playback stop", (HookFunction) clear, widget);

    if (aud_drct_get_ready ())
        widget->update_art ();

    return widget;
}

EXPORT AlbumArtQt aud_plugin_instance;

const char * const AlbumArtQt::defaults[] = {
    "internet_coverartlookup", "FALSE",
    nullptr
};

bool AlbumArtQt::init ()
{
    aud_config_set_defaults ("albumart", defaults);
    return true;
}

const PreferencesWidget AlbumArtQt::widgets[] = {
    WidgetLabel(N_("<b>Albumart Configuration</b>")),
    WidgetCheck (N_("Look for album art on Musicbrainz.com"),
        WidgetBool ("albumart", "internet_coverartlookup")),
};

const PluginPreferences AlbumArtQt::prefs = {{widgets}};
