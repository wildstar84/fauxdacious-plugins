/*
 * playlist.cc
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

#include <QHeaderView>
#include <QKeyEvent>
#include <QMenu>
#include <QSortFilterProxyModel>
#include <QProxyStyle>

#include <libfauxdcore/audstrings.h>
#include <libfauxdcore/drct.h>
#include <libfauxdcore/runtime.h>
#include <libfauxdcore/interface.h>
#include <libfauxdqt/libfauxdqt.h>

#include "playlist.h"
#include "playlist_header.h"
#include "playlist_model.h"

#include "../ui-common/menu-ops.h"
#include "../ui-common/qt-compat.h"

namespace Moonstone {

PlaylistWidget::PlaylistWidget (QWidget * parent, int playlist) :
    audqt::TreeView (parent),
    m_playlist (playlist),
    model (new PlaylistModel (this, playlist)),
    proxyModel (new PlaylistProxyModel (this, playlist))
{
    /* setting up filtering model */
    proxyModel->setSourceModel (model);

    inUpdate = true; /* prevents changing focused row */
    setModel (proxyModel);
    inUpdate = false;

    auto header = new PlaylistHeader (this);
    setHeader (header);

    /* this has to come after setHeader() to take effect */
    header->setSectionsClickable (true);
    header->setSortIndicatorShown (true);

    setAllColumnsShowFocus (true);
    setAlternatingRowColors (true);
    setAttribute (Qt::WA_MacShowFocusRect, false);
    setUniformRowHeights (true);
    setFrameShape (QFrame::NoFrame);
    setSelectionMode (ExtendedSelection);
    setDragDropMode (DragDrop);
    setMouseTracking (true);

    connect (this, &QTreeView::activated, this, &PlaylistWidget::activate);

    updateSettings ();
    header->updateColumns ();

    /* get initial selection and focus from core */
    inUpdate = true;
    updateSelection (0, 0);
    inUpdate = false;
}

PlaylistWidget::~PlaylistWidget ()
{
    delete model;
    delete proxyModel;
}

QModelIndex PlaylistWidget::rowToIndex (int row)
{
    if (row < 0)
        return QModelIndex ();

    return proxyModel->mapFromSource (model->index (row, firstVisibleColumn));
}

int PlaylistWidget::indexToRow (const QModelIndex & index)
{
    if (! index.isValid ())
        return -1;

    return proxyModel->mapToSource (index).row ();
}

QModelIndex PlaylistWidget::visibleIndexNear (int row)
{
    QModelIndex index = rowToIndex (row);
    if (index.isValid ())
        return index;

    int n_entries = aud_playlist_entry_count (m_playlist);

    for (int r = row + 1; r < n_entries; r++)
    {
        index = rowToIndex (r);
        if (index.isValid ())
            return index;
    }

    for (int r = row - 1; r >= 0; r--)
    {
        index = rowToIndex (r);
        if (index.isValid ())
            return index;
    }

    return index;
}

void PlaylistWidget::activate (const QModelIndex & index)
{
    if (index.isValid ())
    {
        aud_playlist_set_position (m_playlist, indexToRow (index));
        aud_playlist_play (m_playlist);
    }
}

void PlaylistWidget::contextMenuEvent (QContextMenuEvent * event)
{
    if (contextMenu)
        contextMenu->popup (event->globalPos ());
}

