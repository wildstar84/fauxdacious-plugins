PLUGIN = info-bar-plugin-gtk${PLUGIN_SUFFIX}

SRCS = info-bar-plugin.cc

include ../../buildsys.mk
include ../../extra.mk

plugindir := ${plugindir}/${GENERAL_PLUGIN_DIR}

LD = ${CXX}
CFLAGS += ${PLUGIN_CFLAGS}
CPPFLAGS += ${PLUGIN_CPPFLAGS} ${GTK_CFLAGS} ${GLIB_CFLAGS} -I../..
LIBS += -lm ${GTK_LIBS} ${GLIB_LIBS} -lfauxdgui
