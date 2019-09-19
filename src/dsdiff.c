/* DSDIFF file support
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
 * http://dsd-guide.com/sites/default/files/white-papers/DSDIFF_1.5_Spec.pdf
 */

#include "sox_i.h"

struct dsdiff {
	uint64_t data_size;
	uint8_t *buf;
	uint32_t bit_pos;
};

#define ID(a, b, c, d) ((a) << 24 | (b) << 16 | (c) << 8 | (d))

static int dff_startread(sox_format_t *ft)
{
	struct dsdiff *dff = ft->priv;
	uint32_t ckid;
	uint32_t cktype;
	uint64_t cksize;
	uint64_t f8size;
	uint32_t fver;
	uint64_t spos, epos;
	uint32_t sample_rate;
	uint16_t num_channels;

	if (lsx_readdw(ft, &ckid) || ckid != ID('F', 'R', 'M', '8')) {
		lsx_fail_errno(ft, SOX_EHDR, "FRM8 tag not found");
		return SOX_EHDR;
	}

	if (lsx_readqw(ft, &f8size)) {
		lsx_fail_errno(ft, SOX_EHDR, "error reading chunk size");
		return SOX_EHDR;
	}

	if (lsx_readdw(ft, &cktype) || cktype != ID('D', 'S', 'D', ' ')) {
		lsx_fail_errno(ft, SOX_EHDR, "DSD tag not found");
		return SOX_EHDR;
	}

	do {
		if (lsx_readdw(ft, &ckid) || lsx_readqw(ft, &cksize)) {
			lsx_fail_errno(ft, SOX_EHDR, "read error");
			return SOX_EHDR;
		}

		spos = lsx_tell(ft);

		switch (ckid) {
		case ID('F', 'V', 'E', 'R'):
			if (cksize != 4)
				return SOX_EHDR;
			if (lsx_readdw(ft, &fver))
				return SOX_EHDR;
			if (fver >> 24 != 1) {
				lsx_fail_errno(ft, SOX_EHDR, "unknown version");
				return SOX_EHDR;
			}
			break;

		case ID('P', 'R', 'O', 'P'):
			if (cksize < 4)
				return SOX_EHDR;
			if (lsx_readdw(ft, &cktype))
				return SOX_EHDR;
			if (cktype == ID('S', 'N', 'D', ' '))
				cksize = 4;
			break;

		case ID('F', 'S', ' ', ' '):
			if (cksize < 4)
				return SOX_EHDR;
			if (lsx_readdw(ft, &sample_rate))
				return SOX_EHDR;
			break;

		case ID('C', 'H', 'N', 'L'):
			if (cksize < 4)
				return SOX_EHDR;
			if (lsx_readw(ft, &num_channels))
				return SOX_EHDR;
			break;

		case ID('C', 'M', 'P', 'R'):
			if (cksize < 4)
				return SOX_EHDR;
			if (lsx_readdw(ft, &cktype))
				return SOX_EHDR;
			if (cktype != ID('D', 'S', 'D', ' ')) {
				lsx_fail_errno(ft, SOX_EHDR,
					       "unsupported compression");
				return SOX_EHDR;
			}
			break;

		case ID('D', 'S', 'D', ' '):
			if (cksize < 8)
				return SOX_EHDR;
			dff->data_size = cksize;
			cksize = 0;
			break;
		}

		cksize += cksize & 1;
		epos = lsx_tell(ft);
		if (epos != spos + cksize)
			lsx_seeki(ft, (off_t)(spos + cksize - epos), SEEK_CUR);
	} while (cksize && epos < f8size);

	if (!sample_rate || !num_channels || !dff->data_size) {
		lsx_fail_errno(ft, SOX_EHDR, "invalid file header");
		return SOX_EHDR;
	}

	if (ckid != ID('D', 'S', 'D', ' ')) {
		lsx_fail_errno(ft, SOX_EHDR, "unsupported data type");
		return SOX_EHDR;
	}

	dff->buf = lsx_malloc(num_channels);
	if (!dff->buf)
		return SOX_ENOMEM;

	ft->data_start = lsx_tell(ft);

	ft->signal.rate = sample_rate;
	ft->signal.channels = num_channels;
	ft->signal.precision = 1;
	ft->signal.length = dff->data_size * 8;

	ft->encoding.encoding = SOX_ENCODING_DSD;
	ft->encoding.bits_per_sample = 1;

	return SOX_SUCCESS;
}

