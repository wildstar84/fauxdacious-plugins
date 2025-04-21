/*
 * playback-history.cc
 * Copyright (C) 2023-2024 Igor Kushnir <igorkuo@gmail.com>
 * GTK version Copyright (c) 2025 Jim Turner <turnerjw784@yahoo.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License,
 * or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include <algorithm>
#include <cassert>
#include <utility>

#include <libfauxdcore/audstrings.h>
#include <libfauxdcore/hook.h>
#include <libfauxdcore/i18n.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <libfauxdcore/index.h>
#include <libfauxdcore/multihash.h>
#include <libfauxdcore/objects.h>
#include <libfauxdcore/playlist.h>
#include <libfauxdcore/plugin.h>
#include <libfauxdcore/preferences.h>
#include <libfauxdcore/runtime.h>

#include <libfauxdgui/gtk-compat.h>
#include <libfauxdgui/libfauxdgui.h>
#include <libfauxdgui/libfauxdgui-gtk.h>
#include <libfauxdgui/list.h>

#include <string.h>

static const char * printable (const String & str)
{
    // Printing nullptr invokes undefined behavior.
    return str ? str : "";
}

class PlaybackHistory : public GeneralPlugin
{
private:
    static constexpr const char * aboutText =
        N_("Playback History Plugin\n\n"
           "Copyright 2023-2024 Igor Kushnir <igorkuo@gmail.com>\n"
           "GTK version Copyright (c) 2025 Jim Turner <turnerjw784@yahoo.com>\n\n"
           "This plugin tracks and provides access to playback history.\n\n"

           "History entries are stored only in memory and are lost\n"
           "on Audacious exit. When the plugin is disabled,\n"
           "playback history is not tracked at all.\n"
           "History entries are only added, never removed automatically.\n"
           "The user can remove selected entries by pressing the Delete key.\n"
           "Restart Audacious or disable the plugin by closing\n"
           "Playback History view to clear the entries.\n\n"

           "Two history item granularities (modes) are supported.\n"
           "The user can select a mode in the plugin's settings.\n"
           "The Song mode is the default. Each played song is stored\n"
           "in history. Song titles are displayed in the list.\n"
           "When the Album mode is selected and multiple songs from\n"
           "a single album are played in a row, a single album entry\n"
           "is stored in history. Album names are displayed in the list.");

    static const char * const defaults[];
    static const PreferencesWidget widgets[];
    static const PluginPreferences prefs;

public:
    static constexpr PluginInfo info = {
        N_("Playback History"),
        PACKAGE,
        aboutText, // about
        &prefs,    // prefs
        PluginGLibOnly
    };

    constexpr PlaybackHistory () : GeneralPlugin (info, false) {}

    void * get_gtk_widget ();
};

EXPORT PlaybackHistory aud_plugin_instance;

static constexpr const char * configSection = "playback-history";
static constexpr const char * configEntryType = "entry_type";
static bool fromTitleChg = false;
static bool forceTitleFetch = false;
static int m_playingPosition = -1;
static int prevPlayingPosition = -1;

class HistoryEntry
{
public:
    enum class Type
    {
        Song = Tuple::Field::Title,
        Album = Tuple::Field::Album
    };
    static constexpr Type defaultType = Type::Song;
    String havetext;

    /**
     * Creates an invalid entry that can only be assigned to or destroyed.
     */
    HistoryEntry () = default;

    /**
     * Stores the currently playing entry in @c this->
     *
     * Call this function when a song playback starts.
     * @return @c true if the entry was retrieved and assigned successfully.
     * @note @c this remains or becomes invalid if @c false is returned.
     */
    bool assignPlayingEntry ();

    /**
     * Gives keyboard focus to the corresponding playlist entry and makes it the
     * single selected entry in the playlist.
     */
    void makeCurrent ();

    /**
     * Starts playing this entry.
     *
     * @return @c true in case of success.
     */
    bool play ();

    /**
     * Prints @c this using @c AUDDBG.
     *
     * @param prefix a possibly empty string to be printed before @c this->
     *               A nonempty @p prefix should end with whitespace.
     */
    void debugPrint (const char * prefix);

    Type type () { return m_type; }
    /**
     * Returns a translated human-readable designation of text() based on
     * type().
     */
    const char * translatedTextDesignation ();
    /**
     * Returns this entry's song title if type() is Song or its album name if
     * type() is Album.
     */
    const String & text () { return m_text; }
    int playlist () { return m_playlist; }
    String playlistTitle () { return aud_playlist_get_title (m_playlist); }
    /** Returns the playlist Entry Number of this entry. */
    int entryNumber ();

    /**
     * Retrieves the song title if type() is Song or the album name if type() is
     * Album at @a m_playlistPosition in @a m_playlist and assigns it to
     * @p text.
     *
     * @return @c true if the text was retrieved successfully.
     */
    bool retrieveText (String & text);

    bool shouldAppendEntry ();

