PLUGIN = info-bar-plugin-qt${PLUGIN_SUFFIX}

SRCS = info-bar-plugin.cc \
    tool_bar.cc \
    time_slider.cc

include ../../buildsys.mk
include ../../extra.mk

plugindir := ${plugindir}/${GENERAL_PLUGIN_DIR}

LD = ${CXX}
CFLAGS += ${PLUGIN_CFLAGS}
CPPFLAGS += ${PLUGIN_CPPFLAGS} -I../.. ${QT_CFLAGS}
LIBS += ${QT_LIBS} -lfauxdqt
