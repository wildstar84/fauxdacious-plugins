/*
 * Audacious - a cross-platform multimedia player
 * Copyright (c) 2007 Tomasz Moń
 * Copyright (c) 2008 William Pitcock
 * Copyright (c) 2009-2011 John Lindgren
 *
 * Based on:
 * BMP - Cross-platform multimedia player
 * Copyright (C) 2003-2004  BMP development team.
 *
 * XMMS:
 * Copyright (C) 1998-2003  XMMS development team.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; under version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses>.
 *
 * The Audacious team does not consider modular code linking to
 * Audacious or using our public API to be a derived work.
 */

#include "menus.h"
#include "skins_cfg.h"
#include "skin.h"
#include "view.h"
#include "playlist-widget.h"
#include "playlist-slider.h"

#include "../ui-common/qt-compat.h"
#include "../ui-common/menu-ops.h"
#include "actions-mainwin.h"
#include "actions-playlist.h"

#include <libfauxdcore/audstrings.h>
#include <libfauxdcore/drct.h>
#include <libfauxdcore/hook.h>
#include <libfauxdcore/i18n.h>
#include <libfauxdcore/runtime.h>
#include <libfauxdcore/playlist.h>
#include <libfauxdcore/interface.h>
#include <libfauxdqt/libfauxdqt.h>

#include <QMimeData>

enum {
    DRAG_SELECT = 1,
    DRAG_MOVE
};

void PlaylistWidget::update_title ()
{
    if (aud_playlist_count () > 1)
    {
        String title = aud_playlist_get_title (m_playlist);
        m_title_text = String (str_printf (_("%s (%d of %d)"),
         (const char *) title, 1 + m_playlist, aud_playlist_count ()));
    }
    else
        m_title_text = String ();
}

void PlaylistWidget::calc_layout ()
{
    m_rows = m_height / m_row_height;

    if (m_rows && m_title_text)
    {
        m_offset = m_row_height;
        m_rows --;
    }
    else
        m_offset = 0;

    if (m_first + m_rows > m_length)
        m_first = m_length - m_rows;
    if (m_first < 0)
        m_first = 0;
}

int PlaylistWidget::calc_position (int y) const
{
    if (y < m_offset)
        return -1;

    int position = m_first + (y - m_offset) / m_row_height;
    if (position >= m_first + m_rows || position >= m_length)
        return m_length;

    return position;
}

int PlaylistWidget::adjust_position (bool relative, int position) const
{
    if (m_length == 0)
        return -1;

    if (relative)
    {
        int focus = aud_playlist_get_focus (m_playlist);
        if (focus == -1)
            return 0;

        position += focus;
    }

    if (position < 0)
        return 0;
    if (position >= m_length)
        return m_length - 1;

    return position;
}

void PlaylistWidget::cancel_all ()
{
    m_drag = false;

    if (m_scroll)
    {
        m_scroll = 0;
        scroll_timer.stop ();
    }

    if (m_hover != -1)
    {
        m_hover = -1;
        queue_draw ();
    }

    popup_hide ();
}

