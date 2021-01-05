/*
 * Audacious CD Digital Audio plugin
 *
 * Copyright (c) 2007 Calin Crisan <ccrisan@gmail.com>
 * Copyright (c) 2009-2012 John Lindgren <john.lindgren@aol.com>
 * Copyright (c) 2009 Tomasz Moń <desowin@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; under version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses>.
 */

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winbase.h>
#endif

extern "C" {
#include <sys/stat.h>
}

/* prevent libcdio from redefining PACKAGE, VERSION, etc. */
#define EXTERNAL_LIBCDIO_CONFIG_H

#include <cdio/cdio.h>
#include <cdio/cdtext.h>
#include <cdio/track.h>
#include <cdio/audio.h>
#include <cdio/sector.h>
#include <cdio/cd_types.h>

#if LIBCDIO_VERSION_NUM >= 90
#include <cdio/paranoia/cdda.h>
#else
#include <cdio/cdda.h>
#endif

#include <cddb/cddb.h>

#include <libfauxdcore/audstrings.h>
#include <libfauxdcore/hook.h>
#include <libfauxdcore/i18n.h>
#include <libfauxdcore/interface.h>
#include <libfauxdcore/mainloop.h>
#include <libfauxdcore/playlist.h>
#include <libfauxdcore/plugin.h>
#include <libfauxdcore/probe.h>
#include <libfauxdcore/preferences.h>
#include <libfauxdcore/runtime.h>

#define MIN_DISC_SPEED 2
#define MAX_DISC_SPEED 24

#define MAX_RETRIES 10
#define MAX_SKIPS 10

static const char * const cdaudio_schemes[] = {"cdda", nullptr};

class CDAudio : public InputPlugin
{
public:
    static const char about[];
    static const char * const defaults[];
    static const PreferencesWidget widgets[];
    static const PluginPreferences prefs;

    static constexpr PluginInfo info = {
        N_("Audio CD Plugin"),
        PACKAGE,
        about,
        & prefs
    };

    constexpr CDAudio () : InputPlugin (info, InputInfo (FlagSubtunes)
        .with_schemes (cdaudio_schemes)) {}

    bool init ();
    void cleanup ();

    bool is_our_file (const char * filename, VFSFile & file);
    bool read_tag (const char * filename, VFSFile & file, Tuple & tuple, Index<char> * image);
    bool play (const char * filename, VFSFile & file);
};

EXPORT CDAudio aud_plugin_instance;

typedef struct
{
    String performer;
    String name;
    String genre;
    int startlsn;
    int endlsn;
    bool tag_read;    /* JWT:TRUE IF WE'VE ALREADY READ THE TAG DATA FOR THIS TRACK.  */
    int tag_source;   /* JWT:0=NO TAGS(YET?), 1=FROM CDD[A|B], 2=FROM tmp_tag_data, 3=FROM CUSTOM TAG FILE */
    String discidstr; /* JWT:SAVE THAT DISK-ID FOR COVER-ART STUFF. */
}
trackinfo_t;

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static bool playing;

/* lock mutex to read / set these variables */
static int firsttrackno = -1;
static int lasttrackno = -1;
static int n_audio_tracks;
static cdrom_drive_t *pcdrom_drive = nullptr;
static Index<trackinfo_t> trackinfo;
static QueuedFunc purge_func;

static bool scan_cd ();
static bool refresh_trackinfo (bool warning);
static void reset_trackinfo ();
static int calculate_track_length (int startlsn, int endlsn);
static int find_trackno_from_filename (const char * filename);
static String coverart_file;       /* JWT:PATH OF LAST GOOD COVER ART FILE (IF ANY) FOR CURRENTLY-PLAYING CD. */
static bool coverart_file_sought;  /* JWT:TRUE IF WE'VE ALREADY LOOKED FOR A COVER ART FILE FOR CURRENTLY-PLAYING CD. */
static bool custom_tagfile_sought; /* JWT:TRUE IF WE'VE ALREADY LOOKED FOR A CUSTOM TAG FILE FOR CURRENTLY-PLAYING CD. */
static bool custom_helper_sought;  /* JWT:TRUE IF WE'VE ALREADY INVOKED HELPER SCRIPT FOR CURRENTLY-PLAYING CD. */

const char CDAudio::about[] =
 N_("Copyright (C) 2007-2012 Calin Crisan <ccrisan@gmail.com> and others.\n\n"
    "Many thanks to libcdio developers <http://www.gnu.org/software/libcdio/>\n"
    "and to libcddb developers <http://libcddb.sourceforge.net/>.\n\n"
    "Also thank you to Tony Vroon for mentoring and guiding me.\n\n"
    "This was a Google Summer of Code 2007 project.");

const char * const CDAudio::defaults[] = {
 "disc_speed", "2",
 "use_cdtext", "TRUE",
 "use_cddb", "TRUE",
 "cddbhttp", "FALSE",
 "cddbserver", "gnudb.gnudb.org",  // SEE: https://www.gnudb.org/
 "cddbport", "8880",
 "use_customtagfiles", "TRUE",
 nullptr};

