# TermX

A fast, stable terminal for network and DevOps work on Windows 10 and 11:
SSH, Telnet, serial consoles and local shells in one tabbed window.

Current release: **1.0.1**. Download the MSI installer (or the portable zip) from the
[Releases](https://github.com/lekienn89/TermX/releases) page.

## Features

- **SSH** (libssh2 + OpenSSL): password, keyboard-interactive, private key, Pageant/OpenSSH agent,
  known-hosts checking with SHA-256 fingerprints and a per-session host key policy (ask, accept new,
  strict), keep-alive. Modern algorithms only by default (curve25519, Ed25519, AES-GCM/CTR, and
  chacha20-poly1305 when the libssh2 build has it); when an old switch needs SHA-1 key exchange,
  ssh-rsa or CBC, TermX asks before using them and can remember the choice for that session.
- **Telnet** with full option negotiation (NAWS, terminal type, echo, binary) and **raw TCP**.
- **Serial** consoles with **automatic detection of USB console cables**: plug a cable in and TermX
  offers to open it (or opens it automatically, or reconnects the tab that was waiting for it).
  Adapters are recognised by chip (FTDI, CH340, CP210x, PL2303, Cisco) and remembered with their settings.
- **Local shell** through ConPTY (PowerShell by default).
- **Command history like WindTerm**: commands are read from the screen when you press Enter,
  suggested as you type (→ or End to accept), and searchable in a history panel (SQLite FTS5).
  Passwords and secrets are never recorded.
- **TeraTerm / PuTTY mouse**: selecting text copies it, right-click pastes (Shift+right-click opens the menu).
- Tabs, split panes, 100k-line scrollback, find, safe multi-line paste,
  bracketed paste, session logging, session tree with folders, dark and light themes.
- **Quick Connect bar** (Ctrl+Shift+K): type `admin@10.0.0.1`, `ssh -p 2222 host`, `telnet host 23`,
  `COM3 115200` or the name of a saved session.
- **Command palette** (Ctrl+Shift+P): fuzzy search over every menu command, saved session and snippet.
- **Snippets** (Ctrl+Shift+S): saved commands in groups; run with Enter or insert without Enter.
- **Broadcast input** (Ctrl+Shift+I): type into every pane of a tab, or every tab, at once.
- **Import sessions** from PuTTY (registry or `.reg` export) and from `~/.ssh/config`.
- **Preferences** (Ctrl+,): terminal font and size with preview, cursor shape and blink,
  copy on select, PuTTY-style right-click paste, and **customizable keyboard shortcuts**.
- Regular-expression search in the scrollback; per-line timestamps in session logs.
- Saved passwords are encrypted with Windows DPAPI for the current user, and passwords are wiped
  from memory after login.
- Crash safety: I/O runs on separate threads, the UI never waits on the network or a driver,
  crash minidumps are written locally, and a safe mode starts after repeated failed launches.

## Keyboard

| Action | Keys |
| --- | --- |
| Command palette | Ctrl+Shift+P |
| Quick Connect | Ctrl+Shift+K |
| New session | Ctrl+Shift+N |
| New local shell | Ctrl+Shift+T |
| Close pane | Ctrl+Shift+W |
| Split right / down | Ctrl+Shift+D / Ctrl+Shift+E |
| Reconnect | Ctrl+Shift+R |
| Copy / Paste | Ctrl+Shift+C / Ctrl+Shift+V (or Shift+Insert) |
| Find | Ctrl+Shift+F |
| History panel | Ctrl+Shift+H |
| Snippets panel | Ctrl+Shift+S |
| Broadcast input to this tab | Ctrl+Shift+I |
| Log to file | Ctrl+Shift+L |
| Preferences | Ctrl+, |
| Accept suggestion | → or End |
| Next suggestion | Alt+↓ / Alt+↑ |

Ctrl+letter keys go to the remote side (Ctrl+C, Ctrl+Z, Ctrl+Shift+6 for Cisco break sequences)
unless you bind one to a command in Preferences › Keyboard. Help › Keyboard Shortcuts shows the
current bindings.

## Installing

`TermX-1.0.1-x64.msi` installs per machine into `C:\Program Files\TermX` with Start menu and
desktop shortcuts, and upgrades earlier versions in place. Silent install:
`msiexec /i TermX-1.0.1-x64.msi /qn` (add `INSTALLDESKTOPSHORTCUT=0` to skip the desktop icon).
The builds are not code-signed yet, so Windows SmartScreen may ask for confirmation the first time.

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

To build the installer, install the WiX 5 tool (`dotnet tool install --global wix --version 5.0.2`
and `wix extension add -g WixToolset.UI.wixext/5.0.2`), deploy the app into `dist\TermX`
with `windeployqt` as in `.github/workflows/ci.yml`, then run
`wix build installer/TermX.wxs -arch x64 -ext WixToolset.UI.wixext -d Version=1.0.1 -d RepoDir=. -d DistDir=dist\TermX -o dist\TermX-1.0.1-x64.msi`.

A note on the libssh2 version string: the vcpkg port builds the 1.11.1 release from its source
archive, whose `libssh2.h` still carries the `1.11.1_DEV` label; the code is the 1.11.1 release.

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

## Code signing policy

Free code signing provided by [SignPath.io](https://about.signpath.io/), certificate by [SignPath Foundation](https://signpath.org/).

Only binaries built from this repository by its GitHub Actions workflow on GitHub-hosted runners are signed:
`TermX.exe` and the MSI installer. Third-party libraries shipped with TermX (Qt, OpenSSL, libssh2, SQLite,
Microsoft Visual C++ runtime) are not signed by this project.

Team roles:

- Committers and reviewers: [lekienn89](https://github.com/lekienn89)
- Approvers: [lekienn89](https://github.com/lekienn89)

Every release is approved by hand before it is signed. Releases published before signing was set up
(1.0.0 and 1.0.1) are not signed.

Privacy policy: This program will not transfer any information to other networked systems unless
specifically requested by the user or the person installing or operating it.

## License

GPL-3.0-or-later. See `LICENSE` and `THIRD_PARTY_NOTICES.txt`.
