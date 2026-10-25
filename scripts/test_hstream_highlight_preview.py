#!/usr/bin/env python3
"""GPU/browser authored-reel qualification; test HTTP state bypasses authentication.

The real HighlightReelPipeline, hardware encoder, signaling, and production
browser player run here. The fixture redirects only run/status/ticket lookups and
the signaling address; it does not qualify the authenticated gateway.
"""

import argparse
import json
import os
import shutil
import socket
import subprocess
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import yaml
from playwright.sync_api import sync_playwright

PREFIX = "HSTREAM_HIGHLIGHT_PREVIEW "


def plan():
    card = {
        "matchup": False,
        "heading": "Native red",
        "team_a": "",
        "team_b": "",
        "logo_a": "",
        "logo_b": "",
        "date": "",
        "duration_ms": 500,
        "background": "#ff0000",
        "color": "#ffffff",
        "size": 0.075,
        "weight": 800,
    }
    cue = {
        "kind": "box",
        "text": "",
        "start_ms": 0,
        "end_ms": 1000,
        "x": 0.1,
        "y": 0.1,
        "dx": 0.4,
        "dy": 0.4,
        "thickness": 0.02,
        "size": 0.055,
        "weight": 900,
        "color": "#ffff00",
        "blink": False,
        "blink_ms": 250,
        "motion": "fixed",
        "positions": [],
        **{key: "" for key in ("database", "run_id", "track_id", "archive_path")},
        **{
            key: 0
            for key in (
                "telemetry_offset_ms",
                "geometry_id",
                "seek_epoch",
                "reset_epoch",
                "source_id",
                "archive_origin_ms",
                "archive_size",
                "archive_mtime_ms",
            )
        },
    }
    common = {"card": card, "event_mode": False, "event_ms": 0, "start_ms": 0}
    return {
        "schema": 2,
        "base_name": "native-browser",
        "archive_path": "",
        "intervals": [
            {
                **common,
                "is_card": True,
                "label": "Card",
                "annotations": [],
                "end_ms": 500,
                "duration_ms": 500,
            },
            {
                **common,
                "is_card": False,
                "label": "Clip",
                "annotations": [cue],
                "end_ms": 1000,
                "duration_ms": 1000,
            },
        ],
    }


def events(path):
    return [
        json.loads(row[len(PREFIX) :])
        for row in path.read_text(errors="replace").splitlines()
        if row.startswith(PREFIX) and row.endswith("}")
    ]


def wait(predicate, process, log, timeout=40):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        if process.poll() is not None:
            raise AssertionError(log.read_text(errors="replace")[-6000:])
        time.sleep(0.1)
    raise AssertionError(log.read_text(errors="replace")[-6000:])


