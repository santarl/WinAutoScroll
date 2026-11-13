#include "overlay.h"

/* No indicator yet; scrolling works without one. */
int overlay_show(const config_t *cfg)
{
	(void)cfg;
	return 0;
}

void overlay_hide(void) {}
