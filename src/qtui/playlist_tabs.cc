/*
 * playlist_tabs.cc
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

#include "playlist_tabs.h"
#include "menus.h"
#include "playlist-qt.h"
#include "search_bar.h"
#include "settings.h"

#include <QBoxLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>

#include <libfauxdcore/drct.h>
#include <libfauxdcore/i18n.h>
#include <libfauxdcore/playlist.h>
#include <libfauxdcore/runtime.h>

#include <libfauxdqt/libfauxdqt.h>

class LayoutWidget : public QWidget
{
public:
    LayoutWidget (QWidget * parent, int playlist, QMenu * contextMenu);

    PlaylistWidget * playlistWidget () const { return m_playlistWidget; }

    void activateSearch ()
    {
        m_searchBar->show ();
        m_searchBar->setFocus ();
    }

private:
    PlaylistWidget * m_playlistWidget;
    SearchBar * m_searchBar;
};

LayoutWidget::LayoutWidget (QWidget * parent, int playlist, QMenu * contextMenu) :
    QWidget (parent),
    m_playlistWidget (new PlaylistWidget (this, playlist)),
    m_searchBar (new SearchBar (this, m_playlistWidget))
{
    auto layout = audqt::make_vbox (this, 0);
    layout->addWidget (m_playlistWidget);
    layout->addWidget (m_searchBar);

    m_playlistWidget->setContextMenu (contextMenu);
    m_searchBar->hide ();
}

/* --------------------------------- */

PlaylistTabs::PlaylistTabs (QWidget * parent) :
    QTabWidget (parent),
    m_pl_menu (qtui_build_pl_menu (this)),
    m_tabbar (new PlaylistTabBar (this))
{
    installEventFilter (this);

    // set up tab bar
    m_tabbar->setFocusPolicy (Qt::NoFocus);
    int tabHeight = aud_get_int ("qtui", "tabheight");
    if (tabHeight > 0)
    {
        QString styleThis = QString("QTabBar::tab { height: %1px; }").arg(tabHeight);
        m_tabbar->setStyleSheet(styleThis);
    }
    setTabBar (m_tabbar);

    addRemovePlaylists ();
    m_tabbar->updateTitles();
    m_tabbar->updateIcons();
    setCurrentIndex (aud_playlist_get_active ());

    connect (this, & QTabWidget::currentChanged, this, & PlaylistTabs::currentChangedTrigger);
}

PlaylistWidget * PlaylistTabs::currentPlaylistWidget () const
{
    return ((LayoutWidget *) currentWidget ())->playlistWidget ();
}

PlaylistWidget * PlaylistTabs::playlistWidget (int idx) const
{
    auto w = (LayoutWidget *) widget (idx);
    return w ? w->playlistWidget () : nullptr;
}

void PlaylistTabs::activateSearch ()
{
    ((LayoutWidget *) currentWidget ())->activateSearch ();
}

void PlaylistTabs::addRemovePlaylists ()
{
    int tabs = count();
    int playlists = aud_playlist_count ();

    for (int i = 0; i < tabs; i++)
    {
        auto w = (LayoutWidget *) widget (i);
        int list_idx = aud_playlist_by_unique_id (w->playlistWidget ()->playlist ());

        if (list_idx < 0)
        {
            removeTab (i);
            delete w;
            tabs--;
            i--;
        }
        else if (list_idx != i)
        {
            bool found = false;

            for (int j = i + 1; j < tabs; j++)
            {
                w = (LayoutWidget *) widget (j);
                list_idx = aud_playlist_by_unique_id (w->playlistWidget ()->playlist ());

                if (list_idx == i)
                {
                    removeTab (j);
                    insertTab (i, w, QString ());
                    found = true;
                    break;
                }
            }

            if (! found)
            {
                insertTab (i, new LayoutWidget (this, i, m_pl_menu), QString ());
                tabs++;
            }
        }
    }

    while (tabs < playlists)
    {
        addTab (new LayoutWidget (this, tabs, m_pl_menu), QString ());
        tabs++;
    }
    /* JWT:ONLY ALLOW TABS TO FOCUS IFF WE HAVE MULTIPLE TABS!: */
    setFocusPolicy (((tabs > 1) ? Qt::TabFocus : Qt::NoFocus));
}

