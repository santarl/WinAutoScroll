/*
 * wlautoscroll - scroll indicator overlay.
 *
 * Speaks the Wayland wire protocol directly (a dozen requests, no libwayland,
 * no wayland-scanner, no protocol XML at build time). The connection is made
 * lazily on first use and re-made if the compositor goes away, so starting
 * before the compositor or surviving a compositor restart just works.
 *
 * The problem: Wayland clients cannot ask where the pointer is, and a
 * layer-shell surface can only be positioned relative to an output. So:
 *
 *   1. Map a transparent full-output overlay surface that accepts input.
 *      The compositor immediately sends wl_pointer.enter with the pointer's
 *      position in surface-local (= output) coordinates.
 *   2. Destroy it, and map a small indicator surface at that position with an
 *      empty input region, so it is click-through and never takes focus.
 *
 * Step 1 makes the compositor briefly focus our surface, so the caller is told
 * (OV_FOCUS_DIRTY) to nudge the pointer afterwards.
 *
 * Opcodes below are from wayland.xml and wlr-layer-shell-unstable-v1.xml.
 */
#include "overlay.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "draw.h"
#include "log.h"

/* --- object ids fixed by the protocol --- */
#define ID_DISPLAY 1

/* wl_display */
#define DISPLAY_SYNC 0
#define DISPLAY_GET_REGISTRY 1
/* wl_registry */
#define REGISTRY_BIND 0
/* wl_compositor */
#define COMPOSITOR_CREATE_SURFACE 0
#define COMPOSITOR_CREATE_REGION 1
/* wl_shm / wl_shm_pool / wl_buffer / wl_region */
#define SHM_CREATE_POOL 0
#define POOL_CREATE_BUFFER 0
#define POOL_DESTROY 1
#define BUFFER_DESTROY 0
#define REGION_DESTROY 0
#define SHM_FORMAT_ARGB8888 0
/* wl_surface */
#define SURFACE_DESTROY 0
#define SURFACE_ATTACH 1
#define SURFACE_DAMAGE 2
#define SURFACE_SET_INPUT_REGION 5
#define SURFACE_COMMIT 6
/* wl_seat */
#define SEAT_GET_POINTER 0
#define SEAT_CAP_POINTER 1
/* zwlr_layer_shell_v1 / zwlr_layer_surface_v1 */
#define LSHELL_GET_LAYER_SURFACE 0
#define LS_SET_SIZE 0
#define LS_SET_ANCHOR 1
#define LS_SET_EXCLUSIVE_ZONE 2
#define LS_SET_MARGIN 3
#define LS_SET_KEYBOARD_INTERACTIVITY 4
#define LS_ACK_CONFIGURE 6
#define LS_DESTROY 7
#define LAYER_OVERLAY 3
#define ANCHOR_TOP 1
#define ANCHOR_LEFT 4
#define ANCHOR_ALL 15
/* wp_viewporter / wp_viewport */
#define VPORTER_GET_VIEWPORT 1
#define VIEWPORT_DESTROY 0
#define VIEWPORT_SET_DESTINATION 2
/* wp_single_pixel_buffer_manager_v1 */
#define SPB_CREATE_U32_RGBA 1

#define NAMESPACE "wlautoscroll"

static struct {
	int fd;
	bool announced_unavailable;
	uint32_t next_id;
	uint8_t in[16384];
	size_t inlen;
	bool err;

	/* server-announced globals (name, version), then our bound ids */
	struct { uint32_t name, ver; } gl_comp, gl_shm, gl_seat, gl_lshell, gl_vp, gl_spb;
	uint32_t registry, comp, shm, seat, lshell, vporter, spb, pointer;

	uint32_t sync_cb;
	bool sync_done;

	/* probe surface */
	uint32_t p_surf, p_ls, p_buf, p_vp;
	bool p_cfg;
	uint32_t p_serial, p_w, p_h;
	bool have_enter;
	double ex, ey;

