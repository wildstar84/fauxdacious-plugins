PLUGIN = dvd-ng${PLUGIN_SUFFIX}

SRCS = dvd-ng.cc

include ../../buildsys.mk
include ../../extra.mk

plugindir := ${plugindir}/${INPUT_PLUGIN_DIR}

LD = ${CXX}

CFLAGS += ${PLUGIN_CFLAGS}
CPPFLAGS += ${PLUGIN_CPPFLAGS} ${FFMPEG_CFLAGS} ${SDL_CFLAGS} -I../..
LIBS += ${FFMPEG_LIBS} -lavcodec -lm -ldvdread ${SDL_LIBS} -lz -lm -ldvdnav
