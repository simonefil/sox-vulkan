/* libSoX packet codecs: Musepack SV7 and SV8, decoded with libmpcdec, and
 * SV8 encoded with libmpcenc and libmpcpsy.
 *
 * The encoding loop, which those libraries leave to their program, comes
 * from mpcenc.c of musepack_src r475:
 * Copyright (c) 2005-2009, The Musepack Development Team
 * Copyright (C) 1999-2004 Buschmann/Klemm/Piecha/Wolf
 * under the GNU Lesser General Public License, version 2.1 or later.
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

/* An SV7 packet is a frame: the bits of its first 32-bit word that precede
 * it, its length in bits excluded, a byte, three zero bytes, then the whole
 * 32-bit words of the file that hold it and the word after, which at the end
 * of the file may be missing; its private data is the five words of the file
 * header that follow the first.  An SV8 packet is the payload of an audio
 * packet block, and its private data the stream header after its CRC and
 * version: the sample counts and the two bytes of parameters.  The decoder
 * leaves out what precedes the signal and what follows it, so the container
 * gives no delay; SV7 is always stereo. */

#include "codec.h"

#include <string.h>
#include <mpc/mpcdec.h>

#ifdef HAVE_MPC_ENCODER
#include <math.h>
#include <mpc/libmpcenc.h>
#include <mpc/libmpcpsy.h>
#include <mpc/datatypes.h>
#include <mpc/minimax.h>
#include <mpc/mpcmath.h>
#endif

#define SV7_PRIVATE_SIZE 20
#define SV7_PACKET_HEADER_SIZE 4
#define READ_AHEAD 8        /* zero bytes after a packet, for the bit reader */

static sox_uint32_t const rates[] = {44100, 48000, 37800, 32000};

#ifdef HAVE_MPC_ENCODER

#define BLOCK_POWER 6       /* 64 frames a block, as mpcenc writes */
#define SAMPLES_SIZE 8      /* bytes the sample count is written in */
#define DECODER_DELAY 481
#define LAST_HUFFMAN 7
#define SCFfac 0.832980664785f
#define DENORMAL_FIX_LEFT (32. * 1024. / (float)(1 << 24))
#define DENORMAL_FIX_RIGHT (DENORMAL_FIX_LEFT * 0.5f)

typedef float SCFTriple[3];

/* What the libraries export without declaring it in their headers. */
void Init_Psychoakustik(PsyModel *);
void Init_Psychoakustiktabellen(PsyModel *);
void SetQualityParams(PsyModel *, float);
SMRTyp Psychoakustisches_Modell(PsyModel *, const int, const PCMDataTyp *, int * TransientL, int * TransientR);
void TransientenCalc(int * Transient, const int * TransientL, const int * TransientR);
void RaiseSMR(PsyModel *, const int, SMRTyp *);
void MS_LR_Entscheidung(const int, unsigned char * MS, SMRTyp *, SubbandFloatTyp *);
void NS_Analyse(PsyModel *, const int, const unsigned char * MS, const SMRTyp, const int * Transient);
void Analyse_Filter(const PCMDataTyp *, SubbandFloatTyp *, const int);
void Analyse_Init(float Left, float Right, SubbandFloatTyp * out, const int MaxBand);
extern float __invSCF[128 + 6];
#define invSCF (__invSCF + 6)
float ISNR_Schaetzer(const float * samples, const float comp, const int res);
float ISNR_Schaetzer_Trans(const float * samples, const float comp, const int res);
void QuantizeSubband(mpc_int16_t * qu_output, const float * input, const int res, float * errors,
    const int maxNsOrder);
void QuantizeSubbandWithNoiseShaping(mpc_int16_t * qu_output, const float * input, const int res,
    float * errors, const float * FIR);

/* The state of the libraries is partly their own, so they take one stream
 * at a time. */
static sox_bool encoder_busy;

#endif

