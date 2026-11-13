/* Unit tests: config parser, scroll math, state machine, keymap, drawing. */
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/config.h"
#include "../src/core.h"
#include "../src/keymap.h"
#include "../src/input.h"

#ifndef TOP_CONFIG
#define TOP_CONFIG "../config.ini"
#endif

static int failures, checks;
#define CHECK(cond)                                                              \
	do {                                                                     \
		checks++;                                                        \
		if (!(cond)) {                                                   \
			failures++;                                              \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		}                                                                \
	} while (0)

/* ---- recording fake backend ---- */
typedef struct {
	int begins, ends, clicks_down, clicks_up, scrolls;
	int last_v, last_h;
} rec_t;

static void r_middle(void *ud, int v) { rec_t *r = ud; if (v) r->clicks_down++; else r->clicks_up++; }
static void r_scroll(void *ud, int v, int h) { rec_t *r = ud; r->scrolls++; r->last_v = v; r->last_h = h; }
static void r_begin(void *ud) { ((rec_t *)ud)->begins++; }
static void r_end(void *ud) { ((rec_t *)ud)->ends++; }
static const core_ops_t OPS = {r_middle, r_scroll, r_begin, r_end};

static void setup(core_t *c, config_t *cfg, rec_t *r, const char *ini)
{
	memset(r, 0, sizeof *r);
	config_load_string(cfg, ini);
	core_init(c, cfg, &OPS, r);
}

static void test_config(void)
{
	config_t d, f;
	config_defaults(&d);
	/* The shipped config.ini documents the defaults: parsing it must
	 * reproduce the compiled-in defaults bit for bit. */
	CHECK(config_load(&f, TOP_CONFIG) == 0);
	CHECK(memcmp(&d, &f, sizeof d) == 0);

	config_t c;
	config_load_string(&c, "trigger_mode = toggle\nsensitivity=0.5\n# sensitivity = 9\n"
			       "  dead_zone  =  7  \nindicator_color = #11223344\n"
			       "outline_color = 0A0B0C\ntrigger_vk_code = 0x70\nbogus = 1\n"
			       "indicator_shape = CROSS\ndead_zone_shape = Square\n");
	CHECK(c.trigger_mode == MODE_TOGGLE);
	CHECK(fabsf(c.sensitivity - 0.5f) < 1e-6f);
	CHECK(c.dead_zone == 7);
	CHECK(c.indicator_color_r == 0x11 && c.indicator_color_g == 0x22 &&
	      c.indicator_color_b == 0x33 && c.indicator_color_a == 0x44);
	CHECK(c.outline_color_r == 0x0A && c.outline_color_g == 0x0B &&
	      c.outline_color_b == 0x0C && c.outline_color_a == 255);
	CHECK(c.trigger_vk_code == 0x70);
	CHECK(c.indicator_shape == SHAPE_CROSS);
	CHECK(c.dead_zone_shape == SHAPE_SQUARE);

	/* Quirks inherited from the Windows parser. */
	config_load_string(&c, "trigger_mode = whatever\n");
	CHECK(c.trigger_mode == MODE_TOGGLE);
	config_load_string(&c, "trigger_mode = HOLD\r\n");
	CHECK(c.trigger_mode == MODE_HOLD);

	/* Linux extras and the freeze_pointer derivation. */
	config_load_string(&c, "");
	CHECK(config_freeze_pointer(&c) == 1);
	config_load_string(&c, "use_send_input_api = 1\n");
	CHECK(config_freeze_pointer(&c) == 0);
	config_load_string(&c, "use_send_input_api = 1\nfreeze_pointer = 1\n");
	CHECK(config_freeze_pointer(&c) == 1);
	config_load_string(&c, "raw_scale = 2.5\ntrigger_key = 70\n");
	CHECK(fabsf(c.raw_scale - 2.5f) < 1e-6f && c.trigger_key == 70);
}