def exercise(args, *, loop, connect=True, abandon=False, background=False):
    root = args.artifacts / str(uuid.uuid4())
    root.mkdir()
    (root / "game").mkdir()
    archive = root / "archive.mp4"
    subprocess.run(
        [
            "ffmpeg",
            "-hide_banner",
            "-loglevel",
            "error",
            "-f",
            "lavfi",
            "-i",
            "color=blue:s=640x360:r=30",
            "-f",
            "lavfi",
            "-i",
            "sine=frequency=440:sample_rate=48000",
            "-t",
            "2",
            "-c:v",
            "libx264",
            "-pix_fmt",
            "yuv420p",
            "-c:a",
            "aac",
            str(archive),
        ],
        check=True,
        timeout=30,
    )
    request = {
        "schema": 1,
        "run_id": root.name,
        "operation": "highlight-live-preview",
        "root": str(root),
        "game_dir": "game",
        "payload": {"plan": plan(), "archive_path": "archive.mp4", "loop": loop},
    }
    request_path = root / "operation-request.json"
    request_path.write_text(json.dumps(request))
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    log_path = root / "native.log"
    settings = yaml.safe_load((args.native_root / "configs/baseline.yaml").read_text())
    settings["web_preview"]["authored_connect_timeout_ms"] = (
        1000 if abandon and not connect else 60000
    )
    settings["web_preview"]["authored_disconnect_timeout_ms"] = 5000
    (root / "config").mkdir()
    (root / "config/baseline.yaml").write_text(yaml.safe_dump(settings))
    env = {
        **os.environ,
        "HM_CONFIG_ROOT": str(root / "config"),
        "GST_REGISTRY": str(args.artifacts / "gst-registry.bin"),
    }
    server = None
    with log_path.open("w") as output:
        process = subprocess.Popen(
            [
                str(args.binary),
                "--request",
                str(request_path),
                f"--preview-port={port}",
            ],
            stdout=output,
            stderr=subprocess.STDOUT,
            env=env,
        )
        try:
            wait(
                lambda: "WebRTC preview signaling"
                in log_path.read_text(errors="replace"),
                process,
                log_path,
            )
            time.sleep(0.2 if abandon and not connect else 2)
            assert process.poll() is None
            assert any(event.get("waiting") for event in events(log_path))
            assert all(event.get("presented", "0") == "0" for event in events(log_path))
            if connect:

                class Handler(BaseHTTPRequestHandler):
                    def do_GET(self):
                        self.send_response(200)
                        self.send_header(
                            "Content-Type",
                            (
                                "application/json"
                                if self.path == "/state"
                                else "text/html"
                            ),
                        )
                        self.end_headers()
                        if self.path == "/state":
                            values = events(log_path)
                            capability = next(
                                v["presentation"]
                                for v in values
                                if v["kind"] == "session"
                            )
                            states = [v for v in values if v["kind"] == "state"]
                            value = {
                                "previewRoutes": capability,
                                "signalingReady": True,
                                "finished": process.poll() is not None,
                                "highlight": states[-1] if states else None,
                            }
                            self.wfile.write(json.dumps(value).encode())
                        else:
                            self.wfile.write(
                                b'<video id="video" autoplay muted playsinline style="width:100%"></video>'
                            )

                    def log_message(self, *_args):
                        return

                server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
                threading.Thread(target=server.serve_forever, daemon=True).start()
                with sync_playwright() as playwright:
                    browser = playwright.chromium.launch(
                        executable_path=args.chrome,
                        headless=not args.headed,
                        args=[
                            "--no-sandbox",
                            "--autoplay-policy=no-user-gesture-required",
                        ],
                    )
                    browser_context = browser.new_context(
                        viewport={"width": 1280, "height": 720}
                    )
                    page = browser_context.new_page()
                    failures = []
                    page.on("pageerror", lambda error: failures.append(str(error)))
                    page.goto(f"http://127.0.0.1:{server.server_port}/")
                    for name in (
                        "stream-preview-presenter.js",
                        "stream-highlight-preview.js",
                    ):
                        page.add_script_tag(
                            path=str(args.web_root / "tools/webapp/static" / name)
                        )
                    page.evaluate(
                        """({run, port}) => {
                      const Socket = window.WebSocket;
                      window.WebSocket = class extends Socket {
                        constructor(url) {
                          if (!url.includes(`/stream/runs/${run}/signal/?ticket=fixture`)) throw Error('Unexpected signaling address');
                          super(`ws://127.0.0.1:${port}/ws`);
                        }
                      };
                      window.receipts = []; window.colors = []; window.cueColors = []; window.messages = []; window.completed = false;
                      window.audioPeak = 0;
                      const audioContext = new AudioContext();
                      let audioSource, analyser, audioMedia;
                      window.sampleAudio = () => {
                        if (!analyser) return 0;
                        const sample = new Float32Array(analyser.fftSize);
                        analyser.getFloatTimeDomainData(sample);
                        return Math.max(...sample.map(Math.abs));
                      };
                      const video = document.getElementById('video');
                      const requestFrame = video.requestVideoFrameCallback.bind(video);
                      video.requestVideoFrameCallback = callback => {
                        window.lastFrameCallback = callback;
                        return requestFrame(callback);
                      };
                      const canvas = document.createElement('canvas'); canvas.width = 1; canvas.height = 1;
                      const context = canvas.getContext('2d', {willReadFrequently:true});
                      window.player = new StreamHighlightPreview({run, video,
                        api: async url => url.endsWith('/signal-ticket') ? {ticket:'fixture'} : (await fetch('/state')).json(),
                        status: (text, error) => { messages.push({text, error}); },
                        presented: value => {
                          receipts.push(value);
                          context.drawImage(video, video.videoWidth*.85, video.videoHeight*.8, 1, 1, 0, 0, 1, 1);
                          colors.push(Array.from(context.getImageData(0,0,1,1).data));
                          context.drawImage(video, video.videoWidth*.1, video.videoHeight*.25, 1, 1, 0, 0, 1, 1);
                          cueColors.push(Array.from(context.getImageData(0,0,1,1).data));
                          if (audioMedia !== video.srcObject && video.srcObject?.getAudioTracks().length) {
                            audioSource?.disconnect();
                            audioMedia = video.srcObject;
                            audioSource = audioContext.createMediaStreamSource(video.srcObject);
                            analyser = audioContext.createAnalyser(); analyser.fftSize = 256;
                            audioSource.connect(analyser);
                          }
                          if (analyser) {
                            const samples = new Float32Array(analyser.fftSize);
                            analyser.getFloatTimeDomainData(samples);
                            audioPeak = Math.max(audioPeak, ...samples.map(Math.abs));
                          }
                        },
                        finished: () => { window.completed = true; },
                      });
                    }""",
                        {"run": root.name, "port": port},
                    )
                    page.wait_for_function("receipts.length >= 5", timeout=30000)
                    if loop:
                        page.wait_for_function(
                            "colors.some(c=>c[0]>180&&c[2]<60)&&colors.some(c=>c[2]>180&&c[0]<60)",
                            timeout=15000,
                        )
                        page.wait_for_timeout(3500)
                        previous = page.evaluate("receipts.at(-1)")
                        assert page.evaluate(
                            "player.peer.getReceivers().some(r=>r.track.kind==='audio')"
                        )
                        page.screenshot(path=str(root / "playing.png"))
                        page.evaluate("player.connect()")
                        page.wait_for_function(
                            "peer=>receipts.at(-1)?.peer!==peer",
                            arg=previous["peer"],
                            timeout=30000,
                        )
                        assert (
                            max(v.get("position_ms", 0) for v in events(log_path))
                            > 3000
                        )
                        if background:
                            page.wait_for_function(
                                "sampleAudio() > 0.01", timeout=10000
                            )
                            page.evaluate("""() => {
                              video.muted = false;
                              video.cancelVideoFrameCallback(player.presenter.callback);
                              player.presenter.callback = null;
                            }""")
                            page.wait_for_timeout(250)
                            before = max(
                                int(v.get("presented", "0")) for v in events(log_path)
                            )
                            background_tab = page.context.new_page()
                            background_tab.goto("about:blank")
                            background_tab.bring_to_front()
                            desktop = None
                            if args.headed:
                                # Playwright may place pages in separate desktop
                                # windows. Minimize this test window so the real
                                # browser visibility state is unambiguously hidden.
                                desktop = browser_context.new_cdp_session(page)
                                window = desktop.send("Browser.getWindowForTarget")
                                desktop.send(
                                    "Browser.setWindowBounds",
                                    {
                                        "windowId": window["windowId"],
                                        "bounds": {"windowState": "minimized"},
                                    },
                                )
                                page.wait_for_timeout(250)
                            hidden = page.evaluate("document.hidden")
                            if args.headed and not hidden:
                                print(
                                    "NOTE: Chrome kept document.hidden=false after tab change/minimize; this run qualifies suspended frame callbacks, not real hidden-tab visibility.",
                                    flush=True,
                                )
                            page.wait_for_timeout(7000)
                            background_audio = page.evaluate("""async () => {
                              let peak = 0;
                              for (let i = 0; i < 8; ++i) {
                                peak = Math.max(peak, sampleAudio());
                                await new Promise(resolve => setTimeout(resolve, 250));
                              }
                              return peak;
                            }""")
                            assert (
                                background_audio > 0.01
                            ), "Background source audio stopped"
                            assert process.poll() is None, log_path.read_text(
                                errors="replace"
                            )[-2000:]
                            assert (
                                max(
                                    int(v.get("presented", "0"))
                                    for v in events(log_path)
                                )
                                == before
                            ), "ACKs were counted as presented frames"
                            if desktop:
                                desktop.send(
                                    "Browser.setWindowBounds",
                                    {
                                        "windowId": window["windowId"],
                                        "bounds": {"windowState": "normal"},
                                    },
                                )
                            page.bring_to_front()
                            page.evaluate(
                                "player.presenter.callback = video.requestVideoFrameCallback(lastFrameCallback)"
                            )
                            page.wait_for_function(
                                "before => receipts.length > before + 3",
                                arg=page.evaluate("receipts.length"),
                                timeout=10000,
                            )
                            background_tab.close()
                            print(
                                f"PASS background={hidden} no_frame_callbacks=7000ms native_presented={before} audio_peak={background_audio}",
                                flush=True,
                            )
                            # Keep the peer open but stop returning batch credit.
                            # Transport alone cannot retain the GPU indefinitely.
                            page.evaluate("""() => {
                              const send = player.presenter.send;
                              player.presenter.send = value => value.type === 'preview-frames-ack' ? true : send(value);
                            }""")
                            page.wait_for_function("completed", timeout=15000)
                    else:
                        page.wait_for_function("completed", timeout=30000)
                    assert page.evaluate(
                        "cueColors.some(c=>c[0]>150&&c[1]>150&&c[2]<100)"
                    ), "Authored cue pixels were not decoded"
                    assert page.evaluate(
                        "audioPeak > 0.01"
                    ), "Authored source audio was not decoded"
                    assert not failures, failures
                    (root / "browser.json").write_text(
                        json.dumps(
                            page.evaluate(
                                "({receipts, colors, cueColors, audioPeak, messages, completed})"
                            ),
                            indent=2,
                        )
                    )
                    page.evaluate("player.close()")
                    browser.close()
            if (loop or not connect) and not abandon:
                process.terminate()
            process.wait(timeout=20)
            assert process.returncode == (
                1 if abandon else 130 if loop or not connect else 0
            ), log_path.read_text(errors="replace")[-6000:]
            assert not (root / "highlights.mp4").exists()
            receipt = json.loads((root / "operation-result.json").read_text())
            if abandon:
                assert receipt["state"] == "failed" and not receipt["artifacts"]
                assert "browser" in receipt["error"]
            elif connect and not loop:
                assert receipt["state"] == "complete"
                assert {(a["path"], a["kind"]) for a in receipt["artifacts"]} == {
                    ("operation-data.json", "metadata")
                }
            else:
                assert receipt["state"] == "cancelled" and not receipt["artifacts"]
            count = max(int(v.get("presented", "0")) for v in events(log_path))
            print(
                f"PASS loop={loop} connect={connect} abandon={abandon} accepted_presentations={count} artifacts={root}",
                flush=True,
            )
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            if server:
                server.shutdown()
                server.server_close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native-root", type=Path, required=True)
    parser.add_argument("--web-root", type=Path, required=True)
    parser.add_argument("--artifacts", type=Path, required=True)
    parser.add_argument(
        "--headed",
        action="store_true",
        help="Use a real desktop to qualify hidden-tab behavior",
    )
    parser.add_argument("--liveness-only", action="store_true")
    parser.add_argument(
        "--chrome", default=shutil.which("google-chrome") or shutil.which("chromium")
    )
    args = parser.parse_args()
    if not args.chrome:
        parser.error("System Chromium is required")
    args.native_root = args.native_root.resolve()
    # A concurrent cross-build may retarget bazel-bin; pin this invocation's
    # actual executable before running multiple scenarios.
    args.binary = (
        args.native_root / "bazel-bin/src/apps/hstream-ui/hstream-web-job"
    ).resolve(strict=True)
    args.web_root = args.web_root.resolve()
    args.artifacts = args.artifacts.resolve()
    args.artifacts.mkdir(exist_ok=True, parents=True)
    if args.liveness_only:
        exercise(args, loop=True, abandon=True, background=True)
        return
    exercise(args, loop=True)
    exercise(args, loop=False)
    exercise(args, loop=True, connect=False)
    exercise(args, loop=True, connect=False, abandon=True)
    exercise(args, loop=True, abandon=True)
    exercise(args, loop=True, abandon=True, background=True)


if __name__ == "__main__":
    main()