const PreferencesWidget CDAudio::widgets[] = {
    WidgetLabel (N_("<b>Device</b>")),
    WidgetSpin (N_("Read speed:"),
        WidgetInt ("CDDA", "disc_speed"),
        {MIN_DISC_SPEED, MAX_DISC_SPEED, 1}),
    WidgetEntry (N_("Override device:"),
        WidgetString ("CDDA", "device")),
    WidgetLabel (N_("<b>Metadata</b>")),
    WidgetCheck (N_("Use CD-Text"),
        WidgetBool ("CDDA", "use_cdtext")),
    WidgetCheck (N_("Use CDDB"),
        WidgetBool ("CDDA", "use_cddb")),
    WidgetCheck (N_("Use HTTP instead of CDDBP"),
        WidgetBool ("CDDA", "cddbhttp"),
        WIDGET_CHILD),
    WidgetEntry (N_("Server:"),
        WidgetString ("CDDA", "cddbserver"),
        {false},
        WIDGET_CHILD),
    WidgetEntry (N_("Path:"),
        WidgetString ("CDDA", "cddbpath"),
        {false},
        WIDGET_CHILD),
    WidgetSpin (N_("Port:"),
        WidgetInt ("CDDA", "cddbport"),
        {0, 65535, 1},
        WIDGET_CHILD),
    WidgetCheck (N_("Allow Custom Tag-files"),
        WidgetBool ("CDDA", "use_customtagfiles"))
};

const PluginPreferences CDAudio::prefs = {{widgets}};

static void cdaudio_error (const char * message_format, ...)
{
    va_list args;
    va_start (args, message_format);
    StringBuf msg = str_vprintf (message_format, args);
    va_end (args);

    aud_ui_show_error (msg);
}

/* main thread only */
static void purge_playlist (int playlist)
{
    int length = aud_playlist_entry_count (playlist);

    for (int count = 0; count < length; count ++)
    {
        String filename = aud_playlist_entry_get_filename (playlist, count);

        if (! strncmp (filename, "cdda://", 7))
        {
            aud_playlist_entry_delete (playlist, count, 1);
            count--;
            length--;
        }
    }
}

/* main thread only */
static void purge_all_playlists ()
{
    int playlists = aud_playlist_count ();
    int count;

    for (count = 0; count < playlists; count++)
        purge_playlist (count);
}

/* main thread only */
static void monitor (void *)
{
    pthread_mutex_lock (& mutex);

    /* make sure not to close drive handle while playing */
    if (! playing)
        refresh_trackinfo (false);

    pthread_mutex_unlock (& mutex);
}

/* main thread only */
bool CDAudio::init ()
{
    aud_config_set_defaults ("CDDA", defaults);

    if (!cdio_init ())
    {
        cdaudio_error (_("Failed to initialize cdio subsystem."));
        return false;
    }

    libcddb_init ();

    return true;
}

/* thread safe (mutex may be locked) */
bool CDAudio::is_our_file (const char * filename, VFSFile & file)
{
    return ! strncmp (filename, "cdda://", 7);
}

/* play thread only */
bool CDAudio::play (const char * name, VFSFile & file)
{
    pthread_mutex_lock (& mutex);

    if (! trackinfo.len () && ! refresh_trackinfo (true))
    {
        pthread_mutex_unlock (& mutex);
        return false;
    }

    bool okay = false;
    int trackno = find_trackno_from_filename (name);

    if (trackno < 0)
        cdaudio_error (_("Invalid URI %s."), name);
    else if (trackno < firsttrackno || trackno > lasttrackno)
        cdaudio_error (_("Track %d not found."), trackno);
    else if (! cdda_track_audiop (pcdrom_drive, trackno))
        cdaudio_error (_("Track %d is a data track."), trackno);
    else
        okay = true;

    if (! okay)
    {
        pthread_mutex_unlock (& mutex);
        return false;
    }

    set_stream_bitrate (1411200);
    open_audio (FMT_S16_LE, 44100, 2);

    int startlsn = trackinfo[trackno].startlsn;
    int endlsn = trackinfo[trackno].endlsn;

    playing = true;

    int buffer_size = aud_get_int (nullptr, "output_buffer_size");
    int speed = aud_get_int ("CDDA", "disc_speed");
    speed = aud::clamp (speed, MIN_DISC_SPEED, MAX_DISC_SPEED);
    int sectors = aud::clamp (buffer_size / 2, 50, 250) * speed * 75 / 1000;
    int currlsn = startlsn;
    int retry_count = 0, skip_count = 0;

    Index<unsigned char> buffer;
    buffer.insert (0, 2352 * sectors);

    while (! check_stop ())
    {
        int seek_time = check_seek ();
        if (seek_time >= 0)
            currlsn = startlsn + (seek_time * 75 / 1000);

        sectors = aud::min (sectors, endlsn + 1 - currlsn);
        if (sectors < 1)
            break;

        /* unlock mutex here to avoid blocking
         * other threads must be careful not to close drive handle */
        pthread_mutex_unlock (& mutex);

        int ret = cdio_read_audio_sectors (pcdrom_drive->p_cdio,
         buffer.begin (), currlsn, sectors);

        if (ret == DRIVER_OP_SUCCESS)
            write_audio (buffer.begin (), 2352 * sectors);

        pthread_mutex_lock (& mutex);

        if (ret == DRIVER_OP_SUCCESS)
        {
            currlsn += sectors;
            retry_count = 0;
            skip_count = 0;
        }
        else if (sectors > 16)
        {
            /* maybe a smaller read size will help */
            sectors /= 2;
        }
        else if (retry_count < MAX_RETRIES)
        {
            /* still failed; retry a few times */
            retry_count ++;
        }
        else if (skip_count < MAX_SKIPS)
        {
            /* maybe the disk is scratched; try skipping ahead */
            currlsn = aud::min (currlsn + 75, endlsn + 1);
            skip_count ++;
        }
        else
        {
            /* still failed; give it up */
            cdaudio_error (_("Error reading audio CD."));
            break;
        }
    }

    playing = false;

    pthread_mutex_unlock (& mutex);
    return true;
}

