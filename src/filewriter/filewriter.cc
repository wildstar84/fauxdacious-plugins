/*  FileWriter-Plugin
 *  (C) copyright 2007 merging of Disk Writer and Out-Lame by Michael Färber
 *
 *  Original Out-Lame-Plugin:
 *  (C) copyright 2002 Lars Siebold <khandha5@gmx.net>
 *  (C) copyright 2006-2007 porting to audacious by Yoshiki Yazawa <yaz@cc.rim.or.jp>
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

#include <glib.h>
#include <string.h>

#include <libfauxdcore/audstrings.h>
#include <libfauxdcore/i18n.h>
#include <libfauxdcore/plugin.h>
#include <libfauxdcore/preferences.h>
#include <libfauxdcore/runtime.h>

#ifdef FILEWRITER_MP3
#include <lame/lame.h>
#endif

#include "filewriter.h"
#include "convert.h"

class FileWriter : public OutputPlugin
{
public:
    static const char about[];
    static const char * const defaults[];
    static const PreferencesWidget widgets[];
    static const PluginPreferences prefs;

    static constexpr PluginInfo info = {
        N_("FileWriter Plugin"),
        PACKAGE,
        about,
        & prefs
    };

    constexpr FileWriter () : OutputPlugin (info, 0, true) {}

    bool init ();
    void cleanup ();

    StereoVolume get_volume () { return {0, 0}; }
    void set_volume (StereoVolume v) {}

    void set_info (const char * filename, const Tuple & tuple);
    bool open_audio (int fmt, int rate, int nch, String & error);
    void close_audio ();

    void period_wait () {}
    int write_audio (const void * ptr, int length);
    void drain () {}

    int get_delay ()
        { return 0; }

    void pause (bool pause) {}
    void flush () {}
};

EXPORT FileWriter aud_plugin_instance;

enum {
    FILENAME_ORIGINAL,
    FILENAME_ORIGINAL_NO_SUFFIX,
    FILENAME_FROM_TAG,
    FILENAME_STDOUT
};

/* really a boolean, but stored here as an integer
 * since WidgetRadio supports only integer settings */
static int save_original;       // SAVE NEW FILE IN ORIGINAL DIRECTORY (OF FILE BEING PLAYED).
static int resetStdoutFmt;      // SAVE USER'S PREVIOUSLY-SELECTED FILE EXTENSION.
static int resetfilename_mode;  // SAVE USER'S PREVIOUSLY-SELECTED FILENAME MODE.

/* stored as two separate booleans in the config file */
static int filename_mode;

#ifdef FILEWRITER_MP3
/* stored as integers (0 or 1) in the config file */
static bool mp3_enforce_iso, mp3_error_protect;
static bool mp3_vbr_on, mp3_enforce_min, mp3_omit_xing;
static bool mp3_frame_copyright, mp3_frame_original;
static bool mp3_id3_force_v2, mp3_id3_only_v1, mp3_id3_only_v2;
#endif

static String in_filename;
static Tuple in_tuple;

enum fileext_t
{
    WAV = 0,
#ifdef FILEWRITER_MP3
    MP3,
#endif
#ifdef FILEWRITER_VORBIS
    VORBIS,
#endif
#ifdef FILEWRITER_FLAC
    FLAC,
#endif
    FILEEXT_MAX
};

static const char *fileext_str[FILEEXT_MAX] =
{
    ".wav",
#ifdef FILEWRITER_MP3
    ".mp3",
#endif
#ifdef FILEWRITER_VORBIS
    ".ogg",
#endif
#ifdef FILEWRITER_FLAC
    ".flac"
#endif
};

static FileWriterImpl *plugin;
static VFSFile output_file;

FileWriterImpl *plugins[FILEEXT_MAX] = {
    &wav_plugin,
#ifdef FILEWRITER_MP3
    &mp3_plugin,
#endif
#ifdef FILEWRITER_VORBIS
    &vorbis_plugin,
#endif
#ifdef FILEWRITER_FLAC
    &flac_plugin,
#endif
};

