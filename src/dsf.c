/* DSF file support
 *
 * Copyright (c) 2015 Mans Rullgard <mans@mansr.com>
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

/* File format specification available at
 * http://dsd-guide.com/sites/default/files/white-papers/DSFFileFormatSpec_E.pdf
 */

#include "sox_i.h"
#include "id3.h"

struct dsf {
	uint64_t file_size;
	uint64_t metadata;
	uint32_t version;
	uint32_t format_id;
	uint32_t chan_type;
	uint32_t chan_num;
	uint32_t sfreq;
	uint32_t bps;
	uint64_t scount;
	uint32_t block_size;

	uint32_t block_start;
	uint32_t block_pos;
	uint32_t bit_pos;
	uint8_t *block;
	uint64_t read_samp;
};

#define TAG(a, b, c, d) ((a) | (b) << 8 | (c) << 16 | (d) << 24)

#define DSF_TAG  TAG('D', 'S', 'D', ' ')
#define FMT_TAG  TAG('f', 'm', 't', ' ')
#define DATA_TAG TAG('d', 'a', 't', 'a')

#define HEADER_SIZE (28 + 52 + 12)

static int dsf_startread(sox_format_t *ft)
{
	struct dsf *dsf = ft->priv;
	uint32_t magic;
	uint64_t csize;
	uint32_t v;

	if (lsx_readdw(ft, &magic) || magic != DSF_TAG) {
		lsx_fail_errno(ft, SOX_EHDR, "DSF signature not found");
		return SOX_EHDR;
	}

	if (lsx_readqw(ft, &csize) || csize != 28) {
		lsx_fail_errno(ft, SOX_EHDR, "invalid DSD chunk size");
		return SOX_EHDR;
	}

	lsx_readqw(ft, &dsf->file_size);
	lsx_readqw(ft, &dsf->metadata);

	if (lsx_readdw(ft, &magic) || magic != FMT_TAG) {
		lsx_fail_errno(ft, SOX_EHDR, "fmt chunk not found");
		return SOX_EHDR;
	}

	if (lsx_readqw(ft, &csize) || csize != 52) {
		lsx_fail_errno(ft, SOX_EHDR, "invalid fmt chunk size");
		return SOX_EHDR;
	}

	if (lsx_readdw(ft, &dsf->version)   ||
	    lsx_readdw(ft, &dsf->format_id) ||
	    lsx_readdw(ft, &dsf->chan_type) ||
	    lsx_readdw(ft, &dsf->chan_num)  ||
	    lsx_readdw(ft, &dsf->sfreq)     ||
	    lsx_readdw(ft, &dsf->bps)       ||
	    lsx_readqw(ft, &dsf->scount)    ||
	    lsx_readdw(ft, &dsf->block_size))
		return SOX_EHDR;

	if (lsx_readdw(ft, &v) || v) /* reserved */
		return SOX_EHDR;

	if (lsx_readdw(ft, &magic) || magic != DATA_TAG) {
		lsx_fail_errno(ft, SOX_EHDR, "data chunk not found");
		return SOX_EHDR;
	}

	if (lsx_readqw(ft, &csize) ||
	    csize < 12 + dsf->block_size * dsf->chan_num) {
		lsx_fail_errno(ft, SOX_EHDR, "invalid data chunk size");
		return SOX_EHDR;
	}

	if (dsf->version != 1) {
		lsx_fail_errno(ft, SOX_EHDR, "unknown format version %d",
			       dsf->version);
		return SOX_EHDR;
	}

	if (dsf->format_id != 0) {
		lsx_fail_errno(ft, SOX_EFMT, "unknown format ID %d",
			       dsf->format_id);
		return SOX_EFMT;
	}

	if (dsf->chan_num < 1 || dsf->chan_num > 6) {
		lsx_fail_errno(ft, SOX_EHDR, "invalid channel count %d",
			       dsf->chan_num);
		return SOX_EHDR;
	}

	if (dsf->bps != 1) {
		lsx_fail_errno(ft, SOX_EFMT, "unsupported bit depth %d",
			       dsf->bps);
		return SOX_EFMT;
	}

	dsf->block = lsx_calloc(dsf->chan_num, (size_t)dsf->block_size);
	if (!dsf->block)
		return SOX_ENOMEM;

	ft->data_start = lsx_tell(ft);

	if (dsf->metadata && ft->seekable) {
		if (!lsx_seeki(ft, dsf->metadata, SEEK_SET))
			lsx_id3_read_tag(ft, sox_false);
		lsx_seeki(ft, ft->data_start, SEEK_SET);
	}

	dsf->block_pos = dsf->block_size;
	dsf->block_start = 0;

	ft->signal.rate = dsf->sfreq;
	ft->signal.channels = dsf->chan_num;
	ft->signal.precision = 1;
	ft->signal.length = dsf->scount * dsf->chan_num;

	ft->encoding.encoding = SOX_ENCODING_DSD;
	ft->encoding.bits_per_sample = 1;

	return SOX_SUCCESS;
}