	/* indicator surface */
	uint32_t i_surf, i_ls, i_buf;
	bool i_cfg;
	uint32_t i_serial;
	bool shown;
} W = {.fd = -1};

/* ------------------------------------------------------------------ */
/* wire encoding                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
	uint8_t b[512];
	size_t n;
	uint32_t op;
} msg_t;

static void m_begin(msg_t *m, uint32_t obj, uint32_t op)
{
	memcpy(m->b, &obj, 4);
	m->op = op;
	m->n = 8;
}

static void m_u32(msg_t *m, uint32_t v)
{
	memcpy(m->b + m->n, &v, 4);
	m->n += 4;
}

static void m_str(msg_t *m, const char *s)
{
	uint32_t len = (uint32_t)strlen(s) + 1;
	m_u32(m, len);
	memcpy(m->b + m->n, s, len);
	size_t pad = (4 - (len & 3)) & 3;
	memset(m->b + m->n + len, 0, pad);
	m->n += len + pad;
}

static void m_send(msg_t *m, int pass_fd)
{
	if (W.fd < 0 || W.err)
		return;
	uint32_t hdr = ((uint32_t)m->n << 16) | m->op;
	memcpy(m->b + 4, &hdr, 4);

	struct iovec iov = {m->b, m->n};
	union {
		char buf[CMSG_SPACE(sizeof(int))];
		struct cmsghdr align;
	} u;
	struct msghdr mh = {.msg_iov = &iov, .msg_iovlen = 1};
	if (pass_fd >= 0) {
		memset(&u, 0, sizeof u);
		mh.msg_control = u.buf;
		mh.msg_controllen = sizeof u.buf;
		struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
		cm->cmsg_level = SOL_SOCKET;
		cm->cmsg_type = SCM_RIGHTS;
		cm->cmsg_len = CMSG_LEN(sizeof(int));
		memcpy(CMSG_DATA(cm), &pass_fd, sizeof(int));
	}
	ssize_t r = sendmsg(W.fd, &mh, MSG_NOSIGNAL);
	if (r != (ssize_t)m->n) {
		LOGD("wayland: send failed: %s", strerror(errno));
		W.err = true;
	}
}

/* Request with only 32-bit arguments. */
static void req_v(uint32_t obj, uint32_t op, const uint32_t *args, int n)
{
	msg_t m;
	m_begin(&m, obj, op);
	for (int i = 0; i < n; i++)
		m_u32(&m, args[i]);
	m_send(&m, -1);
}
#define REQ(obj, op, ...) \
	req_v((obj), (op), (const uint32_t[]){__VA_ARGS__}, (int)(sizeof((uint32_t[]){__VA_ARGS__}) / 4))
#define REQ0(obj, op) req_v((obj), (op), NULL, 0)

static uint32_t new_id(void)
{
	return W.next_id++;
}

/* ------------------------------------------------------------------ */
/* event decoding                                                      */
/* ------------------------------------------------------------------ */

static uint32_t rd_u32(const uint8_t **p, size_t *len)
{
	uint32_t v = 0;
	if (*len >= 4) {
		memcpy(&v, *p, 4);
		*p += 4;
		*len -= 4;
	} else {
		*len = 0;
	}
	return v;
}

/* Returns a pointer into the message (NUL terminated by the protocol). */
static const char *rd_str(const uint8_t **p, size_t *len)
{
	uint32_t n = rd_u32(p, len);
	size_t padded = (n + 3u) & ~3u;
	if (n == 0 || padded > *len)
		return "";
	const char *s = (const char *)*p;
	*p += padded;
	*len -= padded;
	return s;
}