typedef struct {
  mpc_decoder * decoder;
  unsigned block_frames;    /* SV8: frames of a full audio packet */
  unsigned char * bits;     /* the packet as the bit reader takes it */
  size_t bits_size;
  MPC_SAMPLE_FORMAT frame[MPC_DECODER_BUFFER_LENGTH];
  sox_bool ended;
#ifdef HAVE_MPC_ENCODER
  sox_bool encoding;
  PsyModel m;
  mpc_encoder_t e;
  PCMDataTyp main;          /* the frame being filled, after what precedes it */
  SubbandFloatTyp x[32];
  float power_l[32][3], power_r[32][3];
  float error_l[32][36 + MAX_NS_ORDER], error_r[32][36 + MAX_NS_ORDER];
  size_t filled;            /* samples of the frame */
  sox_bool silence, old_silence;
  sox_uint64_t frames;      /* encoded */
  sox_uint64_t samples;     /* taken in */
  sox_uint64_t last_frame;  /* known once flushing, else 0 */
#endif
} priv_t;

static sox_uint32_t get_le32(unsigned char const * b)
{
  return b[0] | (sox_uint32_t)b[1] << 8 | (sox_uint32_t)b[2] << 16 | (sox_uint32_t)b[3] << 24;
}

/* A count as SV8 writes it, seven bits a byte, most significant first; the
 * bytes it took, or 0 if it runs past the end. */
static size_t get_count(unsigned char const * b, size_t size, sox_uint64_t * value)
{
  size_t n = 0;

  *value = 0;
  do {
    if (n == size || n == 9)
      return 0;
    *value = *value << 7 | (b[n] & 0x7f);
  } while (b[n++] & 0x80);
  return n;
}

static int start_decoder(lsx_codec_t * codec, mpc_streaminfo * si)
{
  priv_t * p = codec->priv;

  if (!si->max_band || si->max_band >= 32 || !si->channels || si->channels > 2) {
    lsx_fail_errno(codec->ft, SOX_EHDR, "invalid Musepack stream header");
    return SOX_EOF;
  }
  p->decoder = mpc_decoder_init(si);
  if (!p->decoder) {
    lsx_fail_errno(codec->ft, SOX_ENOMEM, "can't start the Musepack decoder");
    return SOX_EOF;
  }
  return SOX_SUCCESS;
}

/* The fields of the header as libmpcdec reads them, each word from its most
 * significant bit. */
static int start_decode7(lsx_codec_t * codec)
{
  unsigned char const * h = codec->params.private_data;
  mpc_streaminfo si;
  sox_uint32_t frames, parameters, gapless, last;

  if (codec->params.private_size != SV7_PRIVATE_SIZE) {
    lsx_fail_errno(codec->ft, SOX_EHDR, "Musepack header missing");
    return SOX_EOF;
  }
  memset(&si, 0, sizeof(si));
  frames = get_le32(h);
  parameters = get_le32(h + 4);
  gapless = get_le32(h + 16);
  last = gapless >> 20 & 0x7ff;
  si.stream_version = 7;
  si.channels = 2;
  si.ms = parameters >> 30 & 1;
  si.max_band = parameters >> 24 & 0x3f;
  si.sample_freq = rates[parameters >> 16 & 3];
  si.is_true_gapless = gapless >> 31;
  if (last > MPC_FRAME_LENGTH) {
    lsx_fail_errno(codec->ft, SOX_EHDR, "invalid Musepack stream header");
    return SOX_EOF;
  }
  si.samples = (mpc_uint64_t)frames * MPC_FRAME_LENGTH -
      (si.is_true_gapless? MPC_FRAME_LENGTH - (last? last : MPC_FRAME_LENGTH) : MPC_DECODER_SYNTH_DELAY);
  return start_decoder(codec, &si);
}

