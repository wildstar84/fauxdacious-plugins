/*

 LOOK AT:  http://svn.tribler.org/vlc/trunk/modules/access/dvdnav.c

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
#include <time.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
extern "C" {
#include <fcntl.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/cdrom.h>
#include <unistd.h>
}
#define SDL_MAIN_HANDLED
#include <SDL.h>

// #define NODEMUXING

#undef FFAUDIO_DOUBLECHECK  /* Doublecheck probing result for debugging purposes */
#undef FFAUDIO_NO_BLACKLIST /* Don't blacklist any recognized codecs/formats */
// #define RAW_PACKET_BUFFER_SIZE 32768

#include "ffaudio-stdinc.h"

/* prevent libcdio from redefining PACKAGE, VERSION, etc. */
#define EXTERNAL_LIBDVDNAV_CONFIG_H

#include <dvdnav/dvdnav.h>
#include <dvdnav/dvdnav_events.h>

#include <libaudcore/audstrings.h>
#include <libaudcore/hook.h>
#include <libaudcore/i18n.h>
#include <libaudcore/interface.h>
#include <libaudcore/mainloop.h>
#include <libaudcore/playlist.h>
#include <libaudcore/plugin.h>
#include <libaudcore/preferences.h>
#include <libaudcore/multihash.h>
#include <libaudcore/runtime.h>
#include <libaudcore/drct.h>

#define MIN_DISC_SPEED 2
#define MAX_DISC_SPEED 24

#define MAX_RETRIES 10
#define MAX_SKIPS 10
#define WANT_VFS_STDIO_COMPAT
#define IOBUF 4096

#define FFMAX(a,b) ((a) > (b) ? (a) : (b))
#define FFMIN(a,b) ((a) > (b) ? (b) : (a))
#define FFMINMAX(c,a,b) FFMIN(FFMAX(c, a), b)

#if CHECK_LIBAVFORMAT_VERSION (57, 33, 100, 57, 5, 0)
#define ALLOC_CONTEXT 1
#endif

#if CHECK_LIBAVCODEC_VERSION (57, 37, 100, 57, 16, 0)
#define SEND_PACKET 1
#endif

#if CHECK_LIBAVCODEC_VERSION (55, 25, 100, 55, 16, 0)
#define av_free_packet av_packet_unref
#endif

typedef struct
{
    int stream_idx;
    AVStream * stream;
    AVCodecContext * context;  // JWT:ADDED
    AVCodec * codec;
}
CodecInfo;

struct ScopedFrame
{
#if CHECK_LIBAVCODEC_VERSION (55, 45, 101, 55, 28, 1)
    AVFrame * ptr = av_frame_alloc ();
#else
    AVFrame * ptr = avcodec_alloc_frame ();
#endif

    AVFrame * operator-> () { return ptr; }

#if CHECK_LIBAVCODEC_VERSION (55, 45, 101, 55, 28, 1)
    ~ScopedFrame () { av_frame_free (& ptr); }
#elif CHECK_LIBAVCODEC_VERSION (54, 59, 100, 54, 28, 0)
    ~ScopedFrame () { avcodec_free_frame (& ptr); }
#else
    ~ScopedFrame () { av_free (ptr); }
#endif
};

static const char * const dvd_schemes[] = {"dvd", nullptr};

class DVD : public InputPlugin
{
public:
    static const char about[];
    static const char * const defaults[];
    static const PreferencesWidget widgets[];
    static const PluginPreferences prefs;

    static constexpr PluginInfo info = {
        N_("DVD Plugin"),
        PACKAGE,
        about,
        & prefs
    };

    constexpr DVD () : InputPlugin (info, InputInfo (FlagSubtunes)
        .with_schemes (dvd_schemes)) {}

    bool init ();
    void cleanup ();

    bool is_our_file (const char * filename, VFSFile & file);
    bool read_tag (const char * filename, VFSFile & file, Tuple & tuple, Index<char> * image);
    bool play (const char * filename, VFSFile & file);
    void write_audioframe (CodecInfo * cinfo, AVPacket * pkt, int out_fmt, bool planar);
    void write_videoframe (SDL_Renderer * renderer, CodecInfo * vcinfo, 
            SDL_Texture * bmp, AVPacket *pkt, int video_width, 
            int video_height, bool * last_resized, bool * videohasnowh, bool * windowIsStable,
            int * resized_window_width, int * resized_window_height);
    void draw_highlight_buttons (SDL_Renderer * renderer);
private:
    AVFormatContext * open_input_file (struct pollfd * input_fd_p);
    void reader_demuxer ();
    static void * reader_thread (void * data)
    { 
        ((DVD *) data)->reader_demuxer ();
        AUDINFO ("READER_DEMUXER DONE!!!\n");
        return nullptr;
    }
};

EXPORT DVD aud_plugin_instance;

typedef struct
{
    String performer;
    String name;
    String genre;
    uint32_t startlsn;
    uint32_t endlsn;
}
trackinfo_t;

typedef struct {
  dvdnav_t *       dvdnav;              /* handle to libdvdnav stuff */
  char *           filename;            /* path */
  unsigned int     duration;            /* in milliseconds */
  //int              mousex, mousey;
  int              title;
  int              track;
  unsigned int     spu_clut[16];
  //1 uint8_t  palette[4][4];
  dvdnav_highlight_event_t hlev;
  int              still_length;        /* still frame duration */
  unsigned int     state;
  int64_t          end_pos;
  int64_t          pos;
  bool             seek;
  const char *     title_str;
  String           fifo_str;
  bool             wakeup;
  uint16_t         langid;
  bool             beenheredonedat;     /* used to avoid circular menus */
  bool             nochannelhop;        /* don't do channel hop whilst sliding the seek slider */
  int              lastaudiostream;     /* save last audio stream to see if we need to change codecs */
} dvdnav_priv_t;

typedef enum {
  NAV_FLAG_EOF                  = 1 << 0,  /* end of stream has been reached */
  NAV_FLAG_WAIT                 = 1 << 1,  /* wait event */
  NAV_FLAG_WAIT_SKIP            = 1 << 2,  /* wait skip disable */
  NAV_FLAG_CELL_CHANGE          = 1 << 3,  /* cell change event */
  NAV_FLAG_WAIT_READ_AUTO       = 1 << 4,  /* wait read auto mode */
  NAV_FLAG_WAIT_READ            = 1 << 5,  /* suspend read from stream */
  NAV_FLAG_VTS_DOMAIN           = 1 << 6,  /* vts domain */
  NAV_FLAG_SPU_SET              = 1 << 7,  /* spu_clut is valid */
  NAV_FLAG_STREAM_CHANGE        = 1 << 8,  /* title, chapter, audio or SPU */
  NAV_FLAG_AUDIO_CHANGE         = 1 << 9,  /* audio stream change event */
  NAV_FLAG_SPU_CHANGE           = 1 << 10, /* spu stream change event */
} dvdnav_state_t;

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static bool playing;
static bool play_video;  /* JWT: TRUE IF USER IS CURRENTLY PLAYING VIDEO (KILLING VID. WINDOW TURNS OFF)! */
static bool stop_playback;   /* SIGNAL FROM USER TO STOP PLAYBACK */
static bool playback_thread_running;  /* TRUE IF READER/DEMUXER THREAD IS UP AND RUNNING */
static bool playback_fifo_hasbeenopened; /* TRUE IF FIFO IS SUCCESSFULLY OPENED */
static bool reader_please_die;  /* SIGNAL READER/DEMUXER THREAD TO TERMINATE */
static bool playing_a_menu;     /* TRUE IF WE'RE PLAYING A "MENU" (VS. A "MOVIE") */
static bool checkcodecs;        /* SIGNAL THAT WE NEED TO RELOAD THE CODECS (TRACK CHANGE, ETC.) */
static bool readblock;          /* PREVENT READER/DEMUXER THREAD FROM CONTINUING UNTIL DATA READY TO READ */
static bool writeblock;         /* PREVENT DVD ENGINE FROM WRITING DATA UNTIL READER READY (DONE DRAINING) */
static FILE * output_fd;        /* OUTPUT FILE-HANDLE TO FIFO */
static Index<SDL_Rect> menubuttons;  /* ARRAY OF MENUBUTTONS (EACH HAS 2 SETS OF X.Y COORDS. */
static bool havebuttons;        /* SIGNALS THAT WE HAVE FETCHED MENU-BUTTONS FOR THE CURRENT MENU */

/* lock mutex to read / set these variables */
static int firsttrackno = -1;
static int lasttrackno = -1;
static dvdnav_priv_t * dvdnav_priv = nullptr;

static Index<trackinfo_t> trackinfo;
static QueuedFunc purge_func;

static bool scan_dvd ();
static bool refresh_trackinfo (bool warning);
static void reset_trackinfo ();
static int calculate_track_length (uint32_t startlsn, uint32_t endlsn);
static int find_trackno_from_filename (const char * filename);

const char DVD::about[] =
 N_("DVD-player Plugin\n\n"
    "Copyright (C) 2017 Jim Turner <turnerjw784@yahoo.com>.\n\n"
    "Many thanks to libdvdnav developers <http://www.gnu.org/software/libdvdnav/>.");

const char * const DVD::defaults[] = {
    "disc_speed", "2",
    "maxopentries", "4",
    "use_cdtext", "TRUE",
    "device", "/dev/dvd",
    "video_qsize", "6",
    "play_video", "TRUE",
    "highlight_buttons", "TRUE",
    /* setting menudrain FLUSHES UNWRITTEN PACKETS WHEN MENU BUTTON PRESSED AND ADVANCING TO ANOTHER
       MENU.  DEFAULT IS TO NOT DRAIN UNWRITTEN PACKETS, SINCE DOING SO OFTEN RESULTS IN INCOMPLETE 
       RENDERING OF MENU IMAGES.  MY SUSPICION IS THAT THERE'S A DELAY BETWEEN THE DVD ENGINE AND 
       OUR READER-DEMUXER THREAD SUCH THAT WHILE WE'RE STILL IN A MENU, LIBDVDNAV'S ALREADY IN THE 
       NEXT FEATURE.  THE ONLY DESIRABLE THING ABOUT SETTING THIS IS THAT THE FEW EXTRA PACKETS 
       (THE AUDIO ONES IN PARTICULAR) SEEM TO GET WRITTEN OUT TO THE SPEAKERS VERY FAST (SO THAT
       THERE'S A SLIGHTLY ANNOYING LESS THAN A SECOND OF AUDIO BETWEEN THE BUTTON PRESS AND WHEN 
       THE NEXT SELECTED MENU / MOVIE COMES UP (IF THE MENU WITH THE BUTTONS HAS AUDIO) AND SETTING 
       THIS AVOIDS THAT, BUT AT THE RISK OF CRASHING FAUXDACIOUS WITH AN "ASSERT 0" FROM LIBDVDNAV!
       SOME DVDS ALSO SEEM TO NEED THIS IN ORDER TO THINK THAT THE MOVIE FEATURE HAS BEEN SELECTED 
       FROM THE MENU -VS- BEING PLAYED DIRECTLY SO THAT THEY WILL TAKE YOU TO IT PROPERLY (SKIPPING 
       PREVIEWS, ADS, ETC. AND GETTING THE LANGUAGE SET CORRECTLY.
    */
    "menudrain", "FALSE",
    "nomoviedrain", "FALSE",
    "use_cdtext", "TRUE",
    "title_track_only", "FALSE",  // ONLY ADD TITLE TRACK TO PLAYLIST (IF BOTH FALSE, ADD ALL TRACKS TO PLAYLIST)
    "first_track_only", "FALSE",  // ONLY ADD 1ST (MOVIE) TRACK TO PLAYLIST
    "nomenus", "FALSE",           // SKIP MENUS ALWAYS AUTO-SELECTING THE FIRST BUTTON
    nullptr
};

const PreferencesWidget DVD::widgets[] = {
    WidgetLabel (N_("<b>Device</b>")),
    WidgetSpin (N_("Read speed:"),
        WidgetInt ("dvd", "disc_speed"),
        {MIN_DISC_SPEED, MAX_DISC_SPEED, 1}),
    WidgetSpin (N_("Max Tries to Open Disk:"),
        WidgetInt ("dvd", "maxopentries"),
        {1, 10, 1}),
    WidgetEntry (N_("Override device:"),
        WidgetString ("dvd", "device")),
    WidgetSpin (N_("Video packet queue size"),
        WidgetInt ("dvd", "video_qsize"), {0, 24, 1}),
    WidgetCheck (N_("Play video stream in popup window when video stream found"),
        WidgetBool ("dvd", "play_video")),
    WidgetCheck (N_("Highlight menu buttons (with a rectangle)"),
        WidgetBool ("dvd", "highlightbuttons")),
    WidgetCheck (N_("Drain unwritten packets before menus (may smudge menus)"),
        WidgetBool ("dvd", "menudrain")),
    WidgetCheck (N_("Don't drain unwritten packets before movies (extra noise)"),
        WidgetBool ("dvd", "nomoviedrain")),
    WidgetLabel (N_("<b>Metadata</b>")),
    WidgetCheck (N_("Use CD-Text"),
        WidgetBool ("dvd", "use_cdtext")),
    WidgetCheck (N_("Only Title Track in Playlist (menus)"),
        WidgetBool ("dvd", "title_track_only")),
    WidgetCheck (N_("Only 1st Track Playlist (movie)"),
        WidgetBool ("dvd", "first_track_only")),
    WidgetCheck (N_("Skip Menus (Auto-select)"),
        WidgetBool ("dvd", "nomenus")),
};

const PluginPreferences DVD::prefs = {{widgets}};

/* from audacious:  DISPLAY MESSAGE IN A POPUP WINDOW */
static void dvd_error (const char * message_format, ...)
{
    va_list args;
    va_start (args, message_format);
    StringBuf msg = str_vprintf (message_format, args);
    va_end (args);

    aud_ui_show_error (msg);
}

/* from audacious */
static int log_result (const char * func, int ret)
{
    if (ret < 0 && ret != (int) AVERROR_EOF && ret != AVERROR (EAGAIN))
    {
        static char buf[256];
        if (! av_strerror (ret, buf, sizeof buf))
            AUDERR ("%s failed: %s\n", func, buf);
        else
            AUDERR ("%s failed\n", func);
    }

    return ret;
}

#define LOG(function, ...) log_result (#function, function (__VA_ARGS__))

/* from audacious */
static void ffaudio_log_cb (void * avcl, int av_level, const char * fmt, va_list va)
{
    audlog::Level level = audlog::Debug;
    char message [2048];

    switch (av_level)
    {
    case AV_LOG_QUIET:
        return;
    case AV_LOG_PANIC:
    case AV_LOG_FATAL:
    case AV_LOG_ERROR:
        level = audlog::Error;
        break;
    case AV_LOG_WARNING:
        level = audlog::Warning;
        break;
    case AV_LOG_INFO:
        level = audlog::Info;
        break;
    default:
        break;
    }

    AVClass * avc = avcl ? * (AVClass * *) avcl : nullptr;

    vsnprintf (message, sizeof message, fmt, va);

    audlog::log (level, __FILE__, __LINE__, avc ? avc->item_name (avcl) : __FUNCTION__,
            "<%p> %s", avcl, message);
}

/* from audacious - main thread only */
static void purge_playlist (int playlist)
{
    int length = aud_playlist_entry_count (playlist);

    for (int count = 0; count < length; count ++)
    {
        String filename = aud_playlist_entry_get_filename (playlist, count);

        if (! strncmp (filename, "dvd://", 6))
        {
            aud_playlist_entry_delete (playlist, count, 1);
            count--;
            length--;
        }
    }
}

/* from audacious - main thread only */
static void purge_all_playlists (void * = nullptr)
{
    int playlists = aud_playlist_count ();
    int count;

    for (count = 0; count < playlists; count++)
        purge_playlist (count);
}

/* from audacious - main thread only */
bool DVD::init ()
{
    aud_config_set_defaults ("DVD", defaults);

    av_register_all();
    av_log_set_callback (ffaudio_log_cb);

    return true;
}

/* from audacious - thread safe (mutex may be locked) */
bool DVD::is_our_file (const char * filename, VFSFile & file)
{
    return !strncmp (filename, "dvd://", 6);
}

// from mplayer.stream_dvdnav.c:
static int dvdnav_stream_read (dvdnav_priv_t * priv, unsigned char *buf, int *len)
{
    int event = DVDNAV_NOP;

    *len=-1;
    if (dvdnav_get_next_block(priv->dvdnav,buf,&event,len)!=DVDNAV_STATUS_OK)
    {
        AUDERR ("Error getting next block from DVD %d (%s)\n",event, dvdnav_err_to_string (priv->dvdnav));
        *len=-1;
    }
    else if (event != DVDNAV_BLOCK_OK && event != DVDNAV_NAV_PACKET)
        *len = 0;

    return event;
}