static void on_global(const uint8_t *p, size_t len)
{
	uint32_t name = rd_u32(&p, &len);
	const char *iface = rd_str(&p, &len);
	uint32_t ver = rd_u32(&p, &len);
	if (!strcmp(iface, "wl_compositor")) W.gl_comp = (typeof(W.gl_comp)){name, ver};
	else if (!strcmp(iface, "wl_shm")) W.gl_shm = (typeof(W.gl_shm)){name, ver};
	else if (!strcmp(iface, "wl_seat") && !W.gl_seat.name) W.gl_seat = (typeof(W.gl_seat)){name, ver};
	else if (!strcmp(iface, "zwlr_layer_shell_v1")) W.gl_lshell = (typeof(W.gl_lshell)){name, ver};
	else if (!strcmp(iface, "wp_viewporter")) W.gl_vp = (typeof(W.gl_vp)){name, ver};
	else if (!strcmp(iface, "wp_single_pixel_buffer_manager_v1")) W.gl_spb = (typeof(W.gl_spb)){name, ver};
}

static void dispatch(uint32_t id, uint32_t op, const uint8_t *p, size_t len)
{
	if (id == ID_DISPLAY) {
		if (op == 0) { /* error(object, code, message) */
			uint32_t obj = rd_u32(&p, &len), code = rd_u32(&p, &len);
			LOGI("wayland protocol error on object %u (code %u): %s", obj, code, rd_str(&p, &len));
			W.err = true;
		}
	} else if (id == W.registry && op == 0) {
		on_global(p, len);
	} else if (id == W.sync_cb && op == 0) {
		W.sync_done = true;
	} else if (id == W.seat && W.seat && op == 0) { /* capabilities */
		uint32_t caps = rd_u32(&p, &len);
		if ((caps & SEAT_CAP_POINTER) && !W.pointer) {
			W.pointer = new_id();
			REQ(W.seat, SEAT_GET_POINTER, W.pointer);
		}
	} else if (id == W.pointer && W.pointer && op == 0) { /* enter(serial, surface, x, y) */
		rd_u32(&p, &len);
		uint32_t surf = rd_u32(&p, &len);
		int32_t fx = (int32_t)rd_u32(&p, &len), fy = (int32_t)rd_u32(&p, &len);
		if (surf == W.p_surf && W.p_surf) {
			W.ex = fx / 256.0; /* wl_fixed_t is 24.8 */
			W.ey = fy / 256.0;
			W.have_enter = true;
		}
	} else if (op == 0 && ((id == W.p_ls && W.p_ls) || (id == W.i_ls && W.i_ls))) {
		/* configure(serial, width, height) */
		uint32_t serial = rd_u32(&p, &len), w = rd_u32(&p, &len), h = rd_u32(&p, &len);
		if (id == W.p_ls) {
			W.p_serial = serial; W.p_w = w; W.p_h = h; W.p_cfg = true;
		} else {
			W.i_serial = serial; W.i_cfg = true;
		}
	} else if (op == 1 && ((id == W.p_ls && W.p_ls) || (id == W.i_ls && W.i_ls))) {
		LOGD("wayland: layer surface closed by compositor");
		W.err = true;
	}
}

/* Read what's available (waiting up to timeout_ms for the first byte) and
 * dispatch complete events. Returns -1 if the connection is gone. */
static int pump(int timeout_ms)
{
	struct pollfd pfd = {.fd = W.fd, .events = POLLIN};
	int pr = poll(&pfd, 1, timeout_ms);
	if (pr < 0 && errno != EINTR)
		return -1;
	if (pr > 0) {
		if (W.inlen == sizeof W.in)
			return -1;
		ssize_t r = recv(W.fd, W.in + W.inlen, sizeof W.in - W.inlen, MSG_DONTWAIT);
		if (r == 0)
			return -1;
		if (r < 0 && errno != EAGAIN && errno != EINTR)
			return -1;
		if (r > 0)
			W.inlen += (size_t)r;
	}
	size_t off = 0;
	while (W.inlen - off >= 8) {
		uint32_t id, hdr;
		memcpy(&id, W.in + off, 4);
		memcpy(&hdr, W.in + off + 4, 4);
		size_t size = hdr >> 16;
		if (size < 8 || size > sizeof W.in)
			return -1;
		if (W.inlen - off < size)
			break;
		dispatch(id, hdr & 0xffff, W.in + off + 8, size - 8);
		off += size;
	}
	if (off) {
		memmove(W.in, W.in + off, W.inlen - off);
		W.inlen -= off;
	}
	return W.err ? -1 : 0;
}

