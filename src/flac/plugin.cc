/*
 *  A FLAC decoder plugin for the Audacious Media Player
 *  Copyright (C) 2005 Ralf Ertzinger
 *  Copyright (C) 2010-2012 Michał Lipski
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include <string.h>

#include <libfauxdcore/runtime.h>
#include <fauxdacious/audtag.h>

#include "flacng.h"

EXPORT FLACng aud_plugin_instance;

static FLAC__StreamDecoder *decoder, *ogg_decoder;
static callback_info *cinfo;

bool FLACng::init()
{
    FLAC__StreamDecoderInitStatus ret;

    /* Callback structure and decoder for main decoding loop */

    cinfo = new callback_info;

    if ((decoder = FLAC__stream_decoder_new()) == nullptr)
    {
        AUDERR ("Could not create the main FLAC decoder instance!\n");
        return false;
    }

    if (FLAC__STREAM_DECODER_INIT_STATUS_OK != (ret = FLAC__stream_decoder_init_stream(
        decoder,
        read_callback,
        seek_callback,
        tell_callback,
        length_callback,
        eof_callback,
        write_callback,
        metadata_callback,
        error_callback,
        cinfo)))
    {
        AUDERR ("Could not initialize the main FLAC decoder: %s(%d)\n",
            FLAC__StreamDecoderInitStatusString[ret], ret);
        return false;
    }

    if (FLAC_API_SUPPORTS_OGG_FLAC && ((ogg_decoder = FLAC__stream_decoder_new()) == nullptr
            || FLAC__stream_decoder_init_ogg_stream(
        ogg_decoder,
        read_callback,
        seek_callback,
        tell_callback,
        length_callback,
        eof_callback,
        write_callback,
        metadata_callback,
        error_callback,
        cinfo)))
    {
        AUDWARN("w:FLAC:Could not initialize the extra OGG FLAC decoder:  so no playing OGG-FLAC streams!\n");
    }

    AUDDBG("Plugin initialized.\n");
    return true;
}

void FLACng::cleanup()
{
    if (ogg_decoder)  FLAC__stream_decoder_delete(ogg_decoder);
    FLAC__stream_decoder_delete(decoder);
    delete cinfo;
}

bool FLACng::is_our_file(const char *filename, VFSFile &file)
{
    AUDDBG("Probe for FLAC.\n");

    char buf[4];
    if (file.fread (buf, 1, sizeof buf) != sizeof buf)
        return false;

    if (! strncmp (buf, "fLaC", sizeof buf))
        return true;
    else if (FLAC_API_SUPPORTS_OGG_FLAC && ogg_decoder)
    {
        /* WE'RE NOT FLAC, BUT WE CAN PLAY OGG, SO SEE IF WE'RE OGG-FLAC: */
        char buf[33];
        if (! file.fseek (0, VFS_SEEK_SET) && file.fread (buf, 1, sizeof buf) == sizeof buf
                && ! strncasecmp (buf+29, "FLAC", 4))
            return true;  // WILL USE FLAC PLUGIN.
    }
    return false;
}

static void squeeze_audio(int32_t* src, void* dst, unsigned count, unsigned res)
{
    int32_t* rp = src;
    int8_t*  wp = (int8_t*) dst;
    int16_t* wp2 = (int16_t*) dst;
    int32_t* wp4 = (int32_t*) dst;

    switch (res)
    {
        case 8:
            for (unsigned i = 0; i < count; i++, wp++, rp++)
                *wp = *rp & 0xff;
            break;

        case 16:
            for (unsigned i = 0; i < count; i++, wp2++, rp++)
                *wp2 = *rp & 0xffff;
            break;

        case 24:
        case 32:
            for (unsigned i = 0; i < count; i++, wp4++, rp++)
                *wp4 = *rp;
            break;

        default:
            AUDERR ("Can not convert to %u bps\n", res);
    }
}

bool FLACng::play(const char *filename, VFSFile &file)
{
    Index<char> play_buffer;
    bool error = false;
    bool stream = (file.fsize () < 0);
    Tuple tuple;
    if (stream)
    {
        tuple = get_playback_tuple ();
        if (audtag::read_tag (file, tuple, nullptr))
            set_playback_tuple (tuple.ref ());
    }

    cinfo->fd = &file;

    FLAC__StreamDecoder *which_decoder = decoder;
    if (FLAC_API_SUPPORTS_OGG_FLAC && ogg_decoder)
    {
        String mime = file.get_metadata("content-type");
        if (mime && strstr(mime, "ogg"))  which_decoder = ogg_decoder;
    }

    if (read_metadata(which_decoder, cinfo) == false)
    {
        AUDERR ("Could not prepare file for playing!\n");
        error = true;
        goto ERR_NO_CLOSE;
    }

    play_buffer.resize(BUFFER_SIZE_BYTE);

    set_stream_bitrate(cinfo->bitrate);

    if (stream && tuple.fetch_stream_info (file))
        set_playback_tuple (tuple.ref ());

    open_audio(SAMPLE_FMT(cinfo->bits_per_sample), cinfo->sample_rate, cinfo->channels);

    while (FLAC__stream_decoder_get_state(which_decoder) != FLAC__STREAM_DECODER_END_OF_STREAM)
    {
        if (check_stop ())
            break;

        int seek_value = check_seek ();
        if (seek_value >= 0)
            FLAC__stream_decoder_seek_absolute (which_decoder, (int64_t)
             seek_value * cinfo->sample_rate / 1000);

        /* Try to decode a single frame of audio */
        if (FLAC__stream_decoder_process_single(which_decoder) == false)
        {
            AUDERR ("Error while decoding!\n");
            error = true;
            break;
        }

        if (stream && tuple.fetch_stream_info (file))
            set_playback_tuple (tuple.ref ());

        squeeze_audio(cinfo->output_buffer.begin(), play_buffer.begin(),
         cinfo->buffer_used, cinfo->bits_per_sample);
        write_audio(play_buffer.begin(), cinfo->buffer_used *
         SAMPLE_SIZE(cinfo->bits_per_sample));

        cinfo->reset();
    }

ERR_NO_CLOSE:
    cinfo->reset();

    if (FLAC__stream_decoder_flush(which_decoder) == false)
        AUDERR ("Could not flush decoder state!\n");

    return ! error;
}

const char FLACng::about[] =
 N_("Original code by\n"
    "Ralf Ertzinger <ralf@skytale.net>\n\n"
    "http://www.skytale.net/projects/bmp-flac2/");

const char *const FLACng::exts[] = { "flac", "fla", nullptr };
const char *const FLACng::mimes[] = { "audio/flac", "audio/x-flac", "audio/ogg", "application/ogg", nullptr };
