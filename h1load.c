/*
 * H1LOAD - simple HTTP/1 load generator
 *
 * Copyright (C) 2000-2020 Willy Tarreau - w@1wt.eu
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#define _GNU_SOURCE /* for F_SETPIPE_SZ */
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/user.h>
#include <sys/epoll.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* some platforms do not provide PAGE_SIZE */
#ifndef PAGE_SIZE
#define PAGE_SIZE sysconf(_SC_PAGESIZE)
#endif

#ifndef MAXTHREADS
#define MAXTHREADS 64
#endif

/* some useful types */
struct list {
	struct list *n, *p;
};

#define LIST_INIT(lh)         ((lh)->n = (lh)->p = (lh))
#define LIST_ISEMPTY(lh)      ((lh)->n == (lh))
#define LIST_APPEND(lh, el)   ({ (el)->p = (lh)->p; (el)->p->n = (lh)->p = (el); (el)->n = (lh); (el); })
#define LIST_DELETE(el)       ({ typeof(el) __ret = (el); (el)->n->p = (el)->p; (el)->p->n = (el)->n; (__ret); })
#define LIST_ELEM(lh, pt, el) ((pt)(((void *)(lh)) - ((void *)&((pt)0)->el)))
#define LIST_NEXT(lh, pt, el) (LIST_ELEM((lh)->n, pt, el))
#define LIST_DEL_INIT(el)  \
   ({ typeof(el) __ret = (el); typeof(__ret->n) __n = __ret->n; typeof(__ret->p) __p = __ret->p; \
      __n->p = __p; __p->n = __n; __ret->n = __ret->p = __ret; __ret; })

/* an error message returned to a caller */
struct errmsg {
	char *msg;
	size_t size;
	size_t len;
};


/* connection flags */
#define CF_BLKW 0x00000001    // blocked on writes
#define CF_BLKR 0x00000002    // blocked on reads
#define CF_POLW 0x00000004    // subscribed to polling for writing
#define CF_POLR 0x00000008    // subscribed to polling for reading
#define CF_ERR  0x00000010    // I/O error reported
#define CF_HEAD 0x00000020    // a HEAD request was last sent
#define CF_V11  0x00000040    // HTTP/1.1 used for the response
#define CF_EXP  0x00000080    // task expired in a wait queue
#define CF_CHNK 0x00000100    // chunked encoding

/* connection states */
enum cstate {
	CS_NEW = 0,   // just allocated
	CS_CON,       // pending connection attempt
	CS_REQ,       // count a new request and check vs global limits
	CS_SND,       // send attempt (headers or body) (a req is active)
	CS_RCV,       // recv attempt (headers or body) (a req is active)
	CS_THK,       // think time
	CS_END        // finished, must be freed
};

/* describes a connection */
struct conn {
	struct list link;            // empty/io queue/think queue/run queue
	struct timeval expire;       // next expiration date
	uint32_t flags;              // CF_*
	enum cstate state;           // CS_*
	int fd;                      // associated FD
	uint64_t to_recv;            // bytes left to receive; 0=headers; ~0=unlimited
	uint64_t tot_req;            // total requests on this connection
	uint64_t chnk_size;          // current chunk size being parsed
	struct timeval req_date;     // moment the request was sent
};

/* one thread */
struct thread {
	struct list wq;              // wait queue: I/O
	struct list sq;              // sleep queue: sleep
	struct list rq;              // run queue: tasks to call
	struct list iq;              // idle queue: when not anywhere else
	struct timeval now;          // current time
	uint32_t cur_req;            // number of active requests
	/* 32-bit hole here */
	uint32_t curconn;            // number of active connections
	uint32_t maxconn;            // max number of active connections
	uint64_t tot_conn;           // total conns attempted on this thread
	uint64_t tot_req;            // total requests started on this thread
	uint64_t tot_done;           // total requests finished (successes+failures)
	uint64_t tot_sent;           // total bytes sent on this thread
	uint64_t tot_rcvd;           // total bytes received on this thread
	uint64_t tot_serr;           // total socket errors on this thread
	uint64_t tot_cerr;           // total connection errors on this thread
	uint64_t tot_xerr;           // total xfer errors on this thread
	uint64_t tot_perr;           // total protocol errors on this thread
	uint64_t tot_cto;            // total connection timeouts on this thread
	uint64_t tot_xto;            // total xfer timeouts on this thread
	uint64_t tot_fbs;            // total number of ttfb samples
	uint64_t tot_ttfb;           // total time-to-first-byte (us)
	uint64_t tot_lbs;            // total number of ttlb samples
	uint64_t tot_ttlb;           // total time-to-last-byte (us)
	int epollfd;                 // poller's FD
	int start_len;               // request's start line's length
	char *start_line;            // copy of the request's start line to be sent
	char *hdr_block;             // copy of the request's header block to be sent
	int hdr_len;                 // request's header block's length
	int tid;                     // thread number
	struct timeval start_date;   // thread's start date
	pthread_t pth;               // the pthread descriptor
	struct sockaddr_storage dst; // destination address
	struct epoll_event *events;  // event buffer
	__attribute__((aligned(64))) union { } __pad;
};


/* common constants for setsockopt() */
const int zero = 0;
const int one = 1;

/* default settings */
const int pollevents = 100;

/* command line arguments */
int arg_conn = 1;     // concurrent conns
int arg_rcon = -1;    // max requests per conn
long arg_reqs = -1;   // max total requests
int arg_thnk = 0;     // think time (ms)
int arg_thrd = 1;     // number of threads
int arg_wait = 10000; // I/O time out (ms)
int arg_verb = 0;     // verbosity
int arg_fast = 0;     // merge send with connect's ACK
int arg_head = 0;     // use HEAD
int arg_dura = 0;     // test duration in sec if non-nul
int arg_host = 0;     // set if host was passed in a header
int arg_ovre = 0;     // overhead correction, extra bytes
int arg_ovrp = 0;     // overhead correction, per-payload size
int arg_slow = 0;     // slow start: delay in milliseconds
int arg_serr = 0;     // stop on first error
char *arg_url;
char *arg_hdr;

static char *start_line;
static char *hdr_block;

