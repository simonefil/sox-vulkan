/* DTS Coherent Acoustics elementary stream format handler.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#include "sox_i.h"
#include "dts-common.h"

#ifdef HAVE_FFMPEG_CODECS

LSX_FORMAT_HANDLER(dts)
{
  return lsx_dts_format_handler();
}

#endif
