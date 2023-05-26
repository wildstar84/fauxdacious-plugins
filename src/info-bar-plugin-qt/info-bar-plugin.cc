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
static bool moonstone_toolbarstyle;

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

static void toolbarstyle_toggled_fn ()
{
    hook_call ("toggle minifauxd toolbar style", nullptr);
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
    void toolbarstyle_toggle_cb ();

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
        hook11{"set shuffle", this, &MiniFauxdWin::update_toggles},
        hook12{"toggle minifauxd toolbar style", this, &MiniFauxdWin::toolbarstyle_toggle_cb};
};

MiniFauxdWin::MiniFauxdWin () :
    main_window (new QWidget (this))
{
    auto slider = new TimeSlider (nullptr);

    unsigned short indx = 0;
    ToolBarItem items[22];   /* NOTE:MUST BE INCREASED IF ANY OPTIONS ADDED LATER! */

    /* NOW CREATE THE BUTTONS THAT THE USER WANTS ON THE TOOLBAR: */

    if (aud_get_bool ("minifauxdacious", "openfiles-btn"))
        items[indx++] = ToolBarAction("document-open", N_("Open Files"), N_("Open Files"),
                []() { audqt::fileopener_show(audqt::FileMode::Open); });
    if (aud_get_bool ("minifauxdacious", "addfiles-btn"))
        items[indx++] = ToolBarAction ("list-add", N_("Add Files"), N_("Add Files"),
                []() { audqt::fileopener_show(audqt::FileMode::Add); });
    if (aud_get_bool ("minifauxdacious", "openfolder-btn"))
        items[indx++] = ToolBarAction("document-open", N_("Open Folder"), N_("Open Folder"),
                []() { audqt::fileopener_show(audqt::FileMode::OpenFolder); });
    if (aud_get_bool ("minifauxdacious", "addfolder-btn"))
        items[indx++] = ToolBarAction ("list-add", N_("Add Folder"), N_("Add Folder"),
                []() { audqt::fileopener_show(audqt::FileMode::AddFolder); });
    if (aud_get_bool ("minifauxdacious", "openurl-btn"))
        items[indx++] = ToolBarAction("folder-remote", N_("Open URL"), N_("Open URL"),
                []() { audqt::urlopener_show (true); });
    if (aud_get_bool ("minifauxdacious", "addurl-btn"))
        items[indx++] = ToolBarAction("folder-remote", N_("Add URL"), N_("Add URL"),
                []() { audqt::urlopener_show (false); });
    if (aud_get_bool ("minifauxdacious", "settings-btn"))
        items[indx++] = ToolBarAction("preferences-system", N_("Settings"), N_("Settings"),
                []() { audqt::prefswin_show(); });
    if (aud_get_bool ("minifauxdacious", "playback-btns"))
    {
        items[indx++] = ToolBarAction ("media-skip-backward", N_("Previous"), N_("Previous"),
                aud_drct_pl_prev);
        items[indx++] = ToolBarAction ("media-playback-start", N_("Play"), N_("Play"),
                aud_drct_play_pause, &m_play_pause_action);
        items[indx++] = ToolBarAction ("media-playback-stop", N_("Stop"), N_("Stop"),
                aud_drct_stop, &m_stop_action);
        items[indx++] = ToolBarAction (
                "media-playback-stop", N_("Stop After This Song"),
                N_("Stop After This Song"),
                [](bool on) { aud_set_bool (nullptr, "stop_after_current_song", on); },
                &m_stop_after_action);
        items[indx++] = ToolBarAction ("media-skip-forward", N_("Next"), N_("Next"),
                aud_drct_pl_next);
    }
    if (aud_get_bool ("minifauxdacious", "record-btn"))
        items[indx++] = ToolBarAction (
                "media-record", N_("Record Stream"), N_("Record Stream"),
                [](bool on) { aud_set_bool (nullptr, "record", on); }, &m_record_action);
    if (indx > 0)
        items[indx++] = ToolBarSeparator ();

    if (aud_get_bool ("minifauxdacious", "slider-btn"))
        items[indx++] = ToolBarCustom (slider);
    if (aud_get_bool ("minifauxdacious", "time-label"))
        items[indx++] = ToolBarCustom (slider->label ());
    if (aud_get_bool ("minifauxdacious", "slider-btn")
            || aud_get_bool ("minifauxdacious", "time-label"))
        items[indx++] = ToolBarSeparator ();
    if (aud_get_bool ("minifauxdacious", "repeat-btn"))
        items[indx++] = ToolBarAction (
                "media-playlist-repeat", N_("Repeat"), N_("Repeat"),
                [](bool on) { aud_set_bool (nullptr, "repeat", on); }, &m_repeat_action);
    if (aud_get_bool ("minifauxdacious", "shuffle-btn"))
        items[indx++] = ToolBarAction (
                "media-playlist-shuffle", N_("Shuffle"), N_("Shuffle"),
                [](bool on) { aud_set_bool (nullptr, "shuffle", on); }, &m_shuffle_action);
    if (aud_get_bool ("minifauxdacious", "volume-btn"))
        items[indx++] = ToolBarCustom ((QWidget *)audqt::volume_button_new (main_window));
    if (aud_get_bool ("minifauxdacious", "quit-btn"))
        items[indx++] = ToolBarAction("application-exit", N_("Quit"), N_("Quit"),
                aud_quit);

    setCentralWidget (main_window);
    toolbar_widget = new ToolBar (main_window, items, indx);
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

    if (aud_get_bool ("minifauxdacious", "playback-btns"))
    {
        bool stop_after = aud_get_bool (nullptr, "stop_after_current_song");
        m_stop_action->setVisible (!stop_after);
        m_stop_after_action->setVisible (stop_after);
        m_stop_after_action->setChecked (stop_after);
    }

    if (aud_get_bool ("minifauxdacious", "record-btn"))
    {
        m_record_action->setVisible (aud_drct_get_record_enabled());
        m_record_action->setChecked (aud_get_bool(nullptr, "record"));
    }

    if (aud_get_bool ("minifauxdacious", "repeat-btn"))
        m_repeat_action->setChecked (aud_get_bool(nullptr, "repeat"));

    if (aud_get_bool ("minifauxdacious", "shuffle-btn"))
        m_shuffle_action->setChecked (aud_get_bool(nullptr, "shuffle"));
}