const char * const FileWriter::defaults[] = {
#ifdef FILEWRITER_MP3
 "fileext", aud::numeric_string<MP3>::str,
#else
 "fileext", aud::numeric_string<WAV>::str,
#endif
 "filenamefromtags", "TRUE",
 "prependnumber", "FALSE",
 "save_original", "FALSE",
 "stdout_close", "FALSE",
 "stdout_recclose", "TRUE",
 "use_suffix", "FALSE",
 "use_stdout", "FALSE",  /* JWT: ADDED TO WRITE TO STDOUT, IF TRUE. */
 nullptr};

bool FileWriter::init ()
{
    AUDDBG ("--FILEWRITER INIT\n");
    aud_config_set_defaults ("filewriter", defaults);

    save_original = aud_get_bool ("filewriter", "save_original");

    for (int i=0; i<FILEEXT_MAX;i++)  // JWT:NEEDED BY MAIN (-o OPTION)!
        aud_set_int ("filewriter", (const char *) str_concat ({"have_", fileext_str[i]+1}), i+1);

    if (aud_get_bool ("filewriter", "filenamefromtags"))
        filename_mode = FILENAME_FROM_TAG;
    else if (aud_get_bool ("filewriter", "use_suffix"))
        filename_mode = FILENAME_ORIGINAL;
    else if (aud_get_bool ("filewriter", "use_stdout"))
        filename_mode = FILENAME_STDOUT;
    else
        filename_mode = FILENAME_ORIGINAL_NO_SUFFIX;
    if (aud_get_stdout_fmt ())  // USER SPECIFIED "--out=<ext>", TEMPORARILY SWITCH TO stdout:
    {
        resetfilename_mode = filename_mode;  // SAVE USER'S PREVIOUSLY-SELECTED FILENAME MODE.
        resetStdoutFmt = aud_get_int ("filewriter", "fileext");  // SAVE USER'S PREVIOUSLY-SELECTED FILE EXTENSION.
        filename_mode = FILENAME_STDOUT;     // TEMPORARILY SWITCH (FORCE) FILENAME MODE TO stdout.
    }

    for (auto p : plugins)
    {
        if (p->init)
            p->init ();
    }

#ifdef FILEWRITER_MP3
    mp3_enforce_iso = aud_get_int ("filewriter_mp3", "enforce_iso_val");
    mp3_error_protect = aud_get_int ("filewriter_mp3", "error_protect_val");

    mp3_vbr_on = aud_get_int ("filewriter_mp3", "vbr_on");
    mp3_enforce_min = aud_get_int ("filewriter_mp3", "enforce_min_val");
    mp3_omit_xing = ! aud_get_int ("filewriter_mp3", "toggle_xing_val");

    mp3_frame_copyright = aud_get_int ("filewriter_mp3", "mark_copyright_val");
    mp3_frame_original = aud_get_int ("filewriter_mp3", "mark_original_val");

    mp3_id3_force_v2 = aud_get_int ("filewriter_mp3", "force_v2_val");
    mp3_id3_only_v1 = aud_get_int ("filewriter_mp3", "only_v1_val");
    mp3_id3_only_v2 = aud_get_int ("filewriter_mp3", "only_v2_val");
#endif

    return true;
}

