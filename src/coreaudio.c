/* Copyright 2008 Chris Bagwell And Sundry Contributors
 */

/* AudioCore sound handler */

#include "sox_i.h"

#include <CoreAudio/CoreAudio.h>
#include <pthread.h>

#define Buffactor 4

typedef struct {
  AudioDeviceID adid;
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  int device_started;
  size_t bufsize;
  size_t bufrd;
  size_t bufwr;
  size_t bufrdavail;
  float *buf;
} priv_t;

static OSStatus PlaybackIOProc(AudioDeviceID inDevice UNUSED,
                               const AudioTimeStamp *inNow UNUSED,
                               const AudioBufferList *inInputData UNUSED,
                               const AudioTimeStamp *inInputTime UNUSED,
                               AudioBufferList *outOutputData,
                               const AudioTimeStamp *inOutputTime UNUSED,
                               void *inClientData)
{
    priv_t *ac = (priv_t*)((sox_format_t*)inClientData)->priv;
    AudioBuffer *buf;
    size_t copylen, avail;

    pthread_mutex_lock(&ac->mutex);

    /* An output split across several buffers, usually not interleaved, is
     * given the ring's frames one at a time, so that each buffer takes its
     * own channels of every frame. */
    if (outOutputData->mNumberBuffers > 1) {
        size_t channels = 0, frames = SIZE_MAX, frame, ch;
        UInt32 i;

        for (i = 0; i < outOutputData->mNumberBuffers; ++i) {
            AudioBuffer const *b = &outOutputData->mBuffers[i];

            if (b->mNumberChannels) {
                frames = min(frames, b->mDataByteSize / (sizeof(float) * b->mNumberChannels));
                channels += b->mNumberChannels;
            }
        }
        frames = channels? min(frames, ac->bufrdavail / channels) : 0;
        for (i = 0; i < outOutputData->mNumberBuffers; ++i)
            outOutputData->mBuffers[i].mDataByteSize =
                (UInt32)(frames * outOutputData->mBuffers[i].mNumberChannels * sizeof(float));
        for (frame = 0; frame < frames; ++frame)
            for (i = 0; i < outOutputData->mNumberBuffers; ++i) {
                AudioBuffer *b = &outOutputData->mBuffers[i];
                float *dst = b->mData? (float *)b->mData + frame * b->mNumberChannels : NULL;

                for (ch = 0; ch < b->mNumberChannels; ++ch) {
                    if (dst)
                        dst[ch] = ac->buf[ac->bufrd];
                    if (++ac->bufrd == ac->bufsize)
                        ac->bufrd = 0;
                    --ac->bufrdavail;
                }
            }
    }
    else for(buf = outOutputData->mBuffers;
        buf != outOutputData->mBuffers + outOutputData->mNumberBuffers;
        buf++){

        copylen = buf->mDataByteSize / sizeof(float);
        if(copylen > ac->bufrdavail)
            copylen = ac->bufrdavail;

        avail = ac->bufsize - ac->bufrd;
        if(buf->mData == NULL){
            /*do nothing-hardware can't play audio*/
        }else if(copylen > avail){
            memcpy(buf->mData, ac->buf + ac->bufrd, avail * sizeof(float));
            memcpy((float*)buf->mData + avail, ac->buf, (copylen - avail) * sizeof(float));
        }else{
            memcpy(buf->mData, ac->buf + ac->bufrd, copylen * sizeof(float));
        }

        buf->mDataByteSize = copylen * sizeof(float);
        ac->bufrd += copylen;
        if(ac->bufrd >= ac->bufsize)
            ac->bufrd -= ac->bufsize;
        ac->bufrdavail -= copylen;
    }

    pthread_cond_signal(&ac->cond);
    pthread_mutex_unlock(&ac->mutex);

    return kAudioHardwareNoError;
}

