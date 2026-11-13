/* wlautoscroll - on-screen scroll indicator (wlr-layer-shell, click-through). */
#ifndef WLAS_OVERLAY_H
#define WLAS_OVERLAY_H

#include "config.h"

enum {
	OV_SHOWN = 1,       /* the indicator is on screen */
	OV_FOCUS_DIRTY = 2, /* pointer focus may be stale: caller should nudge */
};

/* Show the indicator at the pointer. May block for a few frames; returns a
 * mask of OV_* flags (0 if the overlay is unavailable, which is not fatal). */
int overlay_show(const config_t *cfg);
void overlay_hide(void);

#endif
