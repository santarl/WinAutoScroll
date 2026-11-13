/* Unit tests: config parser, scroll math, state machine, keymap, drawing. */
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/config.h"
#include "../src/core.h"
#include "../src/keymap.h"

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




int main(void)
{
	test_config();
	test_amount();
	test_click_and_hold();
	test_modes();
	test_toggle_and_cancel();
	test_keyboard();
	test_keymap();
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