// from mplayer.stream_dvdnav.c:
static void update_title_len () 
{
    dvdnav_status_t status;
    uint32_t pos = 0, len = 0;

    status = dvdnav_get_position(dvdnav_priv->dvdnav, &pos, &len);
    if (status == DVDNAV_STATUS_OK && len) {
        dvdnav_priv->end_pos = len;
        dvdnav_priv->seek = true;
        AUDDBG ("update_title_len(OK): POS=%d= LEN=%d= END=%ld=\n", pos, len, dvdnav_priv->end_pos);
    }
    else
    {
        dvdnav_priv->end_pos = 0;
        dvdnav_priv->seek = false;
        AUDDBG ("update_title_len(NOT OK): POS=%d= LEN=%d=\n", pos, len);
    }
}

static inline int dvdnav_get_duration (int length) {
  return (length == 255) ? 0 : length * 1000;
}

/* ---------
// from mplayer.stream_dvdnav.c: display languages of the dvd streams found (NOT avcodec streams!)
static void show_audio_subs_languages(dvdnav_t *nav)
{
  uint8_t lg;
  uint16_t i, lang, format, id, channels;
  int base[7] = {128, 0, 0, 0, 160, 136, 0};
  for(i=0; i<8; i++)
  {
    char tmp[] = "unknown";
    lg = dvdnav_get_audio_logical_stream(nav, i);
    if(lg == 0xff) continue;
    channels = dvdnav_audio_stream_channels(nav, lg);
    if(channels == 0xFFFF)
      channels = 2; //unknown
    else
      channels--;
    lang = dvdnav_audio_stream_to_lang(nav, lg);
    audio_attr_t audio_attr;
    dvdnav_get_audio_attr(nav, i, &audio_attr);
    if(lang != 0xFFFF)
    {
      tmp[0] = lang >> 8;
      tmp[1] = lang & 0xFF;
      tmp[2] = 0;
    }
    format = dvdnav_audio_stream_format(nav, lg);
    if(format == 0xFFFF || format > 6)
      format = 1; //unknown
    id = i + base[format];
    if (lang != 0xFFFF && lang && tmp[0])
        AUDINFO ("ID_%d_AID_%d_LANG=%s code=%d\n", lg, id, tmp, audio_attr.lang_code);
  }
  for(i=0; i<32; i++)
  {
    char tmp[] = "unknown";
    lg = dvdnav_get_spu_logical_stream(nav, i);
    if(lg == 0xff) continue;
    lang = dvdnav_spu_stream_to_lang(nav, i);
    if(lang != 0xFFFF)
    {
      tmp[0] = lang >> 8;
      tmp[1] = lang & 0xFF;
      tmp[2] = 0;
    }
    if (lang != 0xFFFF && lang && tmp[0])
        AUDINFO ("ID_SID_%d_LANG=%s\n", lg, tmp);
  }
}
---------- */

// from mplayer.stream_dvdnav.c:
int mp_dvdtimetomsec(dvd_time_t *dt)
{
  static int framerates[4] = {0, 2500, 0, 2997};
  int framerate = framerates[(dt->frame_u & 0xc0) >> 6];
  int msec = (((dt->hour & 0xf0) >> 3) * 5 + (dt->hour & 0x0f)) * 3600000;
  msec += (((dt->minute & 0xf0) >> 3) * 5 + (dt->minute & 0x0f)) * 60000;
  msec += (((dt->second & 0xf0) >> 3) * 5 + (dt->second & 0x0f)) * 1000;
  if(framerate > 0)
    msec += (((dt->frame_u & 0x30) >> 3) * 5 + (dt->frame_u & 0x0f)) * 100000 / framerate;
  return msec;
}

// from mplayer.stream_dvdnav.c:
static void dvdnav_get_highlight (dvdnav_priv_t *priv, int display_mode)
{
    pci_t *pnavpci = NULL;
    dvdnav_highlight_event_t *hlev = &(priv->hlev);
    int32_t btnum;

    if (!priv || !priv->dvdnav)
        return;

    pnavpci = dvdnav_get_current_nav_pci (priv->dvdnav);
    if (!pnavpci)
        return;

    dvdnav_get_current_highlight (priv->dvdnav, (int32_t*)&(hlev->buttonN));
    hlev->display = display_mode; /* show */

    if (hlev->buttonN > 0 && pnavpci->hli.hl_gi.btn_ns > 0 && hlev->display)
    {
//1:from: vlc/modules/access/dvdnav.c:1253l (may use someday when we figure out how):
//1        bool b_mode = (display_mode ? true : false);
//1        dvdnav_highlight_area_t hl;
//1        bool b_button_ok = DVDNAV_STATUS_OK ==
//1                  dvdnav_get_highlight_area (pnavpci, hlev->buttonN, b_mode, &hl);
//1        if( b_button_ok )
//1        {
//1            for( unsigned i = 0; i < 4; i++ )
//1            {
//1                uint32_t i_yuv = priv->spu_clut[(hl.palette>>(16+i*4))&0x0f];
//1                uint8_t i_alpha = ( (hl.palette>>(i*4))&0x0f ) * 0xff / 0xf;

//1                priv->palette[i][0] = (i_yuv >> 16) & 0xff;
//1                priv->palette[i][1] = (i_yuv >> 0) & 0xff;
//1                priv->palette[i][2] = (i_yuv >> 8) & 0xff;
//1                priv->palette[i][3] = i_alpha;
//1            }
//1        }
        AUDDBG ("dvdnav_get_highlight: BUTTON=%d, SHOW=%d HAVE=%d\n", hlev->buttonN, hlev->display, pnavpci->hli.hl_gi.btn_ns);
        for (btnum = 0; btnum < pnavpci->hli.hl_gi.btn_ns; btnum++)
        {
            btni_t *btni = &(pnavpci->hli.btnit[btnum]);

            if ((int32_t)hlev->buttonN == (btnum + 1))
            {
                hlev->sx = FFMIN (btni->x_start, btni->x_end);
                hlev->ex = FFMAX (btni->x_start, btni->x_end);
                hlev->sy = FFMIN (btni->y_start, btni->y_end);
                hlev->ey = FFMAX (btni->y_start, btni->y_end);

                hlev->palette = (btni->btn_coln == 0) ? 0 : pnavpci->hli.btn_colit.btn_coli[btni->btn_coln - 1][0];
                break;
            }
        }
    } 
    else  /* hide button or no button */
    {
        hlev->sx = hlev->ex = 0;
        hlev->sy = hlev->ey = 0;
        hlev->palette = hlev->buttonN = 0;
    }
}

// from audacious:
static void close_input_file (AVFormatContext * c)
{
    AUDINFO ("-close_input_file.\n");

    if (c)
    {
        AVIOContext * io = c->pb;
#if CHECK_LIBAVFORMAT_VERSION (53, 25, 0, 53, 17, 0)
        avformat_close_input (&c);
#else
        av_close_input_file (c);
#endif
        avformat_free_context (c);
        if (io)
        {
            //io_context_free (io);
            av_free (io->buffer);
            av_free (io);
        }
    }
}

// from audacious:
static bool convert_format (int ff_fmt, int & aud_fmt, bool & planar)
{
    switch (ff_fmt)
    {
        case AV_SAMPLE_FMT_U8: aud_fmt = FMT_U8; planar = false; break;
        case AV_SAMPLE_FMT_S16: aud_fmt = FMT_S16_NE; planar = false; break;
        case AV_SAMPLE_FMT_S32: aud_fmt = FMT_S32_NE; planar = false; break;
        case AV_SAMPLE_FMT_FLT: aud_fmt = FMT_FLOAT; planar = false; break;

        case AV_SAMPLE_FMT_U8P: aud_fmt = FMT_U8; planar = true; break;
        case AV_SAMPLE_FMT_S16P: aud_fmt = FMT_S16_NE; planar = true; break;
        case AV_SAMPLE_FMT_S32P: aud_fmt = FMT_S32_NE; planar = true; break;
        case AV_SAMPLE_FMT_FLTP: aud_fmt = FMT_FLOAT; planar = true; break;

    default:
        AUDERR ("Unsupported audio format %d\n", (int) ff_fmt);
        return false;
    }

    return true;
}

/* from audacious:  mutex must be locked */
static bool check_disk_status ()
{
    if (! dvdnav_priv)
        return false;

    int disk;
    if ((disk = open (dvdnav_priv->filename, O_RDONLY | O_NONBLOCK)) < 0)
        return false;

    int diskstatus = ioctl (disk, CDROM_DRIVE_STATUS);
    return (! diskstatus || diskstatus == CDS_DISC_OK);
}

// blits each video frame to the screen:
void DVD::write_videoframe (SDL_Renderer * renderer, CodecInfo * vcinfo, 
        SDL_Texture * bmp, AVPacket *pkt, int video_width, 
        int video_height, bool * last_resized, bool * videohasnowh, bool * windowIsStable, 
        int * resized_window_width, int * resized_window_height)
{
#ifdef SEND_PACKET
    if ((LOG (avcodec_send_packet, vcinfo->context, pkt)) < 0)
        return;
#else
    int subframeCnt = 0;
    int frameFinished = 0;
    int len = 0;
    while (subframeCnt < 16)
    {
#endif
        ScopedFrame vframe;
#ifdef SEND_PACKET
        int res = avcodec_receive_frame (vcinfo->context, vframe.ptr);
        if (res < 0)
        {
            if (res == AVERROR(EAGAIN) &&  playing_a_menu)
            {
                AUDDBG ("w:RECEIVE FRAME FAIL, FLUSHING BUFFERS: %d\n", res);
                avcodec_send_packet(vcinfo->context, nullptr);
                res = avcodec_receive_frame (vcinfo->context, vframe.ptr);
                avcodec_flush_buffers(vcinfo->context);
            }
            if (res < 0)
                return; /* read next packet (continue past errors) */
        }
        else if (! pkt->size)
            avcodec_flush_buffers(vcinfo->context);
#else
        frameFinished = 0;
        len = LOG (avcodec_decode_video2, vcinfo->context, vframe.ptr, &frameFinished, pkt);
        /* Did we get a video frame? */
        if (len < 0)
        {
            AUDERR ("e:decode_video() failed, code %d\n", len);
            return;
        }
        if (frameFinished)
        {
#endif
            if (last_resized)  /* BLIT THE FRAME, BUT ONLY IF WE'RE NOT CURRENTLY RESIZING THE WINDOW! */
            {
                if (*videohasnowh && vframe.ptr->width && vframe.ptr->height)
                {
                    /* Resize the screen. */
                    *resized_window_width = vframe.ptr->width;  // window's reported new size
                    *resized_window_height = vframe.ptr->height;
                    AUDERR ("w:videohasnowh! - SDL_RESIZE(FRAME)!!!!!! rvw=(%d, %d) orig(%d, %d)\n", *resized_window_width, *resized_window_height, video_width, video_height);
                    *last_resized = false;  // false means now we'll need re-aspecting, so stop blitting!
                    //*videohasnowh = false;
                }
                else
                {
                    SDL_UpdateYUVTexture (bmp, nullptr, vframe->data[0], vframe->linesize[0], 
                        vframe->data[1], vframe->linesize[1], vframe->data[2], vframe->linesize[2]);
                    SDL_RenderCopy (renderer, bmp, nullptr, nullptr);  // USE NULL TO GET IMAGE TO FIT WINDOW!
                    SDL_RenderPresent (renderer);
                    (*windowIsStable) = true;
                }
            }
            return;
#ifndef SEND_PACKET
        }
        else
        {
            if (pkt->size <= 0 || pkt->data < 0)
                return;
            pkt->size -= len;
            pkt->data += len;
            if (pkt->size <= 0)
                return;
        }
        ++subframeCnt;
    }
    AUDERR ("w:write_videoframe: runaway frame skipped (more than 16 parts)\n");
    return;
#endif
}

// DRAW A RECTANGLE AROUND EACH MENU BUTTON (USER-OPTION, SINCE WE DON'T CURRENTLY DO SUBPICTURES):
void DVD::draw_highlight_buttons (SDL_Renderer * renderer)
{
    for (int mbtn = 0; mbtn < menubuttons.len (); mbtn ++)
    {
        SDL_RenderDrawRect (renderer, & menubuttons[mbtn]);
    }
    SDL_RenderPresent (renderer); 
}

// CREATE AN SDL "RENDERER":
static SDL_Renderer * createSDL2Renderer (SDL_Window * screen, bool myplay_video)
{
    SDL_Renderer * renderer = nullptr;
    if (screen && myplay_video)
        renderer = SDL_CreateRenderer (screen, -1, 0);

    return renderer;
}

// CREATE AN SDL "TEXTURE":
static SDL_Texture * createSDL2Texture (SDL_Window * screen, SDL_Renderer * renderer, bool myplay_video, int width, int height)
{
    SDL_Texture * texture = nullptr;
    if (myplay_video && renderer)
    {
        texture = SDL_CreateTexture (renderer, SDL_PIXELFORMAT_YV12, SDL_TEXTUREACCESS_STREAMING, width, height);
        if (! texture)
            AUDERR ("e:Could not create texture (%s)\n", SDL_GetError ());
        else
        {
            SDL_SetRenderDrawColor(renderer, 128, 128, 128, 255);
            SDL_RenderFillRect (renderer, nullptr);
            SDL_RenderPresent (renderer);
            SDL_ShowWindow (screen);
        }
    }

    return texture;
}

/* WHEN EXITING PLAY, WE SAVE THE WINDOW-POSITION & SIZE SO WINDOW CAN POP UP IN SAME POSITION NEXT TIME! */
void save_window_xy (SDL_Window * screen, int video_fudge_x, int video_fudge_y)
{
    int x, y, w, h;

    SDL_GetWindowPosition (screen, &x, &y);
    SDL_GetWindowSize (screen, &w, &h);
    x += video_fudge_x;  /* APPLY CALCULATED FUDGE-FACTOR */
    if (x < 0)
        x = 1;
    y += video_fudge_y;
    if (y < 0)
        y = 1;
    aud_set_int ("dvd", "video_window_x", x);
    aud_set_int ("dvd", "video_window_y", y);
    aud_set_int ("dvd", "video_window_w", w);
    aud_set_int ("dvd", "video_window_h", h);
}

// SEND EACH AUDIO FRAME TO THE SPEAKERS:
void DVD::write_audioframe (CodecInfo * cinfo, AVPacket * pkt, int out_fmt, bool planar)
{
    int size = 0;
    Index<char> buf;
#ifdef SEND_PACKET
    if ((LOG (avcodec_send_packet, cinfo->context, pkt)) < 0)
        return;
#else
    int decoded = 0;
    int len = 0;
#endif

    while (pkt->size > 0)
    {
        ScopedFrame frame;
#ifdef SEND_PACKET
        if ((LOG (avcodec_receive_frame, cinfo->context, frame.ptr)) < 0)
            break; /* read next packet (continue past errors) */
#else
        decoded = 0;
        len = LOG (avcodec_decode_audio4, cinfo->context, frame.ptr, & decoded, pkt);
        if (len < 0)
        {
            AUDERR ("e:decode_audio() failed, code %d\n", len);
            break;
        }

        pkt->size -= len;
        pkt->data += len;

        if (! decoded)
        {
            if (pkt->size > 0)
                continue; /* process more of current packet */

            break;
        }
#endif
        size = FMT_SIZEOF (out_fmt) * cinfo->context->channels * frame->nb_samples;
        if (planar)
        {
            if (size > buf.len ())
                buf.resize (size);

            audio_interlace ((const void * *) frame->data, out_fmt,
                    cinfo->context->channels, buf.begin (), frame->nb_samples);
            write_audio (buf.begin (), size);
        }
        else
            write_audio (frame->data[0], size);
    }
    return;
}

/* 
    JWT: ADDED ALL THIS QUEUE STUFF TO SMOOTH VIDEO PERFORMANCE SO THAT VIDEO FRAMES WOULD 
    BE OUTPUT MORE INTERLACED WITH THE AUDIO FRAMES BY QUEUEING VIDEO FRAMES UNTIL AN 
    AUDIO FRAME IS PROCESSED, THEN DEQUEUEING AND PROCESSING 'EM WITH EACH AUDIO FRAME.  
    THE SIZE OF THIS QUEUE IS SET BY video_qsize CONFIG PARAMETER AND DEFAULTS TO 8.
    HAVING TOO MANY CAN RESULT IN DELAYED VIDEO, SO EXPERIMENT.  IDEALLY, PACKETS SHOULD 
    BE PROCESSED:  V A V A V A..., BUT THIS HANDLES:  
    V1 V2 V3 V4 V5 A1 A2 A3 A4 A5 A6 A7 V7 A8... AS: 
    (q:V1 V2 V3 V4 V5 V6) A1 A2 dq:V1 A3 A4 dq:V2 A5 A6 dq:V3 A7 A8...
    WE DON'T WANT TO INTERRUPT AUDIO PERFORMANCE AND I DON'T KNOW HOW TO THREAD IT UP,
    BUT THIS SIMPLE APPROACH SEEMS TO WORK PRETTY SMOOTH FOR ME!  OTHERWISE TRY 
    INCREASING video_qsize IN config file OTHERWISE.
    BORROWED THESE FUNCTIONS FROM:
    http://www.thelearningpoint.net/computer-science/data-structures-queues--with-c-program-source-code
*/

