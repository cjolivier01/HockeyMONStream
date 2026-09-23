#pragma once

#include <functional>
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
  // With a handler, Save leaves this editor open until the caller durably saves
  // the candidate and calls accept(). Without one, Save accepts immediately.
  void setSaveHandler(std::function<void()> handler);
  // Block editing/close while publishing; failure restores the same edits and
  // undo history. Call with false and an error to permit a retry.
  void setSaving(bool saving, const QString& error = {});

 protected:
  void keyPressEvent(QKeyEvent* event) override;
  void changeEvent(QEvent* event) override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
