#include "core.h"

#include <linux/input-event-codes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "keymap.h"

void core_reconfigure(core_t *c)
{
	const config_t *cfg = c->cfg;
	c->n_trigger_keys = 0;
	if (cfg->trigger_key > 0) {
		c->trigger_keys[0] = cfg->trigger_key;
		c->n_trigger_keys = 1;
	} else if (cfg->trigger_vk_code != 0) {
		c->n_trigger_keys = keymap_vk_to_keys(cfg->trigger_vk_code, c->trigger_keys);
	}
}

void core_init(core_t *c, const config_t *cfg, const core_ops_t *ops, void *ud)
{
	memset(c, 0, sizeof *c);
	c->cfg = cfg;
	c->ops = *ops;
	c->ud = ud;
	core_reconfigure(c);
}

bool core_wants_keyboard_grab(const core_t *c)
{
	return c->n_trigger_keys > 0 && !c->cfg->keyboard_passthrough;
}

int core_scroll_amount(const config_t *cfg, int delta)
{
	if (delta == 0)
		return 0;
	/* int * float is evaluated in float precision, exactly as in the Windows
	 * build; doing this in double would turn 300 * 0.01f into 2.9999999 and
	 * 81 into 80. */
	float scaled = abs(delta) * cfg->sensitivity;
	double val = pow((double)scaled, (double)cfg->ramp_exponent);
	if (val > cfg->max_scroll)
		val = (double)cfg->max_scroll;
	int res = (int)val;
	if (res < cfg->min_scroll)
		res = cfg->min_scroll;
	return delta < 0 ? -res : res;
}

/* StartScrolling(): the anchor is the prime position if we were primed (the
 * motion accumulated so far is kept), otherwise "now" (accumulator reset). */
static void start_scrolling(core_t *c)
{
	if (c->state == ST_SCROLLING)
		return;
	if (c->state == ST_IDLE)
		c->dx = c->dy = 0;
	c->state = ST_SCROLLING;
	if (c->ops.scroll_begin)
		c->ops.scroll_begin(c->ud);
}

void core_stop(core_t *c)
{
	if (c->state != ST_SCROLLING)
		return;
	c->state = ST_IDLE;
	if (c->ops.scroll_end)
		c->ops.scroll_end(c->ud);
}

void core_set_paused(core_t *c, bool paused)
{
	c->paused = paused;
	if (paused) {
		core_stop(c);
		if (c->state == ST_PRIMED)
			c->state = ST_IDLE;
	}
}

static void cancel(core_t *c)
{
	if (c->state == ST_PRIMED)
		c->state = ST_IDLE;
	else
		core_stop(c);
}

bool core_button(core_t *c, int code, int value)
{
	if (code != BTN_MIDDLE || !c->cfg->trigger_middle_mouse || c->paused)
		return false;

	if (value == 1) {
		c->mid_swallowed = true;
		if (c->state == ST_IDLE) {
			c->state = ST_PRIMED;
			c->dx = c->dy = 0;
		} else if (c->state == ST_SCROLLING && c->cfg->trigger_mode == MODE_TOGGLE) {
			core_stop(c);
		}
		return true;
	}

	if (value == 0 && c->mid_swallowed) {
		c->mid_swallowed = false;
		if (c->state == ST_PRIMED) {
			/* Released without moving past drag_threshold: a plain click. */
			c->state = ST_IDLE;
			if (c->cfg->middle_mouse_passthrough && c->ops.emit_middle) {
				c->ops.emit_middle(c->ud, 1);
				c->ops.emit_middle(c->ud, 0);
			}
		} else if (c->state == ST_SCROLLING && c->cfg->trigger_mode == MODE_HOLD) {
			core_stop(c);
		}
		return true;
	}
	return false;
}

bool core_motion(core_t *c, int dx, int dy)
{
	if (c->paused || c->state == ST_IDLE)
		return false;

	c->dx += dx * (double)c->cfg->raw_scale;
	c->dy += dy * (double)c->cfg->raw_scale;

	if (c->state == ST_PRIMED) {
		int thr = c->cfg->drag_threshold;
		if (abs((int)c->dx) > thr || abs((int)c->dy) > thr)
			start_scrolling(c);
	}
	return config_freeze_pointer(c->cfg);
}

bool core_key(core_t *c, int code, int value)
{
	if (c->paused)
		return false;

	if (code == KEY_ESC && value == 1 &&
	    (c->state == ST_SCROLLING || c->state == ST_PRIMED)) {
		cancel(c);
		return true;
	}

	for (int i = 0; i < c->n_trigger_keys; i++) {
		if (c->trigger_keys[i] != code)
			continue;
		/* Windows hooks see auto-repeat as extra WM_KEYDOWNs, which would
		 * flip-flop toggle mode. Ignore repeats here. */
		if (value == 1) {
			if (c->cfg->trigger_mode == MODE_HOLD) {
				start_scrolling(c);
			} else if (c->state == ST_SCROLLING) {
				core_stop(c);
			} else {
				start_scrolling(c);
			}
		} else if (value == 0 && c->cfg->trigger_mode == MODE_HOLD) {
			core_stop(c);
		}
		return !c->cfg->keyboard_passthrough;
	}
	return false;
}

void core_tick(core_t *c)
{
	if (c->state != ST_SCROLLING)
		return;
	const config_t *cfg = c->cfg;

	int dx = (int)c->dx, dy = (int)c->dy;
	bool act;
	if (cfg->dead_zone_shape == SHAPE_SQUARE)
		act = abs(dx) > cfg->dead_zone || abs(dy) > cfg->dead_zone;
	else
		act = sqrt((double)dx * dx + (double)dy * dy) > cfg->dead_zone;
	if (!act)
		return;

	/* 2. raw magnitude */
	int vS = core_scroll_amount(cfg, dy);
	int hS = core_scroll_amount(cfg, dx);

	/* 3. axis locking */
	if (cfg->axis_lock_threshold > 0) {
		int adx = abs(dx), ady = abs(dy);
		if (ady >= adx) {
			if (adx <= cfg->axis_lock_threshold)
				hS = 0;
		} else if (ady <= cfg->axis_lock_threshold) {
			vS = 0;
		}
	}

	/* 4. natural scrolling */
	if (cfg->natural_scrolling) {
		vS = -vS;
		hS = -hS;
	}

	/* 5. wheel inversion: line scrolling needs it, "touchpad" mode doesn't */
	if (!cfg->emulate_touchpad_scrolling)
		vS = -vS;

	/* 6. output */
	if ((vS || hS) && c->ops.scroll)
		c->ops.scroll(c->ud, vS, hS);

	/* 7. stats (same bookkeeping as the Windows build) */
	if (cfg->fun_stats) {
		unsigned long long moved = (unsigned long long)(abs(vS) + abs(hS));
		c->stats.total += moved;
		c->stats.session += moved;
		int lv = cfg->emulate_touchpad_scrolling ? -vS : vS;
		if (cfg->natural_scrolling)
			lv = -lv;
		int lh = cfg->natural_scrolling ? -hS : hS;
		if (lv > 0) c->stats.up += lv;
		if (lv < 0) c->stats.down += -lv;
		if (lh > 0) c->stats.right += lh;
		if (lh < 0) c->stats.left += -lh;
		c->stats_dirty = true;
	}
}