typedef struct 
{
    int capacity;
    int size;
    int front;
    int rear;
    AVPacket *elements;
}
pktQueue;

pktQueue * createQueue (int maxElements)
{
    /* Create a Queue */
    pktQueue *Q;
    Q = (pktQueue *) malloc (sizeof (pktQueue));
    /* Initialise its properties */
    Q->elements = (AVPacket *) malloc (sizeof (AVPacket) * maxElements);
    Q->size = 0;
    Q->capacity = maxElements;
    Q->front = 0;
    Q->rear = -1;
    /* Return the pointer */
    return Q;
}

bool Dequeue (pktQueue *Q)
{
    /* If Queue size is zero then it is empty. So we cannot pop */
    if (! Q->size)
        return false;
    /* Removing an element is equivalent to incrementing index of front by one */
    else
    {
        Q->size--;
        if (Q->elements[Q->front].data)
            av_free_packet (&Q->elements[Q->front]);

        Q->front++;
        /* As we fill elements in circular fashion */
        if (Q->front == Q->capacity)
            Q->front = 0;
    }
    return true;
}

/* JWT:FLUSH AND FREE EVERYTHING IN THE QUEUE */
void QFlush (pktQueue *Q)
{
    while (Q->size > 0)
    {
        Q->size--;
        if (Q->elements[Q->front].data)
            av_free_packet (&Q->elements[Q->front]);

        Q->front++;
        /* As we fill elements in circular fashion */
        if (Q->front == Q->capacity)
            Q->front = 0;
    }

}

bool Enqueue (pktQueue *Q, AVPacket element)
{
    /* If the Queue is full, we cannot push an element into it as there is no space for it.*/
    if (Q->size == Q->capacity)
    {
        //AUDDBG ("Queue is Full\n");
        return false;
    }
    else
    {
        Q->size++;
        Q->rear += 1;
        /* As we fill the queue in circular fashion */
        if (Q->rear == Q->capacity)
            Q->rear = 0;
        /* Insert the element in its rear side */ 
        Q->elements[Q->rear] = element;
    }
    return true;
}

void destroyQueue (pktQueue *Q)
{
    QFlush (Q);
    free (Q->elements);
    free (Q);
    Q = nullptr;
}

/* JWT:END OF ADDED VIDEO PACKET QUEUEING FUNCTIONS */

static int read_cb (void * input_fd_p, unsigned char * buf, int size)
{
    int red = 0;
    //if (readblock) AUDERR("BLOCKING! READ_CB CALLED!\n"); else AUDERR("READ_CB CALLED!\n");
readagain:
    while (poll ((struct pollfd *)input_fd_p, 1, 200) <= 0)
    {
        //if (readblock) AUDERR("----(BLOCK) READ WAITING ON POLL...\n"); else AUDERR("----(nonblocking) READ WAITING ON POLL...\n");
        if (!readblock || reader_please_die)
            return -1;
    }
    red = read (((struct pollfd *)input_fd_p)->fd, buf, size);
    if (! red && readblock && !reader_please_die)
        goto readagain;

    //AUDERR("--READ(%d) BYTES (sz=%d)\n", red, size);
    return (reader_please_die ? -1 : red);
}

static bool adjust_menubuttons (uint32_t old_width, uint32_t new_width, uint32_t old_height, uint32_t new_height)
{
    bool adjusted = false;
    if (new_width != old_width)
    {
        float f;
        for (int mbtn = 0; mbtn < menubuttons.len (); mbtn ++)
        {
//            if (mbtn == 0)
                AUDDBG ("---- xBEF(%d): x=%d, w=%d;  Ovw=%d, Nvw=%d\n", mbtn, menubuttons[mbtn].x, menubuttons[mbtn].w, old_width, new_width);
            f = (float)new_width / (float)old_width;
            menubuttons[mbtn].w = (int)(f * (float)menubuttons[mbtn].w);
            menubuttons[mbtn].x = (int)(f * (float)menubuttons[mbtn].x);
//            if (mbtn == 0)
                AUDDBG ("---- xAFT(%d): x=%d, w=%d;  f=%.2f\n", mbtn, menubuttons[mbtn].x, menubuttons[mbtn].w, f);
        }
        adjusted = true;
    }
    if (new_height != old_height)
    {
        float f;
        for (int mbtn = 0; mbtn < menubuttons.len (); mbtn ++)
        {
//            if (mbtn == 0)
                AUDDBG ("---- yBEF(%d): y=%d, h=%d;  Ovh=%d, Nvh=%d\n", mbtn, menubuttons[mbtn].y, menubuttons[mbtn].h, old_height, new_height);
            f = (float)new_height / (float)old_height;
            menubuttons[mbtn].h = (int)(f * (float)menubuttons[mbtn].h);
            menubuttons[mbtn].y = (int)(f * (float)menubuttons[mbtn].y);
//            if (mbtn == 0)
                AUDDBG ("---- yAFT(%d): y=%d, h=%d;  f=%.2f\n", mbtn, menubuttons[mbtn].y, menubuttons[mbtn].h, f);
        }
        adjusted = true;
    }
    return adjusted;
}

AVFormatContext * DVD::open_input_file (struct pollfd * input_fd_p)
{
    if (playback_thread_running)
        return nullptr;
    playback_thread_running = true;
    readblock = true;

    AUDINFO ("PLAY:opening input!!!!!!!!...\n");
    play_video = aud_get_bool ("dvd", "play_video");   /* JWT:RESET PLAY-VIDEO, CASE TURNED OFF ON PREV. PLAY. */
    AVFormatContext * c = avformat_alloc_context();
    if (!c)
    {
        AUDERR ("s:COULD NOT SET UP AVFORMATCONTEXT!\n");
        return nullptr;
    }
    AVInputFormat *f = av_find_input_format("mpeg");
    f->flags &= AVFMT_NOBINSEARCH | AVFMT_NOGENSEARCH | AVFMT_NO_BYTE_SEEK;
    //f->read_header = nullptr;
    //c->skip_initial_bytes = 0;
//               if (avformat_open_input( & c, "/tmp/libdvdnav.mpg", nullptr, nullptr) < 0)

    /* OPEN THE FIFO FOR INPUT. */
    writeblock = false;
    input_fd_p->fd = open ((const char *)dvdnav_priv->fifo_str, O_RDONLY);
    int pollres = 0;
    if (input_fd_p->fd >= 0)
    {
        input_fd_p->events = POLLIN;
        pollres = poll (input_fd_p, 1, 400);
AUDINFO("---FIFO OPENED, POLL RES=%d=\n", pollres);
    }
    else
        AUDERR("----FIFO HAS NOT OPEN ERRNO=%d= DESC=%s= POLL=%d\n", errno, strerror(errno), pollres);

    void * buf = av_malloc (2048);
    if (!buf)
    {
        AUDERR ("s:COULD NOT ALLOCATE AVIOContext BUFFER!\n");
        if (c)
            avformat_free_context (c);
        if (input_fd_p->fd)
            ::close (input_fd_p->fd);
        return nullptr;
    }
    AVIOContext * io = avio_alloc_context ((unsigned char *) buf, 2048, 0, input_fd_p, read_cb, nullptr, nullptr);
    if (!io)
    {
        AUDERR ("s:COULD NOT ALLOCATE AVIOContext!\n");
        if (c)
            avformat_free_context (c);
        if (input_fd_p->fd)
            ::close (input_fd_p->fd);
        return nullptr;
    }
    if (c)
        c->pb = io;
    else
        AUDERR ("s:AVFormatContext IS NOT DEFINED!\n");

    if (!input_fd_p->fd)  AUDERR("s:input_fd NOT DEFINED!\n");
    if (!pollres)  AUDERR("s:pollres NOT DEFINED!\n");
    if (!input_fd_p->fd || avformat_open_input( & c, "", f, nullptr) < 0)
    {
        AUDERR ("s:COULD NOT OPEN AVINPUT!\n");
        if (c)
            avformat_free_context (c);
        if (input_fd_p->fd)
            ::close (input_fd_p->fd);
        return nullptr;
    }
//if (playing_a_menu) AUDERR ("PLAY:input opened for MENU! streams=%d=\n", c->nb_streams); else AUDERR ("PLAY:input opened for MOVIE! streams=%d=\n", c->nb_streams);
    //av_format_inject_global_side_data (c);
    c->flags &= ~AVFMT_FLAG_GENPTS;
    if (avformat_find_stream_info (c, nullptr) < 0)
    {
        AUDERR ("e:PLAY:FAILED TO FIND STREAM INFO!\n");
        if (c)
            avformat_free_context (c);
        if (input_fd_p->fd)
            close (input_fd_p->fd);
        return nullptr;
    }
    playback_fifo_hasbeenopened = true;
    return c;
    nanosleep ((const struct timespec[]){{0, 60000000L}}, NULL);
}