static void test_amount(void)
{
	config_t c;
	config_defaults(&c);
	CHECK(core_scroll_amount(&c, 0) == 0);
	CHECK(core_scroll_amount(&c, 100) == 1);   /* (1.0)^4 */
	CHECK(core_scroll_amount(&c, 300) == 81);  /* (3.0)^4 */
	CHECK(core_scroll_amount(&c, -300) == -81);
	CHECK(core_scroll_amount(&c, 10) == 1);    /* floored to min_scroll */
	CHECK(core_scroll_amount(&c, 5000) == 1000); /* clamped to max_scroll */
}

static void test_click_and_hold(void)
{
	core_t c; config_t cfg; rec_t r;
	setup(&c, &cfg, &r, "");

	/* Plain click: swallowed, then re-emitted as a click on release. */
	CHECK(core_button(&c, BTN_MIDDLE, 1));
	CHECK(c.state == ST_PRIMED);
	CHECK(core_button(&c, BTN_MIDDLE, 0));
	CHECK(r.clicks_down == 1 && r.clicks_up == 1 && r.begins == 0);
	CHECK(c.state == ST_IDLE);

	/* Other buttons are none of our business. */
	CHECK(!core_button(&c, BTN_LEFT, 1));

	/* Hold: press, drag down 300, tick, release. */
	CHECK(core_button(&c, BTN_MIDDLE, 1));
	CHECK(core_motion(&c, 0, 300)); /* frozen by default (use_send_input_api=0) */
	CHECK(c.state == ST_SCROLLING && r.begins == 1);
	core_tick(&c);
	CHECK(r.scrolls == 1 && r.last_v == -81 && r.last_h == 0); /* down = wheel negative */
	CHECK(core_button(&c, BTN_MIDDLE, 0));
	CHECK(r.ends == 1 && c.state == ST_IDLE && r.clicks_down == 1);
	core_tick(&c);
	CHECK(r.scrolls == 1); /* idle ticks do nothing */

	/* Moving up / right gives the opposite signs. */
	core_button(&c, BTN_MIDDLE, 1);
	core_motion(&c, 300, -300);
	core_tick(&c);
	CHECK(r.last_v == 81 && r.last_h == 81);
	core_button(&c, BTN_MIDDLE, 0);
}

static void test_modes(void)
{
	core_t c; config_t cfg; rec_t r;

	/* emulate_touchpad_scrolling flips the vertical sign only. */
	setup(&c, &cfg, &r, "emulate_touchpad_scrolling = 1\n");
	core_button(&c, BTN_MIDDLE, 1);
	core_motion(&c, 0, 300);
	core_tick(&c);
	CHECK(r.last_v == 81);
	core_button(&c, BTN_MIDDLE, 0);

	/* natural_scrolling inverts both axes. */
	setup(&c, &cfg, &r, "natural_scrolling = 1\n");
	core_button(&c, BTN_MIDDLE, 1);
	core_motion(&c, 0, 300);
	core_tick(&c);
	CHECK(r.last_v == 81);
	core_button(&c, BTN_MIDDLE, 0);

	/* Axis lock: mostly-vertical drag drops the horizontal component. */
	setup(&c, &cfg, &r, "");
	core_button(&c, BTN_MIDDLE, 1);
	core_motion(&c, 4, 300);
	core_tick(&c);
	CHECK(r.last_h == 0 && r.last_v != 0);
	core_motion(&c, 296, 0); /* now a true diagonal */
	core_tick(&c);
	CHECK(r.last_h != 0 && r.last_v != 0);
	core_button(&c, BTN_MIDDLE, 0);

	/* Dead zone (circle, radius 1): a 1-count nudge does nothing. */
	setup(&c, &cfg, &r, "");
	core_button(&c, BTN_MIDDLE, 1);
	core_motion(&c, 1, 0);
	core_tick(&c);
	CHECK(r.scrolls == 0);
	core_button(&c, BTN_MIDDLE, 0);

	/* drag_threshold: small jitter is still a click. */
	setup(&c, &cfg, &r, "drag_threshold = 10\n");
	core_button(&c, BTN_MIDDLE, 1);
	core_motion(&c, 5, 5);
	CHECK(c.state == ST_PRIMED);
	core_button(&c, BTN_MIDDLE, 0);
	CHECK(r.begins == 0 && r.clicks_down == 1);

	/* Pointer freeze follows use_send_input_api. */
	setup(&c, &cfg, &r, "use_send_input_api = 1\n");
	core_button(&c, BTN_MIDDLE, 1);
	CHECK(!core_motion(&c, 0, 300));
	core_button(&c, BTN_MIDDLE, 0);

	/* middle_mouse_passthrough = 0 swallows the click entirely. */
	setup(&c, &cfg, &r, "middle_mouse_passthrough = 0\n");
	core_button(&c, BTN_MIDDLE, 1);
	core_button(&c, BTN_MIDDLE, 0);
	CHECK(r.clicks_down == 0);

	/* raw_scale multiplies raw counts. */
	setup(&c, &cfg, &r, "raw_scale = 3\n");
	core_button(&c, BTN_MIDDLE, 1);
	core_motion(&c, 0, 100); /* behaves like 300 */
	core_tick(&c);
	CHECK(r.last_v == -81);
	core_button(&c, BTN_MIDDLE, 0);
}

