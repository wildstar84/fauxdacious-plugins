/*  Audacious - Cross-platform multimedia player
 *  Copyright (C) 2005-2022 Audacious development team
 *
 *  Based on the xmms_sndfile input plugin:
 *  Copyright (C) 2000, 2002 Erik de Castro Lopo
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

#include <stdlib.h>
#include <sndfile.h>

#define WANT_VFS_STDIO_COMPAT
#include <libfauxdcore/plugin.h>
#include <libfauxdcore/i18n.h>
#include <libfauxdcore/audstrings.h>

class SndfilePlugin : public InputPlugin
{
public:
    static const char about[];
    static const char * const exts[];
    static const char * const mimes[];

    static constexpr PluginInfo info = {
        N_("Sndfile Plugin"),
        PACKAGE,
        about
    };

    constexpr SndfilePlugin () : InputPlugin (info, InputInfo ()
        .with_priority (9)  /* low priority fallback (but before ffaudio) */
        .with_exts (exts)
        .with_mimes (mimes)) {}

    bool is_our_file (const char * filename, VFSFile & file);
    bool read_tag (const char * filename, VFSFile & file, Tuple & tuple, Index<char> * image);
    bool play (const char * filename, VFSFile & file);
};

EXPORT SndfilePlugin aud_plugin_instance;

/* Virtual file access wrappers for libsndfile
 */
static sf_count_t
sf_get_filelen (void *user_data)
{
    int64_t size = ((VFSFile *) user_data)->fsize ();
    return (size < 0) ? SF_COUNT_MAX : size;
}

static sf_count_t
sf_vseek (sf_count_t offset, int whence, void *user_data)
{
    if (((VFSFile *) user_data)->fseek (offset, to_vfs_seek_type (whence)) != 0)
        return -1;

    return ((VFSFile *) user_data)->ftell ();
}

static sf_count_t
sf_vseek_dummy (sf_count_t offset, int whence, void *user_data)
{
    return -1;
}

static sf_count_t
sf_vread (void *ptr, sf_count_t count, void *user_data)
{
    return ((VFSFile *) user_data)->fread (ptr, 1, count);
}

static sf_count_t
sf_vwrite_dummy (const void *ptr, sf_count_t count, void *user_data)
{
    return 0;
}

static sf_count_t
sf_tell (void *user_data)
{
    return ((VFSFile *) user_data)->ftell ();
}

static SF_VIRTUAL_IO sf_virtual_io =
{
    sf_get_filelen,
    sf_vseek,
    sf_vread,
    sf_vwrite_dummy,
    sf_tell
};

static SF_VIRTUAL_IO sf_virtual_io_stream =
{
    sf_get_filelen,
    sf_vseek_dummy,
    sf_vread,
    sf_vwrite_dummy,
    sf_tell
};

static void copy_string (SNDFILE * sf, int sf_id, Tuple & tup, Tuple::Field field)
{
    const char * str = sf_get_string (sf, sf_id);
    if (str)
        tup.set_str (field, str);
}

static void copy_int (SNDFILE * sf, int sf_id, Tuple & tup, Tuple::Field field)
{
    const char * str = sf_get_string (sf, sf_id);
    if (str && atoi (str))
        tup.set_int (field, atoi (str));
}

