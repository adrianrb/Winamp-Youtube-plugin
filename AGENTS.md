# AGENTS.md

## What this is

A Winamp 5 **general-purpose plugin** (`gen_youtube.dll`, 32-bit) that downloads
YouTube audio with external `yt-dlp.exe`, supports full playlists, and relays
live YouTube audio through a local HTTP stream. There is no package manager, test
suite, or service layer.

## Layout

- `src/gen_youtube.cpp` — entire plugin: Winamp lifecycle, menus/hotkey, dialogs,
  URL validation, yt-dlp/ffmpeg process launching, playlist download/conversion,
  live yt-dlp→ffmpeg→HTTP relay, cache, IPC playback.
- `src/gen_youtube.rc`, `src/resource.h` — dialogs and control IDs (UTF-8 with
  `#pragma code_page(65001)`).
- `third_party/winamp/GEN.H`, `wa_ipc.h` — vendored Winamp SDK headers; do not
  edit.
- `build.bat` — MSVC; must be run from the **x86** Native Tools prompt.
- `build.sh` — MinGW cross-compile; defaults to `i686-w64-mingw32-*`.
- `CMakeLists.txt`, `cmake/BumpVersion.cmake` — alternative build; still must
  target 32-bit Windows.
- `version.txt` — single source of truth for `X.Y.Z`; successful builds bump `Z`
  unless `--no-bump` / `GEN_YOUTUBE_NO_BUMP=ON` is used.

## Commands

```sh
./build.sh
./build.sh --no-bump
# or
CXX=/path/i686-w64-mingw32-g++ RC=/path/i686-w64-mingw32-windres ./build.sh
```

```bat
build.bat
build.bat --no-bump
```

Output: `build/gen_youtube.dll`. Verify with `file build/gen_youtube.dll`; it must
say PE32 (DLL), Intel i386. An x64 DLL silently will not load in Winamp.

## Non-obvious constraints

- Winamp 5 is 32-bit. Always build x86.
- The plugin exports `winampGetGeneralPurposePlugin` and subclasses the Winamp
  main window with `SetWindowLongPtr` to receive menu commands and `WM_HOTKEY`.
- Menu commands are appended to both the File menu and the main popup:
  `ID_MENU_OPEN` (`0x7E01`), `ID_MENU_STOP_LIVE` (`0x7E03`), and `ID_HOTKEY`
  (`0x7E02`, Ctrl+Alt+Y). Menu insertion uses `IPC_GET_HMENU`; don't add a second
  ID for the same action.
- Config is written to `gen_youtube.ini` in Winamp's INI directory (from
  `IPC_GETINIDIRECTORY`), not next to the DLL.
- `yt-dlp` and `ffmpeg` are external dependencies; the plugin never bundles or
  auto-downloads them. Search order: configured INI path, plugin dir, Winamp dir,
  Winamp-adjacent common install dirs, then `PATH`.
- Cached audio lives under `%TEMP%\WinampYouTube\<FNV-1a hash of URL>`.
  Playlist caches use a `playlist.complete` marker; live logs live under
  `%TEMP%\WinampYouTube\live-<FNV-1a hash of URL>`.
- Playback uses Winamp IPC: `IPC_DELETE` + enqueue + `IPC_STARTPLAY` for play now;
  enqueue only for the playlist button. Wide IPC variants are used on
  Winamp >= 5.3, ANSI fallback below that.
- Regular URLs use `--no-playlist`. URLs containing `/playlist` use
  `--yes-playlist` and download every item (with an output template based on
  `%(playlist_index)03d - %(title)s.%(ext)s`); do not reintroduce a first-item limit.
- Live audio uses a local relay: `yt-dlp ... -o -` → `ffmpeg` MP3 on stdout → a
  minimal HTTP server on an ephemeral `127.0.0.1` port, then
  `http://127.0.0.1:<port>/live.mp3` is enqueued in Winamp. `ffmpeg.exe` is required
  for live playback and the build must link `ws2_32`/`-lws2_32`.
- Downloading happens on a worker thread; the dialog blocks closing while a job is
  running. The live worker outlives the dialog after the stream is ready; use the
  dialog button or `ID_MENU_STOP_LIVE` to stop it. UI updates occur only on the UI
  thread.
- Source and `.rc` strings are UTF-8; keep `/utf-8` (MSVC), `--codepage=65001`
  (windres) or the `#pragma code_page` when editing resources.
- `version.txt` must stay strict `X.Y.Z` (no leading zeros or `v` prefix).
  Successful builds bump `Z`; failed builds leave it untouched. Never bump it by
  hand per build — only change `X`/`Y` for a new minor/major.

## Verification

There are no automated tests. Minimum check after changes: cross-compile with
`./build.sh`, confirm `file` reports a PE32 i386 DLL, and confirm the exported
symbol with `llvm-objdump -p` (or `dumpbin /exports`); the import list should
include `WS2_32.dll` for live support. A successful build also increments the
patch in `version.txt` (use `--no-bump` when verifying without changing it).
Runtime behavior requires manual testing in Winamp on Windows.
