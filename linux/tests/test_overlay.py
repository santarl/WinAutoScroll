#!/usr/bin/env python3
"""Drive `wlautoscroll --test-overlay` against a mock Wayland compositor.

The mock implements just enough of wl_display/registry/compositor/shm/seat,
wlr-layer-shell, wp_viewporter and wp_single_pixel_buffer to run the overlay
flow, records every request, and checks the resulting protocol conversation:
probe surface, pointer position handoff, indicator geometry, click-through
input region, pixels, and teardown order.

It validates our wire framing and request sequencing. It cannot validate the
opcode numbers independently (they are written from wayland.xml in both places),
so a real compositor remains the final judge: run `wlautoscroll --test-overlay`.
"""
import mmap
import os
import socket
import struct
import subprocess
import sys
import tempfile

BIN = sys.argv[1] if len(sys.argv) > 1 else "build/wlautoscroll"
OUT_W, OUT_H = 1920, 1080
PTR_X, PTR_Y = 800.25, 400.25  # surface-local pointer position the mock reports


def fixed(v):
    return int(v * 256)


class Mock:
    def __init__(self, sock, globals_, send_enter=True):
        self.s = sock
        self.globals = globals_
        self.send_enter = send_enter
        self.buf = b""
        self.fds = []
        self.objs = {1: "wl_display"}
        self.ls = {}        # layer surface id -> dict
        self.surf = {}      # surface id -> dict
        self.regions = {}   # region id -> list of ops
        self.pools = {}
        self.buffers = {}
        self.log = []
        self.serial = 1
        self.next_name = 1

    # --- wire helpers ---
    def send(self, obj, op, payload=b""):
        self.s.sendall(struct.pack("<II", obj, ((8 + len(payload)) << 16) | op) + payload)

    @staticmethod
    def pstr(s):
        b = s.encode() + b"\0"
        return struct.pack("<I", len(b)) + b + b"\0" * ((4 - len(b) % 4) % 4)

    @staticmethod
    def rstr(data, off):
        (n,) = struct.unpack_from("<I", data, off)
        s = data[off + 4: off + 4 + n - 1].decode()
        return s, off + 4 + ((n + 3) & ~3)

    # --- main loop ---
    def run(self):
        while True:
            try:
                data, fds, _, _ = socket.recv_fds(self.s, 65536, 8)
            except OSError:
                return
            if not data:
                return
            self.buf += data
            self.fds += fds
            while len(self.buf) >= 8:
                obj, hdr = struct.unpack_from("<II", self.buf)
                size, op = hdr >> 16, hdr & 0xFFFF
                if len(self.buf) < size:
                    break
                payload, self.buf = self.buf[8:size], self.buf[size:]
                self.handle(obj, op, payload)

    def handle(self, obj, op, p):
        kind = self.objs.get(obj, "?")
        self.log.append((kind, obj, op))
        if kind == "wl_display":
            if op == 0:  # sync
                (cb,) = struct.unpack("<I", p)
                self.send(cb, 0, struct.pack("<I", 0))
            elif op == 1:  # get_registry
                (rid,) = struct.unpack("<I", p)
                self.objs[rid] = "wl_registry"
                for name, ver in self.globals:
                    n = self.next_name
                    self.next_name += 1
                    self.send(rid, 0, struct.pack("<I", n) + self.pstr(name) + struct.pack("<I", ver))
        elif kind == "wl_registry" and op == 0:  # bind
            name, off = struct.unpack_from("<I", p)[0], 4
            iface, off = self.rstr(p, off)
            ver, nid = struct.unpack_from("<II", p, off)
            self.objs[nid] = iface
            assert ver >= 1
            if iface == "wl_seat":
                self.send(nid, 0, struct.pack("<I", 3))  # capabilities: pointer|keyboard
        elif kind == "wl_compositor":
            (nid,) = struct.unpack("<I", p)
            if op == 0:
                self.objs[nid] = "wl_surface"
                self.surf[nid] = {"buf": None, "committed": 0, "region": None, "destroyed": False}
            elif op == 1:
                self.objs[nid] = "wl_region"
                self.regions[nid] = []
        elif kind == "wl_seat" and op == 0:
            (nid,) = struct.unpack("<I", p)
            self.objs[nid] = "wl_pointer"
        elif kind == "zwlr_layer_shell_v1" and op == 0:
            nid, surf, output, layer = struct.unpack_from("<IIII", p)
            ns, _ = self.rstr(p, 16)
            self.objs[nid] = "zwlr_layer_surface_v1"
            self.ls[nid] = dict(surface=surf, output=output, layer=layer, ns=ns, size=(0, 0), anchor=0,
                                excl=0, margin=None, kbd=None, configured=False, acked=None,
                                destroyed=False)
        elif kind == "zwlr_layer_surface_v1":
            L = self.ls[obj]
            if op == 0:
                L["size"] = struct.unpack("<II", p)
            elif op == 1:
                (L["anchor"],) = struct.unpack("<I", p)
            elif op == 2:
                (L["excl"],) = struct.unpack("<i", p)
            elif op == 3:
                L["margin"] = struct.unpack("<iiii", p)  # top right bottom left
            elif op == 4:
                (L["kbd"],) = struct.unpack("<I", p)
            elif op == 6:
                (L["acked"],) = struct.unpack("<I", p)
            elif op == 7:
                L["destroyed"] = True
                L["destroy_order"] = len(self.log)
        elif kind == "wl_surface":
            S = self.surf[obj]
            if op == 0:
                S["destroyed"] = True
            elif op == 1:
                S["buf"] = struct.unpack("<Iii", p)[0]
            elif op == 5:
                (S["region"],) = struct.unpack("<I", p)
            elif op == 6:
                S["committed"] += 1
                self.on_commit(obj, S)
        elif kind == "wl_shm" and op == 0:
            nid, size = struct.unpack("<Ii", p)
            self.objs[nid] = "wl_shm_pool"
            self.pools[nid] = dict(size=size, fd=self.fds.pop(0))
        elif kind == "wl_shm_pool":
            if op == 0:
                nid, off, w, h, stride, fmt = struct.unpack("<IiiiiI", p)
                self.objs[nid] = "wl_buffer"
                self.buffers[nid] = dict(kind="shm", pool=obj, off=off, w=w, h=h, stride=stride, fmt=fmt)
        elif kind == "wp_viewporter" and op == 1:
            nid, surf = struct.unpack("<II", p)
            self.objs[nid] = "wp_viewport"
            self.surf[surf]["viewport"] = nid
        elif kind == "wp_viewport" and op == 2:
            self.viewport_dest = struct.unpack("<ii", p)
        elif kind == "wp_single_pixel_buffer_manager_v1" and op == 1:
            nid, r, g, b, a = struct.unpack("<IIIII", p)
            self.objs[nid] = "wl_buffer"
            self.buffers[nid] = dict(kind="spb", rgba=(r, g, b, a))
        elif kind == "wl_buffer" and op == 0:
            self.buffers[obj]["destroyed"] = True

    def on_commit(self, sid, S):
        L = next((l for l in self.ls.values() if l["surface"] == sid), None)
        if L is None:
            return
        lid = next(i for i, l in self.ls.items() if l is L)
        if not L["configured"]:
            L["configured"] = True
            w, h = L["size"]
            if w == 0 or h == 0:
                w, h = OUT_W, OUT_H
            self.send(lid, 0, struct.pack("<III", self.serial, w, h))
            self.serial += 1
        elif S["buf"] is not None and L["size"] == (0, 0) and self.send_enter:
            # probe is mapped: the pointer enters it
            pid = next(i for i, o in self.objs.items() if o == "wl_pointer")
            self.send(pid, 0, struct.pack("<IIii", self.serial, sid, fixed(PTR_X), fixed(PTR_Y)))
            self.serial += 1
            L["got_buffer_commit"] = True
            L["probe_buf"] = S["buf"]
        elif S["buf"] is not None:
            L["got_buffer_commit"] = True
            L["final_buf"] = S["buf"]