static void dsf_read_bits(struct dsf *dsf, sox_sample_t *buf,
                          unsigned start, unsigned len)
{
	uint8_t *dsd = dsf->block + dsf->block_pos;
	unsigned i, j;

	for (i = 0; i < dsf->chan_num; i++) {
		unsigned d = dsd[i * dsf->block_size] >> start;

		for (j = 0; j < len; j++) {
			buf[i + j * dsf->chan_num] = d & 1 ?
				SOX_SAMPLE_MAX : -SOX_SAMPLE_MAX;
			d >>= 1;
		}
	}
}

static size_t dsf_read(sox_format_t *ft, sox_sample_t *buf, size_t len)
{
	struct dsf *dsf = ft->priv;
	uint64_t samp_left = dsf->scount - dsf->read_samp;
	size_t rsamp = 0;

	len /= dsf->chan_num;
	len = min(len, samp_left);

	while (len >= 8) {
		unsigned bits = 8 - dsf->bit_pos;

		if (dsf->block_pos >= dsf->block_size) {
			size_t rlen = dsf->chan_num * dsf->block_size;
			if (lsx_read_b_buf(ft, dsf->block, rlen) < rlen)
				return rsamp * dsf->chan_num;
			dsf->block_pos = dsf->block_start;
			dsf->block_start = 0;
		}

		dsf_read_bits(dsf, buf, dsf->bit_pos, bits);
		buf += bits * dsf->chan_num;
		dsf->block_pos++;
		dsf->bit_pos = 0;
		rsamp += bits;
		len -= bits;
	}

	if (len && samp_left < 8) {
		dsf_read_bits(dsf, buf, 0, len);
		rsamp += len;
	}

	dsf->read_samp += rsamp;

	return rsamp * dsf->chan_num;
}

static int dsf_seek(sox_format_t * ft, sox_uint64_t offset)
{
	struct dsf *dsf = ft->priv;
	uint64_t byte_offset;
	uint64_t block_num;
	unsigned block_start;
        off_t data_offs;
	int err;

	if (offset > dsf->scount)
		return SOX_EOF;

	byte_offset = offset / 8;
	block_num = byte_offset / dsf->block_size;
	block_start = byte_offset % dsf->block_size;
	data_offs = dsf->chan_num * dsf->block_size * block_num;

	err = lsx_seeki(ft, ft->data_start + data_offs, SEEK_SET);
	if (err != SOX_SUCCESS)
		return err;

	dsf->block_pos = dsf->block_size;
	dsf->block_start = block_start;;
	dsf->bit_pos = offset % 8;
	dsf->read_samp = offset;

	return SOX_SUCCESS;
}

static int dsf_stopread(sox_format_t *ft)
{
	struct dsf *dsf = ft->priv;

	free(dsf->block);

	return SOX_SUCCESS;
}

static int dsf_writeheader(sox_format_t *ft)
{
	struct dsf *dsf = ft->priv;
	uint64_t data_size = dsf->file_size ? dsf->file_size - HEADER_SIZE : 0;

	if (lsx_writedw(ft, DSF_TAG) ||
	    lsx_writeqw(ft, (uint64_t)28) ||
	    lsx_writeqw(ft, dsf->file_size) ||
	    lsx_writeqw(ft, dsf->metadata) ||
	    lsx_writedw(ft, FMT_TAG) ||
	    lsx_writeqw(ft, (uint64_t)52) ||
	    lsx_writedw(ft, dsf->version) ||
	    lsx_writedw(ft, dsf->format_id) ||
	    lsx_writedw(ft, dsf->chan_type) ||
	    lsx_writedw(ft, dsf->chan_num) ||
	    lsx_writedw(ft, dsf->sfreq) ||
	    lsx_writedw(ft, dsf->bps) ||
	    lsx_writeqw(ft, dsf->scount ? dsf->scount : SOX_UNKNOWN_LEN) ||
	    lsx_writedw(ft, dsf->block_size) ||
	    lsx_writedw(ft, 0) || /* reserved */
	    lsx_writedw(ft, DATA_TAG) ||
	    lsx_writeqw(ft, data_size + 12))
		return SOX_EOF;

	return SOX_SUCCESS;
}

