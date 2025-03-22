#ifndef SCOREBOARD_H
#define SCOREBOARD_H

#include <cuda_runtime.h>

#include <vector>

namespace sc2 {

// Simple 2D point.
struct Point2f {
    float x;
    float y;
};

// We use CUDA’s built‐in uchar3 for a 3‑channel (RGB) pixel.
// (Include cuda_runtime.h in the .cu file so that uchar3 is defined.)

// Simple image structure: the image data is assumed to reside in device memory.
struct Image {
    int width;
    int height;
    // Pointer to device pixel data (each pixel is uchar3)
    uchar3* d_data;
};

class Scoreboard {
public:
    // Constructor:
    //   srcPts: Four source points (clockwise order: TL, TR, BR, BL)
    //   destWidth, destHeight: desired scoreboard dimensions (in pixels)
    //   autoAspect: if true, adjust dest dimensions to preserve aspect ratio.
    Scoreboard(const std::vector<Point2f>& srcPts,
               int destWidth, int destHeight,
               bool autoAspect = true);

    // forward() applies the perspective warp to an input image (in device memory)
    // and returns a new Image (allocated on the device) containing the warped result.
    Image forward(const Image& input);

    int width() const { return _destWidth; }
    int height() const { return _destHeight; }

private:
    // CPU helper functions.
    void computeBBox(const std::vector<Point2f>& pts, int bbox[4]) const;
    void orderPointsClockwise(std::vector<Point2f>& pts) const; // (optional)
    void computePerspectiveTransform(const std::vector<Point2f>& src,
                                     const std::vector<Point2f>& dst,
                                     float H[3][3]) const;

    // Member variables.
    std::vector<Point2f> _srcPts;  // Source points (adjusted to ROI coordinates)
    int _bbox[4];                 // Bounding box of source points: [x, y, x2, y2]
    int _destWidth;               // Final output (scoreboard) width.
    int _destHeight;              // Final output height.
    int _roiWidth, _roiHeight;    // Width and height of the source bounding box.
    float _H[3][3];              // Perspective transform matrix mapping ROI → destination.
    // The cropped ROI will be resized to these dimensions before warping.
    int _scaledDestW, _scaledDestH;
};

}

#endif // SCOREBOARD_H
