PLUGIN = vumeter-qt${PLUGIN_SUFFIX}

SRCS = vumeter_qt.cc vumeter_qt_widget.cc

include ../../buildsys.mk
include ../../extra.mk

plugindir := ${plugindir}/${VISUALIZATION_PLUGIN_DIR}

LD = ${CXX}
CFLAGS += ${PLUGIN_CFLAGS}
CPPFLAGS += ${PLUGIN_CPPFLAGS} -I../.. ${QT_CFLAGS}
LIBS += -lm ${QT_LIBS}
