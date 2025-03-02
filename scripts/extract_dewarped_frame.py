#!/usr/bin/env python3
import argparse
import sys

import cv2
import gi
import numpy as np

gi.require_version("Gst", "1.0")
from gi.repository import GLib, Gst


def extract_dewarped_frame(input_video, dewarp_config):
    """
    Extracts the first dewarped frame from an input video using DeepStream's nvdewarper.
    Returns the frame as a NumPy array in BGR format.
    """
    result = {}

    def bus_call(bus, message, loop):
        t = message.type
        if t == Gst.MessageType.EOS:
            loop.quit()
        elif t == Gst.MessageType.ERROR:
            err, debug = message.parse_error()
            print("Error: {} ({})".format(err, debug))
            loop.quit()
        return True

    def on_new_sample(sink, loop):
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
                result["frame"] = frame
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
                print("Failed to link decodebin pad to preconv sink pad.")

    # Initialize GStreamer and create a main loop.
    Gst.init(None)
    loop = GLib.MainLoop()

    # Build the pipeline:
    # filesrc -> decodebin -> nvvideoconvert (preconv) -> nvdewarper ->
    # nvvideoconvert (postconv) -> appsink
    pipeline = Gst.Pipeline.new("video-dewarp-pipeline")

    source = Gst.ElementFactory.make("filesrc", "source")
    if not source:
        raise RuntimeError("Could not create 'filesrc' element.")
    source.set_property("location", input_video)

    decoder = Gst.ElementFactory.make("decodebin", "decoder")
    if not decoder:
        raise RuntimeError("Could not create 'decodebin' element.")

    preconv = Gst.ElementFactory.make("nvvideoconvert", "preconv")
    if not preconv:
        raise RuntimeError("Could not create 'nvvideoconvert' (preconv) element.")

    dewarper = Gst.ElementFactory.make("nvdewarper", "dewarper")
    if not dewarper:
        raise RuntimeError("Could not create 'nvdewarper' element.")
    dewarper.set_property("config-file", dewarp_config)

    postconv = Gst.ElementFactory.make("nvvideoconvert", "postconv")
    if not postconv:
        raise RuntimeError("Could not create 'nvvideoconvert' (postconv) element.")

    appsink = Gst.ElementFactory.make("appsink", "sink")
    if not appsink:
        raise RuntimeError("Could not create 'appsink' element.")
    sink_caps = Gst.Caps.from_string("video/x-raw, format=BGR")
    appsink.set_property("caps", sink_caps)
    appsink.set_property("emit-signals", True)
    appsink.connect("new-sample", lambda s: on_new_sample(s, loop))

    # Add all elements to the pipeline.
    for elem in [source, decoder, preconv, dewarper, postconv, appsink]:
        pipeline.add(elem)

    if not source.link(decoder):
        raise RuntimeError("Failed to link 'filesrc' to 'decodebin'.")
    decoder.connect("pad-added", on_pad_added, preconv)
    if not preconv.link(dewarper):
        raise RuntimeError("Failed to link 'preconv' to 'nvdewarper'.")
    if not dewarper.link(postconv):
        raise RuntimeError("Failed to link 'nvdewarper' to 'postconv'.")
    if not postconv.link(appsink):
        raise RuntimeError("Failed to link 'postconv' to 'appsink'.")

    bus = pipeline.get_bus()
    bus.add_signal_watch()
    bus.connect("message", bus_call, loop)

    pipeline.set_state(Gst.State.PLAYING)
    try:
        loop.run()
    except Exception as e:
        pipeline.set_state(Gst.State.NULL)
        raise e
    pipeline.set_state(Gst.State.NULL)
    return result.get("frame")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Extract and save the first dewarped frame from a video using DeepStream's nvdewarper."
    )
    parser.add_argument("input_video", help="Path to the input video file")
    parser.add_argument("dewarp_config", help="Path to the dewarp configuration file")
    parser.add_argument("output_image", help="Path to save the extracted frame image")
    args = parser.parse_args()

    frame = extract_dewarped_frame(args.input_video, args.dewarp_config)
    if frame is not None:
        cv2.imwrite(args.output_image, frame)
        print("Saved extracted frame to:", args.output_image)
    else:
        print("No frame was captured.")
