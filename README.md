# TermX

A fast, stable terminal for network and DevOps work on Windows 10 and 11:
SSH, Telnet, serial consoles and local shells in one tabbed window.

> Status: early development (Phase 1 of the roadmap). Expect rough edges.

## Features

- **SSH** (libssh2 + OpenSSL): password, keyboard-interactive, private key, Pageant/OpenSSH agent,
  known-hosts checking with SHA-256 fingerprints, keep-alive, optional legacy algorithms for old switches.
- **Telnet** with full option negotiation (NAWS, terminal type, echo, binary) and **raw TCP**.
- **Serial** consoles with **automatic detection of USB console cables**: plug a cable in and TermX
  offers to open it (or opens it automatically, or reconnects the tab that was waiting for it).
  Adapters are recognised by chip (FTDI, CH340, CP210x, PL2303, Cisco) and remembered with their settings.
- **Local shell** through ConPTY (PowerShell by default).
- **Command history like WindTerm**: commands are read from the screen when you press Enter,
  suggested as you type (→ or End to accept), and searchable in a history panel (SQLite FTS5).
  Passwords and secrets are never recorded.
- Tabs, split panes, 100k-line scrollback, find, copy on select, safe multi-line paste,
  bracketed paste, session logging, session tree with folders, dark and light themes.
- Saved passwords are encrypted with Windows DPAPI for the current user.
- Crash safety: I/O runs on separate threads, the UI never waits on the network or a driver,
  crash minidumps are written locally, and a safe mode starts after repeated failed launches.

## Keyboard

| Action | Keys |
| --- | --- |
| New session | Ctrl+Shift+N |
| New local shell | Ctrl+Shift+T |
| Close pane | Ctrl+Shift+W |
| Split right / down | Ctrl+Shift+D / Ctrl+Shift+E |
| Reconnect | Ctrl+Shift+R |
| Copy / Paste | Ctrl+Shift+C / Ctrl+Shift+V (or Shift+Insert) |
| Find | Ctrl+Shift+F |
| History panel | Ctrl+Shift+H |
| Accept suggestion | → or End |
| Next suggestion | Alt+↓ / Alt+↑ |

Ctrl+letter keys always go to the remote side (Ctrl+C, Ctrl+Z, Ctrl+Shift+6 for Cisco break sequences).

## Command line

```
TermX.exe [--safe-mode] [--data-dir DIR] [local | user@host | ssh://user@host:22 | telnet://host:23 | raw://host:port | serial://COM7?baud=115200]
```

Put an empty `portable.txt` next to `TermX.exe` to keep all settings in a `data` folder beside it.

## Building on Windows

Requirements: Visual Studio 2022 (C++ workload), CMake 3.25+, Ninja, Qt 6.8 LTS (msvc2022_64 with
Qt Serial Port), and [vcpkg](https://github.com/microsoft/vcpkg).

```powershell
$env:VCPKG_ROOT = "C:\src\vcpkg"
$env:QTDIR = "C:\Qt\6.8.3\msvc2022_64"
cmake --preset windows-msvc
cmake --build --preset windows-msvc
ctest --preset windows-msvc
```

vcpkg installs libssh2 (with OpenSSL) and SQLite automatically from `vcpkg.json`.

## Building on Linux (development and tests)

The core library and tests also build on Linux, which is how the parsers are fuzzed.

```sh
sudo apt install cmake g++ qt6-base-dev qt6-serialport-dev libssh2-1-dev libsqlite3-dev
cmake --preset linux-dev
cmake --build --preset linux-dev
ctest --preset linux-dev
```

Fuzzing (clang + libFuzzer): see `fuzz/CMakeLists.txt`.

## Layout

```
src/core/transport   SSH, Telnet, serial and local shell back ends (one I/O thread each)
src/core/terminal    libvterm screen model, session glue, logging
src/core/history     command capture and the SQLite history store
src/core/serial      USB serial hot-plug watcher and remembered device profiles
src/core/session     session configuration, saved sessions, DPAPI credentials
src/app              Qt Widgets user interface
third_party/libvterm vendored libvterm 0.3.3 (MIT)
tests, fuzz          unit tests (Qt Test) and libFuzzer targets
```

## License

GPL-3.0-or-later. See `LICENSE` and `THIRD_PARTY_NOTICES.txt`.
