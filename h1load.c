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

/* connection states */
enum cstate {
	CS_NEW = 0,   // just allocated
	CS_CON,       // pending connection attempt
	CS_SND,       // send attempt (headers or body)
	CS_RCV,       // recv attempt (headers or body)
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
	uint64_t tot_req;            // total requests on this connection
	uint64_t tot_sent;           // total bytes sent on this connection
	uint64_t tot_rcvd;           // total bytes received on this connection
	struct timeval req_date;     // moment the request was sent
};

/* one thread */
struct thread {
	struct list wq;              // wait queue: I/O
	struct list sq;              // sleep queue: sleep
	struct list rq;              // run queue: tasks to call
	struct list iq;              // idle queue: when not anywhere else
	struct timeval now;          // current time
	uint32_t curconn;            // number of active connections
	uint32_t maxconn;            // max number of active connections
	uint64_t tot_conn;           // total conns attempted on this thread
	uint64_t tot_req;            // total requests on this thread
	uint64_t tot_sent;           // total bytes sent on this thread
	uint64_t tot_rcvd;           // total bytes received on this thread
	uint64_t tot_serr;           // total socket errors on this thread
	uint64_t tot_cerr;           // total connection errors on this thread
	uint64_t tot_xerr;           // total xfer errors on this thread
	uint64_t tot_cto;            // total connection timeouts on this thread
	uint64_t tot_xto;            // total xfer timeouts on this thread
	int epollfd;                 // poller's FD
	struct timeval start_date;   // thread's start date
	int tid;                     // thread number
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
int arg_reqs = -1;    // max total requests
int arg_thnk = 0;     // think time (ms)
int arg_thrd = 1;     // number of threads
int arg_wait = 10000; // I/O time out (ms)
int arg_verb = 0;     // verbosity
int arg_fast = 0;     // merge send with connect's ACK
char *arg_url;

/* global state */
#define THR_STOP_ALL 0x80000000
volatile uint32_t running = 0; // # = running threads, b31 set = must stop now!
struct thread threads[MAXTHREADS];

/* current thread */
__thread struct thread *thr;
__thread char buf[65536];


/************ time manipulation functions ***************/

/* returns non-zero if <tv> not set (at least one fields not 0) */
static inline int tv_isset(const struct timeval tv)
{
	return !!(tv.tv_sec | tv.tv_usec);
}

/* Returns <0 if tv1<tv2, 0 if tv1==tv2, >0 if tv1>tv2 */
static inline int tv_cmp(const struct timeval tv1, const struct timeval tv2)
{
	if ((unsigned)tv1.tv_sec < (unsigned)tv2.tv_sec)
		return -1;
	else if ((unsigned)tv1.tv_sec > (unsigned)tv2.tv_sec)
		return 1;
	else if ((unsigned)tv1.tv_usec < (unsigned)tv2.tv_usec)
		return -1;
	else if ((unsigned)tv1.tv_usec > (unsigned)tv2.tv_usec)
		return 1;
	else
		return 0;
}

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

static inline unsigned long tv_ms_elapsed(const struct timeval tv1, const struct timeval tv2)
{
	unsigned long ret;

	ret  = ((signed long)(tv2.tv_sec  - tv1.tv_sec))  * 1000;
	ret += (((signed long)(tv2.tv_usec - tv1.tv_usec)) + 999) / 1000;
	return ret;
}

static inline unsigned long tv_ms_remain(const struct timeval tv1, const struct timeval tv2)
{
	if (tv_cmp(tv1, tv2) >= 0)
		return 0; /* event elapsed */

	return tv_ms_elapsed(tv1, tv2);
}

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


/************ connection management **************/

/* updates polling on epoll FD <ep> for fd <fd> supposed to match connection
 * flags <old> with new flags <new>.
 */
void update_poll(int ep, int fd, uint32_t old, uint32_t new, void *ptr)
{
	struct epoll_event ev;
	int op;

	ev.data.ptr = ptr;
	ev.events = ((new & CF_POLW) ? EPOLLOUT : 0) | ((new & CF_POLR) ? EPOLLIN : 0);
	if (!(old & (CF_POLR | CF_POLW)))
		op = EPOLL_CTL_ADD;
	else if (!(new & (CF_POLR | CF_POLW)))
		op = EPOLL_CTL_DEL;
	else
		op = EPOLL_CTL_MOD;

	epoll_ctl(ep, op, fd, &ev);
}

/* update epoll_fd <ep> for conn <conn>, adding flag <add> and removing <del> */
static inline void update_conn(int ep, struct conn *conn, uint32_t add, uint32_t del)
{
	uint32_t flags = (conn->flags | add) & ~del;

	if ((flags ^ conn->flags) & (CF_POLR | CF_POLW))
		update_poll(ep, conn->fd, conn->flags, flags, conn);
	conn->flags = flags;
}

static inline void cant_send(struct conn *conn)
{
	conn->flags |= CF_BLKW;
	if (conn->flags & CF_POLW)
		return;
	update_conn(thr->epollfd, conn, CF_POLW, 0);
}

static inline void cant_recv(struct conn *conn)
{
	conn->flags |= CF_BLKR;
	if (conn->flags & CF_POLR)
		return;
	update_conn(thr->epollfd, conn, CF_POLR, 0);
}

static inline void stop_send(struct conn *conn)
{
	conn->flags &= ~CF_BLKW;
	if (!(conn->flags & CF_POLW))
		return;
	update_conn(thr->epollfd, conn, 0, CF_POLW);
}

static inline void stop_recv(struct conn *conn)
{
	conn->flags &= ~CF_BLKR;
	if (!(conn->flags & CF_POLR))
		return;
	update_conn(thr->epollfd, conn, 0, CF_POLR);
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
		conn->expire = (struct timeval){ .tv_sec = 0, .tv_usec = 0 };
		conn->tot_req = conn->tot_sent = conn->tot_rcvd = 0;
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
		conn->state = CS_CON;
		cant_send(conn);
		//conn->expire = tv_ms_add(t->now, arg_wait);
		LIST_APPEND(&t->wq, &conn->link);
	}
	else {
		conn->state = CS_SND;
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
	t->tot_serr++;
	return NULL;
}

/* handles I/O and timeouts for connection <conn> on thread <t> */
void handle_conn(struct thread *t, struct conn *conn)
{
	int expired = tv_isset(conn->expire) && tv_cmp(conn->expire, t->now) <= 0;
	int ret;

	if (conn->state == CS_CON) {
		if (conn->flags & CF_ERR) {
			t->tot_cerr++;
			goto kill_conn;
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
		conn->state = CS_SND;
	}

	if (conn->state == CS_SND) {
		if (conn->flags & CF_ERR) {
			t->tot_xerr++;
			goto kill_conn;
		}

		if (conn->flags & CF_BLKW) {
			if (expired) {
				t->tot_xto++;
				goto kill_conn;
			}
			cant_send(conn);
			goto wait_io;
		}

		/* finally ready, let's try (again?) */
		ret = send(conn->fd, "HEAD / HTTP/1.0\r\n\r\n", 19, MSG_NOSIGNAL | MSG_DONTWAIT);
		if (ret < 0) {
			if (errno == EAGAIN) {
				cant_send(conn);
				goto wait_io;
			}
			t->tot_xerr++;
			goto kill_conn;
		}

		t->tot_sent += ret;

		/* nothing more to send, wait for response */
		stop_send(conn);
		conn->state = CS_RCV;
	}


	if (conn->state == CS_RCV) {
		if (conn->flags & CF_ERR) {
			t->tot_xerr++;
			goto kill_conn;
		}

		if (conn->flags & CF_BLKR) {
			if (expired) {
				t->tot_xto++;
				goto kill_conn;
			}
			cant_recv(conn);
			goto wait_io;
		}

		/* finally ready, let's try (again?) */
		do {
			ret = recv(conn->fd, buf, sizeof(buf), MSG_NOSIGNAL | MSG_DONTWAIT);
			if (ret < 0) {
				if (errno == EAGAIN) {
					cant_recv(conn);
					goto wait_io;
				}
				t->tot_xerr++;
				goto kill_conn;
			}

			t->tot_rcvd += ret;
		} while (ret > 0);

		//if (arg_thnk)
		//	conn->state = CS_THK;
		//else
		conn->state = CS_END;
	}


	if (conn->state == CS_END) {
		/* it was a close */
		goto kill_conn;
	}


 done:
	update_conn(thr->epollfd, conn, 0, 0);
	return;

 wait_io:
	conn->expire = tv_ms_add(t->now, arg_wait);
	LIST_DELETE(&conn->link);
	LIST_APPEND(&t->wq, &conn->link);
	return;

 kill_conn:
	close(conn->fd);
	t->curconn--;
	LIST_DELETE(&conn->link);
	free(conn);
}

void work(void *arg)
{
	struct thread *thread = (struct thread *)arg;
	struct conn *conn;
	int nbev, i;

	thr = thread;

	__sync_fetch_and_add(&running, 1);
	while (running < arg_thrd)
		usleep(10000);

	while (!(running & THR_STOP_ALL)) {
		//printf("thr=%d curconn=%d maxconn=%d\n", thr->tid, thr->curconn, thr->maxconn);
		for (i = 0; thr->curconn < thr->maxconn && i < pollevents; i++) {
			conn = add_connection(thread);
			if (!conn)
				break;
			/* send request or subscribe */
			handle_conn(thr, conn);
		}

		nbev = epoll_wait(thr->epollfd, thr->events, pollevents, 100);
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
	    "  -c <conn>     concurrent connections (1)\n"
	    "  -n <reqs>     maximum total requests (-1)\n"
	    "  -r <reqs>     number of requests per connection (-1)\n"
	    "  -t <threads>  number of threads to create (1)\n"
	    "  -w <time>     I/O timeout in milliseconds (-1)\n"
	    "  -T <time>     think time after a response (0)\n"
	    "  -F	     merge send() with connect's ACK\n"
	    "  -h	     display this help\n"
	    "  -v	     increase verbosity\n"
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

/* creates and initializes thread <th>, returns <0 on failure */
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
		err->len = snprintf(err->msg, err->size, "Failed to allocate %d poll_events ofr thread %d\n", pollevents, th);
		return -1;
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

int main(int argc, char **argv)
{
	const char *name = argv[0];
	struct sockaddr_storage ss;
	struct errmsg err = { .len = 0, .size = 100, .msg = alloca(100) };
	char c;
	int th;

	argv++;
	argc--;

	while (argc > 0) {
		if (**argv != '-')
			break;

		if (strcmp(argv[0], "-c") == 0) {
			if (argc < 2)
				usage(name, 1);
			arg_conn = atoi(argv[1]);
			argv++; argc--;
		}
		else if (strcmp(argv[0], "-n") == 0) {
			if (argc < 2)
				usage(name, 1);
			arg_reqs = atoi(argv[1]);
			argv++; argc--;
		}
		else if (strcmp(argv[0], "-r") == 0) {
			if (argc < 2)
				usage(name, 1);
			arg_rcon = atoi(argv[1]);
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
		else if (strcmp(argv[0], "-T") == 0) {
			if (argc < 2)
				usage(name, 1);
			arg_thnk = atoi(argv[1]);
			argv++; argc--;
		}
		else if (strcmp(argv[0], "-F") == 0)
			arg_fast = 1;
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

	if (addr_to_ss(*argv, &ss, &err) < 0)
		die(1, err.msg);

	if (arg_url)
		*arg_url = c;
	else
		arg_url = "/";

	for (th = 0; th < arg_thrd; th++) {
		if (create_thread(th, &err, &ss) < 0) {
			__sync_fetch_and_or(&running, THR_STOP_ALL);
			die(1, err.msg);
		}
	}

	/* all running now */
	printf("all started\n");
	usleep(5000000);
	__sync_fetch_and_or(&running, THR_STOP_ALL);
	printf("signaled stopping\n");
	for (th = 0; th < arg_thrd; th++)
		pthread_join(threads[th].pth, NULL);

	for (th = 0; th < arg_thrd; th++) {
		printf("thr %d: %lu conn %lu req\n", th, threads[th].tot_conn, threads[th].tot_req);
	}

	printf("all stopped\n");
	return 0;
}
