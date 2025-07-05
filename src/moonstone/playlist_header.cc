/*
 * playlist_header.cc
 * Copyright 2017 John Lindgren and Eugene Paskevich
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

#include "playlist_header.h"
#include "playlist_model.h"
#include "playlist.h"
#include "settings.h"

#include <string.h>

#include <QAction>
#include <QContextMenuEvent>
#include <QMenu>

#include <libfauxdcore/audstrings.h>
#include <libfauxdcore/hook.h>
#include <libfauxdcore/i18n.h>
#include <libfauxdcore/runtime.h>
#include <libfauxdqt/libfauxdqt.h>

namespace Moonstone {

static const char * const s_col_keys[] = {
    "playing",
    "number",
    "title",
    "artist",
    "year",
    "album",
    "album-artist",
    "track",
    "genre",
    "queued",
    "length",
    "path",
    "filename",
    "custom",
    "bitrate",
    "comment",
    "publisher",
    "catalog-number",
    "disc"
};

static const int s_default_widths[] = {
    25,   // now playing
    25,   // entry number
    275,  // title
    175,  // artist
    50,   // year
    175,  // album
    175,  // album artist
    25,   // track
    100,  // genre
    25,   // queue position
    75,   // length
    275,  // path
    275,  // filename
    275,  // custom title
    75,   // bitrate
    275,  // comment
    175,  // publisher
    75,   // catalog number
    25    // disc
};

static const Playlist::SortType s_sort_types[] = {
    Playlist::n_sort_types,    // now playing
    Playlist::n_sort_types,    // entry number
    Playlist::Title,           // title
    Playlist::Artist,          // artist
    Playlist::Date,            // year
    Playlist::Album,           // album
    Playlist::AlbumArtist,     // album artist
    Playlist::Track,           // track
    Playlist::Genre,           // genre
    Playlist::n_sort_types,    // queue position
    Playlist::Length,          // length
    Playlist::Path,            // path
    Playlist::Filename,        // file name
    Playlist::FormattedTitle,  // custom title
    Playlist::Bitrate,         // bitrate
    Playlist::Comment,         // comment
    Playlist::Publisher,       // publisher
    Playlist::CatalogNum,      // catalog number
    Playlist::Disc            // disc
};

static_assert (aud::n_elems (s_col_keys) == PlaylistModel::n_cols, "update s_col_keys");
static_assert (aud::n_elems (s_default_widths) == PlaylistModel::n_cols, "update s_default_widths");
static_assert (aud::n_elems (s_sort_types) == PlaylistModel::n_cols, "update s_sort_types");

static Index<int> s_cols;
static int s_col_widths[PlaylistModel::n_cols];

static void loadConfig (bool force = false)
{
    static bool loaded = false;

    if (loaded && ! force)
        return;

    auto columns = str_list_to_index (aud_get_str ("moonstone", "playlist_columns"), " ");
    int n_columns = aud::min (columns.len (), (int) PlaylistModel::n_cols);

    s_cols.clear ();

    for (int c = 0; c < n_columns; c ++)
    {
        int i = 0;
        while (i < PlaylistModel::n_cols && strcmp (columns[c], s_col_keys[i]))
            i ++;

        if (i < PlaylistModel::n_cols)
            s_cols.append (i);
    }

    auto widths = str_list_to_index (aud_get_str ("moonstone", "column_widths"), ", ");
    int n_widths = aud::min (widths.len (), (int) PlaylistModel::n_cols);

    for (int i = 0; i < n_widths; i ++)
        s_col_widths[i] = audqt::to_native_dpi (str_to_int (widths[i]));
    for (int i = n_widths; i < PlaylistModel::n_cols; i ++)
        s_col_widths[i] = audqt::to_native_dpi (s_default_widths[i]);

    loaded = true;
}

static void saveConfig ()
{
    Index<String> index;
    for (int col : s_cols)
        index.append (String (s_col_keys[col]));

    int widths[PlaylistModel::n_cols];
    for (int i = 0; i < PlaylistModel::n_cols; i ++)
        widths[i] = audqt::to_portable_dpi (s_col_widths[i]);

    aud_set_str ("moonstone", "playlist_columns", index_to_str_list (index, " "));
    aud_set_str ("moonstone", "column_widths", int_array_to_str (widths, PlaylistModel::n_cols));
}

PlaylistHeader::PlaylistHeader (PlaylistWidget * playlist) :
    QHeaderView (Qt::Horizontal, playlist),
    m_playlist (playlist)
{
    loadConfig ();
    updateStyle ();

    setSectionsMovable (true);
    setStretchLastSection (true);

    connect (this, & QHeaderView::sectionClicked, this, & PlaylistHeader::sectionClicked);
    connect (this, & QHeaderView::sectionResized, this, & PlaylistHeader::sectionResized);
    connect (this, & QHeaderView::sectionMoved, this, & PlaylistHeader::sectionMoved);
    m_playlist_no = m_playlist->playlist ();
}

static void toggleColumn (int col, int logical_idx, bool on)
{
    int pos = s_cols.find (col);

    if (on)
    {
        if (pos >= 0)
            return;

/* JWT:CHGD. TO NEXT 3 LINES:        s_cols.append (col);         */
/*     (SO WE CAN INSERT NEW COLUMN NEXT TO ONE RIGHT-CLICKED ON) */
        int idx = s_cols.find (logical_idx - 1) + 1;
        s_cols.insert (idx, 1);
        s_cols[idx] = col;
    }
    else
    {
        if (pos < 0)
            return;

        s_cols.remove (pos, 1);
    }

    saveConfig ();

    // update all playlists
    hook_call ("moonstone update playlist columns", nullptr);
}

