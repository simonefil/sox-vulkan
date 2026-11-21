/* This library is free software; you can redistribute it and/or modify it
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

/* libSoX Monkey's Audio frame decoder and encoder, from the Monkey's Audio
 * SDK.
 *
 * CAPEDecompressCore and CAPECompressCore are the SDK's frame decoder and
 * encoder, which its decompressor and compressor run on worker threads: each
 * works on its own thread when handed a frame and signals when the frame is
 * done.  One worker is used, and waited for.  The decoder asks the stream's
 * parameters of the decompressor it is made for, which here answers from
 * what it is created with.  Floating-point samples are coded as 32-bit
 * integers, their bit patterns transformed: the transform comes before the
 * encoder and is undone after the decoder, as in the SDK's compressor and
 * decompressor.  The SDK is built without its MD5, which the whole-file code
 * linked with these needs: SoX's stands in for it. */

#include "ape-mac.h"

#include <All.h>
#include <APECompressCore.h>
#include <APEDecompress.h>
#include <APEDecompressCore.h>
#include <FloatTransform.h>

#include <cstring>
#include <new>

static void set_format(APE::WAVEFORMATEX * format, unsigned channels, unsigned bits_per_sample, unsigned rate)
{
  format->wFormatTag = WAVE_FORMAT_PCM;
  format->nChannels = static_cast<APE::uint16>(channels);
  format->nSamplesPerSec = rate;
  format->wBitsPerSample = static_cast<APE::uint16>(bits_per_sample);
  format->nBlockAlign = static_cast<APE::uint16>(channels * bits_per_sample / 8);
  format->nAvgBytesPerSec = rate * format->nBlockAlign;
  format->cbSize = 0;
}

/* What the SDK's MD5.h declares, the context being its MD5_CTX: storage of
 * the same size and alignment is all the SDK sees of it. */
struct MD5Context {
  lsx_md5_t md5;
};

static_assert(sizeof(MD5Context) == 88 && alignof(MD5Context) == 4, "MD5_CTX of the SDK");

void MD5Init(MD5Context * context);
void MD5Update(MD5Context * context, void const * data, unsigned int size);
void MD5Final(unsigned char digest[16], MD5Context * context);

void MD5Init(MD5Context * context)
{
  lsx_md5_init(&context->md5);
}

void MD5Update(MD5Context * context, void const * data, unsigned int size)
{
  lsx_md5_add(&context->md5, data, size);
}

void MD5Final(unsigned char digest[16], MD5Context * context)
{
  lsx_md5_final(&context->md5, digest);
}

namespace {

class stream : public APE::CAPEDecompress {
public:
  stream(unsigned version, int level, unsigned frame_blocks, APE::WAVEFORMATEX const & format) :
    m_version(version), m_level(level), m_frame_blocks(frame_blocks), m_format(format) {}

  APE::int64 GetInfo(APE::IAPEDecompress::APE_INFO_FIELDS field, APE::int64 param1, APE::int64) APE_OVERRIDE
  {
    switch (field) {
      case APE_INFO_FILE_VERSION: return m_version;
      case APE_INFO_COMPRESSION_LEVEL: return m_level;
      case APE_INFO_CHANNELS: return m_format.nChannels;
      case APE_INFO_SAMPLE_RATE: return m_format.nSamplesPerSec;
      case APE_INFO_BITS_PER_SAMPLE: return m_format.wBitsPerSample;
      case APE_INFO_BYTES_PER_SAMPLE: return m_format.wBitsPerSample / 8;
      case APE_INFO_BLOCK_ALIGN: return m_format.nBlockAlign;
      case APE_INFO_BLOCKS_PER_FRAME: return m_frame_blocks;
      case APE_INFO_WAVEFORMATEX:
        *reinterpret_cast<APE::WAVEFORMATEX *>(param1) = m_format;
        return APE_ERROR_SUCCESS;
      default: return 0;
    }
  }

private:
  unsigned m_version;
  int m_level;
  unsigned m_frame_blocks;
  APE::WAVEFORMATEX m_format;
};

}

struct lsx_ape_decoder {
  stream * info;
  APE::CAPEDecompressCore * core;
  unsigned block_bytes;
  bool floating_point;
  unsigned char * pcm;
};