void PlaylistWidget::draw (QPainter & cr)
{
    int active_entry = aud_playlist_get_position (m_playlist);
    int left = 3, right = 3;
    int width;
    QRect rect;

    cr.setFont (* m_font);

    /* background */

    cr.fillRect (cr.window (), QColor (skin.colors[SKIN_PLEDIT_NORMALBG]));

    /* playlist title */

    if (m_offset)
    {
        cr.setPen (QColor (skin.colors[SKIN_PLEDIT_NORMAL]));
        cr.drawText (left, 0, m_width - left - right, m_row_height,
         Qt::AlignCenter, (const char *) m_title_text);
    }

    /* selection highlight */

    for (int i = m_first; i < m_first + m_rows && i < m_length; i ++)
    {
        if (aud_playlist_entry_get_selected (m_playlist, i))
            cr.fillRect (0, m_offset + m_row_height * (i - m_first), m_width,
             m_row_height, QColor (skin.colors[SKIN_PLEDIT_SELECTEDBG]));
    }

    /* entry numbers */

    if (aud_get_bool (nullptr, "show_numbers_in_pl"))
    {
        width = 0;

        for (int i = m_first; i < m_first + m_rows && i < m_length; i ++)
        {
            char buf[16];
            snprintf (buf, sizeof buf, "%d.", 1 + i);

            cr.setPen (QColor (skin.colors[(i == active_entry) ?
             SKIN_PLEDIT_CURRENT : SKIN_PLEDIT_NORMAL]));
            cr.drawText (left, m_offset + m_row_height * (i - m_first),
             m_width - left - right, m_row_height,
             Qt::AlignLeft | Qt::AlignVCenter, buf, & rect);

            width = aud::max (width, rect.width ());
        }

        left += width + 4;
    }

    /* entry lengths */

    width = 0;

    for (int i = m_first; i < m_first + m_rows && i < m_length; i ++)
    {
        Tuple tuple = aud_playlist_entry_get_tuple (m_playlist, i, Playlist::NoWait);
        int len = tuple.get_int (Tuple::Length);
        if (len < 0)
            continue;

        cr.setPen (QColor (skin.colors[(i == active_entry) ?
         SKIN_PLEDIT_CURRENT : SKIN_PLEDIT_NORMAL]));
        cr.drawText (left, m_offset + m_row_height * (i - m_first),
         m_width - left - right, m_row_height,
         Qt::AlignRight | Qt::AlignVCenter,
         (const char *) str_format_time (len), & rect);

        width = aud::max (width, rect.width ());
    }

    right += width + 6;

    /* queue positions */

    if (aud_playlist_queue_count (m_playlist))
    {
        width = 0;

        for (int i = m_first; i < m_first + m_rows && i < m_length; i ++)
        {
            int pos = aud_playlist_queue_find_entry (m_playlist, i);
            if (pos < 0)
                continue;

            char buf[16];
            snprintf (buf, sizeof buf, "(#%d)", 1 + pos);

            cr.setPen (QColor (skin.colors[(i == active_entry) ?
             SKIN_PLEDIT_CURRENT : SKIN_PLEDIT_NORMAL]));
            cr.drawText (left, m_offset + m_row_height * (i - m_first),
             m_width - left - right, m_row_height,
             Qt::AlignRight | Qt::AlignVCenter, buf, & rect);

            width = aud::max (width, rect.width ());
        }

        right += width + 6;
    }

    /* titles */

    for (int i = m_first; i < m_first + m_rows && i < m_length; i ++)
    {
        Tuple tuple = aud_playlist_entry_get_tuple (m_playlist, i, Playlist::NoWait);
        String title = tuple.get_str (Tuple::FormattedTitle);

        cr.setPen (QColor (skin.colors[(i == active_entry) ?
         SKIN_PLEDIT_CURRENT : SKIN_PLEDIT_NORMAL]));
        cr.drawText (left, m_offset + m_row_height * (i - m_first),
         m_width - left - right, m_row_height,
         Qt::AlignLeft | Qt::AlignVCenter, (const char *) str_get_one_line (title, true));
    }

    /* focus rectangle */

    int focus = aud_playlist_get_focus (m_playlist);

    /* don't show rectangle if this is the only selected entry */
    if (focus >= m_first && focus <= m_first + m_rows - 1 &&
     (! aud_playlist_entry_get_selected (m_playlist, focus) ||
     aud_playlist_selected_count (m_playlist) > 1))
    {
        cr.setPen (QColor (skin.colors[SKIN_PLEDIT_NORMAL]));
        cr.drawRect (0, m_offset + m_row_height * (focus - m_first), m_width - 1, m_row_height - 1);
    }

    /* hover line */

    if (m_hover >= m_first && m_hover <= m_first + m_rows)
    {
        cr.fillRect (0, m_offset + m_row_height * (m_hover - m_first) - 1, m_width, 2,
                QColor (skin.colors[SKIN_PLEDIT_NORMAL]));
    }
}

PlaylistWidget::PlaylistWidget (int width, int height, const char * font) :
    m_width (width * config.scale),
    m_height (height * config.scale)
{
    add_input (m_width, m_height, true, true);
    set_font (font);  /* calls refresh() */

    setAcceptDrops (true);
}

