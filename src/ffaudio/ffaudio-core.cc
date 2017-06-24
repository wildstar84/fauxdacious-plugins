/*
 * Fauxdacious FFaudio Plugin
 * Copyright © 2009 William Pitcock <nenolod@dereferenced.org>
 *                  Matti Hämäläinen <ccr@tnsp.org>
 * Copyright © 2011-2016 John Lindgren <john.lindgren@aol.com>
 * Video-playing capability added - copyright © 2015-2017 Jim Turner <turnerjw784@yahoo.com>
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
 */

/* Force SDL to v2, as required by Fauxdacious, as switch no longer set by config! */
#define SDL 2

#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#define SDL_MAIN_HANDLED
#include <SDL.h>

#undef FFAUDIO_DOUBLECHECK  /* Doublecheck probing result for debugging purposes */
#undef FFAUDIO_NO_BLACKLIST /* Don't blacklist any recognized codecs/formats */

#include "ffaudio-stdinc.h"

#include <pthread.h>

#include <audacious/audtag.h>
#include <libaudcore/audstrings.h>
#ifdef _WIN32
#include <libaudcore/drct.h>
#endif
#include <libaudcore/i18n.h>
#include <libaudcore/multihash.h>
#include <libaudcore/runtime.h>
#include <libaudcore/playlist.h>
#include <libaudcore/preferences.h>
#include <libaudcore/probe.h>

extern "C" {
#if SDL != 2
#include <SDL_syswm.h>
#include <libswscale/swscale.h>
#endif
}

// #define SDL_AUDIO_BUFFER_SIZE 4096
// #define MAX_AUDIO_FRAME_SIZE 192000

#if SDL != 2
#ifndef _WIN32
#include <X11/Xlib.h>
#endif
#endif

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

class FFaudio : public InputPlugin
{
public:
    static const char about[];
    static const char * const exts[], * const mimes[];
    static const char *const schemes[];
    static const char * const defaults[];
    static const PreferencesWidget widgets[];
    static const PluginPreferences prefs;

    static constexpr PluginInfo info = {
        N_("FFmpeg Plugin"),
        PACKAGE,
        about,
        & prefs
    };

    constexpr FFaudio () : InputPlugin (info, InputInfo (FlagWritesTag)
        .with_priority (10) /* lowest priority fallback */
        .with_exts (exts)
        .with_schemes (schemes)
        .with_mimes (mimes)) {}

    bool init ();
    void cleanup ();

    bool is_our_file (const char * filename, VFSFile & file);
    bool read_tag (const char * filename, VFSFile & file, Tuple & tuple, Index<char> * image);
    bool write_tuple (const char * filename, VFSFile & file, const Tuple & tuple);
    void write_audioframe (CodecInfo * cinfo, AVPacket * pkt, int out_fmt, bool planar);
#if SDL == 2
    void write_videoframe (SDL_Renderer * renderer, CodecInfo * vcinfo, 
            SDL_Texture * bmp, AVPacket *pkt, int video_width, 
            int video_height, bool last_resized, bool * windowIsStable);
#else
    void write_videoframe (SwsContext * sws_ctx, CodecInfo * vcinfo, 
            SDL_Overlay * bmp, AVPacket *pkt, int video_width, 
            int video_height, bool last_resized);
#endif
    bool play (const char * filename, VFSFile & file);
};

EXPORT FFaudio aud_plugin_instance;

const char * const FFaudio::defaults[] = {
    "play_video", "TRUE",
    "video_qsize", "6",
    "video_windowtitle", "Fauxdacious Video",  // APPEND TO VIDEO WINDOW-TITLE
    "video_xmove", "1",     // RESTORE WINDOW TO PREV. SAVED POSITION.
    "video_ysize", "-1",    // ADJUST WINDOW WIDTH TO MATCH PREV. SAVED HEIGHT.
    "save_video", "FALSE",  // DUB VIDEO AS BEING PLAYED.
#ifdef _WIN32
    "save_video_file", "C:\\Temp\\lastvideo",
#else
    "save_video_file", "/tmp/lastvideo",
#endif
    nullptr
};

const PreferencesWidget FFaudio::widgets[] = {
    WidgetLabel (N_("<b>Advanced</b>")),
    WidgetCheck (N_("Play video stream in popup window when video stream found"),
        WidgetBool ("ffaudio", "play_video")),
    WidgetSpin (N_("Video packet queue size"),
        WidgetInt ("ffaudio", "video_qsize"), {0, 24, 1}),
    WidgetCheck (N_("Record video to file"),
        WidgetBool ("ffaudio", "save_video")),
    WidgetEntry (N_("Record path/filename:"),
        WidgetString ("ffaudio", "save_video_file"),
        {false},
        WIDGET_CHILD)
};

const PluginPreferences FFaudio::prefs = {{widgets}};

static bool play_video;  /* JWT: TRUE IF USER IS CURRENTLY PLAYING VIDEO (KILLING VID. WINDOW TURNS OFF)! */
#if SDL != 2
#ifdef _WIN32
HWND hwnd;
#endif
#endif

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
        return false;
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

static SimpleHash<String, AVInputFormat *> extension_dict;

static void create_extension_dict ();

static int lockmgr (void * * mutexp, enum AVLockOp op)
{
    switch (op)
    {
    case AV_LOCK_CREATE:
        * mutexp = new pthread_mutex_t;
        pthread_mutex_init ((pthread_mutex_t *) * mutexp, nullptr);
        break;
    case AV_LOCK_OBTAIN:
        pthread_mutex_lock ((pthread_mutex_t *) * mutexp);
        break;
    case AV_LOCK_RELEASE:
        pthread_mutex_unlock ((pthread_mutex_t *) * mutexp);
        break;
    case AV_LOCK_DESTROY:
        pthread_mutex_destroy ((pthread_mutex_t *) * mutexp);
        delete (pthread_mutex_t *) * mutexp;
        break;
    }

    return 0;
}

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

bool FFaudio::init ()
{
    AUDINFO ("Starting up FFaudio.\n");
    
    aud_config_set_defaults ("ffaudio", defaults);
    av_register_all ();
    av_lockmgr_register (lockmgr);

    create_extension_dict ();

    av_log_set_callback (ffaudio_log_cb);

    return true;
}

