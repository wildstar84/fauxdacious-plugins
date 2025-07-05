/*
 * playlist_model.cc
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

#include <QApplication>
#include <QIcon>
#include <QMimeData>
#include <QUrl>

#include <libfauxdcore/i18n.h>
#include <libfauxdcore/audstrings.h>
#include <libfauxdcore/drct.h>
#include <libfauxdcore/runtime.h>
#include <libfauxdqt/libfauxdqt.h>

#include "playlist_model.h"

#define ICON_SIZE 16

namespace Moonstone {

const char * const PlaylistModel::labels[] = {
    N_("Now Playing"),
    N_("Entry Number"),
    N_("Title"),
    N_("Artist"),
    N_("Year"),
    N_("Album"),
    N_("Album Artist"),
    N_("Track"),
    N_("Genre"),
    N_("Queue Position"),
    N_("Length"),
    N_("File Path"),
    N_("File Name"),
    N_("Custom Title"),
    N_("Bitrate"),
    N_("Comment"),
    N_("Publisher"),
    N_("Catalog Number"),
    N_("Disc")
};

const char * const PlaylistModel::header_labels[] = {  // JWT:MAKE FIT IN DEFAULT COL. WIDTH!
    N_("Now Playing"),
    N_("#"),
    N_("Title"),
    N_("Artist"),
    N_("Year"),
    N_("Album"),
    N_("Album Artist"),
    N_("Trk"),
    N_("Genre"),
    N_("Q#"),
    N_("Length"),
    N_("File Path"),
    N_("File Name"),
    N_("Custom Title"),
    N_("Bitrate"),
    N_("Comment"),
    N_("Publisher"),
    N_("Cat#"),
    N_("Dsk")
};

static const Tuple::Field s_fields[] = {
    Tuple::Invalid,
    Tuple::Invalid,
    Tuple::Title,
    Tuple::Artist,
    Tuple::Year,
    Tuple::Album,
    Tuple::AlbumArtist,
    Tuple::Track,
    Tuple::Genre,
    Tuple::Invalid,
    Tuple::Length,
    Tuple::Path,
    Tuple::Basename,
    Tuple::FormattedTitle,
    Tuple::Bitrate,
    Tuple::Comment,
    Tuple::Publisher,
    Tuple::CatalogNum,
    Tuple::Disc
};

static_assert (aud::n_elems (PlaylistModel::labels) == PlaylistModel::n_cols, "update PlaylistModel::labels");
static_assert (aud::n_elems (PlaylistModel::header_labels) == PlaylistModel::n_cols, "update PlaylistModel::header_labels");
static_assert (aud::n_elems (s_fields) == PlaylistModel::n_cols, "update s_fields");

static inline QPixmap get_icon (const char * name)
{
    return QIcon::fromTheme (name).pixmap (audqt::to_native_dpi (ICON_SIZE));
}

PlaylistModel::PlaylistModel (QObject * parent, int playlist) :
    QAbstractListModel (parent),
    m_playlist (playlist),
    m_rows (aud_playlist_entry_count (playlist))
{
}

int PlaylistModel::rowCount (const QModelIndex & parent) const
{
    return parent.isValid() ? 0 : m_rows;
}

int PlaylistModel::columnCount (const QModelIndex & parent) const
{
    return 1 + n_cols;
}

void PlaylistModel::setFont (const QFont & font)
{
    m_bold = font;
    m_bold.setBold (true);
}

QVariant PlaylistModel::alignment (int col) const
{
    switch (col)
    {
    case NowPlaying:
        return Qt::AlignCenter;
    case Length:
        return static_cast<Qt::Alignment::Int>(Qt::AlignRight | Qt::AlignVCenter);
    default:
        return static_cast<Qt::Alignment::Int>(Qt::AlignLeft | Qt::AlignVCenter);
    }
}

QVariant PlaylistModel::data (const QModelIndex &index, int role) const
{
    int col = index.column () - 1;
    if (col < 0 || col >= n_cols)
        return QVariant ();

    Tuple tuple;
    int val = -1;

    switch (role)
    {
    case Qt::DisplayRole:
        if (s_fields[col] != Tuple::Invalid)
        {
            tuple = aud_playlist_entry_get_tuple (m_playlist, index.row (), Playlist::NoWait);

            if (col == Filename)
                return filename (tuple);

            switch (tuple.get_value_type (s_fields[col]))
            {
            case Tuple::Empty:
                return QVariant ();
            case Tuple::String:
                if (col == Title)       // FLATTEN MULTILINE TITLES TO SINGLE, SPACE-SEPARATED LINE:
                    return QString ((const char *) str_get_one_line (tuple.get_str (Tuple::Title), true));
                else if (col == Artist) // FLATTEN MULTILINE ARTISTS (MAY HAVE MULTIPLE ARTISTS, ONE PER LINE?):
                    return QString ((const char *) str_get_one_line (tuple.get_str (Tuple::Artist), true));
                else
                    return QString (tuple.get_str (s_fields[col]));
            case Tuple::Int:
                val = tuple.get_int (s_fields[col]);
                break;
            }
        }

        switch (col)
        {
        case NowPlaying:
            return QVariant ();
        case EntryNumber:
            return QString::number (index.row () + 1);
        case QueuePos:
            return queuePos (index.row ());
        case Length:
            return QString (str_format_time (val));
        default:
            return QString::number (val);
        }

    case Qt::FontRole:
        if (index.row () == aud_playlist_get_position (m_playlist))
            return m_bold;
        break;

    case Qt::TextAlignmentRole:
        return alignment (col);

    case Qt::DecorationRole:
        if (col == NowPlaying && index.row () == aud_playlist_get_position (m_playlist))
        {
            if (aud_playlist_get_playing () == m_playlist)
                if (aud_drct_get_paused ())
                    return audqt::get_icon ("media-playback-pause");
                else
                    return audqt::get_icon ("media-playback-start");
            else
                return audqt::get_icon ("media-playback-stop");
        }
        else if (col == NowPlaying)
        {
            // Reserve space for the icon in other rows so that row
            // heights don't change at song change. Undocumented but
            // longstanding Qt behavior is that any isValid(),
            // non-isNull() variant in the DecorationRole will set the
            // QStyleOptionViewItem::HasDecoration feature, which will
            // generally cause the QStyle to reserve space for an icon.
            // See QStyledItemDelegate::initStyleOption() and
            // QCommonStylePrivate::viewItemSize().
            return QVariant(true);
        }
        break;
    }
    return QVariant ();
}

QVariant PlaylistModel::headerData (int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal)
        return QVariant ();

    int col = section - 1;
    if (col < 0 || col >= n_cols)
        return QVariant ();

    switch (role)
    {
    case Qt::DisplayRole:
        switch (col)
        {
            case NowPlaying:
                return QVariant ();
            default:
                return QString (_(header_labels[col])); // JWT:ABBREVIATED TO FIT
        }
    case Qt::ToolTipRole:
        return QString (_(labels[col])); // JWT:SHOW FULL HEADER IN TOOLTIP!
    case Qt::TextAlignmentRole:
        return alignment (col);
    case Qt::DecorationRole:
        if (col == NowPlaying)
            return audqt::get_icon ("media-playback-start");
    default:
        return QVariant ();
    }
}

Qt::DropActions PlaylistModel::supportedDropActions () const
{
    return Qt::CopyAction | Qt::MoveAction;
}

Qt::ItemFlags PlaylistModel::flags (const QModelIndex & index) const
{
    if (index.isValid ())
        return Qt::ItemIsSelectable | Qt::ItemIsDragEnabled | Qt::ItemIsEnabled;
    else
        return Qt::ItemIsSelectable | Qt::ItemIsDropEnabled | Qt::ItemIsEnabled;
}

QStringList PlaylistModel::mimeTypes () const
{
    return QStringList ("text/uri-list");
}

QMimeData * PlaylistModel::mimeData (const QModelIndexList & indexes) const
{
    /* we assume that <indexes> contains the selected entries */
    aud_playlist_cache_selected (m_playlist);

    QList<QUrl> urls;
    int prev = -1;

    for (auto & index : indexes)
    {
        int row = index.row ();
        if (row != prev)  /* skip multiple cells in same row */
        {
            urls.append (QString (aud_playlist_entry_get_filename (m_playlist, row)));
            prev = row;
        }
    }

    auto data = new QMimeData;
    data->setUrls (urls);
    return data;
}