void PlaylistWidget::resize (int width, int height)
{
    m_width = width * config.scale;
    m_height = height * config.scale;

    Widget::resize (m_width, m_height);
    refresh ();
}

void PlaylistWidget::set_font (const char * font)
{
    m_font.capture (new QFont (audqt::qfont_from_string (font)));
    m_metrics.capture (new QFontMetrics (* m_font, this));
    m_row_height = m_metrics->height ();
    refresh ();
}

void PlaylistWidget::refresh ()
{
    m_playlist = aud_playlist_get_active ();
    m_length = aud_playlist_entry_count (m_playlist);
    update_title ();
    calc_layout ();

    int id = aud_playlist_get_unique_id (m_playlist);
    if (m_playlist_id != id)
    {
        cancel_all ();
        m_playlist_id = id;
        m_first = 0;
        ensure_visible (aud_playlist_get_focus (m_playlist));
    }

    queue_draw ();

    if (m_slider)
        m_slider->refresh ();
}

void PlaylistWidget::ensure_visible (int position)
{
    if (position < m_first || position >= m_first + m_rows)
        m_first = position - m_rows / 2;

    calc_layout ();
}

void PlaylistWidget::select_single (bool relative, int position)
{
    position = adjust_position (relative, position);

    if (position == -1)
        return;

    aud_playlist_select_all (m_playlist, false);
    aud_playlist_entry_set_selected (m_playlist, position, true);
    aud_playlist_set_focus (m_playlist, position);
    ensure_visible (position);
}

void PlaylistWidget::select_extend (bool relative, int position)
{
    position = adjust_position (relative, position);

    if (position == -1)
        return;

    int count = adjust_position (true, 0);
    int sign = (position > count) ? 1 : -1;

    for (; count != position; count += sign)
        aud_playlist_entry_set_selected (m_playlist, count,
         ! aud_playlist_entry_get_selected (m_playlist, count + sign));

    aud_playlist_entry_set_selected (m_playlist, position, true);
    aud_playlist_set_focus (m_playlist, position);
    ensure_visible (position);
}

void PlaylistWidget::select_slide (bool relative, int position)
{
    position = adjust_position (relative, position);

    if (position == -1)
        return;

    aud_playlist_set_focus (m_playlist, position);
    ensure_visible (position);
}

void PlaylistWidget::select_toggle (bool relative, int position)
{
    position = adjust_position (relative, position);

    if (position == -1)
        return;

    aud_playlist_entry_set_selected (m_playlist, position,
     ! aud_playlist_entry_get_selected (m_playlist, position));
    aud_playlist_set_focus (m_playlist, position);
    ensure_visible (position);
}

void PlaylistWidget::select_move (bool relative, int position)
{
    int focus = aud_playlist_get_focus (m_playlist);
    position = adjust_position (relative, position);

    if (focus == -1 || position == -1 || position == focus)
        return;

    focus += aud_playlist_shift (m_playlist, focus, position - focus);
    ensure_visible (focus);
}

void PlaylistWidget::delete_selected ()
{
    aud_playlist_delete_selected (m_playlist);

    m_length = aud_playlist_entry_count (m_playlist);
    int focus = aud_playlist_get_focus (m_playlist);

    if (focus != -1)
    {
        aud_playlist_entry_set_selected (m_playlist, focus, true);
        ensure_visible (focus);
    }
}

