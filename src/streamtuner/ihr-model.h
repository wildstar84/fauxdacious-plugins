// Copyright (c) 2019 Ariadne Conill <ariadne@dereferenced.org>
//
// Permission to use, copy, modify, and/or distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// This software is provided 'as is' and without any warranty, express or
// implied.  In no event shall the authors be liable for any damages arising
// from the use of this software.

#ifndef STREAMTUNER_IHR_MODEL_H
#define STREAMTUNER_IHR_MODEL_H

#include <libfauxdcore/drct.h>
#include <libfauxdcore/i18n.h>
#include <libfauxdcore/plugin.h>
#include <libfauxdcore/plugins.h>
#include <libfauxdcore/preferences.h>
#include <libfauxdcore/audstrings.h>
#include <libfauxdcore/hook.h>
#include <libfauxdcore/runtime.h>
#include <libfauxdcore/index.h>
#include <libfauxdcore/playlist.h>

#include <libfauxdqt/treeview.h>

#include <QWidget>
#include <QTabWidget>
#include <QAbstractListModel>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>

struct IHRMarketEntry {
    QString city;
    QString state;
    QString country_code;
    int station_count;
    int market_id;
};

struct IHRStationEntry {
    QString title;
    QString description;
    QString call_letters;
    QString stream_uri;
};

class IHRMarketModel : public QAbstractListModel {
public:
    IHRMarketModel (QObject * parent = nullptr);
    ~IHRMarketModel ();

    int columnCount (const QModelIndex &parent = QModelIndex()) const;
    int rowCount (const QModelIndex &parent = QModelIndex()) const;
    QVariant headerData (int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const;
    QVariant data (const QModelIndex &index, int role = Qt::DisplayRole) const;

    void fetch_markets ();

    int id_for_idx (const QModelIndex &index) const;

private:
    Index<IHRMarketEntry> m_results;
};

class IHRTunerModel : public QAbstractListModel {
public:
    IHRTunerModel (QObject * parent = nullptr);
    ~IHRTunerModel ();

    enum {
        CallLetters,
        Title,
        Description,
        NColumns
    };

    int columnCount (const QModelIndex &parent = QModelIndex()) const;
    int rowCount (const QModelIndex &parent = QModelIndex()) const;
    QVariant headerData (int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const;
    QVariant data (const QModelIndex &index, int role = Qt::DisplayRole) const;

    void fetch_stations (int market_id);

    const IHRStationEntry & station_for_idx (const QModelIndex &index) const;

private:
    Index<IHRStationEntry> m_results;
};

#endif
