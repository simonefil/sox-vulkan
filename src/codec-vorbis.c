/* libSoX packet codec: Vorbis, through libvorbis and libvorbisenc.
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

/* The private data holds the three header packets, identification, comment
 * and setup, laced as Xiph lacing does it: the packet count less one, then
 * the size of each packet but the last as a run of 255s ending in a smaller
 * byte.  Matroska stores them so, and the Ogg container translates.
 *
 * A packet decodes to a quarter of the previous block plus a quarter of its
 * own, and the first to none.  Up to 8 channels are in the order the Vorbis
 * specification gives, which is put into SoX's order and back; beyond 8 the
 * order is the application's, and is kept. */

#include "codec.h"

#include <string.h>

#include <vorbis/codec.h>
#include <vorbis/vorbisenc.h>

#define HEADERS 3

typedef struct {
  vorbis_info vi;
  vorbis_comment vc;
  vorbis_dsp_state vd;
  vorbis_block vb;
  ogg_int64_t packetno;
  long block_size;        /* of the previous packet, for its successor's length */
  long counted_size;      /* likewise for packet_frames, which runs ahead */
  ogg_int64_t granulepos; /* encoding: of the last packet written */
} priv_t;

static void make_packet(ogg_packet * op, unsigned char const * data, size_t size, ogg_int64_t packetno)
{
  memset(op, 0, sizeof(*op));
  op->packet = (unsigned char *)data;
  op->bytes = (long)size;
  op->b_o_s = !packetno;
  op->granulepos = -1;
  op->packetno = packetno;
}

/* The three header packets out of the private data. */
static int split_headers(lsx_codec_params_t const * params, unsigned char const * * data, size_t * sizes)
{
  unsigned char const * b = params->private_data;
  size_t pos = 1, total = 0;
  int i;

  if (params->private_size < 1 || b[0] != HEADERS - 1)
    return SOX_EOF;
  for (i = 0; i < HEADERS - 1; total += sizes[i++])
    for (sizes[i] = 0; ; ) {
      if (pos >= params->private_size)
        return SOX_EOF;
      sizes[i] += b[pos];
      if (b[pos++] < 255)
        break;
    }
  if (total > params->private_size - pos)
    return SOX_EOF;
  sizes[HEADERS - 1] = params->private_size - pos - total;
  for (i = 0; i < HEADERS; pos += sizes[i++])
    data[i] = b + pos;
  return SOX_SUCCESS;
}

static int start_decode(lsx_codec_t * codec)
{
  priv_t * p = codec->priv;
  lsx_codec_params_t * params = &codec->params;
  unsigned char const * data[HEADERS];
  size_t sizes[HEADERS];
  int i;

  vorbis_info_init(&p->vi);
  vorbis_comment_init(&p->vc);
  if (split_headers(params, data, sizes) != SOX_SUCCESS) {
    lsx_fail_errno(codec->ft, SOX_EHDR, "Vorbis private data holds no header packets");
    goto error;
  }
  for (i = 0; i < HEADERS; ++i) {
    ogg_packet op;

    make_packet(&op, data[i], sizes[i], p->packetno++);
    if (vorbis_synthesis_headerin(&p->vi, &p->vc, &op)) {
      lsx_fail_errno(codec->ft, SOX_EHDR, "invalid Vorbis header packet");
      goto error;
    }
  }
  if (vorbis_synthesis_init(&p->vd, &p->vi)) {
    lsx_fail_errno(codec->ft, SOX_EHDR, "libVorbis can't decode this stream");
    goto error;
  }
  vorbis_block_init(&p->vd, &p->vb);
  for (i = 0; i < p->vc.comments; ++i)
    sox_append_comment(&codec->ft->oob.comments, p->vc.user_comments[i]);
  params->rate = p->vi.rate;
  params->channels = (unsigned)p->vi.channels;
  params->encoding = SOX_ENCODING_VORBIS;
  params->bits_per_sample = 0;
  return SOX_SUCCESS;

error:
  vorbis_comment_clear(&p->vc);
  vorbis_info_clear(&p->vi);
  return SOX_EOF;
}

/* A Matroska codec delay counts the frames FFmpeg's decoder makes of the
 * first packet, which libvorbis makes none of. */
