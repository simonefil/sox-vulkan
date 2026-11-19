/* libSoX packet codec: FLAC, through libFLAC.
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

/* In a container a FLAC stream is cut in two.  The private data holds the
 * stream header, "fLaC" and the metadata blocks, STREAMINFO first; each
 * packet holds one frame.  libFLAC reads a stream, so the decoder is given
 * the header once and then each packet in turn through its read callback,
 * and the encoder's output is split back the other way: what it writes
 * before the first frame is the private data, and each frame is a packet. */

#include "codec.h"

#include <string.h>

#include <FLAC/all.h>

#define MAX_COMPRESSION 8
#define STREAMINFO_SIZE 34
#define HEADER_SIZE (4 + 4 + STREAMINFO_SIZE) /* "fLaC", block header, STREAMINFO */

typedef struct {
  /* Decoding */
  FLAC__StreamDecoder * decoder;
  unsigned char const * input;
  size_t input_size, input_pos;
  sox_bool failed;
  sox_bool drained;
  sox_bool started;   /* a frame has been decoded */
  sox_bool whole;     /* from the stream's first frame, which the MD5 checksum needs */

  /* Encoding */
  FLAC__StreamEncoder * encoder;
  FLAC__int32 * pcm;
  size_t pcm_size;
  sox_bool closing;
} priv_t;

