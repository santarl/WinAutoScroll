# WinAutoScroll

![Pixels](https://img.shields.io/badge/dynamic/json?url=https://getpantry.cloud/apiv1/pantry/780d7b02-555b-4678-98e4-d438ea0c9397/basket/WinAutoScroll&query=$.pixels&label=total%20pixels%20scrolled&color=blue)
![Distance](https://img.shields.io/badge/dynamic/json?url=https://getpantry.cloud/apiv1/pantry/780d7b02-555b-4678-98e4-d438ea0c9397/basket/WinAutoScroll&query=$.kilometres&label=total%20kilometres%20scrolled&color=success)
![Platform](https://img.shields.io/badge/platform-windows-blue)
![License](https://img.shields.io/badge/license-GPLv3-lightgrey)
![Vibecoded](https://img.shields.io/badge/vibecoded-responsibly-blueviolet)

tiny win32 util for middle-click auto-scrolling. works anywhere.

## 🚀 features

*   **tiny:** <30kb binary. ~1.5mb ram. 0% cpu idle. written in c-style c++.
*   **universal:** works in explorer, browsers, ides, everything.
*   **visuals:** smooth gdi+ overlay. dynamic cursor.
*   **configurable:** change sensitivity, dead-zones, shapes, triggers via `config.ini`.
*   **touchpad mode:** optional smooth pixel-scrolling emulation.
*   **community stats:** tracks scroll distance (opt-in).

## 📽️Screenshots

check out the [github pages page](https://santarl.github.io/WinAutoScroll) or [docs/screenshots.md](docs/screenshots.md)

## 📥 install

1.  grab **`WinAutoScroll.exe`** from [releases](../../releases).
2.  run it.
3.  **middle-click** (or hold) to scroll.

no install or admin rights needed.

## ⚙️ config

auto-generates `config.ini` on first run. edit via **tray icon > edit config**.

key settings:
*   **`trigger_mode`**: `hold` (spring-loaded) or `toggle`.
*   **`update_frequency`**: refresh rate in hz (default 60).
*   **`fun_stats`**: `1` to enable tracking, `0` to disable.

## 📊 global stats

tracks pixels locally. right-click tray -> **view stats** to see them.

click **upload stats** to push your session to the global counters above (uses a simple powershell script, no background network stuff in the exe).

## 🛠️ build

built with msvc `cl.exe`. no external deps.

```cmd
cl WinAutoScroll.cpp /MD /O2 /link /SUBSYSTEM:WINDOWS
```

## 📄 license

open source under [GPL-3 License](LICENSE).