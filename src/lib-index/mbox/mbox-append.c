/* Copyright (C) 2002 Timo Sirainen */

#include "lib.h"
#include "ioloop.h"
#include "iobuffer.h"
#include "hex-binary.h"
#include "md5.h"
#include "mbox-index.h"
#include "mail-index-util.h"

static MailIndexRecord *mail_index_record_append(MailIndex *index,
						 time_t internal_date,
						 unsigned int *uid)
{
	MailIndexRecord trec, *rec;

	memset(&trec, 0, sizeof(MailIndexRecord));
	trec.internal_date = internal_date;

	rec = &trec;
	if (!index->append(index, &rec, uid))
		return NULL;

	return rec;
}

static void mbox_read_message(IOBuffer *inbuf)
{
	unsigned char *msg;
	size_t i, size, startpos;
	int lastmsg;

	/* read until "[\r]\nFrom " is found */
	startpos = i = 0; lastmsg = TRUE;
	while (io_buffer_read_data(inbuf, &msg, &size, startpos) >= 0) {
		for (i = startpos; i < size; i++) {
			if (msg[i] == ' ' && i >= 5) {
				/* See if it's space after "From" */
				if (msg[i-5] == '\n' && msg[i-4] == 'F' &&
				    msg[i-3] == 'r' && msg[i-2] == 'o' &&
				    msg[i-1] == 'm') {
					/* yes, see if we had \r too */
					i -= 5;
					if (i > 0 && msg[i-1] == '\r')
						i--;
					break;
				}
			}
		}

		if (i < size) {
			startpos = i;
                        lastmsg = FALSE;
			break;
		}

		if (i > 0) {
			startpos = i < 7 ? i : 7;
			i -= startpos;

			io_buffer_skip(inbuf, i);
		}
	}

	if (lastmsg && startpos > 0) {
		/* end of file, remove the last [\r]\n */
		msg = io_buffer_get_data(inbuf, &size);
		if (size == startpos) {
			if (msg[startpos-1] == '\n')
				startpos--;
			if (startpos > 0 && msg[startpos-1] == '\r')
				startpos--;
		}
	}

	io_buffer_skip(inbuf, startpos);
}

static int mbox_index_append_next(MailIndex *index, IOBuffer *inbuf)
{
	MailIndexRecord *rec;
	MailIndexUpdate *update;
        MboxHeaderContext ctx;
	time_t internal_date;
	uoff_t abs_start_offset, stop_offset, old_size;
	unsigned char *data, md5_digest[16];
	unsigned int uid;
	size_t size, pos;
	int failed;

	/* get the From-line */
	pos = 0;
	while (io_buffer_read_data(inbuf, &data, &size, pos) >= 0) {
		for (; pos < size; pos++) {
			if (data[pos] == '\n')
				break;
		}

		if (pos < size)
			break;
	}

	if (pos == size || size <= 5 || strncmp(data, "From ", 5) != 0) {
		/* a) no \n found, or line too long
		   b) not a From-line */
		index_set_error(index, "Error indexing mbox file %s: "
				"From-line not found where expected",
				index->mbox_path);
		index->set_flags |= MAIL_INDEX_FLAG_FSCK;
		return FALSE;
	}

	/* parse the From-line */
	internal_date = mbox_from_parse_date(data, size);
	if (internal_date <= 0)
		internal_date = ioloop_time;

	io_buffer_skip(inbuf, pos+1);
	abs_start_offset = inbuf->start_offset + inbuf->offset;

	/* now, find the ending "[\r]\nFrom " */
	mbox_read_message(inbuf);
	stop_offset = inbuf->offset;

	/* add message to index */
	rec = mail_index_record_append(index, internal_date, &uid);
	if (rec == NULL)
		return FALSE;

	update = index->update_begin(index, rec);

	/* location = offset to beginning of headers in message */
	index->update_field_raw(update, FIELD_TYPE_LOCATION,
				&abs_start_offset, sizeof(uoff_t));

	/* parse the header and cache wanted fields. get the message flags
	   from Status and X-Status fields. temporarily limit the buffer size
	   so the message body is parsed properly (FIXME: does this have
	   side effects?) */
	mbox_header_init_context(&ctx, index);

        old_size = inbuf->size;
	inbuf->size = stop_offset;
	io_buffer_seek(inbuf, abs_start_offset - inbuf->start_offset);

	mail_index_update_headers(update, inbuf, 0, mbox_header_func, &ctx);

	inbuf->size = old_size;
	io_buffer_seek(inbuf, stop_offset);

	/* save MD5 */
	md5_final(&ctx.md5, md5_digest);
	index->update_field_raw(update, FIELD_TYPE_MD5,
				md5_digest, sizeof(md5_digest));

	if (!index->update_end(update))
		failed = TRUE;
	else {
		/* save message flags */
		rec->msg_flags = ctx.flags;
		mail_index_mark_flag_changes(index, rec, 0, rec->msg_flags);
		failed = FALSE;

		/* make sure everything is written before setting it's UID
		   to mark it as non-deleted. */
		if (!mail_index_fmsync(index, index->mmap_length))
			return FALSE;
		rec->uid = uid;
	}

	mbox_header_free_context(&ctx);

	return !failed;
}

int mbox_index_append(MailIndex *index, IOBuffer *inbuf)
{
	if (inbuf->offset == inbuf->size) {
		/* no new data */
		return TRUE;
	}

	if (!index->set_lock(index, MAIL_LOCK_EXCLUSIVE))
		return FALSE;

	for (;;) {
		if (inbuf->start_offset + inbuf->offset != 0) {
			/* we're at the [\r]\n before the From-line,
			   skip it */
			if (!mbox_skip_crlf(inbuf)) {
				index_set_error(index,
						"Error indexing mbox file %s: "
						"LF not found where expected",
						index->mbox_path);

				index->set_flags |= MAIL_INDEX_FLAG_FSCK;
				return FALSE;
			}
		}

		if (inbuf->offset == inbuf->size)
			break;

		if (!mbox_index_append_next(index, inbuf))
			return FALSE;
	}

	return TRUE;
}
