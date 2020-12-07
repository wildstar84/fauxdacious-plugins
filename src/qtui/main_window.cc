/*
 * main_window.cc
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

#include "main_window.h"

#include <libfauxdcore/audstrings.h>
#include <libfauxdcore/drct.h>
#include <libfauxdcore/i18n.h>
#include <libfauxdcore/runtime.h>
#include <libfauxdcore/plugins.h>

#include <libfauxdqt/libfauxdqt.h>

#include "info_bar.h"
#include "menus.h"
#include "playlist-qt.h"
#include "playlist_tabs.h"
#include "settings.h"
#include "status_bar.h"
#include "time_slider.h"
#include "tool_bar.h"

#include <QAction>
#include <QBoxLayout>
#include <QCloseEvent>
#include <QDockWidget>
#include <QLabel>
#include <QMenuBar>
#include <QSettings>
#include <QToolButton>

class DockWidget : public QDockWidget
{
public:
    DockWidget(QWidget * parent, audqt::DockItem * item)
        : QDockWidget(parent), m_item(item)
    {
        setObjectName (item->id ());
        setWindowTitle (item->name ());
        setWidget (item->widget ());
        setContextMenuPolicy (Qt::PreventContextMenu);

        item->set_host_data(this);
    }

    void destroy()
    {
        if (in_event)
            deleteLater();
        else
            delete this;
    }

protected:
    void closeEvent (QCloseEvent * event) override
    {
        in_event = true;
        m_item->user_close ();
        event->ignore ();
        in_event = false;
    }

    void keyPressEvent(QKeyEvent * event) override
    {
        auto mods = Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier;
        if (!(event->modifiers () & mods) && event->key () == Qt::Key_Escape &&
                isFloating ())
        {
            in_event = true;
            m_item->user_close ();
            event->accept ();
            in_event = false;
            return;
        }

        QDockWidget::keyPressEvent (event);
    }

private:
    audqt::DockItem * m_item;
    bool in_event = false;
};

static QString get_config_name ()
{
    String instancename = aud_get_instancename ();
    if (instancename == String ("fauxdacious"))
        return QString ("fauxdacious");
    else
        return QString ("fauxdacious-%1").arg ((const char *) instancename);
}

static void toggle_search_tool (bool enable)
{
    if (enable)
        hook_call ("qtui show search tool", nullptr);
    else
    {
        auto search_tool = aud_plugin_lookup_basename ("search-tool-qt");
        if (search_tool)
            aud_plugin_enable (search_tool, false);
    }
}

static QToolButton * create_menu_button (QWidget * parent, QMenuBar * menubar)
{
    auto button = new QToolButton (parent);
    button->setIcon (audqt::get_icon ("fauxdacious"));
    button->setPopupMode (QToolButton::InstantPopup);
    button->setStyleSheet ("QToolButton::menu-indicator { image: none; }");
    button->setToolTip (_("Menu"));

    for (auto action : menubar->actions ())
        button->addAction (action);

    return button;
}

MainWindow::MainWindow () :
    m_config_name (get_config_name ()),
    m_dialogs (this),
    m_menubar (qtui_build_menubar (this)),
    m_playlist_tabs (new PlaylistTabs (this)),
    m_center_widget (new QWidget (this)),
    m_center_layout (audqt::make_vbox (m_center_widget, 0)),
    m_infobar (new InfoBar (this)),
    m_statusbar (new StatusBar (this)),
    m_search_tool (aud_plugin_lookup_basename ("search-tool-qt")),
    m_playlist_manager (aud_plugin_lookup_basename ("playlist-manager-qt"))
{
    auto slider = new TimeSlider (this);

    const ToolBarItem items[] = {
        ToolBarCustom (create_menu_button (this, m_menubar), & m_menu_action),
        ToolBarAction ("edit-find", N_("Search Library"), N_("Search Library"), toggle_search_tool, & m_search_action),
        ToolBarAction ("document-open", N_("Open Files"), N_("Open Files"),
            [] () { audqt::fileopener_show (audqt::FileMode::Open); }),
        ToolBarAction ("list-add", N_("Add Files"), N_("Add Files"),
            [] () { audqt::fileopener_show (audqt::FileMode::Add); }),
        ToolBarSeparator (),
        ToolBarAction ("media-skip-backward", N_("Previous"), N_("Previous"), aud_drct_pl_prev),
        ToolBarAction ("media-playback-start", N_("Play"), N_("Play"), aud_drct_play_pause, & m_play_pause_action),
        ToolBarAction ("media-playback-stop", N_("Stop"), N_("Stop"), aud_drct_stop, & m_stop_action),
        ToolBarAction ("media-playback-stop", N_("Stop After This Song"), N_("Stop After This Song"),
            [] (bool on) { aud_set_bool (nullptr, "stop_after_current_song", on); }, & m_stop_after_action),
        ToolBarAction ("media-skip-forward", N_("Next"), N_("Next"), aud_drct_pl_next),
        ToolBarAction ("media-record", N_("Record Stream"), N_("Record Stream"),
            [] (bool on) { aud_set_bool (nullptr, "record", on); }, & m_record_action),
        ToolBarSeparator (),
        ToolBarCustom (slider),
        ToolBarCustom (slider->label ()),
        ToolBarSeparator (),
        ToolBarAction ("media-playlist-repeat", N_("Repeat"), N_("Repeat"),
            [] (bool on) { aud_set_bool (nullptr, "repeat", on); }, & m_repeat_action),
        ToolBarAction ("media-playlist-shuffle", N_("Shuffle"), N_("Shuffle"),
            [] (bool on) { aud_set_bool (nullptr, "shuffle", on); }, & m_shuffle_action),
        ToolBarCustom (audqt::volume_button_new (this))
    };

    auto toolbar = new ToolBar (this, items);
    addToolBar (Qt::TopToolBarArea, toolbar);

    if (m_search_tool)
        aud_plugin_add_watch (m_search_tool, plugin_watcher, this);
    else
        m_search_action->setVisible (false);

    update_toggles ();

    setStatusBar (m_statusbar);
    setCentralWidget (m_center_widget);
//?    setMouseTracking (true);

    m_center_layout->addWidget (m_playlist_tabs);
    m_center_layout->addWidget (m_infobar);

    setMenuBar (m_menubar);
    setDockNestingEnabled (true);

    audqt::register_dock_host (this);

    if (aud_drct_get_playing ())
    {
        playback_begin_cb ();
        if (aud_drct_get_ready ())
            title_change_cb ();
    }
    else
        playback_stop_cb ();

    read_settings ();
    update_visibility ();

    /* Make sure UI elements are visible, in case restoreState() hid
     * them. It's not clear exactly how they can get hidden in the first
     * place, but user screenshots show that it somehow happens, and in
     * that case we don't want them to be gone forever. */
    toolbar->show ();