static int dsf_startwrite(sox_format_t *ft)
{
	struct dsf *dsf = ft->priv;

	dsf->version = 1;
	dsf->format_id = 0;
	dsf->chan_type = ft->signal.channels + (ft->signal.channels > 4);
	dsf->chan_num = ft->signal.channels;
	dsf->sfreq = ft->signal.rate;
	dsf->bps = ft->encoding.bits_per_sample;
	dsf->block_size = 4096;

	dsf->block = lsx_calloc(dsf->chan_num, (size_t)dsf->block_size);
	if (!dsf->block)
		return SOX_ENOMEM;

	return dsf_writeheader(ft);
}

static int dsf_write_buf(sox_format_t *ft)
{
	struct dsf *dsf = ft->priv;

	if (dsf->block_pos == dsf->block_size) {
		size_t wlen = dsf->chan_num * dsf->block_size;
		if (lsx_write_b_buf(ft, dsf->block, wlen) < wlen)
			return SOX_EOF;
		dsf->block_pos = 0;
		memset(dsf->block, 0, wlen);
	}

	return SOX_SUCCESS;
}

static void dsf_write_bits(struct dsf *dsf, const sox_sample_t *buf,
			   unsigned start_bit, unsigned len)
{
	uint8_t *dsd = dsf->block + dsf->block_pos;
	unsigned i, j;

	for (i = 0; i < dsf->chan_num; i++) {
		unsigned d = dsd[i * dsf->block_size];

		for (j = 0; j < len; j++) {
			d |= (buf[i + j * dsf->chan_num] > 0) <<
				(j + start_bit);
		}

		dsd[i * dsf->block_size] = d;
	}
}

static size_t dsf_write(sox_format_t *ft, const sox_sample_t *buf, size_t len)
{
	struct dsf *dsf = ft->priv;
	unsigned nchan = dsf->chan_num;
	size_t wsamp = 0;

	len /= nchan;

	if (dsf->bit_pos) {
		unsigned pre = min(len, 8 - dsf->bit_pos);

		dsf_write_bits(dsf, buf, dsf->bit_pos, pre);
		buf += pre * nchan;
		wsamp += pre;
		len -= pre;
		dsf->bit_pos += pre;

		if (dsf->bit_pos == 8) {
			dsf->block_pos++;
			dsf->bit_pos = 0;
			if (dsf_write_buf(ft))
				return 0;
		}
	}

	while (len >= 8) {
		dsf_write_bits(dsf, buf, 0, 8);
		buf += 8 * nchan;
		dsf->block_pos++;
		wsamp += 8;
		len -= 8;

		if (dsf_write_buf(ft))
			return wsamp * nchan;
	}

	if (len) {
		dsf_write_bits(dsf, buf, 0, (unsigned)len);
		wsamp += len;
		dsf->bit_pos = len;
	}

	dsf->scount += wsamp;

	return wsamp * nchan;
}

static int dsf_stopwrite(sox_format_t *ft)
{
	struct dsf *dsf = ft->priv;
	int err = SOX_SUCCESS;

	if (dsf->bit_pos)
		dsf->block_pos++;

	if (dsf->block_pos) {
		size_t wlen = dsf->chan_num * dsf->block_size;
		if (lsx_write_b_buf(ft, dsf->block, wlen) < wlen)
			err = SOX_EOF;
	}

	free(dsf->block);

	if (err)
		return err;

	dsf->file_size = lsx_tell(ft);

	if (lsx_seeki(ft, (off_t)0, SEEK_SET)) {
		lsx_fail_errno(ft, SOX_EOF,
			       "error rewinding output to update header");
		return SOX_EOF;
	}

	return dsf_writeheader(ft);
}

LSX_FORMAT_HANDLER(dsf)
{
	static char const * const names[] = { "dsf", NULL };
	static unsigned const write_encodings[] = {
		SOX_ENCODING_DSD, 1, 0,
		0 };
	static sox_format_handler_t const handler = {
		SOX_LIB_VERSION_CODE,
		"Container for DSD data",
		names, SOX_FILE_LIT_END,
		dsf_startread, dsf_read, dsf_stopread,
		dsf_startwrite, dsf_write, dsf_stopwrite,
		dsf_seek, write_encodings, NULL,
		sizeof(struct dsf)
	};
	return &handler;
}