/* separate reader/demuxer thread */
void DVD::reader_demuxer ()
{
    AUDDBG ("---- reader_demuxer started! ----\n");
    int out_fmt;
    int errcount = 0;
    int ret;
    int vx = 0;
    int vy = 0;
    int video_width;     // INITIAL VIDEO WINDOW-SIZE:
    int video_height;
    int video_window_x;  // VIDEO WINDOW POSITION AND SIZE WHEN PROGRAM LAST CLOSED:
    int video_window_y;
    int video_window_w;
    int video_window_h;
    int video_xmove;
    int video_resizedelay;     // MIN. TIME TO WAIT AFTER USER RESIZES VIDEO WINDOW BEFORE RE-ASPECTING (SEC.)
    int resized_window_width;  // VIDEO WINDOW-SIZE AFTER LAST RESIZE.
    int resized_window_height;
    bool videohasnowh;         // TRUE IF VIDEO CONTEXT DID NOT PROVIDE A WIDTH OR HEIGHT.
    uint32_t video_requested_width;   // WINDOW-SIZE REQUESTED BY VIDEO STREAM ITSELF (just initialize for sanity).
    uint32_t video_requested_height;
    uint32_t video_default_width;   // WINDOW-SIZE REQUESTED BY VIDEO STREAM ITSELF (just initialize for sanity).
    uint32_t video_default_height;
    int video_doreset_width;   // WINDOW-SIZE BELOW WHICH WINDOW WILL SNAP BACK TO SIZE REQUESTED BY VIDEO STREAM:
    int video_doreset_height;
    int video_qsize;           // MAX. NO. OF VIDEO PACKETS TO QUEUE AT ONE TIME.
    time_t last_resizeevent_time;  // TIME OF LAST RESIZE EVENT, SO WE CAN DETERMINE WHEN SAFE TO RE-ASPECT.
    time_t scene_start_time;   // START TIME OF CURRENTLY-PLAYING STREAM.
    float video_aspect_ratio;  // ASPECT RATIO OF VIDEO, SAVED TO PERMIT RE-ASPECTING AFTER USER RESIZES (WARPS) WINDOW.
    bool myplay_video; // WHETHER OR NOT TO DISPLAY THE VIDEO.
    bool codec_opened;     // TRUE IF SUCCESSFULLY OPENED CODECS:
    bool vcodec_opened;
    bool planar;           // USED BY Audacious
    //bool returnok = false;
    bool eof;              // BECOMES TRUE WHEN EOF REACHED BY THE READER/DECODER. 
    bool last_resized;     // TRUE IF VIDEO-WINDOW HAS BEEN RE-ASPECTED SINCE LAST RESIZE EVENT (HAS CORRECT ASPECT RATIO).
    bool sdl_initialized;  // TRUE IF SDL (VIDEO) IS SUCCESSFULLY INITIALIZED.
    SDL_Event       event; // SDL EVENTS, IE. RESIZE, KILL WINDOW, ETC.
    int video_fudge_x; int video_fudge_y;  // FUDGE-FACTOR TO MAINTAIN VIDEO SCREEN LOCN. BETWEEN RUNS.
    bool needWinSzFudge;   // TRUE UNTIL A FRAME HAS BEEN BLITTED & WE'RE NOT LETTING VIDEO DECIDE WINDOW SIZE.
    bool windowIsStable;    // JWT:SAVING AND RECREATING WINDOW CAUSES POSN. TO DIFFER BY THE WINDOW DECORATION SIZES, SO WE HAVE TO FUDGE FOR THAT!
    pktQueue *pktQ;        // QUEUE FOR VIDEO-PACKET QUEUEING.
    pktQueue *apktQ;       // QUEUE FOR AUDIO-PACKET QUEUEING.
    bool menubuttons_adjusted;
    SDL_Window * screen = nullptr;  /* JWT: MUST DECLARE VIDEO SCREEN-WINDOW HERE */
    struct pollfd input_fd_p;
#ifdef _WIN32
    SDL_Texture * bmp = nullptr;    // CAN'T USE SMARTPTR HERE IN WINDOWS - renderer.get() FAILS IF VIDEO PLAY NOT TURNED ON?!
#endif

    int videoStream;          // AVCODEC STREAM IDS.
    int audioStream;
    AVPacket pkt;
    CodecInfo cinfo, vcinfo;  // AUDIO AND VIDEO CODECS.
    AVFormatContext * c;

    /* SET UP THE VIDEO SCREEN */
    video_resizedelay = 1;     // MIN. TIME TO WAIT AFTER USER RESIZES VIDEO WINDOW BEFORE RE-ASPECTING (SEC.)
    sdl_initialized = false;  // TRUE IF SDL (VIDEO) IS SUCCESSFULLY INITIALIZED.
    video_doreset_width = 0;   // WINDOW-SIZE BELOW WHICH WINDOW WILL SNAP BACK TO SIZE REQUESTED BY VIDEO STREAM:
    video_doreset_height = 0;
    video_default_width = 720;   // WINDOW-SIZE REQUESTED BY VIDEO STREAM ITSELF (just initialize for sanity).
    video_default_height = 480;
    needWinSzFudge = true;     // TRUE UNTIL A FRAME HAS BEEN BLITTED & WE'RE NOT LETTING VIDEO DECIDE WINDOW SIZE.
    video_fudge_x = 0; 
    video_fudge_y = 0;  // FUDGE-FACTOR TO MAINTAIN VIDEO SCREEN LOCN. BETWEEN RUNS.
    windowIsStable = false;
    video_window_x = video_window_y = video_window_w = video_window_h = 0;

    play_video = aud_get_bool ("dvd", "play_video");   /* JWT:RESET PLAY-VIDEO, CASE TURNED OFF ON PREV. PLAY. */
    if (play_video)
    {
        AUDDBG ("--(INIT) PLAYING VIDEO!\n");
        String video_windowtitle;
        String song_title;
        song_title = String (dvdnav_priv->title_str);
        /* JWT: time in seconds to wait for user to stop dragging before resetting window aspect */
        video_resizedelay = aud_get_int ("ffaudio", "video_resizedelay");
        if (video_resizedelay <= 0 or video_resizedelay > 9)
            video_resizedelay = 1;
        /* JWT: size below which window is reset to video's original requested size. */
        video_doreset_width = aud_get_int ("ffaudio", "video_doreset_width");
        if ( video_doreset_width <= 0)
            video_doreset_width = 149;
        video_doreset_height = aud_get_int ("ffaudio", "video_doreset_height");
        if ( video_doreset_height <= 0)
            video_doreset_height = 149;

        SDL_SetMainReady ();
        if (SDL_InitSubSystem (SDL_INIT_VIDEO) < 0)
        {
            AUDERR ("e:Failed to init SDL (no video playing): %s.\n", SDL_GetError ());
            play_video = false;
            goto breakout1;
        }
        sdl_initialized = true;
        Uint32 flags = SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE;
    	   if (aud_get_bool ("dvd", "allow_highdpi"))
    	       flags |= SDL_WINDOW_ALLOW_HIGHDPI;

        screen = SDL_CreateWindow ("Fauxdacious DVD", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 
            video_default_width, video_default_height, flags);
        if (! screen)
        {
            AUDERR ("e:Failed to create SDL window (no video playing): %s.\n", SDL_GetError ());
            play_video = false;
            goto breakout1;
        }
        else
            SDL_SetHint (SDL_HINT_VIDEO_X11_NET_WM_PING, "0");

        video_windowtitle = aud_get_str ("dvd", "video_windowtitle");
        StringBuf titleBuf = (video_windowtitle && video_windowtitle[0])
                ? str_printf ("%s - %s", (const char *) song_title, (const char *) video_windowtitle)
                : str_copy ((const char *) song_title, -1);
        song_title = String ();
        video_windowtitle = String ();
        str_replace_char (titleBuf, '_', ' ');
        SDL_SetWindowTitle (screen, (const char *) titleBuf);
        /* NOW CALCULATE THE WIDTH, HEIGHT, & ASPECT BASED ON VIDEO'S SIZE & AND ANY USER PARAMATERS GIVEN:
            IDEALLY, ONE SHOULD ONLY SET X OR Y AND LET Fauxdacious CALCULATE THE OTHER DIMENSION,
            SO THAT THE ASPECT RATIO IS MAINTAINED, THOUGH ONE CAN SPECIFY BOTH AND FORCE
            THE ASPECT TO BE ADJUSTED TO FIT.  IF A SINGLE ONE IS SPECIFIED AS "-1", THEN
            THE NEW WINDOW WILL KEEP THE SAME VALUE FOR THAT DIMENSION AS THE PREV. WINDOW,
            AND ADJUST THE OTHER DIMENTION ACCORDINGLY TO FIT THE NEW VIDEO'S ASPECT RATIO.
            IF BOTH ARE SPECIFIED AS "-1", USE PREVIOUSLY-SAVED WINDOW SIZE REGUARDLESS OF ASPECT RATIO.
        */
        video_xmove = aud_get_int ("dvd", "video_xmove");
        /*  -1:always let windowmanager place (random); 0(DEPRECIATED/SDL1):place window via
            SDL_putenv() - may work with Windows?; 1(default):relocate window via SDL; 
            2:(both 0, then 1).  This is sometimes useful for multiple X
            desktops where the default of placing the window via SDL_putenv will ALWAYS
            place the window in the same desktop that Fauxdacious is in.  By setting to 1,
            the window will be moved to the proper location relative to the current
            desktop, and Fauxdacious is treated as "sticky" by the window manager.  Setting
            to 2 MAY be useful IF for some reason, neither 0 nor 1 work properly.
        */
        /* GET SAVED PREV. VIDEO WINDOW LOCN. AND SIZE AND TRY TO PLACE NEW WINDOW ACCORDINGLY: */
        /* JWT: I ADDED THIS TO AVOID NEW VID. WINDOW RANDOMLY POPPING UP IN NEW LOCN., IE. WHEN REPEATING A VIDEO. */
        video_window_x = aud_get_int ("dvd", "video_window_x");
        video_window_y = aud_get_int ("dvd", "video_window_y");
        video_window_w = aud_get_int ("dvd", "video_window_w");
        video_window_h = aud_get_int ("dvd", "video_window_h");
        if (video_xmove == -1)
            needWinSzFudge = false;  // NO FUDGING NEEDED IF WINDOW TO BE PLACED RANDOMLY BY WINDOWMANAGER!
        else if (video_xmove >= 0 && video_xmove != 1)  // (0 or 2)
        {
            char video_windowpos[40];
            sprintf (video_windowpos, "SDL_VIDEO_WINDOW_POS=%d, %d", video_window_x, video_window_y);
            putenv (video_windowpos);
        }
        if (video_xmove > 0)  // (1 or 2)
            SDL_SetWindowPosition (screen, video_window_x, video_window_y);
    }

breakout1:
    SmartPtr<SDL_Renderer, SDL_DestroyRenderer> renderer (createSDL2Renderer (screen, play_video));

startover:
    AUDERR ("---- reader_demuxer starting over! ----\n");
    out_fmt = 0;
    errcount = 0;
    vx = 0;
    vy = 0;
    video_width = 0;     // INITIAL VIDEO WINDOW-SIZE:
    video_height = 0;
    resized_window_width = 0;  // VIDEO WINDOW-SIZE AFTER LAST RESIZE.
    resized_window_height = 0;
    video_requested_width = 720;   // WINDOW-SIZE REQUESTED BY VIDEO STREAM ITSELF (just initialize for sanity).
    video_requested_height = 480;
    video_default_width = 720;   // WINDOW-SIZE REQUESTED BY VIDEO STREAM ITSELF (just initialize for sanity).
    video_default_height = 480;
    videohasnowh = false;
    video_qsize = 0;
    last_resizeevent_time = time (nullptr);  // TIME OF LAST RESIZE EVENT, SO WE CAN DETERMINE WHEN SAFE TO RE-ASPECT.
    video_aspect_ratio = 0;  // ASPECT RATIO OF VIDEO, SAVED TO PERMIT RE-ASPECTING AFTER USER RESIZES (WARPS) WINDOW.
    /* JWT:SAVE (static)fromstdin's STATE AT START OF PLAY, SINCE PROBES WILL CHANGE IT IN PLAYLIST ADVANCE BEFORE WE CLOSE! */
    myplay_video = play_video; // WHETHER OR NOT TO DISPLAY THE VIDEO.
    codec_opened = false;     // TRUE IF SUCCESSFULLY OPENED CODECS:
    vcodec_opened = false;
    planar = 0;
    //bool returnok = false;
    eof = false;
    last_resized = true;      // TRUE IF VIDEO-WINDOW HAS BEEN RE-ASPECTED SINCE LAST RESIZE EVENT (HAS CORRECT ASPECT RATIO).
    //wedohaveaudio = false;  // BECOMES TRUE IF WE FIND AN AUDIO FRAME.
    pktQ = nullptr;      // QUEUE FOR VIDEO-PACKET QUEUEING.
    apktQ = nullptr;     // QUEUE FOR AUDIO-PACKET QUEUEING.
    menubuttons_adjusted = false;
    c = open_input_file (& input_fd_p);  // OPEN UP THE FIFO FOR READING AND THE DEMUXER ("AV-STUFF").
    //SmartPtr<AVFormatContext, close_input_file> c (open_input_file ());
    if (!c)
    {
        AUDERR ("s:COULD NOT OPEN_INPUT_FILE!\n");
        stop_playback = true;
        playback_thread_running = false;
        return;
    }

    /* IF NOT DRAINING MENUS, DON'T EVEN BOTHER "PLAYING" MENU IF WE'RE SKIPPING, JUST ACTIVATE DEFAULT BUTTON! */
    if (playing_a_menu && aud_get_bool ("dvd", "nomenus") && ! aud_get_bool ("dvd", "menudrain"))
    {
AUDERR("--------SKIPPING MENUS WITHOUT DRAINING!------------\n");
        pci_t * pci = dvdnav_get_current_nav_pci (dvdnav_priv->dvdnav);
        writeblock = true;
        dvdnav_button_activate (dvdnav_priv->dvdnav, pci);
        //checkcodecs = true;
        playing_a_menu = ! (dvdnav_is_domain_vts (dvdnav_priv->dvdnav));
        //dvdnav_get_current_nav_dsi (dvdnav_priv->dvdnav);
        //goto error_exit;
    }

    /* FIND THE AUDIO AND VIDEO STREAMS AND OPEN CODECS: */
    videoStream = -1;
    audioStream = -1;
#ifndef ALLOC_CONTEXT
#define codecpar codec
#endif
    for (unsigned i = 0; i < c->nb_streams; i++)
    {
        if (c->streams[i]->codecpar->codec_type==AVMEDIA_TYPE_VIDEO && videoStream < 0)
            videoStream=i;
        else if (c->streams[i]->codecpar->codec_type==AVMEDIA_TYPE_AUDIO && audioStream < 0)
            audioStream=i;
    }
    if (videoStream >= 0)
    {
        AVCodec * vcodec = avcodec_find_decoder (c->streams[videoStream]->codecpar->codec_id);
        if (vcodec)
        {
            AUDINFO ("--HAVE A VIDEO CODEC!\n");
            vcinfo.stream_idx = videoStream;
            vcinfo.stream = c->streams[videoStream];
            // NO!: vcinfo.stream->discard = AVDISCARD_ALL;
            // JWT:CHGD. TO ifdef BLOCK: vcinfo.context = c->streams[videoStream]->codec;
            vcinfo.codec = vcodec;
#ifdef ALLOC_CONTEXT
            vcinfo.context = avcodec_alloc_context3 (vcinfo.codec);
            avcodec_parameters_to_context (vcinfo.context, c->streams[videoStream]->codecpar);
#else
            vcinfo.context = c->streams[videoStream]->codec;  // AVCodecContext *
#endif
            //JWT:AS/OF v3.8, LOW-QUALITY VIDEOS SEEM BETTER W/O THIS, BUT WE LEAVE IT AS A CONFIG. OPTION - YMMV:
            if (aud_get_bool ("dvd", "video_codec_flag_truncated") && vcodec->capabilities&CODEC_CAP_TRUNCATED)
                vcinfo.context->flags |= CODEC_FLAG_TRUNCATED; /* we do not send complete frames */
            if (aud_get_bool ("dvd", "video_codec_flag_gray"))
                vcinfo.context->flags |= CODEC_FLAG_GRAY; /* output in monochrome (REQUIRES FFMPEG COMPILED W/--enable-gray!) */
            vcodec_opened = true;
            AUDINFO ("got VIDEO codec %s for stream index %d, opening; w=%d= h=%d=\n", vcinfo.codec->name, vcinfo.stream_idx,
                    vcinfo.context->width, vcinfo.context->height);
        }
        else
            play_video = false;  /* turn off video playback, since no video codec! */
    }
    else
        play_video = false;  /* turn off video playback, since we could not find a video stream! */

    dvdnav_priv->lastaudiostream = audioStream;  // SAVE IT!
    if (audioStream >= 0)
    {
        AVCodec * codec = avcodec_find_decoder (c->streams[audioStream]->codecpar->codec_id);
        if (codec)
        {
            AUDDBG ("--HAVE AN AUDIO CODEC!\n");
            cinfo.stream_idx = audioStream;
            cinfo.stream = c->streams[audioStream];
            // JWT:CHGD. TO ifdef BLOCK: cinfo.context = c->streams[audioStream]->codec;
            cinfo.codec = codec;
#ifdef ALLOC_CONTEXT
            cinfo.context = avcodec_alloc_context3 (cinfo.codec);
            avcodec_parameters_to_context (cinfo.context, c->streams[audioStream]->codecpar);
#else
            cinfo.context = c->streams[audioStream]->codec;  // AVCodecContext *
#endif
            codec_opened = true;
            AUDINFO ("got AUDIO codec %s for stream index %d, opening...\n", cinfo.codec->name, cinfo.stream_idx);
        }
    }
    AUDINFO ("----VIDEO STREAM=%d=  aud=%d= count=%d=\n", videoStream, audioStream, c->nb_streams);
//    for (unsigned int x=0; x<c->nb_streams; x++)  // JWT:HELPFUL FOR DEBUGGING CODEC ISSUES BUG SEGFAULTS IF RECEIVING NAV PACKETS?!:
//    {
//        AUDERR ("------STREAM (%d)\n", x);
//        AVCodec * xcodec = avcodec_find_decoder (c->streams[x]->codecpar->codec_id);
//        AVDictionaryEntry *language = av_dict_get( c->streams[x]->metadata, "language", NULL, 0 );
//        if ( language && language->value )
//            AUDERR ("----------LANGUAGE=%s=\n", strdup( language->value ));
//AUDERR ("CODEC NAME(%d)=%s=\n", x, xcodec->name);
//        }
//    }
    myplay_video = play_video;

    if (vcodec_opened)
    {
        vcinfo.context->thread_count=1;  //from: http://stackoverflow.com/questions/22930109/call-to-avformat-find-stream-info-prevents-decoding-of-simple-png-image
        if (LOG (avcodec_open2, vcinfo.context, vcinfo.codec, nullptr) < 0)
            vcodec_opened = false;
    }
    if (playing_a_menu && !vcodec_opened && aud_get_bool ("dvd", "play_video"))  //NEEDED FOR ACTS DVD (SINCE 8.4)
    {
        AUDERR("BAD CODEC SEARCH, START OVER AND TRY AGAIN...\n");
        checkcodecs = true;
        goto error_exit;
    }
    if (codec_opened)
    {
        cinfo.context->thread_count=1;
        if (LOG (avcodec_open2, cinfo.context, cinfo.codec, nullptr) < 0)
            codec_opened = false;
    }
    if (codec_opened && ! convert_format (cinfo.context->sample_fmt, out_fmt, planar))
        codec_opened = false;

    set_stream_bitrate (c->bit_rate);
    if (codec_opened)
    {
        AUDINFO ("---OPEN_AUDIO(%d, %d, %d)\n", out_fmt, cinfo.context->sample_rate, cinfo.context->channels);
        open_audio (out_fmt, cinfo.context->sample_rate, cinfo.context->channels);
    }

    checkcodecs = false;  //WE JUST CHECKED 'EM!
    /* JWT: IF abUSER ALSO WANTS TO PLAY VIDEO THEN WE SET UP POP-UP VIDEO SCREEN: */
    if (playing_a_menu)
        readblock = false;
    if (myplay_video)
    {
        // int x, y;
        dvdnav_get_video_resolution (dvdnav_priv->dvdnav, & video_requested_width, & video_requested_height);
        AUDINFO ("----- VIDEO REQUESTED (%d x %d) or RQ:(%d x %d)-----\n", vcinfo.context->width, 
                vcinfo.context->height, video_requested_width, video_requested_height);
        if (!vcinfo.context->width && !vcinfo.context->height)
            videohasnowh = true;
        if (!vcinfo.context->width)
            vcinfo.context->width = video_requested_width;     
        if (!vcinfo.context->height)
            vcinfo.context->height = video_requested_height;     
        uint8_t dvd_video_aspect_ratio_code = dvdnav_get_video_aspect(dvdnav_priv->dvdnav);
        video_default_width = vcinfo.context->width;
        video_default_height = vcinfo.context->height;
        if (! dvd_video_aspect_ratio_code)
        {
            video_aspect_ratio = (float)4 / (float)3;
            video_default_width = video_default_height * video_aspect_ratio;
        }
        else if (dvd_video_aspect_ratio_code == 3)
        {
            video_aspect_ratio = (float)16 / (float)9;
            video_default_width = video_default_height * video_aspect_ratio;
        }
        else
            video_aspect_ratio = vcinfo.context->height
                ? (float)vcinfo.context->width / (float)vcinfo.context->height : 1.0;

        AUDINFO ("---ASPECT RATIO=%f= code=%d=\n", video_aspect_ratio, dvd_video_aspect_ratio_code);
        vx = aud_get_int ("dvd", "video_xsize");
        vy = aud_get_int ("dvd", "video_ysize");
        video_requested_width = vcinfo.context->width;
        video_requested_height = vcinfo.context->height;
        if (vx && !vy)   /* User specified (or saved) width only, calc. height based on aspect: */
        {
            video_width = (vx == -1) ? (video_window_w ? video_window_w : video_default_width) : vx;
            video_height = (int)((float)video_width / video_aspect_ratio);
            //video_default_height = (int)((float)video_default_width / video_aspect_ratio);
        }
        else if (!vx && vy)   /* User specified (or saved) height only, calc. width based on aspect: */
        {
            video_height = (vy == -1) ? (video_window_h ? video_window_h : video_default_height) : vy;
            video_width = (int)((float)video_height * video_aspect_ratio);
            //video_default_width = (int)((float)video_default_height * video_aspect_ratio);
        }
        else if (vx && vy)   /* User specified fixed width and height: */
        {
            if (vx == -1 && vy == -1)  /* Use same (saved) settings or video's settings (SCREW THE ASPECT)! */
            {
                video_width = video_window_w ? video_window_w : video_default_width;
                video_height = video_window_h ? video_window_h : video_default_height;
            }
            else if (vx == -1)  /* Use same (saved) height & calculate new width based on aspect: */
            {
                video_height = vy;
                video_width = (int)((float)video_height * video_aspect_ratio);
                //video_default_width = (int)((float)video_default_height * video_aspect_ratio);
            }
            else if (vy == -1)  /* Use same (saved) width & calculate new height based on aspect: */
            {
                video_width = vx;
                video_height = (int)((float)video_width / video_aspect_ratio);
                //video_default_height = (int)((float)video_default_width / video_aspect_ratio);
            }
            else  /* User specified window size (SCREW THE ASPECT)! */
            {
                video_width = vx;
                video_height = vy;
            }
        }
        else   /* User specified nothing, use the video's desired wXh (& ignore saved settings!): */
        {
            video_width = video_default_width;
            video_height = video_default_height;
        }
        AUDINFO ("---VIDEO W x H SET TO (%d x %d)!\n", video_width, video_height);
        if (playing_a_menu && menubuttons.len () > 0)
        {
            menubuttons_adjusted = adjust_menubuttons (video_requested_width, (uint32_t)video_width, 
                    video_requested_height, (uint32_t)video_height);
        }
        video_aspect_ratio = video_height
            ? (float)video_width / (float)video_height : 1.0;   /* Fall thru to square to avoid possibliity of "/0"! */

        /* NOW "RESIZE" screen to user's wXh, if user set something: */
        SDL_SetWindowSize (screen, video_width, video_height);

        if (aud_get_str ("dvd", "video_render_scale"))
            SDL_SetHint (SDL_HINT_RENDER_SCALE_QUALITY, aud_get_str ("dvd", "video_render_scale"));
        else
            SDL_SetHint (SDL_HINT_RENDER_SCALE_QUALITY, "1");

        /*
           YET ANOTHER STUPID SDL FUDGE HACK NEEDED SINCE SDL_SetWindowResizable() *MOVES* THE WINDOW 
           DOWN BY THE SIZE OF THE WINDOW DECORATIONS (THE "FUDGE" FACTOR) - GRRRRRR WASTED A WHOLE 
           EVENING TRYING TO FIGURE WHY MY WINDOW WAS MOVING. :(
        */
        /* STILL DOESN'T WORK CONSISTANTLY, SO, FOR NOW, DON'T USE SDL_SetWindowResizable()!
        SDL_GetWindowPosition (screen, &x, &y);

        if (playing_a_menu)
        {
            if (video_width < video_doreset_width && video_height <  video_doreset_height)  //MAKE SURE MENU IS VISIBLE!
                SDL_SetWindowSize (screen, (int)((float)video_doreset_height * video_aspect_ratio), video_doreset_height);
            SDL_SetWindowResizable (screen, SDL_FALSE);  // DON'T ALLOW MENU WINDOWS TO BE RESIZED!
        }
        else
            SDL_SetWindowResizable (screen, SDL_TRUE);
        //if (windowIsStable) AUDERR ("-FUDGEHACK: Y=%d= YFUDGE=%d=\n", y, video_fudge_y); else AUDERR ("---NOT STABLE: Y=%d= YFUDGE=%d=\n", y, video_fudge_y);
        if (windowIsStable)  // JWT:REVERSE STUPID SDL_SetWindowResizable()'S MOVEMENT OF OUR WINDOW!
            SDL_SetWindowPosition (screen, x+video_fudge_x, y+video_fudge_y);
        */
    }

    /* JWT:video_qsize:  MAX # PACKETS TO QUEUE UP FOR INTERLACING TO SMOOTH VIDEO
        PLAYBACK - GOOD RANGE IS 6-28, DEFAULT IS 8:
        NOT ENOUGH = JITTERY VIDEO
        TOO MANY = AUDIO/VIDEO BECOME NOTICABLY OUT OF SYNC!
    */
    video_qsize = aud_get_int ("dvd", "video_qsize");
    if (video_qsize < 1)
        video_qsize = (aud_get_int ("ffaudio", "video_qsize"))
                ? aud_get_int ("ffaudio", "video_qsize") : 5;
    if (video_qsize < 1)
        video_qsize = 5;

    /* TYPICALLY THERE'S TWICE AS MANY AUDIO PACKETS AS VIDEO, SO THIS IS COUNTER-INTUITIVE, BUT IT WORKS BEST! */
    pktQ = createQueue (2 * video_qsize);
    apktQ = createQueue (video_qsize);

    {   // SUBSCOPE FOR DECLARING SDL2 TEXTURE AS SCOPED SMARTPOINTER:
    bool windowNowExposed = false;  // JWT:NEEDED TO PREVENT RESIZING WINDOW BEFORE EXPOSING ON MS-WINDOWS?!
    int seek_value;
    bool highlightbuttons = playing_a_menu ? aud_get_bool("dvd", "highlightbuttons") : false;
    int minmenushowsec = aud_get_int ("dvd", "minmenushowsec");
    if (minmenushowsec < 1)
        minmenushowsec = 16;
    if (! renderer)
    {
        AUDERR ("e:SDL: could not create video renderer - no video play (%s)\n", SDL_GetError ());
        myplay_video = false;
    }
#ifdef _WIN32
#define bmpptr bmp
    else  // CAN'T SMARTPTR THIS IN WINBLOWS SINCE FATAL ERROR (ON renderer.get IF NO RENDERER) IF VIDEO-PLAY TURNED OFF (COMPILER DIFFERENCE)!
        bmp = createSDL2Texture (screen, renderer.get (), myplay_video, vcinfo.context->width, vcinfo.context->height);
#else
#define bmpptr bmp.get ()
    SmartPtr<SDL_Texture, SDL_DestroyTexture> bmp (createSDL2Texture (screen, renderer.get (), myplay_video, vcinfo.context->width, vcinfo.context->height));
#endif
    if (! bmp)
    {
        AUDERR("e:NO VIDEO DUE TO INABILITY TO GET TEXTURE!\n");
        myplay_video = false;
    }

    /* OUTER LOOP TO READ, QUEUE AND PROCESS AUDIO & VIDEO PACKETS FROM THE STREAM: */
    update_title_len ();
    if (! vcodec_opened)  AUDERR("w:UNABLE TO OPEN VIDEO CODEC2!\n");
    if (! myplay_video)  AUDERR("w:NO VIDEO WHEN STARTING LOOP!\n");

    if (myplay_video && screen)
    {
        SDL_SetWindowInputFocus (screen);  //TRY TO SET INPUT FOCUS ON VIDEO WINDOW FOR EASIER (1-CLICK) MENU-SELECTION:
        if (highlightbuttons)
            SDL_SetRenderDrawBlendMode(renderer.get (), SDL_BLENDMODE_BLEND);
    }
    AUDDBG ("READER-DEMUXER: STARTING LOOP\n");
    pci_t * pci = dvdnav_get_current_nav_pci (dvdnav_priv->dvdnav);
    scene_start_time = time (nullptr);
    bool were_playing_a_menu = playing_a_menu;

    /* MAIN LOOP TO DEMUX AUDIO/VIDEO DATA AND PRESENT IT TO USER - EXITS WHEN CHANNEL CHANGES (HOPS): */
    while (! reader_please_die)
    {
        if (playing_a_menu != were_playing_a_menu)
            checkcodecs = true;
        if (checkcodecs)  /* WE NEED TO START OVER - CHANNEL CHANGE, NEED TO RESCAN CODECS / STREAMS! */
        {
            if (vcodec_opened)  // SEND EMPTY PACKET TO FLUSH VIDEO SCREAM:
            {
                AVPacket emptypkt = AVPacket ();
                av_init_packet (& emptypkt);
                emptypkt.data=nullptr; emptypkt.size=0;
                write_videoframe (renderer.get (), & vcinfo, bmpptr, & emptypkt, 
                      video_width, video_height, & last_resized, & videohasnowh, & windowIsStable,
                      & resized_window_width, & resized_window_height);
                av_free_packet (& emptypkt);
            }
            writeblock = false;  // DON'T BLOCK FURTHER DVD ENGINE WRITES:
            QFlush (apktQ);      // FLUSH PACKET QUEUES:
            QFlush (pktQ);
            goto error_exit;
        }
        writeblock = false;

        /* CHECK IF USER MOVED THE SEEK/POSITION SLIDER, IF SO FLUSH QUEUES AND SEEK TO NEW POSITION: */
        seek_value = check_seek ();
        if (seek_value >= 0)
        {
            dvdnav_priv->nochannelhop = true;     // DON'T ALLOW CHANNEL HOPPING WHILST WE'RE MOVING SEEK SLIDER!
            int faudlen = aud_drct_get_length (); // STREAM LENGTH ACCORDING TO Fauxdacius.
            /* JWT:FIRST, FLUSH ANY PACKETS SITTING IN THE QUEUES TO CLEAR THE QUEUES! */
            if (! playing_a_menu && faudlen > 0)  // NO SEEKING IN MENUS, PRETTY POINTLESS!
            {
                QFlush (apktQ);
                QFlush (pktQ);
                //if (LOG (av_seek_frame, ic.get (), -1, (int64_t) seek_value *
                //        AV_TIME_BASE / 1000, AVSEEK_FLAG_BACKWARD) >= 0)
                uint32_t dvpos = 0, dvlen = 0;
                dvdnav_get_position(dvdnav_priv->dvdnav, &dvpos, &dvlen);
                AUDDBG ("**** fSEEK =%d= dvpos=%d= dvlen=%d= flen=%d= endpos=%ld=\n", seek_value, dvpos, dvlen, faudlen, dvdnav_priv->end_pos);
                int64_t newpos = (int)(((float)dvlen / (float)faudlen) * (float)seek_value);  // CONVERT FAUXDACIOUS SEEKIES INTO DVD SEEKIES!
                if (dvdnav_priv->end_pos && newpos > dvdnav_priv->end_pos)
                    newpos = dvdnav_priv->end_pos;
                if (dvdnav_sector_search (dvdnav_priv->dvdnav, (uint64_t) newpos, SEEK_SET) != DVDNAV_STATUS_OK)
                    AUDERR ("w:COULD NOT SEEK!\n");
            }
            errcount = 0;
            seek_value = -1;
        }

        /* READ AND PROCESS NEXT FRAME: */
        pkt = AVPacket ();
        av_init_packet (& pkt);
//AUDERR("---WILL READ A FRAME!\n");
        ret = LOG (av_read_frame, c, & pkt);
//if (playing_a_menu)  AUDERR("---READ FRAME, res=%d\n", ret);
        if (ret < 0)  // CHECK FOR EOF OR ERRORS:
        {
            ++errcount;
            if (ret == (int) AVERROR_EOF)  /* END OF FILE WHILST READING FRAMES: */
            {
                if (playing_a_menu) AUDINFO ("i:eof reached in menu, continue\n"); else AUDINFO ("i:eof reached in movie ********************\n");
                av_free_packet (& pkt);
                eof = true;

                /* FIRST, PROCESS ANYTHING STILL IN THE QUEUES: */
                while (apktQ->size > 0 || pktQ->size > 0)
                {
                    AVPacket * pktRef;
                    while (1)   // WE PREFER TO OUTPUT ORDERED AS AUDIO, VIDEO, AUDIO, ...
                    {
                        if ((pktRef = (apktQ->size ? & apktQ->elements[apktQ->front] : nullptr)))  // PROCESS NEXT AUDIO FRAME IN QUEUE:
                        {
                            write_audioframe (& cinfo, pktRef, out_fmt, planar);
                            Dequeue (apktQ);
                        }
                        if ((pktRef = (pktQ->size ? & pktQ->elements[pktQ->front] : nullptr)))  // PROCESS NEXT VIDEO FRAME IN QUEUE:
                        {
                            write_videoframe (renderer.get (), & vcinfo, bmpptr, pktRef, 
                                    video_width, video_height, & last_resized, & videohasnowh, & windowIsStable,
                                        & resized_window_width, & resized_window_height);
                            if (highlightbuttons)
                            {
                                if (! menubuttons_adjusted && menubuttons.len () > 0)
                                    menubuttons_adjusted = adjust_menubuttons (video_requested_width, (uint32_t)video_width, 
                                            video_requested_height, (uint32_t)video_height);
                                draw_highlight_buttons (renderer.get ());
                            }
                            Dequeue (pktQ);
                        }
                        else
                            break;
                        if ((pktRef = (apktQ->size ? & apktQ->elements[apktQ->front] : nullptr)))  // PROCESS A 2ND AUDIO FRAME IN QUEUE (DO 2 AUDIOS PER VIDEO!):
                        {
                            write_audioframe (& cinfo, pktRef, out_fmt, planar);
                            Dequeue (apktQ);
                        }
                        else
                            break;
                    }
                }
                if (playing_a_menu)
                {
                    if (vcodec_opened)  /* FLUSH VIDEO CODEC TO ENSURE USER SEES ALL OF THE MENU SCREEN: */
                    {
                        AVPacket emptypkt = AVPacket ();
                        av_init_packet (& emptypkt);
                        emptypkt.data=nullptr; emptypkt.size=0;
                        write_videoframe (renderer.get (), & vcinfo, bmpptr, & emptypkt, 
                              video_width, video_height, & last_resized, & videohasnowh, & windowIsStable,
                              & resized_window_width, & resized_window_height);
                        av_free_packet (& emptypkt);
                    }
                    AUDINFO ("i:MENU EOF: BUTTON COUNT IN THIS MENU:  %d! duration=%d\n", pci->hli.hl_gi.btn_ns, dvdnav_priv->duration);
                    if (pci->hli.hl_gi.btn_ns < 1)
                    {
                        if (dvdnav_priv->duration <= 0 || difftime (time (nullptr), scene_start_time) > dvdnav_priv->duration / 1000)
                        {
                            dvdnav_priv->nochannelhop = false;
                            if (dvdnav_menu_call (dvdnav_priv->dvdnav, DVD_MENU_Escape) == DVDNAV_STATUS_OK)
                                AUDINFO ("i:WE ARE MAKING OUR ESCAPE!...\n");
                            else
                                AUDERR ("e:WE SEEM TO BE STUCK IN A BUTTONLESS MENU!...\n");
                            eof = false;
                            were_playing_a_menu = playing_a_menu;
                        }
//                        else
//                            AUDERR("WAITING %d SECONDS FOR BUTTONLESS MENU...\n", dvdnav_priv->duration / 1000);
                    }
                }
                else if (dvdnav_next_pg_search (dvdnav_priv->dvdnav) == DVDNAV_STATUS_OK)  // PLAYING A MOVIE:
                {
                    dvdnav_priv->nochannelhop = false;
                    AUDINFO ("i:MOVIE ENDED, ADVANCING TO NEXT SCENE(%d)...\n", dvdnav_priv->duration / 1000);
                    eof = false;
                    were_playing_a_menu = playing_a_menu;
                }
                else  // PLAYING THE LAST MOVIE (NO NEXT SCENE TO GO TO, SO STOP PLAYBACK:
                {
                    dvdnav_priv->nochannelhop = false;
                    AUDINFO ("i:MOVIE ENDED, NO NEXT STREAM, STOPPING PLAY...\n");
                    stop_playback = true;
                    checkcodecs = false;
                    readblock = false;
                    eof = false;
                }
            }
            else if (errcount > 4)  // TOO MANY FRAME ERRORS, PUNT:
            {
                dvdnav_priv->nochannelhop = false;
                AUDERR ("w:av_read_frame error %d, giving up.\n", ret);
                av_free_packet (& pkt);
                goto error_exit;
            }
            else
            {
                AUDERR ("w:Error reading packet, try again?\n");
                av_free_packet (& pkt);
                continue;
            }
        }
        else
        {
            errcount = 0;
            eof = false;
        }

        /* AFTER READING BUT BEFORE PROCESSING EACH PACKET, CHECK IF EITHER QUEUE IS FULL, IF SO, WRITE NEXT 
           WAITING PACKETS UNTIL AT LEAST ONE QUEUE IS EMPTIED, ORDERING OUTPUT AS AUDIO, VIDEO, AUDIO, ... 
           SINCE WE TYPICALLY HAVE TWICE AS MANY AUDIO PACKETS AS VIDEO.  THIS IS THE SECRET TO KEEPING 
           OUTPUT SYNCED & SMOOTH! */
        if (apktQ->size == apktQ->capacity || pktQ->size == pktQ->capacity)  // ONE OF THE PACKET QUEUES IS FULL:
        {
            AVPacket * pktRef;
            while (1)  // TRY TO READ AT LEAST 1 AUDIO, THEN 1 VIDEO, BUT KEEP GOING UNTIL ONE QUEUE IS EMPTY:
            {
                if ((pktRef = (apktQ->size ? & apktQ->elements[apktQ->front] : nullptr)))  // PROCESS NEXT AUDIO FRAME IN QUEUE:
                {
                    write_audioframe (& cinfo, pktRef, out_fmt, planar);
                    Dequeue (apktQ);
                }
                if ((pktRef = (pktQ->size ? & pktQ->elements[pktQ->front] : nullptr)))  // PROCESS NEXT VIDEO FRAME IN QUEUE:
                {
                    write_videoframe (renderer.get (), & vcinfo, bmpptr, pktRef, 
                            video_width, video_height, & last_resized, & videohasnowh, & windowIsStable,
                                & resized_window_width, & resized_window_height);
                    if (highlightbuttons)
                    {
                        if (! menubuttons_adjusted && menubuttons.len () > 0)
                            menubuttons_adjusted = adjust_menubuttons (video_requested_width, (uint32_t)video_width, 
                                    video_requested_height, (uint32_t)video_height);
                        draw_highlight_buttons (renderer.get ());
                    }
                    Dequeue (pktQ);
                }
                else
                    break;
                if ((pktRef = (apktQ->size ? & apktQ->elements[apktQ->front] : nullptr)))  // PROCESS A 2ND AUDIO FRAME IN QUEUE (DO 2 AUDIOS PER VIDEO!):
                {
                    write_audioframe (& cinfo, pktRef, out_fmt, planar);
                    Dequeue (apktQ);
                }
                else
                    break;
            }
        }
        /* NOW PROCESS THE CURRENTLY-READ PACKET, EITHER OUTPUTTING IT OR QUEUEING IT: */
        if (! eof)
        {
            if (codec_opened && pkt.stream_index == cinfo.stream_idx)  /* WE READ AN AUDIO PACKET: */
            {
//if (playing_a_menu) AUDERR("audio pkt: pkt=%d= audio=%d=\n", pkt.stream_index, cinfo.stream_idx);
                Enqueue (apktQ, pkt);
            }
            else
            {
//if (playing_a_menu) AUDERR("NON-AUDIO PACKET: pkt=%d= video=%d=\n", pkt.stream_index, vcinfo.stream_idx);
                if (vcodec_opened)
                {
//if (playing_a_menu) AUDERR("VIDEO CODEC OPEN!!!\n");
                    if (pkt.stream_index == vcinfo.stream_idx)  /* WE READ A VIDEO PACKET: */
                    {
//if (playing_a_menu) AUDERR("WE HAVE A VIDEO PACKET!\n");
                        Enqueue (pktQ, pkt);
                    }
                    else
                        av_free_packet (& pkt);
                }
                else   /* IGNORE ANY OTHER SUBSTREAMS */
                {
                    av_free_packet (& pkt);
                    continue;
                }
            }
        }
        /* AT THIS POINT THE PACKET MUST BE EITHER ENQUEUED OR FREED! */
        /* JWT: NOW HANDLE ANY VIDEO UI EVENTS SUCH AS RESIZE OR KILL VIDEO SCREEN (IF PLAYING VIDEO): */
        /*      IF WE'RE HERE, WE *ARE* STILL PLAYING VIDEO (BUT THAT MAY CHANGE WITHIN THIS LOOP)! */
// polluserevents:
        while (SDL_PollEvent (&event))
        {
            switch (event.type) {
                case SDL_MOUSEBUTTONDOWN: // if the event is mouse click
//                    if(event.button == SDL_BUTTON_LEFT)
                    if (playing_a_menu)
                    {
                        /* USER PRESSED A MENU BUTTON, SEE WHICH ONE AND ACTIVATE IT: */
//AUDERR("--1MOUSE BUTTON PRESSED...\n");
                        for (int mbtn = 0; mbtn < menubuttons.len (); mbtn ++)
                        {
//AUDERR("---1CHECK MENU BUTTON %d...\n", mbtn);
                            if (event.motion.x >= menubuttons[mbtn].x && event.motion.x <= (menubuttons[mbtn].x + menubuttons[mbtn].w)
                                && event.motion.y >= menubuttons[mbtn].y && event.motion.y <= (menubuttons[mbtn].y + menubuttons[mbtn].h))
                            {
                                dvdnav_priv->nochannelhop = false;
                               AUDERR ("---------- 1MENU BUTTON %d SELECTED! --------------\n", (mbtn+1));
                                dvdnav_priv->wakeup = true;
                                //pci_t * pci = dvdnav_get_current_nav_pci(dvdnav_priv->dvdnav);
                                //dvdnav_get_current_nav_dsi(dvdnav_priv->dvdnav);
                                dvdnav_button_select (dvdnav_priv->dvdnav, pci, (mbtn+1));
                                SDL_RenderFillRect (renderer.get (), nullptr);
                                SDL_RenderPresent (renderer.get ());
                                AUDINFO ("i:ACTIVATE MENU BUTTON2 (MAY ASSERT ITSELF)!\n");
                                writeblock = true;  // DON'T LET DVD ENGINE WRITE ANY MORE UNTIL WE'RE DONE DRAINING!
                                dvdnav_button_activate (dvdnav_priv->dvdnav, pci);
//if (dvdnav_is_domain_vts (dvdnav_priv->dvdnav)) AUDERR("WE'RE A MOVIE, I THINK\n"); else AUDERR("WE'RE A MENU, I THINK\n");
                                //DRAIN ANY REMAINING AUDIO/VIDEO PACKETS (UNLESS USER TOLD US NOT TO - SOME DVDS HAVE ISSUES):
                                //if (! aud_get_bool ("dvd", "nomenudrain") && dvdnav_is_domain_vts (dvdnav_priv->dvdnav))
                                playing_a_menu = ! (dvdnav_is_domain_vts (dvdnav_priv->dvdnav));
                                if ((aud_get_bool ("dvd", "menudrain") && playing_a_menu)
                                        || (! playing_a_menu && ! aud_get_bool ("dvd", "nomoviedrain")))
                                {
                                    AUDINFO ("---START DRAINING!\n");
                                    while (! eof)
                                    {
                                        ret = LOG (av_read_frame, c, & pkt);
                                        if (ret)
                                            break;
                                        //AUDINFO ("---DRAIN MORE STUFFAGE!\n");
                                    }
                                    AUDINFO ("---DONE DRAINING!\n");
                                }
                                writeblock = false;
                                pci = dvdnav_get_current_nav_pci(dvdnav_priv->dvdnav);
                                were_playing_a_menu = playing_a_menu = ! (dvdnav_is_domain_vts (dvdnav_priv->dvdnav));
                                break;
                            }
                        }
                    }
                case SDL_WINDOWEVENT:
                    switch (event.window.event)
                    {
                        case SDL_WINDOWEVENT_CLOSE:  /* USER CLICKED THE "X" IN UPPER-RIGHT CORNER, KILL VIDEO WINDOW BUT KEEP PLAYING AUDIO! */
                            dvdnav_priv->nochannelhop = false;
                            AUDINFO ("i:SDL_CLOSE (User killed video window for this play)!\n");
                            stop_playback = true;
                            checkcodecs = false;
                            readblock = false;
                            break;
                        case SDL_WINDOWEVENT_RESIZED:  /* WINDOW CHANGED SIZE EITHER BY US OR BY USER DRAGGING WINDOW CORNER (WE DON'T KNOW WHICH HERE) */
                            if (! windowNowExposed)
                                break;
                            /* Resize the screen. */
                            resized_window_width = event.window.data1;  // window's reported new size
                            resized_window_height = event.window.data2;
                            AUDINFO ("i:SDL_RESIZE!!!!!! rvw=%d h=%d\n", resized_window_width, resized_window_height);
                            last_resized = false;  // false means now we'll need re-aspecting, so stop blitting!
                            last_resizeevent_time = time (nullptr);  // reset the wait counter for when to assume user's done dragging window corner.
                            break;
                        case SDL_WINDOWEVENT_EXPOSED:  // window went from underneith another to visible (clicked on?)
                            if (last_resized)
                            {
                                SDL_RenderPresent (renderer.get ());  // only blit a single frame at startup will get refreshed!
                                windowNowExposed = true;
                            }
                    }
            }
        }
        if (! last_resized)  /* IF WINDOW CHANGED SIZE (SINCE LAST RE-ASPECTING: */
        {
            /* RE-ASPECT ONLY IF IT'S BEEN AT LEAST 1 SECOND SINCE LAST RESIZE EVENT (USER STOPPED DRAGGING
               WINDOW CORNER!  PBM. IS WE DON'T KNOW WHEN USER'S DONE DRAGGING THE MOUSE, SO WE GET 
               A CONTINUOUS SPEWAGE OF "RESIZED" EVENTS AND WE WON'T BOTHER RE-ASPECTING THE WINDOW UNTIL
               WE'RE (PRETTY) SURE HE'S DONE! (RE-CALCULATING THE ASPECT RATIO AND RESIZING WINDOW TO 
               MATCH THAT WHILST THE WINDOW IS CHANGING SIZE IS *EXTREMELY* INEFFICIENT AND COCKS
               UP THE DISPLAY AND PLAYBACK!)
            */
            if (!windowIsStable || difftime (time (nullptr), last_resizeevent_time) > video_resizedelay)
            {
                float new_aspect_ratio;  // ASPECT (for comparing), W, & H OF WINDOW AFTER USER DONE RESIZING:
                int new_video_width;     // WILL ADJUST ONE OF THESE TO RESTORE TO VIDEO'S PROPER ASPECT
                int new_video_height;    // THEN RESIZE (RE-ASPECT) THE WINDOW TO KEEP ASPECT CONSTANT!
                /* CALCULATE THE RESIZED WINDOW'S ASPECT RATIO */
                new_aspect_ratio = resized_window_height
                    ? (float)resized_window_width / (float)resized_window_height : 1.0;
                if (videohasnowh)
                {
                    video_aspect_ratio = new_aspect_ratio;
AUDERR("---RESIZE(videohasnowh): NEW RATIO=%f=\n", video_aspect_ratio);
                    videohasnowh = false;
                }
                /* NOW MANUALLY ADJUST EITHER THE WIDTH OR HEIGHT BASED ON USER'S CONFIG. TO RESTORE 
                   THE NEW WINDOW TO THE PROPER ASPECT RATIO FOR THE CURRENTLY-PLAYING VIDEO:
                */
                if (vy == -1)  // USER SAYS ADJUST HEIGHT TO MATCH WIDTH:
                {
                    new_video_height = resized_window_height;
                    new_video_width = (int)(video_aspect_ratio * (float)new_video_height);
                }
                else if (vx == -1)  // USER SAYS ADJUST WIDTH TO MATCH HEIGHT:
                {
                    new_video_width = resized_window_width;
                    new_video_height = (int)((float)new_video_width / video_aspect_ratio);
                }
                else  // USER DOESN'T CARE, SO WE DECIDE WHICH TO ADJUST:
                {
                    if (resized_window_width < video_width || resized_window_height < video_height)
                    {
                        if (new_aspect_ratio > video_aspect_ratio)  // WINDOW SHRANK & BECAME MORE HORIZONTAL - ADJUST WIDTH TO NEW HEIGHT:
                        {
                            new_video_height = resized_window_height;
                            new_video_width = (int)(video_aspect_ratio * (float)new_video_height);
                        }
                        else  // WINDOW SHRANK & BECAME MORE VERTICAL - ADJUST HEIGHT TO NEW WIDTH:
                        {
                            new_video_width = resized_window_width;
                            new_video_height = (int)((float)new_video_width / video_aspect_ratio);
                        }
                    }
                    else
                    {
                        if (new_aspect_ratio > video_aspect_ratio)  // WINDOW GREW & BECAME MORE HORIZONTAL - ADJUST HEIGHT TO NEW WIDTH:
                        {
                            new_video_width = resized_window_width;
                            new_video_height = (int)((float)new_video_width / video_aspect_ratio);
                        }
                        else  // WINDOW GREW & BECAME MORE VERTICAL - ADJUST WIDTH TO NEW HEIGHT:
                        {
                            new_video_height = resized_window_height;
                            new_video_width = (int)(video_aspect_ratio * (float)new_video_height);
                        }
                    }
                }
                /* USER SHRANK THE WINDOW BELOW "DORESET" THRESHOLD (user-configurable) SO RESIZE TO VIDEO'S ORIGINALLY REQUESTED (IDEAL) SIZE: */
                if (resized_window_width <  video_doreset_width && resized_window_height <  video_doreset_height)
                {
                    new_video_width = video_default_width;
                    new_video_height = video_default_height;
                }
// JWT:FIXME - DON'T DO THIS SINCE WE CAN'T REBLIT THE MENU SCREEN AS THE IMAGE IS NO LONGER AT OUR DISPOSAL!:
//                if (playing_a_menu && menubuttons.len () > 0)
//                {
//                    menubuttons_adjusted = adjust_menubuttons ((uint32_t)video_width, (uint32_t)new_video_width, 
//                            (uint32_t)video_height, (uint32_t)new_video_height);
//                }
                video_width = new_video_width;
                video_height = new_video_height;
                AUDINFO (" ----RESIZED TO(%d, %d)\n", video_width, video_height);
                /* NOW MANUALLY RESIZE (RE-ASPECT) WINDOW BASED ON VIDEO'S ORIGINALLY-CALCULATED ASPECT RATIO: */
                SDL_SetWindowSize (screen, video_width, video_height);
                SDL_Delay (50);
                last_resized = true;  // WE'VE RE-ASPECTED, SO ALLOW BLITTING TO RESUME!
                SDL_RenderPresent (renderer.get ());  // only blit a single frame at startup will get refreshed!
            }
        }
        /* (SDL2): WE HAVE TO WAIT UNTIL HERE FOR SDL_GetWindowPosition() TO RETURN THE CORRECT POSITION
           SO WE CAN CALCULATE A "FUDGE FACTOR" SINCE:  
           1) SDL_SetWindowPosition(x, y) FOLLOWED BY SDL_GetWindowPosition() DOES *NOT* 
           RETURN (x, y) BUT x+<windowdecorationwidth>, y+windowdecorationheight!
           (THIS IS B/C THE WINDOW IS PLACED W/IT'S UPPER LEFT CORNER AT THE SPECIFIED POSITION, *BUT* THE 
           COORDINATES RETURNED REFER TO THE UPPER LEFT CORNER OF THE ACTUAL (UNDECORATED) VIDEO SCREEN!!)
           2) SDL_GetWindowPosition() SEEMS TO RETURN PSUEDO-RANDOMLY *DIFFERENT* COORDINATES AFTER THE FIRST 
           BLIT THAN THOSE WHERE THE WINDOW WAS ORIGINALLY PLACED, BUT IS CONSISTANT AFTER THAT, SO WE WAIT 
           UNTIL NOW (AFTER FIRST BLIT BUT BEFORE USER CAN MOVE THE WINDOW) TO GET THE WINDOW-POSITION AND 
           COMPARE WITH WHAT WE INITIALLY SET THE WINDOW TO TO DETERMINE THE NECESSARY "FUDGE FACTOR" IN 
           ORDER TO ADJUST THE COORDINATES WHEN WE'RE READY TO SAVE THE WINDOW-POSITION WHEN EXITING PLAY!
           (VERY ANNOYING, ISN'T IT!)  FOR SDL-1, THE FUDGE-FACTOR IS ONLY THE WxH OF WINDOW-DECORATIONS.
           (WE MARK THE WINDOW AS "STABLE" FOR THIS PURPOSE AFTER BLITTING A FRAME)
           NOTE:  THE WINDOW DOESN'T *MOVE* AFTER INITIAL PLACEMENT, JUST THE RETURNED COORDINATES DIFFER!
        */
        if (needWinSzFudge && windowIsStable)
        {
            int x, y;
            SDL_GetWindowPosition (screen, &x, &y);
            video_fudge_x = video_window_x - x;
            video_fudge_y = video_window_y - y;
            AUDDBG ("FUDGE SET(x=%d y=%d) vw=(%d, %d) F=(%d, %d)\n", x,y,video_window_x,video_window_y,video_fudge_x,video_fudge_y);
            needWinSzFudge = false;
        }
        if (codec_opened && check_stop ())   //check_stop NO WORKEE IF WE'RE A VIDEO-ONLY STREAM!
        {
            stop_playback = true;
            checkcodecs = false;
            readblock = false;
            AUDDBG ("USER PRESSED STOP BUTTON!!!!!!!!!!!!!!!!!!!!!!!!!\n");
            // DON'T DO THIS, CAUSES HANG!:  goto error_exit;
        }
    }  // END PACKET-PROCESSING LOOP.

    }  // END OF SUBSCOPE FOR DECLARING SDL2 TEXTURE AS SCOPED SMARTPOINTER.

error_exit:  /* WE END UP HERE WHEN PLAYBACK IS STOPPED: */

    AUDINFO ("end of playback.\n");
    if (pktQ)
        destroyQueue (pktQ);
    if (apktQ)
        destroyQueue (apktQ);

    /* CLOSE UP THE CODECS, ETC.: */
    if (vcodec_opened)
    {
#ifdef ALLOC_CONTEXT
        avcodec_free_context (& vcinfo.context);
#else
        avcodec_close (vcinfo.context);
#endif
        av_free (vcinfo.context);
    }
    if (codec_opened)
    {
#ifdef ALLOC_CONTEXT
        avcodec_free_context (& cinfo.context);
#else
        avcodec_close (cinfo.context);
#endif
        av_free (cinfo.context);
    }
    close_input_file (c);
    c = nullptr;
    if (input_fd_p.fd)
        close (input_fd_p.fd);
    if (checkcodecs)
    {
        AUDINFO ("DEMUXER: CODEC CHECK, RESTART!......\n");
        writeblock = false;
        checkcodecs = false;
        playback_thread_running = false;
        goto startover;
    }

        if (screen)  // bmp ALREADY FREED & NOW OUT OF SCOPE BUT MAKE SURE VIDEO WINDOW IS FREED & GONE!
        {
            if (! needWinSzFudge)
                save_window_xy (screen, video_fudge_x, video_fudge_y);
            SDL_DestroyWindow (screen);
            screen = nullptr;
        }
    if (sdl_initialized)
        SDL_QuitSubSystem (SDL_INIT_VIDEO);

    playback_thread_running = false;
    AUDINFO ("READER THREAD RETURNING!\n");
    return;
}

