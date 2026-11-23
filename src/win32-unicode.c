/* Copyright (c) 2004-2015 LoRd_MuldeR <mulder2@gmx.de>
   File: win32-unicode.c

   This file was originally part of a patch included with LameXP,
   released under the same license as the original audio tools.

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   - Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

   - Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
#include "sox_i.h"

#include <windows.h>
#include <direct.h>
#include <fcntl.h>
#include <io.h>

char *win32_utf16_to_utf8(const wchar_t *input)
{
  char *Buffer;
  int BuffSize = 0, Result = 0;

  BuffSize = WideCharToMultiByte(CP_UTF8, 0, input, -1, NULL, 0, NULL, NULL);
  lsx_valloc(Buffer, BuffSize);
  Result = WideCharToMultiByte(CP_UTF8, 0, input, -1, Buffer, BuffSize, NULL, NULL);

  return ((Result > 0) && (Result <= BuffSize)) ? Buffer : NULL;
}

wchar_t *win32_utf8_to_utf16(const char *input)
{
  wchar_t *Buffer;
  int BuffSize = 0, Result = 0;

  BuffSize = MultiByteToWideChar(CP_UTF8, 0, input, -1, NULL, 0);
  lsx_valloc(Buffer, BuffSize);
  Result = MultiByteToWideChar(CP_UTF8, 0, input, -1, Buffer, BuffSize);

  return ((Result > 0) && (Result <= BuffSize)) ? Buffer : NULL;
}

/* getenv() gives the value in the ANSI code page; a path taken from the
 * environment has to be UTF-8 like every other name passed to the wrappers
 * below.  The caller frees the result. */
char *win32_getenv_utf8(const wchar_t *name)
{
  const wchar_t *value = _wgetenv(name);

  return value ? win32_utf16_to_utf8(value) : NULL;
}

FILE *lsx_fopen(const char *filename_utf8, const char *mode_utf8)
{
  FILE *ret = NULL;
  wchar_t *filename_utf16 = win32_utf8_to_utf16(filename_utf8);
  wchar_t *mode_utf16 = win32_utf8_to_utf16(mode_utf8);

  if(filename_utf16 && mode_utf16) {
    ret = _wfopen(filename_utf16, mode_utf16);
  }

  if(filename_utf16) free(filename_utf16);
  if(mode_utf16) free(mode_utf16);

  return ret;
}

int lsx_stat(const char *path_utf8, struct stat *buf)
{
  int ret = -1;
  struct _stat st;

  wchar_t *path_utf16 = win32_utf8_to_utf16(path_utf8);
  if(path_utf16) {
    ret = _wstat(path_utf16, &st);
    buf->st_mode = st.st_mode;
    free(path_utf16);
  }

  return ret;
}

int lsx_unlink(const char *path_utf8)
{
  int ret = -1;

  wchar_t *path_utf16 = win32_utf8_to_utf16(path_utf8);
  if(path_utf16) {
    ret = _wunlink(path_utf16);
    free(path_utf16);
  }

  return ret;
}

int lsx_remove(const char *path_utf8)
{
  int ret = -1;

  wchar_t *path_utf16 = win32_utf8_to_utf16(path_utf8);
  if(path_utf16) {
    ret = _wremove(path_utf16);
    free(path_utf16);
  }

  return ret;
}

int lsx_rename(const char *old_utf8, const char *new_utf8)
{
  int ret = -1;
  wchar_t *old_utf16 = win32_utf8_to_utf16(old_utf8);
  wchar_t *new_utf16 = win32_utf8_to_utf16(new_utf8);

  if(old_utf16 && new_utf16) {
    ret = _wrename(old_utf16, new_utf16);
  }

  if(old_utf16) free(old_utf16);
  if(new_utf16) free(new_utf16);

  return ret;
}

int lsx_mkdir(const char *path_utf8)
{
  int ret = -1;

  wchar_t *path_utf16 = win32_utf8_to_utf16(path_utf8);
  if(path_utf16) {
    ret = _wmkdir(path_utf16);
    free(path_utf16);
  }

  return ret;
}

/* mkstemp() for a UTF-8 template, which receives the name chosen.  The file
 * is deleted when it is closed. */
