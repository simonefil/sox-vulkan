/* libSoX config file for MSVC9: (c) 2009 SoX contributors
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

/* Enable some extra warnings.
   Track the number of times each warning has been helpful. */
#pragma warning(3: 4287) /* 0 - constant sign mismatch */
#pragma warning(3: 4296) /* 1 - expression is always false */
#pragma warning(3: 4365) /* 0 - conversion sign mismatch */
#pragma warning(3: 4431) /* 0 - default int assumed */
#pragma warning(3: 4545) /* 0 - comma expression */
#pragma warning(3: 4546) /* 0 - comma expression */
#pragma warning(3: 4547) /* 0 - comma expression */
#pragma warning(3: 4548) /* 0 - comma expression */
#pragma warning(3: 4549) /* 0 - comma expression */
#pragma warning(3: 4555) /* 0 - expression has no effect */
#pragma warning(3: 4628) /* 0 - digraph */
#pragma warning(3: 4826) /* 0 - conversion sign extension */
#pragma warning(3: 4837) /* 0 - trigraph */
#pragma warning(3: 4905) /* 0 - string assignment mismatch */
#pragma warning(3: 4906) /* 0 - string assignment mismatch */

/* Used only by sox.c: */
#define MORE_INTERACTIVE

#define PACKAGE_EXTRA "msvc"

/* Special behavior defined by win32-ltdl: "./" is replaced with the name of the
   directory containing sox.exe. */
#define PKGLIBDIR "./soxlib"

#define HAVE_AMRNB 1
#define STATIC_AMRNB 1
#define DL_AMRNB 1

#define HAVE_AMRWB 1
#define STATIC_AMRWB 1
#define DL_AMRWB 1

#undef HAVE_FLAC
#undef STATIC_FLAC
#undef FLAC__NO_DLL

#undef HAVE_GSM
#undef STATIC_GSM

#undef HAVE_ID3TAG

#undef DL_LAME

#undef HAVE_LPC10
#undef STATIC_LPC10

#undef HAVE_MAD_H
#undef DL_MAD

#undef HAVE_MP3
#undef STATIC_MP3

#undef HAVE_OGG_VORBIS
#undef STATIC_OGG_VORBIS

#undef HAVE_PNG

#undef HAVE_SNDFILE
#undef HAVE_SNDFILE_1_0_18
#define HAVE_SFC_SET_SCALE_INT_FLOAT_WRITE 1
#undef STATIC_SNDFILE

#undef HAVE_SPEEXDSP

#undef HAVE_WAVEAUDIO
#undef STATIC_WAVEAUDIO

#undef HAVE_WAVPACK
#undef HAVE_WAVPACK_H
#undef STATIC_WAVPACK

#define HAVE_CONIO_H 1
#define HAVE__FSEEKI64 1
#define HAVE_FCNTL_H 1
#define HAVE_IO_H 1
#define HAVE_MEMORY_H 1
#define HAVE_POPEN 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRDUP 1
#define HAVE_STRING_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TIMEB_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_VSNPRINTF 1
#define HAVE_WIN32_GLOB_H 1
#define HAVE_WIN32_LTDL_H 1

#ifndef __cplusplus
#define inline __inline
#endif
