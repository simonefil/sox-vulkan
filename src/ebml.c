/* libSoX EBML, the binary markup Matroska and WebM are written in.
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

/* An element is an ID, a size and that many bytes of content.  IDs and
 * sizes are both variable-length integers: the count of leading zero bits
 * in the first byte gives the count of bytes that follow.  A size whose
 * value bits are all ones is unknown, and the element then ends where an
 * element that cannot be its child begins. */

#include "ebml.h"

#include <string.h>

static int read_exactly(sox_format_t * ft, void * buf, size_t len)
{
  if (lsx_readbuf(ft, buf, len) == len)
    return SOX_SUCCESS;
  if (!ft->sox_errno)
    lsx_fail_errno(ft, SOX_EHDR, "unexpected end of file");
  return SOX_EOF;
}

int lsx_ebml_read_id(sox_format_t * ft, sox_uint32_t * id)
{
  unsigned char b[4];
  unsigned length, i;

  if (lsx_readbuf(ft, b, 1) != 1) {
    if (lsx_error(ft) && !ft->sox_errno)
      lsx_fail_errno(ft, SOX_EHDR, "read error");
    return SOX_EOF;
  }
  for (length = 1; length <= 4 && !(b[0] & (0x100 >> length)); ++length);
  if (length > 4) {
    lsx_fail_errno(ft, SOX_EHDR, "invalid EBML element ID");
    return SOX_EOF;
  }
  if (length > 1 && read_exactly(ft, b + 1, length - 1) != SOX_SUCCESS)
    return SOX_EOF;
  for (*id = 0, i = 0; i < length; ++i)
    *id = *id << 8 | b[i];
  return SOX_SUCCESS;
}

int lsx_ebml_read_size(sox_format_t * ft, sox_uint64_t * size)
{
  unsigned char b[8];
  unsigned length, i;
  sox_bool unknown;

  if (read_exactly(ft, b, 1) != SOX_SUCCESS)
    return SOX_EOF;
  for (length = 1; length <= 8 && !(b[0] & (0x100 >> length)); ++length);
  if (length > 8) {
    lsx_fail_errno(ft, SOX_EHDR, "invalid EBML element size");
    return SOX_EOF;
  }
  if (length > 1 && read_exactly(ft, b + 1, length - 1) != SOX_SUCCESS)
    return SOX_EOF;
  *size = b[0] & (0xff >> length);
  unknown = *size == (0xffu >> length);
  for (i = 1; i < length; ++i) {
    *size = *size << 8 | b[i];
    unknown = unknown && b[i] == 0xff;
  }
  if (unknown)
    *size = EBML_UNKNOWN_SIZE;
  return SOX_SUCCESS;
}

int lsx_ebml_read_uint(sox_format_t * ft, sox_uint64_t size, sox_uint64_t * value)
{
  unsigned char b[8];
  unsigned i;

  if (size > 8) {
    lsx_fail_errno(ft, SOX_EHDR, "EBML integer of %lu bytes", (unsigned long)size);
    return SOX_EOF;
  }
  if (read_exactly(ft, b, (size_t)size) != SOX_SUCCESS)
    return SOX_EOF;
  for (*value = 0, i = 0; i < size; ++i)
    *value = *value << 8 | b[i];
  return SOX_SUCCESS;
}

int lsx_ebml_read_sint(sox_format_t * ft, sox_uint64_t size, sox_int64_t * value)
{
  sox_uint64_t u;

  if (lsx_ebml_read_uint(ft, size, &u) != SOX_SUCCESS)
    return SOX_EOF;
  if (size && size < 8 && (u >> (8 * size - 1)))
    u |= ~(sox_uint64_t)0 << (8 * size);
  *value = (sox_int64_t)u;
  return SOX_SUCCESS;
}

