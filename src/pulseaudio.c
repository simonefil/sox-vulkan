/* Copyright 2008 Chris Bagwell And Sundry Contributors
 */

/* Pulse Audio sound handler */

#include "sox_i.h"

#include <pulse/simple.h>
#include <pulse/error.h>

typedef struct {
  pa_simple *pasp;
  int32_t * buf;    /* The stream carries S32 samples. */
  size_t    buf_len;  /* In samples. */
} priv_t;

static int setup(sox_format_t *ft, int is_input)
{
  priv_t *pa = (priv_t *)ft->priv;
  char *server;
  pa_stream_direction_t dir;
  char *app_str;
  char *dev;
  pa_sample_spec spec;
  pa_channel_map map;
  int error;
  /* SoX's channel order, the one canonical_layout in ffmpeg-codec.c gives
   * the codecs; beyond 8 channels the order is the file's, and unknown. */
  static pa_channel_position_t const positions[9][8] = {
    {PA_CHANNEL_POSITION_INVALID},
    {PA_CHANNEL_POSITION_MONO},
    {PA_CHANNEL_POSITION_FRONT_LEFT, PA_CHANNEL_POSITION_FRONT_RIGHT},
    {PA_CHANNEL_POSITION_FRONT_LEFT, PA_CHANNEL_POSITION_FRONT_RIGHT,
     PA_CHANNEL_POSITION_FRONT_CENTER},
    {PA_CHANNEL_POSITION_FRONT_LEFT, PA_CHANNEL_POSITION_FRONT_RIGHT,
     PA_CHANNEL_POSITION_REAR_LEFT, PA_CHANNEL_POSITION_REAR_RIGHT},
    {PA_CHANNEL_POSITION_FRONT_LEFT, PA_CHANNEL_POSITION_FRONT_RIGHT,
     PA_CHANNEL_POSITION_FRONT_CENTER,
     PA_CHANNEL_POSITION_REAR_LEFT, PA_CHANNEL_POSITION_REAR_RIGHT},
    {PA_CHANNEL_POSITION_FRONT_LEFT, PA_CHANNEL_POSITION_FRONT_RIGHT,
     PA_CHANNEL_POSITION_FRONT_CENTER, PA_CHANNEL_POSITION_LFE,
     PA_CHANNEL_POSITION_REAR_LEFT, PA_CHANNEL_POSITION_REAR_RIGHT},
    {PA_CHANNEL_POSITION_FRONT_LEFT, PA_CHANNEL_POSITION_FRONT_RIGHT,
     PA_CHANNEL_POSITION_FRONT_CENTER, PA_CHANNEL_POSITION_LFE,
     PA_CHANNEL_POSITION_REAR_CENTER,
     PA_CHANNEL_POSITION_SIDE_LEFT, PA_CHANNEL_POSITION_SIDE_RIGHT},
    {PA_CHANNEL_POSITION_FRONT_LEFT, PA_CHANNEL_POSITION_FRONT_RIGHT,
     PA_CHANNEL_POSITION_FRONT_CENTER, PA_CHANNEL_POSITION_LFE,
     PA_CHANNEL_POSITION_REAR_LEFT, PA_CHANNEL_POSITION_REAR_RIGHT,
     PA_CHANNEL_POSITION_SIDE_LEFT, PA_CHANNEL_POSITION_SIDE_RIGHT}
  };

  /* TODO: If user specified device of type "server:dev" then
   * break up and override server.
   */
  server = NULL;

  if (is_input)
  {
    dir = PA_STREAM_RECORD;
    app_str = "record";
  }
  else
  {
    dir = PA_STREAM_PLAYBACK;
    app_str = "playback";
  }

  if (strncmp(ft->filename, "default", (size_t)7) == 0)
    dev = NULL;
  else
    dev = ft->filename;

  /* If user doesn't specify, default to some reasonable values.
   * Since this is mainly for recording case, default to typical
   * 16-bit values to prevent saving larger files then average user
   * wants.  Power users can override to 32-bit if they wish.
   */
  if (ft->signal.channels == 0)
    ft->signal.channels = 2;
  if (ft->signal.rate == 0)
    ft->signal.rate = 44100;
  if (ft->encoding.bits_per_sample == 0)
  {
    ft->encoding.bits_per_sample = 16;
    ft->encoding.encoding = SOX_ENCODING_SIGN2;
  }
 
  spec.format = PA_SAMPLE_S32NE;
  spec.rate = ft->signal.rate;
  spec.channels = ft->signal.channels;

  if (spec.channels <= 8) {
    pa_channel_map_init(&map);
    map.channels = spec.channels;
    memcpy(map.map, positions[spec.channels], sizeof(positions[0]));
  }
  else if (pa_channel_map_init_auto(&map, spec.channels, PA_CHANNEL_MAP_AUX) == NULL)
  {
    lsx_fail_errno(ft, SOX_EINVAL, "can not map %u channels to Pulse Audio", spec.channels);
    return SOX_EOF;
  }

  pa->pasp = pa_simple_new(server, "SoX", dir, dev, app_str, &spec,
                          &map, NULL, &error);

  if (pa->pasp == NULL)
  {
    lsx_fail_errno(ft, SOX_EPERM, "can not open audio device: %s", pa_strerror(error));
    return SOX_EOF;
  }

  /* TODO: Is it better to convert format/rates in SoX or in
   * always let Pulse Audio do it?  Since we don't know what
   * hardware prefers, assume it knows best and give it
   * what user specifies.
   */

  pa->buf_len = sox_globals.bufsiz;
  pa->buf = lsx_malloc(pa->buf_len * sizeof(*pa->buf));

  return SOX_SUCCESS;
}

