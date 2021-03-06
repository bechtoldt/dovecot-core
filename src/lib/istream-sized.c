/* Copyright (c) 2003-2016 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "istream-private.h"
#include "istream-sized.h"

struct sized_istream {
	struct istream_private istream;

	istream_sized_callback_t *error_callback;
	void *error_context;

	uoff_t size;
};

static void i_stream_sized_destroy(struct iostream_private *stream)
{
	struct sized_istream *sstream = (struct sized_istream *)stream;
	uoff_t v_offset;

	v_offset = sstream->istream.parent_start_offset +
		sstream->istream.istream.v_offset;
	if (sstream->istream.parent->seekable ||
	    v_offset > sstream->istream.parent->v_offset) {
		/* get to same position in parent stream */
		i_stream_seek(sstream->istream.parent, v_offset);
	}
}

static const char *
i_stream_create_sized_default_error_callback(
	const struct istream_sized_error_data *data, void *context ATTR_UNUSED)
{
	if (data->v_offset + data->new_bytes < data->wanted_size) {
		return t_strdup_printf("Stream is smaller than expected "
			"(%"PRIuUOFF_T" < %"PRIuUOFF_T")",
			data->v_offset + data->new_bytes, data->wanted_size);
	} else {
		return t_strdup_printf("Stream is larger than expected "
			"(%"PRIuUOFF_T" > %"PRIuUOFF_T", eof=%d)",
			data->v_offset + data->new_bytes, data->wanted_size,
			data->eof ? 1 : 0);
	}
}

static ssize_t i_stream_sized_read(struct istream_private *stream)
{
	struct sized_istream *sstream =
		(struct sized_istream *)stream;
	struct istream_sized_error_data data;
	const char *error;
	uoff_t left;
	ssize_t ret;
	size_t pos;

	if (stream->istream.v_offset +
	    (stream->pos - stream->skip) >= sstream->size) {
		stream->istream.eof = TRUE;
		return -1;
	}

	i_stream_seek(stream->parent, sstream->istream.parent_start_offset +
		      stream->istream.v_offset);

	stream->pos -= stream->skip;
	stream->skip = 0;

	stream->buffer = i_stream_get_data(stream->parent, &pos);
	if (pos > stream->pos)
		ret = 0;
	else do {
		if ((ret = i_stream_read(stream->parent)) == -2)
			return -2;

		stream->istream.stream_errno = stream->parent->stream_errno;
		stream->istream.eof = stream->parent->eof;
		stream->buffer = i_stream_get_data(stream->parent, &pos);
	} while (pos <= stream->pos && ret > 0);

	memset(&data, 0, sizeof(data));
	data.v_offset = stream->istream.v_offset;
	data.new_bytes = pos;
	data.wanted_size = sstream->size;
	data.eof = stream->istream.eof;

	left = sstream->size - stream->istream.v_offset;
	if (pos == left)
		stream->istream.eof = TRUE;
	else if (pos > left) {
		error = sstream->error_callback(&data, sstream->error_context);
		io_stream_set_error(&stream->iostream, "%s", error);
		i_error("read(%s) failed: %s",
			i_stream_get_name(&stream->istream),
			stream->iostream.error);
		pos = left;
		stream->istream.eof = TRUE;
		stream->istream.stream_errno = EINVAL;
		return -1;
	} else if (!stream->istream.eof) {
		/* still more to read */
	} else if (stream->istream.stream_errno == ENOENT) {
		/* lost the file */
	} else {
		error = sstream->error_callback(&data, sstream->error_context);
		io_stream_set_error(&stream->iostream, "%s", error);
		i_error("read(%s) failed: %s",
			i_stream_get_name(&stream->istream),
			stream->iostream.error);
		stream->istream.stream_errno = EINVAL;
	}

	ret = pos > stream->pos ? (ssize_t)(pos - stream->pos) :
		(ret == 0 ? 0 : -1);
	stream->pos = pos;
	i_assert(ret != -1 || stream->istream.eof ||
		 stream->istream.stream_errno != 0);
	return ret;
}

static int
i_stream_sized_stat(struct istream_private *stream, bool exact ATTR_UNUSED)
{
	struct sized_istream *sstream = (struct sized_istream *)stream;
	const struct stat *st;

	/* parent stream may be base64-decoder. don't waste time decoding the
	   entire stream, since we already know what the size is supposed
	   to be. */
	if (i_stream_stat(stream->parent, FALSE, &st) < 0) {
		stream->istream.stream_errno = stream->parent->stream_errno;
		return -1;
	}

	stream->statbuf = *st;
	stream->statbuf.st_size = sstream->size;
	return 0;
}

static struct sized_istream *
i_stream_create_sized_common(struct istream *input, uoff_t size)
{
	struct sized_istream *sstream;

	sstream = i_new(struct sized_istream, 1);
	sstream->size = size;
	sstream->istream.max_buffer_size = input->real_stream->max_buffer_size;

	sstream->istream.iostream.destroy = i_stream_sized_destroy;
	sstream->istream.read = i_stream_sized_read;
	sstream->istream.stat = i_stream_sized_stat;

	sstream->istream.istream.readable_fd = input->readable_fd;
	sstream->istream.istream.blocking = input->blocking;
	sstream->istream.istream.seekable = input->seekable;
	(void)i_stream_create(&sstream->istream, input,
			      i_stream_get_fd(input));
	return sstream;
}

struct istream *i_stream_create_sized(struct istream *input, uoff_t size)
{
	struct sized_istream *sstream;

	sstream = i_stream_create_sized_common(input, size);
	sstream->error_callback = i_stream_create_sized_default_error_callback;
	sstream->error_context = sstream;
	return &sstream->istream.istream;
}

#undef i_stream_create_sized_with_callback
struct istream *
i_stream_create_sized_with_callback(struct istream *input, uoff_t size,
				    istream_sized_callback_t *error_callback,
				    void *context)
{
	struct sized_istream *sstream;

	sstream = i_stream_create_sized_common(input, size);
	sstream->error_callback = error_callback;
	sstream->error_context = context;
	return &sstream->istream.istream;
}