bool PlaylistModel::dropMimeData (const QMimeData * data, Qt::DropAction action,
        int row, int column, const QModelIndex & parent)
{
    if (action != Qt::CopyAction || ! data->hasUrls ())
        return false;

    Index<PlaylistAddItem> items;
    for (auto & url : data->urls ())
        items.append (String (url.toEncoded ()));

    aud_playlist_entry_insert_batch (m_playlist, row, std::move (items), false);
    return true;
}

void PlaylistModel::entriesAdded (int row, int count)
{
    if (count < 1)
        return;

    int last = row + count - 1;
    beginInsertRows (QModelIndex (), row, last);
    m_rows += count;
    endInsertRows ();
}

void PlaylistModel::entriesRemoved (int row, int count)
{
    if (count < 1)
        return;

    int last = row + count - 1;
    beginRemoveRows (QModelIndex (), row, last);
    m_rows -= count;
    endRemoveRows ();
}

void PlaylistModel::entriesChanged (int row, int count)
{
    if (count < 1)
        return;

    int bottom = row + count - 1;
    auto topLeft = createIndex (row, 0);
    auto bottomRight = createIndex (bottom, columnCount () - 1);
    emit dataChanged (topLeft, bottomRight);
}

QString PlaylistModel::queuePos (int row) const
{
    int at = aud_playlist_queue_find_entry (m_playlist, row);
    if (at < 0)
        return QString ();
    else
        return QString ("#%1").arg (at + 1);
}

QString PlaylistModel::filename (const Tuple & tuple) const
{
    String basename = tuple.get_str (Tuple::Basename);
    String suffix = tuple.get_str (Tuple::Suffix);

    return (suffix && aud_get_bool ("qtui", "filename_column_incl_ext"))
        ? QString ("%1.%2").arg (
            static_cast<const char *>(basename),
            static_cast<const char *>(suffix))
        : QString (basename);
}

/* ---------------------------------- */

void PlaylistProxyModel::setFilter (const char * filter)
{
    m_searchTerms = str_list_to_index (filter, " ");
    invalidateFilter ();
}

bool PlaylistProxyModel::filterAcceptsRow (int source_row, const QModelIndex &) const
{
    if (! m_searchTerms.len ())
        return true;

    Tuple tuple = aud_playlist_entry_get_tuple (m_playlist, source_row, Playlist::NoWait);

    String strings[] = {
        tuple.get_str (Tuple::Title),
        tuple.get_str (Tuple::Artist),
        tuple.get_str (Tuple::Album),
        tuple.get_str (Tuple::Basename)
    };

    for (auto & term : m_searchTerms)
    {
        bool found = false;

        for (auto & s : strings)
        {
            if (s && strstr_nocase_utf8 (s, term))
            {
                found = true;
                break;
            }
        }

        if (! found)
            return false;
    }

    return true;
}

}  // END MOONSTONE NAMESPACE