void FileWriter::cleanup ()
{
    AUDDBG ("--FILEWRITER CLEANUP\n");
    if (output_file && plugin && filename_mode == FILENAME_STDOUT 
            && (aud_get_stdout_fmt () || aud_get_bool ("filewriter", "stdout_recclose")))  // CLOSE UP ANY DANGLING OPEN OUTPUT STREAM (INCLUDING stdout!):
    {
        AUDDBG ("-----ACTUALLY CLOSING STDOUT!\n");
        plugin->close (output_file);
        convert_free ();
        aud_set_str ("filewriter", "_record_fid", "");

        plugin = nullptr;
        output_file = VFSFile ();
        in_filename = String ();
        in_tuple = Tuple ();
    }
    if (aud_get_stdout_fmt ())
    {
        // RESTORE USER'S PREVIOUSLY-SELECTED PARAMETERS WE TEMPORARILY CHANGED FOR "--out=<ext>" (stdout)!:
        filename_mode = resetfilename_mode;
        aud_set_bool ("filewriter", "filenamefromtags", (filename_mode == FILENAME_FROM_TAG));
        aud_set_bool ("filewriter", "use_suffix", (filename_mode == FILENAME_ORIGINAL));
        aud_set_bool ("filewriter", "use_stdout", (filename_mode == FILENAME_STDOUT));
        aud_set_int ("filewriter", "fileext", resetStdoutFmt);  // RESTORE USER'S PREVIOUSLY-SELECTED FILE EXTENSION.
    }
}

static StringBuf get_file_path ()
{
    String path = aud_get_str ("filewriter", "file_path");
    return path[0] ? str_copy (path) : filename_to_uri (g_get_home_dir ());
}

static VFSFile safe_create (const char * filename)
{
    if (filename_mode == FILENAME_STDOUT || ! VFSFile::test_file (filename, VFS_EXISTS))
    {
        aud_set_str ("filewriter", "_record_fid", filename);
        return VFSFile (filename, "w");
    }

    const char * extension = strrchr (filename, '.');

    for (int count = 1; count < 100; count ++)
    {
        StringBuf scratch = extension ?
         str_printf ("%.*s-%d%s", (int) (extension - filename), filename, count, extension) :
         str_printf ("%s-%d", filename, count);

        if (! VFSFile::test_file (scratch, VFS_EXISTS))
        {
            aud_set_str ("filewriter", "_record_fid", (const char *) scratch);
            return VFSFile (scratch, "w");
        }
    }

    return VFSFile ();
}

void FileWriter::set_info (const char * filename, const Tuple & tuple)
{
    in_filename = String (filename);
    in_tuple = tuple.ref ();
}

static StringBuf format_filename (const char * suffix, bool fallback2unnamed)
{
    const char * slash = in_filename ? strrchr ((const char *) in_filename, '/') : nullptr;
    const char * base = slash ? slash + 1 : nullptr;

    StringBuf filename;

    if (! fallback2unnamed && filename_mode == FILENAME_STDOUT)
    {
#ifdef _WINDOWS
        filename = str_copy ("file://CON");
#else
        filename = str_copy ("file:///dev/stdout");
#endif
    }
    else
    {
        if (save_original)
        {
            StringBuf scheme = uri_get_scheme ((const char *) in_filename);
            if (! strcmp (scheme, "file") || ! aud_get_bool ("filewriter", "save_original_local"))
            {
                g_return_val_if_fail (base, StringBuf ());
                filename.insert (0, in_filename, base - (const char *) in_filename);
            }
            else
            {
                filename = get_file_path ();
                if (filename[filename.len () - 1] != '/')
                    filename.insert (-1, "/");
            }
        }
        else
        {
            filename = get_file_path ();
            if (filename[filename.len () - 1] != '/')
                filename.insert (-1, "/");
        }

        if (aud_get_bool ("filewriter", "prependnumber"))
        {
            int number = in_tuple.get_int (Tuple::Track);
            if (number >= 0)
                str_append_printf (filename, "%02d%%20", number);
        }

        if (! fallback2unnamed && aud_get_bool ("filewriter", "filenamefromtags"))
        {
            String title = in_tuple.get_str (Tuple::FormattedTitle);

            /* truncate title at 200 bytes to avoid hitting filesystem limits */
            int len = aud::min ((int) strlen (title), 200);

            /* prevent truncation in middle of UTF-8 character */
            while ((title[len] & 0xc0) == 0x80)
                len ++;

            StringBuf buf = str_copy (title, len);

            /* replace non-portable characters */
            const char * reserved = "<>:\"/\\|?*";
            for (char * c = buf; * c; c ++)
            {
                if (strchr (reserved, * c))
                    * c = ' ';
            }

            /* URI-encode */
            filename.insert (-1, str_encode_percent (buf));
        }
        else
        {
            g_return_val_if_fail (base, StringBuf ());

            const char * end = nullptr;

            if (fallback2unnamed || ! base || base[0] == '\0')
            {
                if (filename_mode == FILENAME_STDOUT)
                    filename.insert (-1, "stdout", 6);
                else if (! strncmp ((const char *)in_filename, "stdin:/", 7))
                    filename.insert (-1, "stdin", 5);
                else
                    filename.insert (-1, "unnamed", 7);
            }
            else 
            {
                if (! aud_get_bool ("filewriter", "use_suffix"))
                    end = strrchr (base, '.');
                if (strncmp (base, "-.", 2))  /* JWT:AVOID CREATING FILES NAMED "-.ext", RENAME TO "stdin.ext"! */
                    filename.insert (-1, base, end ? end - base : -1);
                else   /* JWT:WE HAVE:  m"^\-\..*"! */
                    filename.insert (-1, "stdin", 5);
            }
        }

        filename.insert (-1, suffix);
    }
    return filename.settle ();
}

