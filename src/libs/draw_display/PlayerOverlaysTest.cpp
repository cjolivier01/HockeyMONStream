#include "hstream/src/libs/draw_display/PlayerOverlays.h"
#include "hstream/src/libs/common/TrackColorMeta.h"
#include "hstream/src/libs/player_analytics/FrameMeta.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <new>
#include <stdexcept>
#include <vector>

std::atomic<uint64_t> allocations{0};
std::atomic<bool> fail_allocation{false};
void* operator new(size_t bytes) {
  ++allocations;
  if (fail_allocation.exchange(false))
    throw std::bad_alloc();
  if (void* p = std::malloc(bytes ? bytes : 1))
    return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept {
  std::free(p);
}
void operator delete(void* p, size_t) noexcept {
  std::free(p);
}
namespace a = hm::draw_display::analytics;
namespace pa = hm::player_analytics;
namespace po = hm::preview_overlay;
namespace {
constexpr uint32_t kAll = a::kPlayerBoxes | a::kPose | a::kJerseys | a::kActions;
void Check(bool okay, const char* message) {
  if (!okay)
    throw std::runtime_error(message);
}
void Near(float actual, float wanted, const char* message) {
  Check(std::abs(actual - wanted) < 0.002F, message);
}
void Color(a::Color actual, const NvOSD_ColorParams& expected) {
  Near(actual.red, expected.red, "red differs from authoritative object color");
  Near(actual.green, expected.green, "green differs from authoritative object color");
  Near(actual.blue, expected.blue, "blue differs from authoritative object color");
  Near(actual.alpha, expected.alpha, "alpha differs from authoritative object color");
}
struct Frame {
  NvDsBatchMeta* batch{nvds_create_batch_meta(1)};
  NvDsFrameMeta* frame{batch ? nvds_acquire_frame_meta_from_pool(batch) : nullptr};
  Frame() {
    Check(frame != nullptr, "create metadata fixture");
    nvds_add_frame_meta_to_batch(batch, frame);
    frame->source_id = 7;
    frame->buf_pts = 100;
    frame->frame_num = 3;
    frame->source_frame_width = 800;
    frame->source_frame_height = 600;
  }
  ~Frame() {
    if (batch)
      nvds_destroy_batch_meta(batch);
  }
  Frame(const Frame&) = delete;
  void CopyFrom(const Frame& source) {
    nvds_copy_frame_meta(source.frame, frame);
  }
  NvDsObjectMeta* Object(uint64_t id = 100) {
    auto* object = nvds_acquire_obj_meta_from_pool(batch);
    Check(object != nullptr, "create object fixture");
    object->object_id = id;
    object->class_id = 0;
    object->rect_params.left = 140;
    object->rect_params.top = 100;
    object->rect_params.width = 100;
    object->rect_params.height = 200;
    object->rect_params.border_color = {0.45, 0.45, 0.45, 1};
    nvds_add_obj_meta_to_frame(frame, object, nullptr);
    return object;
  }
  void Attach(const pa::FrameResult& result) {
    Check(pa::AttachFrameResult(frame, result), "attach semantic fixture");
  }
};
pa::FrameResult Result(size_t count = 1) {
  pa::FrameResult result;
  result.stream_id = 7;
  result.pts_ns = 100;
  result.sequence = 3;
  result.epoch = 2;
  result.geometry_token = 4;
  result.coordinate_width = 800;
  result.coordinate_height = 600;
  result.player_count = count;
  for (size_t i = 0; i < count; ++i) {
    auto& p = result.players[i];
    p.track_id = 100 + i;
    p.box = {140, 100, 100, 200};
    p.has_pose = true;
    p.pose_observed_at = 100;
    for (size_t j = 0; j < p.pose.size(); ++j)
      p.pose[j] = {200.0F + 4 * j + 2 * i, 150.0F + 6 * j, 0.9F};
    p.jersey.text = {'0', '7', '\0'};
    p.jersey.observed_at = 90;
    p.jersey.expires_at = 120;
    p.jersey.confidence = .8F;
    p.jersey.evidence = 3;
    p.action.label = 42;
    p.action.confidence = .7F;
    p.action.window_start = 1;
    p.action.observed_at = 95;
    p.action.expires_at = 130;
    std::memcpy(p.action_text.data(), "falling", 8);
  }
  return result;
}
po::PlayCropperTransform Transform() {
  po::PlayCropperTransform t;
  t.input_width = 1600;
  t.input_height = 900;
  t.metadata_width = 800;
  t.metadata_height = 600;
  t.source_left = 100;
  t.source_top = 20;
  t.anchor_x = 400;
  t.anchor_y = 300;
  t.crop_left = 80;
  t.crop_top = 40;
  t.crop_width = 900;
  t.crop_height = 400;
  t.output_width = 1280;
  t.output_height = 720;
  t.angle_degrees = 90;
  return t;
}
po::Point ExpectedProgram(float x, float y) {
  // Independent simplification of this 90-degree fixture: x/y metadata scale
  // is 2/1.5, rotation is around (400,300), then crop and nonuniform resize.
  return {(640 - 1.5F * y) * 1280 / 900, (2 * x - 240) * 720 / 400};
}
void GeometryAndColors() {
  Frame source;
  const auto object = source.Object();
  source.Attach(Result());
  po::TrackColorState owner;
  Check(owner.Apply(source.frame), "authoritative producer color join failed");
  Frame stitched, program;
  stitched.CopyFrom(source);
  program.CopyFrom(source);
  const auto* shared = pa::FindFrameResult(source.frame);
  Check(
      shared == pa::FindFrameResult(stitched.frame) && shared == pa::FindFrameResult(program.frame),
      "copy did not share immutable result");
  Check(shared->players[0].color_slot != pa::kNoColor, "result lacks producer color");
  const auto transform = Transform();
  a::CommandList original, mapped;
  a::BuildPlayerOverlays(stitched.frame, kAll, .3F, nullptr, 800, 600, &original);
  a::BuildPlayerOverlays(program.frame, kAll, .3F, &transform, 1280, 720, &mapped);
  Check(original.size() == 49 && mapped.size() == 49, "missing box/pose/label commands");
  for (size_t i = 0; i < 40; ++i) {
    const auto& in = original.data()[i];
    const auto& out = mapped.data()[i];
    Check(in.kind == out.kind, "transform changed command kind");
    const auto start = ExpectedProgram(in.x0, in.y0);
    Near(out.x0, start.x, "rotated/scaled x mismatch");
    Near(out.y0, start.y, "rotated/scaled y mismatch");
    if (in.kind == a::detail::Kind::kLine) {
      const auto end = ExpectedProgram(in.x1, in.y1);
      Near(out.x1, end.x, "mapped end x mismatch");
      Near(out.y1, end.y, "mapped end y mismatch");
    }
    Color(in.color, object->rect_params.border_color);
    Color(out.color, object->rect_params.border_color);
  }
  // Labels anchor above the transformed box's axis-aligned bounds, independent
  // of the original top-left corner (rotation changes which corner is highest).
  Near(mapped.data()[40].x0, ExpectedProgram(140, 300).x, "label x ignores rotated corner bounds");
  Near(mapped.data()[40].y0, ExpectedProgram(140, 100).y - 16, "label y ignores transformed box");
  Check(original.data()[40].glyph == '0' - 32 && original.data()[41].glyph == '7' - 32, "leading zero jersey lost");
  for (size_t i = 40; i < 49; ++i) {
    Color(original.data()[i].color, object->rect_params.border_color);
    Color(mapped.data()[i].color, object->rect_params.border_color);
  }
  Near(object->rect_params.left, 140, "command building mutated borrowed object coordinates");
  Near(shared->players[0].pose[0].x, 200, "command building mutated shared semantic coordinates");
  const auto count = allocations.load();
  original.Clear();
  mapped.Clear();
  a::BuildPlayerOverlays(stitched.frame, kAll, .3F, nullptr, 800, 600, &original);
  a::BuildPlayerOverlays(program.frame, kAll, .3F, &transform, 1280, 720, &mapped);
  Check(allocations.load() == count, "warmed command construction allocated");
}
void CopyOnWriteColors() {
  Frame source;
  source.Object();
  source.Attach(Result());
  Frame a_copy, b_copy;
  a_copy.CopyFrom(source);
  b_copy.CopyFrom(source);
  const auto* original = pa::FindFrameResult(source.frame);
  pa::TrackColorAllocator allocator;
  const uint64_t id = 100;
  allocator.Update(1, 100, &id, 1);
  Check(pa::AssignFrameColors(a_copy.frame, allocator), "copy color assignment failed");
  const auto* assigned = pa::FindFrameResult(a_copy.frame);
  Check(
      assigned && assigned != original && assigned->players[0].color_slot == allocator.Find(id)->slot,
      "assignment did not copy on write");
  Check(
      pa::FindFrameResult(source.frame) == original && pa::FindFrameResult(b_copy.frame) == original &&
          original->players[0].color_slot == pa::kNoColor,
      "assignment mutated another copy");
  const auto before = allocations.load();
  Check(
      pa::AssignFrameColors(a_copy.frame, allocator) && pa::FindFrameResult(a_copy.frame) == assigned,
      "identical assignment replaced handle");
  Check(allocations.load() == before, "identical colors allocated");
  fail_allocation = true;
  const bool okay = pa::AssignFrameColors(b_copy.frame, allocator);
  Check(
      !okay && !fail_allocation.load() && pa::FindFrameResult(b_copy.frame) == original,
      "failed assignment changed or escaped immutable handle");
  pa::TrackColorAllocator empty;
  Check(pa::AssignFrameColors(a_copy.frame, empty), "missing lease assignment failed");
  Check(pa::FindFrameResult(a_copy.frame)->players[0].color_slot == pa::kNoColor, "missing lease left stale color");
  auto survivor = std::make_unique<Frame>();
  {
    Frame temporary;
    temporary.Object();
    temporary.Attach(Result());
    Check(pa::AssignFrameColors(temporary.frame, allocator), "temporary color assignment failed");
    survivor->CopyFrom(temporary);
  }
  Check(
      pa::FindFrameResult(survivor->frame)->players[0].color_slot == allocator.Find(id)->slot,
      "pool destruction retired surviving copy");
  Frame no_result;
  const auto no_result_before = allocations.load();
  Check(
      pa::AssignFrameColors(nullptr, allocator) && pa::AssignFrameColors(no_result.frame, allocator),
      "no-result assignment failed");
  Check(allocations.load() == no_result_before, "no-result color assignment allocated");
}
void OverflowColors() {
  Frame frame;
  auto semantic = Result(33);
  for (size_t i = 0; i < 33; ++i)
    frame.Object(100 + i);
  frame.Attach(semantic);
  po::TrackColorState owner;
  Check(owner.Apply(frame.frame), "overflow producer join failed");
  const auto* result = pa::FindFrameResult(frame.frame);
  bool overflow = false;
  for (const auto* item = frame.frame->obj_meta_list; item; item = item->next) {
    const auto* object = static_cast<const NvDsObjectMeta*>(item->data);
    const auto& p = result->players[object->object_id - 100];
    Check(p.color_slot < pa::kTrackPalette.size(), "overflow has no bounded color");
    overflow |= p.color_shared;
    const auto& color = pa::kTrackPalette[p.color_slot];
    Color({color.red, color.green, color.blue, color.alpha}, object->rect_params.border_color);
  }
  Check(overflow, "overflow alias was not advertised");
}
void FreshnessAndLabels() {
  Frame expired;
  auto semantic = Result();
  semantic.players[0].jersey.expires_at = 99;
  semantic.players[0].action.expires_at = 99;
  expired.Attach(semantic);
  a::CommandList list;
  a::BuildPlayerOverlays(expired.frame, a::kPose | a::kJerseys | a::kActions, .3F, nullptr, 800, 600, &list);
  Check(list.size() == 36, "expired labels painted or fresh pose vanished");
  list.Clear();
  expired.frame->buf_pts = 101;
  a::BuildPlayerOverlays(expired.frame, a::kPose | a::kJerseys | a::kActions, .3F, nullptr, 800, 600, &list);
  Check(list.empty(), "old semantic timestamp painted as current pose");
  expired.frame->buf_pts = 100;
  expired.frame->source_id = 8;
  a::BuildPlayerOverlays(expired.frame, a::kPose | a::kJerseys | a::kActions, .3F, nullptr, 800, 600, &list);
  Check(list.empty(), "another stream's results were painted");
  semantic.players[0].pose_observed_at = 99;
  Check(!pa::ValidateFrameResult(semantic), "stale pose payload validated");
  semantic.players[0].pose_observed_at = 100;
  semantic.players[0].action_text[0] = '\n';
  Check(!pa::ValidateFrameResult(semantic), "nonprintable label validated");
  semantic.players[0].action_text.fill('a');
  Check(!pa::ValidateFrameResult(semantic), "unterminated label validated");
  Frame boundary;
  semantic = Result();
  semantic.players[0].jersey.expires_at = 100;
  semantic.players[0].action.expires_at = 100;
  boundary.Attach(semantic);
  a::BuildPlayerOverlays(boundary.frame, a::kJerseys | a::kActions, .3F, nullptr, 800, 600, &list);
  Check(list.size() == 9, "labels at their expiry boundary disappeared");
  Frame many;
  many.Attach(Result(2));
  a::CommandList limited(72);
  a::BuildPlayerOverlays(many.frame, a::kPose | a::kJerseys | a::kActions, .3F, nullptr, 800, 600, &limited);
  Check(limited.size() == 72 && limited.rejected() > 0, "geometry priority did not fill bounded commands before text");
  for (size_t i = 0; i < limited.size(); ++i)
    Check(limited.data()[i].kind != a::detail::Kind::kGlyph, "text starved later player's geometry");
}
void BakedTransformsAndEmpty() {
  Frame original;
  original.Object();
  original.Attach(Result());
  auto transform = Transform();
  transform.baked_player_layers = kAll;
  Check(po::add_playcropper_transform_meta(original.frame, transform), "attach baked transform");
  Frame copy;
  copy.CopyFrom(original);
  const auto* first = po::find_playcropper_transform_meta(original.frame);
  const auto* second = po::find_playcropper_transform_meta(copy.frame);
  Check(
      first && second && first != second && second->baked_player_layers == kAll,
      "copied transform lost or shared baked bits");
  a::CommandList list;
  const auto before = allocations.load();
  a::BuildPlayerOverlays(original.frame, 0, .3F, nullptr, NAN, NAN, &list);
  a::BuildPlayerOverlays(copy.frame, kAll, .3F, second, 1280, 720, &list);
  Check(list.empty() && allocations.load() == before, "off/baked-empty path allocated or drew");
  transform.baked_player_layers = a::kPose | a::kJerseys;
  a::BuildPlayerOverlays(original.frame, kAll, .3F, &transform, 1280, 720, &list);
  Check(list.size() == 11, "partial baked transform did not preserve only boxes/actions");
  Frame semantic;
  auto result = Result();
  result.baked_layers = a::kPose | a::kJerseys | a::kActions;
  semantic.Attach(result);
  list.Clear();
  a::BuildPlayerOverlays(semantic.frame, a::kPose | a::kJerseys | a::kActions, .3F, nullptr, 800, 600, &list);
  Check(list.empty(), "semantic baked pose/labels repainted");
}
void ZeroConfidenceNeverDraws() {
  Frame frame;
  auto result = Result();
  for (auto& joint : result.players[0].pose)
    joint = {0, 0, 0};
  frame.Attach(result);
  a::CommandList list;
  a::BuildPlayerOverlays(frame.frame, a::kPose, 0, nullptr, 800, 600, &list);
  Check(list.empty(), "zero-confidence invalid joints/bones are painted when threshold is zero");
  Frame one_valid;
  result.players[0].pose[0] = {20, 30, .4F};
  one_valid.Attach(result);
  a::BuildPlayerOverlays(one_valid.frame, a::kPose, 0, nullptr, 800, 600, &list);
  Check(
      list.size() == 1 && list.data()[0].kind == a::detail::Kind::kDisc,
      "zero threshold lost the valid joint or joined it to an invalid endpoint");
}
void BakedBoxesNeverDraw() {
  Frame frame;
  frame.Object();
  auto result = Result();
  result.baked_layers = a::kPlayerBoxes;
  frame.Attach(result);
  a::CommandList list;
  a::BuildPlayerOverlays(frame.frame, a::kPlayerBoxes, .3F, nullptr, 800, 600, &list);
  Check(list.empty(), "FrameResult baked box bit was ignored");
  frame.frame->buf_pts = 101;
  a::BuildPlayerOverlays(frame.frame, a::kPlayerBoxes, .3F, nullptr, 800, 600, &list);
  Check(list.size() == 4, "stale semantic baked bits suppressed current object boxes");
  list.Clear();
  frame.frame->buf_pts = 100;
  frame.frame->source_id = 8;
  a::BuildPlayerOverlays(frame.frame, a::kPlayerBoxes, .3F, nullptr, 800, 600, &list);
  Check(list.size() == 4, "another stream's baked bits suppressed current object boxes");
}
void LabelsRequireVisiblePlayer() {
  auto transform = po::PlayCropperTransform{};
  transform.input_width = transform.metadata_width = 800;
  transform.input_height = transform.metadata_height = 600;
  transform.crop_left = 200;
  transform.crop_top = 150;
  transform.crop_width = transform.output_width = 400;
  transform.crop_height = transform.output_height = 300;
  const auto commands_for = [](const pa::Box& box, const po::PlayCropperTransform& t) {
    Frame frame;
    auto result = Result();
    result.players[0].box = box;
    frame.Attach(result);
    a::CommandList list;
    a::BuildPlayerOverlays(frame.frame, a::kJerseys | a::kActions, .3F, &t, t.output_width, t.output_height, &list);
    return list.size();
  };
  for (const auto& box :
       {pa::Box{10, 200, 30, 50}, pa::Box{250, 10, 30, 50}, pa::Box{650, 200, 30, 50}, pa::Box{250, 500, 30, 50}})
    Check(commands_for(box, transform) == 0, "Fully offcrop player emitted clamped labels");
  Check(commands_for({190, 200, 30, 50}, transform) == 9, "Partly visible player lost labels");
  // A 100x100 box rotated 45 degrees forms a diamond. This viewport overlaps
  // its AABB but lies beyond the diamond's lower-right edge (x+y > sqrt(2)*100).
  transform.angle_degrees = 45;
  transform.crop_left = 40;
  transform.crop_top = 110;
  transform.crop_width = transform.crop_height = 30;
  transform.output_width = transform.output_height = 300;
  Check(commands_for({0, 0, 100, 100}, transform) == 0, "Rotated offcrop polygon admitted by AABB painted labels");
  transform.crop_left = 10;
  transform.crop_top = 80;
  Check(commands_for({0, 0, 100, 100}, transform) == 9, "Visible rotated player lost labels");
}
} // namespace
int main() {
  struct Test {
    const char* name;
    void (*run)();
  };
  const Test cases[] = {
      {"geometry_and_colors", GeometryAndColors},
      {"copy_on_write_colors", CopyOnWriteColors},
      {"overflow_colors", OverflowColors},
      {"freshness_labels_priority", FreshnessAndLabels},
      {"baked_transform_and_empty", BakedTransformsAndEmpty},
      {"zero_confidence", ZeroConfidenceNeverDraws},
      {"baked_boxes", BakedBoxesNeverDraw},
      {"labels_require_visible_player", LabelsRequireVisiblePlayer}};
  unsigned failures = 0;
  for (const auto& test : cases) {
    try {
      test.run();
      std::cout << "PASS " << test.name << '\n';
    } catch (const std::exception& error) {
      ++failures;
      std::cerr << "FAIL " << test.name << ": " << error.what() << '\n';
    }
  }
  return failures ? 1 : 0;
}