private:
    /**
     * Returns an untranslated human-readable designation of text() based on
     * type().
     */
    const char * untranslatedTextDesignation ();

    /**
     * Returns @c true if @a m_playlist exists and @a m_playlistPosition still
     * points to the same playlist entry as at the time of last assignment.
     */
    bool isAvailable ();

    String m_text;
    int m_playlist; /**< the playlist, in which this entry was played */
    /** The position in @a m_playlist of the song titled text() if type() is
     * Song or of the first played song from the album named text() if type() is
     * Album.
     * When the user modifies the playlist, positions shift, but this should
     * happen rarely enough and therefore doesn't have to be handled perfectly.
     * A linear search for a song title or an album name equal to text() in
     * @a m_playlist is inefficient, and thus is never performed by this plugin.
     */
    int m_playlistPosition = -1;
    Type m_type = defaultType;
};

Index<HistoryEntry> m_entries;

/* ADDS NEW SONG ENTRIES INTO THE HISTORY-LIST: */
bool HistoryEntry::assignPlayingEntry ()
{
    m_playlist = aud_playlist_get_playing ();
    if (m_playlist < 0)
    {
        AUDWARN ("Playback just started but no playlist is playing.\n");
        return false;
    }

    m_playlistPosition = aud_playlist_get_position (m_playlist);
    if (m_playlistPosition == -1)
    {
        AUDWARN ("Playback just started but the playing playlist %s "
                "has no playing entry.\n",
                printable (playlistTitle ()));
        return false;
    }
    assert (m_playlistPosition >= 0);
    assert (m_playlistPosition < aud_playlist_entry_count (m_playlist));

    const auto entryType = aud_get_int (configSection, configEntryType);
    if (entryType == static_cast<int>(Type::Song) ||
            entryType == static_cast<int>(Type::Album))
        m_type = static_cast<Type>(entryType);
    else
    {
        AUDWARN ("Invalid %s.%s config value: %d.\n", configSection,
                configEntryType, entryType);
        m_type = defaultType;
    }

    return retrieveText (m_text);
}

/* HIGHLIGHTS THE SELECTED HISTORY-ENTRY IN THE CORRESPONDING PLAYLIST: */
void HistoryEntry::makeCurrent ()
{
    if (! isAvailable ())
        return;

    aud_playlist_select_all (m_playlist, false);
    aud_playlist_entry_set_selected (m_playlist, m_playlistPosition, true);
    aud_playlist_set_focus (m_playlist, m_playlistPosition);
    aud_playlist_set_active (m_playlist);
}

/* (RE)PLAYS THE MATCHING SONG WHEN USER DOUBLE-CLICKS ON A HISTORY ENTRY: */
bool HistoryEntry::play ()
{
    if (! isAvailable ())
        return false;

    aud_playlist_set_position (m_playlist, m_playlistPosition);
    aud_playlist_play (m_playlist);

    // Double-clicking a history entry makes it current just before activation.
    // In this case m_playlist is already active here. However, m_playlist is
    // not active here if the user performs the following steps:
    // 1) select a history entry; 2) activate another playlist;
    // 3) give focus to History view; 4) press the Enter key.
    aud_playlist_set_active (m_playlist);

    return true;
}

void HistoryEntry::debugPrint (const char * prefix)
{
    AUDDBG ("%s%s=\"%s\", playlist=\"%s\", entry number=%d\n", prefix,
           untranslatedTextDesignation (), printable (m_text),
           printable (playlistTitle ()), entryNumber ());
}

const char * HistoryEntry::translatedTextDesignation ()
{
    return (m_type == Type::Album) ? _("Album") : _("Title");
}

int HistoryEntry::entryNumber ()
{
    // Add 1 because a playlist position is 0-based
    // but an Entry Number in the UI is 1-based.
    return m_playlistPosition + 1;
}

const char * HistoryEntry::untranslatedTextDesignation ()
{
    return (m_type == Type::Album) ? "album" : "title";
}

