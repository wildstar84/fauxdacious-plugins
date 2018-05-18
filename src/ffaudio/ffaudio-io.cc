/*
 * ffaudio-io.c
 * Copyright 2011 John Lindgren
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions, and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions, and the following disclaimer in the documentation
 *    provided with the distribution.
 *
 * This software is provided "as is" and without any warranty, express or
 * implied. In no event shall the authors be liable for any damages arising from
 */

#define WANT_VFS_STDIO_COMPAT
#include <stdlib.h>
#include <stdio.h>
#include "ffaudio-stdinc.h"
#include <libfauxdcore/runtime.h>

#define IOBUF 4096

static FILE * m_savefile = NULL;    /* File to echo video stream out to (optional). */

static int read_cb (void * file, unsigned char * buf, int size)
{
    int res = ((VFSFile *) file)->fread (buf, 1, size);
    if (m_savefile && res > 0)
        ::fwrite (buf, res, 1, m_savefile);
    return res;
}

static int64_t seek_cb (void * file, int64_t offset, int whence)
{
    if (whence == AVSEEK_SIZE)
        return ((VFSFile *) file)->fsize ();
    if (m_savefile || ((VFSFile *) file)->fseek (offset, to_vfs_seek_type (whence & ~(int) AVSEEK_FORCE)))
        return -1;
    return ((VFSFile *) file)->ftell ();
}

AVIOContext * io_context_new (VFSFile & file)
{
    void * buf = av_malloc (IOBUF);
    if (aud_get_bool ("ffaudio", "save_video"))
    {
        String save_video_file = aud_get_str ("ffaudio", "save_video_file");
        if (! save_video_file[0])
#ifdef _WIN32
            save_video_file = String ("C:\\Temp\\lastvideo");
#else
            save_video_file = String ("/tmp/lastvideo");
#endif
        m_savefile = ::fopen ((const char *)save_video_file, "w");
    }
    return avio_alloc_context ((unsigned char *) buf, IOBUF, 0, & file, read_cb, nullptr, seek_cb);
}

AVIOContext * io_context_new2 (VFSFile & file)
{
    void * buf = av_malloc (IOBUF);
    if (aud_get_bool ("ffaudio", "save_video"))
    {
        String save_video_file = aud_get_str ("ffaudio", "save_video_file");
        if (! save_video_file[0])
#ifdef _WIN32
            save_video_file = String ("C:\\Temp\\lastvideo");
#else
            save_video_file = String ("/tmp/lastvideo");
#endif
        m_savefile = ::fopen ((const char *)save_video_file, "w");
    }
    return avio_alloc_context ((unsigned char *) buf, IOBUF, 0, & file, read_cb, nullptr, nullptr);
}

void io_context_free (AVIOContext * io)
{
    if (m_savefile)
    {
        ::fclose (m_savefile);
        m_savefile = nullptr;
    }
    av_free (io->buffer);
    av_free (io);
}
