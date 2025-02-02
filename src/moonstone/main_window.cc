/*
 * main_window.cc
 * Copyright 2014 Michał Lipski
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

#include <QCloseEvent>
#include <QToolButton>
#include <QPushButton>
#include <QLayout>
#include <QSettings>
#include <QList>
#include <QKeyEvent>

#include <libfauxdcore/drct.h>
#include <libfauxdcore/i18n.h>
#include <libfauxdcore/plugins.h>
#include <libfauxdcore/runtime.h>

#include <libfauxdqt/libfauxdqt.h>
#include <libfauxdqt/info_bar.h>
#include <libfauxdqt/menu.h>

#include "main_window.h"
#include "tool_bar.h"
#include "time_slider.h"
#include "playlist.h"
#include "playlist_tabs.h"


namespace Moonstone {

void action_playlist_add_url ()
    { audqt::urlopener_show (false); }
void action_playlist_add_files ()
    { audqt::fileopener_show (audqt::FileMode::Add); }
void action_playlist_add_folder ()
    { audqt::fileopener_show (audqt::FileMode::AddFolder); }
void action_playlist_open_url ()
    { audqt::urlopener_show (true); }
void action_playlist_open_files ()
    { audqt::fileopener_show (audqt::FileMode::Open); }
void action_playlist_open_folder ()
    { audqt::fileopener_show (audqt::FileMode::OpenFolder); }

static bool m_is_shaded;

static const audqt::MenuItem add_menu_items[] = {
    audqt::MenuCommand ({N_("Add URL ..."), "folder-remote"}, action_playlist_add_url),
    audqt::MenuCommand ({N_("Add Folder ..."), "list-add"}, action_playlist_add_folder),
    audqt::MenuCommand ({N_("Add Files ..."), "list-add"}, action_playlist_add_files),
    audqt::MenuSep (),
    audqt::MenuCommand ({N_("Open URL ..."), "folder-remote"}, action_playlist_open_url),
    audqt::MenuCommand ({N_("Open Folder ..."), "document-open"}, action_playlist_open_folder),
    audqt::MenuCommand ({N_("Open Files ..."), "document-open"}, action_playlist_open_files)
};

static QString get_config_name ()
{
    String instancename = aud_get_instancename ();
    if (instancename == String ("fauxdacious"))
        return QString ("fauxdacious");
    else
        return QString ("fauxdacious-%1").arg ((const char *) instancename);
}

MainWindow::MainWindow () :
    m_config_name (get_config_name ()),
    m_center_widget (new QWidget(this)),
    m_center_layout (audqt::make_vbox (m_center_widget, 0)),
    m_playlist_tabs (new PlaylistTabs)
{
    int width = aud_get_int ("monestone", "window_w");
    int height = aud_get_int ("moonstone", "window_h");
    if (width < 100 && height < 100)
        this->resize (768, 480);
    else
        this->resize (width, height);

    m_is_shaded = aud_get_bool ("moonstone", "shaded");

    setCentralWidget (m_center_widget);

    QMenu * add_menu = audqt::menu_build (add_menu_items);
    auto shade_button = new QToolButton (this);
    shade_button->setIcon (QIcon::fromTheme ("go-top"));
    shade_button->setToolTip ("Shade");
    shade_button->setFocusPolicy (Qt::NoFocus);

    auto add_button = new QToolButton (this);
    add_button->setIcon (QIcon::fromTheme ("list-add"));
    add_button->setToolTip("Add Stuff");
    add_button->setFocusPolicy (Qt::NoFocus);
    add_button->setMenu (add_menu);
    add_button->setPopupMode (QToolButton::InstantPopup);

    auto slider = new TimeSlider (this);

    const ToolBarItem items[] = {
        ToolBarCustom (shade_button),
        ToolBarAction ("preferences-system", N_("Settings"), N_("Settings"),
                      []() { audqt::prefswin_show (); }),
        ToolBarCustom (add_button),
        ToolBarSeparator (),
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
        ToolBarCustom (slider->label()),
        ToolBarSeparator (),
        ToolBarAction (
            "media-playlist-repeat", N_("Repeat"), N_("Repeat"),
            [](bool on) { aud_set_bool (nullptr, "repeat", on); }, &m_repeat_action),
        ToolBarAction (
            "media-playlist-shuffle", N_("Shuffle"), N_("Shuffle"),
            [](bool on) { aud_set_bool (nullptr, "shuffle", on); }, &m_shuffle_action),
        ToolBarCustom (audqt::volume_button_new(this)),
        ToolBarSeparator (),
        ToolBarAction ("application-exit", N_("Quit"), N_("Quit"), aud_quit)
    };

    m_toolbar = new ToolBar (this, items);
    m_infobar = new audqt::InfoBar (nullptr);  /* nullptr=TREAT AS IF MINIFAUXDACIOUS INFOBAR! */

    m_center_layout->addWidget (m_infobar);
    m_center_layout->addWidget (m_toolbar);
    m_center_layout->addWidget (m_playlist_tabs);

    m_infobar->setToolTip ("Space: pause\nUp|Down: volume\nLeft|Right: seek\nB: next\nC: pause\nDelete: selected entries\nM: mute\nJ: jump to song\nT: toggle shade\nV: stop\nX: play\nZ: previous\nCtrl-Q: Quit");

    connect (shade_button, & QToolButton::clicked, [this] () {
        show_titlebar_toggle_cb ();
    });

    read_settings ();
    update_toggles ();
    width = this->geometry ().width ();
    height = this->geometry ().height ();
    m_is_shaded = aud_get_bool ("moonstone", "shaded");
    if (m_is_shaded)
    {
        int shaded_height = m_infobar->geometry ().height ();
        m_playlist_tabs->hide ();
        this->update ();
        this->resize (width, shaded_height);
        updateGeometry ();
        adjustSize ();
        update ();
        resize (width, shaded_height);
    }
    else
    {
        aud_set_int ("moonstone", "window_h", height);
        aud_set_int ("moonstone", "window_w", width);
    }
}

