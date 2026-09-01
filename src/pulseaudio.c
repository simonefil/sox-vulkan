/* Pulse Audio sound handler
 *
 * Copyright 2008 Chris Bagwell And Sundry Contributors
 */

#include "sox_i.h"

#include <pulse/simple.h>
#include <pulse/error.h>

typedef struct {
  pa_simple *pasp;
  int32_t * buf;    /* The stream carries S32; the pipeline no longer does. */
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
  int error;

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

  pa->pasp = pa_simple_new(server, "SoX", dir, dev, app_str, &spec,
                          NULL, NULL, &error);

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

/* Pulse Audio buffer lengths are true buffer lengths and not count of samples,
 * and the two used to be the same thing said twice: a sample was an int32 and
 * the stream is S32, so the pipeline buffer could go to the device untouched.
 * A sample is a double now, so it goes through the same conversion every other
 * device driver uses, one bufsiz-sized block at a time. */
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