static void test_toggle_and_cancel(void)
{
	core_t c; config_t cfg; rec_t r;
	setup(&c, &cfg, &r, "trigger_mode = toggle\n");

	core_button(&c, BTN_MIDDLE, 1);
	core_motion(&c, 0, 300);
	CHECK(c.state == ST_SCROLLING);
	CHECK(core_button(&c, BTN_MIDDLE, 0)); /* release keeps scrolling */
	CHECK(c.state == ST_SCROLLING && r.ends == 0);
	CHECK(core_button(&c, BTN_MIDDLE, 1)); /* second press stops */
	CHECK(r.ends == 1 && c.state == ST_IDLE);
	CHECK(core_button(&c, BTN_MIDDLE, 0)); /* ...and its release is swallowed */
	CHECK(r.clicks_down == 0);

	/* A release we never saw the press for must pass through. */
	CHECK(!core_button(&c, BTN_MIDDLE, 0));

	/* Esc cancels a primed gesture without clicking, and the release after
	 * it is still swallowed. */
	setup(&c, &cfg, &r, "");
	core_button(&c, BTN_MIDDLE, 1);
	CHECK(core_key(&c, KEY_ESC, 1));
	CHECK(c.state == ST_IDLE);
	CHECK(core_button(&c, BTN_MIDDLE, 0));
	CHECK(r.clicks_down == 0);

	/* Esc while scrolling stops it. */
	core_button(&c, BTN_MIDDLE, 1);
	core_motion(&c, 0, 300);
	CHECK(core_key(&c, KEY_ESC, 1));
	CHECK(r.ends == 1);
	core_button(&c, BTN_MIDDLE, 0);

	/* Esc when idle is not ours. */
	CHECK(!core_key(&c, KEY_ESC, 1));
}

static void test_keyboard(void)
{
	core_t c; config_t cfg; rec_t r;

	setup(&c, &cfg, &r, "trigger_vk_code = 0x91\n"); /* VK_SCROLL */
	CHECK(c.n_trigger_keys == 1 && c.trigger_keys[0] == KEY_SCROLLLOCK);
	CHECK(!core_key(&c, KEY_SCROLLLOCK, 1)); /* passthrough: not swallowed */
	CHECK(r.begins == 1 && c.state == ST_SCROLLING);
	core_key(&c, KEY_SCROLLLOCK, 2);         /* auto-repeat ignored */
	CHECK(r.begins == 1);
	core_key(&c, KEY_SCROLLLOCK, 0);
	CHECK(r.ends == 1);
	CHECK(!core_wants_keyboard_grab(&c));

	setup(&c, &cfg, &r, "trigger_vk_code = 0x91\nkeyboard_passthrough = 0\n");
	CHECK(core_wants_keyboard_grab(&c));
	CHECK(core_key(&c, KEY_SCROLLLOCK, 1)); /* swallowed */
	core_key(&c, KEY_SCROLLLOCK, 0);

	/* toggle mode + repeats must not flip-flop. */
	setup(&c, &cfg, &r, "trigger_vk_code = 0x91\ntrigger_mode = toggle\n");
	core_key(&c, KEY_SCROLLLOCK, 1);
	core_key(&c, KEY_SCROLLLOCK, 2);
	core_key(&c, KEY_SCROLLLOCK, 0);
	CHECK(c.state == ST_SCROLLING);
	core_key(&c, KEY_SCROLLLOCK, 1);
	CHECK(c.state == ST_IDLE);

	/* Linux-native override wins over the VK code. */
	setup(&c, &cfg, &r, "trigger_vk_code = 0x91\ntrigger_key = 70\n");
	CHECK(c.n_trigger_keys == 1 && c.trigger_keys[0] == 70);

	/* Pause makes everything pass through. */
	setup(&c, &cfg, &r, "");
	core_set_paused(&c, true);
	CHECK(!core_button(&c, BTN_MIDDLE, 1));
	core_set_paused(&c, false);
	CHECK(core_button(&c, BTN_MIDDLE, 1));
	core_set_paused(&c, true); /* pausing mid-gesture resets state */
	CHECK(c.state == ST_IDLE);
}