static int start_decode8(lsx_codec_t * codec)
{
  priv_t * p = codec->priv;
  unsigned char const * h = codec->params.private_data;
  size_t size = codec->params.private_size, n, m;
  mpc_streaminfo si;

  memset(&si, 0, sizeof(si));
  n = get_count(h, size, &si.samples);
  m = n? get_count(h + n, size - n, &si.beg_silence) : 0;
  if (!m || size != n + m + 2 || si.beg_silence > si.samples || h[n + m] >> 5 >= array_length(rates)) {
    lsx_fail_errno(codec->ft, SOX_EHDR, "invalid Musepack stream header");
    return SOX_EOF;
  }
  h += n + m;
  si.stream_version = 8;
  si.is_true_gapless = 1;
  si.sample_freq = rates[h[0] >> 5];
  si.max_band = (h[0] & 0x1f) + 1u;
  si.channels = (h[1] >> 4) + 1u;
  si.ms = h[1] >> 3 & 1;
  si.block_pwr = (h[1] & 7) * 2u;
  p->block_frames = 1u << si.block_pwr;
  return start_decoder(codec, &si);
}

/* The packet as the bit reader takes it, with what it may read beyond. */
static unsigned char * copy_bits(priv_t * p, unsigned char const * data, size_t size)
{
  if (size + READ_AHEAD > p->bits_size) {
    p->bits_size = size + READ_AHEAD;
    p->bits = lsx_realloc(p->bits, p->bits_size);
  }
  memcpy(p->bits, data, size);
  memset(p->bits + size, 0, READ_AHEAD);
  return p->bits;
}

/* The frame the reader is at; 0 once the decoder has given the whole
 * signal. */
static int decode_frame(lsx_codec_t * codec, mpc_bits_reader * reader, unsigned char const * end, sox_bool key)
{
  priv_t * p = codec->priv;
  mpc_frame_info frame;
  sox_sample_t * out;
  size_t i;

  frame.buffer = p->frame;
  frame.is_key_frame = key;
  mpc_decoder_decode_frame(p->decoder, reader, &frame);
  if (frame.bits == -1) {
    p->ended = sox_true;
    return 0;
  }
  if (reader->buff + ((8 - reader->count) >> 3) > end) {
    lsx_fail_errno(codec->ft, SOX_EOF, "Musepack frame runs past its packet");
    return SOX_EOF;
  }
  out = lsx_codec_output(codec, frame.samples * codec->params.channels);
  for (i = 0; i < frame.samples * codec->params.channels; ++i)
    out[i] = p->frame[i];
  return 1;
}

/* The words go most significant byte first, as the bit reader reads. */
static int decode7(lsx_codec_t * codec, unsigned char const * data, size_t size)
{
  priv_t * p = codec->priv;
  unsigned char * bits;
  mpc_bits_reader reader;
  size_t i;

  if (!data || p->ended)
    return SOX_SUCCESS;
  if (size < SV7_PACKET_HEADER_SIZE + 4 || data[0] > 52) {
    lsx_fail_errno(codec->ft, SOX_EOF, "invalid Musepack packet");
    return SOX_EOF;
  }
  bits = copy_bits(p, data + SV7_PACKET_HEADER_SIZE, size - SV7_PACKET_HEADER_SIZE);
  for (i = 0; i + 4 <= size - SV7_PACKET_HEADER_SIZE; i += 4) {
    unsigned char t = bits[i];

    bits[i] = bits[i + 3];
    bits[i + 3] = t;
    t = bits[i + 1];
    bits[i + 1] = bits[i + 2];
    bits[i + 2] = t;
  }
  reader.buff = bits + data[0] / 8;
  reader.count = 8 - data[0] % 8;
  return decode_frame(codec, &reader, bits + size - SV7_PACKET_HEADER_SIZE, sox_false) == SOX_EOF?
      SOX_EOF : SOX_SUCCESS;
}

static int decode8(lsx_codec_t * codec, unsigned char const * data, size_t size)
{
  priv_t * p = codec->priv;
  mpc_bits_reader reader;
  unsigned i;
  int result = 1;

  if (!data || p->ended)
    return SOX_SUCCESS;
  reader.buff = copy_bits(p, data, size);
  reader.count = 8;
  for (i = 0; i < p->block_frames && result == 1; ++i)
    result = decode_frame(codec, &reader, p->bits + size, !i);
  return result == SOX_EOF? SOX_EOF : SOX_SUCCESS;
}