void PlaylistTabs::currentChangedTrigger (int idx)
{
    if (! m_in_update)
        aud_playlist_set_active (idx);
}

bool PlaylistTabs::eventFilter (QObject * obj, QEvent * e)
{
    if (e->type() == QEvent::KeyPress)
    {
        QKeyEvent * ke = (QKeyEvent *)e;

        if (ke->key() == Qt::Key_Escape)
            return m_tabbar->cancelRename();
    }

    return QTabWidget::eventFilter(obj, e);
}

void PlaylistTabs::renameCurrent()
{
    auto playlist = currentPlaylistWidget()->playlist();

    if (m_tabbar->isVisible())
        m_tabbar->startRename(playlist);
    else
        audqt::playlist_show_rename(playlist);
}

void PlaylistTabs::playlist_activate_cb()
{
    m_in_update = true;
    setCurrentIndex(aud_playlist_get_active ());
    m_tabbar->cancelRename();
    m_in_update = false;
}

void PlaylistTabs::playlist_update_cb(Playlist::UpdateLevel global_level)
{
    m_in_update = true;
    if (global_level == Playlist::Structure)
        addRemovePlaylists();
    if (global_level >= Playlist::Metadata)
        m_tabbar->updateTitles();

    for (int i = 0; i < count(); i++)
        playlistWidget(i)->playlistUpdate();

    m_tabbar->updateIcons();  // JWT:NEED THIS TO KEEP TAB ICONS SET?! (AUDACIOUS DOESN'T)
    setCurrentIndex (aud_playlist_get_active ());
    m_in_update = false;
}

void PlaylistTabs::playlist_position_cb(int list)
{
    auto widget = playlistWidget(list);
    if (widget)
        widget->scrollToCurrent();
}

PlaylistTabBar::PlaylistTabBar(QWidget * parent) : QTabBar(parent)
{
    setMovable(true);
    setDocumentMode(true);
    updateSettings();

    connect(this, &QTabBar::tabMoved, this, &PlaylistTabBar::tabMoved);
    connect(this, &QTabBar::tabCloseRequested, [](int idx) {
        audqt::playlist_confirm_delete(idx);
    });
}

void PlaylistTabBar::updateTitles()
{
    int tabs = count();
    for (int i = 0; i < tabs; i++)
        updateTabText(i);
}

void PlaylistTabBar::updateIcons()
{
    QIcon icon;
    int playing = aud_playlist_get_playing ();
    if (playing >= 0)
        icon = audqt::get_icon(aud_drct_get_paused() ? "media-playback-pause"
                                                     : "media-playback-start");

    int tabs = count();
    for (int i = 0; i < tabs; i++)
    {
        /* hide icon when editing so it doesn't get shown on the wrong side */
        setTabIcon(i, (i == playing && !getTabEdit(i)) ? icon : QIcon());
    }
}

void PlaylistTabBar::startRename(int playlist)
{
    int idx = playlist;
    QLineEdit * edit = getTabEdit(idx);

    if (!edit)
    {
        edit = new QLineEdit((const char *) aud_playlist_get_title (idx));

        connect(edit, &QLineEdit::returnPressed, [this, playlist, edit]() {
            aud_playlist_set_title (playlist, (edit->text().toUtf8()));
            cancelRename();
        });

        setupTab(idx, edit, &m_leftbtn);
        updateIcons();
    }

    edit->selectAll();
    edit->setFocus();
}

bool PlaylistTabBar::cancelRename()
{
    bool cancelled = false;

    for (int i = 0; i < count(); i++)
    {
        QLineEdit * edit = getTabEdit(i);
        if (!edit)
            continue;

        setupTab(i, m_leftbtn, nullptr);
        m_leftbtn = nullptr;
        cancelled = true;
        updateIcons();
    }

    return cancelled;
}

void PlaylistTabBar::mousePressEvent(QMouseEvent * e)
{
    if (e->button() == Qt::MiddleButton)
    {
        int index = tabAt(e->pos());
        if (index >= 0)
        {
            audqt::playlist_confirm_delete(index);
            e->accept();
        }
    }

    QTabBar::mousePressEvent(e);
}

