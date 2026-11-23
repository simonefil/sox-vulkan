/* libSoX minimal glob for MS-Windows: (c) 2009 SoX contributors
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

#include "win32-glob.h"
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#define WIN32_LEAN_AND_MEAN 1
#include <windows.h>

/* The rest of SoX passes file names around as UTF-8, so a name of MAX_PATH
 * UTF-16 code units needs up to four bytes each once it gets here. */
#define MAX_PATH_UTF8 (MAX_PATH * 4)

typedef struct file_entry
{
    char name[MAX_PATH_UTF8];
    struct file_entry *next;
} file_entry;

static int
insert(
    const char* path,
    const char* name,
    file_entry** phead)
{
    int len;
    file_entry* cur = malloc(sizeof(file_entry));
    if (!cur)
    {
        return ENOMEM;
    }

    len = _snprintf(cur->name, MAX_PATH_UTF8, "%s%s", path, name);
    cur->name[MAX_PATH_UTF8 - 1] = 0;
    cur->next = *phead;
    *phead = cur;

    return len < 0 || len >= MAX_PATH_UTF8 ? ENAMETOOLONG : 0;
}

static int
entry_comparer(
    const void* pv1,
    const void* pv2)
{
	const file_entry* const * pe1 = pv1;
	const file_entry* const * pe2 = pv2;
    return _stricmp((*pe1)->name, (*pe2)->name);
}

int
glob(
    const char *pattern,
    int flags,
    void *unused,
    glob_t *pglob)
{
    char path[MAX_PATH_UTF8];
    char name[MAX_PATH_UTF8];
    wchar_t wpattern[MAX_PATH];
    file_entry *head = NULL;
    int err = 0;
    size_t len;
    unsigned entries = 0;
    WIN32_FIND_DATAW finddata;
    HANDLE hfindfile;

    if (!pattern || flags != (flags & GLOB_FLAGS) || unused || !pglob)
    {
        errno = EINVAL;
        return EINVAL;
    }

    /* The terminating nul is included in the MAX_PATH (260) characters */
    if (strlen(pattern) > MAX_PATH_UTF8 - 1)
    {
        errno = ENAMETOOLONG;
        return ENAMETOOLONG;
    }
    strncpy(path, pattern, MAX_PATH_UTF8 - 1);
    path[MAX_PATH_UTF8 - 1] = '\0';

    len = strlen(path);
    while (len > 0 && path[len - 1] != '/' && path[len - 1] != '\\')
        len--;
    path[len] = 0;

    /* Only the wide API can round-trip a name that the ANSI code page has no
     * room for: FindFirstFileA hands back a literal '?' in its place, which
     * then fails to open -- and it reads the pattern in that code page too,
     * so a UTF-8 pattern with any non-ASCII in it matches nothing at all. */
    if (!MultiByteToWideChar(CP_UTF8, 0, pattern, -1, wpattern, MAX_PATH))
    {
        errno = ENAMETOOLONG;
        return ENAMETOOLONG;
    }

    hfindfile = FindFirstFileW(wpattern, &finddata);

    if (hfindfile == INVALID_HANDLE_VALUE)
    {
        if (flags & GLOB_NOCHECK)
        {
            err = insert("", pattern, &head);
            entries++;
        }
    }
    else
    {
        do
        {
            if (!WideCharToMultiByte(CP_UTF8, 0, finddata.cFileName, -1,
                                     name, MAX_PATH_UTF8, NULL, NULL))
            {
                err = ENAMETOOLONG;
                break;
            }
            err = insert(path, name, &head);
            entries++;
        } while (!err && FindNextFileW(hfindfile, &finddata));

        FindClose(hfindfile);
    }

    if (err == 0)
    {
        pglob->gl_pathv = malloc((entries + 1) * sizeof(char*));
        if (pglob->gl_pathv)
        {
            pglob->gl_pathc = entries;
            pglob->gl_pathv[entries] = NULL;
            for (; head; head = head->next, entries--)
                pglob->gl_pathv[entries - 1] = (char*)head;
            qsort(pglob->gl_pathv, pglob->gl_pathc, sizeof(char*), entry_comparer);
        }
        else
        {
            pglob->gl_pathc = 0;
            err = ENOMEM;
        }
    }
    else if (pglob)
    {
        pglob->gl_pathc = 0;
        pglob->gl_pathv = NULL;
    }

    if (err)
    {
        file_entry *cur;
        while (head)
        {
            cur = head;
            head = head->next;
            free(cur);
        }

        errno = err;
    }

    return err;
}

void
globfree(
    glob_t* pglob)
{
    if (pglob)
    {
        char** cur;
        for (cur = pglob->gl_pathv; *cur; cur++)
        {
            free(*cur);
        }

        pglob->gl_pathc = 0;
        pglob->gl_pathv = NULL;
    }
}
