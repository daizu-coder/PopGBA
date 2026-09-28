/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
/*
 * Minimal libretro-common <streams/file_stream.h> replacement.
 *
 * The gpSP core (gba_memory.c: load_bios / load_gamepak /
 * load_gamepak_page) is the only consumer of the RFILE API, and only ever
 * for read-only BIOS / ROM access via exactly six entry points:
 *   filestream_open, filestream_read, filestream_seek,
 *   filestream_get_size, filestream_close, filestream_vfs_init
 * (verified by grep over the whole core tree).  libretro-common's real
 * file_stream.c pulls in vfs_implementation.c (31 KB: mmap, dirent,
 * _wstat64, symlink handling) none of which cegcc/WinCE provides or the
 * core exercises, so this stub implements just those six over plain
 * fopen/fread/fseek/ftell/fclose from the cegcc CRT.
 *
 * filestream_vfs_init is a no-op: the CE frontend never answers
 * RETRO_ENVIRONMENT_GET_VFS_INTERFACE, so libretro.c's guarded call is
 * dead - the symbol only has to resolve at link time.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include <libretro.h>
#include <streams/file_stream.h>

struct RFILE
{
   FILE *fp;
};

RFILE *filestream_open(const char *path, unsigned mode, unsigned hints)
{
   const char *cmode;
   RFILE *stream;
   FILE  *fp;

   (void)hints;

   if (!path)
      return NULL;

   switch (mode & RETRO_VFS_FILE_ACCESS_READ_WRITE)
   {
      case RETRO_VFS_FILE_ACCESS_READ_WRITE:
         cmode = (mode & RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING)
                 ? "r+b" : "w+b";
         break;
      case RETRO_VFS_FILE_ACCESS_WRITE:
         cmode = (mode & RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING)
                 ? "r+b" : "wb";
         break;
      case RETRO_VFS_FILE_ACCESS_READ:
      default:
         cmode = "rb";
         break;
   }

   fp = fopen(path, cmode);
   if (!fp)
      return NULL;

   stream = (RFILE *)malloc(sizeof(*stream));
   if (!stream)
   {
      fclose(fp);
      return NULL;
   }
   stream->fp = fp;
   return stream;
}

int filestream_close(RFILE *stream)
{
   int rc = 0;
   if (!stream)
      return -1;
   if (stream->fp)
      rc = fclose(stream->fp);
   free(stream);
   return rc;
}

int64_t filestream_seek(RFILE *stream, int64_t offset, int seek_position)
{
   if (!stream || !stream->fp)
      return -1;
   if (fseek(stream->fp, (long)offset, seek_position) != 0)
      return -1;
   return ftell(stream->fp);
}

int64_t filestream_read(RFILE *stream, void *data, int64_t len)
{
   if (!stream || !stream->fp || len < 0)
      return -1;
   return (int64_t)fread(data, 1, (size_t)len, stream->fp);
}

int64_t filestream_write(RFILE *stream, const void *data, int64_t len)
{
   if (!stream || !stream->fp || len < 0)
      return -1;
   return (int64_t)fwrite(data, 1, (size_t)len, stream->fp);
}

int64_t filestream_tell(RFILE *stream)
{
   if (!stream || !stream->fp)
      return -1;
   return ftell(stream->fp);
}

int64_t filestream_get_size(RFILE *stream)
{
   long cur, end;

   if (!stream || !stream->fp)
      return -1;

   cur = ftell(stream->fp);
   if (cur < 0 || fseek(stream->fp, 0, SEEK_END) != 0)
      return -1;
   end = ftell(stream->fp);
   fseek(stream->fp, cur, SEEK_SET);
   return (int64_t)end;
}

void filestream_vfs_init(const struct retro_vfs_interface_info *vfs_info)
{
   (void)vfs_info;
}
