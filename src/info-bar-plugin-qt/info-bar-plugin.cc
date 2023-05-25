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

#include <QWidget>
#include <QBoxLayout>
#include <QDockWidget>
#include <QMainWindow>
#include <QToolBar>
#include <libfauxdcore/preferences.h>
#include <libfauxdcore/drct.h>
#include <libfauxdcore/hook.h>
#include <libfauxdcore/i18n.h>
#include <libfauxdcore/plugin.h>
#include <libfauxdcore/runtime.h>
#include <libfauxdcore/playlist.h>
#include <libfauxdcore/plugins.h>
#include <libfauxdcore/hook.h>

#include "tool_bar.h"
#include "time_slider.h"
#include <libfauxdqt/libfauxdqt.h>
#include <libfauxdqt/info_bar.h>

static bool show_toolbar;

/* CALLED BY info_bar (USER DBL-CLICK OR "T") TO TOGGLE TOOLBAR SHADE: */
static void show_titlebar_toggle_fn ()
{
    show_toolbar = ! show_toolbar;
    hook_call ("toggle minifauxd toolbar", nullptr);
}

/* CALLED INTERNALLY BY PLUGIN-OPTIONS CHECKBOX (WHICH ALREADY PRETOGGLES B4 CALLING THIS): */
static void show_titlebar_toggled_fn ()
{
    hook_call ("toggle minifauxd toolbar", nullptr);
}

static void widget_cleanup (QObject * widget)
{
    hook_dissociate ("reverse minifauxd toolbar", (HookFunction) show_titlebar_toggle_fn, nullptr);
    if (aud_get_bool ("qtui", "_infoarea_was_visible"))
    {
        aud_set_bool ("qtui", "infoarea_visible", true);
        hook_call ("qtui toggle infoarea", nullptr);
    }
}

/* MINI-FAUXDACIOUS'S MAIN WINDOW, CONSISTS OF THE INFO-BAR, AND OPTIONALLY A MOONSTONE-ISH TOOLBAR: */
/* NOTE: THIS IS NOT THE SAME WIDGET AS THE DOCABLE WINDOW, WHICH SWALLOWS UP THIS ONE */
class MiniFauxdWin : public QMainWindow {
public:
    MiniFauxdWin ();

    QWidget * main_window;
    audqt::InfoBar * infobar_widget;
    ToolBar * toolbar_widget;

    QAction *m_play_pause_action, *m_stop_action, *m_stop_after_action;
    QAction *m_record_action;
    QAction *m_repeat_action, *m_shuffle_action;

    void update_toggles ();
    void update_play_pause ();
    void playback_begin_cb ();
    void pause_cb ();
    void playback_stop_cb ();
    void show_titlebar_toggle_cb ();  // NEEDED BY THE PLUGIN-CONFIG WIDGETS:

    const HookReceiver<MiniFauxdWin>
        hook1{"toggle minifauxd toolbar", this, &MiniFauxdWin::show_titlebar_toggle_cb},
        hook2{"playback begin", this, &MiniFauxdWin::playback_begin_cb},
        hook4{"playback pause", this, &MiniFauxdWin::pause_cb},
        hook5{"playback unpause", this, &MiniFauxdWin::pause_cb},
        hook6{"playback stop", this, &MiniFauxdWin::playback_stop_cb},
        hook7{"set stop_after_current_song", this, &MiniFauxdWin::update_toggles},
        hook8{"enable record", this, &MiniFauxdWin::update_toggles},
        hook9{"set record", this, &MiniFauxdWin::update_toggles},
        hook10{"set repeat", this, &MiniFauxdWin::update_toggles},
        hook11{"set shuffle", this, &MiniFauxdWin::update_toggles};
};

