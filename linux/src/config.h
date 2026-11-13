/* wlautoscroll - config.ini parsing (same format as WinAutoScroll.exe) */
#ifndef WLAS_CONFIG_H
#define WLAS_CONFIG_H

typedef enum { SHAPE_CIRCLE, SHAPE_SQUARE, SHAPE_CROSS } shape_t;
typedef enum { MODE_TOGGLE, MODE_HOLD } trigger_mode_t;

typedef struct {
	/* --- keys shared with WinAutoScroll.exe (names, units, defaults) --- */
	int min_scroll, max_scroll;
	float sensitivity, ramp_exponent;
	int update_frequency;
	int trigger_vk_code, trigger_middle_mouse, emulate_touchpad_scrolling;
	trigger_mode_t trigger_mode;
	int middle_mouse_passthrough, keyboard_passthrough, drag_threshold;
	shape_t dead_zone_shape;
	int dead_zone;
	int axis_lock_threshold;
	int use_send_input_api;
	int show_indicator;
	shape_t indicator_shape;
	int indicator_size, indicator_cross_thickness;
	int indicator_color_r, indicator_color_g, indicator_color_b, indicator_color_a;
	float indicator_thickness;
	int indicator_filled;
	int fun_stats;
	int natural_scrolling;
	int show_outline;
	float outline_thickness;
	int outline_color_r, outline_color_g, outline_color_b, outline_color_a;
	int disable_cursor_changes; /* accepted, no effect on Wayland */

	/* --- Linux-only extras (unknown keys are ignored by the Windows exe) --- */
	float raw_scale;    /* multiplier for raw mouse counts; default 1.0 */
	int trigger_key;    /* evdev KEY_* code; overrides trigger_vk_code if != 0 */
	int freeze_pointer; /* -1 = auto (from use_send_input_api), 0 / 1 = force */
} config_t;

void config_defaults(config_t *c);

/* Parse one "key = value" line into c. Returns 1 if the key was recognised. */
int config_parse_line(config_t *c, char *line);

/* Reset to defaults, then apply a file. Returns -1 if the file can't be opened. */
int config_load(config_t *c, const char *path);

/* Same as config_load but from memory (used by tests). */
void config_load_string(config_t *c, const char *text);

/* Whether motion is swallowed (cursor frozen) while an autoscroll is active. */
int config_freeze_pointer(const config_t *c);

#endif
