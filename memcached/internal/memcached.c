/*
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <stdio.h>
#include <unistd.h>
#include <inttypes.h>
#include <stdbool.h>
#include <tarantool.h>
#include <msgpuck/msgpuck.h>
#include <small/ibuf.h>
#include <small/obuf.h>

#include "memcached.h"
#include "memcached_layer.h"

#include "error.h"
#include "network.h"
#include "proto_binary.h"
#include "proto_text.h"

static inline int
memcached_skip_request(struct memcached_connection *con) {
	struct ibuf *in = con->in;
	while (ibuf_used(in) < con->len && con->noprocess) {
		con->len -= ibuf_used(in);
		ibuf_reset(in);
		ssize_t read = mnet_read_ibuf(con->fd, in, 1);
		if (read == -1)
			memcached_error_ENOMEM(1, "mnet_read_ibuf", "ibuf");
		if (read < 1) {
			return -1;
		}
		con->cfg->stat.bytes_read += read;
	}
	in->rpos += con->len;
	return 0;
}

static inline ssize_t
memcached_flush(struct memcached_connection *con) {
	ssize_t total = mnet_writev(con->fd, con->out->iov,
				    obuf_iovcnt(con->out),
				    obuf_size(con->out));
	con->cfg->stat.bytes_written += total;
	if (ibuf_used(con->in) == 0)
		ibuf_reset(con->in);
	obuf_reset(con->out);
	if (ibuf_reserve(con->in, con->cfg->readahead) == NULL)
		return -1;
	return total;
}

static inline int
memcached_loop_read(struct memcached_connection *con, size_t to_read)
{
	if (ibuf_reserve(con->in, to_read) == NULL) {
/*		memcached_error_ENOMEM(to_read,"memcached_loop_read","ibuf");*/
		return -1;
	}
	ssize_t read = mnet_read_ibuf(con->fd, con->in, to_read);
	if (read == -1)
		memcached_error_ENOMEM(to_read, "mnet_read_ibuf", "ibuf");
	if (read < (ssize_t )to_read) {
		return -1;
	}
	con->cfg->stat.bytes_read += read;
	return 0;
}

static inline int
memcached_loop_error(struct memcached_connection *con) {
	box_error_t *error = box_error_last();
	if (!error) return 0;
	int errcode = box_error_code(error);
	const char *errstr = box_error_message(error);
	if (errcode > box_error_code_MAX) {
		errcode -= box_error_code_MAX;
		/* TODO proper retval checking */
		con->cb.process_error(con, errcode, errstr);
	} else {
		/* TODO proper retval checking */
		memcached_error_SERVER_ERROR(
				"SERVER ERROR %d: %s", errcode, errstr);
	}
	return 0;
}

static inline void
memcached_loop(struct memcached_connection *con)
{
	int rc = 0;
	size_t to_read = 24;
	int batch_count = 0;

	for (;;) {
		rc = memcached_loop_read(con, to_read);
		if (rc == -1) {
			/**
			 * We can't read input (OOM or SocketError)
			 * We're closing connection anyway and don't reply.
			 */
			break;
		}
		to_read = 24;
next:
		con->noreply = false;
		con->noprocess = false;
		rc = con->cb.parse_request(con);
		if (rc == -1) {
			memcached_loop_error(con);
			con->write_end = obuf_create_svp(con->out);
			memcached_skip_request(con);
			if (con->close_connection) {
				/* If magic is wrong we'll close connection */
				break;
			}
			memcached_flush(con);
			continue;
		} else if (rc > 0) {
			to_read = rc;
			continue;
		}
		assert(!con->close_connection);
		rc = 0;
		if (!con->noprocess) rc = con->cb.process_request(con);
		con->write_end = obuf_create_svp(con->out);
		memcached_skip_request(con);
		if (rc == -1)
			memcached_loop_error(con);
		if (con->close_connection) {
			say_debug("Requesting exit. Exiting.");
			break;
		} else if (rc == 0 && ibuf_used(con->in) > 0 &&
			   batch_count < con->cfg->batch_count) {
			batch_count++;
			goto next;
		}
		batch_count = 0;
		/* Write back answer */
		if (!con->noreply)
			memcached_flush(con);
		continue;
	}
	memcached_flush(con);
}

void
memcached_handler(struct memcached_service *p, int fd)
{
	struct memcached_connection con;
	/* TODO: move to connection_init */
	memset(&con, 0, sizeof(struct memcached_connection));
	con.fd        = fd;
	con.in        = ibuf_new();
	con.out       = obuf_new();
	con.write_end = obuf_create_svp(con.out);
	con.cfg       = p;

	/* read-write cycle */
	con.cfg->stat.curr_conns++;
	con.cfg->stat.total_conns++;
//	memcached_set_binary(&con);
	memcached_set_text(&con);
	memcached_loop(&con);
	con.cfg->stat.curr_conns--;
	close(con.fd);
	iobuf_delete(con.in, con.out);
	const box_error_t *err = box_error_last();
	if (err)
		say_error("%s", box_error_message(err));
}