static int decode(lsx_codec_t * codec, unsigned char const * data, size_t size)
{
  priv_t * p = codec->priv;
  ogg_packet op;
  float * * pcm;
  int n;

  if (!data)
    return SOX_SUCCESS;
  make_packet(&op, data, size, p->packetno++);
  if (p->packetno == HEADERS + 1) {
    sox_uint64_t first = (sox_uint64_t)max(vorbis_packet_blocksize(&p->vi, &op), 0) / 2;

    codec->params.delay -= min(first, codec->params.delay);
  }
  if (vorbis_synthesis(&p->vb, &op) == 0)
    vorbis_synthesis_blockin(&p->vd, &p->vb);
  while ((n = vorbis_synthesis_pcmout(&p->vd, &pcm)) > 0) {
    unsigned channels = codec->params.channels, channel;
    sox_sample_t * out = lsx_codec_output(codec, (size_t)n * channels);
    int i;

    for (i = 0; i < n; ++i, out += channels)
      for (channel = 0; channel < channels; ++channel)
        out[channels > 2 && channels <= 8? lsx_vorbis_channel_from_sox[channels][channel] : channel] =
          SOX_FLOAT_32BIT_TO_SAMPLE(pcm[channel][i], codec->ft->clips);
    vorbis_synthesis_read(&p->vd, n);
  }
  return SOX_SUCCESS;
}

static size_t packet_frames(lsx_codec_t * codec, unsigned char const * data, size_t size)
{
  priv_t * p = codec->priv;
  ogg_packet op;
  long block_size, previous = p->counted_size;

  make_packet(&op, data, size, HEADERS);
  if ((block_size = vorbis_packet_blocksize(&p->vi, &op)) <= 0)
    return 0;
  p->counted_size = block_size;
  return previous? (size_t)(previous + block_size) / 4 : 0;
}

static void stop_decode(lsx_codec_t * codec)
{
  priv_t * p = codec->priv;

  vorbis_block_clear(&p->vb);
  vorbis_dsp_clear(&p->vd);
  vorbis_comment_clear(&p->vc);
  vorbis_info_clear(&p->vi);
}

/* SoX's comments, each made a NAME=value pair under the name Comment when it
 * is not one already. */
static void make_comments(sox_format_t * ft, vorbis_comment * vc)
{
  static char const prepend[] = "Comment=";
  int i;

  memset(vc, 0, sizeof(*vc));
  vc->comments = (int)sox_num_comments(ft->oob.comments);
  if (!vc->comments)
    return;
  vc->comment_lengths = lsx_calloc((size_t)vc->comments, sizeof(*vc->comment_lengths));
  vc->user_comments = lsx_calloc((size_t)vc->comments, sizeof(*vc->user_comments));
  for (i = 0; i < vc->comments; ++i) {
    char * text = lsx_calloc(strlen(prepend) + strlen(ft->oob.comments[i]) + 1, sizeof(*text));

    if (!strchr(ft->oob.comments[i], '='))
      strcpy(text, prepend);
    vc->user_comments[i] = strcat(text, ft->oob.comments[i]);
    vc->comment_lengths[i] = (int)strlen(text);
  }
}

/* The header packets laced into the private data. */
static void put_headers(lsx_codec_params_t * params, ogg_packet const * headers)
{
  unsigned char * b;
  size_t size = 1, pos = 1;
  int i;

  for (i = 0; i < HEADERS; ++i)
    size += (size_t)headers[i].bytes + (i < HEADERS - 1? (size_t)headers[i].bytes / 255 + 1 : 0);
  b = params->private_data = lsx_malloc(size);
  params->private_size = size;
  b[0] = HEADERS - 1;
  for (i = 0; i < HEADERS - 1; ++i) {
    size_t n;

    for (n = (size_t)headers[i].bytes; n >= 255; n -= 255)
      b[pos++] = 255;
    b[pos++] = (unsigned char)n;
  }
  for (i = 0; i < HEADERS; pos += (size_t)headers[i++].bytes)
    memcpy(b + pos, headers[i].packet, (size_t)headers[i].bytes);
}