static void test_keymap(void)
{
	int k[2];
	CHECK(keymap_vk_to_keys(0x70, k) == 1 && k[0] == KEY_F1);
	CHECK(keymap_vk_to_keys(0x7B, k) == 1 && k[0] == KEY_F12);
	CHECK(keymap_vk_to_keys(0x7C, k) == 1 && k[0] == KEY_F13);
	CHECK(keymap_vk_to_keys(0x87, k) == 1 && k[0] == KEY_F24);
	CHECK(keymap_vk_to_keys('A', k) == 1 && k[0] == KEY_A);
	CHECK(keymap_vk_to_keys('Q', k) == 1 && k[0] == KEY_Q);
	CHECK(keymap_vk_to_keys('M', k) == 1 && k[0] == KEY_M);
	CHECK(keymap_vk_to_keys('0', k) == 1 && k[0] == KEY_0);
	CHECK(keymap_vk_to_keys('1', k) == 1 && k[0] == KEY_1);
	CHECK(keymap_vk_to_keys(0x60, k) == 1 && k[0] == KEY_KP0);
	CHECK(keymap_vk_to_keys(0x69, k) == 1 && k[0] == KEY_KP9);
	CHECK(keymap_vk_to_keys(0x91, k) == 1 && k[0] == KEY_SCROLLLOCK);
	CHECK(keymap_vk_to_keys(0x13, k) == 1 && k[0] == KEY_PAUSE);
	CHECK(keymap_vk_to_keys(0xA2, k) == 1 && k[0] == KEY_LEFTCTRL);
	CHECK(keymap_vk_to_keys(0x10, k) == 2 && k[0] == KEY_LEFTSHIFT && k[1] == KEY_RIGHTSHIFT);
	CHECK(keymap_vk_to_keys(0x07, k) == 0); /* undefined VK */
}

static struct input_event E(int type, int code, int value)
{
	return (struct input_event){.type = type, .code = code, .value = value};
}
#define SYN E(EV_SYN, SYN_REPORT, 0)

/* Like OPS, but the passthrough click really goes through the input layer. */
static void i_middle(void *ud, int v) { r_middle(ud, v); input_emit_middle(v); }
static const core_ops_t IOPS = {i_middle, r_scroll, r_begin, r_end};
static void isetup(core_t *c, config_t *cfg, rec_t *r, const char *ini)
{
	memset(r, 0, sizeof *r);
	config_load_string(cfg, ini);
	core_init(c, cfg, &IOPS, r);
}

