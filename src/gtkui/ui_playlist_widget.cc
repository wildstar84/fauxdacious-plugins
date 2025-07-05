/*
 * ui_playlist_widget.c
 * Copyright 2011-2012 John Lindgren, William Pitcock, and Michał Lipski
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

#include <string.h>

#include <gtk/gtk.h>

#include <libfauxdcore/audstrings.h>
#include <libfauxdcore/i18n.h>
#include <libfauxdcore/mainloop.h>
#include <libfauxdcore/playlist.h>
#include <libfauxdcore/runtime.h>
#include <libfauxdcore/tuple.h>
#include <libfauxdcore/hook.h>
#include <libfauxdgui/libfauxdgui.h>
#include <libfauxdgui/libfauxdgui-gtk.h>
#include <libfauxdgui/list.h>

#include "gtkui.h"
#include "playlist_util.h"
#include "ui_playlist_widget.h"
#include "ui_playlist_notebook.h"

static const GType pw_col_types[PW_COLS] =
{
    G_TYPE_INT,     // entry number
    G_TYPE_STRING,  // title
    G_TYPE_STRING,  // artist
    G_TYPE_STRING,  // year
    G_TYPE_STRING,  // album
    G_TYPE_STRING,  // album artist
    G_TYPE_STRING,  // track
    G_TYPE_STRING,  // genre
    G_TYPE_STRING,  // queue position
    G_TYPE_STRING,  // length
    G_TYPE_STRING,  // path
    G_TYPE_STRING,  // file name
    G_TYPE_STRING,  // custom title
    G_TYPE_STRING,  // bitrate
    G_TYPE_STRING,  // comment
    G_TYPE_STRING,  // publisher
    G_TYPE_STRING,  // catalog number
    G_TYPE_STRING   // disc
};

static const int pw_col_min_widths[PW_COLS] = {
    3,   // entry number
    10,  // title
    10,  // artist
    4,   // year
    10,  // album
    10,  // album artist
    3,   // track
    10,  // genre
    3,   // queue position
    7,   // length
    10,  // path
    10,  // file name
    10,  // custom title
    3,   // bitrate
    10,  // comment,
    10,  // publisher
    3,   // catalog number
    2    // disc
};

static const Playlist::SortType pw_col_sort_types[PW_COLS] = {
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
    Playlist::Disc             // disc
};

struct PlaylistWidgetData
{
    int list;
    int popup_pos = -1;
    QueuedFunc popup_timer;
    GtkTreeViewColumn * s_sortedbycol = nullptr;
    GtkTreeViewColumn * s_sortindicatorcol = nullptr;

    void show_popup ()
    {
        GtkWindow * parent = get_main_window ();
        audgui_infopopup_show (parent, list, popup_pos);
    }
};

static void set_int_from_tuple (GValue * value, const Tuple & tuple, Tuple::Field field)
{
    int i = tuple.get_int (field);
    if (i > 0)
        g_value_take_string (value, g_strdup_printf ("%d", i));
    else
        g_value_set_string (value, "");
}

static void set_string_from_tuple (GValue * value, const Tuple & tuple, Tuple::Field field)
{
    g_value_set_string (value, tuple.get_str (field));
}

/* JWT:ADDED TO KEEP MULTI-LINE TITLES FROM GARBLING UP THE PLAYLIST ROWS.
   NOTE:WE DECIDED AGAINST CHANGING IT FOR THE "CUSTOM" TITLE FIELD.
*/
static void set_string_from_tuple_flattened (GValue * value, const Tuple & tuple, Tuple::Field field)
{
    String fieldval = tuple.get_str (field);
    if (fieldval && fieldval[0])  //JWT:NEEDED FOR OVERRUN IF ADD CD W/NO DISK IN GTK INTERFACE (SEGFAULT)?!
        g_value_set_string (value, (const char *) str_get_one_line (fieldval, true));
    else
        g_value_set_string (value, "");
}