static void stop_decode(lsx_codec_t * codec)
{
  priv_t * p = codec->priv;

  mpc_decoder_exit(p->decoder);
  free(p->bits);
}

#ifdef HAVE_MPC_ENCODER

static unsigned char const Penalty[256] = {
  255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
  255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
  255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
  255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
  255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
  255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
  255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
  255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    0,  2,  5,  9, 15, 23, 36, 54, 79,116,169,246,255,255,255,255,
  255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
  255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
  255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
  255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
  255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
  255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
  255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
};

#define P(new, old) Penalty[128 + (old) - (new)]

/* In the order of the stream header's rate index. */
static sox_rate_t const write_rates[] = {44100, 48000, 37800, 32000, 0};

/* The scalefactors of a channel's three subframes in a band, and the
 * subband samples normalised by them. */
static void scalefactors(PsyModel * m, mpc_encoder_t * e, float power[32][3], int * scf, float * comp,
    float * x, int Band)
{
  int n, d01, d12, d02, warn = 0, compare[3];
  float tmp[3], fac;

  for (n = 0; n < 3; ++n) {
    float peak = FABS(x[12 * n]), sum = x[12 * n] * x[12 * n];
    int k;

    for (k = 12 * n + 1; k < 12 * n + 12; ++k) {
      if (peak < FABS(x[k]))
        peak = FABS(x[k]);
      sum += x[k] * x[k];
    }
    power[Band][n] = sum;
    tmp[n] = peak;
  }
  /* -12.6f*log10(x)+57.8945021823f = -10*log10(x/32767)*1.26+1, which
   * normalises a maximum of +/- 32767 against quantizer overflow. */
  for (n = 0; n < 3; ++n)
    if (tmp[n] > 0.)
      scf[n] = IFLOORF(-12.6f * LOG10(tmp[n]) + 57.8945021823f);
  for (n = 0; n < 3; ++n) {
    if (scf[n] < -6) scf[n] = -6, warn = 1;
    if (scf[n] > 121) scf[n] = 121, warn = 1;
  }
  compare[0] = scf[0]; compare[1] = scf[1]; compare[2] = scf[2];

  /* Scalefactors of small differences are replaced with the smaller. */
  if (m->CombPenalities >= 0) {
    if      (P(scf[0],scf[1]) + P(scf[0],scf[2]) <= m->CombPenalities) scf[2] = scf[1] = scf[0];
    else if (P(scf[1],scf[0]) + P(scf[1],scf[2]) <= m->CombPenalities) scf[0] = scf[2] = scf[1];
    else if (P(scf[2],scf[0]) + P(scf[2],scf[1]) <= m->CombPenalities) scf[0] = scf[1] = scf[2];
    else if (P(scf[0],scf[1])                    <= m->CombPenalities) scf[1] = scf[0];
    else if (P(scf[1],scf[0])                    <= m->CombPenalities) scf[0] = scf[1];
    else if (P(scf[1],scf[2])                    <= m->CombPenalities) scf[2] = scf[1];
    else if (P(scf[2],scf[1])                    <= m->CombPenalities) scf[1] = scf[2];
  }
  else {
    d12 = scf[2] - scf[1];
    d01 = scf[1] - scf[0];
    d02 = scf[2] - scf[0];
    if      ( 0 < d12 && d12 < 5) scf[2] = scf[1];
    else if (-3 < d12 && d12 < 0) scf[1] = scf[2];
    else if ( 0 < d01 && d01 < 5) scf[1] = scf[0];
    else if (-3 < d01 && d01 < 0) scf[0] = scf[1];
    else if ( 0 < d02 && d02 < 4) scf[2] = scf[0];
    else if (-2 < d02 && d02 < 0) scf[0] = scf[2];
  }

  /* The SNR compensation, and the normalisation. */
  for (n = 0; n < 3; ++n)
    tmp[n] = invSCF[compare[n] - scf[n]];
  comp[Band] = (tmp[0] * tmp[0] + tmp[1] * tmp[1] + tmp[2] * tmp[2]) * 0.3333333333f;
  for (n = 0; n < 36; ++n) {
    if (n % 12 == 0)
      fac = invSCF[scf[n / 12]];
    x[n] *= fac;
  }

  /* Internal clipping is limited to +/- 32767. */
  if (warn)
    for (n = 0; n < 36; ++n) {
      if (x[n] > +32767.f) {
        e->Overflows++;
        x[n] = 32767.f;
      }
      else if (x[n] < -32767.f) {
        e->Overflows++;
        x[n] = -32767.f;
      }
    }
}

