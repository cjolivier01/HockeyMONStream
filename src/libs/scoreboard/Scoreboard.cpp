#include <opencv2/opencv.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace std;
using namespace cv;

// Helper function to compute Euclidean distance between two points.
static float pointDistance(const Point2f& pt0, const Point2f& pt1) {
  return norm(pt0 - pt1);
}

// Order 4 points in clockwise order starting from top-left.
// This uses the sum and difference of coordinates similar to the Python version.
vector<Point2f> orderPointsClockwise(const vector<Point2f>& pts) {
  if (pts.size() != 4)
    throw runtime_error("orderPointsClockwise: exactly 4 points are required.");

  vector<Point2f> ordered(4);
  vector<float> sumPts, diffPts;
  for (const auto& pt : pts) {
    sumPts.push_back(pt.x + pt.y);
    diffPts.push_back(pt.x - pt.y);
  }

  // Top-left has the smallest sum.
  int tlIdx = min_element(sumPts.begin(), sumPts.end()) - sumPts.begin();
  // Bottom-right has the largest sum.
  int brIdx = max_element(sumPts.begin(), sumPts.end()) - sumPts.begin();
  // Top-right has the smallest difference.
  int trIdx = min_element(diffPts.begin(), diffPts.end()) - diffPts.begin();
  // Bottom-left has the largest difference.
  int blIdx = max_element(diffPts.begin(), diffPts.end()) - diffPts.begin();

  ordered[0] = pts[tlIdx];
  ordered[1] = pts[trIdx];
  ordered[2] = pts[brIdx];
  ordered[3] = pts[blIdx];

  return ordered;
}

// Scoreboard class: sets up the perspective transform given four source points.
class Scoreboard {
 public:
  // Constructor parameters:
  // - srcPts: 4 points (in any order; they will be re-ordered clockwise).
  // - destWidth, destHeight: desired output dimensions.
  // - autoAspect: if true, adjusts dimensions to match the aspect ratio of srcPts.
  // - clipBox: optional; if provided, it (a Rect) is subtracted from the source points.
  Scoreboard(
      const vector<Point2f>& srcPts,
      int destWidth,
      int destHeight,
      bool autoAspect = true,
      const Rect* clipBox = nullptr)
      : dest_width(destWidth), dest_height(destHeight) {
    if (srcPts.size() != 4) {
      throw runtime_error("Scoreboard: exactly 4 source points required.");
    }

    // Order the points clockwise (top-left, top-right, bottom-right, bottom-left).
    // src_pts = orderPointsClockwise(srcPts);
    src_pts = srcPts;

    // If a clipping rectangle is provided, adjust the source points.
    if (clipBox != nullptr) {
      for (auto& pt : src_pts) {
        pt.x -= clipBox->x;
        pt.y -= clipBox->y;
      }
    }

    // Compute the bounding box (min and max coordinates) of the source points.
    float minX = src_pts[0].x, minY = src_pts[0].y;
    float maxX = src_pts[0].x, maxY = src_pts[0].y;
    for (const auto& pt : src_pts) {
      minX = min(minX, pt.x);
      minY = min(minY, pt.y);
      maxX = max(maxX, pt.x);
      maxY = max(maxY, pt.y);
    }
    bbox_src = Rect(Point2f(minX, minY), Point2f(maxX, maxY));

    // Adjust the source points relative to the bounding box.
    for (auto& pt : src_pts) {
      pt.x -= bbox_src.x;
      pt.y -= bbox_src.y;
    }

    float src_width = bbox_src.width;
    float src_height = bbox_src.height;

    // If auto_aspect is true, adjust the output dimensions based on the measured aspect ratio.
    if (autoAspect) {
      float w_top = pointDistance(src_pts[0], src_pts[1]);
      float w_bottom = pointDistance(src_pts[3], src_pts[2]); // bottom-left to bottom-right
      float h_left = pointDistance(src_pts[0], src_pts[3]);
      float h_right = pointDistance(src_pts[1], src_pts[2]);
      float w_avg = (w_top + w_bottom) / 2.0f;
      float h_avg = (h_left + h_right) / 2.0f;
      float aspect_ratio = w_avg / h_avg;
      int dest_width_new = static_cast<int>(dest_height * aspect_ratio);
      int dest_height_new = static_cast<int>(dest_width / aspect_ratio);
      // Choose the adjustment that causes the least relative change.
      if (fabs(dest_width - dest_width_new) / float(dest_width) <
          fabs(dest_height - dest_height_new) / float(dest_height)) {
        dest_width = dest_width_new;
      } else {
        dest_height = dest_height_new;
      }
    }

    // Determine scaling factors. We scale the source points if the desired output is larger.
    int totw = max(dest_width, static_cast<int>(src_width));
    int toth = max(dest_height, static_cast<int>(src_height));
    if (totw > src_width || toth > src_height) {
      float ratio_w = totw / src_width;
      float ratio_h = toth / src_height;
      dest_w = totw;
      dest_h = toth;
      for (auto& pt : src_pts) {
        pt.x *= ratio_w;
        pt.y *= ratio_h;
      }
    } else {
      dest_w = static_cast<int>(src_width);
      dest_h = static_cast<int>(src_height);
    }

    // Set up destination points in the order:
    // top-left, top-right, bottom-right, bottom-left.
    vector<Point2f> dst_pts;
    dst_pts.push_back(Point2f(0, 0)); // top-left
    dst_pts.push_back(Point2f(dest_width - 1, 0)); // top-right
    dst_pts.push_back(Point2f(dest_width - 1, dest_height - 1)); // bottom-right
    dst_pts.push_back(Point2f(0, dest_height - 1)); // bottom-left

    // Compute the perspective transform matrix.
    perspectiveMatrix = getPerspectiveTransform(src_pts, dst_pts);
  }