int lsx_ebml_read_float(sox_format_t * ft, sox_uint64_t size, double * value)
{
  sox_uint64_t u;

  if ((size && size != 4 && size != 8) || lsx_ebml_read_uint(ft, size, &u) != SOX_SUCCESS) {
    if (!ft->sox_errno)
      lsx_fail_errno(ft, SOX_EHDR, "EBML float of %lu bytes", (unsigned long)size);
    return SOX_EOF;
  }
  if (size == 4) {
    sox_uint32_t u32 = (sox_uint32_t)u;
    float f;

    memcpy(&f, &u32, sizeof(f));
    *value = f;
  }
  else if (size == 8)
    memcpy(value, &u, sizeof(*value));
  else *value = 0;
  return SOX_SUCCESS;
}

/* The content is followed by a NUL, so a string element reads as a string. */
int lsx_ebml_read_binary(sox_format_t * ft, sox_uint64_t size, size_t limit,
    unsigned char * * data)
{
  if (size > limit) {
    lsx_fail_errno(ft, SOX_EHDR, "EBML element of %lu bytes is too large", (unsigned long)size);
    return SOX_EOF;
  }
  *data = lsx_realloc(*data, (size_t)size + 1);
  (*data)[size] = 0;
  return read_exactly(ft, *data, (size_t)size);
}

int lsx_ebml_skip(sox_format_t * ft, sox_uint64_t size)
{
  unsigned char buf[4096];

  if (size == EBML_UNKNOWN_SIZE) {
    lsx_fail_errno(ft, SOX_EHDR, "EBML element of unknown size where one is not allowed");
    return SOX_EOF;
  }
  if (ft->seekable)
    return lsx_seeki(ft, (off_t)size, SEEK_CUR) == SOX_SUCCESS? SOX_SUCCESS : SOX_EOF;
  while (size) {
    size_t n = (size_t)min(size, sizeof(buf));

    if (read_exactly(ft, buf, n) != SOX_SUCCESS)
      return SOX_EOF;
    size -= n;
  }
  return SOX_SUCCESS;
}

/* A variable-length integer inside a buffer, marker removed; returns the
 * bytes it took, or 0 if it is invalid or runs past len. */
size_t lsx_ebml_parse_vint(unsigned char const * data, size_t len, sox_uint64_t * value)
{
  size_t length, i;

  if (!len)
    return 0;
  for (length = 1; length <= 8 && !(data[0] & (0x100 >> length)); ++length);
  if (length > 8 || length > len)
    return 0;
  *value = data[0] & (0xff >> length);
  for (i = 1; i < length; ++i)
    *value = *value << 8 | data[i];
  return length;
}

void lsx_ebml_put_bytes(lsx_ebml_buffer_t * b, void const * data, size_t size)
{
  if (b->size + size > b->alloc) {
    b->alloc = max(b->size + size, b->alloc * 2);
    b->data = lsx_realloc(b->data, b->alloc);
  }
  memcpy(b->data + b->size, data, size);
  b->size += size;
}

static void put_be(lsx_ebml_buffer_t * b, sox_uint64_t value, unsigned bytes)
{
  unsigned char buf[8];
  unsigned i;

  for (i = 0; i < bytes; ++i)
    buf[i] = (unsigned char)(value >> (8 * (bytes - 1 - i)));
  lsx_ebml_put_bytes(b, buf, bytes);
}

void lsx_ebml_put_id(lsx_ebml_buffer_t * b, sox_uint32_t id)
{
  put_be(b, id, id > 0xffffff? 4 : id > 0xffff? 3 : id > 0xff? 2 : 1);
}

/* width 0 takes the fewest bytes; a value with all bits set in its width
 * would read as unknown, so it moves up a byte. */
void lsx_ebml_put_size(lsx_ebml_buffer_t * b, sox_uint64_t size, unsigned width)
{
  if (size == EBML_UNKNOWN_SIZE) {
    put_be(b, 0x01ffffffffffffffull, 8);
    return;
  }
  if (!width)
    for (width = 1; width < 8 && size >= (1ull << (7 * width)) - 1; ++width);
  put_be(b, size | 1ull << (7 * width), width);
}

void lsx_ebml_put_uint(lsx_ebml_buffer_t * b, sox_uint32_t id, sox_uint64_t value)
{
  unsigned bytes;

  for (bytes = 1; bytes < 8 && value >> (8 * bytes); ++bytes);
  lsx_ebml_put_id(b, id);
  lsx_ebml_put_size(b, bytes, 1);
  put_be(b, value, bytes);
}

