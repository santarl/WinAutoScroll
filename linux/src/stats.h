/* wlautoscroll - stats.ini persistence (same [Stats] keys as WinAutoScroll) */
#ifndef WLAS_STATS_H
#define WLAS_STATS_H

#include <stdio.h>

#include "core.h"

/* $XDG_STATE_HOME/wlautoscroll/stats.ini (default ~/.local/state/...). */
void stats_default_path(char *buf, size_t n);
int stats_load(stats_t *s, const char *path);
int stats_save(const stats_t *s, const char *path);
void stats_print(FILE *f, const stats_t *s);

#endif