/* THIS "PLAYS" THE DVD - FETCHING INSTRUCTION AND DATA PACKETS FROM THE DVD ENGINE - audacious play thread only */
bool DVD::play (const char * name, VFSFile & file)
{
    pthread_mutex_lock (& mutex);

    if (! check_disk_status ())  // CHECK THAT DISK IS STILL IN THE DRIVE!:
    {
        dvd_error ("Attempt to play removed disk, clearing playlist of DVD tracks!\n");
        reset_trackinfo ();
        purge_func.queue (purge_all_playlists, nullptr);
        pthread_mutex_unlock (& mutex);
        return false;
    }

    if (! trackinfo.len () && ! refresh_trackinfo (true))
    {
        pthread_mutex_unlock (& mutex);
        return false;
    }

    play_video = false;
    pthread_t rdmux_thread;
    playback_thread_running = false;
    playback_fifo_hasbeenopened = false;
    reader_please_die = false;
    playing_a_menu = false;
    checkcodecs = false;
AUDINFO("OPENING FIFO (w+, should not block!)\n");

// SEE:  http://www.linux-mag.com/id/357/; http://man7.org/linux/man-pages/man2/poll.2.html

    dvdnav_priv->track = find_trackno_from_filename (name);  // WHICH TRACK WE'RE GOING TO PLAY.
    AUDINFO ("PLAY:  ============ STARTING TO PLAY DVD (Track# %d NAME=%s) =============\n", dvdnav_priv->track, name); 

    //int buffer_size = aud_get_int (nullptr, "output_buffer_size");
    int speed = aud_get_int ("dvd", "disc_speed");
    speed = aud::clamp (speed, MIN_DISC_SPEED, MAX_DISC_SPEED);

    int len, event;
    uint8_t buf[2048];
    struct stat statbuf;
    playing = true;
    stop_playback = false;

    /* CREATE THE FIFO, IF IT DOESN'T ALREADY EXIST. */
    AUDDBG ("PLAY:FIFO OPENEDfixing to start loop\n");
    if (stat (dvdnav_priv->fifo_str, &statbuf) && mkfifo((const char *)dvdnav_priv->fifo_str, S_IWUSR | S_IRUSR | S_IRGRP | S_IROTH))
    {
        AUDERR ("f:Error creating playback fifo at: (%s)\n", (const char *)dvdnav_priv->fifo_str);
        stop_playback = true;
    }

    /* SPAWN THE READER/DEMUXER THREAD */
#ifndef NODEMUXING
    if (pthread_create (&rdmux_thread, nullptr, reader_thread, this))
    {
        AUDERR ("f:Error creating playback reader thread\n");
        stop_playback = true;
    }
    else
#endif
        output_fd = fopen ((const char *)dvdnav_priv->fifo_str, "w");  /* OPEN THE FIFO FOR OUTPUT. */

    /* PLAY THE SELECTED OR FIRST TRACK/TITLE */
    if (dvdnav_priv->track > 0)
    {
        if (dvdnav_title_play (dvdnav_priv->dvdnav, dvdnav_priv->track) != DVDNAV_STATUS_OK)
        {
            dvd_error ("Couldn't select title %d, error '%s'\n", dvdnav_priv->track, dvdnav_err_to_string (dvdnav_priv->dvdnav));
            pthread_mutex_unlock (& mutex);
            return false;
        }
        //show_audio_subs_languages(dvdnav_priv->dvdnav);
        AUDINFO ("i:ID_DVD_CURRENT_TITLE=%d\n", dvdnav_priv->track);
    }
    else  // PLAYING DEFAULT TRACK (0) - USUALLY THE MAIN MENU:
    {
        AUDINFO ("i:PLAY: CALLING TOP MENU!\n");
        if (dvdnav_menu_call (dvdnav_priv->dvdnav, DVD_MENU_Root) != DVDNAV_STATUS_OK)
            if (dvdnav_menu_call (dvdnav_priv->dvdnav, DVD_MENU_Title) != DVDNAV_STATUS_OK)
                stop_playback = true;
    }
    if (! stop_playback)
        update_title_len ();

    if (stop_playback)  AUDINFO ("i:PLAY STOPPED:starting loop\n"); else AUDINFO ("i:PLAY:starting loop\n");

    /* LOOP TO FETCH AND PROCESS DATA FROM THE DVD ENGINE. */
    writeblock = false;
    while (!stop_playback)  // LOOP UNTIL SOMETHING STOPS US OR WE RUN OUT OF DVD DATA:
    {
        //if (playback_fifo_hasbeenopened) AUDERR ("PLAY:(fifo opened) LOOPING!\n"); else AUDERR ("PLAY:LOOPING!\n");
        /* unlock mutex here to avoid blocking
         * other threads must be careful not to close drive handle */
        pthread_mutex_unlock (& mutex);

        //int ret = dvdnav_get_next_block (dvdnav, buf, &event, &len);
        event = dvdnav_stream_read (dvdnav_priv, buf, &len);  // READ NEXT PACKET FROM DVD ENGINE.
        AUDDBG ("PLAY:READ STREAM: EVENT=%d=!\n", event);
        if (event == -1 || len == -1)
        {
            AUDERR ("s:DVDNAV stream read error!\n");
            stop_playback = true;
            pthread_mutex_lock (& mutex);
            goto GETMEOUTTAHERE;
        }

        pthread_mutex_lock (& mutex);
        if (event != DVDNAV_BLOCK_OK)
            dvdnav_get_highlight (dvdnav_priv, 1);

        // FROM/SEE:  https://github.com/microe/libdvdnav/blob/master/doc/tutorial.cpp:
        switch (event)  // HANDLE EACH TYPE OF DVD PACKET (INSTRUCTIONS AND DATA):
        {
            case DVDNAV_STILL_FRAME:
            {
                dvdnav_still_event_t *still_event = (dvdnav_still_event_t *)buf;
                dvdnav_priv->still_length = still_event->length;
                if (stop_playback) AUDINFO ("(STOP!) DVDNAV_STILL_FRAME len=%d\n", dvdnav_priv->still_length); else AUDINFO ("DVDNAV_STILL_FRAME len=%d\n", dvdnav_priv->still_length);
                /* set still frame duration */
                dvdnav_priv->duration = dvdnav_get_duration (dvdnav_priv->still_length);
                if (dvdnav_priv->still_length >= 255)
                {
                    AUDDBG ("i:Skipping indefinite still frame.\n");
                }
                else if (dvdnav_priv->still_length <= 1)
                {
                    pci_t *pnavpci = dvdnav_get_current_nav_pci (dvdnav_priv->dvdnav);
                    dvdnav_priv->duration = mp_dvdtimetomsec (&pnavpci->pci_gi.e_eltm);
                }
                else
                {
                    bool readblocking = readblock;
                    readblock = false;
                    dvdnav_priv->wakeup = false;
                    for (int isec=0; isec<dvdnav_priv->still_length; isec++)
                    {
                        if (dvdnav_priv->wakeup)
                            break;
                        AUDDBG ("i:SLEEPING UNTIL %d...\n", dvdnav_priv->still_length);
                        sleep (1);
                    }
                    readblock = readblocking;
                }
                dvdnav_still_skip (dvdnav_priv->dvdnav);
                if (stop_playback)
                    goto GETMEOUTTAHERE;

                nanosleep ((const struct timespec[]){{0, 40000000L}}, NULL);
                break;
            }
            case DVDNAV_HIGHLIGHT:
            {
                while (writeblock)
                {
AUDDBG("-WAIT FOR DRAINING TO STOP!\n");
                }
                AUDINFO ("DVDNAV_HIGHLIGHT\n");
                dvdnav_get_highlight (dvdnav_priv, 1);
                break;
            }
            case DVDNAV_SPU_CLUT_CHANGE:
            {
                memcpy(dvdnav_priv->spu_clut, buf, 16*sizeof(unsigned int));
                dvdnav_priv->state |= NAV_FLAG_SPU_SET;
                break;
            }
            case DVDNAV_STOP:  // DVD ENGINE SAYS STOP:
            {
                AUDINFO ("DVDNAV_STOP\n");
//                int written = fwrite (buf, 1, len, output_fd);
                dvdnav_priv->state |= NAV_FLAG_EOF;
                stop_playback = true;
                goto GETMEOUTTAHERE;
            }
            case DVDNAV_BLOCK_OK:  // AUDIO/VIDEO DATA FRAME:
            {
                while (writeblock)
                {
AUDDBG("-WB!\n");
                }
                fwrite (buf, 1, len, output_fd);
                AUDDBG ("-OK: len=%d=\n", len);
                break;
            }
            case DVDNAV_NAV_PACKET: 
            {
//                AUDINFO ("DVDNAV_NAV_PACKET: len=%d= written=%d\n", len, written);
                if (havebuttons) AUDINFO ("DVDNAV_NAV_PACKET(with buttons): len=%d\n", len); else AUDINFO ("DVDNAV_NAV_PACKET: len=%d\n", len);
                fwrite (buf, 1, len, output_fd);
     /* A NAV packet provides PTS discontinuity information, angle linking information and
      * button definitions for DVD menus. Angles are handled completely inside libdvdnav.
      * For the menus to work, the NAV packet information has to be passed to the overlay
      * engine of the player so that it knows the dimensions of the button areas. */
                if (! havebuttons)
                {
                   	pci_t *pci = dvdnav_get_current_nav_pci(dvdnav_priv->dvdnav);

     /* Applications with fifos should not use these functions to retrieve NAV packets,
      * they should implement their own NAV handling, because the packet you get from these
      * functions will already be ahead in the stream which can cause state inconsistencies.
      * Applications with fifos should therefore pass the NAV packet through the fifo
      * and decoding pipeline just like any other data. */
                   	//dvdnav_get_current_nav_dsi(dvdnav_priv->dvdnav);

                   	if (pci->hli.hl_gi.btn_ns > 0) 
                   	{
                    	   int32_t button;

                        AUDINFO ("Found %i DVD menu buttons...\n", pci->hli.hl_gi.btn_ns);

                        menubuttons.resize (pci->hli.hl_gi.btn_ns);
                     	  for (button = 0; button < pci->hli.hl_gi.btn_ns; button++)
                     	  {
                       	    btni_t *btni = &(pci->hli.btnit[button]);
                       	    //AUDINFO ("Button %i top-left @ (%i,%i), bottom-right @ (%i,%i)\n",
                          		//    button + 1, btni->x_start, btni->y_start,
                          		//    btni->x_end, btni->y_end);
                            menubuttons[button].x = btni->x_start;
                            menubuttons[button].y = btni->y_start;
                            menubuttons[button].w = btni->x_end - btni->x_start;
                            menubuttons[button].h = btni->y_end - btni->y_start;
                     	  }
                     	  havebuttons = true;
                        /* SOME DVDS DON'T HANDLE STREAM ARRANGEMENT CORRECTLY UNLESS YOU PLAY THRU THE MENU,
                           SO WE DEFER SELECTING THE DEFAULT BUTTON NOW UNTIL THE MENU HAS "PLAYED", THIS ALSO 
                           GETS THE USER THE BUMPER MUSIC THAT SOME VIDEO MENUS HAVE, AS A SIDE-EFFECT,
                           WHICH SOME MIGHT LIKE, IE. IF PLAYING AUDIO-ONLY. 
                        */

                        button = 0;
                        /* IF WE'RE SKIPPING MENUS AND IT'S OK TO DRAIN THEM, JUST ACTIVATE DEFAULT BUTTON NOW!: */
                        if (aud_get_bool ("dvd", "nomenus") && aud_get_bool ("dvd", "menudrain"))  //NO VIDEO SCREEN, SO WE MUST PRESS A MENU BUTTON TO PLAY FOR THE USER!:
                        {
                            if (dvdnav_get_current_highlight (dvdnav_priv->dvdnav, & button) != DVDNAV_STATUS_OK
                                    || button <= 0)
                                button = 1;   // NONE HIGHLIGHTED, SO JUST TRY THE FIRST ONE.
                            AUDINFO ("i:(audio only) You can't see menu so select highlighted or first button for you (%d)!...\n", button);
                            dvdnav_button_select_and_activate (dvdnav_priv->dvdnav, pci, button);
                        }
                    }
               	}
                break;
            }
            case DVDNAV_HOP_CHANNEL:
                if (dvdnav_priv->nochannelhop)
                    break;
                AUDINFO ("--------------- CHANNEL HOPPING! ------------------------\n");
                checkcodecs = true;  // RESET READER/DEMUXER LOOP & RESCAN CODECS!
                AUDDBG ("FLUSHING OUTPUT FIFO!\n");
                fflush (output_fd);
                nanosleep ((const struct timespec[]){{0, 40000000L}}, NULL);
                break;
            case DVDNAV_WAIT:
            {
                if ((dvdnav_priv->state & NAV_FLAG_WAIT_SKIP) &&
                        !(dvdnav_priv->state & NAV_FLAG_WAIT))
                    dvdnav_wait_skip (dvdnav_priv->dvdnav);
                else
                {
                    dvdnav_priv->state |= NAV_FLAG_WAIT;
                    readblock = false;
                    //nanosleep ((const struct timespec[]){{0, 500000000L}}, NULL);
                    sleep (1);  //SEEM TO NEED AT LEAST A HALF-SECOND OF SLEEP HERE!
                    AUDDBG ("WAIT:SLEEP\n");
                }
                break;
            }
            case DVDNAV_VTS_CHANGE:
            {
                int tit = 0, part = 0;
                havebuttons = false;
                menubuttons.resize (0);
                AUDINFO ("DVDNAV_VTS_CHANGE\n");
                dvdnav_vts_change_event_t *vts_event = (dvdnav_vts_change_event_t *)buf;
                AUDINFO ("VTS_CHANGE: switched to title: from %d to %d\r\n", vts_event->old_vtsN, vts_event->new_vtsN);
//                if (vts_event->old_vtsN > 0)
//                {
                    //AUDINFO ("--------CHECK THEM CODECS!!!!!\n");
                    //checkcodecs = true;
//                }
                dvdnav_priv->state |= NAV_FLAG_CELL_CHANGE;
                dvdnav_priv->state |= NAV_FLAG_AUDIO_CHANGE;
                dvdnav_priv->state |= NAV_FLAG_SPU_CHANGE;
                dvdnav_priv->state |= NAV_FLAG_STREAM_CHANGE;
                dvdnav_priv->state &= ~NAV_FLAG_WAIT_SKIP;
                dvdnav_priv->state &= ~NAV_FLAG_WAIT;
                dvdnav_priv->end_pos = 0;
                update_title_len ();
                //show_audio_subs_languages(dvdnav_priv->dvdnav);
                if (dvdnav_priv->state & NAV_FLAG_WAIT_READ_AUTO)
                    dvdnav_priv->state |= NAV_FLAG_WAIT_READ;
                if (dvdnav_current_title_info(dvdnav_priv->dvdnav, &tit, &part) == DVDNAV_STATUS_OK)
                {
                    AUDINFO ("i:VTS_CHANGE: TITLE=%d TIT=%d part=%d\r\n", dvdnav_priv->title, tit, part);
                    dvdnav_get_highlight (dvdnav_priv, 0);
//                    if (! dvdnav_priv->title && ! tit && part > 1)
                    if (! dvdnav_priv->title && ! tit)
                    {
                        if (dvdnav_priv->beenheredonedat && dvdnav_title_play (dvdnav_priv->dvdnav, 1) == DVDNAV_STATUS_OK)
                        {
                            AUDERR ("---FORCING JUMP TO MOVIE (TITLE 1)!\n");
                            //checkcodecs = true;
                            dvdnav_priv->beenheredonedat = false;
                        }
                        else
                            dvdnav_priv->beenheredonedat = true;
                    } else if (dvdnav_priv->title > 0 && tit != dvdnav_priv->title)
                    {
                        AUDERR ("i:VTS_CHANGE: SETTING NAV_FLAG TO EOF!\n");
                        dvdnav_priv->state |= NAV_FLAG_EOF;
                    }
                }
                break;
            }
            case DVDNAV_CELL_CHANGE:
            {
                AUDINFO("DVDNAV_CELL_CHANGE:\n");
                havebuttons = false;
                dvdnav_cell_change_event_t *ev =  (dvdnav_cell_change_event_t*)buf;
                uint32_t nextstill;
    
                dvdnav_priv->state &= ~NAV_FLAG_WAIT_SKIP;
                dvdnav_priv->state |= NAV_FLAG_STREAM_CHANGE;
                if (ev->pgc_length)
                    dvdnav_priv->duration = ev->pgc_length/90;

                if (dvdnav_is_domain_vts(dvdnav_priv->dvdnav))  // CHANGED TO A MOVIE:
                {
                    AUDINFO ("DVDNAV_CELL_CHANGE: new title is MOVIE (%d) -----------\n", dvdnav_priv->duration);
                    dvdnav_priv->state &= ~NAV_FLAG_VTS_DOMAIN;
                    menubuttons.resize (0);
                    dvdnav_priv->beenheredonedat = false;
                    playing_a_menu = false;
                }
                else                                            // CHANGED TO A MENU:
                {
                    AUDINFO ("DVDNAV_CELL_CHANGE: new title is MENU (%d) -----------\n", dvdnav_priv->duration);
                    dvdnav_priv->state |= NAV_FLAG_VTS_DOMAIN;
                    playing_a_menu = true;
                }

                nextstill = dvdnav_get_next_still_flag (dvdnav_priv->dvdnav);
                if (nextstill)
                {
                    dvdnav_priv->duration = dvdnav_get_duration (nextstill);
                    dvdnav_priv->still_length = nextstill;
                    if (dvdnav_priv->still_length <= 1)
                    {
                        pci_t *pnavpci = dvdnav_get_current_nav_pci (dvdnav_priv->dvdnav);
                        dvdnav_priv->duration = mp_dvdtimetomsec (&pnavpci->pci_gi.e_eltm);
                    }
                    AUDINFO ("i:We have a next STILL, len=%d= duration=%d\n", dvdnav_priv->still_length, dvdnav_priv->duration);
                }

                dvdnav_priv->state |= NAV_FLAG_CELL_CHANGE;
                dvdnav_priv->state |= NAV_FLAG_AUDIO_CHANGE;
                dvdnav_priv->state |= NAV_FLAG_SPU_CHANGE;
                dvdnav_priv->state &= ~NAV_FLAG_WAIT_SKIP;
                dvdnav_priv->state &= ~NAV_FLAG_WAIT;
                if (dvdnav_priv->state & NAV_FLAG_WAIT_READ_AUTO)
                    dvdnav_priv->state |= NAV_FLAG_WAIT_READ;
                //if(dvdnav_priv->title > 0 && dvd_last_chapter > 0)
                if (dvdnav_priv->title > 0)  // WE'RE NOT PLAYING THE TOP MENU (TRACK 0):
                {
                    int tit=0, part=0;
                    //if(dvdnav_current_title_info(dvdnav_priv->dvdnav, &tit, &part) == DVDNAV_STATUS_OK && part > dvd_last_chapter)
                    if (dvdnav_current_title_info(dvdnav_priv->dvdnav, &tit, &part) == DVDNAV_STATUS_OK)
                    {
                        AUDINFO ("e:CELL_CHANGE:  WE CUT OUT (TIT=%d, PART=%d), NEED LAST CHAPTER? --------------------\n", tit, part);
                        dvdnav_priv->state |= NAV_FLAG_EOF;
                        stop_playback = true;
                        goto GETMEOUTTAHERE;
                    }
                    AUDINFO ("i:STILL: TIT=%d, part=%d=\n", tit, part);
                }
                dvdnav_get_highlight (dvdnav_priv, 1);
                break;
            }
            case DVDNAV_AUDIO_STREAM_CHANGE:
            {
                dvdnav_audio_stream_change_event_t *ev =
                        (dvdnav_audio_stream_change_event_t*)buf;
                dvdnav_priv->state |= NAV_FLAG_AUDIO_CHANGE;
                AUDINFO ("DVDNAV_AUDIO_STREAM_CHANGE: physical=%d= prev=%d\n", ev->physical, dvdnav_priv->lastaudiostream);
                if (ev->physical >= 0 && dvdnav_priv->lastaudiostream < 0 && dvdnav_is_domain_vts (dvdnav_priv->dvdnav))
{
AUDERR("w:AUDIO CHANGE FORCED CODEC CHANGE!\n");
                    checkcodecs = true;
}
                break;
            }
            case DVDNAV_SPU_STREAM_CHANGE:   // WE'RE NOT HANDLING SUBTITLES RIGHT NOW, MAYBE SOMEDAY!:
                AUDINFO ("DVDNAV_SPU_STREAM_CHANGE\n");
                dvdnav_priv->state |= NAV_FLAG_SPU_CHANGE;
                dvdnav_priv->state |= NAV_FLAG_STREAM_CHANGE;
                break;
            default:
                AUDINFO ("Unhandled event (%i)\n", event);
        }

        if (stop_playback)
        {
            AUDINFO("CHECK_STOP: STOP_PLAYBACK\n");
            goto GETMEOUTTAHERE;
        }
    }

GETMEOUTTAHERE:
    reader_please_die = true;
    pthread_mutex_unlock (& mutex);
    AUDINFO ("WE HAVE EXITED THE PLAY LOOP, WAITING FOR READER THREAD TO STOP!...\n");
    if (pthread_join (rdmux_thread, NULL))
        AUDERR ("Error joining thread\n");
    fclose (output_fd);
    AUDINFO ("------------ END PLAY! -------------\n");
    playing = false;
    return true;
}