MiniFauxdWin::MiniFauxdWin () :
    main_window (new QWidget (this))
{
    auto slider = new TimeSlider (nullptr);

    const ToolBarItem items[] = {
        ToolBarAction ("list-add", N_("Add Files"), N_("Add Files"),
                []() { audqt::fileopener_show(audqt::FileMode::Add); }),
        ToolBarAction ("media-skip-backward", N_("Previous"), N_("Previous"),
                aud_drct_pl_prev),
        ToolBarAction ("media-playback-start", N_("Play"), N_("Play"),
                aud_drct_play_pause, &m_play_pause_action),
        ToolBarAction ("media-playback-stop", N_("Stop"), N_("Stop"),
                aud_drct_stop, &m_stop_action),
        ToolBarAction (
                "media-playback-stop", N_("Stop After This Song"),
                N_("Stop After This Song"),
                [](bool on) { aud_set_bool (nullptr, "stop_after_current_song", on); },
                &m_stop_after_action),
        ToolBarAction ("media-skip-forward", N_("Next"), N_("Next"),
                aud_drct_pl_next),
        ToolBarAction (
                "media-record", N_("Record Stream"), N_("Record Stream"),
                [](bool on) { aud_set_bool (nullptr, "record", on); }, &m_record_action),
        ToolBarSeparator (),
        ToolBarCustom (slider),
        ToolBarCustom (slider->label ()),
        ToolBarSeparator (),
        ToolBarAction (
            "media-playlist-repeat", N_("Repeat"), N_("Repeat"),
            [](bool on) { aud_set_bool (nullptr, "repeat", on); }, &m_repeat_action),
        ToolBarAction (
            "media-playlist-shuffle", N_("Shuffle"), N_("Shuffle"),
            [](bool on) { aud_set_bool (nullptr, "shuffle", on); }, &m_shuffle_action),
        ToolBarCustom ((QWidget *)audqt::volume_button_new (main_window))
    };

    setCentralWidget (main_window);
    toolbar_widget = new ToolBar (main_window, items);
    infobar_widget = new audqt::InfoBar (nullptr);
    if (aud_get_bool ("minifauxdacious", "moonstone_toolbarstyle"))
        toolbar_widget->setStyleSheet("QToolBar { background: rgba(255, 255, 255, 0.6); }");

    auto vbox = audqt::make_vbox (main_window, 0);
    vbox->addWidget (infobar_widget);
    vbox->addWidget (toolbar_widget);

    update_toggles ();

    infobar_widget->setToolTip ("Space: pause\nEsc: close\nUp|Down: volume\nCtrl-Q: Quit\nB: next\nC: pause\nT: toggle toolbar\nV: stop\nX: play\nZ: previous");

    /* SET THE INITIAL WINDOW-SIZE HERE BASED ON WHETHER STARTED W/TOOLBAR SHADED OR NOT:
       (FUTURE USER-CHANGES WILL BE HANDLED BY show_titlebar_toggle_cb ())
    */
    int width = main_window->geometry ().width ();  // SEEM TO BE PROPER VALUES (from main_window.cc):
    int height = main_window->geometry ().height ();
    int toolbar_height = toolbar_widget->geometry ().height ();

    if (aud_get_bool ("minifauxdacious", "show_toolbar"))
        toolbar_widget->show ();
    else
    {
        toolbar_widget->hide ();
        main_window->resize(width, height-toolbar_height);
        updateGeometry ();
        adjustSize ();
        update ();
        resize (width, height-toolbar_height);
    }
}

void MiniFauxdWin::update_toggles ()
{
#if 0
    if (m_search_tool)
        m_search_action->setChecked(aud_plugin_get_enabled(m_search_tool));
#endif

    bool stop_after = aud_get_bool (nullptr, "stop_after_current_song");
    m_stop_action->setVisible (!stop_after);
    m_stop_after_action->setVisible (stop_after);
    m_stop_after_action->setChecked (stop_after);

    m_record_action->setVisible (aud_drct_get_record_enabled());
    m_record_action->setChecked (aud_get_bool(nullptr, "record"));

    m_repeat_action->setChecked (aud_get_bool(nullptr, "repeat"));
    m_shuffle_action->setChecked (aud_get_bool(nullptr, "shuffle"));
}

void MiniFauxdWin::update_play_pause ()
{
    if (!aud_drct_get_playing() || aud_drct_get_paused ())
    {
        m_play_pause_action->setIcon (QIcon::fromTheme("media-playback-start"));
        m_play_pause_action->setText (_("Play"));
        m_play_pause_action->setToolTip (_("Play"));
    }
    else
    {
        m_play_pause_action->setIcon (QIcon::fromTheme("media-playback-pause"));
        m_play_pause_action->setText (_("Pause"));
        m_play_pause_action->setToolTip (_("Pause"));
    }
}

void MiniFauxdWin::playback_begin_cb ()
{
    update_play_pause ();
}

void MiniFauxdWin::pause_cb ()
{
    update_play_pause ();
}

void MiniFauxdWin::playback_stop_cb ()
{
    update_play_pause ();
}

