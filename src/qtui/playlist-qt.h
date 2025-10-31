/*
 * playlist.h
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

#ifndef PLAYLIST_H
#define PLAYLIST_H

#include <libfauxdcore/hook.h>
#include <libfauxdcore/mainloop.h>
#include <libfauxdcore/playlist.h>
#include <libfauxdqt/treeview.h>

class PlaylistModel;
class PlaylistProxyModel;
class QContextMenuEvent;
class QMenu;

class PlaylistWidget : public audqt::TreeView
{
public:
    PlaylistWidget (QWidget * parent, int playlist);
    ~PlaylistWidget ();

    int playlist () const
        { return m_playlist; }

    bool scrollToCurrent (bool force = false);
    void updatePlaybackIndicator ();
    void playlistUpdate ();
    void playCurrentIndex ();
    void setFilter (const char * text);
    void setFirstVisibleColumn (int col);
    void moveFocus (int distance);

    void setContextMenu (QMenu * menu)
        { contextMenu = menu; }

private:
    int m_playlist;
    PlaylistModel * model;
    PlaylistProxyModel * proxyModel;
    QMenu * contextMenu = nullptr;

    int currentPos = -1;
    bool inUpdate = false;
    int firstVisibleColumn = 0;

    int m_popup_pos = -1;
    QueuedFunc m_popup_timer;

    QModelIndex rowToIndex (int row);
    int indexToRow (const QModelIndex & index);
    QModelIndex visibleIndexNear (int row);

    void getSelectedRanges (int rowsBefore, int rowsAfter,
     QItemSelection & selected, QItemSelection & deselected);
    void updateSelection (int rowsBefore, int rowsAfter);

    void changeEvent (QEvent * event) override;
    void contextMenuEvent (QContextMenuEvent * event) override;
    void keyPressEvent (QKeyEvent * event) override;
    void mouseDoubleClickEvent (QMouseEvent * event) override;
    void mouseMoveEvent (QMouseEvent * event) override;
    void leaveEvent (QEvent * event) override;
    void dragMoveEvent (QDragMoveEvent * event) override;
    void dropEvent (QDropEvent * event) override;
    void currentChanged (const QModelIndex & current,
            const QModelIndex & previous) override;
    void selectionChanged (const QItemSelection & selected,
            const QItemSelection & deselected) override;
    QRect visualRect(const QModelIndex & index) const override;

    void showPopup ();
    void triggerPopup (int pos);
    void hidePopup ();

    void updateSettings ();

    const HookReceiver<PlaylistWidget>
     hook1 {"qtui update playlist settings", this, & PlaylistWidget::updateSettings};
};

#endif
