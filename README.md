## M0 — Core skeleton (daemon bootstrapped)

* [x] Repo structure: `src/common`, `src/windows`, `src/linux`, `src/clipdebugui`
* [x] Classes (stubs): `Logger`, `Config`, `CaptureBase` (+ Win/Linux), `ReplayBuffer`, `MarkerManager`, `Detector`, `IpcServer` + `StdinIpcServer`
* [x] `main.cpp` wiring: start detector stub, handle commands: `status`, `start`, `stop`, `marker`, `export`
* [x] CMake configured for Windows/Linux
* [x] Runs and logs without crashing

---

## M1 — Real IPC (Named Pipe / UDS) + clipdebugui

* [X] Implement `IpcServerPipe`

    * [x] Windows: Named Pipe `\\.\pipe\glintd`
    * [x] Linux: Unix Domain Socket `/run/user/$UID/glintd.sock`
* [x] JSON request/response over line-delimited frames

    * [x] `{"cmd":"status"}`
    * [x] `{"cmd":"start"} / {"cmd":"stop"}`
    * [x] `{"cmd":"marker","pre":20,"post":10}`
    * [x] `{"cmd":"export","mode":"last"}`
* [ ] `clipdebugui` (Qt Widgets): grid of buttons, log pane
* [x] Robustness: reconnects, no crashes on client drop

---

## M2 — SQLite sessions & markers

* [x] Add SQLite DB file (Win: `%LOCALAPPDATA%\glint\glintd.db`, Linux: `~/.local/share/glint/glintd.db`)
* [x] Schema

    * [x] `sessions(id, game, started_at, stopped_at, container, output_mp4)`
    * [x] `markers(id, session_id, ts_ms, pre, post)`
    * [x] `chunks(id, session_id, path, start_ms, end_ms, keyframe_ms)`
* [x] Migrate `MarkerManager` to SQLite
* [x] IPC: basic list queries (optional now)

---

## M3 — Real video/audio capture + rolling replay buffer

* [ ] Video capture

    * [ ] Windows: DXGI Desktop Duplication → **NVENC** via libavcodec/libavformat
    * [ ] Linux: PipeWire screen-cast (X11 fallback) → **NVENC/VAAPI**
* [ ] Audio capture (minimal)

    * [ ] Windows: WASAPI loopback (system) + microphone (2 tracks)
    * [ ] Linux: PipeWire default monitor + mic (2 tracks)
* [ ] Rolling buffer

    * [ ] Segment files `buffer/seg_XXXXXXXX.mkv` (~2 s each)
    * [ ] Insert `chunks` rows with timestamps & keyframes
    * [ ] Time/size retention (delete oldest)
* [ ] Verify files play in VLC/mpv

---

## M4 — Instant clip export (no re-encode)

* [ ] Given `timestamp_ms`, find covering segments (DB)
* [ ] Fast path: cut on nearest keyframes → **remux (copy)**
* [ ] (Optional later) Smart render edges only
* [ ] Use libavformat as muxer (no external ffmpeg process)
* [ ] IPC: `{"cmd":"export","timestamp_ms":..., "pre":20000,"post":10000,"out":"clips/..."}`

---

## M5 — Game auto-detect + auto postprocessing

* [ ] Auto-detect “game running”

    * [ ] Windows: foreground window + PID + exe name JSON map; fullscreen heuristic
    * [ ] Linux: X11 `_NET_ACTIVE_WINDOW` + `_NET_WM_PID/NAME`; Wayland = full-screen fallback (portal)
* [ ] Auto start/stop session based on detector
* [ ] Postprocess after stop: stitch/convert session to MP4, set `sessions.output_mp4`
* [ ] Cleanup session buffer files after export (policy)

---

## M6 — Hotkeys + “ding” that’s not recorded

* [ ] Global hotkeys via **libuiohook**
* [ ] Play “ding” on a non-captured device

    * [ ] Windows: choose capture device for loopback; play ding on another
    * [ ] Linux: PipeWire separate sink for UI
* [ ] IPC/Hotkey: `add_marker` writes to DB

---

## M7 — Per-app audio tracks

* [ ] Windows: WASAPI + `IAudioSessionManager2` (capture by session/PID: game/Discord/system)
* [ ] Linux: PipeWire: virtual sinks (game/discord/ui/system) + simple user guide
* [ ] Multiplex separate tracks in MKV; preserve on MP4 export

---

## M8 — Config & live reload

* [ ] `config.toml/json` (bitrates, GOP, buffer length, paths, devices, hotkeys)
* [ ] IPC: `get_config` / `set_config`
* [ ] Apply changed settings without restart where possible

---

## M9 — Stability, CI, packaging

* [ ] CI (GitHub Actions): Windows (MSVC), Linux (GCC/Clang)
* [ ] Cache dependencies, reproducible builds
* [ ] Packages
* 
    * [ ] Windows: zip + `install.ps1` (autostart, create folders)
    * [ ] Linux: AppImage or `.deb` + systemd user service
* [ ] Logs rotation, crash dumps (WER/coredumpctl)

---

## Defaults & technical guardrails

* [ ] Video codec defaults: H.264 NVENC/VAAPI, `b-frames=0`, `gop≈30`, `IDR≈1s`, low-latency preset
* [ ] Audio defaults: 48 kHz, per-track AAC 160–192 kbps on export (buffer can store PCM/Opus)
* [ ] Containers: buffer = **MKV**, export = **MP4**
* [ ] Unified timebase (µs) for A/V; preserve `extradata`, `time_base`, `start_time` on remux
* [ ] Multi-monitor: start with primary, list others later
* [ ] Wayland reality: need portal grant; provide optional scripts in `scripts/` for autogrant (user opt-in)
* [ ] Security by default: **no network** (only Named Pipe/UDS); add `cliphttpd` later with pairing/tokens if needed

---

## IPC (v0) — commands (for reference)

* [ ] `status` → `{ recording: bool, session_id, buffer_minutes, disk_free }`
* [ ] `start` / `stop`
* [ ] `marker {pre,post}`
* [ ] `export {timestamp_ms, pre, post, out}`
* [ ] `list_sessions {limit}`
* [ ] `list_markers {session_id}`
* [ ] `get_config` / `set_config {...}`

---

## Testing checklist

* [ ] Unit: chunk indexing by time; IPC JSON parse/format; DB inserts/selects
* [ ] Integration: 10-minute buffer at target bitrate (no leaks); export from middle/start/end; postprocess after stop
* [ ] System: Sleep/lock/RDP behavior (DXGI); resolution/monitor change; missing audio device resilience

---

## Backlog (later)

* [ ] Smart-render edges (partial re-encode)
* [ ] `cliphttpd` (HTTP/WebSocket) with pairing/tokens
* [ ] Multi-marker clip stitching
* [ ] Stream Deck integration
* [ ] ML “moment” detection (kills/goals etc.)
