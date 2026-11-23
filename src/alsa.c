/* libSoX device driver: ALSA   (c) 2006-2012 SoX contributors
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
#include <alsa/asoundlib.h>

typedef struct {
  snd_pcm_uframes_t  buf_len, period;
  snd_pcm_t          * pcm;
  char               * buf;
  unsigned int       format;
  unsigned int       device_channels;
  lsx_route_t        * route;   /* NULL when the channels go as they are */
  sox_sample_t       * frames;  /* buf_len samples in the device's order */
} priv_t;

static const
  struct {
    unsigned int bits;
    enum _snd_pcm_format alsa_fmt;
    unsigned int bytes; /* occupied in the buffer per sample */
    sox_encoding_t enc;
  } formats[] = {
    /* order by # of bits; within that, preferred first */
    {  8, SND_PCM_FORMAT_S8, 1, SOX_ENCODING_SIGN2 },
    {  8, SND_PCM_FORMAT_U8, 1, SOX_ENCODING_UNSIGNED },
    { 16, SND_PCM_FORMAT_S16, 2, SOX_ENCODING_SIGN2 },
    { 16, SND_PCM_FORMAT_U16, 2, SOX_ENCODING_UNSIGNED },
    { 24, SND_PCM_FORMAT_S24, 4, SOX_ENCODING_SIGN2 },
    { 24, SND_PCM_FORMAT_U24, 4, SOX_ENCODING_UNSIGNED },
    { 24, SND_PCM_FORMAT_S24_3LE, 3, SOX_ENCODING_SIGN2 },
    { 32, SND_PCM_FORMAT_S32, 4, SOX_ENCODING_SIGN2 },
    { 32, SND_PCM_FORMAT_U32, 4, SOX_ENCODING_UNSIGNED },
    {  0, 0, 0, SOX_ENCODING_UNKNOWN } /* end of list */
  };

static int select_format(
    sox_encoding_t              * encoding_,
    unsigned                    * nbits_,
    snd_pcm_format_mask_t const * mask,
    unsigned int                * format)
{
  unsigned int from = 0, to; /* NB: "to" actually points one after the last */
  int cand = -1;

  while (formats[from].bits < *nbits_ && formats[from].bits != 0)
    from++;  /* find the first entry with at least *nbits_ bits */
  for (to = from; formats[to].bits != 0; to++) ;  /* find end of list */

  while (to > 0) {
    unsigned int i, bits_next = 0;
    for (i = from; i < to; i++) {
      lsx_debug_most("select_format: trying #%u", i);
      if (snd_pcm_format_mask_test(mask, formats[i].alsa_fmt)) {
        if (formats[i].enc == *encoding_) {
          cand = i;
          break; /* found a match */
        } else if (cand == -1) /* don't overwrite a candidate that
                                       was earlier in the list */
          cand = i; /* will work, but encoding differs */
      }
    }
    if (cand != -1)
      break;
    /* no candidate found yet; now try formats with less bits: */
    to = from;
    if (from > 0)
      bits_next = formats[from-1].bits;
    while (from && formats[from-1].bits == bits_next)
      from--; /* go back to the first entry with bits_next bits */
  }

  if (cand == -1) {
    lsx_debug("select_format: no suitable ALSA format found");
    return -1;
  }

  if (*nbits_ != formats[cand].bits || *encoding_ != formats[cand].enc) {
    lsx_warn("can't encode %u-bit %s", *nbits_,
        sox_encodings_info[*encoding_].desc);
    *nbits_ = formats[cand].bits;
    *encoding_ = formats[cand].enc;
  }
  lsx_debug("selecting format %d: %s (%s)", cand,
      snd_pcm_format_name(formats[cand].alsa_fmt),
      snd_pcm_format_description(formats[cand].alsa_fmt));
  *format = cand;
  return 0;
}

/* ALSA's order for a device without a channel map: snd_pcm_std_chmaps' in
 * the kernel, which the pulse and pipewire plugins and speaker-test follow. */