bool PlaylistWidget::handle_keypress (QKeyEvent * event)
{
    cancel_all ();

    switch (event->modifiers () & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier))
    {
      /* KEYS HANDLED HERE BY PLAYLIST-WINDOW REGARDLESS OF FOCUS -
         (UNLESS ALREADY HANDLED BY MAIN WINDOW WHEN IT'S FOCUSED)!:
      */
      case 0:
        switch (event->key ())
        {
          case Qt::Key_Up:
            select_single (true, -1);
            break;
          case Qt::Key_Down:
            select_single (true, 1);
            break;
          case Qt::Key_PageUp:
            select_single (true, -m_rows);
            break;
          case Qt::Key_PageDown:
            select_single (true, m_rows);
            break;
          case Qt::Key_Home:
            select_single (false, 0);
            break;
          case Qt::Key_End:
            select_single (false, m_length - 1);
            break;
          case Qt::Key_Return:
            select_single (true, 0);
            aud_playlist_set_position (m_playlist, aud_playlist_get_focus (m_playlist));
            aud_playlist_play (m_playlist);
            break;
          case Qt::Key_Escape:
            select_single (false, aud_playlist_get_position (m_playlist));
            break;
          case Qt::Key_Delete:
            delete_selected ();
            break;
          case Qt::Key_Tab:
            pl_next ();
            break;
          /* JWT:MUST ADD THESE MENU ITEMS MANUALLY HERE (GTK BINDS 'EM AUTOMATICALLY) */
          /* DUE TO "Qt::WidgetShortcut" ADDED BY MAINS COMMIT# c9403ee (APRIL, 2025): */
          case Qt::Key_B:
            aud_drct_pl_next ();
            break;
          case Qt::Key_C:
            aud_drct_pause ();
            break;
          case Qt::Key_F:
            action_playlist_add_files ();
            break;
          case Qt::Key_I:
            audqt::infowin_show_current ();
            break;
          case Qt::Key_J:
            aud_ui_show_jump_to_song ();
            break;
          case Qt::Key_L:
            action_play_file ();
            break;
          case Qt::Key_O:
            audqt::fileopener_show (audqt::FileMode::ImportPlaylist);
            break;
          case Qt::Key_P:
            action_playlist_manager ();
            break;
          case Qt::Key_U:
            action_playlist_add_url ();
            break;
          case Qt::Key_V:
            aud_drct_stop ();
            break;
          case Qt::Key_X:
            aud_drct_play ();
            break;
          case Qt::Key_Y:
            action_search_tool ();
            break;
          case Qt::Key_Z:
            aud_drct_pl_prev ();
            break;
          default:
            return false;
        }
        break;
      case Qt::ShiftModifier:
        switch (event->key ())
        {
          case Qt::Key_Up:
            select_extend (true, -1);
            break;
          case Qt::Key_Down:
            select_extend (true, 1);
            break;
          case Qt::Key_PageUp:
            select_extend (true, -m_rows);
            break;
          case Qt::Key_PageDown:
            select_extend (true, m_rows);
            break;
          case Qt::Key_Home:
            select_extend (false, 0);
            break;
          case Qt::Key_End:
            select_extend (false, m_length - 1);
            break;
          case Qt::Key_Backtab: /* JWT:NOTE:THIS == "[Shift+[Tab]]"!!!: */
            pl_prev ();
            break;
          /* JWT:MUST ADD THESE MENU ITEMS MANUALLY HERE (GTK BINDS 'EM AUTOMATICALLY)!: */
          case Qt::Key_Return:
            pl_play ();
            break;
          case Qt::Key_D:
            /* action_playlist_delete (); */
            audqt::playlist_confirm_delete (aud_playlist_get_active ());
            break;
          case Qt::Key_F:
            action_playlist_add_folder ();
            break;
          case Qt::Key_L:
            action_play_folder ();
            break;
          case Qt::Key_Q:
            pl_queue_clear ();
            break;
          case Qt::Key_S:
            audqt::fileopener_show (audqt::FileMode::ExportPlaylist);
            break;
          default:
            return false;
        }
        break;
      case Qt::ControlModifier:
        switch (event->key ())
        {
          case Qt::Key_Space:
            select_toggle (true, 0);
            break;
          case Qt::Key_Up:
            select_slide (true, -1);
            break;
          case Qt::Key_Down:
            select_slide (true, 1);
            break;
          case Qt::Key_PageUp:
            select_slide (true, -m_rows);
            break;
          case Qt::Key_PageDown:
            select_slide (true, m_rows);
            break;
          case Qt::Key_Home:
            select_slide (false, 0);
            break;
          case Qt::Key_End:
            select_slide (false, m_length - 1);
            break;
          /* JWT:MUST ADD THESE MENU ITEMS MANUALLY HERE (GTK BINDS 'EM AUTOMATICALLY)!: */
          case Qt::Key_D:
            view_set_double_size (! aud_get_bool ("skins", "double_size"));
            break;
          case Qt::Key_F:
            audqt::playlist_search_and_select ();
            break;
          case Qt::Key_L:
            action_play_location ();
            break;
          case Qt::Key_M:
            aud_set_bool (nullptr, "stop_after_current_song",
                    ! aud_get_bool (nullptr, "stop_after_current_song"));
            view_redraw_main ();
            break;
          case Qt::Key_N:
            aud_set_bool (nullptr, "no_playlist_advance",
                    ! aud_get_bool (nullptr, "no_playlist_advance"));
            view_redraw_main ();
            break;
          case Qt::Key_O:
            view_set_on_top (! aud_get_bool ("skins", "always_on_top"));
            break;
          case Qt::Key_P:
            audqt::prefswin_show ();
            break;
          case Qt::Key_S:
            view_set_sticky (! aud_get_bool ("skins", "sticky"));
            break;
          case Qt::Key_T:
            aud_playlist_new ();
            break;
          case Qt::Key_U:
            audqt::queue_manager_show ();
            break;
          case Qt::Key_W:
            view_set_player_shaded (! aud_get_bool ("skins", "player_shaded"));
            break;
          default:
            return false;
        }
        break;
      case Qt::AltModifier:
        switch (event->key ())
        {
          case Qt::Key_Up:
            select_move (true, -1);
            break;
          case Qt::Key_Down:
            select_move (true, 1);
            break;
          case Qt::Key_PageUp:
            select_move (true, -m_rows);
            break;
          case Qt::Key_PageDown:
            select_move (true, m_rows);
            break;
          case Qt::Key_Home:
            select_move (false, 0);
            break;
          case Qt::Key_End:
            select_move (false, m_length - 1);
            break;
          /* JWT:MUST ADD THESE MENU ITEMS MANUALLY HERE (GTK BINDS 'EM AUTOMATICALLY)!: */
          case Qt::Key_E:
            view_set_show_playlist (! aud_get_bool ("skins", "playlist_visible"));
            break;
          case Qt::Key_G:
            view_set_show_equalizer (! aud_get_bool ("skins", "equalizer_visible"));
            break;
          default:
            return false;
        }
        break;
      default:
        return false;
    }

    refresh ();
    return true;
}