/* main thread only */
void CDAudio::cleanup ()
{
    pthread_mutex_lock (& mutex);

    reset_trackinfo ();
    purge_func.stop ();

    libcddb_shutdown ();

    pthread_mutex_unlock (& mutex);
}

/* thread safe */
bool CDAudio::read_tag (const char * filename, VFSFile & file, Tuple & tuple,
 Index<char> * image)
{
    bool whole_disk = ! strcmp (filename, "cdda://");
    bool valid = false;

    pthread_mutex_lock (& mutex);

    /* reset cached info when adding CD to the playlist */
    if (whole_disk && ! playing)
        reset_trackinfo ();

    if (! trackinfo.len () && ! refresh_trackinfo (true))
        goto DONE;

    if (whole_disk)
    {
        Index<short> subtunes;

        /* only add the audio tracks to the playlist */
        for (int trackno = firsttrackno; trackno <= lasttrackno; trackno++)
            if (cdda_track_audiop (pcdrom_drive, trackno))
                subtunes.append (trackno);

        tuple.set_subtunes (subtunes.len (), subtunes.begin ());
        cdio_get_media_changed (pcdrom_drive->p_cdio);  /* JWT:PREVENT EXTRA SCAN. */

        valid = true;
    }
    else
    {
        int trackno = find_trackno_from_filename (filename);

        if (trackno < firsttrackno || trackno > lasttrackno)
        {
            AUDERR ("Track %d not found.\n", trackno);
            goto DONE;
        }

        int disktagrefresh = aud_get_int (nullptr, "_disktagrefresh");
        if (disktagrefresh == trackno)  /* JWT:FORCE REFRESH ON TRACK# WITH UPDATED SONG INFO! */
        {
            trackinfo[trackno].tag_read = false;
            custom_tagfile_sought = false;
        }
        tuple.set_int (Tuple::Length, 
                calculate_track_length (trackinfo[trackno].startlsn, trackinfo[trackno].endlsn));
        tuple.set_int (Tuple::Channels, cdio_get_track_channels (pcdrom_drive->p_cdio, trackno));

        if (! trackinfo[trackno].tag_read)  /* JWT:ONLY NEED TO FETCH TRACK INFO ONCE! */
        {
            if (!cdda_track_audiop (pcdrom_drive, trackno))
            {
                AUDERR ("Track %d is a data track.\n", trackno);
                goto DONE;
            }

            trackinfo[trackno].tag_read = true;
            tuple.set_format (_("Audio CD"), 2, 44100, 1411);
            tuple.set_int (Tuple::Track, trackno);
            if (trackinfo[trackno].name)
                tuple.set_str (Tuple::Title, trackinfo[trackno].name);
            if (trackinfo[trackno].performer)
                tuple.set_str (Tuple::Artist, trackinfo[trackno].performer);
            if (trackinfo[0].name)
                tuple.set_str (Tuple::Album, trackinfo[0].name);
            if (trackinfo[0].performer)
                tuple.set_str (Tuple::AlbumArtist, trackinfo[0].performer);
            if (trackinfo[trackno].genre)
                tuple.set_str (Tuple::Genre, trackinfo[trackno].genre);
            if (! custom_tagfile_sought && aud_get_bool ("CDDA", "use_customtagfiles")
                    && trackinfo[0].discidstr && trackinfo[0].discidstr[0])
            {
                AUDINFO ("--DISKID=%s= TRYING CUSTOM\n", (const char *)trackinfo[0].discidstr);
                String tag_file = String (str_concat ({(const char *)trackinfo[0].discidstr, ".tag"}));
                Tuple user_tuple = Tuple ();
                int precedence = aud_read_tag_from_tagfile ((const char *)str_printf ("%s%d", "cdda://?", trackno), 
                        (const char *)tag_file, user_tuple);
                AUDDBG ("--TAG FID=%s= TRACK=%d= PREC=%d=\n", (const char *)tag_file, trackno, precedence);
                if (precedence)
                {
                    if (disktagrefresh == trackno)
                        precedence = 2;  // FORCE OVERWRITE IF USER CHGD. SONG INFO!:
                    trackinfo[trackno].tag_source = 3;  // WE FETCHED FROM CUSTOM FILE.
                    AUDINFO ("--CUSTOM TAG(%d) FILE(%s) PRECEDENCE=%d\n", trackno, (const char *)tag_file, precedence);
                    const char * tfld = (const char *) user_tuple.get_str (Tuple::Title);
                    if (tfld && (precedence > 1 || ! trackinfo[trackno].name))
                    {
                        tuple.set_str (Tuple::Title, tfld);
                        trackinfo[trackno].name = String (tfld);
                    }
                    tfld = (const char *) user_tuple.get_str (Tuple::Artist);
                    if (tfld && (precedence > 1 || ! trackinfo[trackno].performer))
                    {
                        tuple.set_str (Tuple::Artist, tfld);
                        trackinfo[trackno].performer = String (tfld);
                    }
                    tfld = (const char *) user_tuple.get_str (Tuple::Album);
                    if (tfld && (precedence > 1 || ! trackinfo[0].name))
                    {
                        tuple.set_str (Tuple::Album, tfld);
                        if (! trackinfo[0].name)
                            trackinfo[0].name = String (tfld);
                    }
                    tfld = (const char *) user_tuple.get_str (Tuple::AlbumArtist);
                    if (tfld && (precedence > 1 || ! trackinfo[trackno].performer))
                    {
                        tuple.set_str (Tuple::AlbumArtist, tfld);
                        if (! trackinfo[trackno].performer)
                            trackinfo[trackno].performer = String (tfld);
                    }
                    else if (tfld)
                    {
                        const char * aafld = (const char *) tuple.get_str (Tuple::AlbumArtist);
                        if (! aafld)
                            tuple.set_str (Tuple::AlbumArtist, tfld);
                    }
                    tfld = (const char *) user_tuple.get_str (Tuple::Genre);
                    if (tfld && (precedence > 1 || ! trackinfo[trackno].genre))
                    {
                        tuple.set_str (Tuple::Genre, tfld);
                        trackinfo[trackno].genre = String (tfld);
                    }
                    tfld = (const char *) user_tuple.get_str (Tuple::Comment);
                    if (tfld && (precedence > 1 || ! coverart_file))
                        tuple.set_str (Tuple::Comment, tfld);
                    int ifld = user_tuple.get_int (Tuple::Year);
                    if (ifld && ifld > 1000)
                        tuple.set_int (Tuple::Year, ifld);
                }
                else
                    custom_tagfile_sought = true;  //ONLY SEEK ONCE IF NOT FOUND, OTHERWISE FOR EACH TRACK!
            }

            if (! coverart_file_sought)
            {
                Index<String> extlist = str_list_to_index ("jpg,png,jpeg", ",");
                String coverart_path = aud_get_str ("CDDA", "cover_art_path");
                if (! coverart_path || ! coverart_path[0])
                    coverart_path = String (aud_get_path (AudPath::UserDir));
                if (trackinfo[0].name)  //SEE IF WE HAVE A COVER-ART IMAGE FILE NAMED AFTER THE DISK TITLE:
                {
                    AUDINFO ("--CVAPATH=%s= tk0name=%s=\n", (const char *)coverart_path, (const char *)trackinfo[0].name);
                    StringBuf fid_buf = filename_build ({(const char *)coverart_path, 
                            (const char *)trackinfo[0].name});
                    for (auto & ext : extlist)
                    {
                        coverart_file = String (str_concat ({"file://", fid_buf, ".", (const char *)ext}));
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
                }
                //IF NO COVER-ART FILE NAMED AFTER THE TITLE, SEE IF ONE NAMED AFTER THE DISK-ID:
                if ((! coverart_file || ! coverart_file[0])
                        && trackinfo[0].discidstr && trackinfo[0].discidstr[0])
                {
                    Index<String> extlist = str_list_to_index ("jpg,png,jpeg", ",");
                    AUDINFO ("--NO COVER ART FILE FOR TITLE, TRY DISK-ID:\n");
                    StringBuf fid_buf = filename_build ({(const char *)coverart_path, 
                            (const char *)trackinfo[0].discidstr});
                    for (auto & ext : extlist)
                    {
                        coverart_file = String (str_concat ({"file://", fid_buf, ".", (const char *)ext}));
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
                }
            }
            if (aud_get_bool (nullptr, "user_tag_data"))  /* JWT:SEE IF WE HAVE A CUSTOM TAGFILE FOR THIS CD: */
            {
                if (! custom_helper_sought && trackinfo[0].discidstr && trackinfo[0].discidstr[0]
                        && (! coverart_file || ! coverart_file[0] || ! trackinfo[0].name || ! trackinfo[trackno].name))
                {
                    /* NOTE: NOT ALL CDS HAVE TRACK TITLE INFO OR WORK W/CDDB, SO IF NOT, WE FETCH THAT TOO!: */
                    /* NOTE2:  IF USER SAVED COVER-ART FILE BY TITLE, THIS WEB SEARCH IS NOT MADE! */
                    AUDINFO ("--NO COVER ART BY DISK-ID OR NO CD TITLE, LOOK FOR HELPER:\n");
                    String cover_helper = aud_get_str ("audacious", "cover_helper");
                    const char * cdt = ((! trackinfo[0].name || ! trackinfo[trackno].name) 
                            && aud_get_bool ("CDDA", "use_customtagfiles")) ? " CDT " : " CD ";
                    if (trackinfo[0].name) AUDINFO ("--T0name=%s=\n", (const char *)trackinfo[0].name);
                    if (trackinfo[trackno].name) AUDINFO ("--T(%d)name=%s=\n", trackno, (const char *)trackinfo[trackno].name);
                    if (cover_helper[0])  //JWT:WE HAVE A PERL HELPER, LESSEE IF IT CAN FIND/DOWNLOAD A COVER IMAGE FOR US:
                    {
                        AUDINFO ("----HELPER FOUND: WILL DO (%s)\n", (const char *)str_concat ({cover_helper, cdt, 
                                (const char *)trackinfo[0].discidstr, " ", aud_get_path (AudPath::UserDir)}));
#ifdef _WIN32
                        WinExec ((const char *) str_concat ({cover_helper, cdt, 
                                (const char *)trackinfo[0].discidstr, " ", aud_get_path (AudPath::UserDir)}),
                                SW_HIDE);
#else
                        system ((const char *) str_concat ({cover_helper, cdt, 
                                (const char *)trackinfo[0].discidstr, " ", aud_get_path (AudPath::UserDir)}));
#endif
                        Index<String> extlist = str_list_to_index ("jpg,png,jpeg", ",");
                        String coverart_path = aud_get_str ("CDDA", "cover_art_path");
                        if (! coverart_path || ! coverart_path[0])
                            coverart_path = String (aud_get_path (AudPath::UserDir));
                        AUDINFO ("--NO COVER ART FILE FOR TITLE, TRY DISK-ID:\n");
                        StringBuf fid_buf = filename_build ({(const char *)coverart_path, 
                                (const char *)trackinfo[0].discidstr});
                        for (auto & ext : extlist)
                        {
                            coverart_file = String (str_concat ({"file://", fid_buf, ".", (const char *)ext}));
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
                    }
                }
                custom_helper_sought = true;

                /* WE MUST MANUALLY CHECK EACH TRACK TITLE IS UPDATED IF TITLES FETCHED BY COVER-ART HELPER! */
                if (trackno && ! trackinfo[trackno].name)
                {
                    AUDINFO ("--NO TITLE SET FOR TRACK(%d), WILL TRY TO FETCH!\n", trackno);
                    Tuple user_tuple = Tuple ();
                    if (aud_read_tag_from_tagfile ((const char *) str_printf ("%s%d", "cdda://?", trackno),
                            "tmp_tag_data", user_tuple))
                    {
                        trackinfo[trackno].tag_source = 2; // WE FETCHED FROM tmp_tag_data.
                        const char * tfld = (const char *) user_tuple.get_str (Tuple::Title);
                        if (tfld)
                        {
                            AUDINFO ("--SET TRK(%d):=%s=\n", trackno, tfld);
                            tuple.set_str (Tuple::Title, tfld);
                            trackinfo[trackno].name = String (tfld);
                        }
                        if (! trackinfo[trackno].performer)
                        {
                            const char * tfld = (const char *) user_tuple.get_str (Tuple::Artist);
                            if (tfld)
                                tuple.set_str (Tuple::Artist, tfld);
                        }
                        if (! tuple.get_str (Tuple::Album))
                        {
                            const char * tfld = (const char *) user_tuple.get_str (Tuple::Album);
                            if (tfld)
                                tuple.set_str (Tuple::Album, tfld);
                        }
                        if (! tuple.get_str (Tuple::AlbumArtist))
                        {
                            const char * tfld = (const char *) user_tuple.get_str (Tuple::AlbumArtist);
                            if (tfld)
                                tuple.set_str (Tuple::AlbumArtist, tfld);
                        }
                    }
                }
            }
            coverart_file_sought = true;
            if (coverart_file)
            {
                const char * tfld = (const char *) tuple.get_str (Tuple::Comment);
                if (! tfld)
                    tuple.set_str (Tuple::Comment, coverart_file);
            }
            if (trackinfo[trackno].tag_source > 0) // WE GOT SOME TAG DATA FROM SOMEWHERE!:
            {
                if (trackinfo[trackno].tag_source < 3 // 3=WE FETCHED FROM CUSTOM TAG FILE: NO NEED TO OVERWRITE!
                        && aud_get_bool ("CDDA", "use_customtagfiles")
                        && trackinfo[0].discidstr && trackinfo[0].discidstr[0])
                {
                    String tag_file = String (str_concat ({(const char *) trackinfo[0].discidstr, ".tag"}));
                    aud_write_tag_to_tagfile (filename, tuple, (const char *) tag_file);
                }
                if (trackinfo[trackno].tag_source != 2) // 2=WE FETCHED FROM EXISTING tmp_tag_data: NO NEED TO OVERWRITE!
                    aud_write_tag_to_tagfile (filename, tuple, "tmp_tag_data");
            }
            else if (coverart_file)  // NO TAG DATA - JUST WRITE OUT A tmp_tag_data W/"Comment=<cover-art-fid> FOR EACH TRACK:
                    aud_write_tag_to_tagfile (filename, tuple, "tmp_tag_data");
        }
        if (disktagrefresh)
            aud_set_int (nullptr, "_disktagrefresh", 0);

        valid = true;
    }

  DONE:
    pthread_mutex_unlock (& mutex);
    return valid;
}

/* mutex must be locked */
static bool open_cd ()
{
    if (pcdrom_drive)
        return true;

    AUDDBG ("Opening CD drive.\n");
    String device = aud_get_str ("CDDA", "device");

    if (device[0])
    {
        if (! (pcdrom_drive = cdda_identify (device, 1, nullptr)))
            cdaudio_error (_("Failed to open CD Device %s."), (const char *) device);
    }
    else
    {
        char * * ppcd_drives = cdio_get_devices_with_cap (nullptr, CDIO_FS_AUDIO, false);

        if (ppcd_drives && ppcd_drives[0])
        {
            if (! (pcdrom_drive = cdda_identify (ppcd_drives[0], 1, nullptr)))
                cdaudio_error (_("Failed to open CD device %s."), ppcd_drives[0]);
        }
        else
            cdaudio_error (_("No audio capable CD drive found."));

        if (ppcd_drives)
            cdio_free_device_list (ppcd_drives);
    }

    return (bool) pcdrom_drive;
}

/* mutex must be locked */
static bool check_disc_mode (bool warning)
{
    int mode = cdio_get_discmode (pcdrom_drive->p_cdio);

#ifdef _WIN32 /* cdio_get_discmode reports the wrong disk type sometimes */
    if (mode == CDIO_DISC_MODE_NO_INFO || mode == CDIO_DISC_MODE_ERROR)
#else
    if (mode != CDIO_DISC_MODE_CD_DA && mode != CDIO_DISC_MODE_CD_MIXED)
#endif
    {
        if (warning)
        {
            if (mode == CDIO_DISC_MODE_NO_INFO)
                cdaudio_error (_("Drive is empty."));
            else
                cdaudio_error (_("Unsupported disk type."));
        }

        return false;
    }

    return true;
}

/* mutex must be locked */
static bool scan_cd ()
{
    AUDDBG ("Scanning CD drive.\n");
    trackinfo.clear ();
    coverart_file_sought = false;
    custom_tagfile_sought = false;
    custom_helper_sought = false;

    /* general track initialization */

    /* skip endianness detection (because it only affects cdda_read, and we use
     * cdio_read_audio_sectors instead) */
    pcdrom_drive->bigendianp = 0;

    /* finish initialization of drive/disc (performs disc TOC sanitization) */
    if (cdda_open (pcdrom_drive) != 0)
    {
        cdaudio_error (_("Failed to finish initializing opened CD drive."));
        return false;
    }

    int speed = aud_get_int ("CDDA", "disc_speed");
    speed = aud::clamp (speed, MIN_DISC_SPEED, MAX_DISC_SPEED);
    if (cdda_speed_set (pcdrom_drive, speed) != DRIVER_OP_SUCCESS)
        AUDERR ("Cannot set drive speed.\n");

    firsttrackno = cdio_get_first_track_num (pcdrom_drive->p_cdio);
    lasttrackno = cdio_get_last_track_num (pcdrom_drive->p_cdio);
    if (firsttrackno == CDIO_INVALID_TRACK || lasttrackno == CDIO_INVALID_TRACK)
    {
        cdaudio_error (_("Failed to retrieve first/last track number."));
        return false;
    }
    AUDDBG ("first track is %d and last track is %d\n", firsttrackno,
           lasttrackno);

    trackinfo.insert (0, lasttrackno + 1);

    trackinfo[0].startlsn = cdda_track_firstsector (pcdrom_drive, 0);
    trackinfo[0].endlsn = cdda_track_lastsector (pcdrom_drive, lasttrackno);
    trackinfo[0].tag_read = false;
    trackinfo[0].tag_source = 0;

    n_audio_tracks = 0;

    for (int trackno = firsttrackno; trackno <= lasttrackno; trackno++)
    {
        trackinfo[trackno].startlsn = cdda_track_firstsector (pcdrom_drive, trackno);
        trackinfo[trackno].endlsn = cdda_track_lastsector (pcdrom_drive, trackno);
        trackinfo[trackno].tag_read = false;
        trackinfo[trackno].tag_source = 0;

        if (trackinfo[trackno].startlsn == CDIO_INVALID_LSN
            || trackinfo[trackno].endlsn == CDIO_INVALID_LSN)
        {
            cdaudio_error (_("Cannot read start/end LSN for track %d."), trackno);
            return false;
        }

        /* count how many tracks are audio tracks */
        if (cdda_track_audiop (pcdrom_drive, trackno))
            n_audio_tracks++;
    }

    /* get trackinfo[0] cdtext information (the disc) */
    cdtext_t *pcdtext = nullptr;
    if (aud_get_bool ("CDDA", "use_cdtext"))
    {
        AUDDBG ("getting cd-text information for disc\n");
#if LIBCDIO_VERSION_NUM >= 90
        pcdtext = cdio_get_cdtext (pcdrom_drive->p_cdio);
        if (pcdtext == nullptr)
#else
        pcdtext = cdio_get_cdtext (pcdrom_drive->p_cdio, 0);
        if (pcdtext == nullptr || pcdtext->field[CDTEXT_TITLE] == nullptr)
#endif
            AUDDBG ("no cd-text available for disc\n");
        else
        {
#if LIBCDIO_VERSION_NUM >= 90
            trackinfo[0].performer = String (cdtext_get_const (pcdtext, CDTEXT_FIELD_PERFORMER, 0));
            trackinfo[0].name = String (cdtext_get_const (pcdtext, CDTEXT_FIELD_TITLE, 0));
            trackinfo[0].genre = String (cdtext_get_const (pcdtext, CDTEXT_FIELD_GENRE, 0));
#else
            trackinfo[0].performer = String (pcdtext->field[CDTEXT_PERFORMER]);
            trackinfo[0].name = String (pcdtext->field[CDTEXT_TITLE]);
            trackinfo[0].genre = String (pcdtext->field[CDTEXT_GENRE]);
#endif
            trackinfo[0].tag_source = 1; // WE FETCHED FROM CDD[A|B].
        }
    }

    /* get track information from cdtext */
    bool cdtext_was_available = false;
    for (int trackno = firsttrackno; trackno <= lasttrackno; trackno++)
    {
#if LIBCDIO_VERSION_NUM < 90
        if (aud_get_bool ("CDDA", "use_cdtext"))
        {
            AUDDBG ("getting cd-text information for track %d\n", trackno);
            pcdtext = cdio_get_cdtext (pcdrom_drive->p_cdio, trackno);
            if (pcdtext == nullptr || pcdtext->field[CDTEXT_PERFORMER] == nullptr)
            {
                AUDDBG ("no cd-text available for track %d\n", trackno);
                pcdtext = nullptr;
            }
        }
#endif

        if (pcdtext != nullptr)
        {
#if LIBCDIO_VERSION_NUM >= 90
            trackinfo[trackno].performer = String (cdtext_get_const (pcdtext, CDTEXT_FIELD_PERFORMER, trackno));
            trackinfo[trackno].name = String (cdtext_get_const (pcdtext, CDTEXT_FIELD_TITLE, trackno));
            trackinfo[trackno].genre = String (cdtext_get_const (pcdtext, CDTEXT_FIELD_GENRE, trackno));
#else
            trackinfo[trackno].performer = String (pcdtext->field[CDTEXT_PERFORMER]);
            trackinfo[trackno].name = String (pcdtext->field[CDTEXT_TITLE]);
            trackinfo[trackno].genre = String (pcdtext->field[CDTEXT_GENRE]);
#endif
            trackinfo[trackno].tag_source = 1; // WE FETCHED FROM CDD[A|B].
            cdtext_was_available = true;
        }
    }

    // JWT: FETCH DISK-ID ANYWAY FOR COVERART QUERY:
    if (! trackinfo[0].discidstr)
    {
        cddb_disc_t *pcddb_disc = nullptr;
        cddb_track_t *pcddb_track = nullptr;
        lba_t lba;              /* Logical Block Address */

        pcddb_disc = cddb_disc_new ();

        lba = cdio_get_track_lba (pcdrom_drive->p_cdio,
                                  CDIO_CDROM_LEADOUT_TRACK);
        cddb_disc_set_length (pcddb_disc, FRAMES_TO_SECONDS (lba));

        for (int trackno = firsttrackno; trackno <= lasttrackno; trackno++)
        {
            pcddb_track = cddb_track_new ();
            cddb_track_set_frame_offset (pcddb_track,
                                         cdio_get_track_lba (
                                             pcdrom_drive->p_cdio,
                                             trackno));
            cddb_disc_add_track (pcddb_disc, pcddb_track);
        }

        cddb_disc_calc_discid (pcddb_disc);

        unsigned discid = cddb_disc_get_discid (pcddb_disc);
        AUDINFO ("CDDB2 disc id = %x\n", discid);
        trackinfo[0].discidstr = String (str_printf("%x", discid));
        if (trackinfo[0].discidstr && trackinfo[0].discidstr[0])
            aud_set_str (nullptr, "playingdiskid", trackinfo[0].discidstr);
        else
        {
            AUDINFO ("w:no Disc ID available (no custom tag file possible & edits saved to tmp_tag_data)!\n");
            aud_set_str (nullptr, "playingdiskid", "tmp_tag_data");
        }
    }

    if (!cdtext_was_available)
    {
        /* initialize de cddb subsystem */
        cddb_conn_t *pcddb_conn = nullptr;
        cddb_disc_t *pcddb_disc = nullptr;
        cddb_track_t *pcddb_track = nullptr;
        lba_t lba;              /* Logical Block Address */
        bool use_cddb = aud_get_bool ("CDDA", "use_cddb");
        if (use_cddb && aud_get_bool ("CDDA", "use_customtagfiles"))  // JWT:DON'T WASTE TIME W/CDDB IF WE HAVE CUSTOM TAG FILE!:
        {
            if (trackinfo[0].discidstr && trackinfo[0].discidstr[0])
            {
                StringBuf fid_buf = str_concat ({aud_get_path (AudPath::UserDir), 
                        "/", (const char *)trackinfo[0].discidstr, ".tag"});
                struct stat statbuf;
                if (stat ((const char *)fid_buf, &statbuf) >= 0)
                    use_cddb = false;
            }
        }

        if (use_cddb)
        {
            pcddb_conn = cddb_new ();
            if (pcddb_conn == nullptr)
                cdaudio_error (_("Failed to create the cddb connection."));
            else
            {
                AUDDBG ("getting CDDB info\n");

                cddb_cache_enable (pcddb_conn);
                // cddb_cache_set_dir(pcddb_conn, "~/.cddbslave");

                String server = aud_get_str ("CDDA", "cddbserver");
                String path = aud_get_str ("CDDA", "cddbpath");
                int port = aud_get_int ("CDDA", "cddbport");

                if (aud_get_bool (nullptr, "use_proxy"))
                {
                    String prhost = aud_get_str (nullptr, "proxy_host");
                    int prport = aud_get_int (nullptr, "proxy_port");
                    String pruser = aud_get_str (nullptr, "proxy_user");
                    String prpass = aud_get_str (nullptr, "proxy_pass");

                    cddb_http_proxy_enable (pcddb_conn);
                    cddb_set_http_proxy_server_name (pcddb_conn, prhost);
                    cddb_set_http_proxy_server_port (pcddb_conn, prport);
                    cddb_set_http_proxy_username (pcddb_conn, pruser);
                    cddb_set_http_proxy_password (pcddb_conn, prpass);

                    cddb_set_server_name (pcddb_conn, server);
                    cddb_set_server_port (pcddb_conn, port);
                }
                else if (aud_get_bool ("CDDA", "cddbhttp"))
                {
                    cddb_http_enable (pcddb_conn);
                    cddb_set_server_name (pcddb_conn, server);
                    cddb_set_server_port (pcddb_conn, port);
                    cddb_set_http_path_query (pcddb_conn, path);
                }
                else
                {
                    cddb_set_server_name (pcddb_conn, server);
                    cddb_set_server_port (pcddb_conn, port);
                }

                pcddb_disc = cddb_disc_new ();

                lba = cdio_get_track_lba (pcdrom_drive->p_cdio,
                                          CDIO_CDROM_LEADOUT_TRACK);
                cddb_disc_set_length (pcddb_disc, FRAMES_TO_SECONDS (lba));

                for (int trackno = firsttrackno; trackno <= lasttrackno; trackno++)
                {
                    pcddb_track = cddb_track_new ();
                    cddb_track_set_frame_offset (pcddb_track,
                                                 cdio_get_track_lba (
                                                     pcdrom_drive->p_cdio,
                                                     trackno));
                    cddb_disc_add_track (pcddb_disc, pcddb_track);
                }

                cddb_disc_calc_discid (pcddb_disc);

                unsigned discid = cddb_disc_get_discid (pcddb_disc);
                trackinfo[0].discidstr = String (str_printf("%x", discid));
                AUDINFO ("CDDB disc id = %x\n", discid);

                int matches;
                if ((matches = cddb_query (pcddb_conn, pcddb_disc)) == -1)
                {
                    if (cddb_errno (pcddb_conn) == CDDB_ERR_OK)
                        cdaudio_error (_("Failed to query the CDDB server"));
                    else
                        cdaudio_error (_("Failed to query the CDDB server: %s"),
                                       cddb_error_str (cddb_errno
                                                       (pcddb_conn)));

                    cddb_disc_destroy (pcddb_disc);
                    pcddb_disc = nullptr;
                }
                else
                {
                    if (matches == 0)
                    {
                        AUDDBG ("no cddb info available for this disc\n");

                        cddb_disc_destroy (pcddb_disc);
                        pcddb_disc = nullptr;
                    }
                    else
                    {
                        AUDDBG ("CDDB disc category = \"%s\"\n",
                               cddb_disc_get_category_str (pcddb_disc));

                        cddb_read (pcddb_conn, pcddb_disc);
                        if (cddb_errno (pcddb_conn) != CDDB_ERR_OK)
                        {
                            cdaudio_error (_("Failed to read the cddb info: %s"),
                                           cddb_error_str (cddb_errno
                                                           (pcddb_conn)));
                            cddb_disc_destroy (pcddb_disc);
                            pcddb_disc = nullptr;
                        }
                        else
                        {
                            trackinfo[0].performer = String (cddb_disc_get_artist (pcddb_disc));
                            trackinfo[0].name = String (cddb_disc_get_title (pcddb_disc));
                            trackinfo[0].genre = String (cddb_disc_get_genre (pcddb_disc));
                            trackinfo[0].tag_source = 1; // WE FETCHED FROM CDD[A|B].

                            int trackno;
                            for (trackno = firsttrackno; trackno <= lasttrackno;
                                 trackno++)
                            {
                                cddb_track_t *pcddb_track =
                                    cddb_disc_get_track (pcddb_disc,
                                                         trackno - 1);

                                trackinfo[trackno].performer = String (cddb_track_get_artist (pcddb_track));
                                trackinfo[trackno].name = String (cddb_track_get_title (pcddb_track));
                                trackinfo[trackno].genre = String (cddb_disc_get_genre (pcddb_disc));
                                trackinfo[trackno].tag_source = 1; // WE FETCHED FROM CDD[A|B].
                            }
                        }
                    }
                }
            }
        }

        if (pcddb_disc != nullptr)
            cddb_disc_destroy (pcddb_disc);

        if (pcddb_conn != nullptr)
            cddb_destroy (pcddb_conn);
    }

    return true;
}

/* mutex must be locked */
static bool refresh_trackinfo (bool warning)
{
    if (! open_cd () || ! check_disc_mode (warning))
        goto fail;

    if (! trackinfo.len () || cdio_get_media_changed (pcdrom_drive->p_cdio))
    {
        if (! scan_cd ())
            goto fail;

        timer_add (TimerRate::Hz1, monitor);
    }

    return true;

fail:
    reset_trackinfo ();
    purge_func.queue (purge_all_playlists);
    return false;
}

/* mutex must be locked */
static void reset_trackinfo ()
{
    timer_remove (TimerRate::Hz1, monitor);

    if (pcdrom_drive != nullptr)
    {
        cdda_close (pcdrom_drive);
        pcdrom_drive = nullptr;
    }

    trackinfo.clear ();
    if (aud_get_bool (nullptr, "user_tag_data"))  /* JWT:CLEAN UP USER TAG DATA FILE: */
    {
        for (int i=0; i<=n_audio_tracks; i++)
            aud_delete_tag_from_tagfile (str_printf ("%s%d", "cdda://?", i), "tmp_tag_data");
    }
    coverart_file = String ();
    custom_tagfile_sought = false;
}

/* thread safe (mutex may be locked) */
static int calculate_track_length (int startlsn, int endlsn)
{
    return ((endlsn - startlsn + 1) * 1000) / 75;
}

/* thread safe (mutex may be locked) */
static int find_trackno_from_filename (const char * filename)
{
    int track;

    if (strncmp (filename, "cdda://?", 8) || sscanf (filename + 8, "%d",
                                                     &track) != 1)
        return -1;

    return track;
}
