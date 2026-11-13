#include "log.h"

#include <stdarg.h>
#include <stdio.h>

/* 0 = errors only, 1 = info (default), 2 = debug (-v). */
int wlas_verbose = 1;

void wlas_log(int level, const char *fmt, ...)
{
	if (level > wlas_verbose)
		return;
	static const char *tag[] = {"error: ", "", "debug: "};
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "wlautoscroll: %s", tag[level]);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}