static long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static bool wait_for(const bool *flag, int timeout_ms)
{
	long end = now_ms() + timeout_ms;
	while (!*flag) {
		long left = end - now_ms();
		if (left <= 0 || pump((int)left) < 0)
			return false;
	}
	return true;
}

static bool roundtrip(void)
{
	W.sync_cb = new_id();
	W.sync_done = false;
	REQ(ID_DISPLAY, DISPLAY_SYNC, W.sync_cb);
	return wait_for(&W.sync_done, 1000);
}

/* ------------------------------------------------------------------ */
/* connection                                                          */
/* ------------------------------------------------------------------ */

static void disconnect(void)
{
	if (W.fd >= 0)
		close(W.fd);
	bool announced = W.announced_unavailable;
	memset(&W, 0, sizeof W);
	W.fd = -1;
	W.announced_unavailable = announced;
}

static int try_connect(const char *name)
{
	const char *rt = getenv("XDG_RUNTIME_DIR");
	struct sockaddr_un sa = {.sun_family = AF_UNIX};
	if (name[0] == '/')
		snprintf(sa.sun_path, sizeof sa.sun_path, "%s", name);
	else if (rt)
		snprintf(sa.sun_path, sizeof sa.sun_path, "%s/%s", rt, name);
	else
		return -1;
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static void bind_global(uint32_t name, uint32_t have_ver, uint32_t want_ver,
			const char *iface, uint32_t *out_id)
{
	uint32_t ver = have_ver < want_ver ? have_ver : want_ver;
	*out_id = new_id();
	msg_t m;
	m_begin(&m, W.registry, REGISTRY_BIND);
	m_u32(&m, name);
	m_str(&m, iface);
	m_u32(&m, ver);
	m_u32(&m, *out_id);
	m_send(&m, -1);
}

static bool connect_display(void)
{
	disconnect();
	const char *wd = getenv("WAYLAND_DISPLAY");
	int fd = -1;
	if (wd && *wd) {
		fd = try_connect(wd);
	} else {
		/* Started from a systemd unit that didn't import the session env. */
		for (int i = 0; i < 4 && fd < 0; i++) {
			char n[32];
			snprintf(n, sizeof n, "wayland-%d", i);
			fd = try_connect(n);
		}
	}
	if (fd < 0)
		return false;
	W.fd = fd;
	W.next_id = 2;

	W.registry = new_id();
	REQ(ID_DISPLAY, DISPLAY_GET_REGISTRY, W.registry);
	if (!roundtrip())
		goto fail;

	if (!W.gl_comp.name || !W.gl_shm.name || !W.gl_seat.name || !W.gl_lshell.name) {
		if (!W.announced_unavailable)
			LOGI("indicator disabled: compositor lacks %s", !W.gl_lshell.name ? "wlr-layer-shell" : "a required global");
		W.announced_unavailable = true;
		goto fail;
	}
	bind_global(W.gl_comp.name, W.gl_comp.ver, 4, "wl_compositor", &W.comp);
	bind_global(W.gl_shm.name, W.gl_shm.ver, 1, "wl_shm", &W.shm);
	bind_global(W.gl_lshell.name, W.gl_lshell.ver, 3, "zwlr_layer_shell_v1", &W.lshell);
	if (W.gl_vp.name && W.gl_spb.name) {
		bind_global(W.gl_vp.name, W.gl_vp.ver, 1, "wp_viewporter", &W.vporter);
		bind_global(W.gl_spb.name, W.gl_spb.ver, 1, "wp_single_pixel_buffer_manager_v1", &W.spb);
	}
	bind_global(W.gl_seat.name, W.gl_seat.ver, 5, "wl_seat", &W.seat);
	if (!roundtrip() || !roundtrip() || !W.pointer) /* caps event, then get_pointer */
		goto fail;
	LOGD("wayland: connected (layer-shell v%u%s)", W.gl_lshell.ver,
	     W.spb ? ", single-pixel-buffer" : "");
	return true;
fail:
	disconnect();
	return false;
}

/* ------------------------------------------------------------------ */
/* buffers and surfaces                                                */
/* ------------------------------------------------------------------ */

/* wl_buffer backed by an shm pool; `px` (if non-NULL) is copied in first. */
static uint32_t make_shm_buffer(int w, int h, const uint32_t *px)
{
	size_t size = (size_t)w * h * 4;
	int fd = memfd_create("wlautoscroll", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, (off_t)size) < 0) {
		if (fd >= 0)
			close(fd);
		return 0;
	}
	if (px) {
		void *map = mmap(NULL, size, PROT_WRITE, MAP_SHARED, fd, 0);
		if (map == MAP_FAILED) {
			close(fd);
			return 0;
		}
		memcpy(map, px, size);
		munmap(map, size);
	}
	uint32_t pool = new_id(), buf = new_id();
	msg_t m;
	m_begin(&m, W.shm, SHM_CREATE_POOL);
	m_u32(&m, pool);
	m_u32(&m, (uint32_t)size);
	m_send(&m, fd);
	close(fd);
	REQ(pool, POOL_CREATE_BUFFER, buf, 0, (uint32_t)w, (uint32_t)h, (uint32_t)w * 4, SHM_FORMAT_ARGB8888);
	REQ0(pool, POOL_DESTROY);
	return buf;
}

