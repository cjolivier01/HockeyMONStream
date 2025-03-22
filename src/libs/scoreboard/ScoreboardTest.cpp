#include "hstream/src/libs/scoreboard/Scoreboard.h"

#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <cstdlib>
#include <iostream>

/**
 * @brief Main function for testing the Scoreboard perspective transform.
 *
 * Expects a game ID as a command-line argument to construct an image path.
 * Loads the image, applies the perspective transform using Scoreboard, and displays the result.
 *
 * @param argc Number of command-line arguments.
 * @param argv Command-line arguments.
 * @return int Exit status.
 */
int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <game_id>" << std::endl;
    return -1;
  }
  std::string gameId = argv[1];

  // Construct the image path; assumes the HOME environment variable is set.
  std::string home = (std::getenv("HOME") != nullptr) ? std::getenv("HOME") : ".";
  std::string imagePath = home + "/Videos/" + gameId + "/s.png";

  // Load the image.
  cv::Mat image = cv::imread(imagePath);
  if (image.empty()) {
    std::cerr << "Could not open image at: " << imagePath << std::endl;
    return -1;
  }

  // Hard-coded set of scoreboard points (format: top-left, top-right, bottom-right, bottom-left).
  std::vector<cv::Point2f> selected_points = {
      cv::Point2f(864, 824), cv::Point2f(1309, 654), cv::Point2f(1352, 758), cv::Point2f(922, 923)};

  std::cout << "Selected points:" << std::endl;
  for (const auto& pt : selected_points) {
    std::cout << "(" << pt.x << ", " << pt.y << ")" << std::endl;
  }

  // Create a Scoreboard instance with desired output dimensions.
  hm::scoreboard::Scoreboard scoreboard(selected_points, 700, 300);

  // Apply the perspective transformation.
  cv::Mat warpedImage = scoreboard.forward(image);

  // Display the warped image.
  cv::imshow("Warped Image", warpedImage);
  cv::waitKey(0);

  return 0;
}
