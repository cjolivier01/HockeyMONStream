#pragma once

#include <QtCore/QScopedValueRollback>
#include <QtGui/QTextCursor>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QScrollBar>

#include <algorithm>
#include <functional>
#include <optional>
#include <utility>

// Match the main runtime log: follow new output until the reader scrolls away,
// then resume only when they return to the tail. Document edits are not scrolling.
class LogTextEdit : public QPlainTextEdit {
 public:
  explicit LogTextEdit(QWidget* parent = nullptr) : QPlainTextEdit(parent) {
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
      if (!updating_)
        follows_tail_ = value >= verticalScrollBar()->maximum() - std::max(2, verticalScrollBar()->singleStep());
    });
    connect(verticalScrollBar(), &QScrollBar::rangeChanged, this, [this] {
      if (!updating_)
        scrollToTail();
    });
  }

  void editLog(const std::function<void()>& edit, bool reset = false) {
    snapshot_.reset();
    preserveViewport(edit, reset);
  }

  void setLogText(QString text, const QString& source) {
    text.replace("\r\n", "\n");
    text.replace('\r', '\n');
    const bool reset = source != source_;
    source_ = source;
    // The document normalizes text and may trim leading blocks. Compare the
    // supplied snapshots so neither operation turns an unchanged poll into a reset.
    if (!reset && snapshot_ && text == *snapshot_)
      return;
    const qsizetype prefix = !reset && snapshot_ && text.startsWith(*snapshot_) ? snapshot_->size() : -1;
    editLog(
        [&] {
          if (prefix >= 0) {
            QTextCursor cursor(document());
            cursor.movePosition(QTextCursor::End);
            cursor.insertText(text.mid(prefix));
          } else {
            setPlainText(text);
          }
        },
        reset);
    snapshot_ = std::move(text);
  }

  void appendLogText(const QString& text) {
    editLog([&] { appendPlainText(text); });
  }

 protected:
  void showEvent(QShowEvent* event) override {
    // Showing an editor can scroll its editing cursor into view. That cursor
    // is unrelated to the reader's viewport, especially after hidden appends.
    preserveViewport([&] { QPlainTextEdit::showEvent(event); });
  }

  void resizeEvent(QResizeEvent* event) override {
    preserveViewport([&] { QPlainTextEdit::resizeEvent(event); });
  }

 private:
  void preserveViewport(const std::function<void()>& change, bool reset = false) {
    const int vertical = verticalScrollBar()->value();
    const int horizontal = horizontalScrollBar()->value();
    QScopedValueRollback<bool> guard(updating_, true);
    if (reset)
      follows_tail_ = true;
    change();
    verticalScrollBar()->setValue(follows_tail_ ? verticalScrollBar()->maximum() : vertical);
    horizontalScrollBar()->setValue(reset ? 0 : horizontal);
  }

  void scrollToTail() {
    if (!follows_tail_)
      return;
    QScopedValueRollback<bool> guard(updating_, true);
    verticalScrollBar()->setValue(verticalScrollBar()->maximum());
  }

  bool follows_tail_{true};
  bool updating_{false};
  QString source_;
  std::optional<QString> snapshot_;
};
