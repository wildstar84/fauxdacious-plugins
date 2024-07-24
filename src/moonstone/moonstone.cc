/*
 * moonstone.cc
 * Copyright 2020 Ariadne Conill
 * Fauxdacious enhancements Copyright 2024 Jim Turner
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
#include <QPointer>

#include <libfauxdcore/i18n.h>
#include <libfauxdcore/plugin.h>
#include <libfauxdcore/runtime.h>

#include <libfauxdqt/iface.h>
#include <libfauxdqt/libfauxdqt.h>

#include "settings.h"
#include "main_window.h"

static QPointer<Moonstone::MainWindow> s_window;

/* CALLED BY info_bar (USER DBL-CLICK OR "T") TO TOGGLE TOOLBAR SHADE: */
static void show_titlebar_toggle_fn ()
{
    hook_call ("toggle minifauxd toolbar", nullptr);
}

class MoonstoneUI : public audqt::QtIfacePlugin
{
public:
    constexpr MoonstoneUI () : audqt::QtIfacePlugin ({
        N_("Moonstone"),
        PACKAGE,
        nullptr,
        & moonstone_prefs,
        PluginQtOnly
    }) {}

    bool init ()
    {
        audqt::init ();
        aud_config_set_defaults ("moonstone", moonstone_defaults);
        s_window = new Moonstone::MainWindow;
        hook_associate ("reverse minifauxd toolbar", (HookFunction) show_titlebar_toggle_fn, nullptr);
        return true;
    }

    void cleanup ()
    {
        hook_dissociate ("reverse minifauxd toolbar", (HookFunction) show_titlebar_toggle_fn, nullptr);
        delete s_window;
        audqt::cleanup ();
    }

    void run () { QApplication::exec (); }

    void show (bool show)
    {
        if (! s_window)
            return;

        s_window->setVisible (show);

        if (show)
        {
            s_window->activateWindow ();
            s_window->raise ();
        }
    }

    void quit ()
    {
        QObject::connect (s_window.data (), &QObject::destroyed, QApplication::quit);
        s_window->deleteLater ();
    }
};

EXPORT MoonstoneUI aud_plugin_instance;