static OSStatus RecIOProc(AudioDeviceID inDevice UNUSED,
                          const AudioTimeStamp *inNow UNUSED,
                          const AudioBufferList *inInputData,
                          const AudioTimeStamp *inInputTime UNUSED,
                          AudioBufferList *outOutputData UNUSED,
                          const AudioTimeStamp *inOutputTime UNUSED,
                          void *inClientData)
{
    priv_t *ac = (priv_t *)((sox_format_t*)inClientData)->priv;
    size_t channels = 0, frames = SIZE_MAX, frame, ch, nfree;
    UInt32 i;

    pthread_mutex_lock(&ac->mutex);

    /* Input may come split across several buffers, usually not interleaved:
     * each frame is put together in the ring from all of them in turn. */
    for (i = 0; i < inInputData->mNumberBuffers; ++i) {
        AudioBuffer const *b = &inInputData->mBuffers[i];

        if (b->mData && b->mNumberChannels) {
            frames = min(frames, b->mDataByteSize / (sizeof(float) * b->mNumberChannels));
            channels += b->mNumberChannels;
        }
    }
    if (channels) {
        nfree = ac->bufsize - ac->bufrdavail - 1;
        if(nfree == 0)
            lsx_warn("coreaudio: unhandled buffer overrun.  Data discarded.");
        frames = min(frames, nfree / channels);
        for (frame = 0; frame < frames; ++frame)
            for (i = 0; i < inInputData->mNumberBuffers; ++i) {
                AudioBuffer const *b = &inInputData->mBuffers[i];
                float const *src;

                if (!b->mData || !b->mNumberChannels)
                    continue;
                src = (float const *)b->mData + frame * b->mNumberChannels;
                for (ch = 0; ch < b->mNumberChannels; ++ch) {
                    ac->buf[ac->bufwr] = src[ch];
                    if (++ac->bufwr == ac->bufsize)
                        ac->bufwr = 0;
                    ++ac->bufrdavail;
                }
            }
    }

    pthread_cond_signal(&ac->cond);
    pthread_mutex_unlock(&ac->mutex);

    return kAudioHardwareNoError;
}

/* The channels of all the device's streams in one direction, which the
 * stream format, describing a single stream, may not count all of. */
static size_t device_channels(AudioObjectID adid, int is_input)
{
  AudioObjectPropertyAddress address = {
    kAudioDevicePropertyStreamConfiguration,
    is_input? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput,
    kAudioObjectPropertyElementWildcard
  };
  UInt32 size = 0, i;
  AudioBufferList *list;
  size_t channels = 0;

  if (AudioObjectGetPropertyDataSize(adid, &address, 0, NULL, &size) != kAudioHardwareNoError)
    return 0;
  list = lsx_malloc(size);
  if (AudioObjectGetPropertyData(adid, &address, 0, NULL, &size, list) == kAudioHardwareNoError)
    for (i = 0; i < list->mNumberBuffers; ++i)
      channels += list->mBuffers[i].mNumberChannels;
  free(list);
  return channels;
}