bool HistoryEntry::retrieveText (String & text)
{
    if (! forceTitleFetch && havetext && havetext[0])
        text = havetext;
    else
    {
        String errorMessage;
        const auto tuple = aud_playlist_entry_get_tuple (m_playlist,
                m_playlistPosition, Playlist::NoWait);

        if (errorMessage || tuple.state () != Tuple::Valid)
        {
            AUDWARN ("Failed to retrieve metadata of entry #%d in playlist %s: %s\n",
                    entryNumber (), printable(playlistTitle ()),
                    errorMessage ? printable (errorMessage)
                                 : "Song info could not be read");
            return false;
        }

        text = tuple.get_str (static_cast<Tuple::Field>(m_type));
        if (! text || ! text[0])
            text = String (str_printf ("--no %s!--", untranslatedTextDesignation ()));

        if (! forceTitleFetch)
            havetext = text;
        else
            forceTitleFetch = false;  // ALWAYS A ONE-OFF!
    }
    return true;
}

/* RETURNS TRUE IF THE HISTORY ENTRY IS STILL IN IT'S PLAYLIST & SAME POSITION: */
bool HistoryEntry::isAvailable ()
{
    if (m_playlist < 0)
    {
        AUDWARN ("The selected entry's playlist has been deleted.\n");
        return false;
    }

    assert (m_playlistPosition >= 0);
    if (m_playlistPosition >= aud_playlist_entry_count (m_playlist))
    {
        AUDWARN ("The selected entry's position is now out of bounds.\n");
        return false;
    }

    forceTitleFetch = true;
    String currentTextAtPlaylistPosition;
    if (! retrieveText (currentTextAtPlaylistPosition))
        return false;

    // Text equality does not guarantee that the song, for which this history
    // entry was created, still resides at m_playlistPosition in m_playlist. In
    // case the user inserts or removes a few songs above m_playlistPosition:
    // * if type() is Song, a different song with the same title is unnoticed;
    // * if type() is Album, a different song from the same album or a song from
    //   an unrelated album that happens to have the same name goes undetected.
    // But such coincidences should be much more rare
    // and less of a problem than the text inequality condition
    // checked here. Therefore, information that uniquely identifies the
    // referenced song is not stored in a history entry just for this case.
    if (strcmp_safe ((const char *) currentTextAtPlaylistPosition,
            (const char *) m_text))
    {
        if (type () == HistoryEntry::Type::Song)
        {
            Tuple tuple = aud_playlist_entry_get_tuple (m_playlist,
                    m_playlistPosition, Playlist::NoWait);
            String album = tuple.get_str (Tuple::Album);
            if (! album || ! album[0])
                album = String ("--no album!--");

            if (! strcmp_safe ((const char *) album, (const char *) m_text))
                return true;
        }

        AUDWARN ("The %s at the selected entry's playlist position has"
                " changed.\n",
                untranslatedTextDesignation ());
        return false;
    }

    return true;
}

/* CHECKS WHETHER OR NOT TO ADD THE ENTRY TO THE HISTORY LIST: */
bool HistoryEntry::shouldAppendEntry ()
{
    if (m_playingPosition < 0 || fromTitleChg)
        return true;
    else if (m_playingPosition == prevPlayingPosition)
        return false;

    auto & prevPlayingEntry = m_entries[m_playingPosition];

    if (prevPlayingEntry.type () != this->type () ||
            prevPlayingEntry.playlist () != this->playlist ())
        return true; // the two entries are very different indeed

    // When the entry numbers differ, either the entries point to two
    // different songs or the user has modified the playlist and playlist
    // positions have shifted, which invalidated prevPlayingEntry. Either
    // way, the entry should be appended if the type is Song.
    if (this->type () == HistoryEntry::Type::Song &&
            prevPlayingEntry.entryNumber () != this->entryNumber ())
        return true;

    // If the type is Song, equal entry numbers but differing song titles
    // mean that the user has modified the playlist and playlist positions
    // have shifted. Append the entry in this case, because it is not the
    // same as the previous one.
    // If the type is Album, let us assume the Shuffle by Album playback
    // mode is enabled. Then equal album names, in all probability,
    // mean that a song from the same album started playing, in which case
    // the entry should not be appended. Much less likely, a different
    // album with the same name, separated by other albums from the
    // previously played one, was just randomly selected.
    // Ignore this possibility here. The users concerned about such an
    // occurrence are advised to edit metadata in their music collections
    // and ensure unique album names. The unique album names would also
    // prevent Audacious from erroneously playing same-name albums
    // in order if they happen to end up adjacent in a playlist.
    if (strcmp_safe ((const char *) prevPlayingEntry.text (),
            (const char *) this->text ()))
        return true;

    return false;
}

