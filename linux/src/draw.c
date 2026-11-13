#include "draw.h"

#include <math.h>

void draw_size(const config_t *cfg, int *w, int *h)
{
	int s = cfg->indicator_size;
	int padding = (int)(cfg->indicator_thickness + cfg->outline_thickness + 5);
	*w = *h = (s + padding) * 2;
}

/* ---- signed distance functions (negative inside) ---- */

static float sd_circle(float x, float y, float r)
{
	return hypotf(x, y) - r;
}

static float sd_box(float x, float y, float hx, float hy)
{
	float qx = fabsf(x) - hx, qy = fabsf(y) - hy;
	float ox = fmaxf(qx, 0.f), oy = fmaxf(qy, 0.f);
	return hypotf(ox, oy) + fminf(fmaxf(qx, qy), 0.f);
}

/* 1-pixel-wide anti-aliased edge: coverage from signed distance. */
static float cov(float sd)
{
	float c = 0.5f - sd;
	return c < 0.f ? 0.f : c > 1.f ? 1.f : c;
}

typedef struct {
	float r, g, b, a; /* premultiplied */
} acc_t;

static void over(acc_t *d, float r, float g, float b, float a, float coverage)
{
	float sa = a * coverage;
	float k = 1.f - sa;
	d->r = r * sa + d->r * k;
	d->g = g * sa + d->g * k;
	d->b = b * sa + d->b * k;
	d->a = sa + d->a * k;
}

static uint8_t to8(float v)
{
	int i = (int)(v * 255.f + 0.5f);
	return (uint8_t)(i < 0 ? 0 : i > 255 ? 255 : i);
}

void draw_indicator(uint32_t *px, int w, int h, const config_t *c)
{
	const float s = (float)c->indicator_size;
	const float T = c->indicator_thickness, hT = T / 2.f;
	const float O = c->outline_thickness, hO = O / 2.f;
	const float ct = (float)c->indicator_cross_thickness;
	const float mx = w / 2.f, my = h / 2.f;

	const float mr = c->indicator_color_r / 255.f, mg = c->indicator_color_g / 255.f,
		    mb = c->indicator_color_b / 255.f, ma = c->indicator_color_a / 255.f;
	const float or_ = c->outline_color_r / 255.f, og = c->outline_color_g / 255.f,
		    ob = c->outline_color_b / 255.f, oa = c->outline_color_a / 255.f;

	for (int yi = 0; yi < h; yi++) {
		for (int xi = 0; xi < w; xi++) {
			float x = xi + 0.5f - mx, y = yi + 0.5f - my;
			acc_t a = {0, 0, 0, 0};

			/* Like the GDI+ version: main shape first, outline pen on top. */
			switch (c->indicator_shape) {
			case SHAPE_SQUARE:
				if (c->indicator_filled) {
					over(&a, mr, mg, mb, ma, cov(sd_box(x, y, s, s)));
					if (c->show_outline)
						over(&a, or_, og, ob, oa, cov(fabsf(sd_box(x, y, s, s)) - hO));
				} else {
					float outer = sd_box(x, y, s + hT, s + hT);
					float inner = sd_box(x, y, s - hT, s - hT);
					over(&a, mr, mg, mb, ma, cov(fmaxf(outer, -inner)));
					if (c->show_outline) {
						over(&a, or_, og, ob, oa, cov(fabsf(outer) - hO));
						over(&a, or_, og, ob, oa, cov(fabsf(inner) - hO));
					}
				}
				break;
			case SHAPE_CROSS: {
				float sd = fminf(sd_box(x, y, s, ct), sd_box(x, y, ct, s));
				over(&a, mr, mg, mb, ma, cov(sd));
				if (c->show_outline)
					over(&a, or_, og, ob, oa, cov(fabsf(sd) - hO));
				break;
			}
			case SHAPE_CIRCLE:
			default:
				if (c->indicator_filled) {
					over(&a, mr, mg, mb, ma, cov(sd_circle(x, y, s)));
					if (c->show_outline)
						over(&a, or_, og, ob, oa, cov(fabsf(sd_circle(x, y, s)) - hO));
				} else {
					float d = sd_circle(x, y, s);
					over(&a, mr, mg, mb, ma, cov(fabsf(d) - hT));
					if (c->show_outline) {
						over(&a, or_, og, ob, oa, cov(fabsf(d - hT) - hO));
						over(&a, or_, og, ob, oa, cov(fabsf(d + hT) - hO));
					}
				}
				break;
			}
			px[yi * w + xi] = ((uint32_t)to8(a.a) << 24) | ((uint32_t)to8(a.r) << 16) |
					  ((uint32_t)to8(a.g) << 8) | to8(a.b);
		}
	}
}
