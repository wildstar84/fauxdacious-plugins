/*
 * info-bar-plugin.cc (aka. "Mini-Fauxdacious")
 * Copyright 2021 Jim Turner
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

#include <libfauxdcore/drct.h>
#include <libfauxdcore/hook.h>
#include <libfauxdcore/i18n.h>
#include <libfauxdcore/plugin.h>
#include <libfauxdcore/runtime.h>
#include <libfauxdcore/playlist.h>
#include <libfauxdcore/hook.h>

#include <libfauxdqt/libfauxdqt.h>
#include <libfauxdqt/info_bar.h>

class InfoBarPlugin : public GeneralPlugin {
public:
    static const char about[];

    static constexpr PluginInfo info = {
        N_("Mini-Fauxdacious"),
        PACKAGE,
        about, // about
        nullptr, // prefs
        PluginQtOnly
    };

    constexpr InfoBarPlugin () : GeneralPlugin (info, false) {}
    void * get_qt_widget ();
};

const char InfoBarPlugin::about[] =
 N_("Mini-Fauxdacious Plugin for Fauxdacious\n\n"
    "Copyright (C) 2021 Jim Turner <turnerjw784@yahoo.com>.\n\n"
    "Creates an InfoBar as a dockable window, which when main-\n"
    "window is minimized, provides a minimalist Fauxdacious!");

static void widget_cleanup (QObject * widget)
{
    if (aud_get_bool ("qtui", "_infoarea_was_visible"))
    {
        aud_set_bool ("qtui", "infoarea_visible", true);
        hook_call ("qtui toggle infoarea", nullptr);
        aud_set_bool ("qtui", "infoarea_visible", true);
    }
}

/* CALLED ON STARTUP (WIDGET CREATION): */
void * InfoBarPlugin::get_qt_widget ()
{
    audqt::InfoBar * widget = new audqt::InfoBar (nullptr);

    QObject::connect (widget, &QObject::destroyed, widget_cleanup);

    /* JWT:HIDE EMBEDDED (CLASSIC) INFOBAR WHILE THIS PLUGIN IS ACTIVE: */
    bool show = aud_get_bool ("qtui", "infoarea_visible");
    aud_set_bool ("qtui", "_infoarea_was_visible", show);
    if (show)
    {
        aud_set_bool ("qtui", "infoarea_visible", false);
        hook_call ("qtui toggle infoarea", nullptr);
        aud_set_bool ("qtui", "infoarea_visible", false);
    }
    widget->setToolTip ("Space: pause\nEsc: close\nUp|Down: volume\nCtrl-Q: Quit\nB: next\nC: pause\nV: stop\nX: play\nZ: previous");

    return widget;
}

EXPORT InfoBarPlugin aud_plugin_instance;