bool FileWriter::open_audio (int fmt, int rate, int nch, String & error)
{
    if (output_file && filename_mode == FILENAME_STDOUT && plugin)
    {   /* JWT: IF WRITING TO STDOUT, ONLY OPEN EVERYTHING UP THE *FIRST* TIME!
           JUST SET THE CONVERSION TYPE (IT MAY'VE CHANGED) AND RETURN */
        convert_init (fmt, plugin->format_required (fmt));
        return true;
    }
    int ext = aud_get_int ("filewriter", "fileext");
    /* JWT:SAVE AND TEMP. OVERRIDE, IF SET ON COMMAND-LINE: */
    if (aud_get_stdout_fmt ())
    {
        resetStdoutFmt = ext;  // SAVE USER'S PREVIOUSLY-SELECTED FILE EXTENSION.
        ext = aud_get_stdout_fmt () - 1;  // CONVERT 1-BASED (0=OFF) TO 0-BASED FMT INDEX!
    }
    g_return_val_if_fail (ext >= 0 && ext < FILEEXT_MAX, false);

    StringBuf filename = format_filename (fileext_str[ext], false);
    if (! filename)
        filename = format_filename (fileext_str[ext], true);
    if (! filename)
        return false;

    plugin = plugins[ext];

    int out_fmt = plugin->format_required (fmt);
    convert_init (fmt, out_fmt);

    output_file = safe_create (filename);
    if (! output_file)  /* JWT:FILENAME (FROM URL?) TOO LONG, ETC, TRY FALLING BACK TO "unnamed": */
    {
        filename = format_filename (fileext_str[ext], true);
        output_file = safe_create (filename);
    }
    if (output_file)
    {
        if (plugin->open (output_file, {out_fmt, rate, nch}, in_tuple))
            return true;
        else
            aud_set_str ("filewriter", "_record_fid", "");
    }
    else
    {
        error = String (str_printf (_("Error opening %s:\n%s"),
         (const char *) filename, output_file.error ()));
    }

    plugin = nullptr;
    output_file = VFSFile ();
    in_filename = String ();
    in_tuple = Tuple ();
    return false;
}

int FileWriter::write_audio (const void * ptr, int length)
{
    auto & buf = convert_process (ptr, length);
    plugin->write (output_file, buf.begin (), buf.len ());

    return length;
}

void FileWriter::close_audio ()
{
    AUDDBG ("--FILEWRITER:CLOSE AUDIO\n");
    if (output_file && plugin)
    {
        if (filename_mode == FILENAME_STDOUT && 
                (aud_get_stdout_fmt () || ! aud_get_bool ("filewriter", "stdout_close")))
        {
            /* JWT: IF WRITING TO STDOUT, DON'T CLOSE OUTPUT, BUT NEXT OPEN WILL CHANGE CONVERSION TYPE! */
            convert_free ();
            AUDDBG ("-----FILEWRITER:NOT ACTUALLY CLOSING!...\n");
        }
        else
        {
            plugin->close (output_file);
            convert_free ();
            aud_set_str ("filewriter", "_record_fid", "");

            plugin = nullptr;
            output_file = VFSFile ();
            in_filename = String ();
            in_tuple = Tuple ();
            AUDDBG ("-----FILEWRITER:CLOSING!\n");
        }
    }
}

