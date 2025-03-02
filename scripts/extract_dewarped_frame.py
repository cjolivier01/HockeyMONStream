#!/usr/bin/env python3
import argparse
import sys

import cv2
import gi
import numpy as np

gi.require_version("Gst", "1.0")
from gi.repository import GLib, Gst


def bus_call(bus, message, loop):
    t = message.type
    if t == Gst.MessageType.EOS:
        print("End-of-stream reached.")
        loop.quit()
    elif t == Gst.MessageType.ERROR:
        err, debug = message.parse_error()
        print("Error: {} ({})".format(err, debug))
        loop.quit()
    return True


def on_new_sample(sink, output_file, loop):
    sample = sink.emit("pull-sample")
    if sample:
        buffer = sample.get_buffer()
        caps = sample.get_caps()
        structure = caps.get_structure(0)
        width = structure.get_value("width")
        height = structure.get_value("height")
        success, map_info = buffer.map(Gst.MapFlags.READ)
        if success:
            frame = np.frombuffer(map_info.data, dtype=np.uint8)
            frame = frame.reshape((height, width, 3))
            cv2.imwrite(output_file, frame)
            print("Saved frame to:", output_file)
            buffer.unmap(map_info)
        loop.quit()
    return Gst.FlowReturn.OK


def on_pad_added(decodebin, pad, preconv):
    caps = pad.get_current_caps()
    if not caps:
        return
    caps_str = caps.to_string()
    if caps_str.startswith("video/"):
        sink_pad = preconv.get_static_pad("sink")
        ret = pad.link(sink_pad)
        if ret != Gst.PadLinkReturn.OK:
            print("Failed to link decodebin pad to nvvideoconvert (preconv) sink pad.")


def main():
    parser = argparse.ArgumentParser(
        description="Decode and dewarp the first frame of a video using DeepStream's nvdewarper"
    )
    parser.add_argument("input_video", help="Path to input video file")
    parser.add_argument("dewarp_config", help="Path to dewarp config file")
    parser.add_argument("output_image", help="Path to output image file")
    args = parser.parse_args()

    Gst.init(None)
    loop = GLib.MainLoop()

    pipeline = Gst.Pipeline.new("video-dewarp-pipeline")

    source = Gst.ElementFactory.make("filesrc", "source")
    source.set_property("location", args.input_video)
    decoder = Gst.ElementFactory.make("decodebin", "decoder")

    # Pre-conversion: convert system memory buffers to NVMM buffers.
    preconv = Gst.ElementFactory.make("nvvideoconvert", "preconv")

    dewarper = Gst.ElementFactory.make("nvdewarper", "dewarper")
    dewarper.set_property("config-file", args.dewarp_config)

    # Post-conversion: convert NVMM buffers from nvdewarper to system memory (BGR)
    postconv = Gst.ElementFactory.make("nvvideoconvert", "postconv")

    sink = Gst.ElementFactory.make("appsink", "sink")
    sink_caps = Gst.Caps.from_string("video/x-raw, format=BGR")
    sink.set_property("caps", sink_caps)
    sink.set_property("emit-signals", True)
    sink.connect("new-sample", lambda s: on_new_sample(s, args.output_image, loop))

    for elem in [source, decoder, preconv, dewarper, postconv, sink]:
        if not elem:
            sys.exit("Could not create one of the elements.")
        pipeline.add(elem)

    if not source.link(decoder):
        sys.exit("Failed to link source to decoder.")

    # Link decodebin dynamically to preconv.
    decoder.connect("pad-added", on_pad_added, preconv)

    if not preconv.link(dewarper):
        sys.exit("Failed to link preconv to nvdewarper.")
    if not dewarper.link(postconv):
        sys.exit("Failed to link nvdewarper to postconv.")
    if not postconv.link(sink):
        sys.exit("Failed to link postconv to appsink.")

    bus = pipeline.get_bus()
    bus.add_signal_watch()
    bus.connect("message", bus_call, loop)

    pipeline.set_state(Gst.State.PLAYING)
    try:
        loop.run()
    except Exception as e:
        print("Exception:", e)
    finally:
        pipeline.set_state(Gst.State.NULL)


if __name__ == "__main__":
    main()