static void set_queued (GValue * value, int list, int row)
{
    int q = aud_playlist_queue_find_entry (list, row);
    if (q < 0)
        g_value_set_string (value, "");
    else
        g_value_take_string (value, g_strdup_printf ("#%d", 1 + q));
}

static void set_length (GValue * value, const Tuple & tuple)
{
    int len = tuple.get_int (Tuple::Length);
    if (len >= 0)
        g_value_set_string (value, str_format_time (len));
    else
        g_value_set_string (value, "");
}

static void set_filename (GValue * value, const Tuple & tuple)
{
    String basename = tuple.get_str (Tuple::Basename);
    String suffix = tuple.get_str (Tuple::Suffix);

    g_value_set_string (value,
            (suffix && aud_get_bool ("gtkui", "filename_column_incl_ext"))
        ? str_concat ({basename ? basename : "", ".", suffix}) : basename);
}

static void get_value (void * user, int row, int column, GValue * value)
{
    PlaylistWidgetData * data = (PlaylistWidgetData *) user;
    g_return_if_fail (column >= 0 && column < pw_num_cols);
    g_return_if_fail (row >= 0 && row < aud_playlist_entry_count (data->list));

    column = pw_cols[column];

    Tuple tuple;

    if (column != PW_COL_NUMBER && column != PW_COL_QUEUED)
        tuple = aud_playlist_entry_get_tuple (data->list, row, Playlist::NoWait);

    switch (column)
    {
    case PW_COL_NUMBER:
        g_value_set_int (value, 1 + row);
        break;
    case PW_COL_TITLE:   // FLATTEN MULTILINE TITLES TO SINGLE, SPACE-SEPARATED LINE:
        set_string_from_tuple_flattened (value, tuple, Tuple::Title);
        break;
    case PW_COL_ARTIST:  // FLATTEN MULTILINE ARTISTS (MAY HAVE MULTIPLE ARTISTS, ONE PER LINE?):
        set_string_from_tuple_flattened (value, tuple, Tuple::Artist);
        break;
    case PW_COL_YEAR:
        set_int_from_tuple (value, tuple, Tuple::Year);
        break;
    case PW_COL_ALBUM:
        set_string_from_tuple (value, tuple, Tuple::Album);
        break;
    case PW_COL_ALBUM_ARTIST:
        set_string_from_tuple (value, tuple, Tuple::AlbumArtist);
        break;
    case PW_COL_TRACK:
        set_int_from_tuple (value, tuple, Tuple::Track);
        break;
    case PW_COL_GENRE:
        set_string_from_tuple (value, tuple, Tuple::Genre);
        break;
    case PW_COL_QUEUED:
        set_queued (value, data->list, row);
        break;
    case PW_COL_LENGTH:
        set_length (value, tuple);
        break;
    case PW_COL_FILENAME:
        set_filename (value, tuple);
        break;
    case PW_COL_PATH:
        set_string_from_tuple (value, tuple, Tuple::Path);
        break;
    case PW_COL_CUSTOM:
        set_string_from_tuple (value, tuple, Tuple::FormattedTitle);
        break;
    case PW_COL_BITRATE:
        set_int_from_tuple (value, tuple, Tuple::Bitrate);
        break;
    case PW_COL_COMMENT:
        set_string_from_tuple (value, tuple, Tuple::Comment);
        break;
    case PW_COL_PUBLISHER:
        set_string_from_tuple (value, tuple, Tuple::Publisher);
        break;
    case PW_COL_CATALOG_NUM:
        set_string_from_tuple (value, tuple, Tuple::CatalogNum);
        break;
    case PW_COL_DISC:
        set_int_from_tuple (value, tuple, Tuple::Disc);
        break;
    }
}

static bool get_selected (void * user, int row)
{
    return aud_playlist_entry_get_selected (((PlaylistWidgetData *) user)->list, row);
}

static void set_selected (void * user, int row, bool selected)
{
    aud_playlist_entry_set_selected (((PlaylistWidgetData *) user)->list, row, selected);
}

