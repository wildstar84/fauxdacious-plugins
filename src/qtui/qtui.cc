/*
 * qtui.cc
 * Copyright 2014 Michał Lipski
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

#include <QApplication>
#include <QDockWidget>

#define AUD_GLIB_INTEGRATION
#include <libfauxdcore/i18n.h>
#include <libfauxdcore/plugin.h>
#include <libfauxdcore/plugins.h>
#include <libfauxdcore/runtime.h>
#include <libfauxdcore/audstrings.h>

#include <libfauxdqt/libfauxdqt.h>
#include <libfauxdqt/iface.h>

#include <libfauxdqt/dock.h>
#include "main_window.h"
#include "settings.h"

class QtUI : public audqt::QtIfacePlugin
{
private:
    MainWindow * window = nullptr;

public:
    constexpr QtUI () : audqt::QtIfacePlugin ({
        N_("Qt Interface"),
        PACKAGE,
        nullptr,
        & qtui_prefs,
        PluginQtOnly
    }) {}

    bool init ()
    {
        audqt::init ();
        aud_config_set_defaults ("qtui", qtui_defaults);
        window = new MainWindow;
        return true;
    }

    void cleanup ()
    {
        /* JWT: Qt'S restoreState() DOESN'T PLAY NICE W/WINDOW-MANAGERS SINCE IT RESTORES ALL THE DOCK
           WINDOWS (SLAVES) *BEFORE* RESTORING THE MAIN (MASTER) WINDOW, WHICH MESSES UP THE "TAB-ORDER"
           IN MOST WINDOW-MANAGERS, *AND* PREVENTS "HonorGroupHints" AND PROPER "Layer" PLACEMENT IN
           AfterStep, AND PERHAPS OTHER (OLDER?) WINDOW-MANAGERS, SO WE OFFER THIS OPTION (DEFAULT: TRUE)
           TO WORK AROUND THIS BY DEACTIVATING ALL CURRENTLY "FLOATING" (INDEPENDENT WINDOW) DOCK WINDOW
           PLUGINS ON EXIT HERE, THEN MANUALLY RESTORING THEM *AFTER* SHOWING (MAPPING) THE MAIN WINDOW!:
        */
        if (aud_get_bool ("audqt", "restore_floating_dockapps_late"))
        {
            GKeyFile * rcfile = g_key_file_new ();
            int flag;
            for (PluginHandle * plugin : aud_plugin_list (PluginType::General))
            {
                flag = 0;
                if (aud_plugin_get_enabled (plugin))
                {
                    auto item = audqt::DockItem::find_by_plugin (plugin);
                    if (item)
                    {
                        auto w = (QDockWidget *) item->host_data ();
                        if (w && w->isFloating ())
                        {
                            aud_plugin_enable (plugin, false);
                            flag = 1;
                        }
                    }
                }
                g_key_file_set_double (rcfile, "Restore Floating Dockapps",
                        aud_plugin_get_basename (plugin), flag);
            }
            for (PluginHandle * plugin : aud_plugin_list (PluginType::Vis))
            {
                flag = 0;
                if (aud_plugin_get_enabled (plugin))
                {
                    auto item = audqt::DockItem::find_by_plugin (plugin);
                    if (item)
                    {
                        auto w = (QDockWidget *) item->host_data ();
                        if (w && w->isFloating ())
                        {
                            aud_plugin_enable (plugin, false);
                            flag = 1;
                        }
                    }
                }
                g_key_file_set_double (rcfile, "Restore Floating Dockapps",
                        aud_plugin_get_basename (plugin), flag);
            }
            flag = aud_get_bool("audqt", "equalizer_visible") ? 1 : 0;
            if (flag)
            {
                audqt::equalizer_hide ();
                aud_set_bool("audqt", "equalizer_visible", false);
            }
            g_key_file_set_double (rcfile, "Restore Floating Dockapps",
                    "equalizer", flag);
            flag = aud_get_bool("audqt", "queue_manager_visible") ? 1 : 0;
            if (flag)
            {
                audqt::queue_manager_hide ();
                aud_set_bool("audqt", "queue_manager_visible", false);
            }
            g_key_file_set_double (rcfile, "Restore Floating Dockapps",
                    "queue_manager", flag);

            size_t len;
            CharPtr data (g_key_file_to_data (rcfile, & len, nullptr));

            String late_restore_fid = String (str_concat ({aud_get_path (AudPath::UserDir), "/_restore_floating_dockapps.txt"}));
            VFSFile file (late_restore_fid, "w");
            if (! file || (file.fwrite (data, 1, len) != (int64_t) len))
                AUDERR ("e:Could not save floating dock plugin status!\n");

            g_key_file_free (rcfile);
        }

        delete window;
        window = nullptr;
        audqt::cleanup ();
    }

    void run ()
    {
        audqt::run ();
    }

    void show (bool show)
    {
        window->setVisible (show);  /* THIS "MAPS" THE MAIN WINDOW FOR THE WINDOW-MANAGER! */

        if (show)
        {
            /* JWT: Qt'S restoreState() DOESN'T PLAY NICE W/WINDOW-MANAGERS SINCE IT RESTORES ALL THE DOCK
               WINDOWS (SLAVES) *BEFORE* RESTORING THE MAIN (MASTER) WINDOW, WHICH MESSES UP THE "TAB-ORDER"
               IN MOST WINDOW-MANAGERS, *AND* PREVENTS "HonorGroupHints" AND PROPER "Layer" PLACEMENT IN
               AfterStep AND PERHAPS OTHER (OLDER?) WINDOW-MANAGERS, SO WE OFFER THIS OPTION (DEFAULT: TRUE)
               TO WORK AROUND THIS BY DEACTIVATING ALL CURRENTLY "FLOATING" (INDEPENDENT WINDOW) DOCK WINDOW
               PLUGINS ON EXIT, THEN MANUALLY RESTORING THEM *AFTER* SHOWING (MAPPING) THE MAIN WINDOW HERE!:
            */
            if (aud_get_bool ("audqt", "restore_floating_dockapps_late"))
            {
                String late_restore_fid = String (str_concat ({aud_get_path (AudPath::UserDir), "/_restore_floating_dockapps.txt"}));
                GKeyFile * rcfile = g_key_file_new ();
                if (g_key_file_load_from_file (rcfile, late_restore_fid,
                        (GKeyFileFlags)(G_KEY_FILE_KEEP_COMMENTS), nullptr))
                {
                    const char * flag;
                    aud_set_bool ("qtui", "_nofocusgrab", TRUE);  // JWT:DON'T GRAB FOCUS WHEN RESTARTING UP (MAIN WINDOW WON'T GET IT BACK - aud_get_bool ("qtui", "_nofocusgrab")Qt SUX AT THIS)!\n"
                    for (PluginHandle * plugin : aud_plugin_list (PluginType::General))
                    {
                        flag = g_key_file_get_string (rcfile, "Restore Floating Dockapps",
                                aud_plugin_get_basename (plugin), nullptr);
                        if (flag && flag[0] == '1')
                            aud_plugin_enable (plugin, true);
                    }
                    for (PluginHandle * plugin : aud_plugin_list (PluginType::Vis))
                    {
                        flag = g_key_file_get_string (rcfile, "Restore Floating Dockapps",
                                aud_plugin_get_basename (plugin), nullptr);
                        if (flag && flag[0] == '1')
                            aud_plugin_enable (plugin, true);
                    }
                    flag = g_key_file_get_string (rcfile, "Restore Floating Dockapps",
                            "equalizer", nullptr);
                    if (flag && flag[0] == '1')
                    {
                        audqt::equalizer_show ();
                        aud_set_bool("audqt", "equalizer_visible", true);
                    }
                    flag = g_key_file_get_string (rcfile, "Restore Floating Dockapps",
                            "queue_manager", nullptr);
                    if (flag && flag[0] == '1')
                    {
                        audqt::queue_manager_show ();
                        aud_set_bool("audqt", "queue_manager_visible", true);
                    }
                    aud_set_bool ("qtui", "_nofocusgrab", FALSE);  // JWT:DON'T GRAB FOCUS WHEN RESTARTING UP (MAIN WINDOW WON'T GET IT BACK - aud_get_bool ("qtui", "_nofocusgrab")Qt SUX AT THIS)!\n"
                }
            }
            window->activateWindow ();
            window->raise ();
            window->setFocus (Qt::OtherFocusReason);
        }
    }

    void quit ()
    {
        audqt::quit ();
    }
};

EXPORT QtUI aud_plugin_instance;
