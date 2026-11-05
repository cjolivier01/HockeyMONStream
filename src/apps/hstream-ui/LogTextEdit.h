#pragma once

#include <QtCore/QScopedValueRollback>
#include <QtGui/QTextCursor>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QScrollBar>

#include <algorithm>
#include <functional>

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
    const int vertical = verticalScrollBar()->value();
    const int horizontal = horizontalScrollBar()->value();
    QScopedValueRollback<bool> guard(updating_, true);
    if (reset)
      follows_tail_ = true;
    edit();
    verticalScrollBar()->setValue(follows_tail_ ? verticalScrollBar()->maximum() : vertical);
    horizontalScrollBar()->setValue(reset ? 0 : horizontal);
  }

  void setLogText(const QString& text, const QString& source) {
    const bool reset = source != source_;
    source_ = source;
    const QString previous = toPlainText();
    if (!reset &&
        (text == previous ||
         (maximumBlockCount() > 0 && document()->blockCount() == maximumBlockCount() && text.endsWith(previous))))
      return;
    editLog(
        [&] {
          if (!reset && text.startsWith(previous)) {
            QTextCursor cursor(document());
            cursor.movePosition(QTextCursor::End);
            cursor.insertText(text.mid(previous.size()));
          } else {
            setPlainText(text);
          }
        },
        reset);
  }

  void appendLogText(const QString& text) {
    editLog([&] { appendPlainText(text); });
  }

 protected:
  void showEvent(QShowEvent* event) override {
    // Showing an editor can scroll its editing cursor into view. That cursor
    // is unrelated to the reader's viewport, especially after hidden appends.
    editLog([&] { QPlainTextEdit::showEvent(event); });
  }

  void resizeEvent(QResizeEvent* event) override {
    editLog([&] { QPlainTextEdit::resizeEvent(event); });
  }

 private:
  void scrollToTail() {
    if (!follows_tail_)
      return;
    QScopedValueRollback<bool> guard(updating_, true);
    verticalScrollBar()->setValue(verticalScrollBar()->maximum());
  }

  bool follows_tail_{true};
  bool updating_{false};
  QString source_;
};
