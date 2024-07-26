/*
 * menus.cc
 * Copyright 2014 Michał Lipski and William Pitcock
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

#include "menus.h"
#include "../ui-common/menu-ops.h"

#include <libfauxdcore/drct.h>
#include <libfauxdcore/hook.h>
#include <libfauxdcore/i18n.h>
#include <libfauxdcore/runtime.h>
#include <libfauxdcore/plugins.h>
#include <libfauxdcore/drct.h>
#include <libfauxdcore/interface.h>
#include <libfauxdcore/playlist.h>

#include <libfauxdqt/libfauxdqt.h>
#include <libfauxdqt/menu.h>

static QMenu * services_menu_pl () { return audqt::menu_get_by_id (AudMenuID::Playlist); }

QMenu * qtui_build_pl_menu (QWidget * parent)
{
    static const audqt::MenuItem pl_items[] = {
        audqt::MenuCommand ({N_("Song _Info ..."), "dialog-information", "Alt+I"}, pl_song_info),
        audqt::MenuCommand ({N_("_Queue/Unqueue"), nullptr, "Alt+Q"}, pl_queue_toggle),
        audqt::MenuSep (),
        audqt::MenuCommand ({N_("_Open Containing Folder"), "folder"}, pl_open_folder),
        audqt::MenuCommand ({N_("_Refresh Selected"), "view-refresh", "F6"}, pl_refresh_sel),
        audqt::MenuSep (),
        audqt::MenuCommand ({N_("Cu_t"), "edit-cut", "Ctrl+X"}, pl_cut),
        audqt::MenuCommand ({N_("_Copy"), "edit-copy", "Ctrl+C"}, pl_copy),
        audqt::MenuCommand ({N_("_Paste"), "edit-paste", "Ctrl+V"}, pl_paste),
        audqt::MenuCommand ({N_("Paste at _End"), "edit-paste", "Shift+Ctrl+V"}, pl_paste_end),
        audqt::MenuCommand ({N_("Select _All"), "edit-select-all"}, pl_select_all),
        audqt::MenuCommand ({N_("Select _None"), "edit-clear"}, pl_select_none),
        audqt::MenuSep (),
        audqt::MenuSub ({N_("_Services")}, services_menu_pl)
    };

    return audqt::menu_build (pl_items, parent);
}
