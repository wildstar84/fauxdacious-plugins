PLUGIN = skins-qt${PLUGIN_SUFFIX}

SRCS = actions.cc \
       button.cc \
       dialogs-qt.cc \
       dock.cc \
       drag-handle.cc \
       eq-graph.cc \
       eq-slider.cc \
       equalizer.cc \
       hslider.cc \
       main.cc \
       menurow.cc \
       menu-ops.cc \
       menus.cc \
       monostereo.cc \
       number.cc \
       playlistwin.cc \
       playlist-slider.cc \
       playlist-widget.cc \
       playstatus.cc \
       plugin.cc \
       plugin-window.cc \
       skin.cc \
       skin-ini.cc \
       skins_cfg.cc \
       skins_util.cc \
       skinselector.cc \
       svis.cc \
       textbox.cc \
       view.cc \
       vis-callbacks.cc \
       vis.cc \
       widget.cc \
       window.cc \
       x11-utils.cc

include ../../buildsys.mk
include ../../extra.mk

plugindir := ${plugindir}/${GENERAL_PLUGIN_DIR}

LD = ${CXX}
CFLAGS += ${PLUGIN_CFLAGS}
CPPFLAGS += ${PLUGIN_CPPFLAGS} ${QT_CFLAGS} ${QTX11EXTRAS_CFLAGS} -I../.. ${GLIB_CFLAGS} -I..
LIBS += -lm ${GLIB_LIBS} ${QT_LIBS} ${QTX11EXTRAS_LIBS} -lX11 -lfauxdqt