void FFaudio::cleanup ()
{
    AUDINFO ("Shutting down FFaudio.\n");

    aud_set_bool ("ffaudio", "save_video", false);  // JWT:MAKE SURE WE DON'T LEAVE VIDEO RECORDING ON!
    extension_dict.clear ();
    av_lockmgr_register (nullptr);
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

static void create_extension_dict ()
{
    AVInputFormat * f;
    for (f = av_iformat_next (nullptr); f; f = av_iformat_next (f))
    {
        if (! f->extensions)
            continue;

        StringBuf exts = str_tolower (f->extensions);
        Index<String> extlist = str_list_to_index (exts, ",");

        for (auto & ext : extlist)
            extension_dict.add (ext, std::move (f));
    }
}

static AVInputFormat * get_format_by_extension (const char * name)
{
    StringBuf ext = uri_get_extension (name);
    if (! ext)
        return nullptr;

    AUDDBG ("Get format by extension: %s\n", name);
    AVInputFormat * * f = extension_dict.lookup (String (str_tolower (ext)));

    if (f && * f)
        AUDDBG ("Format %s.\n", (* f)->name);
    else
        AUDDBG ("Format unknown.\n");

    return f ? * f : nullptr;
}

static AVInputFormat * get_format_by_content (const char * name, VFSFile & file)
{
    AUDDBG ("Get format by content: %s\n", name);

    AVInputFormat * f = nullptr;

    unsigned char buf[16384 + AVPROBE_PADDING_SIZE];
    int size = 16;
    int filled = 0;
    int target = 100;
    int score = 0;

    while (1)
    {
        if (filled < size)
            filled += file.fread (buf + filled, 1, size - filled);

        memset (buf + filled, 0, AVPROBE_PADDING_SIZE);
        AVProbeData d = {name, buf, filled};
        score = target;

        f = av_probe_input_format2 (& d, true, & score);
        if (f)
            break;

        if (size < 16384 && filled == size)
            size *= 4;
        else if (target > 10)
            target = 10;
        else
            break;
    }

    if (f)
        AUDDBG ("Format %s, buffer size %d, score %d.\n", f->name, filled, score);
    else
        AUDDBG ("Format unknown.\n");

    if (file.fseek (0, VFS_SEEK_SET) < 0)
        ; /* ignore errors here */

    return f;
}

static AVInputFormat * get_format (const char * name, VFSFile & file)
{
    AVInputFormat * f = get_format_by_extension (name);
    return f ? f : get_format_by_content (name, file);
}

static AVFormatContext * open_input_file (const char * name, VFSFile & file)
{
    AVFormatContext * c = nullptr;

    play_video = aud_get_bool ("ffaudio", "play_video");  /* JWT:RESET PLAY-VIDEO, CASE TURNED OFF ON PREV. PLAY. */
    if (! strcmp (name, "-") || strstr (name, "://-."))   /* (DEPRECIATED?) WE'RE OPENING UP stdin */
    {
        AUDDBG ("-open_input_file (stdin)\n");
        const char * xname = "pipe:";
        if (LOG (avformat_open_input, & c, xname, nullptr, nullptr) < 0)
            return nullptr;
    }
    else if (strstr (name, "ytdl://"))  /* (DEPRECIATED?) WE'RE OPENING A PIPED-IN STREAM (LIKE stdin but w/an open filehandle), NO SEEKING! */
    {
        const char * xname = "pipe:";
        AUDDBG ("-open_input_file for youtube-dl (%s)\n", xname);
        AVInputFormat * f = nullptr;
        if (! file)
        {
            AUDDBG ("i:File wasn't opened sooner, so we're gonna open it now(%s)\n", name);
            file = VFSFile (name, "r");
            if (! file)
                return nullptr;
        }
        f = get_format (name, file);
        if (! f)
        {
            AUDERR ("e:Unknown format for %s.\n", name);
            return nullptr;
        }
        c = avformat_alloc_context ();
        AVIOContext * io = io_context_new2 (file);
        if (c)
            c->pb = io;
        if (LOG (avformat_open_input, & c, xname, f, nullptr) < 0)
        {
            if (c)
                avformat_free_context (c);
            if (io)
                io_context_free (io);
            return nullptr;
        }
    }
    else  /* WE'RE OPENING A NORMAL SEEKABLE FILE OR STREAM. */
    {
        AUDDBG ("-open_input_file (%s)\n", name);
        AVInputFormat * f = nullptr;
        if (! file)
        {
            AUDDBG ("i:File wasn't opened sooner(probe or read_tag), so we're gonna open it now(%s)\n", name);
            file = VFSFile (name, "r");
            if (! file)
                return nullptr;
        }
        else
        {
            AUDDBG ("i:REWINDING!\n");
            if (file.fseek (0, VFS_SEEK_SET) < 0)
                AUDWARN ("w:Could not rewind file?! (%s)\n", name);
        }
        f = get_format (name, file);
        if (! f)
        {
            AUDERR ("e:Unknown format for %s.\n", name);
            return nullptr;
        }
        c = avformat_alloc_context ();
        AVIOContext * io = io_context_new (file);
        if (c)
            c->pb = io;
        if (LOG (avformat_open_input, & c, name, f, nullptr) < 0)
        {
            if (c)
                avformat_free_context (c);
            if (io)
                io_context_free (io);
            return nullptr;
        }
    }

    return c;
}

static void close_input_file (AVFormatContext * c)
{
    AUDDBG ("-close_input_file.\n");

    if (c)
    {
        if (c->pb)
        {
            if (strcmp (c->filename, "pipe:"))
                io_context_free (c->pb);
        }
#if CHECK_LIBAVFORMAT_VERSION (53, 25, 0, 53, 17, 0)
        avformat_close_input (&c);
#else
        av_close_input_file (c);
#endif
        avformat_free_context (c);
    }
}

static bool find_codec (AVFormatContext * c, CodecInfo * cinfo, CodecInfo * vcinfo)
{
    if (avformat_find_stream_info (c, nullptr) < 0)
        return false;

    int audioStream = av_find_best_stream (c, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    int videoStream = (vcinfo && play_video) ? av_find_best_stream (c, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0) : -1;

#ifndef ALLOC_CONTEXT
#define codecpar codec
#endif
    if (audioStream < 0)   /* PUNT IF NO AUDIO SINCE AUDACIOUS IS AN *AUDIO* PLAYER! */
    {
        if (videoStream >= 0)
            AUDERR ("e:VIDEO-ONLY stream found (no audio, no play, use a video player!)\n");
        return false;
    }

    AVCodec * codec = avcodec_find_decoder (c->streams[audioStream]->codecpar->codec_id);
    if (codec)
    {
        cinfo->stream_idx = audioStream;
        cinfo->stream = c->streams[audioStream];
        cinfo->codec = codec;
#ifdef ALLOC_CONTEXT
        cinfo->context = avcodec_alloc_context3 (cinfo->codec);
        avcodec_parameters_to_context (cinfo->context, c->streams[audioStream]->codecpar);
#else
        cinfo->context = c->streams[audioStream]->codec;  // AVCodecContext *
#endif

        /* JWT: NOW IF USER WANTS VIDEO, SEE IF WE GOT A VIDEO STREAM TOO: */
        if (videoStream >= 0)
        {
            AVCodec * vcodec = avcodec_find_decoder (c->streams[videoStream]->codecpar->codec_id);
            if (vcodec)
            {
                vcinfo->stream_idx = videoStream;
                vcinfo->stream = c->streams[videoStream];
                vcinfo->codec = vcodec;
#ifdef ALLOC_CONTEXT
                vcinfo->context = avcodec_alloc_context3 (vcinfo->codec);
                avcodec_parameters_to_context (vcinfo->context, c->streams[videoStream]->codecpar);
#else
                vcinfo->context = c->streams[videoStream]->codec;  // AVCodecContext *
#endif
                //JWT:AS/OF v3.8, LOW-QUALITY VIDEOS SEEM BETTER W/O THIS, BUT WE LEAVE IT AS A CONFIG. OPTION - YMMV:
                if (aud_get_bool ("ffaudio", "video_codec_flag_truncated") && vcodec->capabilities&CODEC_CAP_TRUNCATED)
                    vcinfo->context->flags |= CODEC_FLAG_TRUNCATED; /* we do not send complete frames */
                if (aud_get_bool ("ffaudio", "video_codec_flag_gray"))
                    vcinfo->context->flags |= CODEC_FLAG_GRAY; /* output in monochrome (REQUIRES FFMPEG COMPILED W/--enable-gray!) */
            }
            else
                play_video = false;  /* TURN OFF VIDEO PLAYBACK, SINCE NO VIDEO CODEC! */
        }
        else
            play_video = false;  /* TURN OFF VIDEO PLAYBACK, SINCE WE COULD NOT FIND A VIDEO STREAM! */

        return true;
    }
#undef codecpar

    return false;
}

bool FFaudio::is_our_file (const char * filename, VFSFile & file)
{
    return (bool) get_format (filename, file);
}

static const struct {
    Tuple::ValueType ttype;  /* Tuple field value type */
    Tuple::Field field;      /* Tuple field constant */
    const char * keys[5];    /* Keys to match (case-insensitive), ended by nullptr */
} metaentries[] = {
    {Tuple::String, Tuple::Artist, {"author", "hor", "artist", nullptr}},
    {Tuple::String, Tuple::Title, {"title", "le", nullptr}},
    {Tuple::String, Tuple::Album, {"album", "WM/AlbumTitle", nullptr}},
    {Tuple::String, Tuple::Performer, {"performer", nullptr}},
    {Tuple::String, Tuple::Copyright, {"copyright", nullptr}},
    {Tuple::String, Tuple::Genre, {"genre", "WM/Genre", nullptr}},
    {Tuple::String, Tuple::Comment, {"comment", nullptr}},
    {Tuple::String, Tuple::Composer, {"composer", nullptr}},
    {Tuple::Int, Tuple::Year, {"year", "WM/Year", "date", nullptr}},
    {Tuple::Int, Tuple::Track, {"track", "WM/TrackNumber", nullptr}},
};

static void read_metadata_dict (Tuple & tuple, AVDictionary * dict)
{
    for (auto & meta : metaentries)
    {
        AVDictionaryEntry * entry = nullptr;

        for (int j = 0; ! entry && meta.keys[j]; j ++)
            entry = av_dict_get (dict, meta.keys[j], nullptr, 0);

        if (entry && entry->value)
        {
            if (meta.ttype == Tuple::String)
                tuple.set_str (meta.field, entry->value);
            else if (meta.ttype == Tuple::Int)
                tuple.set_int (meta.field, atoi (entry->value));
        }
    }
}

bool FFaudio::read_tag (const char * filename, VFSFile & file, Tuple & tuple, Index<char> * image)
{
    bool fromstdin = (!strcmp (filename, "-") || strstr (filename, "://-.")) ? true : false;
    AUDDBG ("i:read_tag: fid=%s=\n", filename);
    if (fromstdin || strstr (filename, "ytdl://"))  /* (DEPRECIATED?) NON-SEEKABLE STREAMS */
    {
        tuple.set_filename (filename);  /* All we can do here is just get the file name. */
        if (! fromstdin)  // WE'RE A YOUTUBE-DL STREAM, GET TAGS FROM SPECIAL TAG FILE CREATED BY THE HELPER SCRIPT (IF AVAILABLE):
        {
            Tuple file_tuple = Tuple ();
            if (aud_read_tag_from_tagfile (filename, "youtubedl_tag_data", file_tuple))
            {
                //tuple = std::move (file_tuple);
                const char * tfld = (const char *) file_tuple.get_str (Tuple::Title);
                if (tfld)
                    tuple.set_str (Tuple::Title, tfld);
                tfld = (const char *) file_tuple.get_str (Tuple::Artist);
                if (tfld)
                    tuple.set_str (Tuple::Artist, tfld);
                String lasturl = aud_get_str ("youtubedl", "lasturl");
                if (lasturl == String (filename))
                {
                    int restore_length = aud_get_int ("youtubedl", "lastlen");
                    if (restore_length && restore_length > 0)
                        tuple.set_int (Tuple::Length, restore_length);
                }
            }
        }
    }
    else  /* JWT:THIS STUFF DEFERRED UNTIL PLAY() FOR STDIN(nonseekable), BUT SEEMS TO HAVE TO BE HERE FOR DIRECT */
    {
        SmartPtr<AVFormatContext, close_input_file> ic (open_input_file (filename, file));
        if (! ic)
            return false;

        CodecInfo cinfo;

        if (! find_codec (ic.get (), & cinfo, nullptr))   //CAN CHANGE play_video!
            return false;

        if ((int)ic->duration != 0)
            tuple.set_int (Tuple::Length, ic->duration / 1000);
        else
            tuple.unset (Tuple::Length);

        tuple.set_int (Tuple::Bitrate, ic->bit_rate / 1000);

        if (cinfo.codec->long_name)
            tuple.set_str (Tuple::Codec, cinfo.codec->long_name);

        if (ic->metadata)
            read_metadata_dict (tuple, ic->metadata);
        if (cinfo.stream->metadata)
            read_metadata_dict (tuple, cinfo.stream->metadata);

        if (! file.fseek (0, VFS_SEEK_SET))
            audtag::read_tag (file, tuple, image);

#if CHECK_LIBAVFORMAT_VERSION (54, 2, 100, 54, 2, 0)
        if (image && ! image->len ())
        {
            for (unsigned i = 0; i < ic->nb_streams; i ++)
            {
                if (ic->streams[i]->attached_pic.size > 0)
                {
                    image->insert ((char *) ic->streams[i]->attached_pic.data, 0,
                        ic->streams[i]->attached_pic.size);
                    break;
                }
            }
        }
#endif
#ifdef ALLOC_CONTEXT
        avcodec_free_context (& cinfo.context);
        av_free (cinfo.context);
#else
        avcodec_close (cinfo.context);
#endif
    }

    return true;
}

bool FFaudio::write_tuple (const char * filename, VFSFile & file, const Tuple & tuple)
{
    if (str_has_suffix_nocase (filename, ".ape"))
        return audtag::write_tuple (file, tuple, audtag::TagType::APE);

#if SDL == 2
    return audtag::write_tuple (file, tuple, audtag::TagType::None);
#else
    /* JWT:FIXME: FOR SOME REASON ABOVE LINE WON'T COMPILE WITH SDL_syswm.h AND X11/Xlib.h INCLUDED?!?!
        "error: expected unqualified-id before numeric constant return audtag::tuple_write (tuple, file, audtag::TagType::None);"
        SO REPLACED WITH NEXT LINE:
    */
    //return audtag::write_tuple (file, tuple, audtag::TagType::ID3v2);
    return audtag::write_tuple (file, tuple, (audtag::TagType) 0);
#endif
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

void FFaudio::write_audioframe (CodecInfo * cinfo, AVPacket * pkt, int out_fmt, bool planar)
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

/* JWT: NEW FUNCTION TO WRITE VIDEO FRAMES TO THE POPUP WINDOW: */
#if SDL == 2
void FFaudio::write_videoframe (SDL_Renderer * renderer, CodecInfo * vcinfo, 
    SDL_Texture * bmp, AVPacket *pkt, int video_width, 
    int video_height, bool last_resized, bool * windowIsStable)
{
#else
void FFaudio::write_videoframe (SwsContext * sws_ctx, CodecInfo * vcinfo, 
    SDL_Overlay * bmp, AVPacket *pkt, int video_width, 
    int video_height, bool last_resized)
{
#endif
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
        if ((LOG (avcodec_receive_frame, vcinfo->context, vframe.ptr)) < 0)
            return; /* read next packet (continue past errors) */
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
            if (last_resized)  /* BLIT THE FRAME, BUT ONLY IF WE'RE NOT CURRENTLY RESIZING THE WINDOW! */
            {
#if SDL == 2
                //SDL_RenderClear (renderer);
                SDL_UpdateYUVTexture (bmp, nullptr, vframe->data[0], vframe->linesize[0], 
                    vframe->data[1], vframe->linesize[1], vframe->data[2], vframe->linesize[2]);
                SDL_RenderCopy (renderer, bmp, nullptr, nullptr);  // USE NULL TO GET IMAGE TO FIT WINDOW!
                SDL_RenderPresent (renderer);
                (*windowIsStable) = true;
#else
                SDL_Rect rect;
                AVFrame pict = { { 0 } };
                SDL_LockYUVOverlay (bmp);
                pict.data[0] = bmp->pixels[0];
                pict.data[1] = bmp->pixels[2];
                pict.data[2] = bmp->pixels[1];
                pict.linesize[0] = bmp->pitches[0];
                pict.linesize[1] = bmp->pitches[2];
                pict.linesize[2] = bmp->pitches[1];
                /* Convert the image into YUV format that SDL uses. */
                sws_scale (sws_ctx, (uint8_t const * const *)vframe->data,
                    vframe->linesize, 0, vcinfo->context->height,
                    pict.data, pict.linesize);
                SDL_UnlockYUVOverlay (bmp);

                rect.x = 0;
                rect.y = 0;
                rect.w = video_width;
                rect.h = video_height;
                SDL_DisplayYUVOverlay (bmp, &rect);
#endif
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

#if SDL == 2
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
//            SDL_RenderClear (renderer);
            SDL_RenderPresent (renderer);
            SDL_ShowWindow (screen);
        }
    }

    return texture;
}
#endif

/* WHEN EXITING PLAY, WE SAVE THE WINDOW-POSITION & SIZE SO WINDOW CAN POP UP IN SAME POSITION NEXT TIME! */
#if SDL == 2
void save_window_xy (SDL_Window * screen, int video_fudge_x, int video_fudge_y)
{
    int x, y, w, h;

    SDL_GetWindowSize (screen, &w, &h);
    if (w < 1 || h < 1)  /* SDL RETURNED BAD WINDOW INFO, DON'T SAVE! */
        return;

    SDL_GetWindowPosition (screen, &x, &y);
    x += video_fudge_x;  /* APPLY CALCULATED FUDGE-FACTOR */
    if (x < 0)
        x = 1;
    y += video_fudge_y;
    if (y < 0)
        y = 1;
    aud_set_int ("ffaudio", "video_window_x", x);
    aud_set_int ("ffaudio", "video_window_y", y);
    aud_set_int ("ffaudio", "video_window_w", w);
    aud_set_int ("ffaudio", "video_window_h", h);
#else
void save_window_xy ()
{
    int video_fudge_x;
    int video_fudge_y;
    int video_fudge_w;
    int video_fudge_h;
    int ii;

#ifndef _WIN32
    SDL_SysWMinfo info;
    SDL_VERSION (&info.version);
    if (SDL_GetWMInfo (&info) > 0)
    {
        int x = 0; int y = 0;
        Window chldwin;
        XWindowAttributes attr0, attr;

        ii = aud_get_int ("ffaudio", "video_fudge_x");
        video_fudge_x = ii ? ii : -1;
        ii = aud_get_int ("ffaudio", "video_fudge_y");
        video_fudge_y = ii ? ii : -1;
        video_fudge_w = aud_get_int ("ffaudio", "video_fudge_w");
        video_fudge_h = aud_get_int ("ffaudio", "video_fudge_h");
        info.info.x11.lock_func ();
        XGetWindowAttributes (info.info.x11.display, info.info.x11.wmwindow, &attr0);
        XGetWindowAttributes (info.info.x11.display, info.info.x11.window, &attr);
        XTranslateCoordinates (info.info.x11.display, info.info.x11.window, attr.root, attr.x, attr.y, &x, &y, &chldwin);
        info.info.x11.unlock_func ();
        aud_set_int ("ffaudio", "video_window_x", (x-attr0.x)+video_fudge_x);
        aud_set_int ("ffaudio", "video_window_y", (y-attr0.y)+video_fudge_y);
        aud_set_int ("ffaudio", "video_window_w", attr.width+video_fudge_w);
        aud_set_int ("ffaudio", "video_window_h", attr.height+video_fudge_h);
    }
#else
    RECT rc;

    ii = aud_get_int ("ffaudio", "video_fudge_x");
    video_fudge_x = ii ? ii : 8;
    ii = aud_get_int ("ffaudio", "video_fudge_y");
    video_fudge_y = ii ? ii : 30;
    ii = aud_get_int ("ffaudio", "video_fudge_w");
    video_fudge_w = ii ? ii : -68;
    ii = aud_get_int ("ffaudio", "video_fudge_h");
    video_fudge_h = ii ? ii : -38;
    GetWindowRect (hwnd, &rc);
    aud_set_int ("ffaudio", "video_window_x", rc.left+video_fudge_x);
    aud_set_int ("ffaudio", "video_window_y", rc.top+video_fudge_y);
    aud_set_int ("ffaudio", "video_window_w", (rc.right-rc.left)+video_fudge_w);
    aud_set_int ("ffaudio", "video_window_h", (rc.bottom-rc.top)+video_fudge_h);
#endif
#endif
}

bool FFaudio::play (const char * filename, VFSFile & file)
{
    AUDDBG ("FFaudio::play(%s).\n", filename);

    bool fromstdin = (!strcmp (filename, "-") || strstr (filename, "://-.")) ? true : false;
    SmartPtr<AVFormatContext, close_input_file> ic (open_input_file (filename, file));
    if (! ic)
        return false;

    AUDDBG ("FFaudio::play () -----------------Playing %s.\n", filename);

    int errcount = 0;
    int out_fmt;
    int vx = 0;
    int vy = 0;
    int video_width = 0;     // INITIAL VIDEO WINDOW-SIZE:
    int video_height = 0;
    int video_window_x = 0;  // VIDEO WINDOW POSITION AND SIZE WHEN PROGRAM LAST CLOSED:
    int video_window_y = 0;
    int video_window_w = 0;
    int video_window_h = 0;
    int video_resizedelay = 1;     // MIN. TIME TO WAIT AFTER USER RESIZES VIDEO WINDOW BEFORE RE-ASPECTING (SEC.)
    int resized_window_width = 0;  // VIDEO WINDOW-SIZE AFTER LAST RESIZE.
    int resized_window_height = 0;
    int video_requested_width = 320;   // WINDOW-SIZE REQUESTED BY VIDEO STREAM ITSELF (just initialize for sanity).
    int video_requested_height = 200;
    int video_doreset_width = 0;   // WINDOW-SIZE BELOW WHICH WINDOW WILL SNAP BACK TO SIZE REQUESTED BY VIDEO STREAM:
    int video_doreset_height = 0;
    int video_qsize = 0;
    time_t last_resizeevent_time = time (nullptr);  // TIME OF LAST RESIZE EVENT, SO WE CAN DETERMINE WHEN SAFE TO RE-ASPECT.
    float video_aspect_ratio = 0;  // ASPECT RATIO OF VIDEO, SAVED TO PERMIT RE-ASPECTING AFTER USER RESIZES (WARPS) WINDOW.
    /* JWT:SAVE (static)fromstdin's STATE AT START OF PLAY, SINCE PROBES WILL CHANGE IT IN PLAYLIST ADVANCE BEFORE WE CLOSE! */
    bool myplay_video = play_video; // WHETHER OR NOT TO DISPLAY THE VIDEO.
    bool codec_opened = false;     // TRUE IF SUCCESSFULLY OPENED CODECS:
    bool vcodec_opened = false;
    bool planar;                   // USED BY Audacious
    bool returnok = false;
    bool eof = false;
    bool last_resized = true;      // TRUE IF VIDEO-WINDOW HAS BEEN RE-ASPECTED SINCE LAST RESIZE EVENT (HAS CORRECT ASPECT RATIO).
    bool is_youtubedl = false;     // TRUE IF WE'RE STREAMING A Youtube-DL STREAM (UNSEEKABLE).
    bool sdl_initialized = false;  // TRUE IF SDL (VIDEO) IS SUCCESSFULLY INITIALIZED.
    String ytdl_songtitle;         // SONG-TITLE SNIPPET TO ADD FOR Youtube-DL STREAMS THAT WE CAN'T GET METADATA FOR.
    pktQueue *pktQ = nullptr;      // QUEUE FOR VIDEO-PACKET QUEUEING.
    pktQueue *apktQ = nullptr;     // QUEUE FOR AUDIO-PACKET QUEUEING.
    SDL_Event       event;         // SDL EVENTS, IE. RESIZE, KILL WINDOW, ETC.
#if SDL == 2
    int video_fudge_x = 0; int video_fudge_y = 0;  // FUDGE-FACTOR TO MAINTAIN VIDEO SCREEN LOCN. BETWEEN RUNS.
    bool needWinSzFudge = true;     // TRUE UNTIL A FRAME HAS BEEN BLITTED & WE'RE NOT LETTING VIDEO DECIDE WINDOW SIZE.
    SDL_Window * screen = nullptr;  /* JWT: MUST DECLARE VIDEO SCREEN-WINDOW HERE */
#ifdef _WIN32
    SDL_Texture * bmp = nullptr;    // CAN'T USE SMARTPTR HERE IN WINDOWS - renderer.get() FAILS IF VIDEO PLAY NOT TURNED ON?!
#endif
#else
    SDL_Surface * screen = nullptr;
    SDL_Overlay * bmp = nullptr;
    struct SwsContext *sws_ctx = nullptr;
#endif

/* STUFF THAT GETS FREED MUST BE DECLARED AND INITIALIZED AFTER HERE B/C BEFORE HERE, WE RETURN, 
   AFTER HERE, WE GO TO error_exit (AND FREE STUFF)! */

    AVPacket pkt;
    CodecInfo cinfo, vcinfo;  //AUDIO AND VIDEO CODECS

    if (! find_codec (ic.get (), & cinfo, & vcinfo))   //CAN CHANGE play_video!
    {
        AUDERR ("No codec found for %s, can't play.\n", filename);
        goto error_exit;
    }
    codec_opened = true;
    if (play_video)
        vcodec_opened = true;

    /* JWT:WE CAN NOT RE-OPEN stdin or youtube-dl PIPED STREAMS (THEY CAN ONLY BE OPENED ONE TIME, SO WE 
       DON'T OPEN THOSE UNTIL HERE - WE'RE READY TO PLAY)! */
    if (fromstdin || strstr (filename, "ytdl://"))  /* (DEPRECIATED?) JWT: FOR STDIN: TRY TO GET "read_tag()" STUFF NOW, SINCE FILE COULD NOT BE OPENED EARLIER IN read_tag()! */
    {
        Tuple tuple;

        AUDDBG ("---- playing from STDIN: get TUPLE stuff now (if at front of stream): IC is defined\n");
        tuple.set_filename (filename);

        if ((int)ic->duration != 0)
            tuple.set_int (Tuple::Length, ic->duration / 1000);
        else
            tuple.unset (Tuple::Length);

        tuple.set_int (Tuple::Bitrate, ic->bit_rate / 1000);
        if (cinfo.codec->long_name)
            tuple.set_str (Tuple::Codec, cinfo.codec->long_name);
        if (ic->metadata)
            read_metadata_dict (tuple, ic->metadata);
        if (cinfo.stream->metadata)
            read_metadata_dict (tuple, cinfo.stream->metadata);

        if (strstr (filename, "ytdl://"))  // FETCH TITLE, ARTIST SAVED BY Youtube-DL METADATA HELPER SCRIPT:
        {
            Tuple file_tuple = Tuple ();
            /* TEMPORARILY SAVE STREAM TIME WHILST PLAYING IN CASE USER HOVERS MOUSE OVER PLAYLIST TITLE WHILST PLAYING */
            aud_set_str ("youtubedl", "lasturl", filename);  /* JWT:WE GOTTA SAVE THE LENGTH OF CURRENTLY PLAYING */
            aud_set_int ("youtubedl", "lastlen", ic->duration / 1000);  /* VIDEO SINCE IT'S LOST IF read_tag() CALLED DURING PLAY. */
            if (aud_read_tag_from_tagfile (filename, "youtubedl_tag_data", file_tuple))
            {
                //tuple = std::move (file_tuple);
                const char * tfld = (const char *) file_tuple.get_str (Tuple::Title);
                if (tfld)
                {
                    tuple.set_str (Tuple::Title, tfld);
                    ytdl_songtitle = String (tfld);
                }
                tfld = (const char *) file_tuple.get_str (Tuple::Artist);
                if (tfld)
                    tuple.set_str (Tuple::Artist, tfld);
            }
            is_youtubedl = true;
        }
        set_playback_tuple (tuple.ref ());
    }

    AUDDBG ("got codec %s for stream index %d, opening\n", cinfo.codec->name, cinfo.stream_idx);

    if (LOG (avcodec_open2, cinfo.context, cinfo.codec, nullptr) < 0)
        goto error_exit;

    if (! convert_format (cinfo.context->sample_fmt, out_fmt, planar))
        goto error_exit;

    myplay_video = play_video;
    /* JWT: IF abUSER ALSO WANTS TO PLAY VIDEO THEN WE SET UP POP-UP VIDEO SCREEN: */
    if (myplay_video)
    {
        String video_windowtitle;
        String song_title;
        int current_playlist = aud_playlist_get_active ();
        if (is_youtubedl)  // DON'T FETCH TITLE FROM TUPLE - CAUSES STREAM TO BE RE-OPENED, & IT'S USELESS ANYWAY!
        {
            if (ytdl_songtitle && ytdl_songtitle[0])
                song_title = ytdl_songtitle;
            else
            {
                String youtube_windowtitle = aud_get_str ("youtubedl", "video_windowtitle");
                if (youtube_windowtitle[0])
                    song_title = (youtube_windowtitle == String ("FILENAME")) ? String (filename) : youtube_windowtitle;
                else
                    song_title = String ("Youtube-DL");
            }
        }
        else
        {
            Tuple tuple = aud_playlist_entry_get_tuple (current_playlist, aud_playlist_get_position (current_playlist));
            song_title = tuple.get_str (Tuple::Title);
        }
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

#if SDL == 2
        int video_xmove = 1;
#else
        int video_sws_scale;
        int video_xmove = 0;
#endif

        video_xmove = aud_get_int ("ffaudio", "video_xmove");
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
        video_window_x = aud_get_int ("ffaudio", "video_window_x");
        video_window_y = aud_get_int ("ffaudio", "video_window_y");
        video_window_w = aud_get_int ("ffaudio", "video_window_w");
        video_window_h = aud_get_int ("ffaudio", "video_window_h");
#if SDL == 2
        if (video_xmove == -1)
            needWinSzFudge = false;  // NO FUDGING NEEDED IF WINDOW TO BE PLACED RANDOMLY BY WINDOWMANAGER!
        else if (video_xmove >= 0 && video_xmove != 1)  // (0 or 2)
#else
        if (video_xmove >= 0 && video_xmove != 1)  // (0 or 2)
#endif
        {
            char video_windowpos[40];
            sprintf (video_windowpos, "SDL_VIDEO_WINDOW_POS=%d, %d", video_window_x, video_window_y);
            putenv (video_windowpos);
        }
        if (LOG (avcodec_open2, vcinfo.context, vcinfo.codec, nullptr) < 0)
            goto error_exit;

        /* NOW CALCULATE THE WIDTH, HEIGHT, & ASPECT BASED ON VIDEO'S SIZE & AND ANY USER PARAMATERS GIVEN:
            IDEALLY, ONE SHOULD ONLY SET X OR Y AND LET Fauxdacious CALCULATE THE OTHER DIMENSION,
            SO THAT THE ASPECT RATIO IS MAINTAINED, THOUGH ONE CAN SPECIFY BOTH AND FORCE
            THE ASPECT TO BE ADJUSTED TO FIT.  IF A SINGLE ONE IS SPECIFIED AS "-1", THEN
            THE NEW WINDOW WILL KEEP THE SAME VALUE FOR THAT DIMENSION AS THE PREV. WINDOW,
            AND ADJUST THE OTHER DIMENTION ACCORDINGLY TO FIT THE NEW VIDEO'S ASPECT RATIO.
            IF BOTH ARE SPECIFIED AS "-1", USE PREVIOUSLY-SAVED WINDOW SIZE REGUARDLESS OF ASPECT RATIO.
        */        
        video_aspect_ratio = vcinfo.context->height
            ? (float)vcinfo.context->width / (float)vcinfo.context->height : 1.0;
        vx = aud_get_int ("ffaudio", "video_xsize");
        vy = aud_get_int ("ffaudio", "video_ysize");
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
        video_aspect_ratio = video_height
            ? (float)video_width / (float)video_height : 1.0;   /* Fall thru to square to avoid possibliity of "/0"! */

        /* NOW "RESIZE" screen to user's wXh, if user set something: */
#if SDL == 2
        if (! SDL_WasInit (SDL_INIT_VIDEO) && ! sdl_initialized)
        {
            AUDERR ("w:SDL2 NOT INITIALIZED IN (Audacious) main(), MAY SEGFAULT ON EXIT!\n");
            SDL_SetMainReady ();
            if (SDL_InitSubSystem (SDL_INIT_VIDEO) < 0)
            {
                AUDERR ("Failed to init SDL (no video playing): %s.\n", SDL_GetError ());
                myplay_video = false;
                goto breakout1;
            }
        }
        sdl_initialized = true;
        Uint32 flags = SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE;
    	   if (aud_get_bool ("ffaudio", "allow_highdpi"))
    	       flags |= SDL_WINDOW_ALLOW_HIGHDPI;

        screen = SDL_CreateWindow ("Fauxdacious Video", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 
            video_width, video_height, flags);
        if (! screen)
        {
            AUDERR ("Failed to create SDL window (no video playing): %s.\n", SDL_GetError ());
            myplay_video = false;
            goto breakout1;
        }
#if SDL_COMPILEDVERSION >= 2004
        else
            SDL_SetHint (SDL_HINT_VIDEO_X11_NET_WM_PING, "0");
#endif
#else
        screen = SDL_SetVideoMode (video_width, video_height, 0, SDL_HWSURFACE | SDL_ASYNCBLIT | SDL_HWACCEL | SDL_RESIZABLE);
        if (! screen) {
            AUDERR ("SDL: could not re-set video mode - will only play audio.\n");
            myplay_video = false;
            goto breakout1;
        }
#ifdef _WIN32
        hwnd = GetActiveWindow ();
#endif
        if (SDL_InitSubSystem (SDL_INIT_VIDEO) < 0)
        {
            AUDERR ("Could not initialize SDL video - %s\n", SDL_GetError ());
            myplay_video = false;
            goto breakout1;
        }
        sdl_initialized = true;
#ifndef _WIN32
        if (video_xmove > 0)  // (1 or 2)
        {
            SDL_SysWMinfo info;
            SDL_VERSION (&info.version);
            if (SDL_GetWMInfo (&info) > 0 ) {
                SDL_Delay (50);
                XMoveWindow (info.info.x11.gfxdisplay, info.info.x11.wmwindow, video_window_x, video_window_y);
            }
        }
#endif
#endif
        video_windowtitle = aud_get_str ("ffaudio", "video_windowtitle");
        StringBuf titleBuf = (video_windowtitle && video_windowtitle[0])
                ? str_printf ("%s - %s", (const char *) song_title, (const char *) video_windowtitle)
                : str_copy ((const char *) song_title, -1);
        song_title = String ();
        video_windowtitle = String ();
        str_replace_char (titleBuf, '_', ' ');
#if SDL == 2
        SDL_SetWindowSize (screen, video_width, video_height);

        if (video_xmove > 0)  // (1 or 2)
            SDL_SetWindowPosition (screen, video_window_x, video_window_y);
        if (aud_get_str ("ffaudio", "video_render_scale"))
            SDL_SetHint (SDL_HINT_RENDER_SCALE_QUALITY, aud_get_str ("ffaudio", "video_render_scale"));
        else
            SDL_SetHint (SDL_HINT_RENDER_SCALE_QUALITY, "1");
        SDL_SetWindowTitle (screen, (const char *) titleBuf);
        //SDL_ShowWindow (screen);
#else
        SDL_WM_SetCaption ((const char *) titleBuf, nullptr);
        if (aud_get_int ("ffaudio", "video_sws_scale"))   /* USER CAN CHOOSE SWS_SCALE VALUE. */
            video_sws_scale = aud_get_int ("ffaudio", "video_sws_scale");
        else
            video_sws_scale = SWS_BICUBIC;  /* default=4 (SWS_BICUBIC). */
        bmp = SDL_CreateYUVOverlay (
            vcinfo.context->width,
            vcinfo.context->height,
            SDL_YV12_OVERLAY,
            screen
        );
        sws_ctx = sws_getContext (
               vcinfo.context->width,
               vcinfo.context->height,
               vcinfo.context->pix_fmt,
               vcinfo.context->width,
               vcinfo.context->height,
               AV_PIX_FMT_YUV420P,
               video_sws_scale,
               nullptr,
               nullptr,
               nullptr
        );
        if (! bmp)
        {
            AUDERR ("e:Could not do sws_getContext (%s)\n", SDL_GetError ());
            myplay_video = false;
        }
#endif
    }

breakout1:

    /* Open audio output */
    AUDDBG ("opening audio output - bitrate=%ld=\n", (long) ic->bit_rate);

    set_stream_bitrate (ic->bit_rate);
    open_audio (out_fmt, cinfo.context->sample_rate, cinfo.context->channels);

    int seek_value, ret;
    /* JWT:video_qsize:  MAX # PACKETS TO QUEUE UP FOR INTERLACING TO SMOOTH VIDEO
        PLAYBACK - GOOD RANGE IS 6-28, DEFAULT IS 8:
        NOT ENOUGH = JITTERY VIDEO
        TOO MANY = AUDIO/VIDEO BECOME NOTICABLY OUT OF SYNC!
    */
    if (strstr (filename, "ytdl://"))  // Youtube-DL STREAMING SEEMS TO WORK BETTER W/LARGER QUEUE-SIZE.
        video_qsize = aud_get_int ("youtubedl", "video_qsize");
    if (video_qsize < 1)
        video_qsize = (aud_get_int ("ffaudio", "video_qsize"))
                ? aud_get_int ("ffaudio", "video_qsize") : 6;
    if (video_qsize < 1)
        video_qsize = 6;

    /* TYPICALLY THERE'S TWICE AS MANY AUDIO PACKETS AS VIDEO, SO THIS IS COUNTER-INTUITIVE, BUT IT WORKS BEST! */
    pktQ = createQueue (2 * video_qsize);
    apktQ = createQueue (video_qsize);
    returnok = true;
    AUDDBG ("i:video queue size %d\n", video_qsize);

#if SDL == 2
    {   // SUBSCOPE FOR DECLARING SDL2 TEXTURE AS SCOPED SMARTPOINTER:
    bool windowIsStable = false;    // JWT:SAVING AND RECREATING WINDOW CAUSES POSN. TO DIFFER BY THE WINDOW DECORATION SIZES, SO WE HAVE TO FUDGE FOR THAT!
    bool windowNowExposed = false;  // JWT:NEEDED TO PREVENT RESIZING WINDOW BEFORE EXPOSING ON MS-WINDOWS?!
    SmartPtr<SDL_Renderer, SDL_DestroyRenderer> renderer (createSDL2Renderer (screen, myplay_video));
    if (! renderer)
    {
        if (myplay_video)
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
        myplay_video = false;
#else
#define bmpptr bmp
#endif

    /* OUTER LOOP TO READ, QUEUE AND PROCESS AUDIO & VIDEO PACKETS FROM THE STREAM: */
    while (1)
    {
        /* CHECK IF WE NEED TO QUIT (EOF OR USER PRESSED STOP BUTTON OR WENT TO ANOTHER SONG): */
        if (eof || check_stop ())
        {
            if (eof)   /* EOF: WRITE OUT EVERYTHING THAT'S WAITING IN THE QUEUES: */
            {
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
#if SDL == 2
                            write_videoframe (renderer.get (), & vcinfo, bmpptr, pktRef, 
                                    video_width, video_height, last_resized, & windowIsStable);
#else
                            write_videoframe (sws_ctx, & vcinfo, bmp, pktRef, 
                                    video_width, video_height, last_resized);
#endif
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
            }

            /* On shutdown/EOF, send an empty packet to "flush" the decoders */
            pkt = AVPacket ();
            av_init_packet (& pkt);
            pkt.data=nullptr; pkt.size=0;
            write_audioframe (& cinfo, & pkt, out_fmt, planar);
            if (myplay_video)  /* IF VIDEO-WINDOW STILL INTACT (NOT CLOSED BY USER PRESSING WINDOW'S CORNER [X]): */
#if SDL == 2
                write_videoframe (renderer.get (), & vcinfo, bmpptr, & pkt, 
                        video_width, video_height, last_resized, & windowIsStable);
#else
                write_videoframe (sws_ctx, & vcinfo, bmp, & pkt, 
                        video_width, video_height, last_resized);
#endif
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

        /* CHECK IF USER MOVED THE SEEK/POSITION SLIDER, IF SO FLUSH QUEUES AND SEEK TO NEW POSITION: */
        seek_value = check_seek ();
        if (seek_value >= 0)
        {
            /* JWT:FIRST, FLUSH ANY PACKETS SITTING IN THE QUEUES TO CLEAR THE QUEUES! */
            QFlush (apktQ);
            QFlush (pktQ);
            /* JWT: HAD TO CHANGE THIS FROM "AVSEEK_FLAG_ANY" TO AVSEEK_FLAG_BACKWARD 
                TO GET SEEK TO NOT RANDOMLY BRICK?! */
            if (LOG (av_seek_frame, ic.get (), -1, (int64_t) seek_value *
                    AV_TIME_BASE / 1000, AVSEEK_FLAG_BACKWARD) >= 0)
                errcount = 0;
            seek_value = -1;
        }

        /* READ NEXT FRAME (OR MORE) OF DATA */
        pkt = AVPacket ();
        av_init_packet (& pkt);
        ret = LOG (av_read_frame, ic.get (), & pkt);
        if (ret < 0)  // CHECK FOR EOF OR ERRORS:
        {
            ++errcount;
            if (ret == (int) AVERROR_EOF)
            {
                AUDDBG ("eof reached\n");
                av_free_packet (& pkt);
                eof = true;
                continue;
            }
            else if (errcount > 4)
            {
                AUDERR ("av_read_frame error %d, giving up.\n", ret);
                av_free_packet (& pkt);
                returnok = false;
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
                av_free_packet (& pkt);
                continue;
            }
        }
        else
            errcount = 0;

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
#if SDL == 2
                    write_videoframe (renderer.get (), & vcinfo, bmpptr, pktRef, 
                            video_width, video_height, last_resized, & windowIsStable);
#else
                    write_videoframe (sws_ctx, & vcinfo, bmp, pktRef, 
                            video_width, video_height, last_resized);
#endif
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
        if (pkt.stream_index == cinfo.stream_idx)  /* WE READ AN AUDIO PACKET, IF PLAYING VIDEO, QUEUE IT, OTHERWISE JUST PROCESS IT: */
        {
            if (myplay_video)
                Enqueue (apktQ, pkt);
            else
            {
                write_audioframe (& cinfo, & pkt, out_fmt, planar);
                av_free_packet (& pkt);
                continue;
            }
        }
        else if (myplay_video)
        {
            if (pkt.stream_index == vcinfo.stream_idx)  /* WE READ A VIDEO PACKET, QUEUE IT (ALWAYS): */
                Enqueue (pktQ, pkt);
            else
                av_free_packet (& pkt);
        }
        else   /* IGNORE ANY OTHER SUBSTREAMS */
        {
            av_free_packet (& pkt);
            continue;
        }
        /* AT THIS POINT THE PACKET MUST BE EITHER ENQUEUED OR FREED! */

        /* JWT: NOW HANDLE ANY VIDEO UI EVENTS SUCH AS RESIZE OR KILL VIDEO SCREEN (IF PLAYING VIDEO): */
        /*      IF WE'RE HERE, WE *ARE* STILL PLAYING VIDEO (BUT THAT MAY CHANGE WITHIN THIS LOOP)! */
        while (SDL_PollEvent (&event))
        {
            switch (event.type) {
#if SDL == 2
                case SDL_QUIT:  /* USER CLICKED THE "X" IN UPPER-RIGHT CORNER, KILL VIDEO WINDOW BUT KEEP PLAYING AUDIO! */
                    AUDDBG ("i:SDL_QUIT (User killed video window for this play)!\n");
                    if (myplay_video)
                    {
                        myplay_video = false;
                        QFlush (pktQ);  // FLUSH ANY VIDEO PACKETS IN QUEUE SINCE WE CAN'T WRITE 'EM AFTER CLOSING.
                        if (bmp)  // SMARTPTR OR NOT, WE GOTTA FREE THIS BEFORE CLOBBERING WINDOW!
                            SDL_DestroyTexture (bmpptr);
                        if (renderer)
                            SDL_DestroyRenderer (renderer.get ());
                        if (screen)
                        {
                            save_window_xy (screen, video_fudge_x, video_fudge_y);
                            SDL_DestroyWindow (screen);           
                            screen = nullptr;
                        }
                    }
                    break;
                case SDL_WINDOWEVENT:
                    switch (event.window.event)
                    {
                        case SDL_WINDOWEVENT_CLOSE:  /* USER CLICKED THE "X" IN UPPER-RIGHT CORNER, KILL VIDEO WINDOW BUT KEEP PLAYING AUDIO! */
                            AUDDBG ("i:SDL_CLOSE (User killed video window for this play)!\n");
                            if (myplay_video)
                            {
                                myplay_video = false;
                                QFlush (pktQ);  // FLUSH ANY VIDEO PACKETS IN QUEUE SINCE WE CAN'T WRITE 'EM AFTER CLOSING.
                                if (bmp)  // SMARTPTR OR NOT, WE GOTTA FREE THIS BEFORE CLOBBERING WINDOW!
                                    SDL_DestroyTexture (bmpptr);
                                if (renderer)
                                    SDL_DestroyRenderer (renderer.get ());
                                if (screen)
                                {
                                    save_window_xy (screen, video_fudge_x, video_fudge_y);
                                    SDL_DestroyWindow (screen);           
                                    screen = nullptr;
                                }
                                //if (sdl_initialized)
                                //    SDL_QuitSubSystem (SDL_INIT_VIDEO);
                            }
                            break;
                        case SDL_WINDOWEVENT_RESIZED:  /* WINDOW CHANGED SIZE EITHER BY US OR BY USER DRAGGING WINDOW CORNER (WE DON'T KNOW WHICH HERE) */
                            if (! windowNowExposed)
                                break;
                            /* Resize the screen. */
                            resized_window_width = event.window.data1;  // window's reported new size
                            resized_window_height = event.window.data2;
                            AUDDBG ("i:SDL_RESIZE!!!!!! rvw=%d h=%d\n", resized_window_width, resized_window_height);
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
#else
                case SDL_QUIT:  /* OLD SDL-1 WAY (SEE COMMENTS ABOVE): */
                    QFlush (pktQ);  // FLUSH ANY VIDEO PACKETS IN QUEUE SINCE WE CAN'T WRITE 'EM AFTER CLOSING.
                    save_window_xy ();
                    if (sws_ctx)
                        sws_freeContext (sws_ctx);
                    if (bmp)
                    {
                        SDL_FreeYUVOverlay (bmp);
                        bmp = nullptr;
                    }
                    if (screen)
                    {
                        SDL_FreeSurface (screen);
                        screen = nullptr;
                    }
                    if (sdl_initialized)
                        SDL_QuitSubSystem (SDL_INIT_VIDEO);
                    myplay_video = false;
                    AUDDBG ("i:SDL_QUIT (User killed video window for this play)!\n");
                    break;

                case SDL_VIDEORESIZE:
                    /* Resize the screen. */
                    resized_window_width = event.resize.w;
                    resized_window_height = event.resize.h;
                    AUDDBG ("i:SDL_RESIZE!!!!!! rvw=%d h=%d\n", resized_window_width, resized_window_height);
                    last_resized = false;
                    last_resizeevent_time = time (nullptr);
                    break;
                case SDL_VIDEOEXPOSE:
                    if (last_resized)
                    {
                        SDL_Rect rect;
                        rect.x = 0;
                        rect.y = 0;
                        rect.w = video_width;
                        rect.h = video_height;
                        SDL_DisplayYUVOverlay (bmp, &rect);
                    }
#endif
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
            if (difftime (time (nullptr), last_resizeevent_time) > video_resizedelay)
            {
                float new_aspect_ratio;  // ASPECT (for comparing), W, & H OF WINDOW AFTER USER DONE RESIZING:
                int new_video_width;     // WILL ADJUST ONE OF THESE TO RESTORE TO VIDEO'S PROPER ASPECT
                int new_video_height;    // THEN RESIZE (RE-ASPECT) THE WINDOW TO KEEP ASPECT CONSTANT!
                /* CALCULATE THE RESIZED WINDOW'S ASPECT RATIO */
                new_aspect_ratio = resized_window_height
                    ? (float)resized_window_width / (float)resized_window_height : 1.0;
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
                    new_video_width = video_requested_width;
                    new_video_height = video_requested_height;
                }
                video_width = new_video_width;
                video_height = new_video_height;
                /* NOW MANUALLY RESIZE (RE-ASPECT) WINDOW BASED ON VIDEO'S ORIGINALLY-CALCULATED ASPECT RATIO: */
#if SDL == 2
                SDL_SetWindowSize (screen, video_width, video_height);
                SDL_Delay (50);
#else
                SDL_Rect rect;
                //AUDDBG ("i:RESIZING IT to (%d, %d)\n", video_width, video_height);
                screen = SDL_SetVideoMode (video_width, video_height, 0, SDL_HWSURFACE | SDL_ASYNCBLIT | SDL_HWACCEL | SDL_RESIZABLE);
                if (! screen)
                {
                    AUDERR ("Could not recreate screen after resizing!");
                    break;
                }
#ifdef _WIN32
                hwnd = GetActiveWindow ();
#endif
                rect.x = 0;
                rect.y = 0;
                rect.w = video_width;
                rect.h = video_height;
                SDL_DisplayYUVOverlay (bmp, &rect);
#endif
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
#if SDL == 2
        if (needWinSzFudge && windowIsStable)
        {
            int x, y;
            SDL_GetWindowPosition (screen, &x, &y);
            video_fudge_x = video_window_x - x;
            video_fudge_y = video_window_y - y;
            AUDDBG ("FUDGE SET(x=%d y=%d) vw=(%d, %d) F=(%d, %d)\n", x,y,video_window_x,video_window_y,video_fudge_x,video_fudge_y);
            needWinSzFudge = false;
        }
#endif
    }  // END PACKET-PROCESSING LOOP.

#if SDL == 2
    }  // END OF SUBSCOPE FOR DECLARING SDL2 TEXTURE AS SCOPED SMARTPOINTER.
#endif

error_exit:  /* WE END UP HERE WHEN PLAYBACK IS STOPPED: */

    AUDDBG ("end of playback.\n");
    aud_set_int ("youtubedl", "lastlen", 0);   // CLEAR TEMP. STREAM LENGTH (NEEDED TO DISPLAY TIME IN Youtube-DL STREAMS:
    aud_set_str ("youtubedl", "lasturl", "");
    if (pktQ)
        destroyQueue (pktQ);
    if (apktQ)
        destroyQueue (apktQ);
    if (myplay_video)  /* WE WERE PLAYING VIDEO && FINISHED WITH VIDEO-WINDOW STILL INTACT (NOT CLOSED BY USER PRESSING WINDOW'S CORNER [X]) */
    {
        AUDDBG ("i:ffaudio: QUITTING VIDEO!\n");
#if SDL == 2
        if (screen)  // bmp ALREADY FREED & NOW OUT OF SCOPE BUT MAKE SURE VIDEO WINDOW IS FREED & GONE!
        {
            save_window_xy (screen, video_fudge_x, video_fudge_y);
            SDL_DestroyWindow (screen);
            screen = nullptr;
        }
#ifdef _WIN32
        if (sdl_initialized)
            SDL_QuitSubSystem (SDL_INIT_VIDEO);
#endif
#else
        save_window_xy ();
        if (sws_ctx)
            sws_freeContext (sws_ctx);
        if (bmp)
        {
            SDL_FreeYUVOverlay (bmp);
            bmp = nullptr;
        }
        if (screen)
        {
            SDL_FreeSurface (screen);
            screen = nullptr;
        }
        if (sdl_initialized)
            SDL_QuitSubSystem (SDL_INIT_VIDEO);
#endif
    }

    if (vcodec_opened)
    {
#ifdef ALLOC_CONTEXT
        avcodec_free_context (& vcinfo.context);
        av_free (vcinfo.context);
#else
        avcodec_close (vcinfo.context);
#endif
    }
    if (codec_opened)
    {
#ifdef ALLOC_CONTEXT
        avcodec_free_context (& cinfo.context);
        av_free (cinfo.context);
#else
        avcodec_close (cinfo.context);
#endif
    }

    return returnok;
}

const char FFaudio::about[] =
 N_("Multi-format audio decoding plugin for Fauxdacious using\n"
    "FFmpeg multimedia framework (http://www.ffmpeg.org/)\n"
    "\n"
    "Audacious plugin by:\n"
    "William Pitcock <nenolod@nenolod.net>\n"
    "Matti Hämäläinen <ccr@tnsp.org>\n"
    "\n"
    "Video-playing capability (added 2015) by:\n"
    "Jim Turner <turnerjw784@yahoo.com>");

const char * const FFaudio::exts[] = {
    /* musepack, SV7/SV8 */
    "mpc", "mp+", "mpp",

    /* windows media audio */
    "wma",

    /* shorten */
    "shn",

    /* atrac3 */
    "aa3", "oma",

    /* MPEG 2/4 AC3 */
    "ac3",

    /* monkey's audio */
    "ape",

    /* DTS */
    "dts",

    /* VQF */
    "vqf",

    /* MPEG-4 */
    "m4a", "mp4",

    /* WAV (there are some WAV formats sndfile can't handle) */
    "wav",

    /* Handle OGG streams (FLAC/Vorbis etc.) */
    "ogg", "oga", "ogv", "ogx",

    /* Opus */
    "opus",

    /* Speex */
    "spx",

    /* True Audio */
    "tta",

    /* AVI  // JWT:ADDED */
    /* NOTE:  .mov CAN NOT BE PLAYED OVER STDIN (FFPLAY WON'T EVEN PLAY IT THAT WAY!) */
    "avi", "flv", "swf", "mov",

    /* 3rd Generation Partnership Videos  // JWT:ADDED */
    "3gp",

    /* DVD Video Object Files  // JWT:ADDED */
    "vob",

    /* WebM Videos  // JWT:ADDED */
    "webm",

    /* end of table */
    nullptr
};

const char * const FFaudio::mimes[] = {"application/ogg", nullptr};
const char * const FFaudio::schemes[] = {"ytdl", nullptr};