static int startread(sox_format_t *ft)
{
    return setup(ft, 1);
}

static int stopread(sox_format_t * ft)
{
  priv_t *pa = (priv_t *)ft->priv;

  pa_simple_free(pa->pasp);
  free(pa->buf);

  return SOX_SUCCESS;
}

/* Pulse Audio buffer lengths are in bytes.  Samples are converted between
 * the pipeline's doubles and the S32 stream through pa->buf, as in the other
 * device drivers, one bufsiz-sized block at a time. */
static size_t read_samples(sox_format_t *ft, sox_sample_t *buf, size_t nsamp)
{
  priv_t *pa = (priv_t *)ft->priv;
  size_t done, i, n;
  int error;

  for (done = 0; done < nsamp; done += n) {
    n = min(nsamp - done, pa->buf_len);
    if (pa_simple_read(pa->pasp, pa->buf, n * sizeof(*pa->buf), &error) < 0)
    {
      lsx_fail_errno(ft, SOX_EPERM, "error reading from pulse audio device: %s", pa_strerror(error));
      return SOX_EOF;
    }
    for (i = 0; i < n; ++i)
      buf[done + i] = SOX_SIGNED_32BIT_TO_SAMPLE(pa->buf[i],);
  }
  return nsamp;
}

static int startwrite(sox_format_t * ft)
{
    return setup(ft, 0);
}

static size_t write_samples(sox_format_t *ft, const sox_sample_t *buf, size_t nsamp)
{
  priv_t *pa = (priv_t *)ft->priv;
  size_t done, i, n;
  int error;
  SOX_SAMPLE_LOCALS;

  for (done = 0; done < nsamp; done += n) {
    n = min(nsamp - done, pa->buf_len);
    for (i = 0; i < n; ++i)
      pa->buf[i] = SOX_SAMPLE_TO_SIGNED_32BIT(buf[done + i], ft->clips);
    if (pa_simple_write(pa->pasp, pa->buf, n * sizeof(*pa->buf), &error) < 0)
    {
      lsx_fail_errno(ft, SOX_EPERM, "error writing to pulse audio device: %s", pa_strerror(error));
      return SOX_EOF;
    }
  }
  return nsamp;
}

static int stopwrite(sox_format_t * ft)
{
  priv_t *pa = (priv_t *)ft->priv;
  int error;

  pa_simple_drain(pa->pasp, &error);
  pa_simple_free(pa->pasp);
  free(pa->buf);

  return SOX_SUCCESS;
}

LSX_FORMAT_HANDLER(pulseaudio)
{
  static char const *const names[] = { "pulseaudio", NULL };
  static unsigned const write_encodings[] = {
    SOX_ENCODING_SIGN2, 32, 0,
    0};
  static sox_format_handler_t const handler = {SOX_LIB_VERSION_CODE,
    "Pulse Audio client",
    names, SOX_FILE_DEVICE | SOX_FILE_NOSTDIO,
    startread, read_samples, stopread,
    startwrite, write_samples, stopwrite,
    NULL, write_encodings, NULL, sizeof(priv_t)
  };
  return &handler;
}