int lsx_mkstemp(char *template_utf8)
{
  int ret = -1;
  wchar_t *template_utf16 = win32_utf8_to_utf16(template_utf8);

  if(template_utf16 && _wmktemp(template_utf16)) {
    char *name_utf8 = win32_utf16_to_utf8(template_utf16);

    /* Only the trailing XXXXXX change, so the length is the same */
    if(name_utf8) {
      strcpy(template_utf8, name_utf8);
      free(name_utf8);
    }
    ret = _wopen(template_utf16, _O_BINARY | _O_TEMPORARY | _O_RDWR | _O_TRUNC | _O_CREAT,
                 _S_IREAD | _S_IWRITE);
  }

  if(template_utf16) free(template_utf16);

  return ret;
}

/* popen() for a UTF-8 command, run by cmd.exe as popen() does. */
FILE *lsx_shell_popen(const char *command_utf8, const char *mode)
{
  FILE *ret = NULL;
  wchar_t *command_utf16 = win32_utf8_to_utf16(command_utf8);
  wchar_t *mode_utf16 = win32_utf8_to_utf16(mode);

  if(command_utf16 && mode_utf16)
    ret = _wpopen(command_utf16, mode_utf16);

  free(command_utf16);
  free(mode_utf16);

  return ret;
}

/* A program started by win32_popen_argv() and the stream of its pipe. */
typedef struct win32_child {
  FILE *stream;
  HANDLE process;
  struct win32_child *next;
} win32_child;

static win32_child *win32_children;

/* Appends arg to a command line so that the child's C runtime, which splits
 * it as CommandLineToArgvW does, gets it back unchanged: inside double quotes
 * a quote is escaped with a backslash, and backslashes are doubled only where
 * they come before a quote, the closing one included. */
static wchar_t *append_quoted(wchar_t *q, const wchar_t *arg)
{
  const wchar_t *p;
  size_t backslashes, i;

  if(*arg && !wcspbrk(arg, L" \t\n\v\"")) {
    for(p = arg; *p; p++)
      *q++ = *p;
    return q;
  }
  *q++ = L'"';
  for(p = arg; ; p++) {
    for(backslashes = 0; *p == L'\\'; p++)
      backslashes++;
    if(!*p) {
      for(i = 0; i < backslashes * 2; i++)
        *q++ = L'\\';
      break;
    }
    if(*p == L'"')
      backslashes = backslashes * 2 + 1;
    for(i = 0; i < backslashes; i++)
      *q++ = L'\\';
    *q++ = *p;
  }
  *q++ = L'"';
  return q;
}

/* An inheritable copy of one of this process's standard handles, or NULL. */
static HANDLE inheritable_std_handle(DWORD which)
{
  HANDLE h = GetStdHandle(which), copy;

  if(h == NULL || h == INVALID_HANDLE_VALUE ||
     !DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &copy, 0, TRUE, DUPLICATE_SAME_ACCESS))
    return NULL;
  return copy;
}

/* lsx_popen() for Windows: runs argv[0] with the arguments argv[1...], all
 * UTF-8, without a shell, so nothing in them is interpreted as command
 * syntax.  The child's standard output (type 'r') or input (type 'w') is a
 * pipe to the stream returned; it inherits only its three standard handles. */