void lsx_ebml_put_sint(lsx_ebml_buffer_t * b, sox_uint32_t id, sox_int64_t value)
{
  unsigned bytes;

  for (bytes = 1; bytes < 8; ++bytes) {
    sox_int64_t limit = (sox_int64_t)1 << (8 * bytes - 1);

    if (value >= -limit && value < limit)
      break;
  }
  lsx_ebml_put_id(b, id);
  lsx_ebml_put_size(b, bytes, 1);
  put_be(b, (sox_uint64_t)value, bytes);
}

void lsx_ebml_put_float(lsx_ebml_buffer_t * b, sox_uint32_t id, double value)
{
  sox_uint64_t u;

  memcpy(&u, &value, sizeof(u));
  lsx_ebml_put_id(b, id);
  lsx_ebml_put_size(b, 8, 1);
  put_be(b, u, 8);
}

void lsx_ebml_put_binary(lsx_ebml_buffer_t * b, sox_uint32_t id, void const * data, size_t size)
{
  lsx_ebml_put_id(b, id);
  lsx_ebml_put_size(b, size, 0);
  lsx_ebml_put_bytes(b, data, size);
}

void lsx_ebml_put_string(lsx_ebml_buffer_t * b, sox_uint32_t id, char const * value)
{
  lsx_ebml_put_binary(b, id, value, strlen(value));
}

/* A master element whose size is filled in by lsx_ebml_close_master once
 * its children are in; the size always takes eight bytes. */
size_t lsx_ebml_open_master(lsx_ebml_buffer_t * b, sox_uint32_t id)
{
  lsx_ebml_put_id(b, id);
  lsx_ebml_put_size(b, 0, 8);
  return b->size;
}

void lsx_ebml_close_master(lsx_ebml_buffer_t * b, size_t start)
{
  sox_uint64_t size = (b->size - start) | 1ull << 56;
  unsigned i;

  for (i = 0; i < 8; ++i)
    b->data[start - 8 + i] = (unsigned char)(size >> (8 * (7 - i)));
}

/* A Void element of total bytes, header included; total is at least 2. */
void lsx_ebml_put_void(lsx_ebml_buffer_t * b, size_t total)
{
  static unsigned char const zeros[64];
  size_t content = total - (total <= 128? 2 : 9);

  lsx_ebml_put_id(b, EBML_ID_VOID);
  lsx_ebml_put_size(b, content, total <= 128? 1 : 8);
  while (content) {
    size_t n = min(content, sizeof(zeros));

    lsx_ebml_put_bytes(b, zeros, n);
    content -= n;
  }
}

/* The type to read a file as from its first bytes: the EBML header, and
 * in it the document type.  A header too long for what was read is taken
 * for Matroska, the EBML magic being specific enough on its own. */
char const * lsx_matroska_detect(unsigned char const * data, size_t len)
{
  sox_uint64_t size;
  size_t pos, end, n;

  if (len < 5 || memcmp(data, "\x1a\x45\xdf\xa3", 4) || !(n = lsx_ebml_parse_vint(data + 4, len - 4, &size)))
    return NULL;
  pos = 4 + n;
  end = size < len - pos? pos + (size_t)size : len;
  while (pos < end) {
    sox_uint64_t id, child;

    /* IDs keep their marker, so they are read as sizes and re-marked. */
    if (!(n = lsx_ebml_parse_vint(data + pos, end - pos, &id)) || n > 4)
      break;
    id |= 1ull << (7 * n);
    pos += n;
    if (!(n = lsx_ebml_parse_vint(data + pos, end - pos, &child)))
      break;
    pos += n;
    if (id == EBML_ID_DOCTYPE && child <= end - pos) {
      if (child == 8 && !memcmp(data + pos, "matroska", 8))
        return "mka";
      if (child == 4 && !memcmp(data + pos, "webm", 4))
        return "webm";
      return NULL;
    }
    if (child > end - pos)
      break;
    pos += (size_t)child;
  }
  return "mka";
}