static lsx_speaker_t const * default_speakers(unsigned channels)
{
  #define _(x) lsx_speaker_##x
  static lsx_speaker_t const speakers[4][8] = {
    {_(fl), _(fr)},
    {_(fl), _(fr), _(bl), _(br)},
    {_(fl), _(fr), _(bl), _(br), _(fc), _(lfe)},
    {_(fl), _(fr), _(bl), _(br), _(fc), _(lfe), _(sl), _(sr)}};
  #undef _
  return channels >= 2 && channels <= 8 && !(channels & 1)?
    speakers[channels / 2 - 1] : NULL;
}

static void chmap_speakers(snd_pcm_chmap_t const * map, lsx_speaker_t * speakers)
{
  unsigned i;

  for (i = 0; i < map->channels; ++i)
    switch (map->pos[i] & SND_CHMAP_POSITION_MASK) {
      case SND_CHMAP_MONO:
      case SND_CHMAP_FC:  speakers[i] = lsx_speaker_fc;   break;
      case SND_CHMAP_FL:  speakers[i] = lsx_speaker_fl;   break;
      case SND_CHMAP_FR:  speakers[i] = lsx_speaker_fr;   break;
      case SND_CHMAP_LFE: speakers[i] = lsx_speaker_lfe;  break;
      case SND_CHMAP_RL:  speakers[i] = lsx_speaker_bl;   break;
      case SND_CHMAP_RR:  speakers[i] = lsx_speaker_br;   break;
      case SND_CHMAP_RC:  speakers[i] = lsx_speaker_bc;   break;
      case SND_CHMAP_SL:  speakers[i] = lsx_speaker_sl;   break;
      case SND_CHMAP_SR:  speakers[i] = lsx_speaker_sr;   break;
      default:            speakers[i] = lsx_speaker_none; break;
    }
}

/* The fewest device channels that have a speaker for each of the signal's,
 * among the channel maps the device offers or, if it offers none, ALSA's
 * default ones; 0 if there are none. */
static unsigned choose_channels(snd_pcm_t * pcm, snd_pcm_hw_params_t * params,
    unsigned channels, sox_bool mix)
{
  snd_pcm_chmap_query_t ** maps = snd_pcm_query_chmaps(pcm);
  lsx_speaker_t speakers[64];
  lsx_route_t route[64];
  unsigned best = 0, n, i;

  for (i = 0; maps? maps[i] != NULL : i < 4; ++i) {
    if (maps) {
      n = maps[i]->map.channels;
      if (n > 64)
        continue;
      chmap_speakers(&maps[i]->map, speakers);
    }
    else {
      n = 2 * (i + 1);
      memcpy(speakers, default_speakers(n), n * sizeof(*speakers));
    }
    if ((!best || n < best) &&
        lsx_route_speakers(route, speakers, n, channels, mix) &&
        snd_pcm_hw_params_test_channels(pcm, params, n) == 0)
      best = n;
  }
  if (maps)
    snd_pcm_free_chmaps(maps);
  return best;
}

/* Places the signal's channels on the device's speakers, read from its
 * channel map or, without one, from ALSA's default order.  A device with too
 * few speakers for the signal is given the signal mixed to its channel count,
 * as before, by the effect that sox adds for a change of channels. */
