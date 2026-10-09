#include "src/apps/hstream-ui/HighlightItemEditor.h"
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QRegularExpression>
#include <QtCore/QSignalBlocker>
#include <QtCore/QTimer>
#include <QtGui/QGuiApplication>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QVBoxLayout>
#include <cmath>
#include <functional>
#include "src/apps/hstream-ui/ActionIcons.h"
#include "src/apps/hstream-ui/HighlightReelPipeline.h"
#include "src/apps/hstream-ui/HighlightScene.h"
#include "src/apps/hstream-ui/HighlightTracking.h"
#include "src/apps/hstream-ui/PreviewDialogWindow.h"

namespace hm::ui {
namespace {
// Retained cues/keyframes can extend outside a subsequently trimmed clip.
// Keep signed clip-relative controls so reopening a trim does not rewrite them.
QString relative_time(qint64 ms) {
  return ms < 0 ? "-" + FormatHighlightTime(-ms) : FormatHighlightTime(ms);
}
bool parse_relative(const QString& text, qint64* ms, QString* error = nullptr) {
  const QString t = text.trimmed();
  const bool negative = t.startsWith('-');
  if (!ParseHighlightTime(negative ? t.mid(1) : t, ms, error))
    return false;
  if (negative)
    *ms = -*ms;
  return true;
}
class Target : public QWidget {
 public:
  bool native{false};
  QImage card;
  double aspect{16.0 / 9};
  std::function<void(QPointF)> click;
  explicit Target(QWidget* p) : QWidget(p) {
    setMinimumSize(320, 180);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    if (QGuiApplication::platformName() == "xcb")
      setAttribute(Qt::WA_NativeWindow);
  }
  void active(bool n) {
    native = n;
    setAttribute(Qt::WA_PaintOnScreen, n);
    setAttribute(Qt::WA_NoSystemBackground, n);
    update();
  }
  QPaintEngine* paintEngine() const override {
    return native ? nullptr : QWidget::paintEngine();
  }
  QRectF rectangle() const {
    QSizeF size(width(), height());
    if (size.width() / size.height() > aspect)
      size.setWidth(size.height() * aspect);
    else
      size.setHeight(size.width() / aspect);
    return QRectF((width() - size.width()) / 2, (height() - size.height()) / 2, size.width(), size.height());
  }