MainWindow::~MainWindow ()
{
    QSettings settings (m_config_name, "Moonstone_QtUi");
    settings.setValue ("geometry", saveGeometry ());
    settings.setValue ("windowState", saveState ());

    aud_set_int("moonstone", "window_x", this->geometry ().x ());
    if (! m_is_shaded)
        aud_set_int ("moonstone", "window_y", this->geometry ().y ());
}

void MainWindow::read_settings ()
{
    QSettings settings (m_config_name, "Moonstone_QtUi");

    if (! restoreGeometry (settings.value ("geometry").toByteArray ()))
    {
        // QWidget::restoreGeometry() can sometimes fail, e.g. due to
        // https://bugreports.qt.io/browse/QTBUG-86087. Try to at least
        // restore the correct player size in that case.
        resize (audqt::to_native_dpi ( aud_get_int("moonstone", "window_w")),
               audqt::to_native_dpi (aud_get_int("moonstone", "window_h")));
    }

    restoreState (settings.value ("windowState").toByteArray ());
}

void MainWindow::closeEvent (QCloseEvent * e)
{
    bool handled = false;

    aud_set_int ("moonstone", "window_x", this->geometry ().x ());
    aud_set_int ("moonstone", "window_y", this->geometry ().y ());
    aud_set_int ("moonstone", "window_w", this->geometry ().width ());
    if (! m_is_shaded)
        aud_set_int ("moonstone", "window_h", this->geometry ().height ());

    hook_call ("window close", &handled);

    if (! handled)
    {
        e->accept ();
        aud_quit ();
    }
    else
        e->ignore ();
}

void MainWindow::update_toggles ()
{
#if 0
    if (m_search_tool)
        m_search_action->setChecked(aud_plugin_get_enabled(m_search_tool));
#endif

    bool stop_after = aud_get_bool (nullptr, "stop_after_current_song");
    m_stop_action->setVisible (!stop_after);
    m_stop_after_action->setVisible (stop_after);
    m_stop_after_action->setChecked (stop_after);

    m_record_action->setVisible (aud_drct_get_record_enabled ());
    m_record_action->setChecked (aud_get_bool(nullptr, "record"));

    m_repeat_action->setChecked (aud_get_bool(nullptr, "repeat"));
    m_shuffle_action->setChecked (aud_get_bool(nullptr, "shuffle"));
}

void MainWindow::update_play_pause ()
{
    if (!aud_drct_get_playing () || aud_drct_get_paused ())
    {
        m_play_pause_action->setIcon (QIcon::fromTheme ("media-playback-start"));
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

void MainWindow::title_change_cb ()
{
    auto title = aud_drct_get_title ();
    if (title)
    {
        set_title(QString (title) + QString (" - Fauxdacious"));
        m_buffering_timer.stop ();
    }
}

void MainWindow::playback_begin_cb ()
{
    update_play_pause ();

    auto last_widget = m_playlist_tabs->playlistWidget (m_last_playing);
    if (last_widget)
        last_widget->updatePlaybackIndicator ();

    auto playing = aud_playlist_get_playing ();

    auto widget = m_playlist_tabs->playlistWidget (aud_playlist_get_playing ());
    if (widget)
        widget->scrollToCurrent ();
    if (widget && widget != last_widget)
        widget->updatePlaybackIndicator ();

    m_last_playing = playing;

    m_buffering_timer.queue (250, [this] () { set_title (_("Buffering ...")); });
}

void MainWindow::pause_cb ()
{
    update_play_pause ();

    auto widget = m_playlist_tabs->playlistWidget (m_last_playing);
    if (widget)
        widget->updatePlaybackIndicator ();
}

void MainWindow::playback_stop_cb ()
{
    set_title ("Fauxdacious");
    m_buffering_timer.stop ();

    update_play_pause ();

    auto last_widget = m_playlist_tabs->playlistWidget (m_last_playing);
    if (last_widget)
        last_widget->updatePlaybackIndicator ();

    m_last_playing = -1;
}

void MainWindow::set_title (const QString & title)
{
    String instancename = aud_get_instancename ();
    if (instancename == String ("fauxdacious"))
        QMainWindow::setWindowTitle (title);
    else
        QMainWindow::setWindowTitle (QString ("%1 (%2)").arg (title).arg ((const char *) instancename));
}

void MainWindow::show_titlebar_toggle_cb ()
{
    m_is_shaded = ! m_is_shaded;
    int x = this->geometry ().x ();
    int y = this->geometry ().y ();
    int width = this->geometry ().width ();
    int height = this->geometry ().height ();

    if (m_is_shaded)
    {
        int shaded_height = m_infobar->geometry ().height ();
        aud_set_int ("moonstone", "window_h", height);
        aud_set_int ("moonstone", "window_w", width);
        m_playlist_tabs->hide ();
        this->update ();
        this->resize (width, shaded_height);
        updateGeometry ();
        adjustSize ();
        update ();
        resize (width, shaded_height);
    }
    else
    {
        m_playlist_tabs->show ();
        height = aud_get_int("moonstone", "window_h");
        this->resize (width, height);
        updateGeometry ();
        adjustSize ();
        update ();
        resize (width, height);
        aud_set_int ("moonstone", "window_h", height);
        aud_set_int ("moonstone", "window_w", width);
    }
    aud_set_int ("moonstone", "window_x", x);
    aud_set_int ("moonstone", "window_y", y);
    aud_set_bool ("moonstone", "shaded", m_is_shaded);
}

}  // END MOONSTONE NAMESPACE