static int setup(sox_format_t *ft, int is_input)
{
  priv_t *ac = (priv_t *)ft->priv;
  OSStatus status;
  UInt32 property_size;
  struct AudioStreamBasicDescription stream_desc;
  int32_t buf_size;
  int rc;

  if (strncmp(ft->filename, "default", (size_t)7) == 0)
  {
      property_size = sizeof(ac->adid);
      if (is_input)
          status = AudioHardwareGetProperty(kAudioHardwarePropertyDefaultInputDevice, &property_size, &ac->adid);
      else
          status = AudioHardwareGetProperty(kAudioHardwarePropertyDefaultOutputDevice, &property_size, &ac->adid);
  }
  else
  {
      Boolean is_writable;
      status = AudioHardwareGetPropertyInfo(kAudioHardwarePropertyDevices, &property_size, &is_writable);

      if (status == noErr)
      {
          int device_count = property_size/sizeof(AudioDeviceID);
          AudioDeviceID *devices;

          devices = malloc(property_size);
              status = AudioHardwareGetProperty(kAudioHardwarePropertyDevices, &property_size, devices);

          if (status == noErr)
          {
              int i;
              for (i = 0; i < device_count; i++)
              {
                  char name[256];

                  property_size = sizeof(name);
                  status = AudioDeviceGetProperty(devices[i],0,false,kAudioDevicePropertyDeviceName,&property_size,&name);

                  lsx_report("Found Audio Device \"%s\"\n",name);

                  /* String returned from OS is truncated so only compare
                   * as much as returned.
                   */
                  if (strncmp(name,ft->filename,strlen(name)) == 0)
                  {
                      ac->adid = devices[i];
                      break;
                  }
              }
          }
          free(devices);
      }
  }

  if (status || ac->adid == kAudioDeviceUnknown)
  {
    lsx_fail_errno(ft, SOX_EPERM, "can not open audio device");
    return SOX_EOF;
  }

  /* Query device to get initial values */
  property_size = sizeof(struct AudioStreamBasicDescription);
  status = AudioDeviceGetProperty(ac->adid, 0, is_input,
                                  kAudioDevicePropertyStreamFormat,
                                  &property_size, &stream_desc);
  if (status)
  {
    lsx_fail_errno(ft, SOX_EPERM, "can not get audio device properties");
    return SOX_EOF;
  }

  if (!(stream_desc.mFormatFlags & kLinearPCMFormatFlagIsFloat) ||
      stream_desc.mBitsPerChannel != 32)
  {
    lsx_fail_errno(ft, SOX_EPERM, "audio device does not accept float32 samples");
    return SOX_EOF;
  }

  /* An input counts the channels of all its streams, of which the stream
   * format may describe only one. */
  ft->signal.channels = is_input? device_channels(ac->adid, is_input) : 0;
  if (!ft->signal.channels)
    ft->signal.channels = stream_desc.mChannelsPerFrame;
  if (!ft->signal.channels)
    ft->signal.channels = 2;
  ft->signal.rate = 44100;
  ft->encoding.bits_per_sample = 32;

  /* TODO: only float at a fixed rate has been tried.  Where the hardware can
   * be set to the stream's rate and channel count, that should be preferred
   * to resampling and mixing. */

  if (is_input && stream_desc.mChannelsPerFrame != ft->signal.channels)
    lsx_debug("stream format reports %d channels but stream configuration reports %d channels",
              (int)stream_desc.mChannelsPerFrame, (int)ft->signal.channels);

  if (stream_desc.mSampleRate != ft->signal.rate)
  {
    lsx_debug("audio device did not accept %d sample rate. Use %d instead.", (int)ft->signal.rate,
              (int)stream_desc.mSampleRate);
    ft->signal.rate = stream_desc.mSampleRate;
  }

  /* `bufsiz' is a count of samples: the device buffer asked for below holds
   * a quarter of it, and this ring Buffactor such quarters. */
  ac->bufsize = sox_globals.bufsiz / 4 * Buffactor;
  ac->bufrd = 0;
  ac->bufwr = 0;
  ac->bufrdavail = 0;
  ac->buf = lsx_malloc(ac->bufsize * sizeof(float));

  buf_size = sox_globals.bufsiz / 4 * sizeof(float);
  property_size = sizeof(buf_size);
  status = AudioDeviceSetProperty(ac->adid, NULL, 0, is_input,
                                  kAudioDevicePropertyBufferSize,
                                  property_size, &buf_size);

  rc = pthread_mutex_init(&ac->mutex, NULL);
  if (rc)
  {
    lsx_fail_errno(ft, SOX_EPERM, "failed initializing mutex");
    return SOX_EOF;
  }

  rc = pthread_cond_init(&ac->cond, NULL);
  if (rc)
  {
    lsx_fail_errno(ft, SOX_EPERM, "failed initializing condition");
    return SOX_EOF;
  }

  ac->device_started = 0;

  /* Registers callback with the device without activating it. */
  if (is_input)
    status = AudioDeviceAddIOProc(ac->adid, RecIOProc, (void *)ft);
  else
    status = AudioDeviceAddIOProc(ac->adid, PlaybackIOProc, (void *)ft);

  return SOX_SUCCESS;
}

static int startread(sox_format_t *ft)
{
    return setup(ft, 1);
}