bool SndfilePlugin::read_tag (const char * filename, VFSFile & file, Tuple & tuple,
 Index<char> * image)
{
    SF_INFO sfinfo {}; // must be zeroed before sf_open()
    const char *format, *subformat;

    bool stream = (file.fsize () < 0);
    SNDFILE * sndfile = sf_open_virtual (stream ? & sf_virtual_io_stream :
     & sf_virtual_io, SFM_READ, & sfinfo, & file);

    if (! sndfile)
        return false;

    copy_string (sndfile, SF_STR_TITLE, tuple, Tuple::Title);
    copy_string (sndfile, SF_STR_ARTIST, tuple, Tuple::Artist);
    copy_string (sndfile, SF_STR_ALBUM, tuple, Tuple::Album);
    copy_string (sndfile, SF_STR_COMMENT, tuple, Tuple::Comment);
    copy_string (sndfile, SF_STR_GENRE, tuple, Tuple::Genre);
    copy_int (sndfile, SF_STR_DATE, tuple, Tuple::Year);
    copy_int (sndfile, SF_STR_TRACKNUMBER, tuple, Tuple::Track);

    sf_close (sndfile);

    if (sfinfo.samplerate > 0)
    {
        int64_t length = aud::rescale<int64_t> (sfinfo.frames, sfinfo.samplerate, 1000);
        tuple.set_int (Tuple::Length, length);
    }

    switch (sfinfo.format & SF_FORMAT_TYPEMASK)
    {
        case SF_FORMAT_WAV:
        case SF_FORMAT_WAVEX:
            format = "Microsoft WAV";
            break;
        case SF_FORMAT_AIFF:
            format = "Apple/SGI AIFF";
            break;
        case SF_FORMAT_AU:
            format = "Sun/NeXT AU";
            break;
        case SF_FORMAT_RAW:
            format = "Raw PCM data";
            break;
        case SF_FORMAT_PAF:
            format = "Ensoniq PARIS";
            break;
        case SF_FORMAT_SVX:
            format = "Amiga IFF / SVX8 / SV16";
            break;
        case SF_FORMAT_NIST:
            format = "Sphere NIST";
            break;
        case SF_FORMAT_VOC:
            format = "Creative VOC";
            break;
        case SF_FORMAT_IRCAM:
            format = "Berkeley/IRCAM/CARL";
            break;
        case SF_FORMAT_W64:
            format = "Sonic Foundry's 64 bit RIFF/WAV";
            break;
        case SF_FORMAT_MAT4:
            format = "Matlab (tm) V4.2 / GNU Octave 2.0";
            break;
        case SF_FORMAT_MAT5:
            format = "Matlab (tm) V5.0 / GNU Octave 2.1";
            break;
        case SF_FORMAT_PVF:
            format = "Portable Voice Format";
            break;
        case SF_FORMAT_XI:
            format = "Fasttracker 2 Extended Instrument";
            break;
        case SF_FORMAT_HTK:
            format = "HMM Tool Kit";
            break;
        case SF_FORMAT_SDS:
            format = "Midi Sample Dump Standard";
            break;
        case SF_FORMAT_AVR:
            format = "Audio Visual Research";
            break;
        case SF_FORMAT_SD2:
            format = "Sound Designer 2";
            break;
        case SF_FORMAT_FLAC:
            format = "Free Lossless Audio Codec";
            break;
        case SF_FORMAT_CAF:
            format = "Core Audio File";
            break;
        default:
            format = nullptr;
    }

    switch (sfinfo.format & SF_FORMAT_SUBMASK)
    {
        case SF_FORMAT_PCM_S8:
            subformat = "signed 8 bit";
            break;
        case SF_FORMAT_PCM_16:
            subformat = "signed 16 bit";
            break;
        case SF_FORMAT_PCM_24:
            subformat = "signed 24 bit";
            break;
        case SF_FORMAT_PCM_32:
            subformat = "signed 32 bit";
            break;
        case SF_FORMAT_PCM_U8:
            subformat = "unsigned 8 bit";
            break;
        case SF_FORMAT_FLOAT:
            subformat = "32 bit float";
            break;
        case SF_FORMAT_DOUBLE:
            subformat = "64 bit float";
            break;
        case SF_FORMAT_ULAW:
            subformat = "U-Law";
            break;
        case SF_FORMAT_ALAW:
            subformat = "A-Law";
            break;
        case SF_FORMAT_IMA_ADPCM:
            subformat = "IMA ADPCM";
            break;
        case SF_FORMAT_MS_ADPCM:
            subformat = "MS ADPCM";
            break;
        case SF_FORMAT_GSM610:
            subformat = "GSM 6.10";
            break;
        case SF_FORMAT_VOX_ADPCM:
            subformat = "Oki Dialogic ADPCM";
            break;
        case SF_FORMAT_G721_32:
            subformat = "32kbs G721 ADPCM";
            break;
        case SF_FORMAT_G723_24:
            subformat = "24kbs G723 ADPCM";
            break;
        case SF_FORMAT_G723_40:
            subformat = "40kbs G723 ADPCM";
            break;
        case SF_FORMAT_DWVW_12:
            subformat = "12 bit Delta Width Variable Word";
            break;
        case SF_FORMAT_DWVW_16:
            subformat = "16 bit Delta Width Variable Word";
            break;
        case SF_FORMAT_DWVW_24:
            subformat = "24 bit Delta Width Variable Word";
            break;
        case SF_FORMAT_DWVW_N:
            subformat = "N bit Delta Width Variable Word";
            break;
        case SF_FORMAT_DPCM_8:
            subformat = "8 bit differential PCM";
            break;
        case SF_FORMAT_DPCM_16:
            subformat = "16 bit differential PCM";
            break;
        default:
            subformat = nullptr;
    }

    if (format == nullptr)
    {
        SF_FORMAT_INFO info = {.format = sfinfo.format & SF_FORMAT_SUBMASK};
        if (sf_command (sndfile, SFC_GET_FORMAT_INFO, & info, sizeof (info)) == 0)
            format = info.name;
        else
            format = "Unknown format";
    }

    if (subformat != nullptr)
        tuple.set_format (str_printf ("%s (%s)", format, subformat),
         sfinfo.channels, sfinfo.samplerate, 0);
    else
        tuple.set_format (format, sfinfo.channels, sfinfo.samplerate, 0);

    return true;
}