 protected:
  void paintEvent(QPaintEvent*) override {
    if (native)
      return;
    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    if (!card.isNull())
      p.drawImage(rectangle(), card);
  }
  void mousePressEvent(QMouseEvent* e) override {
    if (e->button() != Qt::LeftButton || !click)
      return;
    const auto r = rectangle();
    if (r.contains(e->position()))
      click(QPointF((e->position().x() - r.x()) / r.width(), (e->position().y() - r.y()) / r.height()));
  }
};
class Editor : public QDialog {
 public:
  HighlightInterval item;
  QString game_dir, game_id, archive, route;
  qint64 offset;
  ArchiveMediaInfo media;
  HighlightReelPipeline pipeline;
  QTimer timer;
  Target* target;
  QLabel* status;
  QListWidget* cues{nullptr};
  QComboBox *kind{nullptr}, *motion{nullptr};
  QLineEdit *start{nullptr}, *end{nullptr}, *text{nullptr}, *color{nullptr};
  QDoubleSpinBox *x{nullptr}, *y{nullptr}, *dx{nullptr}, *dy{nullptr}, *size{nullptr}, *thickness{nullptr},
      *weight{nullptr}, *phase{nullptr};
  QCheckBox* blink{nullptr};
  QPlainTextEdit* keys{nullptr};
  QDoubleSpinBox* inspect{nullptr};
  QLineEdit *db{nullptr}, *pts_offset{nullptr};
  QComboBox *runs{nullptr}, *tracks{nullptr}, *mapping{nullptr};
  QCheckBox* binding{nullptr};
  QCheckBox* pick_track{nullptr};
  QVector<HighlightTrackChoice> choices;
  qint64 pending_frame_ms{-1}, held_frame_ms{-1}, choices_frame_ms{-1};
  int loaded_row{-1};
  std::function<void()> restore_preview;
  QLineEdit *heading{nullptr}, *a{nullptr}, *b{nullptr}, *date{nullptr}, *la{nullptr}, *lb{nullptr}, *bg{nullptr},
      *fg{nullptr};
  QDoubleSpinBox *duration{nullptr}, *card_size{nullptr}, *card_weight{nullptr};
  QCheckBox* matchup{nullptr};
  QLineEdit* line(QFormLayout* form, const QString& label, const QString& value, const QString& name = {}) {
    auto* e = new QLineEdit(value, this);
    e->setObjectName(name);
    form->addRow(label, e);
    return e;
  }
  QDoubleSpinBox* number(
      QFormLayout* form,
      const QString& label,
      double value,
      double minimum,
      double maximum,
      int decimals = 3) {
    auto* e = new QDoubleSpinBox(this);
    e->setRange(minimum, maximum);
    e->setDecimals(decimals);
    e->setValue(value);
    e->setSingleStep(decimals ? 0.01 : 100);
    form->addRow(label, e);
    return e;
  }
  QPushButton* button(QBoxLayout* l, const QString& label, ActionIcon icon, std::function<void()> action) {
    auto* b = new QPushButton(action_icon(icon), label, this);
    l->addWidget(b);
    connect(b, &QPushButton::clicked, this, std::move(action));
    return b;
  }
  Editor(
      const HighlightInterval& original,
      QString dir,
      QString game,
      QString path,
      QString kind,
      qint64 origin,
      ArchiveMediaInfo info,
      QWidget* parent)
      : QDialog(parent, Qt::Window),
        item(original),
        game_dir(std::move(dir)),
        game_id(std::move(game)),
        archive(std::move(path)),
        route(std::move(kind)),
        offset(origin),
        media(info) {
    setObjectName("highlightItemEditor");
    setWindowTitle(item.is_card ? "Title / intermission card" : "Clip annotations");
    configure_preview_dialog_window(this);
    resize(1250, 850);
    auto* root = new QVBoxLayout(this);
    auto* splitter = new QSplitter(this);
    root->addWidget(splitter, 1);
    auto* scroll = new QScrollArea(splitter);
    scroll->setWidgetResizable(true);
    auto* controls = new QWidget(scroll);
    auto* left = new QVBoxLayout(controls);
    scroll->setWidget(controls);
    auto* preview = new QWidget(splitter);
    auto* right = new QVBoxLayout(preview);
    auto* actions = new QHBoxLayout;
    actions->addWidget(new QLabel("Authored preview", this));
    actions->addStretch();
    actions->addWidget(new PreviewDialogWindowSizeButton(this));
    auto* focus = new PreviewFocusButton(preview);
    focus->setObjectName("highlightItemFocusButton");
    actions->addWidget(focus);
    connect(focus, &QToolButton::clicked, this, [scroll, focus] {
      const bool expanded = scroll->isVisible();
      scroll->setVisible(!expanded);
      focus->setFocused(expanded);
    });
    restore_preview = [scroll, focus] {
      scroll->show();
      focus->setFocused(false);
    };
    right->addLayout(actions);
    target = new Target(preview);
    if (media.width > 0 && media.height > 0)
      target->aspect = double(media.width) / media.height;
    right->addWidget(target, 1);
    status = new QLabel(this);
    status->setObjectName("highlightItemStatus");
    status->setWordWrap(true);
    root->addWidget(status);
    if (item.is_card)
      card_controls(left);
    else
      annotation_controls(left, right);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    buttons->setObjectName("highlightItemButtons");
    root->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, [&] {
      QString error;
      if (item.is_card ? read_card() : apply_cue(false)) {
        if (NormalizeHighlightInterval(&item, &error))
          accept();
        else
          status->setText(error);
      }
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    timer.setInterval(100);
    connect(&timer, &QTimer::timeout, this, [&] {
      const auto s = pipeline.Poll();
      if (!s.error.isEmpty()) {
        pending_frame_ms = held_frame_ms = -1;
        status->setText(s.error);
        timer.stop();
        target->active(false);
      } else if (s.finished) {
        timer.stop();
        held_frame_ms = s.cancelled ? -1 : pending_frame_ms;
        pending_frame_ms = -1;
        status->setText("Click the image to position the cue. Inspect another time to add a manual keyframe.");
      }
    });
  }
  ~Editor() override {
    pipeline.Cancel();
  }
  bool read_card() {
    auto& c = item.card;
    c.matchup = matchup->isChecked();
    c.heading = heading->text();
    c.team_a = a->text();
    c.team_b = b->text();
    c.date = date->text();
    c.logo_a = la->text();
    c.logo_b = lb->text();
    c.background = bg->text();
    c.color = fg->text();
    c.duration_ms = std::llround(duration->value() * 1000);
    c.size = card_size->value();
    c.weight = std::lround(card_weight->value());
    QString error;
    if (!NormalizeHighlightInterval(&item, &error)) {
      status->setText(error);
      return false;
    }
    return true;
  }
  void card_controls(QVBoxLayout* left) {
    const auto& c = item.card;
    auto* f = new QFormLayout;
    left->addLayout(f);
    matchup = new QCheckBox("Matchup (game date required)", this);
    matchup->setChecked(c.matchup);
    f->addRow(matchup);
    heading = line(f, "Heading / section text", c.heading, "highlightCardHeading");
    a = line(f, "Team A", c.team_a, "highlightTeamA");
    b = line(f, "Team B", c.team_b, "highlightTeamB");
    QString initial = c.date;
    if (initial.isEmpty()) {
      const auto m = QRegularExpression("(\\d{4}-\\d{2}-\\d{2})").match(game_id);
      if (m.hasMatch())
        initial = m.captured(1);
    }
    date = line(f, "Game date (YYYY-MM-DD)", initial, "highlightGameDate");
    la = line(f, "Team A logo", c.logo_a);
    lb = line(f, "Team B logo", c.logo_b);
    auto* logos = new QHBoxLayout;
    left->addLayout(logos);
    auto choose = [&](QLineEdit* dest) {
      const QString source =
          QFileDialog::getOpenFileName(this, "Team logo", game_dir, "Images (*.png *.jpg *.jpeg *.webp)");
      if (source.isEmpty())
        return;
      QString path, error;
      if (ImportHighlightLogo(source, game_dir, &path, &error))
        dest->setText(path);
      else
        status->setText(error);
    };
    button(logos, "Choose A logo", ActionIcon::Open, [=] { choose(la); });
    button(logos, "Choose B logo", ActionIcon::Open, [=] { choose(lb); });
    duration = number(f, "Duration (seconds)", c.duration_ms / 1000.0, 0.05, 600);
    bg = line(f, "Background (#RRGGBB)", c.background);
    fg = line(f, "Text color (#RRGGBB)", c.color);
    card_size = number(f, "Type size / image height", c.size, 0.02, 0.2);
    card_weight = number(f, "Type weight (100–900)", c.weight, 100, 900, 0);
    auto* row = new QHBoxLayout;
    left->addLayout(row);
    button(row, "Preview card", ActionIcon::Play, [&] {
      if (!read_card())
        return;
      QString error;
      QImage image;
      if (RasterHighlightCard(item.card, game_dir, &image, &error, QSize(media.width, media.height))) {
        target->card = image;

        target->update();
      } else
        status->setText(error);
    });
    left->addStretch();
    if (read_card()) {
      QString e;
      RasterHighlightCard(item.card, game_dir, &target->card, &e, QSize(media.width, media.height));
    }
  }
  void refresh_cues(int select) {
    cues->blockSignals(true);
    cues->clear();
    for (const auto& a : item.annotations)
      cues->addItem(QString("%1  %2 – %3%4")
                        .arg(
                            a.kind,
                            relative_time(a.start_ms - item.start_ms),
                            relative_time(a.end_ms - item.start_ms),
                            a.text.isEmpty() ? QString() : "  " + a.text));
    cues->setCurrentRow(select);
    cues->blockSignals(false);
    load_cue(select);
  }
  void load_cue(int row) {
    loaded_row = row;
    if (row < 0 || row >= item.annotations.size())
      return;
    const auto& a = item.annotations[row];
    kind->setCurrentText(a.kind);
    motion->setCurrentText(a.motion);
    text->setText(a.text);
    color->setText(a.color);
    start->setText(relative_time(a.start_ms - item.start_ms));
    end->setText(relative_time(a.end_ms - item.start_ms));
    x->setValue(a.x);
    y->setValue(a.y);
    dx->setValue(a.dx);
    dy->setValue(a.dy);
    size->setValue(a.size);
    thickness->setValue(a.thickness);
    weight->setValue(a.weight);
    phase->setValue(a.blink_ms);
    blink->setChecked(a.blink);
    QStringList rows;
    for (const auto& p : a.positions)
      rows.append(
          QString("%1, %2, %3").arg(relative_time(p.time_ms - item.start_ms)).arg(p.x, 0, 'f', 5).arg(p.y, 0, 'f', 5));
    keys->setPlainText(rows.join('\n'));
  }
  bool read_cue(HighlightAnnotation* a, bool allow_empty_motion = false) {
    qint64 s = 0, e = 0;
    QString error;
    if (!parse_relative(start->text(), &s, &error) || !parse_relative(end->text(), &e, &error)) {
      status->setText(error);
      return false;
    }
    if (e <= s || s < -item.start_ms) {
      status->setText("Cue end must follow its start, without going before source time zero");
      return false;
    }
    a->start_ms = item.start_ms + s;
    a->end_ms = item.start_ms + e;
    a->kind = kind->currentText();
    a->text = text->text();
    a->color = color->text();
    a->motion = motion->currentText();
    a->x = x->value();
    a->y = y->value();
    a->dx = dx->value();
    a->dy = dy->value();
    a->size = size->value();
    a->thickness = thickness->value();
    a->weight = std::lround(weight->value());
    a->blink = blink->isChecked();
    a->blink_ms = std::llround(phase->value());
    a->positions.clear();
    for (const auto& row : keys->toPlainText().split('\n', Qt::SkipEmptyParts)) {
      const auto fields = row.split(',');
      qint64 time;
      bool xo = false, yo = false;
      if (fields.size() != 3 || !parse_relative(fields.value(0), &time, &error)) {
        status->setText("Keyframes use clip time, x, y on each line");
        return false;
      }
      const double px = fields[1].trimmed().toDouble(&xo), py = fields[2].trimmed().toDouble(&yo);
      if (!xo || !yo || time < -item.start_ms) {
        status->setText("Invalid keyframe coordinates or clip time");
        return false;
      }
      a->positions.append({item.start_ms + time, px, py});
    }
    auto validation = *a;
    if (allow_empty_motion && validation.positions.isEmpty())
      validation.motion = "fixed";
    if (!ValidateHighlightAnnotation(validation, &error)) {
      status->setText(error);
      return false;
    }
    return true;
  }
  bool apply_cue(bool refresh = true) {
    const int row = loaded_row;
    if (row < 0 || row >= item.annotations.size())
      return true;
    auto a = item.annotations[row];
    if (!read_cue(&a))
      return false;
    item.annotations[row] = a;
    if (refresh)
      refresh_cues(row);
    return true;
  }
  void inspect_frame() {
    auto preview_item = item;
    const int row = loaded_row;
    if (row >= 0 && row < item.annotations.size()) {
      auto a = item.annotations[row];
      if (!read_cue(&a, true))
        return;
      // Inspection must work before the author clicks the first movement point.
      // Keep the incomplete cue in the controls, but omit it from this preview.
      if (a.motion != "fixed" && a.positions.isEmpty())
        preview_item.annotations.removeAt(row);
      else
        preview_item.annotations[row] = a;
    }
    if (!HighlightReelPipeline::PreviewAvailable() || QGuiApplication::platformName() != "xcb") {
      status->setText("Frame inspection requires the NVIDIA/X11 preview");
      return;
    }
    HighlightReelPipeline::Request r;
    r.archive_path = archive;
    r.asset_root = game_dir;
    r.archive_offset_ms = offset;
    r.media = media;
    r.items = {preview_item};
    r.window_id = target->winId();
    r.inspect_game_ms = item.start_ms + std::llround(inspect->value() * 1000);
    QString error;
    pending_frame_ms = held_frame_ms = -1;
    target->active(true);
    if (!pipeline.Start(r, &error)) {
      status->setText(error);
      target->active(false);
      return;
    }
    pending_frame_ms = r.inspect_game_ms;
    timer.start();
    status->setText("Decoding the selected frame…");
  }
  void annotation_controls(QVBoxLayout* left, QVBoxLayout* right) {
    auto* tip = new QLabel(
        "All cues are added by you. Coordinates are fractions of the video image. Cue times below are relative to this clip. Tracking gaps hide the cue; use manual keyframes to correct a hand/contact point.",
        this);
    tip->setWordWrap(true);
    left->addWidget(tip);
    cues = new QListWidget(this);
    cues->setObjectName("highlightAnnotations");
    cues->setMaximumHeight(130);
    left->addWidget(cues);
    auto* actions = new QHBoxLayout;
    left->addLayout(actions);
    button(actions, "Add cue", ActionIcon::Add, [&] {
      if (item.annotations.size() >= 64)
        return;
      if (!apply_cue())
        return;
      HighlightAnnotation a;
      a.start_ms = item.start_ms + std::llround(inspect->value() * 1000);
      a.end_ms = std::min(item.end_ms, a.start_ms + 2000);
      item.annotations.append(a);
      refresh_cues(item.annotations.size() - 1);
    });
    button(actions, "Apply cue", ActionIcon::Apply, [&] { apply_cue(); });
    button(actions, "Remove cue", ActionIcon::Remove, [&] {
      const int row = cues->currentRow();
      if (row >= 0) {
        item.annotations.removeAt(row);
        refresh_cues(std::min(row, int(item.annotations.size()) - 1));
      }
    });
    auto* f = new QFormLayout;
    left->addLayout(f);
    kind = new QComboBox(this);
    kind->addItems({"arrow", "text", "box"});
    f->addRow("Type", kind);
    start = line(f, "Start in clip", "00:00:00");
    end = line(f, "End in clip", "00:00:02");
    text = line(f, "Text", "WHISTLE!");
    color = line(f, "Color (#RRGGBB)", "#ffff00");
    x = number(f, "X / tracked X offset", 0.5, -2, 2);
    y = number(f, "Y / tracked Y offset", 0.35, -2, 2);
    dx = number(f, "Arrow end / box width X", 0, -2, 2);
    dy = number(f, "Arrow end / box height Y", 0.12, -2, 2);
    size = number(f, "Text size / image height", 0.055, 0.005, 0.4);
    thickness = number(f, "Stroke / image height", 0.008, 0.001, 0.1);
    weight = number(f, "Text weight (100–900)", 900, 100, 900, 0);
    blink = new QCheckBox("Blink", this);
    blink->setChecked(true);
    f->addRow(blink);
    phase = number(f, "Blink phase (milliseconds)", 250, 40, 5000, 0);
    motion = new QComboBox(this);
    motion->addItems({"fixed", "keyframes", "track"});
    f->addRow("Movement", motion);
    keys = new QPlainTextEdit(this);
    keys->setMaximumHeight(120);
    keys->setPlaceholderText("00:00:01, 0.5, 0.3\n00:00:02, 0.6, 0.35");
    f->addRow("Positions: clip time, X, Y", keys);
    db = line(f, "Telemetry database", QDir(game_dir).filePath(game_id + ".telemetry.sqlite"));
    pts_offset = line(f, "Telemetry PTS minus game time (ms)", "0");
    runs = new QComboBox(this);
    tracks = new QComboBox(this);
    mapping = new QComboBox(this);
    mapping->addItems({"program", "stitched", "program-full"});
    mapping->setCurrentText(route == "stitched" ? "stitched" : "program");
    f->addRow("Recorded run", runs);
    f->addRow("Archive geometry", mapping);
    f->addRow("Track / segment", tracks);
    binding = new QCheckBox("This recorded run produced the selected archive", this);
    f->addRow(binding);
    pick_track = new QCheckBox("Click preview to select a recorded track", this);
    f->addRow(pick_track);
    auto* tracking = new QHBoxLayout;
    left->addLayout(tracking);
    button(tracking, "Choose database", ActionIcon::Open, [&] {
      const auto path = QFileDialog::getOpenFileName(this, "Telemetry database", game_dir, "Databases (*.sqlite *.db)");
      if (!path.isEmpty())
        db->setText(path);
    });
    button(tracking, "Load runs", ActionIcon::Refresh, [&] {
      QString error;
      runs->clear();
      runs->addItems(HighlightTrackingRuns(db->text(), game_id, &error));
      status->setText(error.isEmpty() ? "Select the run that produced this archive" : error);
    });
    button(tracking, "Find tracks", ActionIcon::Inspect, [&] {
      qint64 time;
      QString error;
      bool ok;
      const qint64 delta = pts_offset->text().toLongLong(&ok);
      if (!ok || !parse_relative(start->text(), &time, &error)) {
        status->setText("Enter a cue start and integer telemetry time offset");
        return;
      }
      if (HighlightTrackingChoices(
              db->text(), runs->currentText(), mapping->currentText(), item.start_ms + time, delta, &choices, &error)) {
        tracks->clear();
        for (const auto& c : choices)
          tracks->addItem(QString("%1 · geometry %2 · seek %3 · reset %4 · source %5")
                              .arg(c.id)
                              .arg(c.geometry)
                              .arg(c.seek)
                              .arg(c.reset)
                              .arg(c.source));
        choices_frame_ms = item.start_ms + time;
        tracks->setCurrentIndex(-1);
        pick_track->setChecked(true);
        status->setText(
            held_frame_ms == choices_frame_ms
                ? "Click the player/referee at this time or select its recorded ID"
                : "Inspect clip time " + relative_time(time) + " to click a player/referee, or select its recorded ID");
      } else
        status->setText(error);
    });
    auto* attach = new QHBoxLayout;
    left->addLayout(attach);
    button(attach, "Attach selected track", ActionIcon::Apply, [&] {
      const int row = cues->currentRow(), choice = tracks->currentIndex();
      if (row < 0 || choice < 0 || choice >= choices.size() || !binding->isChecked()) {
        status->setText("Select a cue and track, and verify this archive's recorded run");
        return;
      }
      auto a = item.annotations[row];
      const QString old_motion = motion->currentText();
      motion->setCurrentText("fixed");
      const bool read = read_cue(&a);
      motion->setCurrentText(old_motion);
      if (!read)
        return;
      bool ok;
      a.telemetry_offset_ms = pts_offset->text().toLongLong(&ok);
      a.database = db->text();
      a.run_id = runs->currentText();
      a.archive_path = archive;
      a.archive_origin_ms = offset;
      a.archive_size = QFileInfo(archive).size();
      a.archive_mtime_ms = QFileInfo(archive).lastModified().toMSecsSinceEpoch();
      QString error;
      if (ok && CaptureHighlightTrack(&a, choices[choice], mapping->currentText(), &error)) {
        item.annotations[row] = a;
        pick_track->setChecked(false);
        refresh_cues(row);
        status->setText(QString("Saved %1 observations. Gaps over 100 ms hide the cue.").arg(a.positions.size()));
      } else
        status->setText(error);
    });
    auto invalidate = [this] {
      choices.clear();
      choices_frame_ms = -1;
      tracks->clear();
      binding->setChecked(false);
      pick_track->setChecked(false);
    };
    for (auto* field : {db, pts_offset, start})
      connect(field, &QLineEdit::textChanged, this, invalidate);
    connect(runs, qOverload<int>(&QComboBox::currentIndexChanged), this, invalidate);
    connect(mapping, qOverload<int>(&QComboBox::currentIndexChanged), this, invalidate);
    start->setObjectName("highlightCueStart");
    end->setObjectName("highlightCueEnd");
    text->setObjectName("highlightCueText");
    kind->setObjectName("highlightCueKind");
    motion->setObjectName("highlightCueMotion");
    keys->setObjectName("highlightCuePositions");
    left->addStretch();
    auto* inspector = new QHBoxLayout;
    right->addLayout(inspector);
    inspect = new QDoubleSpinBox(this);
    inspect->setDecimals(3);
    inspect->setRange(0, std::max(0.0, HighlightItemDuration(item) / 1000.0 - 0.001));
    inspector->addWidget(new QLabel("Clip seconds", this));
    inspector->addWidget(inspect);
    button(inspector, "Inspect / refresh frame", ActionIcon::Play, [&] { inspect_frame(); });
    button(inspector, "Stop", ActionIcon::Stop, [&] {
      pending_frame_ms = held_frame_ms = -1;
      pipeline.Cancel();
      timer.stop();
      target->active(false);
      restore_preview();
    });
    target->click = [&](QPointF p) {
      if (cues->currentRow() < 0)
        return;
      if (held_frame_ms < 0 || held_frame_ms != item.start_ms + std::llround(inspect->value() * 1000)) {
        if (pick_track->isChecked())
          tracks->setCurrentIndex(-1);
        status->setText("Inspect / refresh the selected time before clicking the image");
        return;
      }
      if (pick_track->isChecked()) {
        if (choices.isEmpty()) {
          status->setText("Find tracks before clicking a recorded player/referee");
          return;
        }
        if (held_frame_ms != choices_frame_ms) {
          tracks->setCurrentIndex(-1);
          status->setText(
              "Inspect clip time " + relative_time(choices_frame_ms - item.start_ms) +
              " before clicking a recorded track");
          return;
        }
        tracks->setCurrentIndex(-1);
        for (int i = 0; i < choices.size(); ++i)
          if (choices[i].box.contains(p)) {
            tracks->setCurrentIndex(i);
            status->setText("Track selected. Attach it to save its short trajectory.");
            return;
          }
        status->setText("No recorded track at that point");
      } else if (motion->currentText() == "keyframes") {
        const qint64 time = held_frame_ms - item.start_ms;
        QStringList rows = keys->toPlainText().split('\n', Qt::SkipEmptyParts);
        QStringList updated;
        bool inserted = false;
        const QString value =
            QString("%1, %2, %3").arg(FormatHighlightTime(time)).arg(p.x(), 0, 'f', 5).arg(p.y(), 0, 'f', 5);
        for (const auto& r : rows) {
          qint64 t;
          if (!parse_relative(r.section(',', 0, 0), &t))
            continue;
          if (!inserted && t >= time) {
            updated.append(value);
            inserted = true;
          }
          if (t != time)
            updated.append(r);
        }
        if (!inserted)
          updated.append(value);
        keys->setPlainText(updated.join('\n'));
      } else if (motion->currentText() == "track") {
        auto a = item.annotations[cues->currentRow()];
        a.blink = false;
        a.x = 0;
        a.y = 0;
        const auto base = HighlightAnchor(a, held_frame_ms);
        if (!base) {
          status->setText("No recorded anchor at this time; use manual keyframes to correct the detail");
          return;
        }
        x->setValue(p.x() - base->x());
        y->setValue(p.y() - base->y());
      } else {
        x->setValue(p.x());
        y->setValue(p.y());
      }
    };
    connect(cues, &QListWidget::currentRowChanged, this, [&](int row) {
      if (loaded_row >= 0 && loaded_row < item.annotations.size()) {
        auto a = item.annotations[loaded_row];
        if (!read_cue(&a)) {
          const QSignalBlocker block(cues);
          cues->setCurrentRow(loaded_row);
          return;
        }
        item.annotations[loaded_row] = a;
      }
      load_cue(row);
    });
    refresh_cues(item.annotations.isEmpty() ? -1 : 0);
  }
};
} // namespace
bool EditHighlightItem(
    HighlightInterval* item,
    const QString& game_dir,
    const QString& game_id,
    const QString& archive,
    const QString& route,
    qint64 offset,
    const ArchiveMediaInfo& media,
    QWidget* parent) {
  Editor editor(*item, game_dir, game_id, archive, route, offset, media, parent);
  if (editor.exec() != QDialog::Accepted)
    return false;
  *item = editor.item;
  return true;
}
} // namespace hm::ui
