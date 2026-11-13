#include "input.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "log.h"

#define PHYS_MARK "wlautoscroll"
#define LONG_BITS (8 * sizeof(unsigned long))
#define NLONGS(bits) (((bits) + LONG_BITS - 1) / LONG_BITS)
#define TEST_BIT(b, a) (((a)[(b) / LONG_BITS] >> ((b) % LONG_BITS)) & 1UL)

static struct {
	core_t *core;
	const config_t *cfg;
	input_dev_t *devs;
	int wheel_fd;
	int acc_v, acc_h; /* sub-notch remainder for the legacy REL_WHEEL events */
	bool grab_ptr, grab_kbd; /* what the currently open devices were opened for */
	input_dev_t *cur;        /* device whose events are being processed */
	struct input_event *cur_out;
	int *cur_no;
} g = {.wheel_fd = -1};

/* ------------------------------------------------------------------ */
/* forwarding logic                                                    */
/* ------------------------------------------------------------------ */

void input_bind(core_t *core, const config_t *cfg)
{
	g.core = core;
	g.cfg = cfg;
}

static void resync_middle(input_dev_t *d)
{
	if (d->fd < 0 || !d->pointer)
		return;
	unsigned long keys[NLONGS(KEY_MAX + 1)];
	memset(keys, 0, sizeof keys);
	if (ioctl(d->fd, EVIOCGKEY(sizeof keys), keys) < 0)
		return;
	if (!TEST_BIT(BTN_MIDDLE, keys) && g.core->mid_swallowed)
		core_button(g.core, BTN_MIDDLE, 0);
}

static void queue(struct input_event *out, int *no, int type, int code, int value)
{
	out[*no] = (struct input_event){.type = type, .code = code, .value = value};
	(*no)++;
}

void input_emit_middle(int value)
{
	if (!g.cur || !g.cur_out)
		return;
	queue(g.cur_out, g.cur_no, EV_KEY, BTN_MIDDLE, value);
	queue(g.cur_out, g.cur_no, EV_SYN, SYN_REPORT, 0);
}

int input_process(input_dev_t *d, const struct input_event *in, int n,
		  struct input_event *out)
{
	int no = 0;
	g.cur = d;
	g.cur_out = out;
	g.cur_no = &no;

	for (int i = 0; i < n; i++) {
		struct input_event ev = in[i];
		bool swallow = false;

		if (ev.type == EV_SYN) {
			if (ev.code == SYN_DROPPED) {
				/* Kernel buffer overflowed: ignore everything up to
				 * the next SYN_REPORT, then re-read the button state. */
				d->dropped = true;
			} else if (ev.code == SYN_REPORT) {
				if (d->dropped) {
					d->dropped = false;
					resync_middle(d);
				}
				if (d->grab && d->dirty) {
					out[no++] = ev;
					d->dirty = false;
				}
			}
			continue;
		}
		if (d->dropped)
			continue;

		if (ev.type == EV_KEY) {
			if (d->pointer && ev.code == BTN_MIDDLE)
				swallow = core_button(g.core, ev.code, ev.value);
			else if (d->keyboard)
				swallow = core_key(g.core, ev.code, ev.value);
		} else if (ev.type == EV_REL && d->pointer) {
			if (ev.code == REL_X)
				swallow = core_motion(g.core, ev.value, 0);
			else if (ev.code == REL_Y)
				swallow = core_motion(g.core, 0, ev.value);
		}

		if (d->grab && !swallow) {
			out[no++] = ev;
			d->dirty = true;
		}
	}

	g.cur = NULL;
	g.cur_out = NULL;
	g.cur_no = NULL;
	return no;
}

/* ------------------------------------------------------------------ */
/* uinput helpers                                                      */
/* ------------------------------------------------------------------ */