  // forward() applies the perspective warp to the input image.
  // It extracts the ROI defined by bbox_src, resizes it to the intermediate size,
  // applies warpPerspective, and then crops to the final destination dimensions.
  cv::Mat forward(const cv::Mat& input_image) {
    // Extract the region of interest.
    cv::Mat src_image = input_image(bbox_src).clone();
    // Resize the source image to the intermediate dimensions.
    cv::Mat resized_image;
    cv::resize(src_image, resized_image, cv::Size(dest_w, dest_h), 0, 0, INTER_NEAREST);
    // Apply the perspective transform.
    cv::Mat warped_image;
    cv::warpPerspective(resized_image, warped_image, perspectiveMatrix, cv::Size(dest_w, dest_h), INTER_LINEAR);
    // Crop the warped image to the final output size.
    cv::Rect cropRect(0, 0, dest_width, dest_height);
    if (cropRect.x + cropRect.width > warped_image.cols || cropRect.y + cropRect.height > warped_image.rows) {
      throw runtime_error("Warped image size is smaller than destination size.");
    }
    cv::Mat final_image = warped_image(cropRect).clone();
    return final_image;
  }

  int getWidth() const {
    return dest_width;
  }
  int getHeight() const {
    return dest_height;
  }

 private:
  vector<Point2f> src_pts;
  Rect bbox_src;
  int dest_width, dest_height; // Final output dimensions.
  int dest_w, dest_h; // Intermediate dimensions.
  Mat perspectiveMatrix;
};

int main(int argc, char** argv) {
  // For this example, we expect a game ID to be passed as a command-line argument.
  if (argc < 2) {
    cerr << "Usage: " << argv[0] << " <game_id>" << endl;
    return -1;
  }
  string game_id = argv[1];

  // Construct the image path; assumes HOME environment variable is set.
  string home = (getenv("HOME") != nullptr) ? getenv("HOME") : ".";
  string image_path = home + "/Videos/" + game_id + "/s.png";

  // Load the image.
  Mat image = imread(image_path);
  if (image.empty()) {
    cerr << "Could not open image at: " << image_path << endl;
    return -1;
  }

  // imshow("Stitched Image", image);

  // For this example, we use a hard-coded set of 4 scoreboard points.
  // In a full implementation these might be loaded from a configuration.
  // Format: top-left, top-right, bottom-right, bottom-left.
  vector<Point2f> selected_points = {Point2f(864, 824), Point2f(1309, 654), Point2f(1352, 758), Point2f(922, 923)};

  // Print the selected points.
  cout << "Selected points:" << endl;
  for (const auto& pt : selected_points) {
    cout << "(" << pt.x << ", " << pt.y << ")" << endl;
  }

  // Create a Scoreboard instance with desired output dimensions.
  Scoreboard scoreboard(selected_points, 700, 300);

  // Apply the warp (forward transformation).
  Mat warped_image = scoreboard.forward(image);

  // Show the warped image.
  imshow("Warped Image", warped_image);
  waitKey(0);

  return 0;
}