void PlaylistWidget::keyPressEvent (QKeyEvent * event)
{
    auto CtrlShiftAlt = Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier;
    if (! (event->modifiers () & CtrlShiftAlt))
    {
        switch (event->key ())
        {
          case Qt::Key_Right:
            aud_drct_seek (aud_drct_get_time () + aud_get_int (nullptr, "step_size") * 1000);
            return;
          case Qt::Key_Left:
            aud_drct_seek (aud_drct_get_time () - aud_get_int (nullptr, "step_size") * 1000);
            return;
          case Qt::Key_Space:
            aud_drct_play_pause ();
            return;
          case Qt::Key_Delete:
            //pl_remove_selected ();
            aud_playlist_delete_selected (aud_playlist_get_active ());
            return;
          case Qt::Key_Z:
            aud_drct_pl_prev ();
            return;
          case Qt::Key_X:
            aud_drct_play ();
            return;
          case Qt::Key_C:
            aud_drct_pause ();
            return;
          case Qt::Key_J:
            aud_ui_show_jump_to_song ();
            return;
          case Qt::Key_M:
            {
              /* JWT:TOGGLE MUTE: */
              int current_volume = aud_drct_get_volume_main ();
              if (current_volume)
              {
                  aud_set_int ("audacious", "_premuted_volume", current_volume);
                  aud_drct_set_volume_main (0);
              }
              else
                  aud_drct_set_volume_main (aud_get_int ("audacious", "_premuted_volume"));

              return;
            }
          case Qt::Key_V:
            aud_drct_stop ();
            return;
          case Qt::Key_B:
            aud_drct_pl_next ();
            return;
        }
    }
    else if (event->modifiers () & Qt::ControlModifier)
    {
        switch (event->key ())
        {
          case Qt::Key_Q:
             aud_quit ();
              return;
        }
    }

    audqt::TreeView::keyPressEvent (event);
}

void PlaylistWidget::mouseMoveEvent (QMouseEvent * event)
{
    int row = indexToRow (indexAt (QtCompat::pos (event)));

    if (row < 0)
        hidePopup ();
    else if (aud_get_bool ("moonstone", "show_filepopup_for_tuple") && m_popup_pos != row)
        triggerPopup (row);

    audqt::TreeView::mouseMoveEvent (event);
}

void PlaylistWidget::leaveEvent (QEvent * event)
{
    hidePopup ();

    audqt::TreeView::leaveEvent (event);
}

/* Since Qt doesn't support both DragDrop and InternalMove at once,
 * this hack is needed to set the drag icon to "move" for internal drags. */
void PlaylistWidget::dragMoveEvent (QDragMoveEvent * event)
{
    if (event->source () == this)
        event->setDropAction (Qt::MoveAction);

    audqt::TreeView::dragMoveEvent (event);

    if (event->source () == this)
        event->setDropAction (Qt::MoveAction);
}

void PlaylistWidget::dropEvent (QDropEvent * event)
{
    /* let Qt forward external drops to the PlaylistModel */
    if (event->source () != this)
        return audqt::TreeView::dropEvent (event);

    int from = indexToRow (currentIndex ());
    if (from < 0)
        return;

    int to;
    switch (dropIndicatorPosition ())
    {
    case AboveItem:
        to = indexToRow (indexAt (QtCompat::pos (event)));
        break;
    case BelowItem:
        to = indexToRow (indexAt (QtCompat::pos (event))) + 1;
        break;
    case OnViewport:
        to = aud_playlist_entry_count (m_playlist);
        break;
    default:
        return;
    }

    /* Adjust the shift amount so that the selected entry closest to the
     * destination ends up at the destination. */
    if (to > from)
        to -= aud_playlist_selected_count (m_playlist, from, to - from);
    else
        to += aud_playlist_selected_count (m_playlist, to, from - to);

    aud_playlist_shift (m_playlist, from, to - from);

    event->acceptProposedAction ();
}

void PlaylistWidget::currentChanged (const QModelIndex & current, const QModelIndex & previous)
{
    audqt::TreeView::currentChanged (current, previous);

    if (! inUpdate)
        aud_playlist_set_focus (m_playlist, indexToRow (current));
}

void PlaylistWidget::selectionChanged (const QItemSelection & selected,
        const QItemSelection & deselected)
{
    audqt::TreeView::selectionChanged (selected, deselected);

    if (! inUpdate)
    {
        for (const QModelIndex & idx : selected.indexes ())
            aud_playlist_entry_set_selected (m_playlist, indexToRow (idx), true);
        for (const QModelIndex & idx : deselected.indexes ())
            aud_playlist_entry_set_selected (m_playlist, indexToRow (idx), false);
    }
}

