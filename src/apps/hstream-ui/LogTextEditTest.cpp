#include "src/apps/hstream-ui/LogTextEdit.h"

#include <QtWidgets/QApplication>

#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
void pump() {
  QApplication::processEvents();
}
void follows(LogTextEdit& log, const char* step) {
  pump();
  require(log.verticalScrollBar()->maximum() > 0, "Fixture needs scrollable output");
  if (log.verticalScrollBar()->value() != log.verticalScrollBar()->maximum())
    throw std::runtime_error(std::string("Log lost newest output: ") + step);
}
} // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  try {
    LogTextEdit log;
    log.setReadOnly(true);
    log.setMaximumBlockCount(100);
    log.resize(500, 180);
    QString text;
    for (int line = 0; line < 150; ++line)
      text += QString("Output %1\n").arg(line);
    log.setLogText(text, "first");
    log.show();
    follows(log, "initial show");
    QTextCursor selection(log.document());
    selection.setPosition(10);
    selection.setPosition(20, QTextCursor::KeepAnchor);
    log.setTextCursor(selection);
    const QString selected_text = selection.selectedText();
    require(!selected_text.isEmpty(), "Fixture needs a selection");
    log.setLogText(text, "first");
    require(log.textCursor().selectedText() == selected_text, "Unchanged bounded log erased selection");
    log.resize(400, 140);
    pump();
    log.setLogText(text, "first");
    require(log.textCursor().selectedText() == selected_text, "Unchanged poll after resize erased selection");
    log.hide();
    log.show();
    pump();
    log.setLogText(text, "first");
    require(log.textCursor().selectedText() == selected_text, "Unchanged poll after showing erased selection");
    log.verticalScrollBar()->setValue(log.verticalScrollBar()->maximum());
    for (int line = 0; line < 200; ++line)
      log.appendLogText(QString("New output %1").arg(line));
    follows(log, "cap");
    require(log.document()->blockCount() == 100, "Log history is no longer bounded");
    const int near_tail = log.verticalScrollBar()->maximum() - 1;
    log.verticalScrollBar()->setValue(near_tail);
    log.appendLogText("One line back is still reading history");
    pump();
    require(log.verticalScrollBar()->value() == near_tail, "Appending ignored a one-line manual scroll");
    log.verticalScrollBar()->setValue(20);
    log.appendLogText("While reading history");
    pump();
    require(log.verticalScrollBar()->value() == 20, "Appending moved the reader");
    log.setLogText(text, "first");
    pump();
    require(log.verticalScrollBar()->value() == 20, "Replacing a bounded snapshot moved the reader");
    log.verticalScrollBar()->setValue(log.verticalScrollBar()->maximum());
    log.appendLogText("Resumed");
    follows(log, "resume");
    log.resize(250, 100);
    follows(log, "resize");
    log.hide();
    log.appendLogText(QString("Hidden output\n").repeated(100));
    log.show();
    follows(log, "hidden-follow");
    log.verticalScrollBar()->setValue(20);
    log.hide();
    log.appendLogText("Hidden while reading");
    log.show();
    pump();
    require(log.verticalScrollBar()->value() == 20, "Hiding the log lost the reader's position");
    log.setLogText(text, "second");
    follows(log, "source");
    log.editLog([&] { log.clear(); }, true);
    log.appendLogText(text);
    follows(log, "clear");
    const QString crlf = "first\r\nsecond\r\nthird\r\n";
    log.setLogText(crlf, "crlf");
    selection = QTextCursor(log.document());
    selection.setPosition(1);
    selection.setPosition(4, QTextCursor::KeepAnchor);
    log.setTextCursor(selection);
    log.setLogText(crlf, "crlf");
    require(log.textCursor().selectedText() == "irs", "Unchanged CRLF snapshot erased selection");
    log.setLogText(crlf + "fourth\r", "crlf");
    log.setLogText(crlf + "fourth\r\nfifth", "crlf");
    require(log.toPlainText() == "first\nsecond\nthird\nfourth\nfifth", "Split CRLF append duplicated a line");
    log.editLog([&] { log.clear(); }, true);
    log.setLogText(crlf, "crlf");
    require(log.toPlainText() == "first\nsecond\nthird\n", "Clearing retained a stale snapshot cache");
    std::cout << "Log tail, manual scroll, bounded history, resize, hidden updates and reset passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
