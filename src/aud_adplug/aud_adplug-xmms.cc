/*
   AdPlug/XMMS - AdPlug XMMS Plugin (Local Fauxdacious Classic version)
   Copyright (C) 2002, 2003 Simon Peter <dn.tlp@gmx.net>

   JWT:NOTE:  THIS PLUGIN IS *DEPRECIATED*!  MOST USERS SHOULD USE THE 
   MODERN VERSION WHICH USES THE SEPARATE libadplug PACKAGE NOW INSTALLED 
   ON MOST SYSTEMS.  The only reason I'm keeping this around is that on 
   64bit linux systems, libadplug requires a patch to prevent a bug that 
   crashes Fauxdacious when probing some streams, and some distros still 
   have the unpatched version!  See Audacious feature #759!
   This is the classic audacious internal version of the adplug plugin 
   that has been removed from mainline Audacious.  When Debian updates 
   libadplug to v2.3 (which is patched and works), I'll remove this plugin!

   AdPlug/XMMS is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This plugin is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this plugin; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include <algorithm>
#include <sstream>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "adplug.h"
#include "emuopl.h"
#include "silentopl.h"
#include "players.h"

#include <libfauxdcore/audstrings.h>
#include <libfauxdcore/i18n.h>
#include <libfauxdcore/plugin.h>
#include <libfauxdcore/runtime.h>
#include <libfauxdcore/preferences.h>

#define MIN_RATE 8000
#define MAX_RATE 192000
#define RATE_STEP 50
#define CFG_VERSION "AdPlug"

class AudAdPlugXMMS : public InputPlugin
{
public:
    static const char about[];
    static const char * const exts[];
    static const PreferencesWidget widgets[];
    static const PluginPreferences prefs;
    static const char * const defaults[];

    static constexpr PluginInfo info = {
        N_("AdPlug Classic (AdLib Player)"),
        PACKAGE,
        about,
        & prefs
    };

    constexpr AudAdPlugXMMS () : InputPlugin (info, InputInfo ()
        .with_exts (exts)) {}

    bool init ();
    void cleanup ();

    bool is_our_file (const char * filename, VFSFile & file);
    bool read_tag (const char * filename, VFSFile & file, Tuple & tuple, Index<char> * image);
    bool play (const char * filename, VFSFile & file);
};

EXPORT AudAdPlugXMMS aud_plugin_instance;

const char AudAdPlugXMMS::about[] =
 N_("AdPlug/XMMS - AdPlug XMMS Plugin\n"
    "(Local Fauxdacious Classic version)\n"
    "\n"
    "Fauxdacious plugin by:\n"
    "Copyright (C) 2002, 2003 Simon Peter <dn.tlp@gmx.net>\n"
    "\n"
    "This is the classic Audacious internal version of AdPlug.\n"
    "\n"
    "NOTE:  THIS PLUGIN IS *DEPRECIATED*!\n"
    "MOST USERS SHOULD USE THE MODERN VERSION which\n"
    "uses the separate libadplug package now installed\n"
    "on most systems.  See Audacious feature #759:\n"
    "https://redmine.audacious-media-player.org/issues/759\n"
    "\n"
    "Do NOT enable both this and the AdPlug plugin at same time!");

const char * const AudAdPlugXMMS::exts[] = {
    "a2m", "adl", "amd", "bam", "cff", "cmf", "d00", "dfm", "dmo", "dro",
    "dtm", "hsc", "hsp", "ins", "jbm", "ksm", "laa", "lds", "m", "mad",
    "mkj", "msc", "rad", "raw", "rix", "rol", "s3m", "sa2", "sat", "sci",
    "sng", "wlf", "xad", "xsm", nullptr
};

/***** Defines *****/

// Sound buffer size in samples
#define SNDBUFSIZE	512

// AdPlug's 8 and 16 bit audio formats
#define FORMAT_8	FMT_U8
#define FORMAT_16	FMT_S16_NE

// Default file name of AdPlug's database file
#define ADPLUGDB_FILE		"adplug.db"

// Default AdPlug user's configuration subdirectory
#define ADPLUG_CONFDIR		".adplug"

/***** Global variables *****/

// Configuration (and defaults)
static struct {
  int freq = 44100l;
  bool bit16 = true, stereo = false, endless = false;
} conf;