FILE *win32_popen_argv(char **argv_utf8, char type)
{
  SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
  STARTUPINFOEXW si;
  PROCESS_INFORMATION pi;
  HANDLE pipe_read, pipe_write, parent_end, child_end, other_std, std_err;
  HANDLE inherited[3];
  DWORD n_inherited = 0;
  SIZE_T attr_size = 0;
  wchar_t **argv_utf16;
  wchar_t *command, *q;
  size_t n_args, length, i;
  FILE *stream = NULL;
  win32_child *child;
  int fd, spawned;

  if(type != 'r' && type != 'w') {
    errno = EINVAL;
    return NULL;
  }

  for(n_args = 0; argv_utf8[n_args]; n_args++);
  argv_utf16 = lsx_calloc(n_args + 1, sizeof(*argv_utf16));
  length = 1;
  for(i = 0; i < n_args; i++) {
    argv_utf16[i] = win32_utf8_to_utf16(argv_utf8[i]);
    if(!argv_utf16[i]) {
      while(i--)
        free(argv_utf16[i]);
      free(argv_utf16);
      errno = EINVAL;
      return NULL;
    }
    /* Every character doubled at worst, two quotes and a space */
    length += wcslen(argv_utf16[i]) * 2 + 3;
  }
  command = lsx_malloc(length * sizeof(*command));
  q = command;
  for(i = 0; i < n_args; i++) {
    if(i > 0)
      *q++ = L' ';
    q = append_quoted(q, argv_utf16[i]);
    free(argv_utf16[i]);
  }
  *q = L'\0';
  free(argv_utf16);

  if(!CreatePipe(&pipe_read, &pipe_write, &sa, 0)) {
    free(command);
    errno = EMFILE;
    return NULL;
  }
  parent_end = type == 'r' ? pipe_read : pipe_write;
  child_end = type == 'r' ? pipe_write : pipe_read;
  SetHandleInformation(parent_end, HANDLE_FLAG_INHERIT, 0);
  other_std = inheritable_std_handle(type == 'r' ? STD_INPUT_HANDLE : STD_OUTPUT_HANDLE);
  std_err = inheritable_std_handle(STD_ERROR_HANDLE);

  memset(&si, 0, sizeof(si));
  si.StartupInfo.cb = sizeof(si);
  si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  si.StartupInfo.hStdInput = type == 'r' ? other_std : child_end;
  si.StartupInfo.hStdOutput = type == 'r' ? child_end : other_std;
  si.StartupInfo.hStdError = std_err;
  inherited[n_inherited++] = child_end;
  if(other_std)
    inherited[n_inherited++] = other_std;
  if(std_err)
    inherited[n_inherited++] = std_err;

  /* Only the handles listed are inherited, not every inheritable handle of
   * this process, such as the files it has open. */
  InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);
  si.lpAttributeList = lsx_malloc(attr_size);
  spawned = InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attr_size) &&
      UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                inherited, n_inherited * sizeof(*inherited), NULL, NULL) &&
      CreateProcessW(NULL, command, NULL, NULL, TRUE, EXTENDED_STARTUPINFO_PRESENT,
                     NULL, NULL, &si.StartupInfo, &pi);
  if(!spawned)
    errno = GetLastError() == ERROR_FILE_NOT_FOUND ? ENOENT : EINVAL;
  DeleteProcThreadAttributeList(si.lpAttributeList);
  free(si.lpAttributeList);
  free(command);
  CloseHandle(child_end);
  if(other_std)
    CloseHandle(other_std);
  if(std_err)
    CloseHandle(std_err);
  if(!spawned) {
    CloseHandle(parent_end);
    return NULL;
  }
  CloseHandle(pi.hThread);

  fd = _open_osfhandle((intptr_t)parent_end, (type == 'r' ? _O_RDONLY : _O_WRONLY) | _O_BINARY);
  if(fd != -1) {
    stream = _fdopen(fd, type == 'r' ? "rb" : "wb");
    if(!stream)
      _close(fd);
  } else
    CloseHandle(parent_end);
  if(!stream) {
    TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess);
    errno = EMFILE;
    return NULL;
  }

  child = lsx_malloc(sizeof(*child));
  child->stream = stream;
  child->process = pi.hProcess;
  child->next = win32_children;
  win32_children = child;
  return stream;
}

/* Closes a stream returned by win32_popen_argv() and waits for its program:
 * returns 0 if the stream is not one of those, else 1 with *status set to
 * the program's exit code, or to -1 if closing the stream failed.  When
 * cancel is set a program still running is terminated and *status is 0. */
int win32_pclose_argv(FILE *stream, int cancel, int *status)
{
  win32_child **link, *child = NULL;
  DWORD exit_code = 0;
  int close_error = 0;

  for(link = &win32_children; *link; link = &(*link)->next) {
    if((*link)->stream == stream) {
      child = *link;
      *link = child->next;
      break;
    }
  }
  if(!child)
    return 0;

  /* A stream abandoned by its consumer must not delay shutdown. */
  if(cancel && WaitForSingleObject(child->process, 0) == WAIT_TIMEOUT)
    TerminateProcess(child->process, 1);
  if(fclose(stream) != 0)
    close_error = errno;
  WaitForSingleObject(child->process, INFINITE);
  GetExitCodeProcess(child->process, &exit_code);
  CloseHandle(child->process);
  free(child);

  if(close_error) {
    errno = close_error;
    *status = -1;
  } else
    *status = cancel ? 0 : (int)exit_code;
  return 1;
}