int
memcached_expire_process(struct memcached_service *p, box_iterator_t **iterp)
{
	box_iterator_t *iter = *iterp;
	box_tuple_t *tpl = NULL;
	box_txn_begin();
	int i = 0;
	for (i = 0; i < p->expire_count; ++i) {
		if (box_iterator_next(iter, &tpl) == -1) {
			box_txn_rollback();
			return -1;
		} else if (tpl == NULL) {
			box_iterator_free(iter);
			box_txn_commit();
			*iterp = NULL;
			return 0;
		} else if (is_expired_tuple(p, tpl)) {
			uint32_t klen = 0;
			const char *kpos = box_tuple_field(tpl, 0);
			            kpos = mp_decode_str(&kpos, &klen);
			size_t sz   = mp_sizeof_array(1) + mp_sizeof_str(klen);
			char *begin = (char *)box_txn_alloc(sz);
			if (begin == NULL) {
				box_txn_rollback();
				memcached_error_ENOMEM(sz,
						"memcached_expire_process",
						"key");
				return -1;
			}
			char *end = mp_encode_array(begin, 1);
			      end = mp_encode_str(end, kpos, klen);
			if (box_delete(p->space_id, 0, begin, end, NULL)) {
				box_txn_rollback();
				return -1;
			}
			p->stat.evictions++;
		}
	}
	box_txn_commit();
	return 0;
}

void
memcached_expire_loop(va_list ap)
{
	struct memcached_service *p = va_arg(ap, struct memcached_service *);
	char key[2], *key_end = mp_encode_array(key, 0);
	box_iterator_t *iter = NULL;
	int rv = 0;
	say_info("Memcached expire fiber started");
restart:
	if (iter == NULL) {
		iter = box_index_iterator(p->space_id, 0, ITER_ALL, key, key_end);
	}
	if (rv == -1 || iter == NULL) {
		const box_error_t *err = box_error_last();
		say_error("Unexpected error %u: %s",
				box_error_code(err),
				box_error_message(err));
		goto finish;
	}
	rv = memcached_expire_process(p, &iter);

	/* This part is where we rest after all deletes */
	double delay = ((double )p->expire_count * p->expire_time) /
			(box_index_len(p->space_id, 0) + 1);
	if (delay > 1) delay = 1;
	fiber_set_cancellable(true);
	fiber_sleep(delay);
	if (fiber_is_cancelled())
		goto finish;
	fiber_set_cancellable(false);

	goto restart;
finish:
	if (iter) box_iterator_free(iter);
	return;
}

int
memcached_expire_start(struct memcached_service *p)
{
	if (p->expire_fiber != NULL)
		return -1;
	struct fiber *expire_fiber = NULL;
	char name[128];
	snprintf(name, 128, "%s_memcached_expire", p->name);
	expire_fiber = fiber_new(name, memcached_expire_loop);
	const box_error_t *err = box_error_last();
	if (err) {
		say_error("Can't start the expire fiber");
		say_error("%s", box_error_message(err));
		return -1;
	}
	p->expire_fiber = expire_fiber;
	fiber_set_joinable(expire_fiber, true);
	fiber_start(expire_fiber, p);
	return 0;
}

void
memcached_expire_stop(struct memcached_service *p)
{
	if (p->expire_fiber == NULL) return;
	fiber_cancel(p->expire_fiber);
	fiber_join(p->expire_fiber);
	p->expire_fiber = NULL;
}

struct memcached_service*
memcached_create(const char *name, uint32_t sid)
{
	iobuf_mempool_create();
	struct memcached_service *srv = (struct memcached_service *)
		calloc(1, sizeof(struct memcached_service));
	if (!srv) {
		say_syserror("failed to allocate memory for memcached service");
		return NULL;
	}
	srv->batch_count    = 20;
	srv->expire_enabled = true;
	srv->expire_count   = 50;
	srv->expire_time    = 3600;
	srv->expire_fiber   = NULL;
	srv->space_id       = sid;
	srv->name           = strdup(name);
	srv->cas            = 1;
	srv->readahead      = 16384;
	if (!srv->name) {
		say_syserror("failed to allocate memory for memcached service");
		free(srv);
		return NULL;
	}
	return srv;
}

void
memcached_free(struct memcached_service *srv)
{
	memcached_stop(srv);
	if (srv) free((void *)srv->name);
	free(srv);
}

int
memcached_start (struct memcached_service *srv)
{
	if (memcached_expire_start(srv) == -1)
		return -1;
	return 0;
}

void
memcached_stop (struct memcached_service *srv)
{
	memcached_expire_stop(srv);
	while (srv->stat.curr_conns != 0)
		fiber_sleep(0.001);
}

void
memcached_set_opt (struct memcached_service *srv, int opt, ...)
{
	va_list va; va_start(va, opt);
	switch (opt) {
	case MEMCACHED_OPT_READAHEAD:
		srv->readahead = va_arg(va, int);
		break;
	case MEMCACHED_OPT_EXPIRE_ENABLED: {
		int flag = va_arg(va, int);
		if (flag == 0) {
			srv->expire_enabled = false;
			memcached_expire_stop(srv);
		} else {
			srv->expire_enabled = true;
		}
		break;
	}
	case MEMCACHED_OPT_EXPIRE_COUNT:
		srv->expire_count = va_arg(va, uint32_t);
		break;
	case MEMCACHED_OPT_EXPIRE_TIME:
		srv->expire_time = va_arg(va, uint32_t);
		break;
	case MEMCACHED_OPT_FLUSH_ENABLED: {
		int flag = va_arg(va, int);
		if (flag == 0) {
			srv->flush_enabled = false;
		} else {
			srv->flush_enabled = true;
		}
		break;
	}
	case MEMCACHED_OPT_VERBOSITY: {
		int flag = va_arg(va, int);
		if (flag > 0) {
			srv->verbosity = (flag < 4 ? flag : 3);
		} else if (flag > 3) {
			srv->verbosity = 0;
		}
	}
	default:
		say_error("No such option %d", opt);
		break;
	}
	va_end(va);
}

struct memcached_stat *memcached_get_stat (struct memcached_service *srv)
{
	return &(srv->stat);
}
