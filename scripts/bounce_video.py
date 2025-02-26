"""
Utility to create bouncy camera movement in a video to simulate video which needs to be motion-corrected
"""

import argparse
import cv2
import math
import torch
import torch.nn.functional as F
from typing import List
import numpy as np


def get_offset(
    time: float,
    bounce_interval: float,
    bounce_duration: float,
    amplitude: float,
    damping: float,
    frequency: float,
) -> float:
    """
    Computes a damped oscillation offset if within the bounce duration.
    Otherwise, returns 0.
    """
    t_mod: float = time % bounce_interval
    if t_mod < bounce_duration:
        offset: float = (
            amplitude
            * math.exp(-damping * t_mod)
            * math.sin(2 * math.pi * frequency * t_mod)
        )
        return offset
    else:
        return 0.0


def process_batch(
    frames_batch: List[torch.Tensor],
    theta_list: List[torch.Tensor],
    width: int,
    height: int,
    device: torch.device,
) -> List[np.ndarray]:
    """
    Processes a batch of frames using the provided affine matrices.
    Returns a list of transformed frames in BGR order for writing.
    """
    # Convert list of frames into a tensor of shape [N, H, W, C]
    batch_tensor: torch.Tensor = torch.stack(frames_batch).to(device)
    # Rearrange to [N, C, H, W] for grid_sample.
    batch_tensor = batch_tensor.permute(0, 3, 1, 2)
    theta_batch: torch.Tensor = torch.stack(theta_list).to(device)  # shape: [N, 2, 3]

    # Create affine grid and apply the transformation.
    grid = F.affine_grid(theta_batch, batch_tensor.size(), align_corners=False)
    transformed_batch: torch.Tensor = F.grid_sample(
        batch_tensor, grid, align_corners=False, padding_mode="border"
    )
    # Convert back to [N, H, W, C] and scale to [0,255].
    transformed_batch = transformed_batch.permute(0, 2, 3, 1)
    transformed_batch = (transformed_batch * 255).clamp(0, 255).byte().cpu().numpy()

    # Convert from RGB to BGR (as OpenCV expects BGR).
    output_frames: List[np.ndarray] = []
    for transformed_frame in transformed_batch:
        transformed_frame_bgr: np.ndarray = cv2.cvtColor(
            transformed_frame, cv2.COLOR_RGB2BGR
        )
        output_frames.append(transformed_frame_bgr)
    return output_frames


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Apply a bounce effect to a video by simulating a camera bump and then stabilizing it."
    )
    parser.add_argument("input", help="Path to the input video file.")
    parser.add_argument("output", help="Path to the output video file.")
    parser.add_argument(
        "--bounce-interval",
        type=float,
        default=5.0,
        dest="bounce_interval",
        help="Seconds between bounce events (default: 5.0).",
    )
    parser.add_argument(
        "--bounce-duration",
        type=float,
        default=1.0,
        dest="bounce_duration",
        help="Duration (in seconds) of each bounce event (default: 1.0).",
    )
    parser.add_argument(
        "--amplitude",
        type=float,
        default=20.0,
        help="Maximum displacement in pixels (default: 20.0).",
    )
    parser.add_argument(
        "--damping",
        type=float,
        default=5.0,
        help="Damping factor for the bounce effect (default: 5.0).",
    )
    parser.add_argument(
        "--frequency",
        type=float,
        default=5.0,
        help="Frequency (in Hz) of the bounce oscillation (default: 5.0).",
    )
    parser.add_argument(
        "--start-frame",
        type=int,
        default=0,
        dest="start_frame",
        help="Frame number to start processing from (default: 0).",
    )
    parser.add_argument(
        "--max-frames",
        type=int,
        default=-1,
        dest="max_frames",
        help="Maximum number of frames to process (-1 for all frames, default: -1).",
    )
    parser.add_argument(
        "--chunk-size",
        type=int,
        default=12,
        dest="chunk_size",
        help="Number of frames to process in a single batch (default: 64).",
    )
    args = parser.parse_args()

    # Open the input video.
    cap: cv2.VideoCapture = cv2.VideoCapture(args.input)
    if not cap.isOpened():
        raise IOError("Cannot open video file {}".format(args.input))

    fps: float = cap.get(cv2.CAP_PROP_FPS)
    width: int = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    height: int = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))

    # Set starting frame.
    if args.start_frame > 0:
        cap.set(cv2.CAP_PROP_POS_FRAMES, args.start_frame)

    # Set up the output video writer.
    fourcc = cv2.VideoWriter_fourcc(*"mp4v")
    out: cv2.VideoWriter = cv2.VideoWriter(args.output, fourcc, fps, (width, height))

    # Prepare for batch processing.
    frames_batch: List[torch.Tensor] = []
    theta_list: List[torch.Tensor] = []
    frame_index: int = args.start_frame
    processed_frames: int = 0
    device: torch.device = torch.device("cuda" if torch.cuda.is_available() else "cpu")

    while True:
        ret, frame = cap.read()
        if not ret:
            # Process any remaining frames in the final batch.
            if frames_batch:
                output_frames = process_batch(
                    frames_batch, theta_list, width, height, device
                )
                for out_frame in output_frames:
                    out.write(out_frame)
            break

        # Check if we've reached the max frames (if specified).
        if args.max_frames > 0 and processed_frames >= args.max_frames:
            break

        # OpenCV reads frames in BGR; convert to RGB.
        frame_rgb: np.ndarray = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
        # Convert frame to a float tensor in range [0, 1] with shape [H, W, C].
        frame_tensor: torch.Tensor = torch.from_numpy(frame_rgb).float() / 255.0
        frames_batch.append(frame_tensor)

        # Calculate the timestamp based on the absolute frame index.
        timestamp: float = frame_index / fps
        # Compute the bounce offset (applied equally to x and y).
        offset: float = get_offset(
            timestamp,
            args.bounce_interval,
            args.bounce_duration,
            args.amplitude,
            args.damping,
            args.frequency,
        )
        dx: float = offset
        dy: float = offset

        # Convert pixel offsets to normalized coordinates (range: [-1, 1]).
        tx: float = dx * 2.0 / width
        ty: float = dy * 2.0 / height

        # Build the affine transformation matrix for translation.
        theta: torch.Tensor = torch.tensor(
            [[1, 0, tx], [0, 1, ty]], dtype=torch.float32
        )
        theta_list.append(theta)

        frame_index += 1
        processed_frames += 1

        # Process the batch if it reaches the specified chunk size.
        if len(frames_batch) == args.chunk_size:
            output_frames = process_batch(
                frames_batch, theta_list, width, height, device
            )
            for out_frame in output_frames:
                out.write(out_frame)
            frames_batch = []
            theta_list = []

    # Release resources.
    cap.release()
    out.release()


if __name__ == "__main__":
    main()