static void resetToDefaults ()
{
    aud_set_str ("moonstone", "playlist_columns", "playing artist title");
    aud_set_str ("moonstone", "column_widths", "");

    loadConfig (true);

    // update all playlists
    hook_call("moonstone update playlist columns", nullptr);
}

void PlaylistHeader::contextMenuEvent (QContextMenuEvent * event)
{
    auto menu = new QMenu (this);
    QAction * actions[PlaylistModel::n_cols];
    int logical_idx = logicalIndexAt (event->pos ());

    for (int col = 0; col < PlaylistModel::n_cols; col ++)
    {
        actions[col] = new QAction (_(PlaylistModel::labels[col]), menu);
        actions[col]->setCheckable (true);

        connect (actions[col], & QAction::toggled, [col, logical_idx] (bool on) {
            toggleColumn (col, logical_idx, on);
        });

        menu->addAction (actions[col]);
    }

    for (int col : s_cols)
        actions[col]->setChecked (true);

    auto sep = new QAction (menu);
    sep->setSeparator (true);
    menu->addAction (sep);

    auto reset = new QAction (_("Reset to Defaults"), menu);
    connect (reset, & QAction::triggered, resetToDefaults);
    menu->addAction (reset);

    menu->popup (event->globalPos ());
}

bool PlaylistHeader::event (QEvent * event)
{
    // Work around Qt 6 resetting column widths during StyleChange
    // (happens at least with 6.6.2, did not happen with Qt 5.x)
    m_inStyleChange = (event->type () == QEvent::StyleChange);

    bool ret = QHeaderView::event (event);

    if (m_inStyleChange)
    {
        updateColumns ();
        m_inStyleChange = false;
    }

    return ret;
}

void PlaylistHeader::updateColumns ()
{
    m_inUpdate = true;

    int n_shown = s_cols.len ();

    // Due to QTBUG-33974, column #0 cannot be moved by the user.
    // As a workaround, hide column #0 and start the real columns at #1.
    // However, Qt will hide the header completely if no columns are visible.
    // This is bad since the user can't right-click to add any columns again.
    // To prevent this, show column #0 if no real columns are visible.
    m_playlist->setColumnHidden (0, (n_shown > 0));

    bool shown[PlaylistModel::n_cols] {};

    for (int i = 0; i < n_shown; i++)
    {
        int col = s_cols[i];
        moveSection (visualIndex (1 + col), 1 + i);
        shown[col] = true;
    }

    // last column expands to fit, so size is not restored
    int last = (n_shown > 0) ? s_cols[n_shown - 1] : -1;

    for (int col = 0; col < PlaylistModel::n_cols; col++)
    {
        if (col != last)
            m_playlist->setColumnWidth (1 + col, s_col_widths[col]);

        m_playlist->setColumnHidden (1 + col, ! shown[col]);
    }

    // width of last column should be set to 0 initially,
    // but doing so repeatedly causes flicker
    if (last >= 0 && last != m_lastCol)
        m_playlist->setColumnWidth (1 + last, 0);

    // this should come after all setColumnHidden() calls
    m_playlist->setFirstVisibleColumn ((n_shown > 0) ? 1 + s_cols[0] : 0);

    m_inUpdate = false;
    m_lastCol = last;
}

