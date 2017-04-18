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
#include <string.h>
extern "C" {
#include <fcntl.h>
#include <sys/poll.h>
#include <sys/stat.h>
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
  int              mousex, mousey;
  int              title;
  int              track;
  unsigned int     spu_clut[16];
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
static bool stop_playback;
static bool playback_thread_running;
static bool playback_fifo_hasbeenopened;
static bool reader_please_die;
static bool playing_a_menu;
static bool checkcodecs;
static bool readblock;
static FILE * output_fd;
static Index<SDL_Rect> menubuttons;
static bool havebuttons;

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
    "use_cdtext", "TRUE",
    "device", "/dev/dvd",
    "use_cdtext", "TRUE",
    "title_track_only", "FALSE",
    "nomenus", "FALSE",
    "play_video", "TRUE",
    nullptr
};

const PreferencesWidget DVD::widgets[] = {
    WidgetLabel (N_("<b>Device</b>")),
    WidgetSpin (N_("Read speed:"),
        WidgetInt ("dvd", "disc_speed"),
        {MIN_DISC_SPEED, MAX_DISC_SPEED, 1}),
    WidgetEntry (N_("Override device:"),
        WidgetString ("dvd", "device")),
    WidgetLabel (N_("<b>Metadata</b>")),
    WidgetCheck (N_("Use CD-Text"),
        WidgetBool ("dvd", "use_cdtext")),
    WidgetCheck (N_("Only Title Track in Playlist"),
        WidgetBool ("dvd", "title_track_only")),
    WidgetCheck (N_("Skip Menus (Auto-select)"),
        WidgetBool ("dvd", "nomenus")),
    WidgetCheck (N_("Play video stream in popup window when video stream found"),
        WidgetBool ("dvd", "play_video"))
};

const PluginPreferences DVD::prefs = {{widgets}};

static void dvd_error (const char * message_format, ...)
{
    va_list args;
    va_start (args, message_format);
    StringBuf msg = str_vprintf (message_format, args);
    va_end (args);

    aud_ui_show_error (msg);
}

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

/* main thread only */
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

/* main thread only */
static void purge_all_playlists (void * = nullptr)
{
    int playlists = aud_playlist_count ();
    int count;

    for (count = 0; count < playlists; count++)
        purge_playlist (count);
}

/* main thread only */
//x static void monitor (void *)
//x {
//x     pthread_mutex_lock (& mutex);
//x AUDINFO ("--MONITOR!--\n");
    /* make sure not to close drive handle while playing */
//x     if (! playing)
//x         refresh_trackinfo (false);

//x     pthread_mutex_unlock (& mutex);
//x }

/* main thread only */
bool DVD::init ()
{
    aud_config_set_defaults ("DVD", defaults);

//x    if (!cdio_init ())
//x    {
//x        dvd_error (_("Failed to initialize cdio subsystem."));
//x        return false;
//x    }
    av_register_all();
    av_log_set_callback (ffaudio_log_cb);

    return true;
}

/* thread safe (mutex may be locked) */
bool DVD::is_our_file (const char * filename, VFSFile & file)
{
    return !strncmp (filename, "dvd://", 6);
}

// FROM mplayer.stream_dvdnav.c:
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

// FROM mplayer.stream_dvdnav.c:
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
        AUDINFO ("ID_AID_%d_LANG=%s\n", id, tmp);
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

// FROM mplayer.stream_dvdnav.c:
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
AUDINFO("RECEIVE FRAME FAIL: %d\n", res);
                avcodec_send_packet(vcinfo->context, nullptr);
                res = avcodec_receive_frame (vcinfo->context, vframe.ptr);
                avcodec_flush_buffers(vcinfo->context);
            }
            if (res < 0)
                return; /* read next packet (continue past errors) */
        }
        else if (playing_a_menu)
        {
//AUDERR("NULL PACKET, FLUSH BUFFERS!\n");
                avcodec_flush_buffers(vcinfo->context);
        }
#else
        frameFinished = 0;
        len = LOG (avcodec_decode_video2, vcinfo->context, vframe.ptr, &frameFinished, pkt);
        /* Did we get a video frame? */
        if (len < 0)
        {
            AUDERR ("decode_video() failed, code %d\n", len);
            return;
        }
        if (frameFinished)
        {
#endif
//if (!last_resized) AUDERR("-RESIZED=FALSE\n");
            if (last_resized)  /* BLIT THE FRAME, BUT ONLY IF WE'RE NOT CURRENTLY RESIZING THE WINDOW! */
            {
                if (*videohasnowh && vframe.ptr->width && vframe.ptr->height)
                {
                    /* Resize the screen. */
                    *resized_window_width = vframe.ptr->width;  // window's reported new size
                    *resized_window_height = vframe.ptr->height;
// AUDERR ("i:SDL_RESIZE(FRAME)!!!!!! rvw=(%d, %d) orig(%d, %d)\n", *resized_window_width, *resized_window_height, video_width, video_height);
                    *last_resized = false;  // false means now we'll need re-aspecting, so stop blitting!
                    //*videohasnowh = false;
                }
                else
                {
//if (! *windowIsStable) AUDERR("WILL WRITE VIDEO FRAME(%d,%d).\n", vframe.ptr->width, vframe.ptr->height);
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

static SDL_Renderer * createSDL2Renderer (SDL_Window * screen, bool myplay_video)
{
    SDL_Renderer * renderer = nullptr;
    if (screen && myplay_video)
        renderer = SDL_CreateRenderer (screen, -1, 0);

    return renderer;
}

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
            SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255);
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
            AUDERR ("decode_audio() failed, code %d\n", len);
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
//AUDERR("--DMUXED: SZ=%d= BFSZ=%d= outf=%d= planar=%d=\n", size, buf.len (), out_fmt, planar);
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
        AUDERR ("COULD NOT SET UP AVFORMATCONTEXT!\n");
        return nullptr;
    }
    AVInputFormat *f = av_find_input_format("mpeg");
    f->flags &= AVFMT_NOBINSEARCH | AVFMT_NOGENSEARCH | AVFMT_NO_BYTE_SEEK;
    //f->read_header = nullptr;
    //c->skip_initial_bytes = 0;
//               if (avformat_open_input( & c, "/tmp/libdvdnav.mpg", nullptr, nullptr) < 0)
    input_fd_p->fd = open ((const char *)dvdnav_priv->fifo_str, O_RDONLY);
    int pollres = 0;
    if (input_fd_p->fd)
    {
        input_fd_p->events = POLLIN;
        pollres = poll (input_fd_p, 1, 4000);
    }
    void * buf = av_malloc (2048);
    if (!buf)
    {
        AUDERR ("COULD NOT ALLOCATE AVIOContext BUFFER!\n");
        if (c)
            avformat_free_context (c);
        if (input_fd_p->fd)
            ::close (input_fd_p->fd);
        return nullptr;
    }
    AVIOContext * io = avio_alloc_context ((unsigned char *) buf, 2048, 0, input_fd_p, read_cb, nullptr, nullptr);
    if (!io)
    {
        AUDERR ("COULD NOT ALLOCATE AVIOContext!\n");
        if (c)
            avformat_free_context (c);
        if (input_fd_p->fd)
            ::close (input_fd_p->fd);
        return nullptr;
    }
    if (c)
        c->pb = io;
    else
        AUDERR("c IS NOT DEFINED!\n");
if (!input_fd_p->fd)  AUDERR("input_fd NOT DEFINED!\n");
if (!pollres) AUDERR("pollres NOT DEFINED!\n");
//    if (!input_fd_p->fd || !pollres || avformat_open_input( & c, "", f, nullptr) < 0)
    if (!input_fd_p->fd || avformat_open_input( & c, "", f, nullptr) < 0)
    {
        AUDERR ("COULD NOT OPEN AVINPUT!\n");
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
//AUDERR ("PLAY:FAILED TO FIND STREAM INFO!\n");
        if (c)
            avformat_free_context (c);
        if (input_fd_p->fd)
            close (input_fd_p->fd);
        return nullptr;
    }
    playback_fifo_hasbeenopened = true;
    return c;
}