void PlaylistTabBar::mouseDoubleClickEvent(QMouseEvent * e)
{
    int idx = tabAt(e->pos());
    if (idx < 0 || e->button() != Qt::LeftButton)
        return;

    aud_playlist_play (idx);
}

void PlaylistTabBar::contextMenuEvent(QContextMenuEvent * e)
{
    int idx = tabAt(e->pos());
    if (idx < 0)
        return;

    auto menu = new QMenu(this);
    auto playlist = idx;

    auto play_act = new QAction(audqt::get_icon("media-playback-start"),
                                audqt::translate_str(N_("_Play")), menu);
    auto rename_act =
        new QAction(audqt::get_icon("insert-text"),
                    audqt::translate_str(N_("_Rename ...")), menu);
    auto remove_act = new QAction(audqt::get_icon("edit-delete"),
                                  audqt::translate_str(N_("Remo_ve")), menu);

    auto import_act = new QAction(audqt::get_icon("document-open"),
                                audqt::translate_str(N_("_Import")), menu);

    auto addfiles_act = new QAction(audqt::get_icon("list-add"),
                                audqt::translate_str(N_("_Add Files")), menu);

    QObject::connect(play_act, &QAction::triggered, [playlist] ()
    {
        aud_playlist_play (playlist);
    });
    QObject::connect(rename_act, &QAction::triggered, [this, playlist]() {
        if (playlist >= 0)
            startRename(playlist);
    });
    QObject::connect(remove_act, &QAction::triggered, [playlist] ()
    {
        if (playlist >= 0)
            audqt::playlist_confirm_delete(playlist);
    });
    QObject::connect(import_act, &QAction::triggered, [this, playlist] ()
    {
        setCurrentIndex(playlist);
        audqt::fileopener_show (audqt::FileMode::ImportPlaylist);
    });
    QObject::connect(addfiles_act, &QAction::triggered, [this, playlist] ()
    {
        setCurrentIndex(playlist);
        audqt::fileopener_show (audqt::FileMode::Add);
    });

    menu->addAction(play_act);
    menu->addAction(rename_act);
    menu->addAction(remove_act);
    menu->addAction(import_act);
    menu->addAction(addfiles_act);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->popup(e->globalPos());
}

QLineEdit * PlaylistTabBar::getTabEdit(int idx)
{
    return dynamic_cast<QLineEdit *>(tabButton(idx, QTabBar::LeftSide));
}

void PlaylistTabBar::updateTabText(int idx)
{
    QString title;

    if (!getTabEdit(idx))
    {
        auto playlist = idx;

        // escape ampersands for setTabText ()
        title = QString(aud_playlist_get_title (playlist)).replace("&", "&&");

        if (aud_get_bool("qtui", "entry_count_visible"))
            title += QString(" (%1)").arg(aud_playlist_entry_count (idx));
    }

    setTabText(idx, title);
}

void PlaylistTabBar::setupTab(int idx, QWidget * button, QWidget ** oldp)
{
    QWidget * old = tabButton(idx, QTabBar::LeftSide);
    setTabButton(idx, QTabBar::LeftSide, button);

    if (oldp)
        *oldp = old;
    else
    {
        old->setParent(nullptr);
        old->deleteLater();
    }

    updateTabText(idx);
}

void PlaylistTabBar::tabMoved(int from, int to)
{
    aud_playlist_reorder (from, to, 1);
}

void PlaylistTabBar::updateSettings()
{
#if QT_VERSION >= 0x050400
    setAutoHide(false);
#endif

    switch (aud_get_int("qtui", "playlist_tabs_visible"))
    {
#if QT_VERSION >= 0x050400
    case PlaylistTabVisibility::AutoHide:
        setAutoHide(true);
        break;
#endif
    case PlaylistTabVisibility::Always:
        show();
        break;

    case PlaylistTabVisibility::Never:
        hide();
        break;
    }

    setTabsClosable(aud_get_bool("qtui", "close_button_visible"));
    updateTitles();
}
