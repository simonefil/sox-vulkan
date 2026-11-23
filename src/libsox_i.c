/* libSoX internal functions that apply to both formats and effects
 * All public functions & data are prefixed with lsx_ .
 *
 * Copyright (c) 2008 robs@users.sourceforge.net
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */


#include "sox_i.h"

#ifdef HAVE_IO_H
  #include <io.h>
#endif

#ifdef HAVE_UNISTD_H
  #include <unistd.h>
#endif

#if !defined(_WIN32) && !defined(HAVE_MKSTEMP)
  #include <fcntl.h>
  #include <sys/types.h>
  #include <sys/stat.h>
  #define mkstemp(t) open(mktemp(t), O_RDWR|O_TRUNC|O_CREAT, S_IREAD|S_IWRITE)
  #define FAKE_MKSTEMP "fake "
#else
  #define FAKE_MKSTEMP
#endif

#ifdef _WIN32
static int check_dir(char * buf, size_t buflen, char const * name)
{
  struct stat st;
  if (!name || lsx_stat(name, &st) || (st.st_mode & S_IFMT) != S_IFDIR)
  {
    return 0;
  }
  else
  {
    strncpy(buf, name, buflen - 1);
    buf[buflen - 1] = 0;
    return strlen(name) == strlen(buf);
  }
}
#endif

FILE * lsx_tmpfile(void)
{
  char const * path = sox_globals.tmp_path;

  /*
  On Win32, tmpfile() is broken - it creates the file in the root directory of
  the current drive (the user probably doesn't have permission to write there!)
  instead of in a valid temporary directory (like TEMP or TMP). So if tmp_path
  is null, figure out a reasonable default.
  To force use of tmpfile(), set sox_globals.tmp_path = "".
  */
#ifdef _WIN32
  if (!path)
  {
    static char default_path[260] = "";
    if (default_path[0] == 0)
    {
      /* UTF-8, as check_dir and the template below expect */
      char * temp = win32_getenv_utf8(L"TEMP");
      char * tmp = win32_getenv_utf8(L"TMP");

      if (!check_dir(default_path, sizeof(default_path), temp)
          && !check_dir(default_path, sizeof(default_path), tmp)
      #ifdef __CYGWIN__
          && !check_dir(default_path, sizeof(default_path), "/tmp")
      #endif
          )
      {
        strcpy(default_path, ".");
      }
      free(temp);
      free(tmp);
    }

    path = default_path;
  }
#endif

  if (path && path[0]) {
    /* Emulate tmpfile (delete on close); tmp dir is given tmp_path: */
    char const * const end = "/libSoX.tmp.XXXXXX";
    char * name = lsx_malloc(strlen(path) + strlen(end) + 1);
    int fildes;
    strcpy(name, path);
    strcat(name, end);
#ifdef _WIN32
    /* The template is UTF-8 like every file name on Windows, and an open
     * file cannot be unlinked there: lsx_mkstemp opens it to be deleted on
     * close. */
    fildes = lsx_mkstemp(name);
    lsx_debug("lsx_mkstemp, name=%s (O_TEMPORARY)", name);
#else
    fildes = mkstemp(name);
    lsx_debug(FAKE_MKSTEMP "mkstemp, name=%s (unlinked)", name);
    lsx_unlink(name);
#endif
    free(name);
    return fildes == -1? NULL : fdopen(fildes, "w+b");
  }

  /* Use standard tmpfile (delete on close); tmp dir is undefined: */
  lsx_debug("tmpfile()");
  return tmpfile();
}