/* global state */
#define THR_STOP_ALL 0x80000000  // set on running: must stop now!
#define THR_ENDING   0x40000000  // set on running: end once done
#define THR_SYNSTART 0x20000000  // set on running: threads wait for 0
#define THR_COUNT    0x0FFFFFFF  // mask applied to check thr count
volatile uint32_t running = 0; // # = running threads + THR_* above
struct thread threads[MAXTHREADS];
struct timeval start_date, stop_date, now;

volatile unsigned long global_req = 0; // global req counter to sync threads.

/* current thread */
__thread struct thread *thr;
__thread char buf[65536];


/************ time manipulation functions ***************/

/* timeval is not set */
#define TV_UNSET ((struct timeval){ .tv_sec = 0, .tv_usec = ~0 })

/* make a timeval from <sec>, <usec> */
static inline struct timeval tv_set(time_t sec, suseconds_t usec)
{
	struct timeval ret = { .tv_sec = sec, .tv_usec = usec };
	return ret;
}

/* used to unset a timeout */
static inline struct timeval tv_unset()
{
	return tv_set(0, ~0);
}

/* used to zero a timeval */
static inline struct timeval tv_zero()
{
	return tv_set(0, 0);
}

/* returns true if the timeval is set */
static inline int tv_isset(struct timeval tv)
{
	return tv.tv_usec != ~0;
}

/* returns the interval in microseconds, which must be set */
static inline uint64_t tv_us(const struct timeval tv)
{
	return tv.tv_sec * (uint64_t)1000000 + tv.tv_usec;
}

/* returns true if <a> is before <b>, taking account unsets */
static inline int tv_isbefore(const struct timeval a, const struct timeval b)
{
	return !tv_isset(b) ? 1 :
	       !tv_isset(a) ? 0 :
	       ( a.tv_sec < b.tv_sec || (a.tv_sec == b.tv_sec && a.tv_usec < b.tv_usec));
}

/* returns the lowest of the two timers, for use in delay computation */
static inline struct timeval tv_min(const struct timeval a, const struct timeval b)
{
	if (tv_isbefore(a, b))
		return a;
	else
		return b;
}

/* returns the normalized sum of the <from> plus <off> */
static inline struct timeval tv_add(const struct timeval from, const struct timeval off)
{
	struct timeval ret;

	ret.tv_sec  = from.tv_sec  + off.tv_sec;
	ret.tv_usec = from.tv_usec + off.tv_usec;

	if (ret.tv_usec >= 1000000) {
		ret.tv_usec -= 1000000;
		ret.tv_sec  += 1;
	}
	return ret;
}

/* returns the normalized sum of <from> plus <ms> milliseconds */
static inline struct timeval tv_ms_add(const struct timeval from, unsigned int ms)
{
	struct timeval tv;

	tv.tv_usec = from.tv_usec + (ms % 1000) * 1000;
	tv.tv_sec  = from.tv_sec  + (ms / 1000);
	if (tv.tv_usec >= 1000000) {
		tv.tv_usec -= 1000000;
		tv.tv_sec++;
	}
	return tv;
}

/* returns the delay between <past> and <now> or zero if <past> is after <now> */
static inline struct timeval tv_diff(const struct timeval past, const struct timeval now)
{
	struct timeval ret = { .tv_sec = 0, .tv_usec = 0 };

	if (tv_isbefore(past, now)) {
		ret.tv_sec  = now.tv_sec  - past.tv_sec;
		ret.tv_usec = now.tv_usec - past.tv_usec;

		if ((signed)ret.tv_usec < 0) { // overflow
			ret.tv_usec += 1000000;
			ret.tv_sec  -= 1;
		}
	}
	return ret;
}

/* returns the time remaining between <tv1> and <tv2>, or zero if passed */
static inline struct timeval tv_remain(const struct timeval tv1, const struct timeval tv2)
{
	struct timeval tv;

	tv.tv_usec = tv2.tv_usec - tv1.tv_usec;
	tv.tv_sec  = tv2.tv_sec  - tv1.tv_sec;
	if ((signed)tv.tv_sec > 0) {
		if ((signed)tv.tv_usec < 0) {
			tv.tv_usec += 1000000;
			tv.tv_sec--;
		}
	} else if (tv.tv_sec == 0) {
		if ((signed)tv.tv_usec < 0)
			tv.tv_usec = 0;
	} else {
		tv.tv_sec = 0;
		tv.tv_usec = 0;
	}
	return tv;
}

/* returns the time remaining between <tv1> and <tv2> in milliseconds rounded
 * up to the next millisecond, or zero if passed.
 */
static inline unsigned long tv_ms_remain(const struct timeval tv1, const struct timeval tv2)
{
	struct timeval tv;

	tv = tv_remain(tv1, tv2);
	return tv.tv_sec * 1000 + (tv.tv_usec + 999) / 1000;
}


/************ connection management **************/