static int PNS_SCF(int * scf, float S0, float S1, float S2)
{
  if (S0 < 0.5 * S1 || S1 < 0.5 * S2 || S0 < 0.5 * S2)
    return 0;
  if (S1 < 0.25 * S0 || S2 < 0.25 * S1 || S2 < 0.25 * S0)
    return 0;
  if (S0 >= 0.8 * S1) {
    if (S0 >= 0.8 * S2 && S1 > 0.8 * S2)
      S0 = S1 = S2 = 0.33333333333f * (S0 + S1 + S2);
    else S0 = S1 = 0.5f * (S0 + S1);
  }
  else if (S1 >= 0.8 * S2)
    S1 = S2 = 0.5f * (S1 + S2);
  scf[0] = scf[1] = scf[2] = 63;
  S0 = sqrt(S0 / 12 * 4 / 1.2005080577484075047860806747022);
  S1 = sqrt(S1 / 12 * 4 / 1.2005080577484075047860806747022);
  S2 = sqrt(S2 / 12 * 4 / 1.2005080577484075047860806747022);
  if (S0 > 0.) scf[0] = IFLOORF(-12.6f * LOG10(S0) + 57.8945021823f);
  if (S1 > 0.) scf[1] = IFLOORF(-12.6f * LOG10(S1) + 57.8945021823f);
  if (S2 > 0.) scf[2] = IFLOORF(-12.6f * LOG10(S2) + 57.8945021823f);
  if (scf[0] & ~63) scf[0] = scf[0] > 63? 63 : 0;
  if (scf[1] & ~63) scf[1] = scf[1] > 63? 63 : 0;
  if (scf[2] & ~63) scf[2] = scf[2] > 63? 63 : 0;
  return 1;
}

/* The quantizer resolution of each band of a channel: the lowest that
 * meets the mask-to-noise ratio, or noise substitution; the scalefactors
 * are then lowered while the ratio holds. */
static void Allocate(const int MaxBand, int * res, float * x, int * scf, const float * comp, const float * smr,
    SCFTriple const * Pow, const int * Transient, const float PNS)
{
  int Band, k;
  float tmpMNR, save[36], MNR = 0;

  for (Band = 0; Band <= MaxBand; Band++, res++, comp++, smr++, scf += 3, x += 72) {
    if (Band > 0 && res[-1] < 3 && *smr >= 1. && *smr < Band * PNS &&
        PNS_SCF(scf, Pow[Band][0], Pow[Band][1], Pow[Band][2]))
      *res = -1;
    else for (MNR = *smr * 1.; MNR > 1. && *res != 15; )
      MNR = *smr * (Transient[Band]? ISNR_Schaetzer_Trans : ISNR_Schaetzer)(x, *comp, ++*res);

    if (*res > 0 && *res <= LAST_HUFFMAN && MNR < 1. && MNR > 0. && !Transient[Band])
      while (scf[0] > 0 && scf[1] > 0 && scf[2] > 0) {
        --scf[2]; --scf[1]; --scf[0];
        memcpy(save, x, sizeof save);
        for (k = 0; k < 36; k++)
          x[k] *= SCFfac;
        tmpMNR = *smr * (Transient[Band]? ISNR_Schaetzer_Trans : ISNR_Schaetzer)(x, *comp, *res);
        if (tmpMNR <= 1)
          MNR = tmpMNR;
        else {
          ++scf[0]; ++scf[1]; ++scf[2];
          memcpy(x, save, sizeof save);
          break;
        }
      }
  }
}