lsx_ape_decoder_t * lsx_ape_decoder_create(unsigned version, int level, unsigned channels,
    unsigned bits_per_sample, unsigned rate, unsigned frame_blocks, int floating_point)
{
  APE::WAVEFORMATEX format;
  lsx_ape_decoder_t * decoder;
  int error = APE_ERROR_SUCCESS;

  set_format(&format, channels, bits_per_sample, rate);
  decoder = new (std::nothrow) lsx_ape_decoder();
  if (!decoder)
    return NULL;
  decoder->block_bytes = format.nBlockAlign;
  decoder->floating_point = floating_point != 0;
  decoder->info = new (std::nothrow) stream(version, level, frame_blocks, format);
  decoder->pcm = new (std::nothrow) unsigned char[static_cast<size_t>(frame_blocks) * format.nBlockAlign];
  if (decoder->info && decoder->pcm)
    decoder->core = new (std::nothrow) APE::CAPEDecompressCore(&error, decoder->info, decoder->info);
  if (!decoder->core || error != APE_ERROR_SUCCESS || decoder->core->InitializeDecompressor() != APE_ERROR_SUCCESS ||
      !decoder->core->Start()) {
    lsx_ape_decoder_destroy(decoder);
    return NULL;
  }
  decoder->core->WaitUntilReady();
  return decoder;
}

int lsx_ape_decoder_decode(lsx_ape_decoder_t * decoder, unsigned char const * frame, size_t bytes,
    unsigned skip, unsigned blocks, unsigned char const * * pcm)
{
  if (!blocks || blocks > decoder->info->GetInfo(APE::IAPEDecompress::APE_INFO_BLOCKS_PER_FRAME, 0, 0))
    return 1;
  memcpy(decoder->core->GetInputBuffer(static_cast<APE::uint32>(bytes)), frame, bytes);
  decoder->core->DecodeFrame(static_cast<int>(skip), blocks);
  decoder->core->WaitUntilReady();
  if (decoder->core->GetErrorState() != APE_ERROR_SUCCESS ||
      decoder->core->GetFrameBytes() != static_cast<APE::uint32>(blocks) * decoder->block_bytes)
    return 1;
  decoder->core->GetFrameData(decoder->pcm);
  if (decoder->floating_point)
    APE::CFloatTransform::Process(reinterpret_cast<APE::uint32 *>(decoder->pcm),
        static_cast<APE::int64>(blocks) * decoder->info->GetInfo(APE::IAPEDecompress::APE_INFO_CHANNELS, 0, 0));
  *pcm = decoder->pcm;
  return 0;
}

void lsx_ape_decoder_destroy(lsx_ape_decoder_t * decoder)
{
  if (decoder) {
    delete decoder->core;
    delete decoder->info;
    delete [] decoder->pcm;
    delete decoder;
  }
}

struct lsx_ape_encoder {
  APE::CAPECompressCore * core;
  unsigned block_bytes;
  bool floating_point;
};

lsx_ape_encoder_t * lsx_ape_encoder_create(unsigned channels, unsigned bits_per_sample,
    unsigned rate, int level, unsigned frame_blocks, int floating_point)
{
  APE::WAVEFORMATEX format;
  lsx_ape_encoder_t * encoder;

  set_format(&format, channels, bits_per_sample, rate);
  encoder = new (std::nothrow) lsx_ape_encoder;
  if (!encoder)
    return NULL;
  encoder->block_bytes = format.nBlockAlign;
  encoder->floating_point = floating_point != 0;
  encoder->core = new (std::nothrow) APE::CAPECompressCore(&format, static_cast<int>(frame_blocks), level);
  if (!encoder->core || !encoder->core->Start()) {
    delete encoder->core;
    delete encoder;
    return NULL;
  }
  encoder->core->WaitUntilReady();
  return encoder;
}

int lsx_ape_encoder_encode(lsx_ape_encoder_t * encoder, void * pcm, size_t bytes,
    unsigned char const * * frame, size_t * frame_bytes)
{
  if (encoder->floating_point)
    APE::CFloatTransform::Process(static_cast<APE::uint32 *>(pcm), static_cast<APE::int64>(bytes / 4));
  if (bytes % encoder->block_bytes || encoder->core->EncodeFrame(pcm, static_cast<int>(bytes)) != APE_ERROR_SUCCESS)
    return 1;
  encoder->core->WaitUntilReady();
  *frame = encoder->core->GetFrameBuffer();
  *frame_bytes = (encoder->core->GetFrameBytes() + 3) / 4 * 4;
  return 0;
}

void lsx_ape_encoder_destroy(lsx_ape_encoder_t * encoder)
{
  if (encoder) {
    delete encoder->core;
    delete encoder;
  }
}