static void select_all (void * user, bool selected)
{
    aud_playlist_select_all (((PlaylistWidgetData *) user)->list, selected);
}

static void focus_change (void * user, int row)
{
    aud_playlist_set_focus (((PlaylistWidgetData *) user)->list, row);
}

static void activate_row (void * user, int row)
{
    int list = ((PlaylistWidgetData *) user)->list;
    aud_playlist_set_position (list, row);
    aud_playlist_play (list);
}

static void right_click (void * user, GdkEventButton * event)
{
    popup_menu_rclick ((const GdkEvent *) event);
}

static void shift_rows (void * user, int row, int before)
{
    int list = ((PlaylistWidgetData *) user)->list;

    /* Adjust the shift amount so that the selected entry closest to the
     * destination ends up at the destination. */
    if (before > row)
        before -= aud_playlist_selected_count (list, row, before - row);
    else
        before += aud_playlist_selected_count (list, before, row - before);

    aud_playlist_shift (list, row, before - row);
}

static void popup_hide (PlaylistWidgetData * data)
{
    audgui_infopopup_hide ();

    data->popup_pos = -1;
    data->popup_timer.stop ();
}

static void popup_trigger (PlaylistWidgetData * data, int pos)
{
    audgui_infopopup_hide ();

    data->popup_pos = pos;
    data->popup_timer.queue (aud_get_int (nullptr, "filepopup_delay") * 100,
            [data] () { data->show_popup (); });
}

static void mouse_motion (void * user, GdkEventMotion * event, int row)
{
    PlaylistWidgetData * data = (PlaylistWidgetData *) user;

    if (row < 0)
    {
        popup_hide (data);
        return;
    }

    if (aud_get_bool (nullptr, "show_filepopup_for_tuple") && data->popup_pos != row)
        popup_trigger (data, row);
}

static void mouse_leave (void * user, GdkEventMotion * event, int row)
{
    popup_hide ((PlaylistWidgetData *) user);
}

static Index<char> get_data (void * user)
{
    int playlist = ((PlaylistWidgetData *) user)->list;
    return audgui_urilist_create_from_selected (playlist);
}

// length is ignored; GtkSelectionData null-terminates the data for us
static void receive_data (void * user, int row, const char * data, int /*length*/)
{
    int playlist = ((PlaylistWidgetData *) user)->list;
    audgui_urilist_insert (playlist, row, data);
}

static const AudguiListCallbacks callbacks = {
    get_value,
    get_selected,
    set_selected,
    select_all,
    activate_row,
    right_click,
    shift_rows,
    "text/uri-list",
    get_data,
    receive_data,
    mouse_motion,
    mouse_leave,
    focus_change
};

/* JWT:CALLED BY OUR HOOK ADDED IN playlist-utils.cc:aud_playlist_sort_by_scheme()
   WHEN USER SORTS PLAYLIST VIA THE MENU: */
