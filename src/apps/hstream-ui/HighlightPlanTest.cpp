#include "src/apps/hstream-ui/HighlightPlan.h"

#include <iostream>

#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QTemporaryDir>

namespace {

bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  return condition;
}

bool write_text(const QString& path, const QByteArray& contents) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(contents) == contents.size();
}

} // namespace

int main() {
  using hm::ui::FormatHighlightTime;
  using hm::ui::HighlightInterval;
  using hm::ui::HighlightPlan;
  using hm::ui::LoadHighlightPlan;
  using hm::ui::NormalizeHighlightInterval;
  using hm::ui::ParseHighlightTime;
  using hm::ui::SaveHighlightPlan;

  bool ok = true;
  QString error;
  {
    const bool had_root = qEnvironmentVariableIsSet("HM_CONFIG_ROOT");
    const QByteArray previous_root = qgetenv("HM_CONFIG_ROOT");
    QTemporaryDir defaults;
    qputenv("HM_CONFIG_ROOT", defaults.path().toUtf8());
    const QString baseline = defaults.filePath("baseline.yaml");
    try {
      ok &= expect(
          write_text(baseline, "hstream_ui:\n  highlights:\n    base_name: season-reel\n"),
          "custom Highlights baseline written");
      HighlightPlan initial;
      ok &= expect(initial.base_name == "season-reel", "new plans must resolve their name from the baseline");
      initial.base_name = "Saved reel";
      const QString saved_path = defaults.filePath("highlights.json");
      ok &= expect(SaveHighlightPlan(saved_path, initial, &error), "explicit plan name saved");
      HighlightPlan reopened;
      ok &= expect(
          LoadHighlightPlan(saved_path, &reopened, &error) && reopened.base_name == "Saved reel",
          "saved plan names must override the baseline");
      for (const auto& invalid : {QByteArray("hstream_ui: {}\n"),
                                 QByteArray("hstream_ui:\n  highlights:\n    base_name: []\n"),
                                 QByteArray("hstream_ui:\n  highlights:\n    base_name: '../outside'\n")}) {
        ok &= expect(write_text(baseline, invalid), "invalid Highlights baseline written");
        bool rejected = false;
        try {
          HighlightPlan invalid_plan;
        } catch (const std::exception&) {
          rejected = true;
        }
        ok &= expect(rejected, "missing or invalid baseline names must fail without a literal fallback");
      }
    } catch (const std::exception& failure) {
      std::cerr << failure.what() << '\n';
      ok = false;
    }
    if (had_root)
      qputenv("HM_CONFIG_ROOT", previous_root);
    else
      qunsetenv("HM_CONFIG_ROOT");
  }
  qint64 milliseconds = -1;
  ok &= expect(
      ParseHighlightTime("123:04:05.006", &milliseconds, &error) && milliseconds == 443045006,
      "unbounded hours and milliseconds must parse exactly");
  ok &= expect(FormatHighlightTime(milliseconds) == "123:04:05.006", "time formatting must preserve milliseconds");
  ok &= expect(
      ParseHighlightTime("00:00:01.5", &milliseconds, &error) && milliseconds == 1500,
      "short fractions must scale to milliseconds");
  ok &= expect(
      ParseHighlightTime(" 32 ", &milliseconds, &error) && milliseconds == 32000 &&
          FormatHighlightTime(milliseconds) == "00:00:32",
      "seconds alone must omit hours and minutes");
  ok &= expect(
      ParseHighlightTime("14:12", &milliseconds, &error) && milliseconds == 852000 &&
          FormatHighlightTime(milliseconds) == "00:14:12",
      "two fields must mean minutes and seconds");
  ok &= expect(
      ParseHighlightTime("1345.25", &milliseconds, &error) && milliseconds == 1345250 &&
          FormatHighlightTime(milliseconds) == "00:22:25.250",
      "large seconds and fractional seconds must normalize");
  ok &= expect(
      ParseHighlightTime("90:01", &milliseconds, &error) && milliseconds == 5401000 &&
          FormatHighlightTime(milliseconds) == "01:30:01",
      "leading minutes may exceed 59");
  ok &= expect(
      ParseHighlightTime("1:2:3", &milliseconds, &error) && milliseconds == 3723000,
      "three fields must mean hours, minutes, and seconds without required padding");
  ok &= expect(FormatHighlightTime(1000) == "00:00:01", "whole seconds need no fraction");
  ok &= expect(
      !ParseHighlightTime("00:60:00", &milliseconds, &error) && !error.isEmpty(), "invalid minutes must be rejected");
  ok &= expect(!ParseHighlightTime("14:60", &milliseconds, &error), "invalid two-field seconds must be rejected");
  ok &= expect(!ParseHighlightTime("-1.5", &milliseconds, &error), "negative offsets must be rejected");
  ok &= expect(
      ParseHighlightTime("90:23.125", &milliseconds, &error) && milliseconds == 5423125,
      "offsets accept minutes above 59 and fractional seconds together");
  ok &= expect(!ParseHighlightTime("9223372037", &milliseconds, &error), "oversized seconds must be rejected");
  ok &= expect(
      !ParseHighlightTime("999999999:00:00", &milliseconds, &error),
      "timestamps beyond signed nanoseconds must be rejected");
  ok &= expect(
      ParseHighlightTime("2562047:47:16.854", &milliseconds, &error) && milliseconds == 9223372036854LL &&
          FormatHighlightTime(milliseconds) == "2562047:47:16.854",
      "largest signed-nanosecond millisecond value must round trip");
  ok &= expect(
      !ParseHighlightTime("2562047:47:16.855", &milliseconds, &error),
      "one millisecond past the pipeline time range must fail");

  HighlightInterval event;
  event.label = "First goal";
  event.event_mode = true;
  ok &= expect(
      ParseHighlightTime("2:00", &event.event_ms, &error) && ParseHighlightTime("40", &event.duration_ms, &error),
      "event time and duration must both accept abbreviated formats");
  ok &= expect(
      NormalizeHighlightInterval(&event, &error) && event.start_ms == 90000 && event.end_ms == 130000,
      "event duration must place 75 percent before and 25 percent after the event");
  HighlightInterval early = event;
  early.event_ms = 10000;
  ok &= expect(
      NormalizeHighlightInterval(&early, &error) && early.start_ms == 0 && early.end_ms == 20000,
      "events near source start must clamp without a negative bound");
  HighlightInterval explicit_interval;
  explicit_interval.label = "Second goal";
  explicit_interval.event_mode = false;
  explicit_interval.start_ms = 230010;
  explicit_interval.end_ms = 245123;

  HighlightPlan moving;
  moving.playback_start_ms = 60000;
  moving.intervals = {event, explicit_interval};
  hm::ui::HighlightAnnotation cue;
  cue.start_ms = 120000;
  cue.end_ms = 122000;
  cue.motion = "keyframes";
  cue.positions = {{120000, 0.2, 0.3}, {122000, 0.4, 0.5}};
  moving.intervals[0].annotations = {cue};
  ok &= expect(
      hm::ui::SetHighlightPlaybackStart(&moving, 90000, &error) && moving.intervals[0].event_ms == 150000 &&
          moving.intervals[0].start_ms == 120000 && moving.intervals[1].start_ms == 260010 &&
          moving.intervals[0].annotations[0].positions[0].time_ms == 150000,
      "changing playback origin preserves clip and authored-keyframe offsets");
  ok &= expect(
      !hm::ui::SetHighlightPlaybackStart(&moving, 9223372036854LL, &error) && moving.playback_start_ms == 90000 &&
          moving.intervals[0].event_ms == 150000,
      "overflowing origin changes leave the complete plan unchanged");
  ok &= expect(NormalizeHighlightInterval(&explicit_interval, &error), "explicit bounds must normalize");
  explicit_interval.end_ms = explicit_interval.start_ms;
  ok &= expect(!NormalizeHighlightInterval(&explicit_interval, &error), "zero length intervals must be rejected");
  explicit_interval.end_ms = 245123;

  QTemporaryDir directory;
  ok &= expect(directory.isValid(), "temporary test directory must be available");
  if (!directory.isValid())
    return 1;
  const QString path = directory.filePath("highlights.json");
  HighlightPlan restored_origin;
  ok &= expect(
      SaveHighlightPlan(directory.filePath("origin.json"), moving, &error) &&
          LoadHighlightPlan(directory.filePath("origin.json"), &restored_origin, &error) &&
          restored_origin.playback_start_ms == 90000 && restored_origin.intervals[0].event_ms == 150000,
      "playback origin and absolute clip moments survive saving and reopening");
  HighlightPlan loaded;
  loaded.base_name = "stale";
  loaded.intervals.push_back(event);
  ok &= expect(
      LoadHighlightPlan(path, &loaded, &error) && loaded.intervals.isEmpty() && loaded.base_name == "highlights",
      "a missing plan must load as defaults");

  HighlightPlan plan;
  plan.base_name = "all goals";
  plan.intervals = {explicit_interval, event};
  ok &= expect(SaveHighlightPlan(path, plan, &error), "ordered intervals must save atomically");
  ok &= expect(
      LoadHighlightPlan(path, &loaded, &error) && loaded.base_name == "all goals" && loaded.intervals.size() == 2 &&
          loaded.intervals.at(0).label == "Second goal" && loaded.intervals.at(0).start_ms == 230010 &&
          loaded.intervals.at(1).event_mode && loaded.intervals.at(1).start_ms == 90000,
      "JSON round trip must preserve order, modes, and integer milliseconds");

  QFile before(path);
  ok &= expect(before.open(QIODevice::ReadOnly), "saved plan must be readable");
  const QByteArray saved_contents = before.readAll();
  before.close();
  plan.base_name = "../escape";
  ok &= expect(!SaveHighlightPlan(path, plan, &error), "base names must not contain path traversal");
  QFile after(path);
  ok &= expect(
      after.open(QIODevice::ReadOnly) && after.readAll() == saved_contents,
      "invalid save must preserve the previous plan");
  after.close();

  ok &= expect(
      write_text(
          path,
          R"({"schema":1,"base_name":"legacy","intervals":[{"label":"Goal","event_mode":false,"event_ms":0,"duration_ms":0,"start_ms":1000,"end_ms":2000}]})"),
      "legacy fixture written");
  HighlightPlan legacy;
  ok &= expect(
      LoadHighlightPlan(path, &legacy, &error) && legacy.intervals.size() == 1 && !legacy.intervals[0].is_card &&
          legacy.intervals[0].annotations.isEmpty(),
      "schema 1 interval plans migrate without inventing cues or cards");
  ok &= expect(write_text(path, "{broken"), "invalid JSON fixture must be writable");
  ok &= expect(
      !LoadHighlightPlan(path, &loaded, &error) && loaded.intervals.size() == 2,
      "invalid JSON must leave the current plan intact");
  ok &= expect(
      write_text(
          path,
          QJsonDocument(QJsonObject{{"schema", 3}, {"base_name", "goals"}, {"intervals", QJsonArray{}}}).toJson()),
      "unsupported schema fixture must be writable");
  ok &= expect(!LoadHighlightPlan(path, &loaded, &error), "unsupported schema must fail clearly");
  return ok ? 0 : 1;
}
