/*
 * wlautoscroll - middle-click autoscroll for Wayland.
 * A port of WinAutoScroll that reads the same config.ini.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/file.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "config.h"
#include "core.h"
#include "input.h"
#include "log.h"
#include "loop.h"
#include "overlay.h"
#include "stats.h"

#ifndef WLAS_VERSION
#define WLAS_VERSION "dev"
#endif

static struct {
	config_t cfg;
	core_t core;
	char cfg_path[512]; /* "" = defaults */
	char explicit_cfg[512];
	int timer_fd;
	fdh_t timer_h, sig_h;
	char stats_path[512];
} g;

/* ------------------------------------------------------------------ */
/* config discovery                                                    */
/* ------------------------------------------------------------------ */

static int file_exists(const char *p)
{
	return access(p, R_OK) == 0;
}

/* -c path, then $XDG_CONFIG_HOME, ~/.config, /etc, finally ./config.ini
 * (the Windows build reads config.ini from its working directory). */
static void resolve_config(char *out, size_t n)
{
	out[0] = 0;
	if (g.explicit_cfg[0]) {
		snprintf(out, n, "%s", g.explicit_cfg);
		return;
	}
	const char *xdg = getenv("XDG_CONFIG_HOME");
	const char *home = getenv("HOME");
	char cand[512];
	if (xdg && *xdg == '/') {
		snprintf(cand, sizeof cand, "%s/wlautoscroll/config.ini", xdg);
		if (file_exists(cand)) { snprintf(out, n, "%s", cand); return; }
	}
	if (home) {
		snprintf(cand, sizeof cand, "%s/.config/wlautoscroll/config.ini", home);
		if (file_exists(cand)) { snprintf(out, n, "%s", cand); return; }
	}
	if (file_exists("/etc/wlautoscroll/config.ini")) {
		snprintf(out, n, "%s", "/etc/wlautoscroll/config.ini");
		return;
	}
	if (file_exists("config.ini"))
		snprintf(out, n, "%s", "config.ini");
}

static void load_config(void)
{
	resolve_config(g.cfg_path, sizeof g.cfg_path);
	if (g.cfg_path[0] && config_load(&g.cfg, g.cfg_path) == 0) {
		LOGI("config: %s", g.cfg_path);
	} else {
		config_defaults(&g.cfg);
		if (g.cfg_path[0])
			LOGE("cannot read %s, using defaults", g.cfg_path);
		else
			LOGI("no config.ini found, using defaults");
	}
	if (g.cfg.update_frequency <= 0)
		g.cfg.update_frequency = 60;
}

/* ------------------------------------------------------------------ */
/* engine callbacks                                                    */
/* ------------------------------------------------------------------ */

static void arm_timer(int on)
{
	struct itimerspec its = {0};
	if (on) {
		long ns = 1000000000L / g.cfg.update_frequency;
		its.it_interval.tv_sec = its.it_value.tv_sec = ns / 1000000000L;
		its.it_interval.tv_nsec = its.it_value.tv_nsec = ns % 1000000000L;
	}
	timerfd_settime(g.timer_fd, 0, &its, NULL);
}

static void on_emit_middle(void *ud, int value) { input_emit_middle(value); }
static void on_scroll(void *ud, int v, int h) { input_emit_scroll(v, h); }

static void on_begin(void *ud)
{
	int fl = 0;
	if (g.cfg.show_indicator)
		fl = overlay_show(&g.cfg);
	/* The overlay probe briefly takes pointer focus; wiggle so the compositor
	 * hands it back to the window under the cursor before we start scrolling. */
	if (fl & OV_FOCUS_DIRTY)
		input_nudge();
	arm_timer(1);
	LOGD("autoscroll started");
}

static void on_end(void *ud)
{
	arm_timer(0);
	overlay_hide();
	if (g.cfg.fun_stats && g.core.stats_dirty) {
		stats_save(&g.core.stats, g.stats_path);
		g.core.stats_dirty = false;
	}
	LOGD("autoscroll stopped");
}

static void timer_cb(fdh_t *h, unsigned events)
{
	uint64_t n;
	if (read(h->fd, &n, sizeof n) > 0)
		core_tick(&g.core); /* one tick even if we were late */
}

static void signal_cb(fdh_t *h, unsigned events)
{
	struct signalfd_siginfo si;
	while (read(h->fd, &si, sizeof si) == sizeof si) {
		switch (si.ssi_signo) {
		case SIGHUP:
			LOGI("reloading config");
			core_stop(&g.core);
			load_config();
			input_reconfigure();
			break;
		case SIGUSR1:
			core_set_paused(&g.core, !g.core.paused);
			LOGI("%s", g.core.paused ? "paused" : "resumed");
			break;
		default:
			loop_quit();
		}
	}
}

/* ------------------------------------------------------------------ */
/* single instance: a second launch reloads the first (like Windows)   */
/* ------------------------------------------------------------------ */