/* returns true if the focus changed or the playlist scrolled */
bool PlaylistWidget::scrollToCurrent (bool force)
{
    bool scrolled = false;
    int entry = aud_playlist_get_position (m_playlist);

    if (entry >= 0 && (aud_get_bool ("moonstone", "autoscroll") || force))
    {
        if (aud_playlist_get_focus (m_playlist) != entry)
            scrolled = true;

        if (aud_get_bool (nullptr, "advance_2_next_selected"))
        {
            int prev_entry = aud_get_int (nullptr, "_prev_entry");
            if (prev_entry >= 0)
            {
                /* JWT:ONLY KEEP PREV. ENTRY SELECTED IFF OPTION SET && ADVANCED-TO ENTRY IS HILIGHTED
                   (THIS ENDS "Advance to next selected entry in list" IF NO OTHER ITEMS HIGHLIGHTED!)
                   NOTE:  Advance to next selected entry in list ONLY WORKS IF 2+ ENTRIES HIGHLIGHTED!
                */
                if (! aud_get_bool (nullptr, "keep_selected_on_advance")
                        || ! aud_playlist_entry_get_selected (m_playlist, entry))
                    aud_playlist_entry_set_selected (m_playlist, prev_entry, false);

                aud_set_int (nullptr, "_prev_entry", -1);
            }
        }
        else
            aud_playlist_select_all (m_playlist, false);

        aud_playlist_entry_set_selected (m_playlist, entry, true);
        aud_playlist_set_focus (m_playlist, entry);

        auto index = rowToIndex (entry);
        auto rect = visualRect (index);

        scrollTo (index);

        if (visualRect (index) != rect)
            scrolled = true;
    }

    return scrolled;
}

void PlaylistWidget::updatePlaybackIndicator ()
{
    if (currentPos >= 0)
        model->entriesChanged (currentPos, 1);
}

void PlaylistWidget::getSelectedRanges (int rowsBefore, int rowsAfter,
        QItemSelection & selected, QItemSelection & deselected)
{
    int entries = aud_playlist_entry_count (m_playlist);
    int last_col = model->columnCount() - 1;
    auto make_range = [last_col](const QModelIndex & first, const QModelIndex & last)
    {
        // expand the range to cover all columns
        return QItemSelectionRange(first.sibling (first.row(), 0),
                                   last.sibling (last.row(), last_col));
    };

    QItemSelection ranges[2];
    QModelIndex first, last;
    bool prev = false;

    for (int row = rowsBefore; row < entries - rowsAfter; row++)
    {
        auto idx = rowToIndex (row);
        if (! idx.isValid ())
            continue;

        bool sel = aud_playlist_entry_get_selected (m_playlist, row);

        if (sel != prev && first.isValid ())
            ranges[prev] += make_range (first, last);

        if (sel != prev || ! first.isValid ())
            first = idx;

        last = idx;
        prev = sel;
    }

    if (first.isValid ())
        ranges[prev] += make_range(first, last);

    selected = std::move (ranges[true]);
    deselected = std::move (ranges[false]);
}

void PlaylistWidget::updateSelection(int rowsBefore, int rowsAfter)
{
    QItemSelection selected, deselected;
    getSelectedRanges (rowsBefore, rowsAfter, selected, deselected);

    auto sel = selectionModel ();
    auto old = sel->selection ();

    // Qt's selection model is complex, with two layers that our internal
    // playlist selection model doesn't mirror. To avoid interfering with
    // interactive selections, we need to avoid changing Qt's model unless
    // it's actually out of sync. So, we start by computing the difference
    // between the two models.
    auto diff = old;
    diff.merge (selected, sel->Select);
    diff.merge (deselected, sel->Deselect);
    diff.merge (old, sel->Toggle);

    if (!diff.isEmpty ())
    {
        // Toggle any cells that need to be toggled to bring the two
        // models in sync.
        sel->select (diff, sel->Toggle);

        // The prior call will have left any cells that were toggled
        // sitting in the interactive layer. Force Qt to finalize this
        // layer by making an empty selection, so that the next interactive
        // selection doesn't behave unexpectedly.
        sel->select (QModelIndex (), sel->Select);
    }

    auto focus = rowToIndex(aud_playlist_get_focus (m_playlist));
    if (sel->currentIndex().row () != focus.row ())
    {
        // The documentation for QAbstractItemView::setCurrentIndex says:
        //
        //     Unless the current selection mode is NoSelection, the
        //     item is also selected. Note that this function also updates
        //     the starting position for any new selections the user
        //     performs.
        //
        //     To set an item as the current item without selecting it,
        //     call:
        //
        //       selectionModel()->
        //         setCurrentIndex(index, QItemSelectionModel::NoUpdate);
        //
        // We need to update that starting position (see bug #981), so
        // selectionModel()->setCurrentIndex() isn't enough here, but
        // we don't want to change the selection. Temporarily changing
        // the selection mode to NoSelection accomplishes what we want.
        //
        setSelectionMode (NoSelection);
        setCurrentIndex (focus);
        setSelectionMode (ExtendedSelection);
    }
}