/* The left channel's errors stand in for the right's where the right is
 * quantized without noise shaping, as in mpcenc, whose files this keeps
 * to. */
static void Quantisierung(priv_t * p, const int MaxBand)
{
  PsyModel * m = &p->m;
  mpc_encoder_t * e = &p->e;
  int Band;

  for (Band = 0; Band <= MaxBand; Band++) {
    if (e->Res_L[Band] > 0) {
      if (m->NS_Order_L[Band] > 0)
        QuantizeSubbandWithNoiseShaping(e->Q[Band].L, p->x[Band].L, e->Res_L[Band], p->error_l[Band], m->FIR_L[Band]);
      else QuantizeSubband(e->Q[Band].L, p->x[Band].L, e->Res_L[Band], p->error_l[Band], MAX_NS_ORDER);
      memcpy(p->error_l[Band], p->error_l[Band] + 36, MAX_NS_ORDER * sizeof(**p->error_l));
    }
    if (e->Res_R[Band] > 0) {
      if (m->NS_Order_R[Band] > 0)
        QuantizeSubbandWithNoiseShaping(e->Q[Band].R, p->x[Band].R, e->Res_R[Band], p->error_r[Band], m->FIR_R[Band]);
      else QuantizeSubband(e->Q[Band].R, p->x[Band].R, e->Res_R[Band], p->error_l[Band], MAX_NS_ORDER);
      memcpy(p->error_r[Band], p->error_r[Band] + 36, MAX_NS_ORDER * sizeof(**p->error_l));
    }
  }
}

/* The sample count in the stream header, written eight bytes wide so that
 * it can be filled in at the end. */
static void put_samples(unsigned char * b, sox_uint64_t samples)
{
  int i;

  for (i = 0; i < SAMPLES_SIZE; ++i)
    b[i] = (unsigned char)((samples >> (7 * (SAMPLES_SIZE - 1 - i)) & 0x7f) | (i + 1 < SAMPLES_SIZE? 0x80 : 0));
}

/* -C is mpcenc's --quality, 0 to 10, which takes fractions; 5, its
 * standard profile, is its default.  Nothing precedes the signal. */
static int start_encode(lsx_codec_t * codec)
{
  priv_t * p = codec->priv;
  lsx_codec_params_t * params = &codec->params;
  sox_format_t * ft = codec->ft;
  double quality = ft->encoding.compression == HUGE_VAL? 5 : ft->encoding.compression;
  unsigned rate;

  if (quality < 0 || quality > 10) {
    lsx_fail_errno(ft, SOX_EINVAL, "Musepack quality must be from 0 to 10");
    return SOX_EOF;
  }
  for (rate = 0; params->rate != write_rates[rate]; ++rate);
  if (params->channels > 2) {
    lsx_fail_errno(ft, SOX_EFMT, "Musepack holds at most 2 channels");
    return SOX_EOF;
  }
  if (encoder_busy) {
    lsx_fail_errno(ft, SOX_EINVAL, "only one Musepack file can be written at a time");
    return SOX_EOF;
  }
  encoder_busy = p->encoding = sox_true;
  p->m.SCF_Index_L = (int *)p->e.SCF_Index_L;
  p->m.SCF_Index_R = (int *)p->e.SCF_Index_R;
  Init_FastMath();          /* the tables the psychoacoustic model looks up */
  Init_Psychoakustik(&p->m);
  p->m.SampleFreq = (float)params->rate;
  SetQualityParams(&p->m, (float)quality);
  mpc_encoder_init(&p->e, 0, BLOCK_POWER, 0);
  /* Blocks are closed here rather than by the library, which would write
   * them to a file of its own. */
  p->e.frames_per_block_pwr = 30;
  Init_Psychoakustiktabellen(&p->m);
  p->e.MS_Channelmode = p->m.MS_Channelmode;
  p->silence = sox_true;
  params->private_size = SAMPLES_SIZE + 3;
  params->private_data = lsx_malloc(params->private_size);
  put_samples(params->private_data, 0);
  params->private_data[SAMPLES_SIZE] = 0;
  params->private_data[SAMPLES_SIZE + 1] = (unsigned char)(rate << 5 | (p->m.Max_Band - 1));
  params->private_data[SAMPLES_SIZE + 2] = (unsigned char)((params->channels - 1) << 4 |
      (p->m.MS_Channelmode > 0) << 3 | BLOCK_POWER >> 1);
  params->delay = DECODER_DELAY;
  return SOX_SUCCESS;
}