static void route_channels(sox_format_t * ft)
{
  priv_t * p = (priv_t *)ft->priv;
  unsigned n = p->device_channels, i;
  snd_pcm_chmap_t * map = snd_pcm_get_chmap(p->pcm);
  lsx_speaker_t * speakers = lsx_calloc(n, sizeof(*speakers));
  sox_bool known = sox_true;

  if (map && map->channels == n)
    chmap_speakers(map, speakers);
  else if (default_speakers(n))
    memcpy(speakers, default_speakers(n), n * sizeof(*speakers));
  else known = sox_false;
  free(map);

  p->route = lsx_malloc(n * sizeof(*p->route));
  if (!known || !lsx_route_speakers(p->route, speakers, n, ft->signal.channels, ft->mode == 'w')) {
    if (n == ft->signal.channels)
      lsx_warn("no speaker positions for the %u channels on this device: they keep SoX's order", n);
    ft->signal.channels = n;
    if (!known || !lsx_route_speakers(p->route, speakers, n, n, ft->mode == 'w'))
      for (i = 0; i < n; ++i)
        p->route[i].channel = i, p->route[i].gain = 1;
  }
  free(speakers);

  for (i = 0; i < n && p->route[i].channel == (int)i && p->route[i].gain == 1; ++i);
  if (i == n && n == ft->signal.channels)
    free(p->route), p->route = NULL;
  for (i = 0; p->route && i < n; ++i)
    lsx_debug("device channel %u plays channel %d", i, p->route[i].channel);
}

#define _(x,y) do {if ((err = x y) < 0) {lsx_fail_errno(ft, SOX_EPERM, #x " error: %s", snd_strerror(err)); goto error;} } while (0)
static int setup(sox_format_t * ft)
{
  priv_t                 * p = (priv_t *)ft->priv;
  snd_pcm_hw_params_t    * params = NULL;
  snd_pcm_format_mask_t  * mask = NULL;
  snd_pcm_uframes_t      min, max;
  unsigned               n;
  int                    err;

  _(snd_pcm_open, (&p->pcm, ft->filename, ft->mode == 'r'? SND_PCM_STREAM_CAPTURE : SND_PCM_STREAM_PLAYBACK, 0));
  _(snd_pcm_hw_params_malloc, (&params));
  _(snd_pcm_hw_params_any, (p->pcm, params));
#if SND_LIB_VERSION >= 0x010009               /* Disable alsa-lib resampling: */
  _(snd_pcm_hw_params_set_rate_resample, (p->pcm, params, 0));
#endif
  _(snd_pcm_hw_params_set_access, (p->pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED));

  _(snd_pcm_format_mask_malloc, (&mask));           /* Set format: */
  snd_pcm_hw_params_get_format_mask(params, mask);
  _(select_format, (&ft->encoding.encoding, &ft->encoding.bits_per_sample, mask, &p->format));
  _(snd_pcm_hw_params_set_format, (p->pcm, params, formats[p->format].alsa_fmt));
  snd_pcm_format_mask_free(mask), mask = NULL;

  n = ft->signal.rate;                              /* Set rate: */
  _(snd_pcm_hw_params_set_rate_near, (p->pcm, params, &n, 0));
  ft->signal.rate = n;

  /* Set channels: from 3 to 8 placed by speaker, the others as they are */
  n = ft->signal.channels >= 3 && ft->signal.channels <= 8?
    choose_channels(p->pcm, params, ft->signal.channels, ft->mode == 'w') : 0;
  if (n)
    _(snd_pcm_hw_params_set_channels, (p->pcm, params, n));
  else {
    n = ft->signal.channels;
    _(snd_pcm_hw_params_set_channels_near, (p->pcm, params, &n));
  }
  p->device_channels = n;

  /* Get number of significant bits: */
  if ((err = snd_pcm_hw_params_get_sbits(params)) > 0)
    ft->signal.precision = min(err, SOX_SAMPLE_PRECISION);
  else lsx_debug("snd_pcm_hw_params_get_sbits can't tell precision: %s",
           snd_strerror(err));

  /* Set buf_len > > sox_globals.bufsiz for no underrun: */
  p->buf_len = sox_globals.bufsiz * 8 / formats[p->format].bytes /
      p->device_channels;
  _(snd_pcm_hw_params_get_buffer_size_min, (params, &min));
  _(snd_pcm_hw_params_get_buffer_size_max, (params, &max));
  p->period = range_limit(p->buf_len, min, max) / 8;
  p->buf_len = p->period * 8;
  _(snd_pcm_hw_params_set_period_size_near, (p->pcm, params, &p->period, 0));
  _(snd_pcm_hw_params_set_buffer_size_near, (p->pcm, params, &p->buf_len));
  if (p->period * 2 > p->buf_len) {
    lsx_fail_errno(ft, SOX_EPERM, "buffer too small");
    goto error;
  }

  _(snd_pcm_hw_params, (p->pcm, params));           /* Configure ALSA */
  snd_pcm_hw_params_free(params), params = NULL;
  _(snd_pcm_prepare, (p->pcm));
  if (ft->signal.channels >= 3 && ft->signal.channels <= 8)
    route_channels(ft);
  else ft->signal.channels = p->device_channels;
  p->buf_len *= p->device_channels;                 /* No longer in `frames' */
  p->buf = lsx_malloc(p->buf_len * formats[p->format].bytes);
  if (p->route)
    p->frames = lsx_malloc(p->buf_len * sizeof(*p->frames));
  return SOX_SUCCESS;

error:
  if (mask) snd_pcm_format_mask_free(mask);
  if (params) snd_pcm_hw_params_free(params);
  return SOX_EOF;
}

