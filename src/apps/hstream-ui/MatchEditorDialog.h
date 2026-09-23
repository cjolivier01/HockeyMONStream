#pragma once

#include <memory>

#include <QtWidgets/QDialog>

#include "hstream/src/libs/stitching/CalibrationMatches.h"

// Edits retained, clean camera images in original camera-pixel coordinates.
// Accepting returns an in-memory set; the caller owns publication/recalibration.
class MatchEditorDialog : public QDialog {
 public:
  MatchEditorDialog(
      hm::stitching::CalibrationMatchSet matches,
      hm::stitching::CalibrationMatchSet automatic,
      QWidget* parent = nullptr);
  ~MatchEditorDialog() override;
  hm::stitching::CalibrationMatchSet editedSet() const;
  void reject() override;

 protected:
  void keyPressEvent(QKeyEvent* event) override;
  void changeEvent(QEvent* event) override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