void PlaylistWidget::row_info (int * rows, int * first)
{
    * rows = m_rows;
    * first = m_first;
}

void PlaylistWidget::scroll_to (int row)
{
    cancel_all ();
    m_first = row;
    refresh ();
}

void PlaylistWidget::set_focused (int row)
{
    cancel_all ();
    aud_playlist_set_focus (m_playlist, row);
    ensure_visible (row);
    refresh ();
}

void PlaylistWidget::hover (int x, int y)
{
    int row;

    if (y < m_offset)
        row = m_first;
    else if (y > m_offset + m_row_height * m_rows)
        row = m_first + m_rows;
    else
        row = m_first + (y - m_offset + m_row_height / 2) / m_row_height;

    if (row > m_length)
        row = m_length;

    if (row != m_hover)
    {
        m_hover = row;
        queue_draw ();
    }
}

int PlaylistWidget::hover_end ()
{
    int temp = m_hover;
    m_hover = -1;

    queue_draw ();
    return temp;
}

bool PlaylistWidget::button_press (QMouseEvent * event)
{
    int position = calc_position (QtCompat::y (event));
    int state = event->modifiers () & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier);

    cancel_all ();

    switch (event->type ())
    {
      case QEvent::MouseButtonPress:
        switch (event->button ())
        {
          case Qt::LeftButton:
            if (position == -1 || position == m_length)
                return true;

            switch (state)
            {
              case 0:
                if (aud_playlist_entry_get_selected (m_playlist, position))
                    select_slide (false, position);
                else
                    select_single (false, position);

                m_drag = DRAG_MOVE;
                break;
              case Qt::ShiftModifier:
                select_extend (false, position);
                m_drag = DRAG_SELECT;
                break;
              case Qt::ControlModifier:
                select_toggle (false, position);
                m_drag = DRAG_SELECT;
                break;
              default:
                return true;
            }

            break;
          case Qt::RightButton:
            if (state)
                return true;

            if (position != -1 && position != m_length)
            {
                if (aud_playlist_entry_get_selected (m_playlist, position))
                    select_slide (false, position);
                else
                    select_single (false, position);
            }

            menu_popup ((position == -1) ? UI_MENU_PLAYLIST :
             UI_MENU_PLAYLIST_CONTEXT, QtCompat::globalX (event), QtCompat::globalY (event),
             false, false);
            break;
          default:
            return false;
        }

        break;
      case QEvent::MouseButtonDblClick:
        if (event->button () != Qt::LeftButton || state || position == m_length)
            return true;

        if (position != -1)
            aud_playlist_set_position (m_playlist, position);

        aud_playlist_play (m_playlist);
        break;
      default:
        return true;
    }

    refresh ();
    return true;
}