static int recover(sox_format_t * ft, snd_pcm_t * pcm, int err)
{
  if (err == -EPIPE)
    lsx_warn("%s-run", ft->mode == 'r'? "over" : "under");
  else if (err != -ESTRPIPE)
    lsx_warn("%s", snd_strerror(err));
  else while ((err = snd_pcm_resume(pcm)) == -EAGAIN) {
    lsx_report("suspended");
    sleep(1);                  /* Wait until the suspend flag is released */
  }
  if (err < 0 && (err = snd_pcm_prepare(pcm)) < 0)
    lsx_fail_errno(ft, SOX_EPERM, "%s", snd_strerror(err));
  return err;
}

static size_t read_(sox_format_t * ft, sox_sample_t * buf, size_t len)
{
  priv_t             * p = (priv_t *)ft->priv;
  snd_pcm_sframes_t  i, n, f;
  size_t             done, frames;
  unsigned           channels = ft->signal.channels, c;
  sox_sample_t       * out;

  frames = min(len / channels, p->buf_len / p->device_channels);
  len = frames * channels;
  for (done = 0; done < len; done += n * channels) {
    do {
      n = snd_pcm_readi(p->pcm, p->buf, frames - done / channels);
      if (n < 0 && recover(ft, p->pcm, (int)n) < 0)
        return 0;
    } while (n <= 0);

    i = n * p->device_channels;
    out = p->route? p->frames : buf + done;
    switch (formats[p->format].alsa_fmt) {
      case SND_PCM_FORMAT_S8: {
        int8_t * buf1 = (int8_t *)p->buf;
        while (i--) *out++ = SOX_SIGNED_8BIT_TO_SAMPLE(*buf1++,);
        break;
      }
      case SND_PCM_FORMAT_U8: {
        uint8_t * buf1 = (uint8_t *)p->buf;
        while (i--) *out++ = SOX_UNSIGNED_8BIT_TO_SAMPLE(*buf1++,);
        break;
      }
      case SND_PCM_FORMAT_S16: {
        int16_t * buf1 = (int16_t *)p->buf;
        if (ft->encoding.reverse_bytes) while (i--)
          *out++ = SOX_SIGNED_16BIT_TO_SAMPLE(lsx_swapw(*buf1++),);
        else
          while (i--) *out++ = SOX_SIGNED_16BIT_TO_SAMPLE(*buf1++,);
        break;
      }
      case SND_PCM_FORMAT_U16: {
        uint16_t * buf1 = (uint16_t *)p->buf;
        if (ft->encoding.reverse_bytes) while (i--)
          *out++ = SOX_UNSIGNED_16BIT_TO_SAMPLE(lsx_swapw(*buf1++),);
        else
          while (i--) *out++ = SOX_UNSIGNED_16BIT_TO_SAMPLE(*buf1++,);
        break;
      }
      case SND_PCM_FORMAT_S24: {
        sox_int24_t * buf1 = (sox_int24_t *)p->buf;
        while (i--) *out++ = SOX_SIGNED_24BIT_TO_SAMPLE(*buf1++,);
        break;
      }
      case SND_PCM_FORMAT_S24_3LE: {
        unsigned char *buf1 = (unsigned char *)p->buf;
        while (i--) {
          uint32_t temp;
          temp  = *buf1++;
          temp |= *buf1++ << 8;
          temp |= *buf1++ << 16;
          *out++ = SOX_SIGNED_24BIT_TO_SAMPLE((sox_int24_t)temp,);
        }
        break;
      }
      case SND_PCM_FORMAT_U24: {
        sox_uint24_t * buf1 = (sox_uint24_t *)p->buf;
        while (i--) *out++ = SOX_UNSIGNED_24BIT_TO_SAMPLE(*buf1++,);
        break;
      }
      case SND_PCM_FORMAT_S32: {
        int32_t * buf1 = (int32_t *)p->buf;
        while (i--) *out++ = SOX_SIGNED_32BIT_TO_SAMPLE(*buf1++,);
        break;
      }
      case SND_PCM_FORMAT_U32: {
        uint32_t * buf1 = (uint32_t *)p->buf;
        while (i--) *out++ = SOX_UNSIGNED_32BIT_TO_SAMPLE(*buf1++,);
        break;
      }
      default: lsx_fail_errno(ft, SOX_EFMT, "invalid format");
        return 0;
    }
    for (f = 0; p->route && f < n; ++f)  /* from the device's order */
      for (c = 0; c < p->device_channels; ++c)
        if (p->route[c].channel >= 0)
          buf[done + f * channels + p->route[c].channel] =
            p->frames[f * p->device_channels + c];
  }
  return len;
}