void MiniFauxdWin::show_titlebar_toggle_cb () {
    PluginHandle * our_handle = aud_plugin_lookup_basename ("info-bar-plugin-qt");
    auto * dockitem = audqt::DockItem::find_by_plugin (our_handle);
    /* FIXME:WE MUST MAKE SURE WE'RE UNDOCKED ("FLOATING") BEFORE CHANGING SHADING
       (WINDOW-HEIGHT) SINCE DOING SO WHILST DOCKED BRICKS PLAYLIST-WINDOW GEOMETRY
       UNRECOVERABLY UNTIL RESTARTING FAUXDACIOUS (Qt LIMITATION/IDK HOW TO WORK AROUND)
       THEREFORE, IF DOCKED TO PLAYLIST-WINDOW, WE IGNORE ATTEMPTS TO CHANGE SHADING:
       (FAQ INSTRUCTS USERS TO FLOAT MINI-FAUXDACIUS BEFORE CHANGING THIS OPTION).
    */
    if (dockitem)
    {
        QDockWidget * dockwidget = (QDockWidget *) dockitem->host_data ();
        if (dockwidget && dockwidget->isFloating ())
        {
            /* WE'RE UNDOCKED (AND MUST BE TO ALLOW SHADING)!: */
            int width = dockwidget->geometry ().width ();
            int height = dockwidget->geometry ().height ();
            int toolbar_height = toolbar_widget->geometry ().height ();

            if (show_toolbar)  /* UNSHADE: */
            {
                height += toolbar_height;
                this->window()->setMaximumHeight(height);
                toolbar_widget->show();
                this->resize(width, height);
                updateGeometry();
                adjustSize();
                update();
                resize(width, height);
            }
            else  /* SHADE: (FIXME:SEEM TO NEED ALL THIS TO PERSUADE Qt TO SHRINK WINDOW TO FIT?!): */
            {
                height -= toolbar_height;
                toolbar_widget->hide ();
                main_window->resize (width, height);
                this->resize (width, height);
                this->window()->setMaximumHeight(height);
                this->window()->updateGeometry ();
                this->window()->adjustSize ();
                this->window()->update ();
                this->window()->resize (width, height);
            }
            aud_set_bool ("minifauxdacious", "show_toolbar", show_toolbar);
            return;
        }
    }
    AUDWARN ("w:Toolbar show/hide request IGNORED, window must be UNdocked first!");
    /* SO WE MUST INSTEAD PUT BACK HOW IT WAS SET (NO CHANGE)! */
    show_toolbar = ! show_toolbar;
    aud_set_bool ("minifauxdacious", "show_toolbar", show_toolbar);
}

static MiniFauxdWin * main_window;

class InfoBarPlugin : public GeneralPlugin {
public:
    static const char about[];

    static const char * const defaults[];
    static const PreferencesWidget widgets[];
    static const PluginPreferences prefs;
    static constexpr PluginInfo info = {
        N_("Mini-Fauxdacious"),
        PACKAGE,
        about,   // about
        & prefs, // prefs
        PluginQtOnly
    };

    constexpr InfoBarPlugin () : GeneralPlugin (info, false) {}

    bool init ();
    void * get_qt_widget ();
};

const char InfoBarPlugin::about[] =
 N_("Mini-Fauxdacious Plugin for Fauxdacious\n\n"
    "Copyright (C) 2021 Jim Turner <turnerjw784@yahoo.com>.\n\n"
    "Creates an InfoBar as a dockable window, which when main-\n"
    "window is minimized, provides a minimalist Fauxdacious\n\n"
    "Our answer to Audacious's Moonstone plugin!");

const char * const InfoBarPlugin::defaults[] = {
    "show_toolbar", "FALSE",
    "moonstone_toolbarstyle", "FALSE",
    nullptr
};

bool InfoBarPlugin::init ()
{
    aud_config_set_defaults ("minifauxdacious", defaults);
    show_toolbar = aud_get_bool ("minifauxdacious", "show_toolbar");
    return true;
}

const PreferencesWidget InfoBarPlugin::widgets[] = {
    WidgetLabel(N_("<b>MiniFauxdacious Configuration</b>")),
    WidgetCheck (N_("Show toolbar under info-bar? (toggle ignored if docked)"),
        WidgetBool (show_toolbar, show_titlebar_toggled_fn)),
    WidgetCheck (N_("Moonstone-ish toolbar? (must restart plugin)"),
        WidgetBool ("minifauxdacious", "moonstone_toolbarstyle")),
};

const PluginPreferences InfoBarPlugin::prefs = {{widgets}};

/* CALLED ON STARTUP (WIDGET CREATION): */
void * InfoBarPlugin::get_qt_widget ()
{
    /* JWT:HIDE EMBEDDED (CLASSIC) INFOBAR WHILE THIS PLUGIN IS ACTIVE: */
    bool show = aud_get_bool ("qtui", "infoarea_visible");
    aud_set_bool ("qtui", "_infoarea_was_visible", show);
    if (show)
    {
        aud_set_bool ("qtui", "infoarea_visible", false);
        hook_call ("qtui toggle infoarea", nullptr);
    }

    main_window = new MiniFauxdWin;

    QObject::connect (main_window, & QObject::destroyed, widget_cleanup);

    hook_associate ("reverse minifauxd toolbar", (HookFunction) show_titlebar_toggle_fn, nullptr);

    return main_window;
}

EXPORT InfoBarPlugin aud_plugin_instance;
