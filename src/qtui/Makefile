PLUGIN = qtui${PLUGIN_SUFFIX}

SRCS = qtui.cc \
       dialogs-qt.cc \
       main_window.cc \
       menu-ops.cc \
       menus.cc \
       playlist-qt.cc \
       playlist_header.cc \
       playlist_model.cc \
       playlist_tabs.cc \
       search_bar.cc \
       status_bar.cc \
       tool_bar.cc \
       time_slider.cc \
       settings.cc

include ../../buildsys.mk
include ../../extra.mk

plugindir := ${plugindir}/${GENERAL_PLUGIN_DIR}

LD = ${CXX}

CFLAGS += ${PLUGIN_CFLAGS}
CPPFLAGS += ${PLUGIN_CPPFLAGS} ${GLIB_CFLAGS} -I../.. ${QT_CFLAGS}
LIBS += ${GLIB_LIBS} ${QT_LIBS} -lfauxdqt

%.moc: %.h
	moc $< -o $@

rc_%.cc: %.qrc
	rcc $< -o $@
