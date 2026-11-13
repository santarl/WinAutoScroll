/* wlautoscroll - software rasteriser for the scroll indicator. */
#ifndef WLAS_DRAW_H
#define WLAS_DRAW_H

#include <stdint.h>

#include "config.h"

/* Canvas size, identical to RenderAndShowOverlay() in the Windows build. */
void draw_size(const config_t *cfg, int *w, int *h);

/* Render into w*h premultiplied ARGB32 pixels (wl_shm ARGB8888, host order).
 * The buffer must be zeroed by the caller. */
void draw_indicator(uint32_t *px, int w, int h, const config_t *cfg);

#endif
