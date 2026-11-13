#include "stats.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void stats_default_path(char *buf, size_t n)
{
	const char *state = getenv("XDG_STATE_HOME");
	const char *home = getenv("HOME");
	if (state && *state == '/')
		snprintf(buf, n, "%s/wlautoscroll/stats.ini", state);
	else
		snprintf(buf, n, "%s/.local/state/wlautoscroll/stats.ini", home ? home : ".");
}

int stats_load(stats_t *s, const char *path)
{
	memset(s, 0, sizeof *s);
	FILE *f = fopen(path, "r");
	if (!f)
		return -1;
	char line[128];
	int in_stats = 0;
	while (fgets(line, sizeof line, f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (line[0] == '[') {
			in_stats = !strcmp(line, "[Stats]");
			continue;
		}
		char *eq = strchr(line, '=');
		if (!in_stats || !eq)
			continue;
		*eq = 0;
		unsigned long long v = strtoull(eq + 1, NULL, 10);
		if (!strcmp(line, "TotalPixels")) s->total = v;
		else if (!strcmp(line, "Up")) s->up = v;
		else if (!strcmp(line, "Down")) s->down = v;
		else if (!strcmp(line, "Left")) s->left = v;
		else if (!strcmp(line, "Right")) s->right = v;
		else if (!strcmp(line, "Unuploaded")) s->session = v;
	}
	fclose(f);
	return 0;
}

static void mkdir_p(char *path)
{
	for (char *p = path + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = 0;
		mkdir(path, 0755);
		*p = '/';
	}
}

int stats_save(const stats_t *s, const char *path)
{
	char tmp[600], dir[600];
	snprintf(dir, sizeof dir, "%s", path);
	mkdir_p(dir);
	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	FILE *f = fopen(tmp, "w");
	if (!f)
		return -1;
	fprintf(f, "[Stats]\nTotalPixels=%llu\nUp=%llu\nDown=%llu\nLeft=%llu\nRight=%llu\nUnuploaded=%llu\n",
		s->total, s->up, s->down, s->left, s->right, s->session);
	if (fclose(f) != 0 || rename(tmp, path) != 0) {
		unlink(tmp);
		return -1;
	}
	return 0;
}

void stats_print(FILE *f, const stats_t *s)
{
	/* Same wording and arithmetic as ShowLocalStats() (96 dpi). */
	double meters = (double)s->total * 0.0254 / 96.0;
	fprintf(f,
		"WinAutoScroll Statistics\n\n"
		"Total Scrolled: %.2f virtual metres\n"
		"Total Pixels: %llu\n\n"
		"Session Pixels (Unuploaded): %llu\n\n"
		"Direction Breakdown:\n"
		"  Up: %llu\n  Down: %llu\n  Left: %llu\n  Right: %llu\n",
		meters, s->total, s->session, s->up, s->down, s->left, s->right);
}