static inline int may_add_req()
{
	unsigned long rq_cnt = global_req;

	if (arg_reqs <= 0)
		return 1;

	do {
		if (rq_cnt >= arg_reqs)
			return 0;
	} while (!__atomic_compare_exchange_n(&global_req, &rq_cnt, rq_cnt + 1,
	                                      0, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
	return 1;
}

/* updates polling on epoll FD <ep> for fd <fd> supposed to match connection
 * flags <flags>.
 */
void update_poll(int ep, int fd, uint32_t flags, void *ptr)
{
	struct epoll_event ev;
	int op;

	ev.data.ptr = ptr;
	ev.events = ((flags & CF_BLKW) ? EPOLLOUT : 0) | ((flags & CF_BLKR) ? EPOLLIN : 0);
	if (!(flags & (CF_POLR | CF_POLW)))
		op = EPOLL_CTL_ADD;
	else if (!(flags & (CF_POLR | CF_POLW)))
		op = EPOLL_CTL_DEL;
	else
		op = EPOLL_CTL_MOD;

	epoll_ctl(ep, op, fd, &ev);
}

/* update epoll_fd <ep> for conn <conn>, adding flag <add> and removing <del> */
static inline void update_conn(int ep, struct conn *conn)
{
	uint32_t flags = conn->flags;

	if ((!(flags & CF_BLKW) ^ !(flags & CF_POLW)) |
	    (!(flags & CF_BLKR) ^ !(flags & CF_POLR))) {
		update_poll(ep, conn->fd, flags, conn);
		if (conn->flags & CF_BLKW)
			conn->flags |= CF_POLW;
		else
			conn->flags &= ~CF_POLW;

		if (conn->flags & CF_BLKR)
			conn->flags |= CF_POLR;
		else
			conn->flags &= ~CF_POLR;
	}
}

static inline void cant_send(struct conn *conn)
{
	conn->flags |= CF_BLKW;
}

static inline void cant_recv(struct conn *conn)
{
	conn->flags |= CF_BLKR;
}

static inline void stop_send(struct conn *conn)
{
	conn->flags &= ~CF_BLKW;
}

static inline void stop_recv(struct conn *conn)
{
	conn->flags &= ~CF_BLKR;
}

static inline void may_send(struct conn *conn)
{
	conn->flags &= ~CF_BLKW;
}

static inline void may_recv(struct conn *conn)
{
	conn->flags &= ~CF_BLKR;
}

struct conn *new_conn()
{
	struct conn *conn;

	conn = malloc(sizeof(struct conn));
	if (conn) {
		conn->flags = 0;
		conn->state = CS_NEW;
		conn->expire = tv_unset();
		conn->req_date = tv_unset();
		conn->tot_req = 0;
	}
	return conn;
}

/* Try to establish a connection to t->dst. Return the conn or NULL in case of error */
struct conn *add_connection(struct thread *t)
{
	struct conn *conn;

	conn = new_conn();
	if (!conn)
		goto fail_conn;

	conn->fd = socket(AF_INET, SOCK_STREAM, 0);
	if (conn->fd < 0)
		goto fail_sock;

	if (fcntl(conn->fd, F_SETFL, O_NONBLOCK) == -1)
		goto fail_setup;

	if (setsockopt(conn->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == -1)
		goto fail_setup;

	if (setsockopt(conn->fd, SOL_TCP, TCP_NODELAY, &one, sizeof(one)) == -1)
		goto fail_setup;

	if (arg_fast && setsockopt(conn->fd, SOL_TCP, TCP_QUICKACK, &zero, sizeof(zero)) == -1)
		goto fail_setup;

	if (connect(conn->fd, (struct sockaddr *)&t->dst, sizeof(t->dst)) < 0) {
		if (errno != EINPROGRESS)
			goto fail_setup;
		cant_send(conn);
		conn->state = CS_CON;
		LIST_APPEND(&t->iq, &conn->link);
	}
	else {
		conn->state = CS_REQ;
		LIST_APPEND(&t->iq, &conn->link);
	}

	t->curconn++;
	t->tot_conn++;
	return conn;

 fail_setup:
	close(conn->fd);
 fail_sock:
	free(conn);
 fail_conn:
	if (arg_serr)
		__sync_fetch_and_or(&running, THR_STOP_ALL);
	t->tot_serr++;
	return NULL;
}

/* parse HTTP response in <buf> of len <len>. Returns <0 on error (incl too
 * short), or the number of bytes of headers block on success.
 */
int parse_resp(struct conn *conn, char *buf, int len)
{
	int ver;
	int status;
	uint64_t cl = 0;
	int do_close = 0;
	char *p, *hdr, *col, *eol, *end;

	if (len < 13)
		goto too_short;

	if (memcmp(buf, "HTTP/1.", 7) != 0)
		return -1;

	ver = buf[7] - '0';
	if (ver < 0 || ver > 1)
		return -1;
	if (ver > 0)
		conn->flags |= CF_V11;
	do_close = !ver;

	if (buf[8] != ' ')
		return -1;

	if (buf[12] != ' ' && buf[12] != '\r' && buf[12] != '\n')
		return -1;

	status = buf[9] * 100 + buf[10] * 10 + buf[11] - '0' * 111;
	if (status < 100 || status > 599)
		return -1;

	end = buf + len;
	p = buf + 13;

	while (1) {
		while (1) {
			if (p == end)
				goto too_short;
			if (*p == '\n')
				break;
			p++;
		}

		hdr = ++p;
		while (1) {
			if (p == end)
				goto too_short;
			if (*p == ':' || *p == '\n')
				break;
			p++;
		}

		if (*hdr == '\n' || (p > hdr && *hdr == '\r' && hdr[1] == '\n')) {
			/* EOH */
			p++;
			break;
		}

		/* '\n' without ':' => error */
		if (*p != ':')
			return -1;

		col = p;
		while (1) {
			if (p == end)
				goto too_short;
			if (*p == '\r' || *p == '\n')
				break;
			p++;
		}
		eol = p;

		/* now we have the header name between <hdr> and <col>, and the
		 * value between <col>+1 and <eol>.
		 */

		/* 1/ connection: close or keep-alive */
		if (col - hdr == 10 && strncasecmp(hdr, "connection", 10) == 0) {
			for (p = col + 1; p <= eol - 5; p++) {
				int l = eol - p;

				l = eol - p < 10 ? eol - p : 10;
				if ((*p == 'k' || *p == 'K') && strncasecmp(p, "keep-alive", l) == 0) {
					do_close = 0;
					p += l;
					continue;
				}

				l = eol - p < 5 ? eol - p : 5;
				if ((*p == 'c' || *p == 'C') && strncasecmp(p, "close", l) == 0) {
					do_close = 1;
					p += l;
					continue;
				}
			}
		}

		/* 2/ transfer-encoding (just check for presence) */
		if (col - hdr == 17 && strncasecmp(hdr, "transfer-encoding", 17) == 0) {
			conn->flags |= CF_CHNK;
		}

		if (col - hdr == 14 && !(conn->flags & CF_CHNK) && status != 204 &&
		    status != 304 && strncasecmp(hdr, "content-length", 14) == 0) {
			unsigned char k;
			cl = 0;
			for (p = col + 1; p < eol && *p == ' '; p++)
				;

			while (p < eol) {
				k = *(p++) - '0';
				if (k > 9)
					break;
				cl = cl * 10 + k;
			}
		}
	}

	/* now we have CF_CHNK set if transfer-encoding must be used, cl equal
	 * to the last content-length header parsed, do_close indicating the
	 * desired connection mode, status set to the HTTP status, ver set to
	 * the version (0 or 1). We just have to set the amount of bytes left
	 * to receive. 0=we already have everything. -1=receive till close.
	 * For other cases we deduce what we already have in the buffer.
	 */
	if (do_close)
		conn->to_recv = -1; // close: tunnel
	else if (conn->flags & CF_HEAD || status == 204 || status == 304)
		conn->to_recv = 0;
	else
		conn->to_recv = cl; // content-length
	return p - buf;

 too_short:
	return -1;
}

/* handles I/O and timeouts for connection <conn> on thread <t> */
void handle_conn(struct thread *t, struct conn *conn)
{
	const struct linger nolinger = { .l_onoff = 1, .l_linger = 0 };
	struct iovec iovec[4];
	struct msghdr msghdr = { };
	int nbvec;
	int expired = !!(conn->flags & CF_EXP);
	int loops;
	int ret, parsed;
	uint64_t ttfb, ttlb;     // time-to-first-byte, time-to-last-byte (in us)

	if (conn->state == CS_CON) {
		if (conn->flags & CF_ERR) {
			if (arg_serr)
				__sync_fetch_and_or(&running, THR_STOP_ALL);
			t->tot_cerr++;
			goto close_conn;
		}

		if (conn->flags & CF_BLKW) {
			if (expired) {
				t->tot_cto++;
				goto kill_conn;
			}
			cant_send(conn);
			goto done;
		}

		/* finally ready, fall through */
		conn->state = CS_REQ;
	}

	if (conn->state == CS_REQ) {
	send_again:
		/* check request counters and increment counts exactly once per
		 * request. We silently abort on I/O errors on idle connections
		 * and prepare to send the request on a new connection instead.
		 */
		if (conn->flags & CF_ERR)
			goto close_conn;

		if (!may_add_req())
			goto kill_conn;

		if (running & (THR_STOP_ALL|THR_ENDING))
			goto kill_conn;

		conn->tot_req++;
		t->tot_req++;
		t->cur_req++;
		conn->state = CS_SND;
	}

	if (conn->state == CS_SND) {
		/* try to prepare a request and send it */
		if (conn->flags & CF_ERR) {
			/* only the first request of a connection sees an error
			 * on the brutal close of a keep-alive connection, for
			 * the other one a silent retry is required.
			 */
			if (conn->tot_req == 1) {
				t->tot_xerr++;
				t->tot_done++;
			}
			goto close_conn;
		}

		if (conn->flags & CF_BLKW) {
			if (expired) {
				t->tot_xto++;
				t->tot_done++;
				goto kill_conn;
			}
			cant_send(conn);
			goto wait_io;
		}

		conn->flags &= ~(CF_HEAD | CF_V11 | CF_EXP);
		conn->to_recv = 0; // wait for headers

		/* check for HEAD */
		if (*(uint32_t *)t->start_line == ntohl(0x48454144))
			conn->flags |= CF_HEAD;

		/* finally ready, let's try (again?) */
		nbvec = 0;

		/* start line */
		iovec[nbvec].iov_base = t->start_line;
		iovec[nbvec].iov_len  = t->start_len;
		nbvec++;

		/* header block */
		if (t->hdr_len) {
			iovec[nbvec].iov_base = t->hdr_block;
			iovec[nbvec].iov_len  = t->hdr_len;
			nbvec++;
		}

		if ((arg_rcon > 0 && conn->tot_req == arg_rcon) ||
		    (arg_reqs > 0 && global_req >= arg_reqs)) {
			iovec[nbvec].iov_base = "Connection: close\r\n";
			iovec[nbvec].iov_len = 19;
			nbvec++;
		}

		iovec[nbvec].iov_base = "\r\n";
		iovec[nbvec].iov_len = 2;
		nbvec++;

		msghdr.msg_iov = iovec;
		msghdr.msg_iovlen = nbvec;

		ret = sendmsg(conn->fd, &msghdr, MSG_NOSIGNAL | MSG_DONTWAIT);
		if (ret < 0) {
			if (errno == EAGAIN) {
				cant_send(conn);
				goto wait_io;
			}
			/* only the first request of a connection sees an error
			 * on the brutal close of a keep-alive connection, for
			 * the other one a silent retry is required.
			 */
			if (conn->tot_req == 1) {
				t->tot_xerr++;
				t->tot_done++;
			}
			goto close_conn;
		}

		t->tot_sent += ret;
		conn->req_date = t->now;

		/* nothing more to send, wait for response */
		stop_send(conn);
		conn->state = CS_RCV;

		/* OPTIM: we know an immediate recv will fail, if we're already
		 * subscribed, let's wait for epoll to notify us. This won't
		 * work with EPOLL_ET.
		 */
		if (conn->flags & CF_POLR) {
			cant_recv(conn);
			goto wait_io;
		}
	}


	if (conn->state == CS_RCV) {
		if (conn->flags & CF_ERR) {
			t->tot_xerr++;
			t->tot_done++;
			goto close_conn;
		}

		if (conn->flags & CF_BLKR) {
			if (expired) {
				t->tot_xto++;
				t->tot_done++;
				goto kill_conn;
			}
			cant_recv(conn);
			goto wait_io;
		}

		/* finally ready, let's try (again?) */

		/* For reads smaller than the current buffer size, it's better
		 * to place the data there again, especially in chunked mode
		 * since we may have to read again afterwards. For other cases
		 * we defer to the more efficient loop which uses MSG_TRUNC. Note
		 * that conn->to_recv==0 indicates that we need to parse, thus
		 * we're facing headers or chunk sizes. As we don't do
		 * pipelining we're certain not to receive more than desired.
		 */
		if (conn->to_recv == 0 || (conn->to_recv <= sizeof(buf) && (conn->flags & CF_CHNK))) {
			/* Headers or small chunks expected. For now we assume
			 * we get all headers at once. Later we may use a temp
			 * buffer to store partial contents when that happens.
			 */
			ret = recv(conn->fd, buf, sizeof(buf), MSG_NOSIGNAL | MSG_DONTWAIT);
			if (ret <= 0) {
				if (ret < 0 && errno == EAGAIN) {
					cant_recv(conn);
					goto wait_io;
				}
				t->tot_xerr++;
				t->tot_done++;
				goto close_conn;
			}

			t->tot_rcvd += ret;
			parsed = 0;
			if (!(conn->flags & CF_CHNK)) {
				/* the only case of !to_recv && !CHNK is when
				 * we are waiting for headers
				 */
				parsed = parse_resp(conn, buf, ret);
				if (parsed < 0) {
					t->tot_perr++;
					t->tot_done++;
					goto kill_conn;
				}
				ttfb = tv_us(tv_diff(conn->req_date, t->now));
				t->tot_ttfb += ttfb;
				t->tot_fbs++;

				/* compute how much left is available in the buffer */
				ret -= parsed;
				conn->chnk_size = 0;
			}

			while (ret && conn->to_recv != -1) {
				/* deduce currently bufferred bytes from C-L or previous partial chunk */
				if (conn->to_recv) {
					if (conn->to_recv >= ret) {
						conn->to_recv -= ret;
						ret = 0;
						break;
					}
					else {
						parsed += conn->to_recv;
						ret -= conn->to_recv;
						conn->to_recv = 0;
					}
				}

				/* next data, if any, starts at <buf+parsed> for <ret>
				 * bytes. In practice it's only the case with chunking.
				 */
				if (conn->flags & CF_CHNK && !conn->to_recv) {
					/* !to_recv+CF_CHNK => reading chunk size.
					 * We're using a local pointer to the thread-local
					 * one to avoid heavy thread-local accesses in the
					 * hot loop (~15% difference)!
					 */
					const char *bufptr = buf + parsed;

					while (ret && conn->to_recv < ret) {
						char c = *bufptr++;

						ret--;
						if (c == '\r') {
							/* commit size into to_recv and count +1 for LF and +2
							 * for post-data CRLF. We should have at most 3 bytes
							 * left in the buffer (LF, CR, LF). We'll truncate them
							 * in case there are trailers or extra data.
							 */
							conn->to_recv = conn->chnk_size + 1 + 2;
							if (!conn->chnk_size) { // final chunk
								if (ret > 3)
									ret = 3;
								conn->flags &= ~CF_CHNK;
								break;
							}

							conn->chnk_size = 0;
							if (conn->to_recv <= ret) {
								/* contents still present in buffer */
								bufptr += conn->to_recv;
								ret -= conn->to_recv;
								conn->to_recv = 0;
							}
						}
						else if ((unsigned char)(c - '0') <= 9)
							conn->chnk_size = (conn->chnk_size << 4) + c - '0';
						else if ((unsigned char)((c|0x20) - 'a') <= 6)
							conn->chnk_size = (conn->chnk_size << 4) + (c|0x20) - 'a' + 0xa;
						else {
							t->tot_perr++;
							t->tot_done++;
							goto kill_conn;
						}
					}
					parsed = bufptr - buf;
				}
			}
		}

		loops = 3;
		while (conn->to_recv) {
			uint64_t try = conn->to_recv;
			void *ptr = buf;

			if (loops-- == 0) {
				cant_recv(conn);
				goto wait_io;
			}

			if (MSG_TRUNC) {
				if (try > 1 << 30)
					try = 1 << 30;
				ptr = NULL;
			}
			else if (try == ~0 || try > sizeof(buf))
				try = sizeof(buf);

			ret = recv(conn->fd, ptr, try, MSG_NOSIGNAL | MSG_DONTWAIT | MSG_TRUNC);
			if (ret <= 0) {
				if (ret == 0) {
					/* received a shutdown, might be OK */
					if (conn->to_recv != ~0)
						t->tot_xerr++;
					t->tot_done++;
					t->cur_req--;
					conn->state = CS_END;
					goto close_conn;
				}

				if (errno == EAGAIN) {
					cant_recv(conn);
					goto wait_io;
				}
				t->tot_xerr++;
				t->tot_done++;
				goto close_conn;
			}

			t->tot_rcvd += ret;
			if (conn->to_recv != ~0)
				conn->to_recv -= ret;
		}

		if (conn->flags & CF_CHNK) {
			/* parsing in progress, need more data */
			cant_recv(conn);
			goto wait_io;
		}

		/* we've reached the end */
		ttlb = tv_us(tv_diff(conn->req_date, t->now));
		t->tot_ttlb += ttlb;
		t->tot_lbs++;
		t->tot_done++;

		if (arg_thnk) {
			conn->expire = tv_ms_add(t->now, arg_thnk * (4096 - 128 + rand()%257) / 4096);
			LIST_DELETE(&conn->link);
			LIST_APPEND(&t->sq, &conn->link);
			conn->state = CS_THK;
			t->cur_req--;
		}
		else {
			conn->state = CS_REQ;
			t->cur_req--;
			stop_recv(conn);
			goto send_again;
		}
	}

	if (conn->state == CS_THK) {
		/* continue to monitor the server connection for a possible
		 * close, and wait for the timeout.
		 */
		uint64_t try = 1 << 30;
		void *ptr = buf;

		try = 1 << 30;
		if (MSG_TRUNC) {
			ptr = NULL;
		}
		else if (try > sizeof(buf))
			try = sizeof(buf);

		ret = recv(conn->fd, ptr, try, MSG_NOSIGNAL | MSG_DONTWAIT | MSG_TRUNC);
		if (ret <= 0) {
			if (ret == 0) {
				/* received a shutdown */
				conn->state = CS_END;
				goto close_conn;
			}

			if (errno != EAGAIN)
				goto close_conn;
		}

		if (running & THR_ENDING)
			goto kill_conn;

		if (expired) {
			LIST_DELETE(&conn->link);
			LIST_APPEND(&t->wq, &conn->link);
			conn->expire = tv_ms_add(t->now, arg_wait);
			conn->state = CS_REQ;
			stop_recv(conn);
			goto send_again;
		}

		cant_recv(conn);
		goto done;
	}

	if (conn->state == CS_END) {
		/* it was a close */
		goto close_conn;
	}
	goto done;


 wait_io:
	conn->expire = tv_ms_add(t->now, arg_wait);
	LIST_DELETE(&conn->link);
	LIST_APPEND(&t->wq, &conn->link);
 done:
	update_conn(thr->epollfd, conn);
	return;

 kill_conn:
	setsockopt(conn->fd, SOL_SOCKET, SO_LINGER, &nolinger, sizeof(nolinger));
 close_conn:
	if (conn->state == CS_END) {
		ttlb = tv_us(tv_diff(conn->req_date, t->now));
		t->tot_ttlb += ttlb;
		t->tot_lbs++;
	}

	close(conn->fd);
	if (conn->state == CS_SND || conn->state == CS_RCV)
		t->cur_req--;
	t->curconn--;
	LIST_DELETE(&conn->link);
	free(conn);
}

/* returns the delay till the next event, or zero if none */
unsigned long check_timeouts(struct thread *t, struct list *list)
{
	struct conn *conn;
	unsigned long remain;

	while (!LIST_ISEMPTY(list)) {
		conn = LIST_NEXT(list, typeof(conn), link);
		remain = tv_ms_remain(t->now, conn->expire);
		if (remain)
			return remain;
		conn->flags |= CF_EXP;
		handle_conn(t, conn);
	}
	return 0;
}

void work(void *arg)
{
	struct thread *thread = (struct thread *)arg;
	struct conn *conn;
	int nbev, i;
	uint32_t maxconn;
	unsigned long t1, t2;

	thr = thread;

	__sync_fetch_and_add(&running, 1);
	while (running & THR_SYNSTART)
		usleep(10000);

	while (!(running & THR_STOP_ALL) && (!(running & THR_ENDING) || thr->cur_req)) {
		maxconn = thr->maxconn;

		if (arg_slow) {
			int duration = tv_ms_remain(start_date, now);

			if (duration < arg_slow) {
				maxconn = ((uint64_t)thr->maxconn * duration + arg_slow / 2) / arg_slow;
				maxconn = maxconn ? maxconn : 1;
			} else {
				/* done, don't come back here */
				arg_slow = 0;
			}
		}

		for (i = 0; thr->curconn < maxconn && i < 2*pollevents; i++) {
			if (running & (THR_STOP_ALL|THR_ENDING))
				break;
			if (arg_reqs > 0 && global_req >= arg_reqs)
				break;
			conn = add_connection(thread);
			if (!conn)
				break;
			/* send request or subscribe */
			handle_conn(thr, conn);
		}

		if (arg_reqs > 0 && global_req >= arg_reqs && !thr->cur_req)
			break;

		t1 = 1000; // one call per second
		t2 = check_timeouts(thr, &thr->wq);

		if (t2 && t2 < t1)
			t1 = t2;
		t2 = check_timeouts(thr, &thr->sq);
		if (t2 && t2 < t1)
			t1 = t2;

		if (thr->curconn < maxconn)
			t1 = 0;

		nbev = epoll_wait(thr->epollfd, thr->events, pollevents, t1);
		gettimeofday(&thr->now, NULL);

		for (i = 0; i < nbev; i++) {
			conn = thr->events[i].data.ptr;

			if (thr->events[i].events & (EPOLLIN|EPOLLHUP|EPOLLRDHUP))
				conn->flags &= ~CF_BLKR;

			if (thr->events[i].events & EPOLLOUT)
				conn->flags &= ~CF_BLKW;

			if (thr->events[i].events & EPOLLERR)
				conn->flags |= CF_ERR;

			/* Note: in theory we should pass this through the run
			 * queue, just like timeouts, and then run everything
			 * from there. In practice we theorically never have a
			 * timeout so there's no point optimizing for this rare
			 * case.
			 */
			if ((conn->flags & CF_ERR) || !(conn->flags & (CF_BLKR|CF_BLKW)))
				handle_conn(thr, conn);
		}
	}

	__sync_fetch_and_sub(&running, 1);
	pthread_exit(0);
}

/* display the message and exit with the code */
__attribute__((noreturn)) void die(int code, const char *format, ...)
{
	va_list args;

	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
	exit(code);
}

__attribute__((noreturn)) void usage(const char *name, int code)
{
	die(code,
	    "Usage: %s [option]* URL\n"
	    "\n"
	    "The following arguments are supported :\n"
	    "  -d <time>     test duration in seconds (0)\n"
	    "  -c <conn>     concurrent connections (1)\n"
	    "  -n <reqs>     maximum total requests (-1)\n"
	    "  -r <reqs>     number of requests per connection (-1)\n"
	    "  -s <time>     soft start: time in ms to reach 100%% load\n"
	    "  -t <threads>  number of threads to create (1)\n"
	    "  -w <time>     I/O timeout in milliseconds (-1)\n"
	    "  -T <time>     think time after a response (0)\n"
	    "  -H \"foo:bar\"  adds this header name and value\n"
	    "  -O extra/payl overhead: #extra bytes per payload size\n"
	    "  -e            stop upon first connection error\n"
	    "  -F            merge send() with connect's ACK\n"
	    "  -I            use HEAD instead of GET\n"
	    "  -h            display this help\n"
	    "  -v            increase verbosity\n"
	    "\n"
	    ,name);
}

/* converts str in the form [<ipv4>|<ipv6>|<hostname>]:port to struct sockaddr_storage.
 * Returns < 0 with err set in case of error.
 */
int addr_to_ss(char *str, struct sockaddr_storage *ss, struct errmsg *err)
{
	char *range;

	/* look for the addr/port delimiter, it's the last colon. */
	if ((range = strrchr(str, ':')) == NULL) {
		err->len = snprintf(err->msg, err->size, "Missing port number: '%s'\n", str);
		return -1;
	}

	*range++ = 0;

	memset(ss, 0, sizeof(*ss));

	if (strrchr(str, ':') != NULL) {
		/* IPv6 address contains ':' */
		ss->ss_family = AF_INET6;
		((struct sockaddr_in6 *)ss)->sin6_port = htons(atoi(range));

		if (!inet_pton(ss->ss_family, str, &((struct sockaddr_in6 *)ss)->sin6_addr)) {
			err->len = snprintf(err->msg, err->size, "Invalid server address: '%s'\n", str);
			return -1;
		}
	}
	else {
		ss->ss_family = AF_INET;
		((struct sockaddr_in *)ss)->sin_port = htons(atoi(range));

		if (*str == '*' || *str == '\0') { /* INADDR_ANY */
			((struct sockaddr_in *)ss)->sin_addr.s_addr = INADDR_ANY;
			return 0;
		}

		if (!inet_pton(ss->ss_family, str, &((struct sockaddr_in *)ss)->sin_addr)) {
			struct hostent *he = gethostbyname(str);

			if (he == NULL) {
				err->len = snprintf(err->msg, err->size, "Invalid server name: '%s'\n", str);
				return -1;
			}
			((struct sockaddr_in *)ss)->sin_addr = *(struct in_addr *) *(he->h_addr_list);
		}
	}

	return 0;
}

/* creates and initializes thread <th>, returns <0 on failure. The initial
 * request is supposed to still be in <buf>.
 */
int create_thread(int th, struct errmsg *err, const struct sockaddr_storage *ss)
{
	if (th > MAXTHREADS) {
		err->len = snprintf(err->msg, err->size, "Invalid thread ID %d\n", th);
		return -1;
	}

	memset(&threads[th], 0, sizeof(threads[th]));
	LIST_INIT(&threads[th].wq);
	LIST_INIT(&threads[th].sq);
	LIST_INIT(&threads[th].rq);
	LIST_INIT(&threads[th].iq);
	/* make sure the conns are evenly distributed amon all threads */
	threads[th].maxconn = (arg_conn + th) / arg_thrd;
	memcpy(&threads[th].dst, ss, sizeof(*ss));
	threads[th].tid = th;
	threads[th].events = calloc(1, sizeof(*threads[th].events) * pollevents);
	if (!threads[th].events) {
		err->len = snprintf(err->msg, err->size, "Failed to allocate %d poll_events for thread %d\n", pollevents, th);
		return -1;
	}

	threads[th].start_line = strdup(start_line);
	if (!threads[th].start_line) {
		err->len = snprintf(err->msg, err->size, "Failed to allocate start line for thread %d\n", th);
		return -1;
	}
	threads[th].start_len = strlen(threads[th].start_line);

	if (hdr_block) {
		threads[th].hdr_block = strdup(hdr_block);
		if (!threads[th].hdr_block) {
			err->len = snprintf(err->msg, err->size, "Failed to allocate header block for thread %d\n", th);
			return -1;
		}
		threads[th].hdr_len = strlen(threads[th].hdr_block);
	}

	threads[th].epollfd = epoll_create(1);
	if (threads[th].epollfd < 0) {
		err->len = snprintf(err->msg, err->size, "Failed to initialize epoll_fd for thread %d\n", th);
		return -1;
	}

	if (pthread_create(&threads[th].pth, NULL, (void *)work, &threads[th]) < 0) {
		err->len = snprintf(err->msg, err->size, "Failed to create thread %d\n", th);
		return -1;
	}
	return 0;
}

/* reports a locally allocated string to represent a human-readable positive
 * number on 4 characters (3 digits and a unit, which may be "." for ones) :
 *   XXXu
 *   XXuX
 *   XuXX
 */
const char *human_number(double x)
{
	static char str[5];
	char unit = '.';

	if (x < 0)
		x = -x;

	do {
		if (x == 0.0 || x >= 1.0) break;
		x *= 1000.0; unit = 'm';
		if (x >= 1.0) break;
		x *= 1000.0; unit = 'u';
		if (x >= 1.0) break;
		x *= 1000.0; unit = 'n';
		if (x >= 1.0) break;
		x *= 1000.0; unit = 'p';
		if (x >= 1.0) break;
		x *= 1000.0; unit = 'f';
	} while (0);

	do {
		if (x < 1000.0) break;
		x /= 1000.0; unit = 'k';
		if (x < 1000.0) break;
		x /= 1000.0; unit = 'M';
		if (x < 1000.0) break;
		x /= 1000.0; unit = 'G';
		if (x < 1000.0) break;
		x /= 1000.0; unit = 'T';
		if (x < 1000.0) break;
		x /= 1000.0; unit = 'P';
		if (x < 1000.0) break;
		x /= 1000.0; unit = 'E';
	} while (0);

	if (x < 10.0)
		snprintf(str, sizeof(str), "%d%c%02d", (int)x, unit, (int)((x - (int)x)*100));
	else if (x < 100.0)
		snprintf(str, sizeof(str), "%d%c%d",   (int)x, unit, (int)((x - (int)x)*10));
	else
		snprintf(str, sizeof(str), "%d%c",     (int)x, unit);
	return str;
}

/* reports current date (now) and aggragated stats */
void summary()
{
	int th;
	uint64_t cur_conn, tot_conn, tot_req, tot_err, tot_rcvd, bytes;
	static uint64_t prev_totc, prev_totr, prev_totb;
	static struct timeval prev_date = TV_UNSET;
	double interval;

	cur_conn = tot_conn = tot_req = tot_err = tot_rcvd = 0;
	for (th = 0; th < arg_thrd; th++) {
		cur_conn += threads[th].curconn;
		tot_conn += threads[th].tot_conn;
		tot_req  += threads[th].tot_done;
		tot_err  += threads[th].tot_serr + threads[th].tot_cerr + threads[th].tot_xerr + threads[th].tot_perr;
		tot_rcvd += threads[th].tot_rcvd;
	}

	/* when called after having stopped, check if we need to dump a final
	 * line or not, to cover for the rare cases of the last thread
	 * finishing just after the last summary line
	 */
	if (!(running & THR_COUNT) && (prev_date.tv_sec == now.tv_sec) &&
	     (prev_totc == tot_conn) && (prev_totr == tot_req) && (prev_totb == tot_rcvd))
		return;

	if (tv_isset(prev_date))
		interval = tv_ms_remain(prev_date, now) / 1000.0;
	else
		interval = 1.0;

	printf("%9lu %5lu %8llu %8llu %14llu %6lu ",
	       (unsigned long)now.tv_sec,
	       (unsigned long)cur_conn,
	       (unsigned long long)tot_conn,
	       (unsigned long long)tot_req,
	       (unsigned long long)tot_rcvd,
	       (unsigned long)tot_err);

	bytes = tot_rcvd - prev_totb;
	if (arg_ovrp) {
		long small_pkt = (bytes + (arg_ovrp - 1)) / arg_ovrp;
		/* we need to account for overhead also on small packets and
		 * at minima once per response.
		 */
		if (small_pkt < tot_req  - prev_totr)
			small_pkt = tot_req  - prev_totr;
		bytes += small_pkt * arg_ovre;
	}

	printf("%s ", human_number((tot_conn - prev_totc) / interval));
	printf("%s ", human_number((tot_req  - prev_totr) / interval));
	printf("%s ", human_number(bytes / interval));
	printf("%s ", human_number(bytes * 8 / interval));
	putchar('\n');

	prev_totc = tot_conn;
	prev_totr = tot_req;
	prev_totb = tot_rcvd;
	prev_date = now;
}

/* appends <txt1>, <txt2> and <txt3> to pfx when not NULL. <str> may also be
 * NULL, in this case it will be allocated first. If everything is empty, an
 * empty string will still be returned. NULL is returned on allocation error.
 */
char *str_append(char *str, const char *txt1, const char *txt2, const char *txt3)
{
	size_t len0 = str  ? strlen(str)  : 0;
	size_t len1 = txt1 ? strlen(txt1) : 0;
	size_t len2 = txt2 ? strlen(txt2) : 0;
	size_t len3 = txt3 ? strlen(txt3) : 0;

	str = realloc(str, len0 + len1 + len2 + len3 + 1);
	if (!str)
		return NULL;
	if (len1)
		memcpy(str + len0, txt1, len1);
	if (len2)
		memcpy(str + len0 + len1, txt2, len2);
	if (len3)
		memcpy(str + len0 + len1 + len2, txt3, len3);
	str[len0 + len1 + len2 + len3] = 0;
	return str;
}

int main(int argc, char **argv)
{
	const char *name = argv[0];
	struct sockaddr_storage ss;
	struct errmsg err = { .len = 0, .size = 100, .msg = alloca(100) };
	int req_len;
	char *host;
	char c;
	int th;

	argv++;
	argc--;
	arg_hdr = NULL;
	while (argc > 0) {
		if (**argv != '-')
			break;

		if (strcmp(argv[0], "-c") == 0) {
			if (argc < 2)
				usage(name, 1);
			arg_conn = atoi(argv[1]);
			argv++; argc--;
		}
		else if (strcmp(argv[0], "-H") == 0) {
			if (argc < 2)
				usage(name, 1);
			arg_hdr = str_append(arg_hdr, argv[1], "\r\n", NULL);
			if (!arg_hdr)
				die(1, "memory allocation error for a header\n");
			if (strncasecmp(argv[1], "host:", 5) == 0)
				arg_host = 1;
			argv++; argc--;
		}
		else if (strcmp(argv[0], "-n") == 0) {
			if (argc < 2)
				usage(name, 1);
			arg_reqs = atol(argv[1]);
			argv++; argc--;
		}
		else if (strcmp(argv[0], "-r") == 0) {
			if (argc < 2)
				usage(name, 1);
			arg_rcon = atoi(argv[1]);
			argv++; argc--;
		}
		else if (strcmp(argv[0], "-s") == 0) {
			if (argc < 2)
				usage(name, 1);
			arg_slow = atoi(argv[1]);
			argv++; argc--;
		}
		else if (strcmp(argv[0], "-t") == 0) {
			if (argc < 2)
				usage(name, 1);
			arg_thrd = atoi(argv[1]);
			argv++; argc--;
		}
		else if (strcmp(argv[0], "-w") == 0) {
			if (argc < 2)
				usage(name, 1);
			arg_wait = atoi(argv[1]);
			argv++; argc--;
		}
		else if (strcmp(argv[0], "-O") == 0) {
			char *slash;
			if (argc < 2)
				usage(name, 1);
			slash = strchr(argv[1], '/');
			if (!slash)
				usage(name, 1);
			*(slash++) = 0;
			arg_ovre = atoi(argv[1]);
			arg_ovrp = atoi(slash);
			argv++; argc--;
		}
		else if (strcmp(argv[0], "-T") == 0) {
			if (argc < 2)
				usage(name, 1);
			arg_thnk = atoi(argv[1]);
			argv++; argc--;
		}
		else if (strcmp(argv[0], "-d") == 0) {
			if (argc < 2)
				usage(name, 1);
			arg_dura = atoi(argv[1]);
			argv++; argc--;
		}
		else if (strcmp(argv[0], "-e") == 0)
			arg_serr = 1;
		else if (strcmp(argv[0], "-F") == 0)
			arg_fast = 1;
		else if (strcmp(argv[0], "-I") == 0)
			arg_head = 1;
		else if (strcmp(argv[0], "-v") == 0)
			arg_verb++;
		else if (strcmp(argv[0], "-h") == 0)
			usage(name, 0);
		else
			usage(name, 1);

		argv++; argc--;
	}

	if (arg_thrd > arg_conn)
	    die(1, "Thread count must not exceed connection count\n");

	if (!argc)
		usage(name, 1);

	if (strncmp(*argv, "http://", 7) == 0)
		*argv += 7;

	arg_url = strchr(*argv, '/');
	c = 0;
	if (arg_url) {
		c = *arg_url;
		*arg_url = 0;
	}

	host = strdup(*argv);

	if (arg_url)
		*arg_url = c;
	else
		arg_url = "/";

	/* prepare the request that will be duplicated */
	req_len = 0;
	req_len += snprintf(buf + req_len, sizeof(buf) - req_len,
	                    "%s %s HTTP/1.1\r\n",
	                    arg_head ? "HEAD" : "GET", arg_url);

	start_line = strdup(buf);
	req_len = 0;

	if (!arg_host)
		req_len += snprintf(buf + req_len, sizeof(buf) - req_len,
		                    "Host: %s\r\n", host);
	if (arg_hdr)
		req_len += snprintf(buf + req_len, sizeof(buf) - req_len,
		                    "%s", arg_hdr);

	hdr_block = strdup(buf);

	req_len += snprintf(buf + req_len, sizeof(buf) - req_len, "\r\n");

	if (addr_to_ss(host, &ss, &err) < 0)
		die(1, err.msg);

	setlinebuf(stdout);

	__sync_fetch_and_or(&running, THR_SYNSTART);
	for (th = 0; th < arg_thrd; th++) {
		if (create_thread(th, &err, &ss) < 0) {
			__sync_fetch_and_or(&running, THR_STOP_ALL);
			die(1, err.msg);
		}
	}

	/* all running now */

	gettimeofday(&start_date, NULL);
	if (arg_dura)
		stop_date = tv_ms_add(start_date, arg_dura * 1000);
	else
		stop_date = tv_unset();

	/* wait for all threads to start (or abort) */
	while ((running & THR_COUNT) < arg_thrd)
		usleep(10000);

	/* OK, all threads are ready now */
	__sync_fetch_and_and(&running, ~THR_SYNSTART);

	printf("#     time conns tot_conn  tot_req      tot_bytes    err  cps  rps  Bps  bps\n");

	while (running & THR_COUNT) {
		sleep(1);
		gettimeofday(&now, NULL);

		if ((arg_reqs > 0 && global_req >= arg_reqs) || !tv_isbefore(now, stop_date))
			__sync_fetch_and_or(&running, THR_ENDING);

		summary();
	}

	/* signal all threads that they must stop */
	__sync_fetch_and_or(&running, THR_ENDING);

	for (th = 0; th < arg_thrd; th++)
		pthread_join(threads[th].pth, NULL);

	gettimeofday(&now, NULL);
	summary();
	return 0;
}