static uint32_t make_layer_surface(uint32_t *surf_out)
{
	uint32_t surf = new_id(), ls = new_id();
	REQ(W.comp, COMPOSITOR_CREATE_SURFACE, surf);
	msg_t m;
	m_begin(&m, W.lshell, LSHELL_GET_LAYER_SURFACE);
	m_u32(&m, ls);
	m_u32(&m, surf);
	m_u32(&m, 0); /* output: compositor's choice (the one with the pointer) */
	m_u32(&m, LAYER_OVERLAY);
	m_str(&m, NAMESPACE);
	m_send(&m, -1);
	REQ(ls, LS_SET_EXCLUSIVE_ZONE, (uint32_t)-1); /* ignore panels' reserved areas */
	REQ(ls, LS_SET_KEYBOARD_INTERACTIVITY, 0);
	*surf_out = surf;
	return ls;
}

static void destroy_probe(void)
{
	if (W.p_ls) REQ0(W.p_ls, LS_DESTROY);
	if (W.p_surf) REQ0(W.p_surf, SURFACE_DESTROY);
	if (W.p_buf) REQ0(W.p_buf, BUFFER_DESTROY);
	if (W.p_vp) REQ0(W.p_vp, VIEWPORT_DESTROY);
	W.p_ls = W.p_surf = W.p_buf = W.p_vp = 0;
}

void overlay_hide(void)
{
	if (!W.shown)
		return;
	if (W.i_ls) REQ0(W.i_ls, LS_DESTROY);
	if (W.i_surf) REQ0(W.i_surf, SURFACE_DESTROY);
	if (W.i_buf) REQ0(W.i_buf, BUFFER_DESTROY);
	W.i_ls = W.i_surf = W.i_buf = 0;
	W.shown = false;
}