static void ui_playlist_set_sort_indicator (void * sort_type_data, GtkWidget * widget)
{
    auto sort_type = aud::from_ptr<Playlist::SortType> (sort_type_data);
    PlaylistWidgetData * data = (PlaylistWidgetData *) audgui_list_get_user (widget);

    if (data && data->list == aud_playlist_get_active ())
    {
        if (data->s_sortindicatorcol)  /* CLEAR ANY CURRENTLY-SET SORT-INDICATOR: */
            gtk_tree_view_column_set_sort_indicator (data->s_sortindicatorcol, false);

        for (int i = 0; i < pw_num_cols; i ++)
        {
            /* JWT:SEARCH EACH VISIBLE COLUMN FOR ONE MATCHING THE CORRESPONDING SORT CRITERIA: */
            int n = pw_cols[i];
            if (pw_col_sort_types[n] < Playlist::n_sort_types)
            {
                auto column = gtk_tree_view_get_column ((GtkTreeView *) widget, i);
                auto sort_type_ptr = g_object_get_data ((GObject *) column, "playlist-sort-type");
                auto column_sort_type = aud::from_ptr<Playlist::SortType> (sort_type_ptr);
                if (column_sort_type == sort_type)
                {
                    /* FOUND MATCHING COLUMN & IT'S VISIBLE, SO SET IT'S SORT INDICATOR: */
                    /* NOTE:  GTK SEEMS TO HAVE THE "GTK_SORT_*" SYMBOLS REVERSED!: */
                    if (column == data->s_sortedbycol)
                    {
                        /* WAS ASCENDING (UP-ARROW), SO REVERSE TO DESCENDING (DOWN-ARROW): */
                        aud_playlist_reverse (data->list);
                        gtk_tree_view_column_set_sort_indicator (data->s_sortedbycol, true);
                        gtk_tree_view_column_set_sort_order (data->s_sortedbycol, GTK_SORT_ASCENDING);
                        data->s_sortindicatorcol = data->s_sortedbycol;
                        data->s_sortedbycol = nullptr;
                    }
                    else  /* INITIAL SORT SORTS ASCENDING, (SO SET UP-ARROW): */
                    {
                        data->s_sortedbycol = column;
                        gtk_tree_view_column_set_sort_indicator (data->s_sortedbycol, true);
                        gtk_tree_view_column_set_sort_order (data->s_sortedbycol, GTK_SORT_DESCENDING);
                        data->s_sortindicatorcol = data->s_sortedbycol;
                    }
                    return;  /* WE'RE DONE, QUIT SEARCHING. */
                }
            }
        }
    }
}

static gboolean search_cb (GtkTreeModel * model, int column, const char * search,
 GtkTreeIter * iter, void * user)
{
    GtkTreePath * path = gtk_tree_model_get_path (model, iter);
    g_return_val_if_fail (path, true);
    int row = gtk_tree_path_get_indices (path)[0];
    g_return_val_if_fail (row >= 0, true);
    gtk_tree_path_free (path);

    Index<String> keys = str_list_to_index (search, " ");

    bool matched = false;

    if (keys.len ())
    {
        int list = ((PlaylistWidgetData *) user)->list;
        Tuple tuple = aud_playlist_entry_get_tuple (list, row);

        String strings[3] = {
            tuple.get_str (Tuple::Title),
            tuple.get_str (Tuple::Artist),
            tuple.get_str (Tuple::Album)
        };

        for (const String & s : strings)
        {
            if (! s)
                continue;

            auto is_match = [&] (const String & key)
                { return (bool) strstr_nocase_utf8 (s, key); };

            keys.remove_if (is_match);
        }

        matched = ! keys.len ();
    }

    return ! matched;
}

static void destroy_cb (PlaylistWidgetData * data, GtkWidget * Widget)
{
    hook_dissociate ("set playlist sort indicator", (HookFunction) ui_playlist_set_sort_indicator,
            Widget);
    delete data;
}

static void column_clicked_cb (GtkTreeViewColumn * column, PlaylistWidgetData * data)
{
    auto sort_type_ptr = g_object_get_data ((GObject *) column, "playlist-sort-type");
    auto sort_type = aud::from_ptr<Playlist::SortType> (sort_type_ptr);
    //gint colindex = gtk_tree_view_column_get_sort_column_id (column);

    aud_playlist_sort_by_scheme (data->list, sort_type);
}

