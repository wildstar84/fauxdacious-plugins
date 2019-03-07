PLUGIN = m3u${PLUGIN_SUFFIX}

SRCS = m3u.cc

include ../../buildsys.mk
include ../../extra.mk

plugindir := ${plugindir}/${CONTAINER_PLUGIN_DIR}

LD = ${CXX}

CFLAGS += ${PLUGIN_CFLAGS}
CPPFLAGS += ${PLUGIN_CPPFLAGS} ${CUE_CFLAGS} ${GLIB_CFLAGS} -I../..
LIBS += ${CUE_LIBS} ${GLIB_LIBS}