/* separate reader/demuxer thread */
void DVD::reader_demuxer ()
{
    AUDINFO ("---- reader_demuxer started! ----\n");
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
    int video_resizedelay;     // MIN. TIME TO WAIT AFTER USER RESIZES VIDEO WINDOW BEFORE RE-ASPECTING (SEC.)
    int resized_window_width;  // VIDEO WINDOW-SIZE AFTER LAST RESIZE.
    int resized_window_height;
    bool videohasnowh;         // TRUE IF VIDEO CONTEXT DID NOT PROVIDE A WIDTH OR HEIGHT.
    uint32_t video_requested_width;   // WINDOW-SIZE REQUESTED BY VIDEO STREAM ITSELF (just initialize for sanity).
    uint32_t video_requested_height;
    int video_doreset_width;   // WINDOW-SIZE BELOW WHICH WINDOW WILL SNAP BACK TO SIZE REQUESTED BY VIDEO STREAM:
    int video_doreset_height;
    int video_qsize;
    time_t last_resizeevent_time;  // TIME OF LAST RESIZE EVENT, SO WE CAN DETERMINE WHEN SAFE TO RE-ASPECT.
    float video_aspect_ratio;  // ASPECT RATIO OF VIDEO, SAVED TO PERMIT RE-ASPECTING AFTER USER RESIZES (WARPS) WINDOW.
    bool myplay_video; // WHETHER OR NOT TO DISPLAY THE VIDEO.
    bool codec_opened;     // TRUE IF SUCCESSFULLY OPENED CODECS:
    bool vcodec_opened;
    bool planar;                   // USED BY Audacious
    //bool returnok = false;
    bool eof;
    bool last_resized;      // TRUE IF VIDEO-WINDOW HAS BEEN RE-ASPECTED SINCE LAST RESIZE EVENT (HAS CORRECT ASPECT RATIO).
    bool sdl_initialized;  // TRUE IF SDL (VIDEO) IS SUCCESSFULLY INITIALIZED.
    SDL_Event       event;         // SDL EVENTS, IE. RESIZE, KILL WINDOW, ETC.
    int video_fudge_x; int video_fudge_y;  // FUDGE-FACTOR TO MAINTAIN VIDEO SCREEN LOCN. BETWEEN RUNS.
    bool needWinSzFudge;     // TRUE UNTIL A FRAME HAS BEEN BLITTED & WE'RE NOT LETTING VIDEO DECIDE WINDOW SIZE.
    pktQueue *pktQ;      // QUEUE FOR VIDEO-PACKET QUEUEING.
    pktQueue *apktQ;     // QUEUE FOR AUDIO-PACKET QUEUEING.
    SDL_Window * screen = nullptr;  /* JWT: MUST DECLARE VIDEO SCREEN-WINDOW HERE */
    struct pollfd input_fd_p;
#ifdef _WIN32
    SDL_Texture * bmp = nullptr;    // CAN'T USE SMARTPTR HERE IN WINDOWS - renderer.get() FAILS IF VIDEO PLAY NOT TURNED ON?!
#endif

    int videoStream;
    int audioStream;
    AVPacket pkt;
    CodecInfo cinfo, vcinfo;  //AUDIO AND VIDEO CODECS
    AVFormatContext * c;

startover:
    AUDINFO ("---- reader_demuxer starting over! ----\n");
    out_fmt = 0;
    errcount = 0;
    vx = 0;
    vy = 0;
    video_width = 0;     // INITIAL VIDEO WINDOW-SIZE:
    video_height = 0;
    video_window_x = 0;  // VIDEO WINDOW POSITION AND SIZE WHEN PROGRAM LAST CLOSED:
    video_window_y = 0;
    video_window_w = 0;
    video_window_h = 0;
    video_resizedelay = 1;     // MIN. TIME TO WAIT AFTER USER RESIZES VIDEO WINDOW BEFORE RE-ASPECTING (SEC.)
    resized_window_width = 0;  // VIDEO WINDOW-SIZE AFTER LAST RESIZE.
    resized_window_height = 0;
    video_requested_width = 720;   // WINDOW-SIZE REQUESTED BY VIDEO STREAM ITSELF (just initialize for sanity).
    video_requested_height = 480;
    videohasnowh = false;
    video_doreset_width = 0;   // WINDOW-SIZE BELOW WHICH WINDOW WILL SNAP BACK TO SIZE REQUESTED BY VIDEO STREAM:
    video_doreset_height = 0;
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
    sdl_initialized = false;  // TRUE IF SDL (VIDEO) IS SUCCESSFULLY INITIALIZED.
    video_fudge_x = 0; 
    video_fudge_y = 0;  // FUDGE-FACTOR TO MAINTAIN VIDEO SCREEN LOCN. BETWEEN RUNS.
    needWinSzFudge = true;     // TRUE UNTIL A FRAME HAS BEEN BLITTED & WE'RE NOT LETTING VIDEO DECIDE WINDOW SIZE.
    pktQ = nullptr;      // QUEUE FOR VIDEO-PACKET QUEUEING.
    apktQ = nullptr;     // QUEUE FOR AUDIO-PACKET QUEUEING.
    c = open_input_file (& input_fd_p);
    //SmartPtr<AVFormatContext, close_input_file> c (open_input_file ());
    if (!c)
    {
        AUDERR ("COULD NOT OPEN_INPUT_FILE!\n");
        stop_playback = true;
        playback_thread_running = false;
        return;
    }
//    videoStream = av_find_best_stream (c, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0) || -1;
//    audioStream = av_find_best_stream (c, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0) || -1;
//    /* HACK! */  if (audioStream && audioStream == videoStream)  videoStream = 0;
//    videoStream = 0;
//    audioStream = 1;
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
            AUDDBG ("--HAVE A VIDEO CODEC!\n");
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
            vcodec_opened = true;
            AUDINFO ("got VIDEO codec %s for stream index %d, opening; w=%d= h=%d=\n", vcinfo.codec->name, vcinfo.stream_idx,
                    vcinfo.context->width, vcinfo.context->height);
        }
        else
            play_video = false;  /* turn off video playback, since no video codec! */
    }
    else
        play_video = false;  /* turn off video playback, since we could not find a video stream! */
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
    for (unsigned int x=0; x<c->nb_streams; x++)  // JWT:HELPFUL FOR DEBUGGING CODEC ISSUES:
    {
        AVCodec * xcodec = avcodec_find_decoder (c->streams[x]->codecpar->codec_id);
        AUDINFO ("CODEC NAME(%d)=%s=\n", x, xcodec->name);
    }
    AUDINFO ("----VIDEO STREAM=%d=  aud=%d= count=%d=\n", videoStream, audioStream, c->nb_streams);
    myplay_video = play_video;

    if (vcodec_opened && LOG (avcodec_open2, vcinfo.context, vcinfo.codec, nullptr) < 0)
        vcodec_opened = false;

    if (!vcodec_opened) AUDERR("UNABLE TO OPEN VIDEO CODEC!\n");
    if (codec_opened && LOG (avcodec_open2, cinfo.context, cinfo.codec, nullptr) < 0)
        codec_opened = false;

    if (codec_opened && ! convert_format (cinfo.context->sample_fmt, out_fmt, planar))
        codec_opened = false;

    set_stream_bitrate (c->bit_rate);
    if (codec_opened)
    {
        AUDINFO ("---OPEN_AUDIO(%d, %d, %d)\n", out_fmt, cinfo.context->sample_rate, cinfo.context->channels);
        open_audio (out_fmt, cinfo.context->sample_rate, cinfo.context->channels);
    }

    /* JWT: IF abUSER ALSO WANTS TO PLAY VIDEO THEN WE SET UP POP-UP VIDEO SCREEN: */
    if (playing_a_menu)
        readblock = false;
    if (myplay_video)
    {
        AUDDBG ("--PLAYING VIDEO!\n");
        String video_windowtitle;
        String song_title;
        //int current_playlist = aud_playlist_get_active ();
        //Tuple tuple = aud_playlist_entry_get_tuple (current_playlist, aud_playlist_get_position (current_playlist));
        //song_title = tuple.get_str (Tuple::Title);
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

        int video_xmove = aud_get_int ("dvd", "video_xmove");
        /*  -1:always let windowmanager place (random); 0(DEPRECIATED/SDL1):place window via
            SDL_putenv() - may work with Windows?; 1(default):relocate window via SDL; 
            2:(both 0, then 1).  This is sometimes useful for multiple X
            desktops where the default of placing the window via SDL_putenv will ALWAYS
            place the window in the same desktop that Fauxdacious is in.  By setting to 1,
            the window will be moved to the proper location relative to the current
            desktop, and Fauxdacious is treated as "sticky" by the window manager.  Setting
            to 2 MAY be useful IF for some reason, neither 0 nor 1 work properly.
        */
        video_window_x = video_window_y = video_window_w = video_window_h = 0;
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
        /* NOW CALCULATE THE WIDTH, HEIGHT, & ASPECT BASED ON VIDEO'S SIZE & AND ANY USER PARAMATERS GIVEN:
            IDEALLY, ONE SHOULD ONLY SET X OR Y AND LET Fauxdacious CALCULATE THE OTHER DIMENSION,
            SO THAT THE ASPECT RATIO IS MAINTAINED, THOUGH ONE CAN SPECIFY BOTH AND FORCE
            THE ASPECT TO BE ADJUSTED TO FIT.  IF A SINGLE ONE IS SPECIFIED AS "-1", THEN
            THE NEW WINDOW WILL KEEP THE SAME VALUE FOR THAT DIMENSION AS THE PREV. WINDOW,
            AND ADJUST THE OTHER DIMENTION ACCORDINGLY TO FIT THE NEW VIDEO'S ASPECT RATIO.
            IF BOTH ARE SPECIFIED AS "-1", USE PREVIOUSLY-SAVED WINDOW SIZE REGUARDLESS OF ASPECT RATIO.
        */
        dvdnav_get_video_resolution (dvdnav_priv->dvdnav, & video_requested_width, & video_requested_height);
        AUDINFO ("----- VIDEO REQUESTED (%d x %d) or (%d x %d)-----\n", vcinfo.context->width, 
                vcinfo.context->height, video_requested_width, video_requested_height);
        if (!vcinfo.context->width && !vcinfo.context->height)
            videohasnowh = true;
        if (!vcinfo.context->width)
            vcinfo.context->width = video_requested_width;     
        if (!vcinfo.context->height)
            vcinfo.context->height = video_requested_height;     
        video_aspect_ratio = vcinfo.context->height
            ? (float)vcinfo.context->width / (float)vcinfo.context->height : 1.0;
        vx = aud_get_int ("dvd", "video_xsize");
        vy = aud_get_int ("dvd", "video_ysize");
        if (vx && !vy)   /* User specified (or saved) width only, calc. height based on aspect: */
        {
            video_width = (vx == -1) ? (video_window_w ? video_window_w : vcinfo.context->width) : vx;
            video_height = (int)((float)video_width / video_aspect_ratio);
        }
        else if (!vx && vy)   /* User specified (or saved) height only, calc. width based on aspect: */
        {
            video_height = (vy == -1) ? (video_window_h ? video_window_h : vcinfo.context->height) : vy;
            video_width = (int)((float)video_height * video_aspect_ratio);
        }
        else if (vx && vy)   /* User specified fixed width and height: */
        {
            if (vx == -1 && vy == -1)  /* Use same (saved) settings or video's settings (SCREW THE ASPECT)! */
            {
                video_width = video_window_w ? video_window_w : vcinfo.context->width;
                video_height = video_window_h ? video_window_h : vcinfo.context->height;
            }
            else if (vx == -1)  /* Use same (saved) height & calculate new width based on aspect: */
            {
                video_height = vy;
                video_width = (int)((float)video_height * video_aspect_ratio);
            }
            else if (vy == -1)  /* Use same (saved) width & calculate new height based on aspect: */
            {
                video_width = vx;
                video_height = (int)((float)video_width / video_aspect_ratio);
            }
            else  /* User specified window size (SCREW THE ASPECT)! */
            {
                video_width = vx;
                video_height = vy;
            }
        }
        else   /* User specified nothing, use the video's desired wXh (& ignore saved settings!): */
        {
            video_width = vcinfo.context->width;
            video_height = vcinfo.context->height;
        }
        video_requested_width = vcinfo.context->width;
        video_requested_height = vcinfo.context->height;
        if (playing_a_menu)
        {
            if ((uint32_t)video_width != video_requested_width)
            {
                float f;
                for (int mbtn = 0; mbtn < menubuttons.len (); mbtn ++)
                {
                    //AUDDBG ("---- xBEF(%d): x=%d, w=%d;  vw=%d, vRw=%d\n", mbtn, menubuttons[mbtn].x, menubuttons[mbtn].w, video_width, video_requested_width);
                    f = (float)video_width / (float)video_requested_width;
                    menubuttons[mbtn].w = (int)(f * (float)menubuttons[mbtn].w);
                    menubuttons[mbtn].x = (int)(f * (float)menubuttons[mbtn].x);
                    //AUDDBG ("---- xAFT(%d): x=%d, w=%d;  f=%.2f\n", mbtn, menubuttons[mbtn].x, menubuttons[mbtn].w, f);
                }
            }
            if ((uint32_t)video_height != video_requested_height)
            {
                float f;
                for (int mbtn = 0; mbtn < menubuttons.len (); mbtn ++)
                {
                    //AUDDBG ("---- yBEF(%d): y=%d, h=%d;  vh=%d, vRh=%d\n", mbtn, menubuttons[mbtn].y, menubuttons[mbtn].h, video_width, video_requested_width);
                    f = (float)video_height / (float)video_requested_height;
                    menubuttons[mbtn].h = (int)(f * (float)menubuttons[mbtn].h);
                    menubuttons[mbtn].y = (int)(f * (float)menubuttons[mbtn].y);
                    //AUDDBG ("---- yAFT(%d): y=%d, h=%d;  f=%.2f\n", mbtn, menubuttons[mbtn].y, menubuttons[mbtn].h, f);
                }
            }
        }
        video_aspect_ratio = video_height
            ? (float)video_width / (float)video_height : 1.0;   /* Fall thru to square to avoid possibliity of "/0"! */

        /* NOW "RESIZE" screen to user's wXh, if user set something: */
        SDL_SetMainReady ();
        if (SDL_InitSubSystem (SDL_INIT_VIDEO) < 0)
        {
            AUDERR ("Failed to init SDL (no video playing): %s.\n", SDL_GetError ());
            myplay_video = false;
            goto breakout1;
        }
        sdl_initialized = true;
        Uint32 flags = SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE;
    	   if (aud_get_bool ("dvd", "allow_highdpi"))
    	       flags |= SDL_WINDOW_ALLOW_HIGHDPI;

        screen = SDL_CreateWindow ("Fauxdacious DVD", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 
            video_width, video_height, flags);
        if (! screen)
        {
            AUDERR ("Failed to create SDL window (no video playing): %s.\n", SDL_GetError ());
            myplay_video = false;
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
        SDL_SetWindowSize (screen, video_width, video_height);

        if (video_xmove > 0)  // (1 or 2)
            SDL_SetWindowPosition (screen, video_window_x, video_window_y);
        if (aud_get_str ("dvd", "video_render_scale"))
            SDL_SetHint (SDL_HINT_RENDER_SCALE_QUALITY, aud_get_str ("dvd", "video_render_scale"));
        else
            SDL_SetHint (SDL_HINT_RENDER_SCALE_QUALITY, "1");
        SDL_SetWindowTitle (screen, (const char *) titleBuf);
    }

breakout1:

    /* JWT:video_qsize:  MAX # PACKETS TO QUEUE UP FOR INTERLACING TO SMOOTH VIDEO
        PLAYBACK - GOOD RANGE IS 6-28, DEFAULT IS 8:
        NOT ENOUGH = JITTERY VIDEO
        TOO MANY = AUDIO/VIDEO BECOME NOTICABLY OUT OF SYNC!
    */
    video_qsize = aud_get_int ("dvd", "video_qsize");
    if (video_qsize < 1)
        video_qsize = (aud_get_int ("ffaudio", "video_qsize"))
                ? aud_get_int ("ffaudio", "video_qsize") : 8;
    if (video_qsize < 1)
        video_qsize = 8;

    /* TYPICALLY THERE'S TWICE AS MANY AUDIO PACKETS AS VIDEO, SO THIS IS COUNTER-INTUITIVE, BUT IT WORKS BEST! */
    pktQ = createQueue (2 * video_qsize);
    apktQ = createQueue (video_qsize);

    {   // SUBSCOPE FOR DECLARING SDL2 TEXTURE AS SCOPED SMARTPOINTER:
    bool windowIsStable = false;    // JWT:SAVING AND RECREATING WINDOW CAUSES POSN. TO DIFFER BY THE WINDOW DECORATION SIZES, SO WE HAVE TO FUDGE FOR THAT!
    bool windowNowExposed = false;  // JWT:NEEDED TO PREVENT RESIZING WINDOW BEFORE EXPOSING ON MS-WINDOWS?!
    int seek_value;
    SmartPtr<SDL_Renderer, SDL_DestroyRenderer> renderer (createSDL2Renderer (screen, myplay_video));
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
        AUDERR("NO VIDEO DUE TO INABILITY TO GET TEXTURE!\n");
        myplay_video = false;
    }

    /* OUTER LOOP TO READ, QUEUE AND PROCESS AUDIO & VIDEO PACKETS FROM THE STREAM: */
    update_title_len ();
    if (! vcodec_opened)  AUDERR("UNABLE TO OPEN VIDEO CODEC2!\n");
    if (! playing_a_menu && ! codec_opened)  AUDERR("UNABLE TO OPEN AUDIO CODEC2!!!!!!!!!!!!!!!!\n");
    if (! myplay_video)  AUDERR("NO VIDEO WHEN STARTING LOOP!\n");
    AUDDBG ("READER-DEMUXER: STARTING LOOP\n");
    while (! reader_please_die)
    {
        if (checkcodecs && !codec_opened)
        {
            goto error_exit;
        }
        /* CHECK IF USER MOVED THE SEEK/POSITION SLIDER, IF SO FLUSH QUEUES AND SEEK TO NEW POSITION: */
        seek_value = check_seek ();
        if (seek_value >= 0)
        {
            /* JWT:FIRST, FLUSH ANY PACKETS SITTING IN THE QUEUES TO CLEAR THE QUEUES! */
            QFlush (apktQ);
            QFlush (pktQ);
            //if (LOG (av_seek_frame, ic.get (), -1, (int64_t) seek_value *
            //        AV_TIME_BASE / 1000, AVSEEK_FLAG_BACKWARD) >= 0)
            int faudlen = aud_drct_get_length ();
            if (! playing_a_menu && faudlen > 0)
            {
                uint32_t dvpos = 0, dvlen = 0;
                dvdnav_get_position(dvdnav_priv->dvdnav, &dvpos, &dvlen);
                AUDDBG ("**** fSEEK =%d= dvpos=%d= dvlen=%d= flen=%d= endpos=%ld=\n", seek_value, dvpos, dvlen, faudlen, dvdnav_priv->end_pos);
                int64_t newpos = (int)(((float)dvlen / (float)faudlen) * (float)seek_value);  // CONVERT FAUXDACIOUS SEEKIES INTO DVD SEEKIES!
                if (dvdnav_priv->end_pos && newpos > dvdnav_priv->end_pos)
                    newpos = dvdnav_priv->end_pos;
                if (dvdnav_sector_search (dvdnav_priv->dvdnav, (uint64_t) newpos, SEEK_SET) != DVDNAV_STATUS_OK)
                    AUDERR ("e:COULD NOT SEEK!\n");
            }
            errcount = 0;
            seek_value = -1;
        }

        pkt = AVPacket ();
        av_init_packet (& pkt);
//AUDERR("---WILL READ A FRAME!\n");
        ret = LOG (av_read_frame, c, & pkt);
//if (playing_a_menu)  AUDERR("---READ FRAME, res=%d\n", ret);
        if (ret < 0)  // CHECK FOR EOF OR ERRORS:
        {
            ++errcount;
            if (ret == (int) AVERROR_EOF)
            {
//if (playing_a_menu) AUDERR ("eof reached in menu, continue\n"); else AUDERR ("eof reached in movie ********************\n");
                av_free_packet (& pkt);
                eof = true;
                if (! playing_a_menu)
                {
                    stop_playback = true;
                    checkcodecs = false;
                    readblock = false;
                }
                else if (vcodec_opened)
                {
                    AVPacket emptypkt = AVPacket ();
                    av_init_packet (& emptypkt);
                    emptypkt.data=nullptr; emptypkt.size=0;
                    write_videoframe (renderer.get (), & vcinfo, bmpptr, & emptypkt, 
                                  video_width, video_height, & last_resized, & videohasnowh, & windowIsStable,
                                  & resized_window_width, & resized_window_height);
                    av_free_packet (& emptypkt);
                }
//                if (vcodec_opened && last_resized)
//                {
//                    SDL_RenderPresent (renderer.get ());  // only blit a single frame at startup will get refreshed!
//                    windowNowExposed = true;
//                }
            }
            else if (errcount > 4)
            {
                AUDERR ("av_read_frame error %d, giving up.\n", ret);
                av_free_packet (& pkt);
#if SDL == 2
#ifdef _WIN32
                if (bmp)  // GOTTA FREE THIS BEFORE WE LEAVE SCOPE!
                {
                    SDL_DestroyTexture (bmp);
                    SDL_Delay (50);
                    bmp = nullptr;
                }
#endif
#endif
                goto error_exit;
            }
            else
            {
                AUDERR ("ERROR READING PACKET, TRY AGAIN?\n");
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
                if (playing_a_menu)
                {
                    write_audioframe (& cinfo, & pkt, out_fmt, planar);
                    av_free_packet (& pkt);
                }
                else
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
                        if (playing_a_menu)
                            write_videoframe (renderer.get (), & vcinfo, bmpptr, & pkt, 
                                  video_width, video_height, & last_resized, & videohasnowh, & windowIsStable,
                                  & resized_window_width, & resized_window_height);
                        else
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
        while (SDL_PollEvent (&event))
        {
            switch (event.type) {
                case SDL_MOUSEBUTTONDOWN: // if the event is mouse click
//                    if(event.button == SDL_BUTTON_LEFT)
                    if (playing_a_menu)
                    {
//AUDERR("--1MOUSE BUTTON PRESSED...\n");
                        for (int mbtn = 0; mbtn < menubuttons.len (); mbtn ++)
                        {
//AUDERR("---1CHECK MENU BUTTON %d...\n", mbtn);
                            if (event.motion.x >= menubuttons[mbtn].x && event.motion.x <= menubuttons[mbtn].w
                                && event.motion.y >= menubuttons[mbtn].y && event.motion.y <= menubuttons[mbtn].h)
                            {
                                AUDINFO ("---------- 1MENU BUTTON %d SELECTED! --------------\n", (mbtn+1));
//                                fflush (output_fd);
                                pci_t *pci;   //dvdnav_is_domain_vts(dvdnav_priv->dvdnav)
                                pci = dvdnav_get_current_nav_pci(dvdnav_priv->dvdnav);
                                //dvdnav_get_current_nav_dsi(dvdnav_priv->dvdnav);
                                dvdnav_button_select_and_activate (dvdnav_priv->dvdnav, pci, (mbtn+1));
                                checkcodecs = true;
                                codec_opened = false;
                                playing_a_menu = ! (dvdnav_is_domain_vts (dvdnav_priv->dvdnav));
                                dvdnav_priv->wakeup = true;
                                dvdnav_get_current_nav_dsi(dvdnav_priv->dvdnav);
                                goto error_exit;
                            }
                        }
                    }
                case SDL_WINDOWEVENT:
                    switch (event.window.event)
                    {
                        case SDL_WINDOWEVENT_CLOSE:  /* USER CLICKED THE "X" IN UPPER-RIGHT CORNER, KILL VIDEO WINDOW BUT KEEP PLAYING AUDIO! */
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
                    videohasnowh = false;
                }
                /* NOW MANUALLY ADJUST EITHER THE WIDTH OR HEIGHT BASED ON USER'S CONFIG. TO RESTORE 
                   THE NEW WINDOW TO THE PROPER ASPECT RATIO FOR THE CURRENTLY-PLAYING VIDEO:
                */
                if (vy == -1)  // USER SAYS ADJUST HEIGHT TO MATCH WIDTH:
                {
                    new_video_width = resized_window_width;
                    new_video_height = (int)((float)new_video_width / video_aspect_ratio);
                }
                else if (vx == -1)  // USER SAYS ADJUST WIDTH TO MATCH HEIGHT:
                {
                    new_video_height = resized_window_height;
                    new_video_width = (int)(video_aspect_ratio * (float)new_video_height);
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
                    new_video_width = video_requested_width;
                    new_video_height = video_requested_height;
                }
                if (playing_a_menu)
                {
                    if (video_width != new_video_width)
                    {
                        float f;
                        for (int mbtn = 0; mbtn < menubuttons.len (); mbtn ++)
                        {
                            f = (float)new_video_width / (float)video_width;
                            menubuttons[mbtn].w = (int)(f * (float)menubuttons[mbtn].w);
                            menubuttons[mbtn].x = (int)(f * (float)menubuttons[mbtn].x);
                        }
                    }
                    if (video_height != new_video_height)
                    {
                        float f;
                        for (int mbtn = 0; mbtn < menubuttons.len (); mbtn ++)
                        {
                            f = (float)new_video_height / (float)video_height;
                            menubuttons[mbtn].h = (int)(f * (float)menubuttons[mbtn].h);
                            menubuttons[mbtn].y = (int)(f * (float)menubuttons[mbtn].y);
                        }
                    }
                }
                video_width = new_video_width;
                video_height = new_video_height;
AUDERR(" ----RESIZED TO(%d, %d)\n", video_width, video_height);
                /* NOW MANUALLY RESIZE (RE-ASPECT) WINDOW BASED ON VIDEO'S ORIGINALLY-CALCULATED ASPECT RATIO: */
                SDL_SetWindowSize (screen, video_width, video_height);
                SDL_Delay (50);
                last_resized = true;  // WE'VE RE-ASPECTED, SO ALLOW BLITTING TO RESUME!
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
        if (codec_opened && check_stop ())   //check_stop NO WORKEE W/O AUDIO PLAYIN'!
        {
            stop_playback = true;
            checkcodecs = false;
            readblock = false;
            AUDINFO ("USER PRESSED STOP BUTTON!!!!!!!!!!!!!!!!!!!!!!!!!\n");
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
    if (myplay_video)  /* WE WERE PLAYING VIDEO && FINISHED WITH VIDEO-WINDOW STILL INTACT (NOT CLOSED BY USER PRESSING WINDOW'S CORNER [X]) */
    {
        AUDDBG ("i:ffaudio: QUITTING VIDEO!\n");
        if (screen)  // bmp ALREADY FREED & NOW OUT OF SCOPE BUT MAKE SURE VIDEO WINDOW IS FREED & GONE!
        {
            save_window_xy (screen, video_fudge_x, video_fudge_y);
            SDL_DestroyWindow (screen);
            screen = nullptr;
        }
        if (sdl_initialized)
            SDL_QuitSubSystem (SDL_INIT_VIDEO);
    }

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
        checkcodecs = false;
        playback_thread_running = false;
        goto startover;
    }
    playback_thread_running = false;
    AUDINFO ("READER THREAD RETURNING!\n");
    return;
}

/* play thread only */
bool DVD::play (const char * name, VFSFile & file)
{
    pthread_mutex_lock (& mutex);

    play_video = false;
    pthread_t rdmux_thread;
    playback_thread_running = false;
    playback_fifo_hasbeenopened = false;
    reader_please_die = false;
    playing_a_menu = false;
    checkcodecs = false;
AUDINFO("OPENING FIFO (w+, should not block!)\n");

// SEE:  http://www.linux-mag.com/id/357/; http://man7.org/linux/man-pages/man2/poll.2.html

    int trackno = find_trackno_from_filename (name);
    AUDINFO ("PLAY:  ============ STARTING TO PLAY DVD (Track# %d NAME=%s) =============\n", trackno, name); 
    dvdnav_priv->track = trackno;

    playing = true;

    //int buffer_size = aud_get_int (nullptr, "output_buffer_size");
    int speed = aud_get_int ("dvd", "disc_speed");
    speed = aud::clamp (speed, MIN_DISC_SPEED, MAX_DISC_SPEED);
    //int sectors = aud::clamp (buffer_size / 2, 50, 250) * speed * 75 / 1000;
    //int retry_count = 0, skip_count = 0;

    int len, event;
    uint8_t buf[2048];
    struct stat statbuf;

    //AVFormatContext * c = nullptr;
    stop_playback = false;
AUDINFO ("PLAY:FIFO OPENEDfixing to start loop\n");
    if (stat (dvdnav_priv->fifo_str, &statbuf) && mkfifo((const char *)dvdnav_priv->fifo_str, S_IWUSR | S_IRUSR | S_IRGRP | S_IROTH))
    {
        AUDERR ("Error creating playback fifo at: (%s)\n", (const char *)dvdnav_priv->fifo_str);
        stop_playback = true;
    }

#ifndef NODEMUXING
    if (pthread_create (&rdmux_thread, nullptr, reader_thread, this))
    {
        AUDERR ("Error creating playback reader thread\n");
        stop_playback = true;
    }
    else
#endif
        output_fd = fopen ((const char *)dvdnav_priv->fifo_str, "w");

    if (dvdnav_priv->track > 0)
    {
        if (dvdnav_title_play (dvdnav_priv->dvdnav, dvdnav_priv->track) != DVDNAV_STATUS_OK)
        {
            AUDERR ("f:dvdnav_stream, couldn't select title %d, error '%s'\n", dvdnav_priv->track, dvdnav_err_to_string (dvdnav_priv->dvdnav));
            pthread_mutex_unlock (& mutex);
            return false;
        }
        //show_audio_subs_languages(dvdnav_priv->dvdnav);
        AUDINFO ("ID_DVD_CURRENT_TITLE=%d\n", dvdnav_priv->track);
    }
    else
    {
        AUDINFO ("PLAY: CALLING TOP MENU!\n");
        if (dvdnav_menu_call (dvdnav_priv->dvdnav, DVD_MENU_Root) != DVDNAV_STATUS_OK)
            dvdnav_menu_call (dvdnav_priv->dvdnav, DVD_MENU_Title);
    }

    update_title_len ();
    //if (!stream->pos && p->track > 0)
       //AUDERR ("INIT ERROR: couldn't get init pos %s\r\n", dvdnav_err_to_string (dvdnav_priv->dvdnav));

if (stop_playback) AUDINFO ("PLAY STOPPED:starting loop\n"); else AUDINFO ("PLAY:starting loop\n");

    //while (!stop_playback && ! check_stop ())
    while (!stop_playback)
    {
//if (playback_fifo_hasbeenopened) AUDERR ("PLAY:(fifo opened) LOOPING!\n"); else AUDERR ("PLAY:LOOPING!\n");
        /* unlock mutex here to avoid blocking
         * other threads must be careful not to close drive handle */
        pthread_mutex_unlock (& mutex);

        //int ret = dvdnav_get_next_block (dvdnav, buf, &event, &len);
        event = dvdnav_stream_read (dvdnav_priv, buf, &len);
        AUDDBG ("PLAY:READ STREAM: EVENT=%d=!\n", event);
        if (event == -1 || len == -1)
        {
            AUDERR ("DVDNAV stream read error!\n");
            stop_playback = true;
            pthread_mutex_lock (& mutex);
            goto GETMEOUTTAHERE;
        }

        pthread_mutex_lock (& mutex);
        if (event != DVDNAV_BLOCK_OK)
            dvdnav_get_highlight (dvdnav_priv, 1);

        switch (event)
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
AUDERR("SLEEPING UNTIL %d...\n", dvdnav_priv->still_length);
                        sleep (1);
                    }
                    readblock = readblocking;
                }
                dvdnav_still_skip (dvdnav_priv->dvdnav);
                if (stop_playback)
                    goto GETMEOUTTAHERE;
                break;
            }
            case DVDNAV_HIGHLIGHT:
            {
                AUDINFO ("DVDNAV_HIGHLIGHT\n");
                dvdnav_get_highlight (dvdnav_priv, 1);
                break;
            }
            case DVDNAV_SPU_CLUT_CHANGE: {
                memcpy(dvdnav_priv->spu_clut, buf, 16*sizeof(unsigned int));
                dvdnav_priv->state |= NAV_FLAG_SPU_SET;
                break;
            }
            case DVDNAV_STOP:
                AUDINFO ("DVDNAV_STOP\n");
//                int written = fwrite (buf, 1, len, output_fd);
                dvdnav_priv->state |= NAV_FLAG_EOF;
                stop_playback = true;
                goto GETMEOUTTAHERE;
            case DVDNAV_BLOCK_OK:
            {
                int written = fwrite (buf, 1, len, output_fd);
if (playing_a_menu) AUDINFO ("DVDNAV_BLOCK_OKAAAAAAAAAAAAAY: len=%d= written=%d\n", len, written);
                break;
            }
            case DVDNAV_NAV_PACKET: 
            {
// CAUSES SEGFAULTS!:                int written = fwrite (buf, 1, len, output_fd);
//                AUDINFO ("DVDNAV_NAV_PACKET: len=%d= written=%d\n", len, written);
                if (havebuttons) AUDINFO ("DVDNAV_NAV_PACKET(with buttons): len=%d\n", len); else AUDINFO ("DVDNAV_NAV_PACKET: len=%d\n", len);
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
                   	dvdnav_get_current_nav_dsi(dvdnav_priv->dvdnav);

                   	if (pci->hli.hl_gi.btn_ns > 0) 
                   	{
                    	   int32_t button;

                        AUDINFO ("Found %i DVD menu buttons...\n", pci->hli.hl_gi.btn_ns);

                        if (pci->hli.hl_gi.btn_ns > menubuttons.len ())
                            menubuttons.resize (pci->hli.hl_gi.btn_ns);
                     	  for (button = 0; button < pci->hli.hl_gi.btn_ns; button++)
                     	  {
                       	    btni_t *btni = &(pci->hli.btnit[button]);
                       	    //AUDINFO ("Button %i top-left @ (%i,%i), bottom-right @ (%i,%i)\n",
                          		//    button + 1, btni->x_start, btni->y_start,
                          		//    btni->x_end, btni->y_end);
                            menubuttons[button].x = btni->x_start;
                            menubuttons[button].y = btni->y_start;
                            menubuttons[button].w = btni->x_end;
                            menubuttons[button].h = btni->y_end;
                     	  }
                     	  havebuttons = true;

                        button = 0;
//button = 1;
                        if (! play_video || aud_get_bool ("dvd", "nomenus"))  //NO VIDEO SCREEN, SO WE MUST PRESS A MENU BUTTON TO PLAY FOR THE USER!:
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
            case DVDNAV_WAIT:
            {
                if ((dvdnav_priv->state & NAV_FLAG_WAIT_SKIP) &&
                    !(dvdnav_priv->state & NAV_FLAG_WAIT))
                {
//AUDERR("WAIT STATE 1 len=%d=\n", len);
                    dvdnav_wait_skip (dvdnav_priv->dvdnav);
                }
                else
                {
//AUDERR("WAIT STATE 2 len=%d=\n", len);
                    dvdnav_priv->state |= NAV_FLAG_WAIT;
                }
                if (dvdnav_priv->state & NAV_FLAG_WAIT)
                {
                    readblock = false;
                    AUDINFO ("WAIT(NO STOP) (%d) \n", len);
                }
                
                break;
            }
            case DVDNAV_VTS_CHANGE:
            {
                int tit = 0, part = 0;
                havebuttons = false;
                AUDINFO ("DVDNAV_VTS_CHANGE\n");
                dvdnav_vts_change_event_t *vts_event = (dvdnav_vts_change_event_t *)buf;
                AUDINFO ("VTS_CHANGE: switched to title: %d from %d\r\n", vts_event->new_vtsN, vts_event->old_vtsN);
                dvdnav_priv->state |= NAV_FLAG_CELL_CHANGE;
                dvdnav_priv->state |= NAV_FLAG_AUDIO_CHANGE;
                dvdnav_priv->state |= NAV_FLAG_SPU_CHANGE;
                dvdnav_priv->state |= NAV_FLAG_STREAM_CHANGE;
                dvdnav_priv->state &= ~NAV_FLAG_WAIT_SKIP;
                dvdnav_priv->state &= ~NAV_FLAG_WAIT;
                dvdnav_priv->end_pos = 0;
                update_title_len ();
                show_audio_subs_languages(dvdnav_priv->dvdnav);
                if (dvdnav_priv->state & NAV_FLAG_WAIT_READ_AUTO)
                    dvdnav_priv->state |= NAV_FLAG_WAIT_READ;
                if (dvdnav_current_title_info(dvdnav_priv->dvdnav, &tit, &part) == DVDNAV_STATUS_OK)
                {
                    AUDINFO ("VTS_CHANGE: TITLE=%d TIT=%d part=%d\r\n", dvdnav_priv->title, tit, part);
                    dvdnav_get_highlight (dvdnav_priv, 0);
                    if(dvdnav_priv->title > 0 && tit != dvdnav_priv->title)
                    {
AUDINFO ("VTS_CHANGE: SETTING NAV_FLAG TO EOF!\n");
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

                if (dvdnav_is_domain_vts(dvdnav_priv->dvdnav))
                {
                    AUDINFO ("DVDNAV_CELL_CHANGE: new title is MOVIE (%d) -----------\n", dvdnav_priv->title);
                    dvdnav_priv->state &= ~NAV_FLAG_VTS_DOMAIN;
                  menubuttons.resize (0);
                  playing_a_menu = false;
                }
                else
                {
                    AUDINFO ("DVDNAV_CELL_CHANGE: new title is MENU (%d) -----------\n", dvdnav_priv->title);
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
                    AUDERR ("We have a STILL, len=%d=\n", dvdnav_priv->still_length);
                }

                dvdnav_priv->state |= NAV_FLAG_CELL_CHANGE;
                dvdnav_priv->state |= NAV_FLAG_AUDIO_CHANGE;
                dvdnav_priv->state |= NAV_FLAG_SPU_CHANGE;
                dvdnav_priv->state &= ~NAV_FLAG_WAIT_SKIP;
                dvdnav_priv->state &= ~NAV_FLAG_WAIT;
                if (dvdnav_priv->state & NAV_FLAG_WAIT_READ_AUTO)
                    dvdnav_priv->state |= NAV_FLAG_WAIT_READ;
                //if(dvdnav_priv->title > 0 && dvd_last_chapter > 0)
                if (dvdnav_priv->title > 0)
                {
                    int tit=0, part=0;
                    //if(dvdnav_current_title_info(dvdnav_priv->dvdnav, &tit, &part) == DVDNAV_STATUS_OK && part > dvd_last_chapter)
                    if (dvdnav_current_title_info(dvdnav_priv->dvdnav, &tit, &part) == DVDNAV_STATUS_OK)
                    {
                        AUDERR ("CELL_CHANGE:  WE CUT OUT (TIT=%d, PART=%d), NEED LAST CHAPTER? --------------------\n", tit, part);
                        dvdnav_priv->state |= NAV_FLAG_EOF;
                        stop_playback = true;
                        goto GETMEOUTTAHERE;
                    }
                    AUDERR ("STILL: TIT=%d, part=%d=\n", tit, part);
                }
                dvdnav_get_highlight (dvdnav_priv, 1);
                break;
            }
            case DVDNAV_AUDIO_STREAM_CHANGE:
                AUDINFO("DVDNAV_AUDIO_STREAM_CHANGE\n");
                dvdnav_priv->state |= NAV_FLAG_AUDIO_CHANGE;
                break;
            case DVDNAV_SPU_STREAM_CHANGE:
                AUDINFO("DVDNAV_AUDIO_STREAM_CHANGE\n");
                dvdnav_priv->state |= NAV_FLAG_SPU_CHANGE;
                dvdnav_priv->state |= NAV_FLAG_STREAM_CHANGE;
                break;
            default:
                AUDINFO ("Unhandled event (%i)\n", event);
        }
            
            
//         if (sectors > 16)
//        {
            /* maybe a smaller read size will help */
//            sectors /= 2;
//        }
//        else if (retry_count < MAX_RETRIES)
//        {
            /* still failed; retry a few times */
//            retry_count ++;
//        }
//        else if (skip_count < MAX_SKIPS)
//        {
            /* maybe the disk is scratched; try skipping ahead */
//            currlsn = aud::min (currlsn + 75, endlsn + 1);
//            skip_count ++;
//        }
//        else
//        {
            /* still failed; give it up */
//            dvd_error (_("Error reading DVD."));
//            break;
//        }
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
    playing = false;
    fclose (output_fd);
    AUDINFO ("------------ END PLAY! -------------\n");
    return true;
}

/* main thread only */
void DVD::cleanup ()
{
    pthread_mutex_lock (& mutex);

    reset_trackinfo ();
    purge_func.stop ();

    pthread_mutex_unlock (& mutex);
}

// FROM mplayer.stream_dvdnav.c:
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

/*    int speed = aud_get_int ("dvd", "disc_speed");
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
        AUDERR ("stream_dvdnav, failed to set PGC positioning\n");
    /* report the title?! */
    //if (dvdnav_get_title_string (priv->dvdnav, & priv->title_str) == DVDNAV_STATUS_OK)
    //    AUDDBG ("ID_DVD_VOLUME_ID=%s\n", priv->title_str);

    //dvdnav_event_clear (priv);

    return priv;
}

static bool open_dvd ()
{
    if (dvdnav_priv && dvdnav_priv->dvdnav)
    {
        AUDINFO ("OPEN_DVD CALLED, DVD ALREADY OPENED, RETURNING OK!\n");
        return true;
    }

    String device = aud_get_str ("dvd", "device");
    if (! device[0])
        device = String ("/dev/dvd");

    AUDINFO ("---- Opening DVD drive: DEVICE =%s=\n", (const char *)device);

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
        AUDERR (_("Failed to open DVD device %s - NOT OK."), (const char *) device);
        return false;
    }
    if (! dvdnav_priv->dvdnav)
    {
        AUDERR (_("Failed to open DVD device %s."), (const char *) device);
        return false;
    }

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

/* thread safe */
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

    if (whole_disk)
    {
        Index<short> subtunes;

        if (lasttrackno >= 0)
            subtunes.append (0);
        if (! aud_get_bool ("dvd", "title_track_only"))
        {
            for (int trackno = 1; trackno <= lasttrackno; trackno++)
// WE ADD ALL, LET abUSER SORT OUT!             if (cdda_track_audiop (pdvd_drive, trackno))
                subtunes.append (trackno);
        }
        tuple.set_subtunes (subtunes.len (), subtunes.begin ());

        valid = true;
    }
    else
    {
        int trackno = find_trackno_from_filename (filename);

        if (trackno < 0 || trackno > lasttrackno)
        {
            AUDERR ("Track %d not found (%s).\n", trackno, filename);
            goto DONE;
        }

        //x if (!cdda_track_audiop (pdvd_drive, trackno))
        //x {
        //x     AUDERR ("Track %d is a data track.\n", trackno);
        //x     goto DONE;
        //x }

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
    AUDINFO (" DVD OPEN SUCCEEDED\n");

DONE:
    pthread_mutex_unlock (& mutex);
    return valid;
}

/* mutex must be locked */
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
    //x firsttrackno = cdio_get_first_track_num (pdvd_drive->p_cdio);
    //x lasttrackno = cdio_get_last_track_num (pdvd_drive->p_cdio);
    if (titles <= 0)
    {
        dvd_error (_("Failed to retrieve any titles."));
        return false;
    }
    firsttrackno = 1;
    lasttrackno = (int) titles;
    uint32_t startpos = 0;
    //x if (firsttrackno == CDIO_INVALID_TRACK || lasttrackno == CDIO_INVALID_TRACK)
    if (! lasttrackno)
    {
        AUDERR ("Failed to retrieve first/last track number.");
        return false;
    }
    AUDINFO ("first track is %d and last track is %d\n", firsttrackno, lasttrackno);

    trackinfo.insert (0, lasttrackno + 1);

    //x trackinfo[0].startlsn = cdda_track_firstsector (pdvd_drive, 0);
    //x trackinfo[0].endlsn = cdda_track_lastsector (pdvd_drive, lasttrackno);

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
            AUDERR ("no dvd-title available for disc\n");
        else
        {
            trackinfo[0].name = String (pcdtext);
            AUDINFO ("GOT DVD TITLE=%s=\n", pcdtext);
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

/* mutex must be locked */
static bool refresh_trackinfo (bool warning)
{
    String langstr = aud_get_str ("dvd", "language");
//x    if (! open_dvd () || ! check_disc_mode (warning))
    if (! open_dvd ())
        goto fail;

//x    if (! trackinfo.len () || cdio_get_media_changed (pdvd_drive->p_cdio))
    if (! trackinfo.len ())
    {
        if (! scan_dvd ())
            goto fail;

        //timer_add (TimerRate::Hz1, monitor);
    }
    AUDINFO ("Success: refresh_trackinfo - DVD opened and scanned!\n");
    dvdnav_priv->langid = 0;
    dvdnav_get_title_string (dvdnav_priv->dvdnav, & dvdnav_priv->title_str);
    if (langstr && langstr[0] && langstr[1])
    {
        char lang[3];   // JWT:THESE STUPID FNS TAKE A char* NOT A const char*:
        lang[0] = langstr[0];
        lang[1] = langstr[1];
        lang[2] = 0;
        dvdnav_audio_language_select(dvdnav_priv->dvdnav, lang);
        dvdnav_menu_language_select(dvdnav_priv->dvdnav, lang);
        dvdnav_spu_language_select(dvdnav_priv->dvdnav, lang);
        dvdnav_priv->langid = (lang[0] << 8) | (lang[1]);
        AUDINFO ("i:Language set to (%s), code=%d!\n", (const char *)langstr, dvdnav_priv->langid);
    }
    return true;

fail:
    AUDERR ("FAIL: refresh_trackinfo, couldn't open or scan DVD!\n");
    reset_trackinfo ();
    purge_func.queue (purge_all_playlists, nullptr);
    return false;
}

/* mutex must be locked */
static void reset_trackinfo ()
{
    //timer_remove (TimerRate::Hz1, monitor);
    AUDINFO ("RESET_TRACKINFO called, will CLOSE DVD IF OPENED!\n");
    //x if (pdvd_drive != nullptr)
    //x {
        //x cdda_close (pdvd_drive);
        //x pdvd_drive = nullptr;
    //x }
    if (dvdnav_priv)
    {
        if (dvdnav_priv->dvdnav)
        {
            AUDINFO ("DVD WAS OPENED, CLOSING IT NOW!\n");
            dvdnav_close (dvdnav_priv->dvdnav);
            dvdnav_priv->dvdnav = nullptr;
            free (dvdnav_priv);
            dvdnav_priv = nullptr;
        }
        // dvd_set_speed (filename, -1);
    }
    trackinfo.clear ();
}

/* thread safe (mutex may be locked) */
static int calculate_track_length (uint32_t startlsn, uint32_t endlsn)
{
    //x return ((endlsn - startlsn + 1) * 1000) / 75;
    return (int) (endlsn - startlsn + 1) / 90;
}

/* thread safe (mutex may be locked) */
static int find_trackno_from_filename (const char * filename)
{
    int track;
    AUDINFO("find_trackno_from_filename: fid=%s=\n", filename);
    if (strncmp (filename, "dvd://?", 7) || sscanf (filename + 7, "%d", &track) != 1)
        return -1;

    return track;
}
