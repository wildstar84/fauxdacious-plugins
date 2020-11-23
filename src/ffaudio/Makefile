PLUGIN = ffaudio${PLUGIN_SUFFIX}

SRCS = ffaudio-core.cc ffaudio-io.cc

include ../../buildsys.mk
include ../../extra.mk

plugindir := ${plugindir}/${INPUT_PLUGIN_DIR}

LD = ${CXX}

CFLAGS += ${PLUGIN_CFLAGS}
CPPFLAGS += ${PLUGIN_CPPFLAGS} ${GLIB_CFLAGS} ${GTK_CFLAGS} ${FFMPEG_CFLAGS} ${SDL_CFLAGS} -I../.. -D_GNU_SOURCE=1 -D_REENTRANT
LIBS += ${GTK_LIBS} ${FFMPEG_LIBS} -lswscale -lavcodec -lfauxdtag ${SDL_LIBS} -lz -lm