static size_t write_(sox_format_t * ft, sox_sample_t const * buf, size_t len)
{
  priv_t             * p = (priv_t *)ft->priv;
  size_t             done, i, n, frames, f;
  unsigned           channels = ft->signal.channels, c;
  snd_pcm_sframes_t  actual;
  sox_sample_t const * in;
  SOX_SAMPLE_LOCALS;

  for (done = 0; done < len; done += frames * channels) {
    frames = min((len - done) / channels, p->buf_len / p->device_channels);
    if (!frames)
      break;
    in = buf + done;
    if (p->route) {                     /* to the device's order */
      sox_sample_t * out = p->frames;

      for (f = 0; f < frames; ++f, in += channels)
        for (c = 0; c < p->device_channels; ++c)
          *out++ = p->route[c].channel < 0? 0 :
            in[p->route[c].channel] * p->route[c].gain;
      in = p->frames;
    }
    i = n = frames * p->device_channels;
    switch (formats[p->format].alsa_fmt) {
      case SND_PCM_FORMAT_S8: {
        int8_t * buf1 = (int8_t *)p->buf;
        while (i--) *buf1++ = SOX_SAMPLE_TO_SIGNED_8BIT(*in++, ft->clips);
        break;
      }
      case SND_PCM_FORMAT_U8: {
        uint8_t * buf1 = (uint8_t *)p->buf;
        while (i--) *buf1++ = SOX_SAMPLE_TO_UNSIGNED_8BIT(*in++, ft->clips);
        break;
      }
      case SND_PCM_FORMAT_S16: {
        int16_t * buf1 = (int16_t *)p->buf;
        if (ft->encoding.reverse_bytes) while (i--)
          *buf1++ = lsx_swapw(SOX_SAMPLE_TO_SIGNED_16BIT(*in++, ft->clips));
        else
          while (i--) *buf1++ = SOX_SAMPLE_TO_SIGNED_16BIT(*in++, ft->clips);
        break;
      }
      case SND_PCM_FORMAT_U16: {
        uint16_t * buf1 = (uint16_t *)p->buf;
        if (ft->encoding.reverse_bytes) while (i--)
          *buf1++ = lsx_swapw(SOX_SAMPLE_TO_UNSIGNED_16BIT(*in++, ft->clips));
        else
          while (i--) *buf1++ = SOX_SAMPLE_TO_UNSIGNED_16BIT(*in++, ft->clips);
        break;
      }
      case SND_PCM_FORMAT_S24: {
        sox_int24_t * buf1 = (sox_int24_t *)p->buf;
        while (i--) *buf1++ = SOX_SAMPLE_TO_SIGNED_24BIT(*in++, ft->clips);
        break;
      }
      case SND_PCM_FORMAT_S24_3LE: {
        unsigned char *buf1 = (unsigned char *)p->buf;
        while (i--) {
          uint32_t temp = (uint32_t)SOX_SAMPLE_TO_SIGNED_24BIT(*in++, ft->clips);
          *buf1++ = (temp & 0x000000FF);
          *buf1++ = (temp & 0x0000FF00) >> 8;
          *buf1++ = (temp & 0x00FF0000) >> 16;
        }
        break;
      }
      case SND_PCM_FORMAT_U24: {
        sox_uint24_t * buf1 = (sox_uint24_t *)p->buf;
        while (i--) *buf1++ = SOX_SAMPLE_TO_UNSIGNED_24BIT(*in++, ft->clips);
        break;
      }
      case SND_PCM_FORMAT_S32: {
        int32_t * buf1 = (int32_t *)p->buf;
        while (i--) *buf1++ = SOX_SAMPLE_TO_SIGNED_32BIT(*in++, ft->clips);
        break;
      }
      case SND_PCM_FORMAT_U32: {
        uint32_t * buf1 = (uint32_t *)p->buf;
        while (i--) *buf1++ = SOX_SAMPLE_TO_UNSIGNED_32BIT(*in++, ft->clips);
        break;
      }
      default: lsx_fail_errno(ft, SOX_EFMT, "invalid format");
        return 0;
    }
    for (i = 0; i < n; i += actual * p->device_channels) do {
      actual = snd_pcm_writei(p->pcm,
          p->buf + i * formats[p->format].bytes,
          (n - i) / p->device_channels);
      if (errno == EAGAIN)     /* Happens naturally; don't report it: */
        errno = 0;
      if (actual < 0 && recover(ft, p->pcm, (int)actual) < 0)
        return 0;
    } while (actual < 0);
  }
  return len;
}