void MiniFauxdWin::update_play_pause ()
{
    if (aud_get_bool ("minifauxdacious", "playback-btns"))
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
                aud_set_int ("qtui", "mini_fauxdacious_w", width);
                width = aud_get_int ("qtui", "mini_fauxdacious_w_tb");
                if (width < 0)  width = dockwidget->geometry ().width ();
                height += toolbar_height;
                toolbar_widget->show();
              main_window->resize (width, height);
                this->resize(width, height);
              this->window()->setMaximumHeight(height);
                this->window()->updateGeometry();
                this->window()->adjustSize();
                this->window()->update();
                this->window()->resize(width, height);
            }
            else  /* SHADE: (FIXME:SEEM TO NEED ALL THIS TO PERSUADE Qt TO SHRINK WINDOW TO FIT?!): */
            {
                aud_set_int ("qtui", "mini_fauxdacious_w_tb", width);
                width = aud_get_int ("qtui", "mini_fauxdacious_w");
                if (width < 0)  width = dockwidget->geometry ().width ();
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

void MiniFauxdWin::toolbarstyle_toggle_cb () {
    if (moonstone_toolbarstyle)
        toolbar_widget->setStyleSheet("QToolBar { background: rgba(255, 255, 255, 0.6); }");
    else
        toolbar_widget->setStyleSheet("");

    aud_set_bool ("minifauxdacious", "moonstone_toolbarstyle", moonstone_toolbarstyle);
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
    "show_toolbar",           "FALSE",
    "moonstone_toolbarstyle", "FALSE",

    "openfiles-btn",      "FALSE",
    "addfiles-btn",       "TRUE",
    "openfolder-btn",     "FALSE",
    "addfolder-btn",      "FALSE",
    "openurl-btn",        "FALSE",
    "addurl-btn",         "FALSE",
    "settings-btn",       "FALSE",
    "playback-btns",      "TRUE",
    "record-btn",         "TRUE",
    "slider-btn",         "TRUE",
    "time-label",         "TRUE",
    "repeat-btn",         "TRUE",
    "shuffle-btn",        "TRUE",
    "volume-btn",         "TRUE",
    "quit-btn",           "FALSE",
    nullptr
};

bool InfoBarPlugin::init ()
{
    aud_config_set_defaults ("minifauxdacious", defaults);
    show_toolbar = aud_get_bool ("minifauxdacious", "show_toolbar");
    moonstone_toolbarstyle = aud_get_bool ("minifauxdacious", "moonstone_toolbarstyle");
    return true;
}

const PreferencesWidget InfoBarPlugin::widgets[] = {
    WidgetLabel(N_("<b>Mini-Fauxdacious Configuration</b>")),
    WidgetCheck (N_("Show toolbar under info-bar? (toggle ignored if docked)"),
        WidgetBool (show_toolbar, show_titlebar_toggled_fn)),
    WidgetCheck (N_("Audacious Moonstone-themed toolbar?"),
        WidgetBool (moonstone_toolbarstyle, toolbarstyle_toggled_fn)),
    WidgetLabel (N_("<b>Select toolbar-buttons:</b>")),
    WidgetCheck (N_("Open Files"),
        WidgetBool ("minifauxdacious", "openfiles-btn")),
    WidgetCheck (N_("Add Files"),
        WidgetBool ("minifauxdacious", "addfiles-btn")),
    WidgetCheck (N_("Open Folder"),
        WidgetBool ("minifauxdacious", "openfolder-btn")),
    WidgetCheck (N_("Add Folder"),
        WidgetBool ("minifauxdacious", "addfolder-btn")),
    WidgetCheck (N_("Open URL"),
        WidgetBool ("minifauxdacious", "openurl-btn")),
    WidgetCheck (N_("Add URL"),
        WidgetBool ("minifauxdacious", "addurl-btn")),
    WidgetCheck (N_("Settings"),
        WidgetBool ("minifauxdacious", "settings-btn")),
    WidgetCheck (N_("Playback controls (4 buttons)"),
        WidgetBool ("minifauxdacious", "playback-btns")),
    WidgetCheck (N_("Record"),
        WidgetBool ("minifauxdacious", "record-btn")),
    WidgetCheck (N_("Time Slider"),
        WidgetBool ("minifauxdacious", "slider-btn")),
    WidgetCheck (N_("Time Display"),
        WidgetBool ("minifauxdacious", "time-label")),
    WidgetCheck (N_("Repeat"),
        WidgetBool ("minifauxdacious", "repeat-btn")),
    WidgetCheck (N_("Shuffle"),
        WidgetBool ("minifauxdacious", "shuffle-btn")),
    WidgetCheck (N_("Volume"),
        WidgetBool ("minifauxdacious", "volume-btn")),
    WidgetCheck (N_("Quit"),
        WidgetBool ("minifauxdacious", "quit-btn")),
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
