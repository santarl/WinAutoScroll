/*
 * wlautoscroll - evdev capture + uinput output.
 *
 * Wayland has no global input hooks, so we sit below the compositor:
 *
 *   physical mouse --EVIOCGRAB--> us --uinput clone--> compositor
 *
 * Every grabbed device gets a uinput twin with the same name and ids (so
 * per-device compositor settings still match). Events are forwarded
 * untouched, except the ones the core decides to swallow. Wheel output goes
 * through one extra virtual device so that natural_scrolling & co. mean
 * exactly what config.ini says, independent of compositor device settings.
 *
 * If this process dies the kernel drops the grab and the clones vanish, so
 * the real devices come straight back.
 */
#ifndef WLAS_INPUT_H
#define WLAS_INPUT_H

#include <linux/input.h>
#include <stdbool.h>

#include "config.h"
#include "core.h"
#include "loop.h"

typedef struct input_dev input_dev_t;
struct input_dev {
	fdh_t h;          /* the evdev fd, registered with the loop */
	fdh_t clone_h;    /* the uinput fd (only watched for LED feedback) */
	int fd, clone_fd; /* -1 when absent (tests use fd-less devices) */
	char path[320];
	char name[96];
	enum { DEV_ACTIVE, DEV_IGNORED, DEV_RETRY } state;
	bool pointer;  /* grabbed + cloned mouse */
	bool keyboard; /* observed, or grabbed + cloned if a trigger needs it */
	bool grab;     /* events are forwarded to the clone */
	bool leds;
	bool dirty;    /* current SYN frame has forwarded events */
	bool dropped;  /* inside a SYN_DROPPED resync window */
	bool warned;
	input_dev_t *next;
};

/* Bind the core/config without touching any device (used by tests). */
void input_bind(core_t *core, const config_t *cfg);

/* input_bind() + create the virtual wheel device. -1 on failure. */
int input_init(core_t *core, const config_t *cfg);

/* Open every /dev/input/event* we haven't looked at yet. */
void input_scan(void);

/* Config changed: re-evaluate which devices need grabbing. */
void input_reconfigure(void);

/* Release all devices; clones are destroyed. */
void input_shutdown(void);

/* Wheel output, 1/120-notch units. */
void input_emit_scroll(int vert, int horiz);

/* Re-emit a middle press/release on the device being processed. */
void input_emit_middle(int value);

/* A +1/-1 pointer wiggle; makes compositors recompute pointer focus. */
void input_nudge(void);

/*
 * Core of the forwarding logic, free of I/O so it can be tested. Feeds the
 * `n` events in `in` through the engine and writes what should reach the
 * clone to `out`, which must have room for n + 4 events. Returns the count.
 */
int input_process(input_dev_t *d, const struct input_event *in, int n,
		  struct input_event *out);

/* Watch /dev/input for hot-plugged devices. */
int input_hotplug_start(void);

#endif
