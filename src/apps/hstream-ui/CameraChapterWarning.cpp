#include "src/apps/hstream-ui/CameraChapterWarning.h"

#include "hstream/src/libs/stitching/GameConfig.h"
#include "hstream/src/libs/stitching/VideoDiscovery.h"
#include "src/apps/hstream-ui/ActionIcons.h"

#include <algorithm>

#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>

QString CameraChapterWarning(const std::filesystem::path& game) {
  const auto saved = hm::stitching::load_game_config_file(game / "_hockeymon/config.yaml", true);
  const auto playlists = [](const YAML::Node& node) {
    std::map<std::string, std::vector<std::string>> result;
    if (node && node.IsMap())
      for (const auto* role : {"left", "right"}) {
        const auto list = node[role];
        if (list && list.IsSequence())
          for (const auto& file : list)
            if (file.IsScalar())
              result[role].push_back(file.as<std::string>());
      }
    return result;
  };
  const auto message = [](const absl::Status& status) {
    return status.ok() ? QString() : QString::fromStdString(std::string(status.message()));
  };
  YAML::Node config;
  if (saved.ok() && saved->has_value())
    config = **saved;
  const YAML::Node ui = config && config.IsMap() ? config["hstream_ui"] : YAML::Node();
  auto explicit_inputs = playlists(ui && ui.IsMap() ? ui["video_roles"] : YAML::Node());
  if (explicit_inputs.size() == 2)
    return message(hm::stitching::ValidateCameraChapters(explicit_inputs));
  const auto discovered = hm::stitching::get_available_videos(game.string());
  if (explicit_inputs.empty()) {
    const YAML::Node game_config = config && config.IsMap() ? config["game"] : YAML::Node();
    explicit_inputs = playlists(game_config && game_config.IsMap() ? game_config["videos"] : YAML::Node());
    // Match Configurator: camN Auto sets supersede generated game.videos,
    // whereas saved playlists remain authoritative beside root Left/Right files.
    if (discovered.ok() && !discovered->count("left") && !discovered->count("right") &&
        std::any_of(discovered->begin(), discovered->end(), [](const auto& item) {
          return item.first.compare(0, 3, "cam") == 0;
        }))
      explicit_inputs.clear();
  }
  if (!explicit_inputs.empty()) {
    if (discovered.ok())
      for (const auto* role : {"left", "right"})
        if (!explicit_inputs.count(role) && discovered->count(role))
          for (const auto& [number, file] : discovered->at(role))
            explicit_inputs[role].push_back(file);
    return message(hm::stitching::ValidateCameraChapters(explicit_inputs));
  }
  return discovered.ok() ? message(hm::stitching::ValidateDiscoveredCameraChapters(*discovered)) : QString();
}

bool ConfirmCameraChapterWarning(QWidget* parent, const QString& warning) {
  if (warning.isEmpty())
    return true;
  QMessageBox prompt(
      QMessageBox::Warning,
      "Missing camera chapter",
      warning + "\n\nA missing first or internal chapter can disrupt synchronization. Continue anyway?",
      QMessageBox::NoButton,
      parent);
  prompt.setObjectName("cameraChapterWarning");
  auto* proceed = prompt.addButton("Continue anyway", QMessageBox::AcceptRole);
  auto* cancel = prompt.addButton("Cancel", QMessageBox::RejectRole);
  proceed->setIcon(action_icon(ActionIcon::Play));
  cancel->setIcon(action_icon(ActionIcon::Cancel));
  prompt.setDefaultButton(cancel);
  prompt.setEscapeButton(cancel);
  prompt.exec();
  return prompt.clickedButton() == proceed;
}
