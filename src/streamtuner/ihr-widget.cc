// Copyright (c) 2019 Ariadne Conill <ariadne@dereferenced.org>
//
// Permission to use, copy, modify, and/or distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// This software is provided 'as is' and without any warranty, express or
// implied.  In no event shall the authors be liable for any damages arising
// from the use of this software.

#include <sys/stat.h>
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
#include <libfauxdcore/vfs_async.h>
#include <libfauxdcore/probe.h>
#include <libfauxdcore/interface.h>

#include "ihr-model.h"
#include "ihr-widget.h"

static IHRStationEntry entry;
static Tuple tuple;

IHRListingWidget::IHRListingWidget (QWidget * parent) :
    audqt::TreeView (parent)
{
    m_model = new IHRTunerModel (this);

    setModel (m_model);
    setRootIsDecorated (false);
}

void IHRListingWidget::activate (const QModelIndex & index)
{
    tuple = Tuple ();

    if (index.row () < 0)
        return;

    entry = m_model->station_for_idx (index);
    if (entry.stream_uri.isNull() || entry.stream_uri.isEmpty() || entry.stream_uri.length () <= 0)
    {
        aud_ui_show_error ("Station has no valid streams, sorry.");
        return;
    }

    /* JWT:MUST TURN OFF URL-HELPER FOR LOADING STREAMTUNER STREAMS!: */
    aud_set_bool ("audacious", "_url_helper_denythistime", true);

    AUDINFO ("Play radio entry %s [%s].\n", (const char *) entry.title.toLocal8Bit (), (const char *) entry.stream_uri.toLocal8Bit ());

    int playlist = aud_playlist_get_active ();
    /* JWT:TRY TO FETCH THE LOGO FILE: */
    if (aud_get_bool (nullptr, "user_tag_data") && ! entry.logo.isNull() && ! entry.logo.isEmpty()
            && entry.logo.length () > 0)
    {
        struct stat statbuf;
        String local_imagefid = String (str_concat ({aud_get_path (AudPath::UserDir), "/",
                (const char *) entry.call_letters.toLocal8Bit().constData()}));
        if (stat ((const char *) local_imagefid, & statbuf))
        {
            vfs_async_file_get_contents (entry.logo.toLocal8Bit().constData(), [&] (const char *, const Index<char> & buf) {
                if (buf.len () > 0)
                {
                    String local_imageuri = String (str_concat ({"file://", aud_get_path (AudPath::UserDir),
                            "/", (const char *) entry.call_letters.toLocal8Bit().constData()}));
                    VFSFile file ((const char *) local_imageuri, "w");
                    if (file)
                    {
                        int sz = buf.len ();
                        if (file.fwrite (buf.begin (), 1, sz) == sz)
                        {
                            AUDINFO ("i:Successfully saved %d bytes of logo image locally to (%s).\n", sz,
                                    (const char *) local_imageuri);
                            tuple.set_str (Tuple::Comment, local_imageuri);
                            aud_write_tag_to_tagfile (entry.stream_uri.toLocal8Bit().constData(), tuple,
                                    "user_tag_data");
                        }
                    }
                }
            });
        }
        else
        {
            String local_imageuri = String (str_concat ({"file://", aud_get_path (AudPath::UserDir),
                    "/", (const char *) entry.call_letters.toLocal8Bit().constData()}));
            tuple.set_str (Tuple::Comment, local_imageuri);
        }
    }

    /* JWT:NOTE:  THIS HAPPENS *BEFORE* THE ABOVE (vfs_async) CODE FINISHES! */
    if (aud_get_bool (nullptr, "user_tag_data"))
    {
        if (! entry.title.isNull() && ! entry.title.isEmpty() && entry.title.length () > 0)
            tuple.set_str (Tuple::Title, entry.title.toLocal8Bit().constData());
        if (! entry.genre.isNull() && ! entry.genre.isEmpty() && entry.genre.length () > 0)
            tuple.set_str (Tuple::Genre, entry.genre.toLocal8Bit().constData());

        /* JWT:WE WRITE TO TAGFILE HERE JUST IN CASE NO LOGO FOUND: */
        aud_write_tag_to_tagfile (entry.stream_uri.toLocal8Bit().constData(), tuple, "user_tag_data");
    }
    //x aud_playlist_entry_insert (playlist, -1, entry.stream_uri.toUtf8 (), Tuple (), false);
    aud_playlist_entry_insert (playlist, -1, entry.stream_uri.toLocal8Bit().constData(), Tuple (), false);
}

IHRMarketWidget::IHRMarketWidget (QWidget * parent) :
    QTreeView (parent)
{
    m_model = new IHRMarketModel (this);

    setModel (m_model);
    setRootIsDecorated (false);
}

IHRTunerWidget::IHRTunerWidget (QWidget * parent) :
    QWidget (parent)
{
    m_layout = new QVBoxLayout (this);

    m_splitter = new QSplitter ();

    m_markets = new IHRMarketWidget ();
    m_splitter->addWidget (m_markets);

    m_tuner = new IHRListingWidget ();
    m_splitter->addWidget (m_tuner);
    m_splitter->setStretchFactor (1, 2);

    m_layout->addWidget (m_splitter);

    auto market_selection_model = m_markets->selectionModel ();
    connect(market_selection_model, &QItemSelectionModel::selectionChanged, [&] (const QItemSelection &selected, const QItemSelection &) {
        // this should never happen, but just to be sure...
        if (! selected.indexes ().length ())
            return;

        auto idx = selected.indexes ().first ();
        IHRMarketModel *market = (IHRMarketModel *) m_markets->model ();

        IHRTunerModel *model = (IHRTunerModel *) m_tuner->model ();
        model->fetch_stations (market->id_for_idx (idx));
    });
}