bool SndfilePlugin::play (const char * filename, VFSFile & file)
{
    SF_INFO sfinfo {}; // must be zeroed before sf_open()

    bool stream = (file.fsize () < 0);
    SNDFILE * sndfile = sf_open_virtual (stream ? & sf_virtual_io_stream :
     & sf_virtual_io, SFM_READ, & sfinfo, & file);

    if (sndfile == nullptr)
        return false;

    open_audio (FMT_FLOAT, sfinfo.samplerate, sfinfo.channels);

    Index<float> buffer;
    buffer.resize (sfinfo.channels * (sfinfo.samplerate / 50));

    while (! check_stop ())
    {
        int seek_value = check_seek ();
        if (seek_value != -1)
        {
            int64_t frames = aud::rescale<int64_t> (seek_value, 1000, sfinfo.samplerate);
            sf_seek (sndfile, aud::min (frames, (int64_t) sfinfo.frames), SEEK_SET);
        }

        int samples = sf_read_float (sndfile, buffer.begin (), buffer.len ());
        if (! samples)
            break;

        write_audio (buffer.begin (), sizeof (float) * samples);
    }

    sf_close (sndfile);

    return true;
}

/* Function to see if WAV file contains embedded DTS format (contains a DTS header):
   (Borrowed & modified from:  https://hydrogenaud.io/index.php?PHPSESSID=0r361d02rgjc1uo2evsr91acue&action=dlattach;topic=94988.0;attach=10766)
   (See Reply# 19 in:  https://hydrogenaud.io/index.php?topic=94988.0)
   Function returns FALSE if DTS header found (DTS-WAV file which libsndfile accepts but can't play).
*/
bool SearchForDTSHeader (VFSFile & file)
{
    unsigned int i;
    unsigned char next6bytes[7];
    unsigned long ScanSize = file.fsize () - 12;

    if (ScanSize > 99999)
        ScanSize = 99999;  /* Limit search to first 99999 bytes, per original program. */

    for (i=0; i<ScanSize; i++)
    {
        if (file.fseek ((long)i, VFS_SEEK_SET) < 0)
            return true;  /* Couldn't seek, so assume NOT DTS. */

        if (file.fread (next6bytes, 6, 1))
        {
            if (next6bytes[0] == 0xFF && next6bytes[1] == 0x1F
                    && next6bytes[2] == 0x00
                    && next6bytes[3] == 0xE8
                    && (next6bytes[4] & 0xF0) == 0xF0
                    && next6bytes[5] == 0x07)
                return false;  /* Header found (we're DTS format embedded in a WAV file), let ffaudio handle! */
        }
        else
            return true;  /* Couldn't read enough, so assume NOT DTS. */
    }
    return true;  /* We're a standard WAV file, so accept. */
}

bool SndfilePlugin::is_our_file (const char * filename, VFSFile & file)
{
    bool ok = true;
    SF_INFO tmp_sfinfo {}; // must be zeroed before sf_open()

    /* Have to open the file virtually as a sndfile to see if libsndfile can handle it. */
    bool stream = (file.fsize () < 0);
    SNDFILE * tmp_sndfile = sf_open_virtual (stream ? & sf_virtual_io_stream :
     & sf_virtual_io, SFM_READ, & tmp_sfinfo, & file);

    if (! tmp_sndfile)
        return false;

    if (file.fsize () > 12)  /* We're not a stream, so check if DTS, if so, punt to ffaudio!: */
        ok = SearchForDTSHeader (file);

    /* It can so close file and return true (unless embedded DTS). */
    sf_close (tmp_sndfile);
    tmp_sndfile = nullptr;

    return ok;
}

const char SndfilePlugin::about[] =
 N_("Based on the xmms_sndfile plugin:\n"
    "Copyright (C) 2000, 2002 Erik de Castro Lopo\n\n"
    "Adapted for Audacious by Tony Vroon <chainsaw@gentoo.org>\n\n"
    "This program is free software; you can redistribute it and/or "
    "modify it under the terms of the GNU General Public License "
    "as published by the Free Software Foundation; either version 2 "
    "of the License, or (at your option) any later version.\n\n"
    "This program is distributed in the hope that it will be useful, "
    "but WITHOUT ANY WARRANTY; without even the implied warranty of "
    "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the "
    "GNU General Public License for more details.\n\n"
    "You should have received a copy of the GNU General Public License "
    "along with this program; if not, write to the Free Software "
    "Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.");

const char * const SndfilePlugin::exts[] = { "aiff", "au", "raw", "wav", nullptr };
const char * const SndfilePlugin::mimes[] = { "audio/wav", "audio/x-wav", nullptr };