/* Quality comes from -C, from -1 to 10, 3 if not given. */
static int start_encode(lsx_codec_t * codec)
{
  priv_t * p = codec->priv;
  lsx_codec_params_t * params = &codec->params;
  sox_format_t * ft = codec->ft;
  double quality = 3;
  ogg_packet headers[HEADERS];
  vorbis_comment vc;
  int status, i;

  if (ft->encoding.compression != HUGE_VAL) {
    if (ft->encoding.compression < -1 || ft->encoding.compression > 10) {
      lsx_fail_errno(ft, SOX_EINVAL, "Vorbis compression quality must be between -1 and 10");
      return SOX_EOF;
    }
    quality = ft->encoding.compression;
  }
  vorbis_info_init(&p->vi);
  if (vorbis_encode_init_vbr(&p->vi, (long)params->channels, (long)(params->rate + .5), quality / 10)) {
    lsx_fail_errno(ft, SOX_EFMT, "libVorbis cannot encode this sample-rate or # of channels");
    vorbis_info_clear(&p->vi);
    return SOX_EOF;
  }
  vorbis_analysis_init(&p->vd, &p->vi);
  vorbis_block_init(&p->vd, &p->vb);
  make_comments(ft, &vc);
  status = vorbis_analysis_headerout(&p->vd, &vc, &headers[0], &headers[1], &headers[2]);
  if (!status)
    put_headers(params, headers);
  for (i = 0; i < vc.comments; ++i)
    free(vc.user_comments[i]);
  free(vc.user_comments);
  free(vc.comment_lengths);
  if (status) {
    lsx_fail_errno(ft, SOX_EHDR, "Error writing header for Ogg Vorbis audio stream");
    vorbis_block_clear(&p->vb);
    vorbis_dsp_clear(&p->vd);
    vorbis_info_clear(&p->vi);
    return SOX_EOF;
  }
  params->delay = 0;
  return SOX_SUCCESS;
}

/* A packet's frames are the advance of its granule position, which for the
 * last is cut short of the block to end the stream on the last sample: there
 * the frames are what the block decodes to, and the rest is discard. */
static int encode(lsx_codec_t * codec, sox_sample_t const * buf, size_t len)
{
  priv_t * p = codec->priv;
  unsigned channels = codec->params.channels;
  size_t frames = len / channels, i;
  float * * buffer = vorbis_analysis_buffer(&p->vd, (int)frames);
  unsigned channel;
  ogg_packet op;

  for (i = 0; i < frames; ++i)
    for (channel = 0; channel < channels; ++channel)
      buffer[channel][i] = SOX_SAMPLE_TO_FLOAT_32BIT(buf[i * channels +
          (channels > 2 && channels <= 8? lsx_vorbis_channel_from_sox[channels][channel] : channel)],
          codec->ft->clips);
  vorbis_analysis_wrote(&p->vd, (int)frames);
  while (vorbis_analysis_blockout(&p->vd, &p->vb) == 1) {
    vorbis_analysis(&p->vb, NULL);
    vorbis_bitrate_addblock(&p->vb);
    while (vorbis_bitrate_flushpacket(&p->vd, &op)) {
      long block_size = vorbis_packet_blocksize(&p->vi, &op);
      lsx_packet_t packet;

      packet.data = op.packet;
      packet.size = (size_t)op.bytes;
      packet.frames = (size_t)(op.granulepos - p->granulepos);
      packet.skip = packet.discard = 0;
      if (op.e_o_s) {
        size_t frames = p->block_size? (size_t)(p->block_size + block_size) / 4 : 0;

        if (frames > packet.frames) {
          packet.discard = frames - packet.frames;
          packet.frames = frames;
        }
      }
      p->block_size = block_size;
      p->granulepos = op.granulepos;
      if (codec->write_packet(codec, &packet) != SOX_SUCCESS)
        return SOX_EOF;
    }
  }
  return SOX_SUCCESS;
}

static void stop_encode(lsx_codec_t * codec)
{
  priv_t * p = codec->priv;

  vorbis_block_clear(&p->vb);
  vorbis_dsp_clear(&p->vd);
  vorbis_info_clear(&p->vi);
}

lsx_codec_handler_t const * lsx_vorbis_codec_fn(void)
{
  static unsigned const write_formats[] = {SOX_ENCODING_VORBIS, 0, 0};
  static lsx_codec_handler_t const handler = {
    "vorbis", write_formats, NULL, sizeof(priv_t),
    start_decode, decode, packet_frames, stop_decode,
    start_encode, encode, stop_encode
  };
  return &handler;
}
