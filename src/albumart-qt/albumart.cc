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

#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libfauxdcore/preferences.h>
#include <libfauxdcore/drct.h>
#include <libfauxdcore/i18n.h>
#include <libfauxdcore/plugin.h>
#include <libfauxdcore/hook.h>
#include <libfauxdcore/audstrings.h>
#include <libfauxdcore/runtime.h>

#include <libfauxdqt/libfauxdqt.h>

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
        String cover_helper = aud_get_str ("audacious", "cover_helper");
        origPixmap = QPixmap ();
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
                system ((const char *) str_concat ({cover_helper, " ALBUM '",
                        (const char *) album_buf, "' ", aud_get_path (AudPath::UserDir), " '",
                        (const char *) artist_buf, "' '", (const char *) title_buf, "' "}));
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
                    origPixmap = QPixmap (audqt::art_request ((const char *) coverart_file, 0, 0));
            }
        }

        if (! origPixmap)
            origPixmap = QPixmap (audqt::art_request_current (0, 0));

        origSize = origPixmap.size ();
        drawArt ();
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