static void save_original_cb ()
{
    aud_set_bool ("filewriter", "save_original", save_original);
}

static void filename_mode_cb ()
{
    aud_set_bool ("filewriter", "filenamefromtags", (filename_mode == FILENAME_FROM_TAG));
    aud_set_bool ("filewriter", "use_suffix", (filename_mode == FILENAME_ORIGINAL));
    aud_set_bool ("filewriter", "use_stdout", (filename_mode == FILENAME_STDOUT));
}

const char FileWriter::about[] =
 N_("This program is free software; you can redistribute it and/or modify "
    "it under the terms of the GNU General Public License as published by "
    "the Free Software Foundation; either version 2 of the License, or "
    "(at your option) any later version.\n\n"
    "This program is distributed in the hope that it will be useful, "
    "but WITHOUT ANY WARRANTY; without even the implied warranty of "
    "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the "
    "GNU General Public License for more details.\n\n"
    "You should have received a copy of the GNU General Public License "
    "along with this program; if not, write to the Free Software "
    "Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, "
    "USA.");

static const ComboItem plugin_combo[] = {
    ComboItem ("WAV", WAV)
#ifdef FILEWRITER_MP3
    ,ComboItem ("MP3", MP3)
#endif
#ifdef FILEWRITER_VORBIS
    ,ComboItem ("Vorbis", VORBIS)
#endif
#ifdef FILEWRITER_FLAC
    ,ComboItem ("FLAC", FLAC)
#endif
};

static const PreferencesWidget main_widgets[] = {
    WidgetCombo (N_("Output file format:"),
        WidgetInt ("filewriter", "fileext"),
        {{plugin_combo}}),
    WidgetSeparator ({true}),
    WidgetRadio (N_("Save into original directory"),
        WidgetInt (save_original, save_original_cb),
        {true}),
    WidgetCheck (N_("(use custom for non-local directories)"),
        WidgetBool ("filewriter", "save_original_local"),
    WIDGET_CHILD),
    WidgetRadio (N_("Save into custom directory:"),
        WidgetInt (save_original, save_original_cb),
        {false}),
    WidgetFileEntry (nullptr,
        WidgetString ("filewriter", "file_path"),
        {FileSelectMode::Folder},
        WIDGET_CHILD),
    WidgetSeparator ({true}),
    WidgetLabel (N_("Generate file name from:")),
    WidgetRadio (N_("Original file name"),
        WidgetInt (filename_mode, filename_mode_cb),
        {FILENAME_ORIGINAL}),
    WidgetRadio (N_("Original file name (no suffix)"),
        WidgetInt (filename_mode, filename_mode_cb),
        {FILENAME_ORIGINAL_NO_SUFFIX}),
    WidgetRadio (N_("Original file tag"),
        WidgetInt (filename_mode, filename_mode_cb),
        {FILENAME_FROM_TAG}),
    WidgetRadio (N_("stdout"),
        WidgetInt (filename_mode, filename_mode_cb),
        {FILENAME_STDOUT}),
    WidgetCheck (N_("close after song chg."),
        WidgetBool ("filewriter", "stdout_close"),
    WIDGET_CHILD),
    WidgetCheck (N_("close when recording stopped."),
        WidgetBool ("filewriter", "stdout_recclose"),
    WIDGET_CHILD),
    WidgetSeparator ({true}),
    WidgetCheck (N_("Prepend track number to file name"),
        WidgetBool ("filewriter", "prependnumber"))
};