/* The audio packet the frames encoded so far make, after the decoder's
 * delay and the signal, padding up to the end of the last frame. */
static int write_block(lsx_codec_t * codec)
{
  priv_t * p = codec->priv;
  mpc_encoder_t * e = &p->e;
  lsx_packet_t packet;
  int status;

  writeBits(e, 0, (8 - e->bitsCount) % 8);
  emptyBits(e);
  packet.data = e->buffer;
  packet.size = e->pos;
  packet.frames = (size_t)e->framesInBlock * BLOCK;
  packet.skip = 0;
  packet.discard = p->frames == p->last_frame? (size_t)(p->frames * BLOCK - DECODER_DELAY - p->samples) : 0;
  packet.duration = 0;
  status = codec->write_packet(codec, &packet);
  e->pos = 0;
  e->framesInBlock = 0;
  return status;
}

/* A frame from the samples in main, got of them taken in, the rest being
 * the last of them repeated, or for the first frame silence; a frame that
 * follows one of digital silence and is one itself is coded empty. */
static int encode_frame(lsx_codec_t * codec, size_t got)
{
  priv_t * p = codec->priv;
  PsyModel * m = &p->m;
  mpc_encoder_t * e = &p->e;
  PCMDataTyp * main = &p->main;
  int TransientL[PART_SHORT], TransientR[PART_SHORT], Transient[32], Band;
  size_t i;

  if (!p->frames) {
    if (got)
      for (i = 0; i < CENTER; ++i) {
        main->L[i] = main->L[CENTER];
        main->R[i] = main->R[CENTER];
        main->M[i] = main->M[CENTER];
        main->S[i] = main->S[CENTER];
      }
    Analyse_Init(main->L[CENTER], main->R[CENTER], p->x, m->Max_Band);
  }
  else for (i = CENTER + got; i < CENTER + BLOCK; ++i) {
    main->L[i] = main->L[CENTER + got - 1];
    main->R[i] = main->R[CENTER + got - 1];
    main->M[i] = main->M[CENTER + got - 1];
    main->S[i] = main->S[CENTER + got - 1];
  }
  memset(e->Res_L, 0, sizeof e->Res_L);
  memset(e->Res_R, 0, sizeof e->Res_R);
  if (!p->silence || !p->old_silence) {
    SMRTyp SMR;

    Analyse_Filter(main, p->x, m->Max_Band);
    SMR = Psychoakustisches_Modell(m, 31, main, TransientL, TransientR);
    if (m->minSMR > 0)
      RaiseSMR(m, m->Max_Band, &SMR);
    if (m->MS_Channelmode > 0)
      MS_LR_Entscheidung(m->Max_Band, e->MS_Flag, &SMR, p->x);
    for (Band = 0; Band <= m->Max_Band; ++Band) {
      scalefactors(m, e, p->power_l, e->SCF_Index_L[Band], m->SNR_comp_L, p->x[Band].L, Band);
      scalefactors(m, e, p->power_r, e->SCF_Index_R[Band], m->SNR_comp_R, p->x[Band].R, Band);
    }
    TransientenCalc(Transient, TransientL, TransientR);
    if (m->NS_Order > 0)
      NS_Analyse(m, m->Max_Band, e->MS_Flag, SMR, Transient);
    Allocate(m->Max_Band, e->Res_L, p->x[0].L, e->SCF_Index_L[0], m->SNR_comp_L, SMR.L,
        (SCFTriple const *)p->power_l, Transient, m->PNS);
    Allocate(m->Max_Band, e->Res_R, p->x[0].R, e->SCF_Index_R[0], m->SNR_comp_R, SMR.R,
        (SCFTriple const *)p->power_r, Transient, m->PNS);
    Quantisierung(p, m->Max_Band);
  }
  p->old_silence = p->silence;
  p->silence = sox_true;
  writeBitstream_SV8(e, m->Max_Band);
  ++p->frames;
  memmove(main->L, main->L + BLOCK, CENTER * sizeof(float));
  memmove(main->R, main->R + BLOCK, CENTER * sizeof(float));
  memmove(main->M, main->M + BLOCK, CENTER * sizeof(float));
  memmove(main->S, main->S + BLOCK, CENTER * sizeof(float));
  return e->framesInBlock == 1u << BLOCK_POWER? write_block(codec) : SOX_SUCCESS;
}