static void test_input(void)
{
	core_t c; config_t cfg; rec_t r;
	isetup(&c, &cfg, &r, "");
	input_bind(&c, &cfg);

	input_dev_t d;
	memset(&d, 0, sizeof d);
	d.fd = d.clone_fd = -1;
	d.pointer = d.grab = true;
	d.state = DEV_ACTIVE;
	struct input_event out[32];
	int n;

	/* Idle: everything is forwarded untouched, SYN included. */
	struct input_event a[] = {E(EV_REL, REL_X, 5), E(EV_REL, REL_Y, 3), SYN};
	n = input_process(&d, a, 3, out);
	CHECK(n == 3 && out[0].code == REL_X && out[2].type == EV_SYN);

	/* Middle press is swallowed, and so is its (now empty) frame. */
	struct input_event b[] = {E(EV_KEY, BTN_MIDDLE, 1), SYN};
	n = input_process(&d, b, 2, out);
	CHECK(n == 0 && c.state == ST_PRIMED);

	/* Left button still passes while primed. */
	struct input_event l[] = {E(EV_KEY, BTN_LEFT, 1), SYN};
	n = input_process(&d, l, 2, out);
	CHECK(n == 2);

	/* Motion starts the scroll and (frozen pointer) is swallowed. */
	struct input_event m[] = {E(EV_REL, REL_X, 4), E(EV_REL, REL_Y, 9), SYN};
	n = input_process(&d, m, 3, out);
	CHECK(n == 0 && c.state == ST_SCROLLING && r.begins == 1);

	/* Real wheel events pass through while scrolling. */
	struct input_event w[] = {E(EV_REL, REL_WHEEL, 1), SYN};
	n = input_process(&d, w, 2, out);
	CHECK(n == 2);

	/* A frame mixing swallowed motion and a forwarded event keeps the latter. */
	struct input_event mix[] = {E(EV_REL, REL_X, 1), E(EV_REL, REL_HWHEEL, 1), SYN};
	n = input_process(&d, mix, 3, out);
	CHECK(n == 2 && out[0].code == REL_HWHEEL);

	struct input_event u[] = {E(EV_KEY, BTN_MIDDLE, 0), SYN};
	n = input_process(&d, u, 2, out);
	CHECK(n == 0 && r.ends == 1 && c.state == ST_IDLE);

	/* Click: press+release in one batch re-emerges as press, SYN, release, SYN
	 * and in order with the surrounding motion. */
	isetup(&c, &cfg, &r, "");
	struct input_event click[] = {E(EV_REL, REL_X, 1), SYN, E(EV_KEY, BTN_MIDDLE, 1), SYN,
				      E(EV_KEY, BTN_MIDDLE, 0), SYN, E(EV_REL, REL_Y, 1), SYN};
	n = input_process(&d, click, 8, out);
	CHECK(n == 8);
	CHECK(out[2].code == BTN_MIDDLE && out[2].value == 1);
	CHECK(out[4].code == BTN_MIDDLE && out[4].value == 0);
	CHECK(out[6].code == REL_Y);

	/* SYN_DROPPED: events until the next SYN_REPORT are discarded. */
	isetup(&c, &cfg, &r, "");
	struct input_event dr[] = {E(EV_SYN, SYN_DROPPED, 0), E(EV_REL, REL_X, 7), SYN,
				   E(EV_REL, REL_X, 2), SYN};
	n = input_process(&d, dr, 5, out);
	CHECK(n == 2 && out[0].value == 2);

	/* Observe-only keyboard: Esc cancels, but nothing is forwarded. */
	isetup(&c, &cfg, &r, "");
	input_dev_t k;
	memset(&k, 0, sizeof k);
	k.fd = k.clone_fd = -1;
	k.keyboard = true;
	k.state = DEV_ACTIVE;
	core_button(&c, BTN_MIDDLE, 1);
	struct input_event esc[] = {E(EV_KEY, KEY_ESC, 1), SYN};
	n = input_process(&k, esc, 2, out);
	CHECK(n == 0 && c.state == ST_IDLE);

	/* Grabbed keyboard with a swallowing trigger. */
	isetup(&c, &cfg, &r, "trigger_vk_code = 0x91\nkeyboard_passthrough = 0\n");
	k.grab = true;
	struct input_event sl[] = {E(EV_KEY, KEY_SCROLLLOCK, 1), SYN, E(EV_KEY, KEY_A, 1), SYN};
	n = input_process(&k, sl, 4, out);
	CHECK(n == 2 && out[0].code == KEY_A && r.begins == 1);
}



int main(void)
{
	test_config();
	test_amount();
	test_click_and_hold();
	test_modes();
	test_toggle_and_cancel();
	test_keyboard();
	test_keymap();
	test_input();
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