static size_t dff_read(sox_format_t *ft, sox_sample_t *buf, size_t len)
{
	struct dsdiff *dff = ft->priv;
	size_t nc = ft->signal.channels;
	size_t rsamp = 0;
	unsigned i, j;

	len /= nc;

	while (len >= 8) {
		unsigned bits = 8 - dff->bit_pos;

		if (lsx_read_b_buf(ft, dff->buf, nc) < nc)
			return rsamp * nc;

		for (i = 0; i < nc; i++) {
			unsigned d = dff->buf[i] << dff->bit_pos;

			for (j = 0; j < 8; j++) {
				buf[i + j * nc] = d & 128 ?
					SOX_SAMPLE_MAX : -SOX_SAMPLE_MAX;
				d <<= 1;
			}
		}

		dff->bit_pos = 0;
		buf += bits * nc;
		rsamp += bits;
		len -= bits;
	}

	return rsamp * nc;
}

static int dff_seek(sox_format_t * ft, sox_uint64_t offset)
{
	struct dsdiff *dff = ft->priv;
	uint64_t byte_offset = offset / 8;
	uint64_t data_offset = byte_offset * ft->signal.channels;
	int err;

	err = lsx_seeki(ft, ft->data_start + data_offset, SEEK_SET);
	if (err != SOX_SUCCESS)
		return err;

	dff->bit_pos = offset % 8;

	return SOX_SUCCESS;
}

static int dff_stopread(sox_format_t *ft)
{
	struct dsdiff *dff = ft->priv;

	free(dff->buf);

	return SOX_SUCCESS;
}

#define DFF_MAX_CHANNELS	1000
#define DFF_CMPR_NAME		"not compressed"

static int dff_writeheader(sox_format_t *ft)
{
	struct dsdiff *dff = ft->priv;
	unsigned chnl_size = 2 + 4 * ft->signal.channels;
	unsigned cmpr_size = 4 + 1 + sizeof(DFF_CMPR_NAME);
	unsigned prop_size =
		4 +			/* SND */
		12 + 4 +		/* FS */
		12 + chnl_size +
		12 + cmpr_size;
	uint64_t frm8_size =
		4 +			/* DSD */
		12 + 4 +		/* FVER */
		12 + prop_size +
		12 + dff->data_size;

	if (lsx_writedw(ft, ID('F', 'R', 'M', '8')) ||
	    lsx_writeqw(ft, dff->data_size ?
			frm8_size + dff->data_size : SOX_UNKNOWN_LEN) ||
            lsx_writedw(ft, ID('D', 'S', 'D', ' ')))
		return SOX_EOF;

	if (lsx_writedw(ft, ID('F', 'V', 'E', 'R')) ||
	    lsx_writeqw(ft, 4) ||
            lsx_writedw(ft, 0x01050000))
		return SOX_EOF;

	if (lsx_writedw(ft, ID('P', 'R', 'O', 'P')) ||
	    lsx_writeqw(ft, prop_size) ||
	    lsx_writedw(ft, ID('S', 'N', 'D', ' ')))
		return SOX_EOF;

	if (lsx_writedw(ft, ID('F', 'S', ' ', ' ')) ||
	    lsx_writeqw(ft, 4) ||
            lsx_writedw(ft, ft->signal.rate))
		return SOX_EOF;

	if (lsx_writedw(ft, ID('C', 'H', 'N', 'L')) ||
            lsx_writeqw(ft, chnl_size) ||
            lsx_writew(ft, ft->signal.channels))
		return SOX_EOF;

	if (ft->signal.channels == 2) {
		if (lsx_writedw(ft, ID('S', 'L', 'F', 'T')) ||
		    lsx_writedw(ft, ID('S', 'R', 'G', 'T')))
			return SOX_EOF;
	} else {
		char ch[8];
		unsigned i;

		for (i = 0; i < ft->signal.channels; i++) {
			snprintf(ch, sizeof(ch), "C%03d", i);
			if (lsx_writedw(ft, ID(ch[0], ch[1], ch[2], ch[3])))
				return SOX_EOF;
		}
	}

	if (lsx_writedw(ft, ID('C', 'M', 'P', 'R')) ||
	    lsx_writeqw(ft, cmpr_size) ||
	    lsx_writedw(ft, ID('D', 'S', 'D', ' ')) ||
	    lsx_writeb(ft, sizeof(DFF_CMPR_NAME)) ||
	    lsx_writes(ft, DFF_CMPR_NAME) ||
	    lsx_writeb(ft, 0))
		return SOX_EOF;

	if (lsx_writedw(ft, ID('D', 'S', 'D', ' ')) ||
	    lsx_writeqw(ft, dff->data_size ? dff->data_size : SOX_UNKNOWN_LEN))
		return SOX_EOF;

	return SOX_SUCCESS;
}