// Player variables
static struct {
  CPlayer *p = nullptr;
  CAdPlugDatabase *db = nullptr;
  unsigned int subsong = 0, songlength = 0;
  String filename;
} plr;

/***** Debugging *****/

#ifdef DEBUG

#include <stdarg.h>

static void
dbg_printf (const char *fmt, ...)
{
  va_list argptr;

  va_start (argptr, fmt);
  vfprintf (stderr, fmt, argptr);
  va_end (argptr);
}

#else
#define dbg_printf AUDINFO
#endif

/***** Main player (!! threaded !!) *****/

bool AudAdPlugXMMS::read_tag (const char * filename, VFSFile & file, Tuple & tuple,
 Index<char> * image)
{
  CSilentopl tmpopl;

  CFileProvider fp (file);

  /* JWT:FIXME: FUSE ADDED B/C FOLLOWING LINE *SEGFAULTS* IF .dll FILE PIPED IN VIA STDIN?! */
  if (! strncmp (filename, "stdin://-.d00", 13))
      return false;

  CPlayer *p = CAdPlug::factory (filename, &tmpopl, fp);

  if (! p)
    return false;

  if (! p->getauthor().empty())
    tuple.set_str (Tuple::Artist, p->getauthor().c_str());

  if (! p->gettitle().empty())
    tuple.set_str (Tuple::Title, p->gettitle().c_str());
  else if (! p->getdesc().empty())
    tuple.set_str (Tuple::Title, p->getdesc().c_str());

  tuple.set_str (Tuple::Codec, p->gettype().c_str());
  tuple.set_str (Tuple::Quality, _("sequenced"));
  tuple.set_int (Tuple::Length, p->songlength (plr.subsong));
  delete p;

  return true;
}

/* Main playback thread. Takes the filename to play as argument. */
bool AudAdPlugXMMS::play (const char * filename, VFSFile & fd)
{
  dbg_printf ("adplug_play(\"%s\"): ", filename);

  conf.bit16 = aud_get_bool (CFG_VERSION, "16bit");
  conf.stereo = aud_get_bool (CFG_VERSION, "Stereo");
  conf.freq = aud_get_int (CFG_VERSION, "Frequency");
  conf.endless = aud_get_bool (CFG_VERSION, "Endless");

  // Set XMMS main window information
  int sampsize = (conf.bit16 ? 2 : 1) * (conf.stereo ? 2 : 1);
  dbg_printf ("xmms, samplerate=%d, bitrate=%d.\n", sampsize, (conf.freq * sampsize * 8));
  set_stream_bitrate (conf.freq * sampsize * 8);

  // open output plugin
  dbg_printf ("open, ");
  open_audio (conf.bit16 ? FORMAT_16 : FORMAT_8, conf.freq, conf.stereo ? 2 : 1);

  CEmuopl opl (conf.freq, conf.bit16, conf.stereo);
  long toadd = 0, i, towrite;
  char *sndbuf, *sndbufpos;
  bool playing = true;  // Song self-end indicator.

  // Try to load module
  dbg_printf ("factory, ");
  CFileProvider fp (fd);
  if (!(plr.p = CAdPlug::factory (filename, &opl, fp)))
  {
    dbg_printf ("error!\n");
    // MessageBox("AdPlug :: Error", "File could not be opened!", "Ok");
    return false;
  }

  // reset to first subsong on new file
  dbg_printf ("subsong, ");
  if (! plr.filename || strcmp (filename, plr.filename))
  {
    plr.filename = String (filename);
    plr.subsong = 0;
  }

  // Allocate audio buffer
  dbg_printf ("buffer, ");
  sndbuf = (char *) malloc (SNDBUFSIZE * sampsize);

  // Rewind player to right subsong
  dbg_printf ("rewind, ");
  plr.p->rewind (plr.subsong);

  int time = 0;

  // main playback loop
  dbg_printf ("loop.\n");
  while ((playing || conf.endless))
  {
    if (check_stop ())
      break;

    int seek = check_seek ();

    // seek requested ?
    if (seek != -1)
    {
      // backward seek ?
      if (seek < time)
      {
        plr.p->rewind (plr.subsong);
        time = 0;
      }

      // seek to requested position
      while (time < seek && plr.p->update ())
        time += (int) (1000 / plr.p->getrefresh ());
    }

    // fill sound buffer
    towrite = SNDBUFSIZE;
    sndbufpos = sndbuf;
    while (towrite > 0)
    {
      while (toadd < 0)
      {
        toadd += conf.freq;
        playing = plr.p->update ();
        if (playing)
          time += (int) (1000 / plr.p->getrefresh ());
      }
      i = std::min (towrite, (long) (toadd / plr.p->getrefresh () + 4) & ~3);
      opl.update ((short *) sndbufpos, i);
      sndbufpos += i * sampsize;
      towrite -= i;
      toadd -= (long) (plr.p->getrefresh () * i);
    }

    write_audio (sndbuf, SNDBUFSIZE * sampsize);
  }

  // free everything and exit
  dbg_printf ("free");
  delete plr.p;
  plr.p = 0;
  free (sndbuf);
  dbg_printf (".\n");
  return true;
}