// from audacious:  main thread only
void DVD::cleanup ()
{
    pthread_mutex_lock (& mutex);

    reset_trackinfo ();
    purge_func.stop ();

    pthread_mutex_unlock (& mutex);
}

// from mplayer.stream_dvdnav.c:
static dvdnav_priv_t * new_dvdnav_stream(const char * filename)
{
    dvdnav_priv_t *priv;

    if (! filename)
        return NULL;

    if (! (priv = (dvdnav_priv_t *)calloc(1,sizeof(dvdnav_priv_t))))
        return NULL;

    if (! (priv->filename = strdup(filename)))
    {
        free (priv);
        return NULL;
    }

/* (THIS DOESN'T SEEM TO WORK/COMPILE BUT DOESN'T SEEM TO BE IMPORTANT):
    int speed = aud_get_int ("dvd", "disc_speed");
    if (! speed)
        speed = aud_get_int ("CDDA", "disc_speed");
    speed = aud::clamp (speed, MIN_DISC_SPEED, MAX_DISC_SPEED);
    if (speed)
        dvd_set_speed (priv->filename, speed);
*/

    if (dvdnav_open (&(priv->dvdnav), priv->filename) != DVDNAV_STATUS_OK || !priv->dvdnav)
    {
        // dvd_set_speed(priv->filename, -1);
        free (priv->filename);
        free (priv);
        return NULL;
    }

//  if (1)	//from vlc: if not used dvdnav from cvs will fail
//  {
    int len, event;
    uint8_t buf[2048];

    dvdnav_get_next_block (priv->dvdnav,buf,&event,&len);
    dvdnav_sector_search (priv->dvdnav, 0, SEEK_SET);
//  }

    /* FROM mplayer.stream_dvdnav.c: turn off dvdnav caching: */
    dvdnav_set_readahead_flag (priv->dvdnav, 0);
    if (dvdnav_set_PGC_positioning_flag (priv->dvdnav, 1) != DVDNAV_STATUS_OK)
        AUDERR ("e:stream_dvdnav, failed to set PGC positioning\n");

    String langstr = aud_get_str ("dvd", "language");
    if (langstr && langstr[0] && langstr[1])
    {
        char lang[4];   // JWT:THESE STUPID FNS TAKE A char* NOT A const char*:
        lang[0] = langstr[0];
        lang[1] = langstr[1];
        lang[2] = langstr[2];
        lang[3] = '\0';
        dvdnav_audio_language_select(priv->dvdnav, lang);
        dvdnav_menu_language_select(priv->dvdnav, lang);
        dvdnav_spu_language_select(priv->dvdnav, lang);
        priv->langid = (lang[0] << 8) | (lang[1]);
        AUDINFO ("i:Language set to (%s), code=%d!\n", (const char *)langstr, priv->langid);
    }

    return priv;
}