static int dff_startwrite(sox_format_t *ft)
{
	struct dsdiff *dff = ft->priv;

	if (ft->signal.channels > DFF_MAX_CHANNELS) {
		lsx_fail_errno(ft, SOX_EOF, "too many channels");
		return SOX_EOF;
	}

	dff->data_size = 0;
	dff->buf = lsx_malloc(ft->signal.channels);
	if (!dff->buf)
		return SOX_ENOMEM;

	return dff_writeheader(ft);
}

static int dff_write_buf(sox_format_t *ft)
{
	struct dsdiff *dff = ft->priv;
	size_t wlen = ft->signal.channels;

	if (lsx_write_b_buf(ft, dff->buf, wlen) < wlen)
		return SOX_EOF;

	memset(dff->buf, 0, wlen);

	return SOX_SUCCESS;
}

static void dff_write_bits(struct dsdiff *dff, const sox_sample_t *buf,
                           unsigned channels, unsigned start_bit, unsigned len)
{
	unsigned i, j;

	for (i = 0; i < channels; i++) {
		unsigned d = dff->buf[i];

		for (j = 0; j < len; j++) {
			d |= (buf[i + j * channels] > 0) <<
				(7 - j - start_bit);
		}

		dff->buf[i] = d;
	}
}

static size_t dff_write(sox_format_t *ft, const sox_sample_t *buf, size_t len)
{
	struct dsdiff *dff = ft->priv;
	unsigned nchan = ft->signal.channels;
	size_t wsamp = 0;

	len /= nchan;

	if (dff->bit_pos) {
		unsigned pre = min(len, 8 - dff->bit_pos);

		dff_write_bits(dff, buf, nchan, dff->bit_pos, pre);

		dff->bit_pos += pre;
		buf += pre * nchan;
		wsamp += pre;
		len -= pre;

		if (dff->bit_pos == 8) {
			dff->bit_pos = 0;

			if (dff_write_buf(ft))
				return 0;

			dff->data_size += ft->signal.channels;
		}
	}

	while (len >= 8) {
		dff_write_bits(dff, buf, nchan, 0, 8);

		buf += 8 * nchan;
		wsamp += 8;
		len -= 8;

		if (dff_write_buf(ft))
			return wsamp * nchan;

		dff->data_size += ft->signal.channels;
	}

	if (len) {
		dff_write_bits(dff, buf, nchan, 0, len);

		dff->bit_pos = len;
		wsamp += len;
	}

	return wsamp * nchan;
}

static int dff_stopwrite(sox_format_t *ft)
{
	struct dsdiff *dff = ft->priv;
	unsigned i;
	int err = SOX_SUCCESS;

	if (dff->bit_pos) {
		unsigned silence = 0x69 & (0xff >> dff->bit_pos);
		for (i = 0; i < ft->signal.channels; i++)
			dff->buf[i] |= silence;

		err = dff_write_buf(ft);
	}

	free(dff->buf);

	if (err)
		return err;

	if (lsx_seeki(ft, 0, SEEK_SET)) {
		lsx_fail_errno(ft, SOX_EOF,
			       "error rewinding output to update header");
		return SOX_EOF;
	}

	return dff_writeheader(ft);
}

LSX_FORMAT_HANDLER(dsdiff)
{
	static char const * const names[] = { "dff", NULL };
	static unsigned const write_encodings[] = {
		SOX_ENCODING_DSD, 1, 0,
		0
        };
	static sox_format_handler_t const handler = {
		SOX_LIB_VERSION_CODE,
		"Direct Stream Digital Interchange File Format (DSDIFF)",
		names, SOX_FILE_BIG_END,
		dff_startread, dff_read, dff_stopread,
		dff_startwrite, dff_write, dff_stopwrite,
		dff_seek, write_encodings, NULL,
		sizeof(struct dsdiff)
	};
	return &handler;
}