static size_t read_samples(sox_format_t *ft, sox_sample_t *buf, size_t nsamp)
{
    priv_t *ac = (priv_t *)ft->priv;
    size_t len;
    SOX_SAMPLE_LOCALS;

    if (!ac->device_started) {
        AudioDeviceStart(ac->adid, RecIOProc);
        ac->device_started = 1;
    }

    pthread_mutex_lock(&ac->mutex);

    /* Wait until input buffer has been filled by device driver */
    while (ac->bufrdavail == 0)
        pthread_cond_wait(&ac->cond, &ac->mutex);

    len = 0;
    while(len < nsamp && ac->bufrdavail > 0){
        buf[len] = SOX_FLOAT_32BIT_TO_SAMPLE(ac->buf[ac->bufrd], ft->clips);
        len++;
        ac->bufrd++;
        if(ac->bufrd == ac->bufsize)
            ac->bufrd = 0;
        ac->bufrdavail--;
    }

    pthread_mutex_unlock(&ac->mutex);

    return len;
}

static int stopread(sox_format_t * ft)
{
  priv_t *ac = (priv_t *)ft->priv;

  AudioDeviceStop(ac->adid, RecIOProc);
  AudioDeviceRemoveIOProc(ac->adid, RecIOProc);
  pthread_cond_destroy(&ac->cond);
  pthread_mutex_destroy(&ac->mutex);
  free(ac->buf);

  return SOX_SUCCESS;
}

static int startwrite(sox_format_t * ft)
{
    return setup(ft, 0);
}

static size_t write_samples(sox_format_t *ft, const sox_sample_t *buf, size_t nsamp)
{
    priv_t *ac = (priv_t *)ft->priv;
    size_t i;

    SOX_SAMPLE_LOCALS;

    pthread_mutex_lock(&ac->mutex);

    /* Wait to start until mutex is locked to help prevent callback
    * getting zero samples.
    */
    if(!ac->device_started){
        if(AudioDeviceStart(ac->adid, PlaybackIOProc)){
            pthread_mutex_unlock(&ac->mutex);
            return SOX_EOF;
        }
        ac->device_started = 1;
    }

    /* ac->bufsize, bufwr and bufrdavail count float samples. */
    for(i = 0; i < nsamp; i++){
        while(ac->bufrdavail == ac->bufsize - 1)
            pthread_cond_wait(&ac->cond, &ac->mutex);

        ac->buf[ac->bufwr] = SOX_SAMPLE_TO_FLOAT_32BIT(buf[i], ft->clips);
        ac->bufwr++;
        if(ac->bufwr == ac->bufsize)
            ac->bufwr = 0;
        ac->bufrdavail++;
    }

    pthread_mutex_unlock(&ac->mutex);
    return nsamp;
}


static int stopwrite(sox_format_t * ft)
{
    priv_t *ac = (priv_t *)ft->priv;

    if(ac->device_started){
        pthread_mutex_lock(&ac->mutex);

        while (ac->bufrdavail > 0)
            pthread_cond_wait(&ac->cond, &ac->mutex);

        pthread_mutex_unlock(&ac->mutex);

        AudioDeviceStop(ac->adid, PlaybackIOProc);
    }

    AudioDeviceRemoveIOProc(ac->adid, PlaybackIOProc);
    pthread_cond_destroy(&ac->cond);
    pthread_mutex_destroy(&ac->mutex);
    free(ac->buf);

    return SOX_SUCCESS;
}

LSX_FORMAT_HANDLER(coreaudio)
{
  static char const *const names[] = { "coreaudio", NULL };
  static unsigned const write_encodings[] = {
    SOX_ENCODING_FLOAT, 32, 0,
    0};
  static sox_format_handler_t const handler = {SOX_LIB_VERSION_CODE,
    "Mac AudioCore device driver",
    names, SOX_FILE_DEVICE | SOX_FILE_NOSTDIO,
    startread, read_samples, stopread,
    startwrite, write_samples, stopwrite,
    NULL, write_encodings, NULL, sizeof(priv_t)
  };
  return &handler;
}