static int open_uinput(void)
{
	int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		fd = open("/dev/input/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	return fd;
}

static void ui_bit(int fd, unsigned long req, int code)
{
	ioctl(fd, req, code);
}

static int copy_bits(int src, int dst, int type, unsigned long set_req, int max)
{
	unsigned long bits[NLONGS(KEY_MAX + 1)];
	memset(bits, 0, sizeof bits);
	if (ioctl(src, EVIOCGBIT(type, sizeof bits), bits) < 0)
		return 0;
	ui_bit(dst, UI_SET_EVBIT, type);
	for (int c = 0; c <= max; c++)
		if (TEST_BIT(c, bits))
			ui_bit(dst, set_req, c);
	return 1;
}

static void finish_uinput(int fd, const char *name, const struct input_id *id)
{
	struct uinput_setup us;
	memset(&us, 0, sizeof us);
	us.id = *id;
	snprintf(us.name, sizeof us.name, "%s", name);
	ioctl(fd, UI_DEV_SETUP, &us);
	ioctl(fd, UI_SET_PHYS, PHYS_MARK);
}

/* A uinput twin of `src_fd` with the same identity and capabilities. */
static int clone_device(int src_fd, const char *name)
{
	int fd = open_uinput();
	if (fd < 0)
		return -1;

	struct input_id id;
	memset(&id, 0, sizeof id);
	ioctl(src_fd, EVIOCGID, &id);

	copy_bits(src_fd, fd, EV_KEY, UI_SET_KEYBIT, KEY_MAX);
	copy_bits(src_fd, fd, EV_REL, UI_SET_RELBIT, REL_MAX);
	copy_bits(src_fd, fd, EV_MSC, UI_SET_MSCBIT, MSC_MAX);
	copy_bits(src_fd, fd, EV_LED, UI_SET_LEDBIT, LED_MAX);

	unsigned long props[NLONGS(INPUT_PROP_MAX + 1)];
	memset(props, 0, sizeof props);
	if (ioctl(src_fd, EVIOCGPROP(sizeof props), props) >= 0)
		for (int p = 0; p <= INPUT_PROP_MAX; p++)
			if (TEST_BIT(p, props))
				ui_bit(fd, UI_SET_PROPBIT, p);

	finish_uinput(fd, name, &id);
	if (ioctl(fd, UI_DEV_CREATE) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static int create_wheel(void)
{
	int fd = open_uinput();
	if (fd < 0)
		return -1;

	ui_bit(fd, UI_SET_EVBIT, EV_KEY);
	ui_bit(fd, UI_SET_KEYBIT, BTN_LEFT);
	ui_bit(fd, UI_SET_KEYBIT, BTN_RIGHT);
	ui_bit(fd, UI_SET_KEYBIT, BTN_MIDDLE);
	ui_bit(fd, UI_SET_EVBIT, EV_REL);
	/* REL_X/Y are declared (never used for real motion) so udev/libinput
	 * classify this as a mouse and give its wheel events to the pointer. */
	ui_bit(fd, UI_SET_RELBIT, REL_X);
	ui_bit(fd, UI_SET_RELBIT, REL_Y);
	ui_bit(fd, UI_SET_RELBIT, REL_WHEEL);
	ui_bit(fd, UI_SET_RELBIT, REL_HWHEEL);
	ui_bit(fd, UI_SET_RELBIT, REL_WHEEL_HI_RES);
	ui_bit(fd, UI_SET_RELBIT, REL_HWHEEL_HI_RES);

	struct input_id id = {.bustype = BUS_VIRTUAL, .vendor = 0x1209, .product = 0x5741, .version = 1};
	finish_uinput(fd, "wlautoscroll wheel", &id);
	if (ioctl(fd, UI_DEV_CREATE) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static void write_events(int fd, const struct input_event *ev, int n)
{
	if (fd < 0 || n <= 0)
		return;
	ssize_t r = write(fd, ev, (size_t)n * sizeof *ev);
	if (r < 0 && errno != EAGAIN)
		LOGD("uinput write failed: %s", strerror(errno));
}

void input_emit_scroll(int vert, int horiz)
{
	struct input_event e[8];
	int n = 0;
	if (vert) {
		queue(e, &n, EV_REL, REL_WHEEL_HI_RES, vert);
		g.acc_v += vert;
		int whole = g.acc_v / 120;
		g.acc_v -= whole * 120;
		if (whole)
			queue(e, &n, EV_REL, REL_WHEEL, whole);
	}
	if (horiz) {
		queue(e, &n, EV_REL, REL_HWHEEL_HI_RES, horiz);
		g.acc_h += horiz;
		int whole = g.acc_h / 120;
		g.acc_h -= whole * 120;
		if (whole)
			queue(e, &n, EV_REL, REL_HWHEEL, whole);
	}
	if (!n)
		return;
	queue(e, &n, EV_SYN, SYN_REPORT, 0);
	write_events(g.wheel_fd, e, n);
}

void input_nudge(void)
{
	struct input_event e[4];
	int n = 0;
	queue(e, &n, EV_REL, REL_X, 1);
	queue(e, &n, EV_SYN, SYN_REPORT, 0);
	queue(e, &n, EV_REL, REL_X, -1);
	queue(e, &n, EV_SYN, SYN_REPORT, 0);
	write_events(g.wheel_fd, e, n);
}

/* ------------------------------------------------------------------ */
/* device lifecycle                                                    */
/* ------------------------------------------------------------------ */

static void free_dev(void *p)
{
	free(p);
}

static void remove_dev(input_dev_t *d)
{
	if (d->state == DEV_ACTIVE) {
		LOGI("removed %s (%s)", d->path, d->name);
		d->h.cb = NULL;
		d->clone_h.cb = NULL;
		if (d->fd >= 0)
			loop_del(&d->h);
		if (d->leds && d->clone_fd >= 0)
			loop_del(&d->clone_h);
	}
	if (d->clone_fd >= 0) {
		ioctl(d->clone_fd, UI_DEV_DESTROY);
		close(d->clone_fd);
	}
	if (d->fd >= 0)
		close(d->fd); /* closing also drops the grab */
	for (input_dev_t **pp = &g.devs; *pp; pp = &(*pp)->next) {
		if (*pp == d) {
			*pp = d->next;
			break;
		}
	}
	/* Later events in the same epoll batch may still point at d. */
	loop_defer(free_dev, d);
}

static void dev_cb(fdh_t *h, unsigned events)
{
	input_dev_t *d = h->data;
	struct input_event in[64], out[64 + 4];

	for (;;) {
		ssize_t r = read(d->fd, in, sizeof in);
		if (r < 0) {
			if (errno == EAGAIN)
				break;
			if (errno == EINTR)
				continue;
			remove_dev(d); /* ENODEV: unplugged */
			return;
		}
		if (r == 0) {
			remove_dev(d);
			return;
		}
		int n = (int)(r / (ssize_t)sizeof in[0]);
		int no = input_process(d, in, n, out);
		if (no)
			write_events(d->clone_fd, out, no);
		if ((size_t)r < sizeof in)
			break;
	}
	if (events & (EPOLLHUP | EPOLLERR))
		remove_dev(d);
}

/* LEDs (caps lock...) are set on the clone by the compositor; relay them. */
static void clone_cb(fdh_t *h, unsigned events)
{
	input_dev_t *d = h->data;
	struct input_event ev[16];
	ssize_t r = read(d->clone_fd, ev, sizeof ev);
	if (r <= 0)
		return;
	for (int i = 0; i < (int)(r / (ssize_t)sizeof ev[0]); i++)
		if (ev[i].type == EV_LED || ev[i].type == EV_SYN)
			write_events(d->fd, &ev[i], 1);
}

static input_dev_t *find_dev(const char *path)
{
	for (input_dev_t *d = g.devs; d; d = d->next)
		if (!strcmp(d->path, path))
			return d;
	return NULL;
}

/* Grabbing while a key is down would leave it stuck "pressed" on the real
 * device from the compositor's point of view, so wait (briefly) for neutral. */
static void wait_keys_released(int fd)
{
	for (int i = 0; i < 40; i++) {
		unsigned long keys[NLONGS(KEY_MAX + 1)];
		memset(keys, 0, sizeof keys);
		if (ioctl(fd, EVIOCGKEY(sizeof keys), keys) < 0)
			return;
		bool any = false;
		for (size_t k = 0; k < NLONGS(KEY_MAX + 1); k++)
			any |= keys[k] != 0;
		if (!any)
			return;
		usleep(50 * 1000);
	}
}

static void try_open(const char *path)
{
	input_dev_t *d = find_dev(path);
	if (d && d->state != DEV_RETRY)
		return;
	if (!d) {
		d = calloc(1, sizeof *d);
		if (!d)
			return;
		snprintf(d->path, sizeof d->path, "%s", path);
		d->fd = d->clone_fd = -1;
		d->next = g.devs;
		g.devs = d;
	}

	int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		d->state = DEV_RETRY;
		if (!d->warned && errno == EACCES) {
			LOGI("no permission for %s (is your user in the 'input' group?)", path);
			d->warned = true;
		}
		return;
	}
	d->state = DEV_IGNORED;

	char phys[64] = "";
	ioctl(fd, EVIOCGPHYS(sizeof phys), phys);
	if (!strcmp(phys, PHYS_MARK)) { /* one of our own clones */
		close(fd);
		return;
	}
	ioctl(fd, EVIOCGNAME(sizeof d->name), d->name);

	unsigned long ev[NLONGS(EV_MAX + 1)] = {0};
	unsigned long key[NLONGS(KEY_MAX + 1)] = {0};
	unsigned long rel[NLONGS(REL_MAX + 1)] = {0};
	ioctl(fd, EVIOCGBIT(0, sizeof ev), ev);
	ioctl(fd, EVIOCGBIT(EV_KEY, sizeof key), key);
	ioctl(fd, EVIOCGBIT(EV_REL, sizeof rel), rel);

	bool pointer = TEST_BIT(EV_REL, ev) && TEST_BIT(REL_X, rel) && TEST_BIT(REL_Y, rel) &&
		       TEST_BIT(BTN_LEFT, key) && TEST_BIT(BTN_MIDDLE, key) && !TEST_BIT(EV_ABS, ev);
	bool keyboard = !pointer && TEST_BIT(EV_KEY, ev) && TEST_BIT(KEY_ESC, key) &&
			TEST_BIT(KEY_A, key);

	bool grab = false;
	if (pointer) {
		if (!g.cfg->trigger_middle_mouse) {
			close(fd);
			return;
		}
		grab = true;
	} else if (keyboard) {
		grab = core_wants_keyboard_grab(g.core);
	} else {
		close(fd);
		return;
	}

	if (grab) {
		wait_keys_released(fd);
		if (ioctl(fd, EVIOCGRAB, (void *)1) < 0) {
			LOGI("skipping %s (%s): cannot grab (%s)", path, d->name, strerror(errno));
			close(fd);
			return;
		}
		d->clone_fd = clone_device(fd, d->name);
		if (d->clone_fd < 0) {
			LOGE("cannot create uinput clone of %s: %s (is /dev/uinput writable?)",
			     d->name, strerror(errno));
			ioctl(fd, EVIOCGRAB, (void *)0);
			close(fd);
			return;
		}
	}

	d->fd = fd;
	d->pointer = pointer;
	d->keyboard = keyboard;
	d->grab = grab;
	d->leds = grab && TEST_BIT(EV_LED, ev);
	d->state = DEV_ACTIVE;
	d->h = (fdh_t){.fd = fd, .cb = dev_cb, .data = d};
	loop_add(&d->h, EPOLLIN);
	if (d->leds) {
		d->clone_h = (fdh_t){.fd = d->clone_fd, .cb = clone_cb, .data = d};
		loop_add(&d->clone_h, EPOLLIN);
	}
	LOGI("%s %s (%s)%s", grab ? "grabbed" : "watching", path, d->name,
	     pointer ? " [pointer]" : " [keyboard]");
}

void input_scan(void)
{
	DIR *dir = opendir("/dev/input");
	if (!dir) {
		LOGE("cannot open /dev/input: %s", strerror(errno));
		return;
	}
	struct dirent *e;
	while ((e = readdir(dir))) {
		if (strncmp(e->d_name, "event", 5))
			continue;
		char path[320];
		snprintf(path, sizeof path, "/dev/input/%s", e->d_name);
		try_open(path);
	}
	closedir(dir);
	g.grab_ptr = g.cfg->trigger_middle_mouse;
	g.grab_kbd = core_wants_keyboard_grab(g.core);
}

void input_shutdown(void)
{
	while (g.devs)
		remove_dev(g.devs);
	if (g.wheel_fd >= 0) {
		ioctl(g.wheel_fd, UI_DEV_DESTROY);
		close(g.wheel_fd);
		g.wheel_fd = -1;
	}
}

void input_reconfigure(void)
{
	core_reconfigure(g.core);
	bool ptr = g.cfg->trigger_middle_mouse;
	bool kbd = core_wants_keyboard_grab(g.core);
	if (ptr == g.grab_ptr && kbd == g.grab_kbd)
		return;
	LOGI("grab requirements changed, re-opening input devices");
	core_stop(g.core);
	while (g.devs)
		remove_dev(g.devs);
	input_scan();
}

int input_init(core_t *core, const config_t *cfg)
{
	input_bind(core, cfg);
	g.wheel_fd = create_wheel();
	if (g.wheel_fd < 0) {
		LOGE("cannot create virtual wheel device: %s", strerror(errno));
		LOGE("need write access to /dev/uinput (see the udev rule + 'input' group in the README)");
		return -1;
	}
	return 0;
}