/*
 * The WSD format uses the same data layout as DSDIFF with a different
 * file header.
 *
 * http://1bitcons.acoust.ias.sci.waseda.ac.jp/pdf/wsd_file_format_ver1_1.pdf
 */

#define WSD_MAGIC ID('1', 'b', 'i', 't')

static int wsd_startread(sox_format_t *ft)
{
	struct dsdiff *dff = ft->priv;
	uint32_t magic;
	uint32_t res32;
	uint8_t version;
	uint8_t res8;
	uint32_t file_sz_l;
	uint32_t file_sz_h;
	uint64_t file_sz;
	uint32_t text_sp;
	uint32_t data_sp;
	uint32_t pb_tm;
	uint32_t fs;
	uint8_t ch_n;
	uint32_t ch_asn;
	uint32_t emph;

	if (lsx_readdw(ft, &magic) || magic != WSD_MAGIC) {
		lsx_fail_errno(ft, SOX_EHDR, "signature not found");
		return SOX_EHDR;
	}

	if (lsx_readdw(ft, &res32) || res32 != 0 ||
	    lsx_readb(ft, &version) ||
	    lsx_readb(ft, &res8) || res8 != 0 ||
	    lsx_readb(ft, &res8) || res8 != 0 ||
	    lsx_readb(ft, &res8) || res8 != 0 ||
	    lsx_readdw(ft, &file_sz_l) ||
	    lsx_readdw(ft, &file_sz_h) ||
	    lsx_readdw(ft, &text_sp) ||
	    lsx_readdw(ft, &data_sp) ||
	    lsx_readdw(ft, &res32) || res32 != 0) {
		lsx_fail_errno(ft, SOX_EHDR, "error reading header");
		return SOX_EHDR;
	}

	if (version != 0x10 && version != 0x11) {
		lsx_fail_errno(ft, SOX_EHDR, "unknown format version 0x%02x",
			       version);
		return SOX_EHDR;
	}

	if (text_sp != 0x80 || data_sp != 0x800) {
		lsx_fail_errno(ft, SOX_EHDR, "incorrect data offset");
		return SOX_EHDR;
	}

	file_sz = (uint64_t)file_sz_h << 32 | file_sz_l;

	if (file_sz <= data_sp) {
		lsx_fail_errno(ft, SOX_EHDR, "invalid file size");
		return SOX_EHDR;
	}

	if (lsx_readdw(ft, &pb_tm) ||
	    lsx_readdw(ft, &fs) ||
	    lsx_readdw(ft, &res32) || res32 != 0 ||
	    lsx_readb(ft, &ch_n) ||
	    lsx_readb(ft, &res8) || res8 != 0 ||
	    lsx_readb(ft, &res8) || res8 != 0 ||
	    lsx_readb(ft, &res8) || res8 != 0 ||
	    lsx_readdw(ft, &ch_asn) ||
	    lsx_readdw(ft, &res32) || res32 != 0 ||
	    lsx_readdw(ft, &res32) || res32 != 0 ||
	    lsx_readdw(ft, &res32) || res32 != 0 ||
	    lsx_readdw(ft, &emph) ||
	    lsx_readdw(ft, &res32) || res32 != 0) {
		lsx_fail_errno(ft, SOX_EHDR, "error reading header");
		return SOX_EHDR;
	}

	if (emph) {
		lsx_fail_errno(ft, SOX_EHDR, "invalid emphasis value");
		return SOX_EHDR;
	}

	if (lsx_seeki(ft, data_sp, SEEK_SET))
		return SOX_EOF;

	dff->buf = lsx_malloc(ch_n);
	if (!dff->buf)
		return SOX_ENOMEM;

	ft->data_start = data_sp;

	ft->signal.rate = fs;
	ft->signal.channels = ch_n;
	ft->signal.precision = 1;
	ft->signal.length = (file_sz - data_sp) * 8;

	ft->encoding.encoding = SOX_ENCODING_DSD;
	ft->encoding.bits_per_sample = 1;

	return SOX_SUCCESS;
}

LSX_FORMAT_HANDLER(wsd)
{
	static char const * const names[] = { "wsd", NULL };
	static sox_format_handler_t const handler = {
		SOX_LIB_VERSION_CODE,
		"Wideband Single-bit Data (WSD)",
		names, SOX_FILE_BIG_END,
		wsd_startread, dff_read, dff_stopread,
		NULL, NULL, NULL,
		dff_seek, NULL, NULL,
		sizeof(struct dsdiff)
	};
	return &handler;
}
