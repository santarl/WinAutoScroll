/*
 * wlautoscroll - input-agnostic autoscroll engine.
 *
 * This is the part that is a straight port of WinAutoScroll's hook procedures
 * and ScrollingThread. It knows nothing about evdev, uinput or Wayland: input
 * events go in, and the engine calls back through core_ops_t. That keeps it
 * unit-testable without any hardware.
 */
#ifndef WLAS_CORE_H
#define WLAS_CORE_H

#include <stdbool.h>

#include "config.h"

typedef enum { ST_IDLE, ST_PRIMED, ST_SCROLLING } scroll_state_t;

typedef struct {
	unsigned long long total, up, down, left, right, session;
} stats_t;

typedef struct {
	/* Re-emit a middle-button press (1) / release (0) on the device that
	 * produced the swallowed click (the "passthrough" click). */
	void (*emit_middle)(void *ud, int value);
	/* Wheel output in 1/120-notch units. +vert = up, +horiz = right. */
	void (*scroll)(void *ud, int vert, int horiz);
	/* State became SCROLLING (show indicator, start the tick timer). */
	void (*scroll_begin)(void *ud);
	/* State left SCROLLING (hide indicator, stop timer, flush stats). */
	void (*scroll_end)(void *ud);
} core_ops_t;

typedef struct {
	const config_t *cfg;
	core_ops_t ops;
	void *ud;

	scroll_state_t state;
	bool paused;
	bool mid_swallowed; /* we swallowed the middle press; swallow its release */
	double dx, dy;      /* raw motion accumulated since the anchor */
	int trigger_keys[2];
	int n_trigger_keys;
	stats_t stats;
	bool stats_dirty;
} core_t;

void core_init(core_t *c, const config_t *cfg, const core_ops_t *ops, void *ud);

/* Call after the config struct changed (resolves the keyboard trigger). */
void core_reconfigure(core_t *c);

/* Each of these returns true if the event must NOT reach applications. */
bool core_button(core_t *c, int code, int value); /* EV_KEY on a pointer */
bool core_motion(core_t *c, int dx, int dy);      /* EV_REL REL_X / REL_Y */
bool core_key(core_t *c, int code, int value);    /* EV_KEY on a keyboard */

/* Periodic work while SCROLLING; call at update_frequency Hz. */
void core_tick(core_t *c);

void core_set_paused(core_t *c, bool paused);
void core_stop(core_t *c);

/* Port of CalculateScrollAmount(); exposed for tests. */
int core_scroll_amount(const config_t *cfg, int delta);

/* True if a keyboard trigger is configured and must be swallowed. */
bool core_wants_keyboard_grab(const core_t *c);

#endif