/* OPEN THE DVD */
static bool open_dvd ()
{
    if (dvdnav_priv && dvdnav_priv->dvdnav)
    {
        AUDINFO ("i:OPEN_DVD CALLED, DVD ALREADY OPENED, RETURNING OK!\n");
        return true;
    }

    String device = aud_get_str ("dvd", "device");
    if (! device[0])
        device = String ("/dev/dvd");

    AUDINFO ("i:Opening DVD drive: DEVICE =%s=\n", (const char *)device);

    //MAY NEED SOMEWHERE:const char *dvdnav_err_to_string (dvdnav_priv->dvdnav)

        //xif (! (pdvd_drive = cdda_identify (device, 1, nullptr)))
/*        int speed = aud_get_int ("dvd", "disc_speed");
        if (! speed)
            speed = aud_get_int ("CDDA", "disc_speed");
        speed = aud::clamp (speed, MIN_DISC_SPEED, MAX_DISC_SPEED);
        if (speed)
            dvd_set_speed ((const char *) device, speed);
*/
        //if (dvdnav_open (&dvdnav, (const char *) device) != DVDNAV_STATUS_OK)
    if (! (dvdnav_priv = new_dvdnav_stream ((const char *) device)))
    {
        // ? dvd_set_speed ((const char *) device, -1);
        AUDERR ("Failed to open DVD device %s - NOT OK.", (const char *) device);
        return false;
    }
    if (! dvdnav_priv->dvdnav)
    {
        AUDERR (_("Failed to open DVD device %s."), (const char *) device);
        return false;
    }

    /* FIGURE OUT WHERE TO PUT THE FIFO: */
    dvdnav_priv->fifo_str = aud_get_str("dvd", "fifo");
    if (! dvdnav_priv->fifo_str || ! dvdnav_priv->fifo_str[0])
        dvdnav_priv->fifo_str = String ("fifo.mpg");
#ifdef _WIN32
    if (dvdnav_priv->fifo_str[0] != '/' && dvdnav_priv->fifo_str[0] != '\\' && dvdnav_priv->fifo_str[1] != ':')
#else
    if (dvdnav_priv->fifo_str[0] != '/')
#endif
    {
        StringBuf fifo_buf = filename_build ({aud_get_path (AudPath::UserDir), (const char *)dvdnav_priv->fifo_str});
        dvdnav_priv->fifo_str = String (fifo_buf);
    }
    AUDDBG ("---- FIFO =%s=\n", (const char *)dvdnav_priv->fifo_str);

    return (bool) dvdnav_priv->dvdnav;
}