/* CALLED WHEN PLAYBACK STARTS: */
static void playbackStarted (void * data, void * list_)
{
    HistoryEntry entry;
    if (! entry.assignPlayingEntry ())
        return;

    GtkWidget * list = (GtkWidget *) list_;
    entry.debugPrint ("Started playing ");
    AUDDBG ("playing position=%d, entry count=%d\n", m_playingPosition,
           m_entries.len());

    if ((fromTitleChg && entry.type () == HistoryEntry::Type::Album) || ! entry.shouldAppendEntry ())
        return;

    prevPlayingPosition = m_playingPosition;

    // The last played entry appears at the top of the view. Therefore, the new
    // entry is inserted at row 0.
    // This code must be kept in sync with modelRowFromPosition().
    // Update m_playingPosition during the row insertion to avoid
    // updating the font for the new playing position separately below.
    m_playingPosition = m_entries.len ();
    m_entries.append (std::move (entry));
    audgui_list_insert_rows (list, 0, 1);
    audgui_list_set_highlight (list, 0);
    audgui_list_set_focus (list, 0);
}

/* CALLED WHEN TUPLE CHANGES (STREAMS CHANGE TITLE, ETC. WHILE PLAYING:  */
static void titleChanged (void * data, void * list_)
{
    if (aud_get_bool("playback-history", "chk_on_title_change"))
    {
        fromTitleChg = true;
        playbackStarted (data, list_);
        fromTitleChg = false;
    }
}

static void get_value (void * user, int row, int column, GValue * value)
{
    int rowindx = m_entries.len ()-(row+1);
    switch (column)
    {
        case 0:
        {
            String title;
            if (m_entries[rowindx].retrieveText(title))
                g_value_set_string (value, title);
            else
                g_value_set_string (value, "--error!--");
            break;
        }
    }
}

/* CALLBACK: UNUSED, BUT REQUIRED FOR set_selected() TO WORK: */
static bool get_selected (void * user, int row)
{
    return false;  // true causes new entries to be highlighted ("selected")!
}

/* CALLBACK WHEN USER SELECTES (HIGHLIGHTS) AN ENTRY: */
static void set_selected (void * user, int row, bool selected)
{
    if (selected)
        m_entries[m_entries.len ()-(row+1)].makeCurrent ();
}

/* CALLBACK: UNUSED, BUT REQUIRED FOR set_selected() TO WORK: */
static void select_all (void * user, bool selected) {}

/* CALLBACK WHEN USER DOUBLE-CLICKS AN ENTRY: */
static void activate_row (void * user, int row)
{
    prevPlayingPosition = m_playingPosition;
    aud_playlist_select_all (m_entries[m_entries.len ()-(row+1)].playlist (), false);
    m_entries[m_entries.len ()-(row+1)].play ();
    m_playingPosition = m_entries.len ()-(row+1);
}

/* CALLED WHEN USER DELETES ROWS VIA PRESSING [Delete] KEY: */
static void remove_selected (GtkWidget * treeview)
{
    GtkTreeModel * model;
    GtkTreeSelection * selection = gtk_tree_view_get_selection ((GtkTreeView *) treeview);
    GList * selected_list = gtk_tree_selection_get_selected_rows (selection, & model);
    GList * cursor = g_list_last (selected_list);
    while (cursor != NULL)
    {
        GtkTreeIter iter;
        GtkTreePath * path = (GtkTreePath *) cursor->data;
        gtk_tree_model_get_iter (model, &iter, path);
        int row = gtk_tree_path_get_indices (path)[0];
        audgui_list_delete_rows (treeview, row, 1);
        m_entries.remove (m_entries.len ()-(row+1), 1);
        cursor = cursor->prev;
    }
    g_list_free_full (selected_list, (GDestroyNotify) gtk_tree_path_free);
}

