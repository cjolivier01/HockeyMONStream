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
        follows_tail_ = value == verticalScrollBar()->maximum();
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

  using FormatLine = std::function<QString(const QString&, bool)>;
  void setLogText(QString text, const QString& source, const FormatLine& format = {}) {
    text.replace("\r\n", "\n");
    text.replace('\r', '\n');
    const bool reset = source != source_;
    source_ = source;
    // The document normalizes text and may trim leading blocks. Compare the
    // supplied snapshots so neither operation turns an unchanged poll into a reset.
    const bool dark = format && palette().color(QPalette::Base).lightness() < 128;
    const bool reformat = dark != formatted_dark_ || bool(format) != bool(last_line_format_);
    if (!format)
      last_line_format_ = {};
    formatted_dark_ = dark;
    if (!reset && !reformat && snapshot_ && text == *snapshot_)
      return;
    const qsizetype prefix = !reset && !reformat && snapshot_ && text.startsWith(*snapshot_) ? snapshot_->size() : -1;
    editLog(
        [&] {
          if (format) {
            auto renderer = prefix >= 0 ? last_line_format_ : format;
            QTextCursor cursor(document());
            qsizetype start = 0;
            if (prefix >= 0) {
              // A poll can extend an incomplete line, changing its severity or ANSI style.
              start = prefix ? text.lastIndexOf('\n', prefix - 1) + 1 : 0;
              cursor.movePosition(QTextCursor::End);
              cursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::KeepAnchor);
            } else {
              cursor.select(QTextCursor::Document);
            }
            cursor.beginEditBlock();
            cursor.removeSelectedText();
            cursor.setCharFormat(QTextCharFormat());
            if (prefix < 0 && maximumBlockCount() > 0 && text.count('\n') >= maximumBlockCount()) {
              // Skip history the document would immediately discard on a full refresh.
              start = text.lastIndexOf('\n');
              for (int block = 1; block < maximumBlockCount(); ++block)
                start = text.lastIndexOf('\n', start - 1);
              ++start;
              renderer(text.left(start), false);
            }
            for (;;) {
              const auto end = text.indexOf('\n', start);
              if (end < 0)
                last_line_format_ = renderer;
              cursor.insertHtml(renderer(text.mid(start, end < 0 ? -1 : end - start), true));
              if (end < 0)
                break;
              cursor.insertBlock();
              cursor.setCharFormat(QTextCharFormat());
              start = end + 1;
            }
            cursor.endEditBlock();
          } else if (prefix >= 0) {
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
  FormatLine last_line_format_;
  bool formatted_dark_ = false;
  std::optional<QString> snapshot_;
};