GtkWidget * ui_playlist_widget_new (int playlist)
{
    PlaylistWidgetData * data = new PlaylistWidgetData;
    data->list = playlist;

    GtkWidget * list = audgui_list_new (& callbacks, data,
     aud_playlist_entry_count (playlist));

    gtk_tree_view_set_headers_visible ((GtkTreeView *) list,
     aud_get_bool ("gtkui", "playlist_headers"));
    gtk_tree_view_set_search_equal_func ((GtkTreeView *) list, search_cb, data,
     nullptr);
    g_signal_connect_swapped (list, "destroy", (GCallback) destroy_cb, data);

    /* Disable type-to-search because it blocks CTRL-V, causing URI's to be
     * pasted into the search box rather than added to the playlist.  The search
     * box can still be brought up with CTRL-F. */
    gtk_tree_view_set_enable_search ((GtkTreeView *) list, false);

    for (int i = 0; i < pw_num_cols; i ++)
    {
        int n = pw_cols[i];
        //audgui_list_add_column (list, pw_col_label[n] ? _(pw_col_names[n]) :
        // nullptr, i, pw_col_types[n], pw_col_min_widths[n]);
        audgui_list_add_column (list, _(pw_col_headers[n]), i, pw_col_types[n], pw_col_min_widths[n]);

        if (pw_col_sort_types[n] < Playlist::n_sort_types)
        {
            auto column = gtk_tree_view_get_column ((GtkTreeView *) list, i);
            auto sort_type_ptr = aud::to_ptr (pw_col_sort_types[n]);

            gtk_tree_view_column_set_clickable (column, true);
            g_object_set_data ((GObject *) column, "playlist-sort-type", sort_type_ptr);
            g_signal_connect (column, "clicked", (GCallback) column_clicked_cb, data);
        }
    }

    /* JWT:ADDED HOOK TO SET SORT-INDICATOR WHEN USER SORTS VIA THE PLAYLIST MENU: */
    hook_associate ("set playlist sort indicator", (HookFunction) ui_playlist_set_sort_indicator,
            list);

    return list;
}

int ui_playlist_widget_get_playlist (GtkWidget * widget)
{
    PlaylistWidgetData * data = (PlaylistWidgetData *) audgui_list_get_user (widget);
    g_return_val_if_fail (data, -1);
    return data->list;
}

void ui_playlist_widget_set_playlist (GtkWidget * widget, int list)
{
    PlaylistWidgetData * data = (PlaylistWidgetData *) audgui_list_get_user (widget);
    g_return_if_fail (data);
    data->list = list;
}

void ui_playlist_widget_update (GtkWidget * widget, const Playlist::Update & update)
{
    PlaylistWidgetData * data = (PlaylistWidgetData *) audgui_list_get_user (widget);
    g_return_if_fail (data);

    int entries = aud_playlist_entry_count (data->list);
    int changed = entries - update.before - update.after;

    if (update.level == Playlist::Structure)
    {
        int old_entries = audgui_list_row_count (widget);
        int removed = old_entries - update.before - update.after;

        audgui_list_delete_rows (widget, update.before, removed);
        audgui_list_insert_rows (widget, update.before, changed);

        /* scroll to end of playlist if entries were added there
           (but not if a newly added entry is playing) */
        if (entries > old_entries && ! update.after &&
         aud_playlist_get_focus (data->list) < old_entries)
            aud_playlist_set_focus (data->list, entries - 1);

        ui_playlist_widget_scroll (widget);
    }
    else if (update.level == Playlist::Metadata || update.queue_changed)
        audgui_list_update_rows (widget, update.before, changed);

    if (update.queue_changed)
    {
        for (int i = aud_playlist_queue_count (data->list); i --; )
        {
            int entry = aud_playlist_queue_get_entry (data->list, i);
            if (entry < update.before || entry >= entries - update.after)
                audgui_list_update_rows (widget, entry, 1);
        }
    }

    audgui_list_update_selection (widget, update.before, changed);
    audgui_list_set_focus (widget, aud_playlist_get_focus (data->list));
}

void ui_playlist_widget_scroll (GtkWidget * widget)
{
    PlaylistWidgetData * data = (PlaylistWidgetData *) audgui_list_get_user (widget);
    g_return_if_fail (data);

    int row = -1;

    if (gtk_widget_get_realized (widget))
    {
        int x, y;
        audgui_get_mouse_coords (widget, & x, & y);
        row = audgui_list_row_at_point (widget, x, y);
    }

    /* Only update the info popup if it is already shown or about to be shown;
     * this makes sure that it doesn't pop up when the Audacious window isn't
     * even visible. */
    if (row >= 0 && data->popup_pos >= 0)
        popup_trigger (data, row);
    else
        popup_hide (data);
}