/* from audacious:  thread safe */
bool DVD::read_tag (const char * filename, VFSFile & file, Tuple & tuple, Index<char> * image)
{
    bool whole_disk = ! strcmp (filename, "dvd://");
    if (whole_disk) AUDINFO ("-read_tag: WHOLE DISK\n"); else AUDINFO ("-read_tag: SINGLE TRACK\n");
    bool valid = false;

    pthread_mutex_lock (& mutex);

    /* reset cached info when adding CD to the playlist */
    if (whole_disk && ! playing)
    {
        AUDINFO ("READ_TAG: whole disk and not playing, open DVD!\n");
        reset_trackinfo ();
        purge_func.queue (purge_all_playlists, nullptr);
    }


    if (! trackinfo.len () && ! refresh_trackinfo (true))
        goto DONE;

    if (whole_disk)  // WE'RE OPENING dvd://:
    {
        bool first_track_only = aud_get_bool ("dvd", "first_track_only");
        Index<short> subtunes;

        if (lasttrackno >= 0 && (! first_track_only || aud_get_bool ("dvd", "title_track_only")))
            subtunes.append (0);
        if (first_track_only || ! aud_get_bool ("dvd", "title_track_only"))
        {
            for (int trackno = 1; trackno <= lasttrackno; trackno++)
            {
// WE ADD ALL, LET abUSER SORT OUT!             if (cdda_track_audiop (pdvd_drive, trackno))
                if (! first_track_only || (trackinfo[trackno].endlsn-trackinfo[trackno].startlsn) > 27000000)
                {
                    subtunes.append (trackno);
                    if (first_track_only)
                        break;
                }
                AUDDBG ("--TRACK(%d): dur=%ld\n", trackinfo[trackno].endlsn-trackinfo[trackno].startlsn);
            }
        }
        tuple.set_subtunes (subtunes.len (), subtunes.begin ());

        valid = true;
    }
    else  // WE'RE LOOKING AT A SINGLE TRACK (dvd://?<#>)
    {
        int trackno = find_trackno_from_filename (filename);

        if (trackno < 0 || trackno > lasttrackno)
        {
            AUDERR ("w:Track %d not found (%s).\n", trackno, filename);
            goto DONE;
        }

        //tuple.set_format (_("DVD"), 2, 44100, 1411);
        tuple.set_int (Tuple::Track, trackno);
        tuple.set_int (Tuple::Length, calculate_track_length
                (trackinfo[trackno].startlsn, trackinfo[trackno].endlsn));

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

        valid = true;
    }
    AUDINFO ("i:DVD OPEN SUCCEEDED\n");

DONE:
    pthread_mutex_unlock (& mutex);
    return valid;
}

/* from audacious cdaudio-ng:  mutex must be locked */
static bool scan_dvd ()
{
    AUDINFO ("Scanning DVD drive...\n");
    trackinfo.clear ();

    int titles = 0;
    if (dvdnav_get_number_of_titles (dvdnav_priv->dvdnav, &titles) != DVDNAV_STATUS_OK)
    {
        dvd_error (_("Failed to retrieve first/last track number."));
        return false;
    }
    if (titles <= 0)
    {
        dvd_error (_("Failed to retrieve any titles."));
        return false;
    }
    firsttrackno = 1;
    lasttrackno = (int) titles;
    uint32_t startpos = 0;
    if (! lasttrackno)
    {
        dvd_error (_("Failed to retrieve first/last track number."));
        return false;
    }
    AUDINFO ("i:first track is %d and last track is %d\n", firsttrackno, lasttrackno);

    trackinfo.insert (0, lasttrackno + 1);

    trackinfo[0].startlsn = startpos;
    for (int trackno = firsttrackno; trackno <= lasttrackno; trackno++)
    {
        uint64_t *parts = NULL, duration = 0;
        uint32_t n = dvdnav_describe_title_chapters(dvdnav_priv->dvdnav, (uint32_t) trackno, &parts, &duration);
        if (parts)
        {
            uint32_t t = duration / 90;
            AUDINFO ("ID_DVD_TITLE_%d_LENGTH=%d.%03d; %d parts.\n", trackno, t / 1000, t % 1000, n);
            free (parts);
            trackinfo[trackno].startlsn = startpos;
            startpos += duration;
            trackinfo[trackno].endlsn = startpos;
        }
    }
    trackinfo[0].endlsn = startpos;
    /* get trackinfo[0] cdtext information (the disc) */
    //x cdtext_t *pcdtext = nullptr;
    const char * pcdtext = nullptr;
    if (aud_get_bool ("dvd", "use_cdtext"))
    {
        AUDDBG ("getting dvd-title information for disc\n");
        if (dvdnav_get_title_string (dvdnav_priv->dvdnav, &pcdtext) != DVDNAV_STATUS_OK)
            AUDINFO ("w:no dvd-title available for disc\n");
        else
        {
            trackinfo[0].name = String (pcdtext);
            AUDINFO ("i:GOT DVD TITLE=%s=\n", pcdtext);
        }
    }

    /* get track information from cdtext */
    //bool cdtext_was_available = false;
    if (! aud_get_bool ("dvd", "title_track_only"))  //IF SET, ONLY ADD SINGLE TITLE TRACK TO PLAYLIST.
    {
        StringBuf titlebuf;
        for (int trackno = firsttrackno; trackno <= lasttrackno; trackno++)  //ADD ALL TRACKS TO PLAYLIST:
        {
            //titlebuf.steal (str_printf ("%s%d", "dvd://?", trackno));
            titlebuf.steal (str_printf ("%s%d", "DVD Track ", trackno));
            trackinfo[trackno].name = String (titlebuf);
            AUDINFO ("---ADDED TRACK# %d: name=%s=\n", trackno, (const char *)trackinfo[trackno].name);
        }
    }

    return true;
}

/* from audacious cdaudio-ng:  mutex must be locked */
static bool refresh_trackinfo (bool warning)
{
    int xtry = 0;
    int maxopentries = aud_get_int ("dvd", "maxopentries");
    if (maxopentries < 1)
        maxopentries = 4;
//x    if (! open_dvd () || ! check_disc_mode (warning))
tryagain:   // WE TRY 4 TIMES TO GIVE THE DRIVE A CHANCE TO SPIN UP...
    if (! open_dvd ())
    {
        if (xtry >= maxopentries)
            goto fail;
        else
        {
            sleep (2);
            ++xtry;
            goto tryagain;
        }
    }

    if (! trackinfo.len ())
    {
        if (! scan_dvd ())
            goto fail;

        //timer_add (TimerRate::Hz1, monitor);
    }
    AUDINFO ("Success: refresh_trackinfo - DVD opened and scanned!\n");
    dvdnav_priv->langid = 0;
    dvdnav_get_title_string (dvdnav_priv->dvdnav, & dvdnav_priv->title_str);
    return true;

fail:
    AUDERR ("s:FAIL: refresh_trackinfo, couldn't open or scan DVD!\n");
    dvd_error ("Failed to open DVD.\n");
    reset_trackinfo ();
    purge_func.queue (purge_all_playlists, nullptr);
    return false;
}

/* from audacious cdaudio-ng:  mutex must be locked */
static void reset_trackinfo ()
{
    //timer_remove (TimerRate::Hz1, monitor);
    AUDINFO ("i:RESET_TRACKINFO called, will CLOSE DVD IF OPENED!\n");
    if (dvdnav_priv)
    {
        if (dvdnav_priv->dvdnav)
        {
            AUDINFO ("i:DVD WAS OPENED, CLOSING IT NOW!\n");
            dvdnav_close (dvdnav_priv->dvdnav);
            dvdnav_priv->fifo_str = String ();
            dvdnav_priv->dvdnav = nullptr;
            free (dvdnav_priv);
            dvdnav_priv = nullptr;
        }
        // dvd_set_speed (filename, -1);
    }
    trackinfo.clear ();
}

/* from audacious cdaudio-ng:  thread safe (mutex may be locked) */
static int calculate_track_length (uint32_t startlsn, uint32_t endlsn)
{
    //x return ((endlsn - startlsn + 1) * 1000) / 75;
    return (int) (endlsn - startlsn + 1) / 90;
}

/* from audacious cdaudio-ng:  thread safe (mutex may be locked) */
static int find_trackno_from_filename (const char * filename)
{
    int track;
    AUDINFO("find_trackno_from_filename: fid=%s=\n", filename);
    if (strncmp (filename, "dvd://?", 7) || sscanf (filename + 7, "%d", &track) != 1)
        return -1;

    return track;
}