static int acquire_instance(void)
{
	char path[256];
	const char *rt = getenv("XDG_RUNTIME_DIR");
	if (rt && *rt)
		snprintf(path, sizeof path, "%s/wlautoscroll.lock", rt);
	else
		snprintf(path, sizeof path, "/tmp/wlautoscroll-%u.lock", (unsigned)getuid());

	int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (fd < 0)
		return 0; /* can't coordinate; run anyway */
	if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
		char buf[32];
		int len = snprintf(buf, sizeof buf, "%d\n", (int)getpid());
		if (ftruncate(fd, 0) == 0 && write(fd, buf, (size_t)len) < 0)
			LOGD("cannot write pid file");
		return 0; /* keep fd open: the lock lives as long as we do */
	}
	char buf[32] = "";
	ssize_t r = read(fd, buf, sizeof buf - 1);
	int pid = r > 0 ? atoi(buf) : 0;
	if (pid > 1 && kill(pid, SIGHUP) == 0)
		fprintf(stderr, "wlautoscroll is already running (pid %d); asked it to reload its config\n", pid);
	else
		fprintf(stderr, "wlautoscroll is already running\n");
	return 1;
}

/* ------------------------------------------------------------------ */

static void usage(FILE *f)
{
	fprintf(f,
		"wlautoscroll %s - middle-click autoscroll for Wayland\n\n"
		"usage: wlautoscroll [options]\n"
		"  -c, --config FILE      config.ini to use (same format as WinAutoScroll)\n"
		"  -v, --verbose          debug logging\n"
		"  -q, --quiet            errors only\n"
		"      --stats            print saved scroll statistics and exit\n"
		"      --test-overlay[=S] show the indicator at the pointer for S seconds (default 3)\n"
		"  -V, --version\n"
		"  -h, --help\n\n"
		"signals: SIGHUP reload config, SIGUSR1 pause/resume, SIGTERM quit.\n"
		"Starting a second instance reloads the running one.\n",
		WLAS_VERSION);
}

int main(int argc, char **argv)
{
	double test_overlay = -1;
	int show_stats = 0;

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
			usage(stdout);
			return 0;
		} else if (!strcmp(a, "-V") || !strcmp(a, "--version")) {
			printf("wlautoscroll %s\n", WLAS_VERSION);
			return 0;
		} else if (!strcmp(a, "-v") || !strcmp(a, "--verbose")) {
			wlas_verbose = 2;
		} else if (!strcmp(a, "-q") || !strcmp(a, "--quiet")) {
			wlas_verbose = 0;
		} else if ((!strcmp(a, "-c") || !strcmp(a, "--config")) && i + 1 < argc) {
			snprintf(g.explicit_cfg, sizeof g.explicit_cfg, "%s", argv[++i]);
		} else if (!strncmp(a, "--config=", 9)) {
			snprintf(g.explicit_cfg, sizeof g.explicit_cfg, "%s", a + 9);
		} else if (!strcmp(a, "--stats")) {
			show_stats = 1;
		} else if (!strncmp(a, "--test-overlay", 14)) {
			test_overlay = a[14] == '=' ? atof(a + 15) : 3.0;
		} else {
			fprintf(stderr, "unknown option: %s\n\n", a);
			usage(stderr);
			return 2;
		}
	}

	load_config();
	stats_default_path(g.stats_path, sizeof g.stats_path);
	if (show_stats) {
		stats_t s;
		stats_load(&s, g.stats_path);
		stats_print(stdout, &s);
		return 0;
	}

	if (test_overlay >= 0) {
		int fl = overlay_show(&g.cfg);
		if (!(fl & OV_SHOWN)) {
			fprintf(stderr, "overlay unavailable (no wlr-layer-shell, or no Wayland session)\n");
			return 1;
		}
		usleep((useconds_t)(test_overlay * 1e6));
		overlay_hide();
		return 0;
	}

	if (acquire_instance())
		return 0;

	/* Signals are handled through a signalfd so the loop stays single-threaded. */
	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, SIGHUP);
	sigaddset(&set, SIGUSR1);
	sigaddset(&set, SIGTERM);
	sigaddset(&set, SIGINT);
	sigprocmask(SIG_BLOCK, &set, NULL);
	signal(SIGPIPE, SIG_IGN); /* a vanished Wayland compositor must not kill us */

	if (loop_init() < 0) {
		LOGE("epoll: %s", strerror(errno));
		return 1;
	}
	int sfd = signalfd(-1, &set, SFD_NONBLOCK | SFD_CLOEXEC);
	g.timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	if (sfd < 0 || g.timer_fd < 0) {
		LOGE("signalfd/timerfd: %s", strerror(errno));
		return 1;
	}
	g.sig_h = (fdh_t){.fd = sfd, .cb = signal_cb};
	g.timer_h = (fdh_t){.fd = g.timer_fd, .cb = timer_cb};
	loop_add(&g.sig_h, EPOLLIN);
	loop_add(&g.timer_h, EPOLLIN);

	core_ops_t ops = {on_emit_middle, on_scroll, on_begin, on_end};
	core_init(&g.core, &g.cfg, &ops, NULL);
	if (g.cfg.fun_stats)
		stats_load(&g.core.stats, g.stats_path);

	if (input_init(&g.core, &g.cfg) < 0)
		return 1;
	input_scan();
	if (input_hotplug_start() < 0)
		LOGI("hotplug unavailable; devices are only detected at startup");

	LOGI("ready (%s mode, %s)", g.cfg.trigger_mode == MODE_HOLD ? "hold" : "toggle",
	     config_freeze_pointer(&g.cfg) ? "pointer frozen while scrolling" : "pointer follows mouse");
	loop_run();

	core_stop(&g.core);
	input_shutdown();
	if (g.cfg.fun_stats)
		stats_save(&g.core.stats, g.stats_path);
	return 0;
}