// sampsize macro not useful anymore.
#undef sampsize

/***** Informational *****/

bool AudAdPlugXMMS::is_our_file (const char * filename, VFSFile & fd)
{
  CSilentopl tmpopl;

  CFileProvider fp (fd);
  CPlayer *p = CAdPlug::factory (filename, &tmpopl, fp);

  dbg_printf ("adplug_is_our_file(\"%s\"): returned ", filename);

  if (p)
  {
    delete p;
    dbg_printf ("true\n");
    return true;
  }

  dbg_printf ("false\n");
  return false;
}

/***** Configuration file handling *****/

#define CFG_VERSION "AdPlug"

static const char * const adplug_defaults[] = {
 "16bit", "TRUE",
 "Stereo", "FALSE",
 "Frequency", "44100",
 "Endless", "FALSE",
 nullptr};

const PreferencesWidget AudAdPlugXMMS::widgets[] = {
    WidgetLabel (N_("<b>Advanced</b>")),
    WidgetCheck (N_("16 Bit Format (vs. 8 Bit)?"),
        WidgetBool (CFG_VERSION, "16bit")),
    WidgetCheck (N_("Stereo?"),
        WidgetBool (CFG_VERSION, "Stereo")),
    WidgetSpin (N_("Frequency (Rate)"),
        WidgetInt (CFG_VERSION, "Frequency"), {MIN_RATE, MAX_RATE, RATE_STEP, N_("Hz")}),
    WidgetCheck (N_("Repeat song in continuous loop?"),
        WidgetBool (CFG_VERSION, "Endless")),
    WidgetLabel (N_("<b>(Changes take effect after restarting play)</b>"))
};

const PluginPreferences AudAdPlugXMMS::prefs = {{widgets}};

bool AudAdPlugXMMS::init ()
{
  aud_config_set_defaults (CFG_VERSION, adplug_defaults);

  conf.bit16 = aud_get_bool (CFG_VERSION, "16bit");
  conf.stereo = aud_get_bool (CFG_VERSION, "Stereo");
  conf.freq = aud_get_int (CFG_VERSION, "Frequency");
  conf.endless = aud_get_bool (CFG_VERSION, "Endless");

  // Load database from disk and hand it to AdPlug
  dbg_printf ("database");
  {
    const char *homedir = getenv ("HOME");

    if (homedir)
    {
      std::string userdb;
      userdb = std::string ("file://") + homedir + "/" ADPLUG_CONFDIR "/" + ADPLUGDB_FILE;

      if (VFSFile::test_file (userdb.c_str (), VFS_EXISTS))
      {
        plr.db = new CAdPlugDatabase;
        plr.db->load (userdb);    // load user's database
        dbg_printf (" (userdb=\"%s\")", userdb.c_str());
        CAdPlug::set_database (plr.db);
      }
    }
  }
  dbg_printf (".\n");

  return true;
}

void AudAdPlugXMMS::cleanup ()
{
  // Close database
  dbg_printf ("db, ");
  if (plr.db)
    delete plr.db;

  plr.filename = String ();

  aud_set_bool (CFG_VERSION, "16bit", conf.bit16);
  aud_set_bool (CFG_VERSION, "Stereo", conf.stereo);
  aud_set_int (CFG_VERSION, "Frequency", conf.freq);
  aud_set_bool (CFG_VERSION, "Endless", conf.endless);
}