BASE = [("wl_compositor", 5), ("wl_shm", 1), ("wl_seat", 7), ("zwlr_layer_shell_v1", 4)]
EXTRA = [("wp_viewporter", 1), ("wp_single_pixel_buffer_manager_v1", 1)]

fails = 0


def check(cond, msg):
    global fails
    if not cond:
        fails += 1
        print("FAIL:", msg)


def run_case(name, globals_, send_enter=True, expect_rc=0, extra_args=()):
    d = tempfile.mkdtemp()
    path = os.path.join(d, "wayland-mock")
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(path)
    srv.listen(1)
    srv.settimeout(10)
    env = dict(os.environ, XDG_RUNTIME_DIR=d, WAYLAND_DISPLAY="wayland-mock")
    p = subprocess.Popen([BIN, "--test-overlay=0.3", "-v", *extra_args], env=env,
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    m = None
    try:
        conn, _ = srv.accept()
        m = Mock(conn, globals_, send_enter)
        m.run()
    except socket.timeout:
        check(False, f"{name}: client never connected")
    out, err = p.communicate(timeout=10)
    check(p.returncode == expect_rc, f"{name}: exit code {p.returncode}, expected {expect_rc}\n{err.decode()}")
    return m, err.decode()


def pixels_of(m, buf_id):
    B = m.buffers[buf_id]
    P = m.pools[B["pool"]]
    mm = mmap.mmap(P["fd"], P["size"], prot=mmap.PROT_READ)
    w, h = B["w"], B["h"]
    return w, h, struct.unpack(f"<{w * h}I", mm[B["off"]: B["off"] + w * h * 4])


# ---- case 1: everything available (single-pixel-buffer + viewporter) ----
m, err = run_case("full", BASE + EXTRA)
if m:
    probes = [l for l in m.ls.values() if l["size"] == (0, 0)]
    inds = [l for l in m.ls.values() if l["size"] != (0, 0)]
    check(len(probes) == 1 and len(inds) == 1, "full: one probe and one indicator layer surface")
    pr, ind = probes[0], inds[0]
    check(pr["layer"] == 3 and pr["anchor"] == 15 and pr["excl"] == -1 and pr["kbd"] == 0 and pr["ns"] == "wlautoscroll",
          f"full: probe parameters {pr}")
    check(getattr(m, "viewport_dest", None) == (OUT_W, OUT_H), "full: probe viewport stretched to output")
    check(m.buffers[pr["probe_buf"]]["kind"] == "spb" and m.buffers[pr["probe_buf"]]["rgba"] == (0, 0, 0, 0),
          "full: probe uses a transparent single-pixel buffer")
    check(pr["destroyed"] and m.surf[pr["surface"]]["destroyed"], "full: probe destroyed")
    check(pr.get("destroy_order", 0) < ind.get("destroy_order", 1 << 30), "full: probe torn down before indicator")
    check(ind["layer"] == 3 and ind["anchor"] == 5 and ind["excl"] == -1 and ind["kbd"] == 0,
          f"full: indicator parameters {ind}")
    check(ind["size"] == (76, 76), f"full: indicator size {ind['size']}")
    # pointer (800.25, 400.25) -> rounded (800, 400); minus half the 76px canvas
    check(ind["margin"] == (362, 0, 0, 762), f"full: indicator margin {ind['margin']}")
    S = m.surf[ind["surface"]]
    check(S["region"] in m.regions and m.regions[S["region"]] == [], "full: empty (click-through) input region")
    check(ind["acked"] is not None, "full: configure acked")
    check(ind["destroyed"] and S["destroyed"], "full: indicator destroyed on hide")
    w, h, px = pixels_of(m, ind["final_buf"])
    check((w, h) == (76, 76) and m.buffers[ind["final_buf"]]["stride"] == 304 and m.buffers[ind["final_buf"]]["fmt"] == 0,
          "full: indicator buffer geometry/format")
    A = lambda x, y: px[y * w + x] >> 24
    check(A(38, 38) == 0, "full: ring centre is transparent")
    check(A(38 + 26, 38) > 100, "full: ring stroke is visible")
    check(A(0, 0) == 0, "full: corner untouched")
    if os.environ.get("WLAS_DUMP_PNG"):
        from PIL import Image
        img = Image.new("RGBA", (w, h))
        for i, v in enumerate(px):
            a = v >> 24
            # un-premultiply for viewing
            r, g, b = ((v >> 16) & 255), ((v >> 8) & 255), (v & 255)
            if a:
                r, g, b = min(255, r * 255 // a), min(255, g * 255 // a), min(255, b * 255 // a)
            img.putpixel((i % w, i // w), (r, g, b, a))
        img.save(os.environ["WLAS_DUMP_PNG"])

# ---- case 2: no viewporter/single-pixel -> full-size shm probe buffer ----
m, err = run_case("shm-probe", BASE)
if m:
    pr = next(l for l in m.ls.values() if l["size"] == (0, 0))
    B = m.buffers[pr["probe_buf"]]
    check(B["kind"] == "shm" and (B["w"], B["h"]) == (OUT_W, OUT_H), f"shm-probe: probe buffer {B}")

# ---- case 3: compositor without layer-shell -> graceful 'unavailable' ----
m, err = run_case("no-layer-shell", [g for g in BASE if g[0] != "zwlr_layer_shell_v1"], expect_rc=1)
check("lacks wlr-layer-shell" in err, "no-layer-shell: explains why the indicator is off")

# ---- case 4: pointer never enters the probe -> no indicator, probe cleaned up ----
m, err = run_case("no-enter", BASE + EXTRA, send_enter=False, expect_rc=1)
if m:
    check(all(l["destroyed"] for l in m.ls.values()), "no-enter: every layer surface destroyed")
    check(len(m.ls) == 1, "no-enter: no indicator was created")

# ---- case 5: no compositor at all -> clean failure, no crash ----
env = dict(os.environ, XDG_RUNTIME_DIR=tempfile.mkdtemp(), WAYLAND_DISPLAY="wayland-nope")
r = subprocess.run([BIN, "--test-overlay=0.1"], env=env, capture_output=True, timeout=10)
check(r.returncode == 1, "no-compositor: exits 1")

print("overlay mock test:", "FAILED" if fails else "ok")
sys.exit(1 if fails else 0)
