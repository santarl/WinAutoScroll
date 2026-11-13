#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

void config_defaults(config_t *c)
{
	/* Mirrors g_config in WinAutoScroll.cpp, field for field. */
	memset(c, 0, sizeof *c);
	c->min_scroll = 1;
	c->max_scroll = 1000;
	c->sensitivity = 0.01f;
	c->ramp_exponent = 4.0f;
	c->update_frequency = 60;
	c->trigger_vk_code = 0;
	c->trigger_middle_mouse = 1;
	c->emulate_touchpad_scrolling = 0;
	c->trigger_mode = MODE_HOLD;
	c->middle_mouse_passthrough = 1;
	c->keyboard_passthrough = 1;
	c->drag_threshold = 0;
	c->dead_zone_shape = SHAPE_CIRCLE;
	c->dead_zone = 1;
	c->axis_lock_threshold = 5;
	c->use_send_input_api = 0;
	c->show_indicator = 1;
	c->indicator_shape = SHAPE_CIRCLE;
	c->indicator_size = 26;
	c->indicator_cross_thickness = 10;
	c->indicator_color_r = 255;
	c->indicator_color_g = 255;
	c->indicator_color_b = 255;
	c->indicator_color_a = 180;
	c->indicator_thickness = 6.0f;
	c->indicator_filled = 0;
	c->fun_stats = 1;
	c->natural_scrolling = 0;
	c->show_outline = 1;
	c->outline_thickness = 1.0f;
	c->outline_color_r = 100;
	c->outline_color_g = 100;
	c->outline_color_b = 100;
	c->outline_color_a = 255;
	c->disable_cursor_changes = 0;

	c->raw_scale = 1.0f;
	c->trigger_key = 0;
	c->freeze_pointer = -1;
}

int config_freeze_pointer(const config_t *c)
{
	return c->freeze_pointer < 0 ? !c->use_send_input_api : c->freeze_pointer != 0;
}

static char *trim(char *s)
{
	while (isspace((unsigned char)*s))
		s++;
	if (!*s)
		return s;
	char *e = s + strlen(s) - 1;
	while (e > s && isspace((unsigned char)*e))
		e--;
	e[1] = 0;
	return s;
}

/* Same quirks as ParseHexColor(): optional '#', RRGGBB or RRGGBBAA, else ignored. */
static void parse_hex_color(const char *hex, int *r, int *g, int *b, int *a)
{
	if (*hex == '#')
		hex++;
	unsigned long v = strtoul(hex, NULL, 16);
	size_t len = strlen(hex);
	if (len == 6) {
		*r = (v >> 16) & 0xFF;
		*g = (v >> 8) & 0xFF;
		*b = v & 0xFF;
		*a = 255;
	} else if (len == 8) {
		*r = (v >> 24) & 0xFF;
		*g = (v >> 16) & 0xFF;
		*b = (v >> 8) & 0xFF;
		*a = v & 0xFF;
	}
}

int config_parse_line(config_t *c, char *line)
{
	char *p = trim(line);
	if (*p == '#' || *p == 0)
		return 0;
	char *eq = strchr(p, '=');
	if (!eq)
		return 0;
	*eq = 0;
	const char *key = trim(p);
	const char *val = trim(eq + 1);

#define INT(k, f) if (!strcmp(key, k)) { c->f = atoi(val); return 1; }
#define FLT(k, f) if (!strcmp(key, k)) { c->f = (float)atof(val); return 1; }
	INT("min_scroll", min_scroll)
	INT("max_scroll", max_scroll)
	FLT("sensitivity", sensitivity)
	FLT("ramp_exponent", ramp_exponent)
	INT("update_frequency", update_frequency)
	INT("trigger_middle_mouse", trigger_middle_mouse)
	if (!strcmp(key, "trigger_vk_code")) {
		c->trigger_vk_code = (int)strtol(val, NULL, 0);
		return 1;
	}
	INT("emulate_touchpad_scrolling", emulate_touchpad_scrolling)
	INT("middle_mouse_passthrough", middle_mouse_passthrough)
	INT("keyboard_passthrough", keyboard_passthrough)
	INT("drag_threshold", drag_threshold)
	INT("dead_zone", dead_zone)
	INT("axis_lock_threshold", axis_lock_threshold)
	INT("use_send_input_api", use_send_input_api)
	INT("show_indicator", show_indicator)
	INT("indicator_size", indicator_size)
	INT("indicator_cross_thickness", indicator_cross_thickness)
	FLT("indicator_thickness", indicator_thickness)
	INT("indicator_filled", indicator_filled)
	INT("fun_stats", fun_stats)
	INT("natural_scrolling", natural_scrolling)
	INT("show_outline", show_outline)
	FLT("outline_thickness", outline_thickness)
	INT("disable_cursor_changes", disable_cursor_changes)
	if (!strcmp(key, "indicator_color")) {
		parse_hex_color(val, &c->indicator_color_r, &c->indicator_color_g,
				&c->indicator_color_b, &c->indicator_color_a);
		return 1;
	}
	if (!strcmp(key, "outline_color")) {
		parse_hex_color(val, &c->outline_color_r, &c->outline_color_g,
				&c->outline_color_b, &c->outline_color_a);
		return 1;
	}
	if (!strcmp(key, "trigger_mode")) {
		/* Anything other than "hold" means toggle, as in the original. */
		c->trigger_mode = strcasecmp(val, "hold") == 0 ? MODE_HOLD : MODE_TOGGLE;
		return 1;
	}
	if (!strcmp(key, "dead_zone_shape")) {
		c->dead_zone_shape = strcasecmp(val, "square") == 0 ? SHAPE_SQUARE : SHAPE_CIRCLE;
		return 1;
	}
	if (!strcmp(key, "indicator_shape")) {
		if (!strcasecmp(val, "square"))
			c->indicator_shape = SHAPE_SQUARE;
		else if (!strcasecmp(val, "cross"))
			c->indicator_shape = SHAPE_CROSS;
		else
			c->indicator_shape = SHAPE_CIRCLE;
		return 1;
	}

	/* Linux-only extras. */
	FLT("raw_scale", raw_scale)
	if (!strcmp(key, "trigger_key")) {
		c->trigger_key = (int)strtol(val, NULL, 0);
		return 1;
	}
	if (!strcmp(key, "freeze_pointer")) {
		c->freeze_pointer = !strcasecmp(val, "auto") ? -1 : atoi(val) != 0;
		return 1;
	}
#undef INT
#undef FLT
	return 0;
}

void config_load_string(config_t *c, const char *text)
{
	config_defaults(c);
	char *copy = strdup(text);
	if (!copy)
		return;
	char *save = NULL;
	for (char *l = strtok_r(copy, "\n", &save); l; l = strtok_r(NULL, "\n", &save))
		config_parse_line(c, l);
	free(copy);
}

int config_load(config_t *c, const char *path)
{
	config_defaults(c);
	FILE *f = fopen(path, "r");
	if (!f)
		return -1;
	char line[512];
	while (fgets(line, sizeof line, f))
		config_parse_line(c, line);
	fclose(f);
	return 0;
}