#ifdef FILEWRITER_MP3
static const ComboItem mp3_sample_rates[] = {
    ComboItem(N_("Auto"), 0),
    ComboItem(N_("8000 Hz"), 8000),
    ComboItem(N_("11025 Hz"), 11025),
    ComboItem(N_("12000 Hz"), 12000),
    ComboItem(N_("16000 Hz"), 16000),
    ComboItem(N_("22050 Hz"), 22050),
    ComboItem(N_("24000 Hz"), 24000),
    ComboItem(N_("32000 Hz"), 32000),
    ComboItem(N_("44100 Hz"), 44100),
    ComboItem(N_("48000 Hz"), 48000)
};

static const ComboItem mp3_bitrates[] = {
    ComboItem(N_("8 kbps"), 8),
    ComboItem(N_("16 kbps"), 16),
    ComboItem(N_("32 kbps"), 32),
    ComboItem(N_("40 kbps"), 40),
    ComboItem(N_("48 kbps"), 48),
    ComboItem(N_("56 kbps"), 56),
    ComboItem(N_("64 kbps"), 64),
    ComboItem(N_("80 kbps"), 80),
    ComboItem(N_("96 kbps"), 96),
    ComboItem(N_("112 kbps"), 112),
    ComboItem(N_("128 kbps"), 128),
    ComboItem(N_("160 kbps"), 160),
    ComboItem(N_("192 kbps"), 192),
    ComboItem(N_("224 kbps"), 224),
    ComboItem(N_("256 kbps"), 256),
    ComboItem(N_("320 kbps"), 320)
};

static const ComboItem mp3_modes[] = {
    ComboItem(N_("Auto"), NOT_SET),
    ComboItem(N_("Joint Stereo"), JOINT_STEREO),
    ComboItem(N_("Stereo"), STEREO),
    ComboItem(N_("Mono"), MONO)
};

static const ComboItem mp3_vbr_modes[] = {
    ComboItem(N_("VBR"), 0),
    ComboItem(N_("ABR"), 1)
};

static void mp3_bools_changed ()
{
    aud_set_int ("filewriter_mp3", "enforce_iso_val", mp3_enforce_iso);
    aud_set_int ("filewriter_mp3", "error_protect_val", mp3_error_protect);

    aud_set_int ("filewriter_mp3", "vbr_on", mp3_vbr_on);
    aud_set_int ("filewriter_mp3", "enforce_min_val", mp3_enforce_min);
    aud_set_int ("filewriter_mp3", "toggle_xing_val", ! mp3_omit_xing);

    aud_set_int ("filewriter_mp3", "mark_copyright_val", mp3_frame_copyright);
    aud_set_int ("filewriter_mp3", "mark_original_val", mp3_frame_original);

    aud_set_int ("filewriter_mp3", "force_v2_val", mp3_id3_force_v2);
    aud_set_int ("filewriter_mp3", "only_v1_val", mp3_id3_only_v1);
    aud_set_int ("filewriter_mp3", "only_v2_val", mp3_id3_only_v2);
}

static const PreferencesWidget mp3_quality_widgets[] = {
    WidgetSpin(N_("Algorithm quality:"),
        WidgetInt("filewriter_mp3", "algo_quality_val"),
        {0, 9, 1}),
    WidgetCombo(N_("Sample rate:"),
        WidgetInt("filewriter_mp3", "out_samplerate_val"),
        {{mp3_sample_rates}}),
    WidgetRadio(N_("Bitrate:"),
        WidgetInt("filewriter_mp3", "enc_toggle_val"),
        {0}),
    WidgetCombo(nullptr,
        WidgetInt("filewriter_mp3", "bitrate_val"),
        {{mp3_bitrates}},
        WIDGET_CHILD),
    WidgetRadio(N_("Compression ratio:"),
        WidgetInt("filewriter_mp3", "enc_toggle_val"),
        {1}),
    WidgetSpin(nullptr,
        WidgetFloat("filewriter_mp3", "compression_val"),
        {0, 100, 1},
        WIDGET_CHILD),
    WidgetCombo(N_("Audio mode:"),
        WidgetInt("filewriter_mp3", "audio_mode_val"),
        {{mp3_modes}}),
    WidgetCheck(N_("Enforce strict ISO compliance"),
        WidgetBool(mp3_enforce_iso, mp3_bools_changed)),
    WidgetCheck(N_("Error protection"),
        WidgetBool(mp3_error_protect, mp3_bools_changed)),
};