#if QT_VERSION < QT_VERSION_CHECK(5, 15, 0)
    int dock_count = 0;
#endif
    for (auto w : findChildren<DockWidget *> ())
    {
        w->show ();
#if QT_VERSION < QT_VERSION_CHECK(5, 15, 0)
        ++dock_count;
#endif
    }

    /* set initial keyboard focus on the playlist */
    m_playlist_tabs->currentPlaylistWidget ()->setFocus (Qt::OtherFocusReason);

#if QT_VERSION < QT_VERSION_CHECK(5, 15, 0)
    /* JWT:PREVENT WINDOW FROM "WALKING" UP BY THE HEIGHT OF THE WINDOW DECORATION!: */
    /* (THE QSettings SYSTEM APPARENTLY DOESN'T HANDLE DECORATED WINDOWS PROPERLY?!  */
    /* -- SEE ALSO: ffaudio-core.cc "FUDGE FACTOR" COMMENTS FOR RELATED ISSUE!)      */
    int x = aud_get_int ("qtui", "player_x");
    int y = aud_get_int ("qtui", "player_y");
    if (y <= 0)
        y = 25; /* JWT:MAKE SURE THE WINDOW TITLEBAR (WHICH USUALLY ALLOWS abUSER TO MOVE) ISN'T OFF THE SCREEN!: */

    if (dock_count > 0)
        this->move (x, y);
#endif
}

MainWindow::~MainWindow ()
{
    QSettings settings (m_config_name, "QtUi");
    settings.setValue ("geometry", saveGeometry ());
    settings.setValue ("windowState", saveState ());

#if QT_VERSION < QT_VERSION_CHECK(5, 15, 0)
    /* JWT:PREVENT WINDOW FROM "WALKING" UP BY THE HEIGHT OF THE WINDOW DECORATION!: */
    aud_set_int ("qtui", "player_x", this->geometry().x());
    aud_set_int ("qtui", "player_y", this->geometry().y());
#endif

    audqt::unregister_dock_host();

    if (m_search_tool)
        aud_plugin_remove_watch (m_search_tool, plugin_watcher, this);
}

void MainWindow::closeEvent (QCloseEvent * e)
{
    bool handled = false;

    hook_call ("window close", & handled);

    if (! handled)
    {
        e->accept ();
        aud_quit ();
    }
    else
        e->ignore ();
}