/* Map the transparent full-output probe; fills W.ex/W.ey. */
static bool probe_pointer(void)
{
	W.p_cfg = W.have_enter = false;
	W.p_ls = make_layer_surface(&W.p_surf);
	REQ(W.p_ls, LS_SET_SIZE, 0, 0);
	REQ(W.p_ls, LS_SET_ANCHOR, ANCHOR_ALL);
	REQ0(W.p_surf, SURFACE_COMMIT);
	if (!wait_for(&W.p_cfg, 300) || !W.p_w || !W.p_h)
		return false;
	REQ(W.p_ls, LS_ACK_CONFIGURE, W.p_serial);

	if (W.spb && W.vporter) {
		/* One transparent pixel stretched over the whole output. */
		W.p_vp = new_id();
		REQ(W.vporter, VPORTER_GET_VIEWPORT, W.p_vp, W.p_surf);
		REQ(W.p_vp, VIEWPORT_SET_DESTINATION, W.p_w, W.p_h);
		W.p_buf = new_id();
		REQ(W.spb, SPB_CREATE_U32_RGBA, W.p_buf, 0, 0, 0, 0);
	} else {
		if ((uint64_t)W.p_w * W.p_h > 16u * 1024 * 1024) /* > 64 MB: give up */
			return false;
		W.p_buf = make_shm_buffer((int)W.p_w, (int)W.p_h, NULL); /* zero-filled */
		if (!W.p_buf)
			return false;
	}
	REQ(W.p_surf, SURFACE_ATTACH, W.p_buf, 0, 0);
	REQ(W.p_surf, SURFACE_DAMAGE, 0, 0, W.p_w, W.p_h);
	REQ0(W.p_surf, SURFACE_COMMIT);
	return wait_for(&W.have_enter, 150);
}

int overlay_show(const config_t *cfg)
{
	int flags = 0;
	if (W.shown)
		overlay_hide();
	if (W.fd < 0 || pump(0) < 0) { /* not connected, or the compositor restarted */
		if (!connect_display())
			return 0;
	}

	bool got = probe_pointer();
	destroy_probe();
	flags |= OV_FOCUS_DIRTY;
	if (!got) {
		LOGD("wayland: could not determine pointer position");
		if (W.err)
			disconnect();
		return flags;
	}

	int w, h;
	draw_size(cfg, &w, &h);
	uint32_t *px = calloc((size_t)w * h, 4);
	if (!px)
		return flags;
	draw_indicator(px, w, h, cfg);
	W.i_buf = make_shm_buffer(w, h, px);
	free(px);
	if (!W.i_buf)
		return flags;

	W.i_cfg = false;
	W.i_ls = make_layer_surface(&W.i_surf);
	REQ(W.i_ls, LS_SET_SIZE, (uint32_t)w, (uint32_t)h);
	REQ(W.i_ls, LS_SET_ANCHOR, ANCHOR_TOP | ANCHOR_LEFT);
	REQ(W.i_ls, LS_SET_MARGIN, (uint32_t)((int)lround(W.ey) - h / 2), 0, 0,
	    (uint32_t)((int)lround(W.ex) - w / 2));
	/* Empty input region: the indicator is click-through. */
	uint32_t region = new_id();
	REQ(W.comp, COMPOSITOR_CREATE_REGION, region);
	REQ(W.i_surf, SURFACE_SET_INPUT_REGION, region);
	REQ0(region, REGION_DESTROY);
	REQ0(W.i_surf, SURFACE_COMMIT);
	if (!wait_for(&W.i_cfg, 300)) {
		W.shown = true; /* let hide() clean the half-made surface up */
		overlay_hide();
		return flags;
	}
	REQ(W.i_ls, LS_ACK_CONFIGURE, W.i_serial);
	REQ(W.i_surf, SURFACE_ATTACH, W.i_buf, 0, 0);
	REQ(W.i_surf, SURFACE_DAMAGE, 0, 0, (uint32_t)w, (uint32_t)h);
	REQ0(W.i_surf, SURFACE_COMMIT);
	W.shown = true;
	return flags | OV_SHOWN;
}