static const PreferencesWidget mp3_vbr_abr_widgets[] = {
    WidgetCheck(N_("Enable VBR/ABR"),
        WidgetBool(mp3_vbr_on, mp3_bools_changed)),
    WidgetCombo(N_("Type:"),
        WidgetInt("filewriter_mp3", "vbr_type"),
        {{mp3_vbr_modes}},
        WIDGET_CHILD),
    WidgetCombo(N_("Minimum bitrate:"),
        WidgetInt("filewriter_mp3", "vbr_min_val"),
        {{mp3_bitrates}},
        WIDGET_CHILD),
    WidgetCombo(N_("Maximum bitrate:"),
        WidgetInt("filewriter_mp3", "vbr_max_val"),
        {{mp3_bitrates}},
        WIDGET_CHILD),
    WidgetCombo(N_("Average bitrate:"),
        WidgetInt("filewriter_mp3", "abr_val"),
        {{mp3_bitrates}},
        WIDGET_CHILD),
    WidgetSpin(N_("VBR quality level:"),
        WidgetInt("filewriter_mp3", "vbr_quality_val"),
        {0, 9, 1},
        WIDGET_CHILD),
    WidgetCheck(N_("Strictly enforce minimum bitrate"),
        WidgetBool(mp3_enforce_min, mp3_bools_changed),
        WIDGET_CHILD),
    WidgetCheck(N_("Omit Xing VBR header"),
        WidgetBool(mp3_omit_xing, mp3_bools_changed),
        WIDGET_CHILD)
};

static const PreferencesWidget mp3_tag_widgets[] = {
    WidgetLabel(N_("<b>Frame Headers</b>")),
    WidgetCheck(N_("Mark as copyright"),
        WidgetBool(mp3_frame_copyright, mp3_bools_changed)),
    WidgetCheck(N_("Mark as original"),
        WidgetBool(mp3_frame_original, mp3_bools_changed)),
    WidgetLabel(N_("<b>ID3 Tags</b>")),
    WidgetCheck(N_("Force addition of version 2 tag"),
        WidgetBool(mp3_id3_force_v2, mp3_bools_changed)),
    WidgetCheck(N_("Only add v1 tag"),
        WidgetBool(mp3_id3_only_v1, mp3_bools_changed)),
    WidgetCheck(N_("Only add v2 tag"),
        WidgetBool(mp3_id3_only_v2, mp3_bools_changed)),
};

static const NotebookTab mp3_tabs[] = {
    {N_("Quality"), {mp3_quality_widgets}},
    {N_("VBR/ABR"), {mp3_vbr_abr_widgets}},
    {N_("Tags"), {mp3_tag_widgets}}
};

static const PreferencesWidget mp3_widgets[] = {
    WidgetNotebook ({{mp3_tabs}})
};
#endif

#ifdef FILEWRITER_VORBIS
static const PreferencesWidget vorbis_widgets[] = {
    WidgetSpin(N_("Quality (0-1):"),
        WidgetFloat("filewriter_vorbis", "base_quality"),
        {0, 1, 0.01})
};
#endif

static const NotebookTab tabs[] = {
    {N_("General"), {main_widgets}}
#ifdef FILEWRITER_MP3
    ,{"MP3", {mp3_widgets}}
#endif
#ifdef FILEWRITER_VORBIS
    ,{"Vorbis", {vorbis_widgets}}
#endif
};

const PreferencesWidget FileWriter::widgets[] = {
    WidgetNotebook ({{tabs}})
};

const PluginPreferences FileWriter::prefs = {{widgets}};
