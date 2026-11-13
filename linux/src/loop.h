/* wlautoscroll - minimal epoll loop. Idle cost is zero wakeups. */
#ifndef WLAS_LOOP_H
#define WLAS_LOOP_H

typedef struct fdh fdh_t;
struct fdh {
	int fd;
	void (*cb)(fdh_t *h, unsigned events);
	void *data;
};

int loop_init(void);
int loop_add(fdh_t *h, unsigned events);
void loop_del(fdh_t *h);
void loop_run(void);
void loop_quit(void);

/* Run fn(arg) once the current batch of events has been dispatched. Used to
 * free handlers that may still be referenced by later events in the batch. */
void loop_defer(void (*fn)(void *), void *arg);

#endif
