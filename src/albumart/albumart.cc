/*
 * albumart.c
 * Copyright 2012-2013 John Lindgren
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
#include <libfauxdgui/libfauxdgui.h>
#include <libfauxdgui/libfauxdgui-gtk.h>

class AlbumArtPlugin : public GeneralPlugin
{
public:
    static const char * const defaults[];
    static const PreferencesWidget widgets[];
    static const PluginPreferences prefs;
    static constexpr PluginInfo info = {
        N_("Album Art"),
        PACKAGE,
        nullptr, // about
        & prefs,
        PluginGLibOnly
    };

    constexpr AlbumArtPlugin () : GeneralPlugin (info, false) {}

    bool init ();
    void * get_gtk_widget ();
};

EXPORT AlbumArtPlugin aud_plugin_instance;

const char * const AlbumArtPlugin::defaults[] = {
    "internet_coverartlookup", "FALSE",
    nullptr
};

bool AlbumArtPlugin::init ()
{
    aud_config_set_defaults ("albumart", defaults);
    return true;
}

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

static void * helper_thread_fn (void * data)
{
    GtkWidget * widget = (GtkWidget *) data;

    bool isLocked = false;
    AudguiPixbuf pixbuf;
    String cover_helper = aud_get_str ("audacious", "cover_helper");

    pthread_mutex_lock (& mutex);
    isLocked = true;

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
            {
                pthread_mutex_unlock (& mutex);
                isLocked = false;
                pixbuf = audgui_pixbuf_request ((const char *) coverart_file);
                if (pixbuf)
                {
                    audgui_scaled_image_set (widget, pixbuf.get ());
                    return nullptr;
                }
            }
        }
    }

    if (isLocked)
        pthread_mutex_unlock (& mutex);

    if (! pixbuf)
        pixbuf = audgui_pixbuf_request_current ();

    if (! pixbuf)
        pixbuf = audgui_pixbuf_fallback ();

    if (pixbuf)
        audgui_scaled_image_set (widget, pixbuf.get ());

    return nullptr;
}

static void album_update (void *, GtkWidget * widget)
{
    if (aud_get_str ("audacious", "cover_helper"))
    {
        pthread_t helper_thread;

        if (pthread_create (&helper_thread, nullptr, helper_thread_fn, widget))
        {
            AUDERR ("s:Error creating helper thread: %s - Expect Delay!...\n", strerror(errno));
            helper_thread_fn (widget);
        }
        else if (pthread_detach (helper_thread))
            AUDERR ("s:Error detaching helper thread: %s!\n", strerror(errno));
    }
    else
    {
        AudguiPixbuf pixbuf = audgui_pixbuf_request_current ();

        if (! pixbuf)
            pixbuf = audgui_pixbuf_fallback ();

        if (pixbuf)
            audgui_scaled_image_set (widget, pixbuf.get ());
    }
}

static void album_init (void *, GtkWidget * widget)
{
    AudguiPixbuf pixbuf = audgui_pixbuf_request_current ();

    if (! pixbuf)
        pixbuf = audgui_pixbuf_fallback ();

    if (pixbuf)
        audgui_scaled_image_set (widget, pixbuf.get ());

    if (aud_get_bool ("albumart", "internet_coverartlookup"))
        album_update (nullptr, widget);  // JWT:CHECK FILES & DISKS (TUPLE DOESN'T CHANGE IN THESE) ONCE NOW ON PLAY START!
}

static void album_clear (void *, GtkWidget * widget)
{
    audgui_scaled_image_set (widget, nullptr);
}

static void album_cleanup (GtkWidget * widget)
{
    hook_dissociate ("playback stop", (HookFunction) album_clear, widget);
    hook_dissociate ("playback ready", (HookFunction) album_init, widget);
    hook_dissociate ("tuple change", (HookFunction) album_update, widget);

    audgui_cleanup ();
}

void * AlbumArtPlugin::get_gtk_widget ()
{
    audgui_init ();

    GtkWidget * widget = audgui_scaled_image_new (nullptr);

    g_signal_connect (widget, "destroy", (GCallback) album_cleanup, nullptr);

    hook_associate ("tuple change", (HookFunction) album_update, widget);
    hook_associate ("playback ready", (HookFunction) album_init, widget);
    hook_associate ("playback stop", (HookFunction) album_clear, widget);

    if (aud_drct_get_ready ())
        album_init (nullptr, widget);

    return widget;
}

/* JWT:FIXME: THIS IS MARKED "EXPERIMENTAL" IN WINDOWS SINCE GUI-INTERACTION CAN
    BECOME INVISIBLE AFTER A TIME UNTIL PLAY STOPPED & RESTARTED LEADING TO A BAD
    USER-EXPERIENCE, AND I HAVEN'T BEEN ABLE TO FIGURE OUT WHY?!?!?!
*/

const PreferencesWidget AlbumArtPlugin::widgets[] = {
    WidgetLabel(N_("<b>Albumart Configuration</b>")),
#ifdef _WIN32
    WidgetCheck (N_("Look for album art on Musicbrainz.com (EXPERIMENTAL!)"),
#else
    WidgetCheck (N_("Look for album art on Musicbrainz.com"),
#endif
        WidgetBool ("albumart", "internet_coverartlookup")),
};

const PluginPreferences AlbumArtPlugin::prefs = {{widgets}};
