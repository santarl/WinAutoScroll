# wlautoscroll — WinAutoScroll for Wayland

Middle-click autoscroll anywhere on a Wayland desktop: press the middle button,
move the mouse, and the page scrolls faster the further you move. It is a port
of [WinAutoScroll](../README.md) that reads the **same `config.ini`** and uses
the same scroll curve, dead zone, axis lock, toggle/hold modes and indicator.

It is a small C program (~120 KB, libc only). Idle cost is zero wakeups: it
sleeps in `epoll` and only runs a timer while you are actually scrolling.

## How it works

Wayland has no global input hooks, so wlautoscroll works below the compositor:

```
physical mouse ──grab──▶ wlautoscroll ──uinput clone──▶ compositor
                              │
                              └── wheel events ──▶ "wlautoscroll wheel" (virtual device)
```

* Mice are grabbed with `EVIOCGRAB`; each gets a uinput twin with the same
  name and ids, so per-device compositor settings still apply. Everything is
  forwarded untouched except the middle button (and pointer motion while an
  autoscroll is active).
* Scroll output goes through one virtual wheel device, in 1/120-notch
  units — the same units WinAutoScroll feeds to `SendInput`.
* The indicator uses `wlr-layer-shell` (a click-through overlay). The Wayland
  protocol is spoken directly, so there is no libwayland dependency.
* If wlautoscroll exits or crashes, the kernel drops the grab and removes the
  clones: your real mouse comes straight back.

## Install

### Arch Linux (AUR)

```sh
yay -S wlautoscroll        # releases
yay -S wlautoscroll-git    # latest main
```

### Release tarball

Download `wlautoscroll-<ver>-linux-<arch>.tar.gz` from the GitHub release and
`sudo tar -C / -xzf` it, then `sudo udevadm control --reload && sudo modprobe uinput`.

### From source

```sh
make -C linux
sudo make -C linux install PREFIX=/usr
```

Needs only a C compiler and make. `make -C linux test` runs the unit tests and a
mock-compositor test of the overlay (python3, standard library only).

## Permissions (one-time)

wlautoscroll reads `/dev/input/event*` and writes `/dev/uinput`. The package
ships a udev rule that gives `/dev/uinput` to the `input` group and loads the
`uinput` module at boot. You just need to be in that group:

```sh
sudo usermod -aG input "$USER"      # then log out and back in
```

> **Security note:** any process running as a member of `input` can read every
> keystroke on the system. That is how all evdev-level tools work (keyd,
> interception-tools, …) — only add accounts you trust.

## Run

```sh
systemctl --user enable --now wlautoscroll.service
# or, from your compositor's autostart:
wlautoscroll &
```

| Action | How |
| --- | --- |
| Reload config | `systemctl --user reload wlautoscroll`, `kill -HUP`, or just run `wlautoscroll` again |
| Pause / resume | `kill -USR1 $(pidof wlautoscroll)` |
| Show saved stats | `wlautoscroll --stats` |
| Try the indicator | `wlautoscroll --test-overlay` |
| Debug log | `wlautoscroll -v` |

## Configuration

Config is looked up in this order: `-c FILE`, `$XDG_CONFIG_HOME/wlautoscroll/config.ini`,
`~/.config/wlautoscroll/config.ini`, `/etc/wlautoscroll/config.ini`, `./config.ini`.
Start from `/usr/share/doc/wlautoscroll/config.ini` (the Windows file, unchanged).

### Differences from the Windows build

| Setting | On Wayland |
| --- | --- |
| `use_send_input_api = 0` (default) | **Pointer is frozen** while scrolling, so the wheel always hits the window you started in — the closest Wayland gets to Windows' "scroll the original window". |
| `use_send_input_api = 1` | Pointer moves freely and scrolls whatever is under it, like Windows. |
| `disable_cursor_changes` | Accepted, no effect: Wayland clients can't change the global cursor. |
| `trigger_vk_code` | Windows VK codes are translated to evdev keys (F1–F24, letters, digits, numpad, modifiers, Pause, ScrollLock, …). |
| Esc to cancel | Always works, but the Esc keypress also reaches the focused app (keyboards are only *watched* unless a keyboard trigger needs swallowing). |
| `keyboard_passthrough = 0` | Needs the keyboard grabbed; wlautoscroll then grabs keyboards too. Default `1` leaves keyboards alone. |
| Reload | Resets to defaults first, so removing a key from the file really removes it. |
| Tray icon, menu, stats upload | Not ported. Stats are kept locally (`~/.local/state/wlautoscroll/stats.ini`, same keys as Windows). |
| Touchpads | Can't be the trigger (libinput synthesises their middle click above evdev). |

`emulate_touchpad_scrolling` keeps its Windows meaning: it only flips the
vertical direction, exactly as in the original.

### Linux-only keys (the Windows exe ignores these)

```ini
# Multiply raw mouse counts. Windows measures post-acceleration screen pixels,
# evdev gives raw counts, so sensitivity may need a nudge; this is the knob.
raw_scale = 1.0
# Trigger on an evdev key code (decimal or 0x..). Overrides trigger_vk_code.
trigger_key = 70            # KEY_SCROLLLOCK
# Force pointer freezing on/off instead of deriving it from use_send_input_api.
freeze_pointer = auto       # auto | 0 | 1
```

## Compositor support

Scrolling works on any compositor. The indicator needs `wlr-layer-shell`
(sway, Hyprland, niri, river, driftwm, KDE Plasma, …); GNOME doesn't offer it,
so there the indicator is skipped. Because a client can't ask the compositor
where the pointer is, the indicator is positioned by briefly mapping a
transparent full-screen surface to learn the pointer position, then nudging the
pointer by ±1 count to hand focus back. On a HiDPI output the indicator is drawn
at logical size and scaled by the compositor.

If the indicator misbehaves on your compositor, `wlautoscroll --test-overlay -v`
prints what happened; `show_indicator = 0` turns it off and scrolling is unaffected.

## Troubleshooting

* **"cannot create virtual wheel device"** — you are not in `input` yet (log
  out/in), or the `uinput` module isn't loaded: `sudo modprobe uinput`.
* **"no permission for /dev/input/eventN"** — same group issue.
* **Middle click does nothing / mouse not detected** — run with `-v` and look
  for the `grabbed … [pointer]` line. Devices with absolute axes (tablets,
  touchpads) are deliberately left alone.
* **Autostart without systemd** — start it from your compositor's autostart;
  it finds the Wayland socket by itself if `WAYLAND_DISPLAY` is unset.
