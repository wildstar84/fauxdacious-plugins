PLUGIN = hotkey${PLUGIN_SUFFIX}

SRCS = plugin.cc gui.cc grab.cc

include ../../buildsys.mk
include ../../extra.mk

plugindir := ${plugindir}/${GENERAL_PLUGIN_DIR}

LD = ${CXX}
CFLAGS += ${PLUGIN_CFLAGS}
CPPFLAGS += ${PLUGIN_CPPFLAGS} ${GTK_CFLAGS} -I../.. -I..
LIBS += ${GTK_LIBS} -lX11 -lfauxdgui