void PlaylistHeader::updateStyle ()
{
    if (aud_get_bool("moonstone", "playlist_headers_bold"))
        setStyleSheet("QHeaderView { font-weight: bold; }");
    else
        setStyleSheet(nullptr);

    this->setSortIndicator (s_lastsortcol, s_lastsortorder);
}

/* JWT:CALLED BY OUR HOOK ADDED IN playlist-utils.cc:aud_playlist_sort_by_scheme()
   TO SET THE SORT INDICATOR ARROW WHEN USER SORTS PLAYLIST VIA THE MENU: */
void PlaylistHeader::set_sort_sndicator (Playlist::SortType * sort_type_data)
{
    if (this->m_playlist_no != aud_playlist_get_active ())
        return;

    auto sort_type = aud::from_ptr<Playlist::SortType> (sort_type_data);

    if (s_sortindicatorcol)  /* CLEAR ANY CURRENTLY-SET SORT-INDICATOR: */
        this->setSortIndicator (-1, Qt::AscendingOrder);

    int col = -1;
    for (int i = 0; i < PlaylistModel::n_cols; i++)
    {
        if (s_sort_types[i] == sort_type)
        {
            col = i + 1;
            if (col == s_sortedbycol)
            {
                aud_playlist_reverse (m_playlist->playlist ());
                s_sortindicatorcol = col;
                s_sortedbycol = -1;
                s_lastsortorder = Qt::AscendingOrder;
            }
            else
            {
                s_sortedbycol = col;
                s_lastsortorder = Qt::DescendingOrder;
                s_sortindicatorcol = s_sortedbycol;
            }
            this->setSortIndicator (col, s_lastsortorder);
            s_lastsortcol = col;
            aud_set_bool(nullptr, "_skip_playlist_reboot", true);
            return;
        }
    }
}

void PlaylistHeader::sectionClicked (int logicalIndex)
{
    int col = logicalIndex - 1;
    s_lastsortcol = -1;
    this->setSortIndicator (-1, Qt::AscendingOrder);

    if (col >= 2 && col < PlaylistModel::n_cols
            && s_sort_types[col] != Playlist::n_sort_types)
        aud_playlist_sort_by_scheme (m_playlist->playlist (), s_sort_types[col]);
}

void PlaylistHeader::sectionMoved (int logicalIndex, int oldVisualIndex, int newVisualIndex)
{
    if (m_inUpdate || m_inStyleChange)
        return;

    int old_pos = oldVisualIndex - 1;
    int new_pos = newVisualIndex - 1;

    if (old_pos < 0 || old_pos > s_cols.len () || new_pos < 0 || new_pos > s_cols.len ())
        return;

    int col = logicalIndex - 1;
    if (col != s_cols[old_pos])
        return;

    s_cols.remove (old_pos, 1);
    s_cols.insert (& col, new_pos, 1);

    saveConfig ();

    // update all the other playlists
    hook_call ("moonstone update playlist columns", nullptr);
}

void PlaylistHeader::sectionResized (int logicalIndex, int /*oldSize*/, int newSize)
{
    if (m_inUpdate || m_inStyleChange)
        return;

    int col = logicalIndex - 1;
    if (col < 0 || col >= PlaylistModel::n_cols)
        return;

    // last column expands to fit, so size is not saved
    int pos = s_cols.find (col);
    if (pos < 0 || pos == s_cols.len () - 1)
        return;

    s_col_widths[col] = newSize;

    saveConfig ();

    // update all the other playlists
    hook_call ("moonstone update playlist columns", nullptr);
}  // END MOONSTONE NAMESPACE

}
