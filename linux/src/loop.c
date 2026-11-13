#include "loop.h"

#include <errno.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <unistd.h>

static int epfd = -1;
static int quitting;

struct deferred {
	void (*fn)(void *);
	void *arg;
};
static struct deferred *dq;
static int dq_n, dq_cap;

int loop_init(void)
{
	epfd = epoll_create1(EPOLL_CLOEXEC);
	return epfd < 0 ? -1 : 0;
}

int loop_add(fdh_t *h, unsigned events)
{
	struct epoll_event ev = {.events = events, .data.ptr = h};
	return epoll_ctl(epfd, EPOLL_CTL_ADD, h->fd, &ev);
}

void loop_del(fdh_t *h)
{
	epoll_ctl(epfd, EPOLL_CTL_DEL, h->fd, NULL);
}

void loop_quit(void)
{
	quitting = 1;
}

void loop_defer(void (*fn)(void *), void *arg)
{
	if (dq_n == dq_cap) {
		int ncap = dq_cap ? dq_cap * 2 : 8;
		struct deferred *n = realloc(dq, (size_t)ncap * sizeof *n);
		if (!n) {
			fn(arg); /* better a tiny risk than a leak loop */
			return;
		}
		dq = n;
		dq_cap = ncap;
	}
	dq[dq_n++] = (struct deferred){fn, arg};
}

void loop_run(void)
{
	struct epoll_event evs[32];
	while (!quitting) {
		int n = epoll_wait(epfd, evs, 32, -1);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		for (int i = 0; i < n; i++) {
			fdh_t *h = evs[i].data.ptr;
			if (h->cb)
				h->cb(h, evs[i].events);
		}
		for (int i = 0; i < dq_n; i++)
			dq[i].fn(dq[i].arg);
		dq_n = 0;
	}
}