void MainWindow::keyPressEvent (QKeyEvent * event)
{
    auto CtrlShiftAlt = Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier;
    if (! (event->modifiers () & CtrlShiftAlt) && event->key () == Qt::Key_Escape)
    {
        auto widget = m_playlist_tabs->currentPlaylistWidget ();

        /* on the first press, set focus to the playlist */
        if (! widget->hasFocus ())
        {
            widget->setFocus (Qt::OtherFocusReason);
            return;
        }

        /* on the second press, scroll to the current entry */
        if (widget->scrollToCurrent (true))
            return;

        /* on the third press, switch to the playing playlist */
        int playingpl = aud_playlist_get_playing ();
        aud_playlist_set_active (playingpl);

        /* ensure currentPlaylistWidget() is up to date */
        widget = m_playlist_tabs->currentPlaylistWidget ();
        if (aud_playlist_update_pending (playingpl))
            widget->playlistUpdate ();

        widget->scrollToCurrent (true);

        return;
    }

    QMainWindow::keyPressEvent (event);
}

void MainWindow::read_settings ()
{
    QSettings settings (m_config_name, "QtUi");

    if (! restoreGeometry (settings.value ("geometry").toByteArray ()))
        resize (audqt::to_native_dpi (768), audqt::to_native_dpi (480));

    restoreState (settings.value ("windowState").toByteArray ());
}

void MainWindow::set_title (const QString & title)
{
    String instancename = aud_get_instancename ();
    if (instancename == String ("fauxdacious"))
        QMainWindow::setWindowTitle (title);
    else
        QMainWindow::setWindowTitle (QString ("%1 (%2)").arg (title).arg ((const char *) instancename));
}

void MainWindow::update_toggles ()
{
    if (m_search_tool)
        m_search_action->setChecked (aud_plugin_get_enabled (m_search_tool));

    bool stop_after = aud_get_bool (nullptr, "stop_after_current_song");
    m_stop_action->setVisible (! stop_after);
    m_stop_after_action->setVisible (stop_after);
    m_stop_after_action->setChecked (stop_after);

    m_record_action->setVisible (aud_drct_get_record_enabled ());
    m_record_action->setChecked (aud_get_bool (nullptr, "record"));

    m_repeat_action->setChecked (aud_get_bool (nullptr, "repeat"));
    m_shuffle_action->setChecked (aud_get_bool (nullptr, "shuffle"));
}

void MainWindow::update_visibility ()
{
    bool menu_visible = aud_get_bool ("qtui", "menu_visible");
    m_menubar->setVisible (menu_visible);
    m_menu_action->setVisible (!menu_visible);

    m_infobar->setVisible (aud_get_bool ("qtui", "infoarea_visible"));
    m_statusbar->setVisible (aud_get_bool ("qtui", "statusbar_visible"));
}

void MainWindow::update_play_pause ()
{
    if (! aud_drct_get_playing () || aud_drct_get_paused ())
    {
        m_play_pause_action->setIcon (audqt::get_icon ("media-playback-start"));
        m_play_pause_action->setText (_("Play"));
        m_play_pause_action->setToolTip (_("Play"));
    }
    else
    {
        m_play_pause_action->setIcon (audqt::get_icon ("media-playback-pause"));
        m_play_pause_action->setText (_("Pause"));
        m_play_pause_action->setToolTip (_("Pause"));
    }
}

void MainWindow::title_change_cb ()
{
    auto title = (const char *) str_get_first_line (aud_drct_get_title ());
    if (title)
    {
        set_title (QString (title) + QString (" - Fauxdacious"));
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
    auto widget = m_playlist_tabs->playlistWidget (playing);
    if (widget)
        widget->scrollToCurrent ();
    if (widget && widget != last_widget)
        widget->updatePlaybackIndicator ();

    m_last_playing = playing;

    m_buffering_timer.queue (250, aud::obj_member<MainWindow, & MainWindow::buffering_cb>, this);
}

void MainWindow::buffering_cb ()
{
    set_title (_("Buffering ..."));
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

void MainWindow::show_dock_plugin (PluginHandle * plugin)
{
    aud_plugin_enable (plugin, true);

    auto item = audqt::DockItem::find_by_plugin (plugin);
    if (item)
        item->grab_focus ();
}

void MainWindow::add_dock_item (audqt::DockItem * item)
{
    auto w = new DockWidget (this, item);

    if (! restoreDockWidget (w))
    {
        addDockWidget (Qt::LeftDockWidgetArea, w);
        // only the search tool and albumart plugins are docked by default:
        if (strcmp (item->id (), "search-tool-qt") && strcmp(item->id(), "albumart-qt"))
            w->setFloating (true);
    }

    /* workaround for QTBUG-89144 to make sure wm can manage the window! */
    auto flags = w->windowFlags();
    if (flags & Qt::X11BypassWindowManagerHint)
        w->setWindowFlags(flags & ~Qt::X11BypassWindowManagerHint);

    w->show (); /* in case restoreDockWidget() hid it */
}

void MainWindow::focus_dock_item (audqt::DockItem * item)
{
    auto w = (DockWidget *) item->host_data ();
    if (w->isFloating ())
        w->activateWindow ();
}

void MainWindow::remove_dock_item (audqt::DockItem * item)
{
    auto w = (DockWidget *) item->host_data ();
    w->destroy ();
}
