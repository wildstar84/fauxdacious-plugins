/*
 * playlist_model.h
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

#ifndef PLAYLIST_MODEL_H
#define PLAYLIST_MODEL_H

#include <QAbstractListModel>
#include <QSortFilterProxyModel>

#include <libfauxdcore/index.h>
#include <libfauxdcore/objects.h>
#include <libfauxdcore/playlist.h>

class PlaylistModel : public QAbstractListModel
{
public:
    enum {
        NowPlaying,
        EntryNumber,
        Title,
        Artist,
        Year,
        Album,
        AlbumArtist,
        Track,
        Genre,
        QueuePos,
        Length,
        Path,
        Filename,
        CustomTitle,
        Bitrate,
        Comment,
        Publisher,
        CatalogNum,
        Disc,
        n_cols
    };

    static const char * const labels[];
    static const char * const header_labels[];

    PlaylistModel (QObject * parent, int playlist);

    int rowCount (const QModelIndex & parent = QModelIndex ()) const;
    int columnCount (const QModelIndex & parent = QModelIndex ()) const;
    QVariant data (const QModelIndex & index, int role = Qt::DisplayRole) const;
    QVariant headerData (int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const;
    Qt::ItemFlags flags (const QModelIndex & index) const;
    QStringList mimeTypes () const;
    QMimeData * mimeData (const QModelIndexList & indexes) const;
    bool dropMimeData (const QMimeData * data, Qt::DropAction action, int row,
     int column, const QModelIndex & parent);

    void entriesAdded (int row, int count);
    void entriesRemoved (int row, int count);
    void entriesChanged (int row, int count);
    void setFont (const QFont & font);

private:
    int m_playlist;
    int m_rows;

    QFont m_bold;
    QVariant alignment (int col) const;
    QString queuePos (int row) const;
    QString filename (const Tuple & tuple) const;
};

class PlaylistProxyModel : public QSortFilterProxyModel
{
public:
    PlaylistProxyModel (QObject * parent, int playlist) :
        QSortFilterProxyModel (parent),
        m_playlist (playlist) {}

    void setFilter (const char * filter);

private:
    bool filterAcceptsRow (int source_row, const QModelIndex &) const;

    int m_playlist;
    Index<String> m_searchTerms;
};

#endif