/* At the end, frames go on until the decoder's delay is covered, the last
 * block, short or not, goes out, and the samples are known. */
static int encode(lsx_codec_t * codec, sox_sample_t const * buf, size_t len)
{
  priv_t * p = codec->priv;
  PCMDataTyp * main = &p->main;
  unsigned channels = codec->params.channels;
  size_t i;

  if (!buf) {
    put_samples(codec->params.private_data, p->samples);
    p->last_frame = (p->samples + DECODER_DELAY + BLOCK - 1) / BLOCK;
    for (i = p->filled; p->frames < p->last_frame; i = 0)
      if (encode_frame(codec, i) != SOX_SUCCESS)
        return SOX_EOF;
    return p->e.framesInBlock? write_block(codec) : SOX_SUCCESS;
  }
  for (i = 0; i < len; i += channels) {
    size_t at = CENTER + p->filled;
    float left = (float)(32768 * SOX_SAMPLE_TO_FLOAT_32BIT(buf[i], ));
    float right = channels == 2? (float)(32768 * SOX_SAMPLE_TO_FLOAT_32BIT(buf[i + 1], )) : left;

    if (left != 0 || right != 0)
      p->silence = sox_false;
    main->L[at] = left + DENORMAL_FIX_LEFT;
    main->R[at] = right + DENORMAL_FIX_RIGHT;
    main->M[at] = (main->L[at] + main->R[at]) * 0.5f;
    main->S[at] = (main->L[at] - main->R[at]) * 0.5f;
    ++p->samples;
    if (++p->filled == BLOCK) {
      p->filled = 0;
      if (encode_frame(codec, BLOCK) != SOX_SUCCESS)
        return SOX_EOF;
    }
  }
  return SOX_SUCCESS;
}

static void stop_encode(lsx_codec_t * codec)
{
  priv_t * p = codec->priv;

  if (!p->encoding)
    return;
  if (p->e.Overflows)
    lsx_warn("Musepack encoder clipped %lu subband samples", (unsigned long)p->e.Overflows);
  mpc_encoder_exit(&p->e);
  encoder_busy = sox_false;
}

#define ENCODER start_encode, encode, stop_encode
#define WRITE_RATES write_rates
#else
#define ENCODER NULL, NULL, NULL
#define WRITE_RATES NULL
#endif

lsx_codec_handler_t const * lsx_mpc7_codec_fn(void)
{
  static lsx_codec_handler_t const handler = {
    "mpc7", NULL, NULL, sizeof(priv_t),
    start_decode7, decode7, NULL, stop_decode,
    NULL, NULL, NULL
  };

  return &handler;
}

lsx_codec_handler_t const * lsx_mpc8_codec_fn(void)
{
  static unsigned const formats[] = {SOX_ENCODING_MUSEPACK, 0, 0};
  static lsx_codec_handler_t const handler = {
    "mpc8", formats, WRITE_RATES, sizeof(priv_t),
    start_decode8, decode8, NULL, stop_decode,
    ENCODER
  };

  return &handler;
}
