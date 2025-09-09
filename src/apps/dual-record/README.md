# Dual IMX477 Recorder (dual-record)

A minimal DeepStream/GStreamer setup to synchronously record two CSI IMX477 cameras to separate H.265 files. Includes:

- A direct CLI (`dual-record`) for ad‑hoc captures.
- A daemon (`dual-recordd`) that can be controlled by a small CLI (`dualctl`).
- A BLE bridge (`gopro_remote_bridge`) that lets a GoPro "The Remote" start/stop recording.
- Systemd unit files and an installer script to run everything as services.

This app targets Jetson (nvarguscamerasrc + NVENC) and defaults to the highest 30 fps mode for IMX477 (typically 3840×2160@30).

## Build

- Full build (Jetson):
  - `bazelisk build --config=jetson //src/apps/dual-record:dual-record`
  - `bazelisk build --config=jetson //src/apps/dual-record:dual-recordd //src/apps/dual-record:dualctl`
  - `bazelisk build --config=jetson //src/apps/dual-record:gopro_remote_bridge`

Prereqs: GStreamer/DeepStream present on the system (see project WORKSPACE), and for BLE: SimpleBLE headers/libs installed under `/usr/local` (headers in `/usr/local/include/simpleble`, libs in `/usr/local/lib/libsimpleble.a` and `/usr/local/lib/libsimpledbus.a`), BlueZ (`libbluetooth-dev`), and DBus.

## `dual-record` (CLI)

Starts a single pipeline with two branches sharing the same clock:

```
 nvarguscamerasrc (sensor-id 0/1)
   → capsfilter (NVMM/NV12, width/height, framerate)
   → queue → nvv4l2h265enc → h265parse → {matroskamux|qtmux} → filesink
```

Key flags:
- `--sensor0/--sensor1` select CSI camera indices (default 0/1)
- `--width/--height` request capture resolution (default 3840×2160)
- `--fps` request framerate (default 30)
- `--bitrate` kbps per stream (default 40000)
- `--sensor-mode` optional Argus sensor mode
- `--container mkv|mp4` container (default mkv)
- `--out0/--out1` explicit output files; otherwise use `--out-dir` with timestamped names
- `--out-dir DIR` place outputs in DIR with auto-names `cam<SENSORID>_YYYYmmdd_HHMMSS.<ext>`
- `--sync true|false` filesink sync to pipeline clock (default false)
- `--duration-sec N` auto‑stop after N seconds (sends EOS to finalize files)
- Exposure/gain:
  - Global: `--exposure-us`, `--gain`
  - Per‑camera: `--exposure0-us`, `--exposure1-us`, `--gain0`, `--gain1`
- `--auto-30fps` adjust 4032×3040@≥30 requests to 3840×2160@30 (IMX477 friendly)

Examples:
- 60 s capture to timestamped files, 40 Mbps per camera:
  - `bazelisk run //src/apps/dual-record:dual-record -- --out-dir /data/captures --duration-sec 60 --bitrate 40000 --auto-30fps`
- MP4 with fixed exposure/gain:
  - `bazelisk run //src/apps/dual-record:dual-record -- --container mp4 --out-dir /data/captures --duration-sec 10 --exposure0-us 15000 --exposure1-us 20000 --gain0 3.0 --gain1 4.0`

## Service mode (`dual-recordd` + `dualctl`)

- Service (daemon) with a Unix domain socket API:
  - Default socket: `/run/dual-record.sock` (override with `--sock`)
  - Debug/foreground mode: `-d` or `--debug`
- Commands (sent as a single line via socket):
  - `START key=value ...` (see keys below)
  - `STOP`
  - `STATUS` → `IDLE|READY|RUNNING`

`dualctl` is a small CLI client:
- Usage: `dualctl [-s|--sock PATH] <START|STOP|STATUS> [key=value ...]`
- Keys accepted by `START` (snake_case):
  - `sensor0`, `sensor1`, `width`, `height`, `fps`, `bitrate`, `sensor_mode`, `duration`
  - `out0`, `out1`, `out_dir`, `container`, `sync`, `auto_30fps`
  - `exposure0_us`, `exposure1_us`, `gain0`, `gain1`

Examples:
- Start, 4K@30 for 2 min, 40 Mbps, mp4:
  - `dualctl START out_dir=/data/captures duration=120 bitrate=40000 container=mp4 auto_30fps=true`
- Stop:
  - `dualctl STOP`

## GoPro BLE remote bridge (`gopro_remote_bridge`)

Bridges a GoPro "The Remote" to the service. When the subscribed characteristic notifies, the tool toggles `START`/`STOP` via the service socket.

Flags:
- `--list` enumerate services/characteristics and exit (useful to discover shutter UUIDs)
- `--remote-name SUBSTR` device name match (default `Remote`)
- `--svc UUID --char UUID` subscribe to a specific characteristic (recommended)
- `--sock PATH` path to `dual-recordd` socket

Examples:
- Discover UUIDs:
  - `bazelisk run //src/apps/dual-record:gopro_remote_bridge -- --list --remote-name "Remote"`
- Subscribe to a specific characteristic:
  - `bazelisk run //src/apps/dual-record:gopro_remote_bridge -- --remote-name Remote --svc <SERVICE-UUID> --char <CHAR-UUID>`

Note: You need SimpleBLE installed under `/usr/local`. We link against:
- `/usr/local/lib/libsimpleble.a` and `/usr/local/lib/libsimpledbus.a`
- `-lbluetooth -ldbus-1 -lpthread -ldl`

## Systemd install

Script: `scripts/install_dual_record_service.sh`
- Builds and installs:
  - `/usr/local/bin/dual-recordd`, `/usr/local/bin/dualctl`, `/usr/local/bin/gopro_remote_bridge`
- Installs and enables units:
  - `/etc/systemd/system/dual-recordd.service`
  - `/etc/systemd/system/gopro-remote-bridge.service`
- Creates `/etc/default/gopro-remote-bridge` if missing. Edit `BRIDGE_ARGS` to pass `--svc/--char` for your Remote.

Usage:
- Default socket and start services:
  - `./scripts/install_dual_record_service.sh`
- Custom socket path:
  - `./scripts/install_dual_record_service.sh --sock /run/dual-record.sock`
- Install but don’t start:
  - `./scripts/install_dual_record_service.sh --no-start`

Manage:
- `systemctl status dual-recordd.service`
- `systemctl status gopro-remote-bridge.service`
- `journalctl -u dual-recordd -f`

## Notes & tips

- IMX477 modes commonly include:
  - 4032×3040@~21 fps, 3840×2160@30 fps, 1920×1080@60 fps. For strict 30 fps at highest resolution, use 3840×2160.
- Synchronization: both branches run inside one pipeline sharing a clock with start-time NONE. This provides good software sync. For true frame-exact sync, hardware trigger sync on the sensors is still required.
- If caps negotiation fails at your requested resolution/fps, pass `--sensor-mode` or pick a supported mode from Argus logs.

---

If you want, we can bake your GoPro "The Remote" shutter UUIDs into default `BRIDGE_ARGS` once you confirm them from `--list`.