bool PlaylistWidget::button_release (QMouseEvent * event)
{
    cancel_all ();
    return true;
}

void PlaylistWidget::scroll_timeout ()
{
    int position = adjust_position (true, m_scroll);
    if (position == -1)
        return;

    switch (m_drag)
    {
      case DRAG_SELECT:
        select_extend (false, position);
        break;
      case DRAG_MOVE:
        select_move (false, position);
        break;
    }

    refresh ();
}

bool PlaylistWidget::motion (QMouseEvent * event)
{
    int position = calc_position (QtCompat::y (event));

    if (m_drag)
    {
        if (position == -1 || position == m_length)
        {
            if (! m_scroll)
                scroll_timer.start ();

            m_scroll = (position == -1 ? -1 : 1);
        }
        else
        {
            if (m_scroll)
            {
                m_scroll = 0;
                scroll_timer.stop ();
            }

            switch (m_drag)
            {
              case DRAG_SELECT:
                select_extend (false, position);
                break;
              case DRAG_MOVE:
                select_move (false, position);
                break;
            }

            refresh ();
        }
    }
    else
    {
        if (position == -1 || position == m_length)
            cancel_all ();
        else if (aud_get_bool (nullptr, "show_filepopup_for_tuple") && m_popup_pos != position)
        {
            cancel_all ();
            popup_trigger (position);
        }
    }

    return true;
}

void PlaylistWidget::dragEnterEvent (QDragEnterEvent * event)
{
    dragMoveEvent (event);
}

void PlaylistWidget::dragMoveEvent (QDragMoveEvent * event)
{
    const QMimeData * mimedata = event->mimeData ();

    if (event->proposedAction () == Qt::CopyAction && mimedata->hasUrls ())
    {
        auto p = QtCompat::pos (event);
        hover (p.x (), p.y ());
        event->acceptProposedAction ();
    }
}

void PlaylistWidget::dragLeaveEvent (QDragLeaveEvent *)
{
    hover_end ();
}

void PlaylistWidget::dropEvent (QDropEvent * event)
{
    const QMimeData * mimedata = event->mimeData ();

    if (event->proposedAction () == Qt::CopyAction && mimedata->hasUrls ())
    {
        auto p = QtCompat::pos (event);
        hover (p.x (), p.y ());

        Index<PlaylistAddItem> files;

        for (const auto &url: mimedata->urls ())
            files.append (String (url.toEncoded ()));

        aud_drct_pl_add_list (std::move (files), hover_end ());
        event->acceptProposedAction ();
    }
    else
        hover_end ();
}

bool PlaylistWidget::leave ()
{
    if (! m_drag)
        cancel_all ();

    return true;
}

void PlaylistWidget::popup_trigger (int pos)
{
    audqt::infopopup_hide ();

    m_popup_pos = pos;
    m_popup_timer.queue (aud_get_int (nullptr, "filepopup_delay") * 100,
            [this] () { popup_show (); });
}

void PlaylistWidget::popup_show ()
{
    audqt::infopopup_show (m_playlist, m_popup_pos);
}

void PlaylistWidget::popup_hide ()
{
    audqt::infopopup_hide ();

    m_popup_pos = -1;
    m_popup_timer.stop ();
}
