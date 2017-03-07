PLUGIN = youtubedl${PLUGIN_SUFFIX}

SRCS = youtubedl.cc

include ../../buildsys.mk
include ../../extra.mk

plugindir := ${plugindir}/${TRANSPORT_PLUGIN_DIR}

LD = ${CXX}

CPPFLAGS += -I../.. ${GIO_CFLAGS}
CFLAGS += ${PLUGIN_CFLAGS}
#LIBS += ${GIO_LIBS}