/* HANDLE SPECIAL KEY PRESSES (CURRENTLY ONLY [DELETE] KEY: */
static gboolean pbhist_keypress_cb (GtkWidget * treeview, GdkEventKey * event)
{
    switch (event->state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK | GDK_MOD1_MASK))
    {
    case 0:  // NO MODIFIER KEYS:
        switch (event->keyval)
        {
        case GDK_KEY_Delete:  // HANDLE [Delete] KEY TO REMOVE HISTORY ENTRIES:
            remove_selected (treeview);
            return true;
        }

        break;
    case GDK_CONTROL_MASK:  // Ctrl-KEYS:
        switch (event->keyval)
        {
        case 'a':
        {
            GtkTreeSelection * sel = gtk_tree_view_get_selection ((GtkTreeView *) treeview);
            for (int i = 0; i < m_entries.len(); i ++)
            {
                GtkTreeIter iter = {0, GINT_TO_POINTER (i)};
                gtk_tree_selection_select_iter (sel, & iter);
            }
            return true;
        }
        case 'c':
        {
            GtkTreeModel * model;
            GtkTreeSelection * selection = gtk_tree_view_get_selection ((GtkTreeView *) treeview);
            GList * selected_list = gtk_tree_selection_get_selected_rows (selection, & model);
            GList * cursor = g_list_last (selected_list);
            StringBuf pastem;
            while (cursor != NULL)
            {
                GtkTreeIter iter;
                GtkTreePath * path = (GtkTreePath *) cursor->data;
                gtk_tree_model_get_iter (model, &iter, path);
                int row = gtk_tree_path_get_indices (path)[0];
                pastem.insert (-1, (const char *) m_entries[row].text ());
                pastem.insert (-1, "\n");
                cursor = cursor->prev;
            }
            gtk_clipboard_clear (gtk_clipboard_get (GDK_SELECTION_PRIMARY));                                                                                  
            gtk_clipboard_set_text (gtk_clipboard_get (GDK_SELECTION_PRIMARY),
                    (const char *) pastem, strlen ((const char *) pastem));
            g_list_free_full (selected_list, (GDestroyNotify) gtk_tree_path_free);
            return true;
        }
        }
        break;
    }

    return false;
}

static const AudguiListCallbacks callbacks = {
    get_value,
    get_selected, set_selected, select_all,  // MUST HAVE ALL 3 FOR ANY TO WORK!
    activate_row
};

static void destroy_cb (GtkWidget * window)
{
    hook_dissociate ("playback ready", (HookFunction) playbackStarted);
    hook_dissociate ("title change", (HookFunction) titleChanged);
    m_entries.clear();
    m_playingPosition = -1;
    prevPlayingPosition = -1; 
}

/* CREATE ALL THE WIDGETS (ON STARTUP): */
void * PlaybackHistory::get_gtk_widget ()
{
    GtkWidget * treeview = audgui_list_new (& callbacks, nullptr, 0);
    gtk_tree_view_set_headers_visible ((GtkTreeView *) treeview, false);
    audgui_list_add_column (treeview, nullptr, 0, G_TYPE_STRING, -1);

    g_signal_connect (treeview, "key-press-event", (GCallback) pbhist_keypress_cb, nullptr);

    GtkWidget * scrollview = gtk_scrolled_window_new (nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type ((GtkScrolledWindow *) scrollview, GTK_SHADOW_IN);
    gtk_scrolled_window_set_policy ((GtkScrolledWindow *) scrollview, GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    GtkWidget * vbox = audgui_vbox_new (6);

    gtk_container_add ((GtkContainer *) scrollview, treeview);
    gtk_box_pack_start ((GtkBox *) vbox, scrollview, true, true, 0);

    gtk_widget_show_all (vbox);

    hook_associate ("playback ready", (HookFunction) playbackStarted, treeview);
    hook_associate ("title change", (HookFunction) titleChanged, treeview);

    g_signal_connect (vbox, "destroy", (GCallback) destroy_cb, nullptr);

    return vbox;
}

const char * const PlaybackHistory::defaults[] = {
    configEntryType,
    aud::numeric_string<static_cast<int>(HistoryEntry::defaultType)>::str,
    nullptr};

const PreferencesWidget PlaybackHistory::widgets[] = {
    WidgetLabel (N_("<b>History Item Granularity</b>")),
    WidgetRadio (N_("Song"), WidgetInt(configSection, configEntryType),
                {static_cast<int>(HistoryEntry::Type::Song)}),
    WidgetRadio (N_("Album"), WidgetInt(configSection, configEntryType),
                {static_cast<int>(HistoryEntry::Type::Album)}),
    // JWT:ALSO RECORD CHANGES WITHIN STREAMING RADIO-STATIONS:
    WidgetCheck (N_("Check station metadata changes."),
        WidgetBool ("playback-history", "chk_on_title_change"))};

const PluginPreferences PlaybackHistory::prefs = {{widgets}};