void PlaylistWidget::playlistUpdate ()
{
    auto update = aud_playlist_update_detail (m_playlist);

    if (update.level == Playlist::NoUpdate)
        return;

    inUpdate = true;

    int entries = aud_playlist_entry_count (m_playlist);
    int changed = entries - update.before - update.after;

    if (update.level == Playlist::Structure)
    {
        int old_entries = model->rowCount ();
        int removed = old_entries - update.before - update.after;

        if (currentPos >= old_entries - update.after)
            currentPos += entries - old_entries;
        else if (currentPos >= update.before)
            currentPos = -1;

        model->entriesRemoved (update.before, removed);
        model->entriesAdded (update.before, changed);
    }
    else if (update.level == Playlist::Metadata || update.queue_changed)
        model->entriesChanged (update.before, changed);

    if (update.queue_changed)
    {
        for (int i = aud_playlist_queue_count (m_playlist); i--;) //JWT:YEA THAT SEMICOLIN IS *REQUIRED!*
        {
            int entry = aud_playlist_queue_get_entry (m_playlist, i);
            if (entry < update.before || entry >= entries - update.after)
                model->entriesChanged (entry, 1);
        }
    }

    int pos = aud_playlist_get_position (m_playlist);

    if (pos != currentPos)
    {
        if (currentPos >= 0)
            model->entriesChanged (currentPos, 1);
        if (pos >= 0)
            model->entriesChanged (pos, 1);

        currentPos = pos;
    }

    updateSelection (update.before, update.after);

    inUpdate = false;
}

void PlaylistWidget::setFilter (const char * text)
{
    // Save the current focus before filtering
    int focus = aud_playlist_get_focus (m_playlist);

    // Empty the model before updating the filter.  This prevents Qt from
    // performing a series of "rows added" or "rows deleted" updates, which can
    // be very slow (worst case O(N^2) complexity) on a large playlist.
    model->entriesRemoved (0, model->rowCount ());

    // Update the filter
    proxyModel->setFilter (text);

    // Repopulate the model
    model->entriesAdded (0, aud_playlist_entry_count (m_playlist));

    // If the previously focused row is no longer visible with the new filter,
    // try to find a nearby one that is, and focus it.
    auto index = visibleIndexNear (focus);

    if (index.isValid ())
    {
        focus = indexToRow (index);
        aud_playlist_set_focus (m_playlist, focus);
        aud_playlist_select_all (m_playlist, false);
        aud_playlist_entry_set_selected (m_playlist, focus, true);
        scrollTo (index);
    }
}

void PlaylistWidget::setFirstVisibleColumn (int col)
{
    inUpdate = true;
    firstVisibleColumn = col;

    // make sure current and selected indexes point to a visible column
    updateSelection (0, 0);

    inUpdate = false;
}

void PlaylistWidget::moveFocus (int distance)
{
    int visibleRows = proxyModel->rowCount ();
    if (! visibleRows)
        return;

    int row = currentIndex ().row ();
    row = aud::clamp (row + distance, 0, visibleRows - 1);
    setCurrentIndex (proxyModel->index (row, 0));
}

void PlaylistWidget::showPopup ()
{
    audqt::infopopup_show (m_playlist, m_popup_pos);
}

void PlaylistWidget::triggerPopup (int pos)
{
    audqt::infopopup_hide ();

    m_popup_pos = pos;
    m_popup_timer.queue(aud_get_int(nullptr, "filepopup_delay") * 100,
                        [this]() { showPopup(); });
}

void PlaylistWidget::hidePopup ()
{
    audqt::infopopup_hide ();

    m_popup_pos = -1;
    m_popup_timer.stop ();
}

void PlaylistWidget::updateSettings ()
{
    setHeaderHidden (! aud_get_bool ("moonstone", "playlist_headers"));
}

}  // END MOONSTONE NAMESPACE