static FLAC__StreamDecoderReadStatus decoder_read(FLAC__StreamDecoder const * decoder UNUSED,
    FLAC__byte buffer[], size_t * bytes, void * client_data)
{
  priv_t * p = ((lsx_codec_t *)client_data)->priv;
  size_t n = min(*bytes, p->input_size - p->input_pos);

  /* An empty answer must end the stream: libFLAC would ask again forever. */
  if (!n) {
    *bytes = 0;
    return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
  }
  memcpy(buffer, p->input + p->input_pos, n);
  p->input_pos += n;
  *bytes = n;
  return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

static FLAC__StreamDecoderWriteStatus decoder_write(FLAC__StreamDecoder const * decoder UNUSED,
    FLAC__Frame const * frame, FLAC__int32 const * const buffer[], void * client_data)
{
  lsx_codec_t * codec = client_data;
  lsx_codec_params_t const * params = &codec->params;
  unsigned bits = frame->header.bits_per_sample;
  unsigned i, channel;
  sox_sample_t * out;

  if (bits != params->bits_per_sample || frame->header.channels != params->channels ||
      frame->header.sample_rate != params->rate) {
    lsx_fail_errno(codec->ft, SOX_EFMT, "FLAC frame parameters differ from the stream's");
    ((priv_t *)codec->priv)->failed = sox_true;
    return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
  }
  if (!((priv_t *)codec->priv)->started) {
    ((priv_t *)codec->priv)->started = sox_true;
    ((priv_t *)codec->priv)->whole = !(frame->header.number_type == FLAC__FRAME_NUMBER_TYPE_SAMPLE_NUMBER?
        frame->header.number.sample_number : frame->header.number.frame_number);
  }
  out = lsx_codec_output(codec, (size_t)frame->header.blocksize * params->channels);
  for (i = 0; i < frame->header.blocksize; ++i)
    for (channel = 0; channel < params->channels; ++channel)
      *out++ = SOX_SIGNED_TO_SAMPLE(bits, buffer[channel][i]);
  return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void decoder_metadata(FLAC__StreamDecoder const * decoder UNUSED,
    FLAC__StreamMetadata const * metadata, void * client_data)
{
  lsx_codec_t * codec = client_data;
  lsx_codec_params_t * params = &codec->params;
  FLAC__uint32 i;

  if (metadata->type == FLAC__METADATA_TYPE_STREAMINFO) {
    params->rate = metadata->data.stream_info.sample_rate;
    params->channels = metadata->data.stream_info.channels;
    params->bits_per_sample = metadata->data.stream_info.bits_per_sample;
  }
  else if (metadata->type == FLAC__METADATA_TYPE_VORBIS_COMMENT)
    for (i = 0; i < metadata->data.vorbis_comment.num_comments; ++i)
      sox_append_comment(&codec->ft->oob.comments,
          (char const *)metadata->data.vorbis_comment.comments[i].entry);
}

static void decoder_error(FLAC__StreamDecoder const * decoder UNUSED,
    FLAC__StreamDecoderErrorStatus status, void * client_data)
{
  lsx_codec_t * codec = client_data;

  lsx_fail_errno(codec->ft, SOX_EFMT, "FLAC: %s", FLAC__StreamDecoderErrorStatusString[status]);
  ((priv_t *)codec->priv)->failed = sox_true;
}

/* The stream header to give the decoder.  The private data normally is one
 * already, but some muxers store only the bare STREAMINFO.  The last-block
 * flag is set on whatever block ends the private data, since libFLAC would
 * otherwise take the first frame for one more metadata block. */
static unsigned char * stream_header(lsx_codec_params_t const * params, size_t * size)
{
  unsigned char * header;
  size_t pos;

  if (params->private_size == STREAMINFO_SIZE) {
    header = lsx_malloc(HEADER_SIZE);
    memcpy(header, "fLaC\x80\0\0\x22", 8);
    memcpy(header + 8, params->private_data, STREAMINFO_SIZE);
    *size = HEADER_SIZE;
    return header;
  }
  if (params->private_size < HEADER_SIZE || memcmp(params->private_data, "fLaC", 4))
    return NULL;
  header = lsx_memdup(params->private_data, params->private_size);
  for (pos = 4; pos + 4 <= params->private_size; ) {
    size_t next = pos + 4 + (((size_t)header[pos + 1] << 16) | (header[pos + 2] << 8) | header[pos + 3]);

    if (next >= params->private_size || (header[pos] & 0x80)) {
      header[pos] |= 0x80;
      *size = min(next, params->private_size);
      return header;
    }
    pos = next;
  }
  free(header);
  return NULL;
}

static int start_decode(lsx_codec_t * codec)
{
  priv_t * p = codec->priv;
  unsigned char * header;
  size_t header_size;
  sox_bool ok;

  if (!(header = stream_header(&codec->params, &header_size))) {
    lsx_fail_errno(codec->ft, SOX_EHDR, "FLAC private data holds no stream header");
    return SOX_EOF;
  }
  if (!(p->decoder = FLAC__stream_decoder_new())) {
    free(header);
    lsx_fail_errno(codec->ft, SOX_ENOMEM, "FLAC: can't create the decoder");
    return SOX_EOF;
  }
  FLAC__stream_decoder_set_md5_checking(p->decoder, sox_true);
  FLAC__stream_decoder_set_metadata_respond(p->decoder, FLAC__METADATA_TYPE_VORBIS_COMMENT);
  p->input = header;
  p->input_size = header_size;
  ok = FLAC__stream_decoder_init_stream(p->decoder, decoder_read, NULL, NULL, NULL, NULL,
      decoder_write, decoder_metadata, decoder_error, codec) == FLAC__STREAM_DECODER_INIT_STATUS_OK &&
    FLAC__stream_decoder_process_until_end_of_metadata(p->decoder) && !p->failed &&
    FLAC__stream_decoder_get_state(p->decoder) == FLAC__STREAM_DECODER_SEARCH_FOR_FRAME_SYNC;
  free(header);
  p->input = NULL;
  p->input_size = p->input_pos = 0;
  if (!ok) {
    if (!p->failed)
      lsx_fail_errno(codec->ft, SOX_EHDR, "FLAC: can't read the stream header");
    FLAC__stream_decoder_delete(p->decoder);
    return SOX_EOF;
  }
  codec->params.encoding = SOX_ENCODING_FLAC;
  return SOX_SUCCESS;
}

static int decode(lsx_codec_t * codec, unsigned char const * data, size_t size)
{
  priv_t * p = codec->priv;

  p->input = data;
  p->input_size = size;
  p->input_pos = 0;
  if (!data) {
    /* A frame may still sit in libFLAC's own buffer if a packet held more
     * than one; run until the empty input ends the stream. */
    size_t before;

    do before = codec->samples_end;
    while (FLAC__stream_decoder_process_single(p->decoder) && !p->failed &&
        FLAC__stream_decoder_get_state(p->decoder) != FLAC__STREAM_DECODER_END_OF_STREAM &&
        codec->samples_end != before);
    p->drained = sox_true;
  }
  else while (p->input_pos < p->input_size) {
    if (!FLAC__stream_decoder_process_single(p->decoder)) {
      if (!p->failed)
        lsx_fail_errno(codec->ft, SOX_EFMT, "FLAC: %s",
            FLAC__StreamDecoderStateString[FLAC__stream_decoder_get_state(p->decoder)]);
      p->failed = sox_true;
    }
    if (p->failed)
      break;
    if (FLAC__stream_decoder_get_state(p->decoder) == FLAC__STREAM_DECODER_END_OF_STREAM) {
      lsx_fail_errno(codec->ft, SOX_EFMT, "FLAC frame cut short");
      p->failed = sox_true;
    }
  }
  p->input = NULL;
  return p->failed? SOX_EOF : SOX_SUCCESS;
}

/* The block size a frame header gives, which is all a frame header says
 * about how long the frame is. */
static size_t packet_frames(lsx_codec_t * codec UNUSED, unsigned char const * data, size_t size)
{
  unsigned code, pos = 5;

  if (size < 6 || data[0] != 0xff || (data[1] & 0xfe) != 0xf8)
    return 0;
  for (code = data[4]; code & 0x80 && pos < 11; code <<= 1)
    ++pos; /* the frame or sample number, UTF-8 coded */
  if (data[4] & 0x80)
    --pos;
  code = data[2] >> 4;
  if (code == 1)
    return 192;
  if (code >= 2 && code <= 5)
    return (size_t)576 << (code - 2);
  if (code >= 8)
    return (size_t)256 << (code - 8);
  if (code == 6 && size > pos)
    return data[pos] + 1u;
  if (code == 7 && size > pos + 1)
    return (((size_t)data[pos] << 8) | data[pos + 1]) + 1;
  return 0;
}

static void stop_decode(lsx_codec_t * codec)
{
  priv_t * p = codec->priv;

  if (!FLAC__stream_decoder_finish(p->decoder) && p->drained && p->whole && !p->failed)
    lsx_warn("decoder MD5 checksum mismatch.");
  FLAC__stream_decoder_delete(p->decoder);
}

static FLAC__StreamEncoderWriteStatus encoder_write(FLAC__StreamEncoder const * encoder UNUSED,
    FLAC__byte const buffer[], size_t bytes, unsigned samples, unsigned current_frame UNUSED,
    void * client_data)
{
  lsx_codec_t * codec = client_data;
  lsx_codec_params_t * params = &codec->params;
  lsx_packet_t packet;

  if (((priv_t *)codec->priv)->closing)
    return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
  if (!samples) { /* the stream header, written while the encoder starts */
    params->private_data = lsx_realloc(params->private_data, params->private_size + bytes);
    memcpy(params->private_data + params->private_size, buffer, bytes);
    params->private_size += bytes;
    return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
  }
  packet.data = buffer;
  packet.size = bytes;
  packet.frames = samples;
  packet.discard = 0;
  return codec->write_packet(codec, &packet) == SOX_SUCCESS?
    FLAC__STREAM_ENCODER_WRITE_STATUS_OK : FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
}

/* The final STREAMINFO, with the sample count, frame sizes and MD5 that
 * only the finished stream has, replaces the one in the private data. */
static void encoder_metadata(FLAC__StreamEncoder const * encoder UNUSED,
    FLAC__StreamMetadata const * metadata, void * client_data)
{
  lsx_codec_params_t * params = &((lsx_codec_t *)client_data)->params;
  FLAC__StreamMetadata_StreamInfo const * info = &metadata->data.stream_info;
  unsigned char * s = params->private_data + 8;
  sox_uint64_t packed;
  int i;

  if (metadata->type != FLAC__METADATA_TYPE_STREAMINFO || params->private_size < HEADER_SIZE)
    return;
  s[0] = info->min_blocksize >> 8; s[1] = info->min_blocksize;
  s[2] = info->max_blocksize >> 8; s[3] = info->max_blocksize;
  s[4] = info->min_framesize >> 16; s[5] = info->min_framesize >> 8; s[6] = info->min_framesize;
  s[7] = info->max_framesize >> 16; s[8] = info->max_framesize >> 8; s[9] = info->max_framesize;
  packed = (sox_uint64_t)info->sample_rate << 44 | (sox_uint64_t)(info->channels - 1) << 41 |
      (sox_uint64_t)(info->bits_per_sample - 1) << 36 | (info->total_samples & 0xfffffffffull);
  for (i = 0; i < 8; ++i)
    s[10 + i] = (unsigned char)(packed >> (56 - 8 * i));
  memcpy(s + 18, info->md5sum, 16);
}

static int start_encode(lsx_codec_t * codec)
{
  priv_t * p = codec->priv;
  lsx_codec_params_t * params = &codec->params;
  sox_format_t * ft = codec->ft;
  unsigned compression_level = MAX_COMPRESSION;
  FLAC__StreamEncoderInitStatus status;
  static unsigned const streamable_rates[] =
    {8000, 16000, 22050, 24000, 32000, 44100, 48000, 96000};
  sox_bool streamable = sox_false;
  size_t i;

  if (ft->encoding.compression != HUGE_VAL) {
    compression_level = ft->encoding.compression;
    if (compression_level != ft->encoding.compression || compression_level > MAX_COMPRESSION) {
      lsx_fail_errno(ft, SOX_EINVAL,
          "FLAC compression level must be a whole number from 0 to %i", MAX_COMPRESSION);
      return SOX_EOF;
    }
  }
  if (!(p->encoder = FLAC__stream_encoder_new())) {
    lsx_fail_errno(ft, SOX_ENOMEM, "FLAC: can't create the encoder");
    return SOX_EOF;
  }
  FLAC__stream_encoder_set_channels(p->encoder, params->channels);
  FLAC__stream_encoder_set_bits_per_sample(p->encoder, params->bits_per_sample);
  FLAC__stream_encoder_set_sample_rate(p->encoder, (unsigned)(params->rate + .5));
  FLAC__stream_encoder_set_compression_level(p->encoder, compression_level);
  for (i = 0; !streamable && i < array_length(streamable_rates); ++i)
    streamable = streamable_rates[i] == params->rate;
  if (!streamable) {
    lsx_report("non-standard rate; output may not be streamable");
    FLAC__stream_encoder_set_streamable_subset(p->encoder, sox_false);
  }
  status = FLAC__stream_encoder_init_stream(p->encoder, encoder_write, NULL, NULL,
      encoder_metadata, codec);
  if (status != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
    lsx_fail_errno(ft, SOX_EINVAL, "FLAC: %s", FLAC__StreamEncoderInitStatusString[status]);
    FLAC__stream_encoder_delete(p->encoder);
    return SOX_EOF;
  }
  return SOX_SUCCESS;
}

static int encode(lsx_codec_t * codec, sox_sample_t const * buf, size_t len)
{
  priv_t * p = codec->priv;
  sox_format_t * ft = codec->ft;
  size_t i;

  if (!buf) {
    if (FLAC__stream_encoder_finish(p->encoder))
      return SOX_SUCCESS;
    lsx_fail_errno(ft, SOX_EINVAL, "FLAC: failed to encode to end of stream");
    return SOX_EOF;
  }
  if (p->pcm_size < len) {
    p->pcm_size = len;
    free(p->pcm);
    p->pcm = lsx_malloc(len * sizeof(*p->pcm));
  }
  for (i = 0; i < len; ++i) {
    SOX_SAMPLE_LOCALS;

    switch (codec->params.bits_per_sample) {
      case  8: p->pcm[i] = SOX_SAMPLE_TO_SIGNED_8BIT(buf[i], ft->clips); break;
      case 16: p->pcm[i] = SOX_SAMPLE_TO_SIGNED_16BIT(buf[i], ft->clips); break;
      default: p->pcm[i] = SOX_SAMPLE_TO_SIGNED_24BIT(buf[i], ft->clips); break;
    }
  }
  if (FLAC__stream_encoder_process_interleaved(p->encoder, p->pcm, (unsigned)(len / codec->params.channels)))
    return SOX_SUCCESS;
  lsx_fail_errno(ft, SOX_EINVAL, "FLAC: %s",
      FLAC__StreamEncoderStateString[FLAC__stream_encoder_get_state(p->encoder)]);
  return SOX_EOF;
}

/* Deleting an encoder that was not finished finishes it, and the frames
 * that produces have nowhere to go by now. */
static void stop_encode(lsx_codec_t * codec)
{
  priv_t * p = codec->priv;

  p->closing = sox_true;
  FLAC__stream_encoder_delete(p->encoder);
  free(p->pcm);
}

lsx_codec_handler_t const * lsx_flac_codec_fn(void)
{
  static unsigned const write_formats[] = {SOX_ENCODING_FLAC, 8, 16, 24, 0, 0};
  static lsx_codec_handler_t const handler = {
    "flac", write_formats, NULL, sizeof(priv_t),
    start_decode, decode, packet_frames, stop_decode,
    start_encode, encode, stop_encode
  };
  return &handler;
}