static int stop(sox_format_t * ft)
{
  priv_t * p = (priv_t *)ft->priv;
  snd_pcm_close(p->pcm);
  free(p->buf);
  free(p->route);
  free(p->frames);
  return SOX_SUCCESS;
}

static int stop_write(sox_format_t * ft)
{
  priv_t * p = (priv_t *)ft->priv;
  size_t n = ft->signal.channels * p->period, npad = n - (ft->olength % n);
  sox_sample_t * buf = lsx_calloc(npad, sizeof(*buf)); /* silent samples */

  if (npad != n)                      /* pad to hardware period: */
    write_(ft, buf, npad);
  free(buf);
  snd_pcm_drain(p->pcm);
  return stop(ft);
}

LSX_FORMAT_HANDLER(alsa)
{
  static char const * const names[] = {"alsa", NULL};
  static unsigned const write_encodings[] = {
    SOX_ENCODING_SIGN2   , 32, 24, 16, 8, 0,
    SOX_ENCODING_UNSIGNED, 32, 24, 16, 8, 0,
    0};
  static sox_format_handler_t const handler = {SOX_LIB_VERSION_CODE,
    "Advanced Linux Sound Architecture device driver",
    names, SOX_FILE_DEVICE | SOX_FILE_NOSTDIO,
    setup, read_, stop, setup, write_, stop_write,
    NULL, write_encodings, NULL, sizeof(priv_t)
  };
  return &handler;
}
