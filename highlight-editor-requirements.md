# Highlight editor requirements

Recorded: 2026-10-08. Updated: 2026-10-09.

Status: implementation authorized after the published-archive editor in PR #247 merged. The requirements below retain
what was agreed; implementation decisions and platform limits are recorded at the end and in docs/highlights.md.

The editor should produce youth sports reels, primarily highlighting a particular player and sometimes presenting a
whole-team game recap. The user chooses the footage, annotations, and sequence. Short visual cues help viewers identify
the subject and understand a play, then disappear so the footage can continue unobstructed.

## Reel contents

The clip table represents the ordered contents of the finished reel. It must support video clips and generated title
cards as individual rows.

| Item type | Content | Timing |
| --- | --- | --- |
| Video clip | A selected source interval and its timed annotations | Source start and end |
| Title card | Text, optional team names and logos, and a designed background | User-selected display duration |

Users must be able to insert, edit, reorder, duplicate, and remove items. Cards can appear at the beginning, between any
two clips, or elsewhere in the sequence. Opening titles and intermissions use the same card item mechanism.

Each item must support preview on its own and within the full reel. Cards participate in selection, playback, looping,
and export like video items. The full sequence uses table order, including adjacent video clips where the user has
chosen no intervening card.

## User-created player markers

The primary workflow is to pause near the start of a clip, select a player, and add a short marker. A representative
style is a thick, blinking yellow arrow above the player. It follows the selected player for a couple of seconds, then
disappears. Viewers follow the player themselves after that cue ends.

The user may add a second, third, or further marker at any later point in the clip, for example after several players
cross paths. Every occurrence must be explicitly created and timed by the user. The software must never decide that the
viewer has lost the player and insert or repeat a marker automatically.

Each marker has its own start time, duration, placement, appearance, and optional tracking selection. Later markers can
be selected independently, so they do not require one uninterrupted track spanning the entire clip. Tracking only moves
an existing annotation during the interval the user assigned to it.

## Timed annotations

Support text, arrows, and boxes throughout a video clip. These share timing and styling controls with player markers.
Multiple annotations may be visible at once, with independent appearance, timing, and movement settings.

| Control | Required behavior |
| --- | --- |
| Start and end | User chooses when the annotation appears and disappears. It may start anywhere in the clip. |
| Text | User enters the wording and controls text size, weight/boldness, and color. Large, heavy yellow lettering must be possible. |
| Arrows | User controls position, direction, tip/tail placement, size, thickness, and color. |
| Boxes | User controls position, dimensions, border thickness, and color. |
| Animation | User can choose steady display or blinking during the annotation's active interval. |
| Movement | User can choose a fixed position in the output image or attachment to a selected tracked person. |
| Editing | User can adjust timing, placement, appearance, and tracking selection, or remove the annotation. |

Fixed placement means the annotation remains at the same position in the video image while playback and camera motion
continue. It is independent of the editor window's size. Tracked placement follows the selected person, with a
user-adjustable offset for positioning text or an arrow near the subject. Text, arrows, and boxes must each allow either
movement mode; movement is not implied by the annotation type.

Players and referees are valid subjects. The user identifies the subject; automatic recognition of a named player,
jersey number, referee, whistle, or penalty is not required by this workflow.

The editor must also accommodate pointing to a specific detail, such as the hand involved in holding. The annotation
must be positionable precisely and correctable over its short active interval. A player bounding box alone does not
establish a hand's position. The mechanism for following such a detail remains an implementation decision; see the open
decisions below.

Annotations disappear when their assigned interval ends, regardless of tracking availability or blink phase. A marker
at the beginning does not imply any later marker. Two annotations can overlap without sharing their tracking or style:
for example, fixed "Holding penalty" text and an arrow following the relevant contact point.

Annotation timing belongs to the video clip. Inserting, removing, or resizing a preceding title card must not move an
annotation to a different moment within the play. Reordering a clip carries its annotations with it. The handling of
subsequent edits to the clip's source in/out points needs to be specified before implementation.

## Required annotation examples

| Scenario | User action | Result |
| --- | --- | --- |
| Identify a player | Add a yellow blinking arrow at the clip's beginning and select the player | The arrow follows briefly and disappears; the clip continues. |
| Identify the player again | Manually add another marker later in the same clip | Only that added interval shows another marker. No additional cues are generated. |
| Holding penalty | Add heavy colored text and an arrow aimed at the relevant hand/contact point | The text and arrow can have independent movement; both disappear at their chosen end times. |
| Whistle before a late hit | Place blinking "WHISTLE!" text at the user-selected whistle moment | The text blinks briefly and disappears, allowing the subsequent hit to be seen. Timing is supplied by the user. |
| Identify the referee | Attach "WHISTLE!" text or an arrow to a selected referee for a short interval | The annotation follows that referee only during its assigned interval. |
| Mark an area | Draw a thick colored box around the area of interest | The box remains fixed or follows a selected person, according to the user's choice. |

An illustrative clip could show an arrow from 0 to 2 seconds, normal footage from 2 to 8 seconds, and another arrow from
8 to 10 seconds. Those times illustrate two user-created annotations; they are not an automatic schedule.

## Title cards and intermissions

A matchup title card must support two team names, a user-supplied logo/icon for each team, and a clear "Team A vs Team B"
composition. Logos render on the card near their respective names. The default layout should look professional without
requiring the user to position every element manually: balanced spacing, readable type, consistent visual hierarchy,
and proportional logo sizing without distortion.

Every matchup/game title card must have a valid game date, displayed as `YYYY-MM-DD`. Use known game metadata to prefill
it when available, and allow the user to correct it. If the date is unknown, require entry rather than silently using
the export date. A generic section or text card does not require a date.

Cards need editable text, display duration, background color, text color, size, and weight. The matchup card should allow
an optional heading such as "Game Highlights". Venue, subtitle, score, additional backgrounds, and further templates are
possible extensions, not prerequisites for the agreed matchup layout.

Intermissions are optional. The user can make a reel with no cards, a title only, cards at selected boundaries, or cards
between every clip. A short card may identify the next clip or section, for example "Holding penalty", "Second period",
or a clip number and label. A matchup card can also be inserted or repeated between clips. Any future convenience action
that inserts several intermissions should create ordinary editable rows so the visible table remains the sequence that
will be previewed and exported.

Example sequence:

| Order | Item | Example duration |
| --- | --- | --- |
| 1 | Matchup card with both team names/logos and `2026-10-08` | 4 seconds |
| 2 | Goal clip with a brief user-added player arrow | 12 seconds |
| 3 | "Holding penalty" text card | 2 seconds |
| 4 | Clip with text and an arrow identifying the contact | 10 seconds |
| 5 | Another video clip, directly after the preceding clip | 15 seconds |

## Preview, export, and saved editing state

Preview must show the authored result: item order, card durations, logos, dates, annotation positions, tracking motion,
colors, weights, and blinking. Users must be able to inspect a selected clip or card on its own and then preview it in
context. Preview and exported output must agree on these features; annotations must be rendered into the exported video.
Window resizing must not change their intended position or apparent size relative to the output image.

Save enough editing state to reopen and revise the reel, including item order, clip source intervals, annotations and
their timing/styles/placement/tracking bindings, card contents and durations, dates, and logo asset references. Existing
saved interval-only plans must remain usable. The specific persistence schema and asset-storage policy should be chosen
after reviewing the upcoming highlight editor change.

Preserve the existing editor's useful behavior, including selected/full-reel previews, stop/cancel, supported output
routes, and publication without overwriting existing completed files. All preview windows must retain the repository's
resize/maximize behavior.

## Native reel pipeline direction

The preferred implementation is an hstream-owned native media pipeline for decoding, rendering video and generated
cards/intermission screens, sequencing the reel, and encoding/muxing its output. Do not base this new workflow on
external FFmpeg rendering commands or a collection of FFmpeg-generated segments. Use the existing C++/GStreamer stack
and hardware media components where suitable. This preference does not mean implementing new codecs.

Conceptually, the reel has two kinds of frame input:

```text
Video item: source decode and required video processing --+
                                                        +--> ordered reel frames and timed GPU rendering
Card item: generated background, text, and logos ---------+       |--> GPU preview
                                                                +--> hardware encode --> mux --> output file
```

The diagram describes responsibilities, not a mandated element graph or process boundary. Playback may use a preview
sink without encoding, and export may run without displaying. Both must use the same authored scene, timing rules, and
rendering behavior so cards, video annotations, and tracked positions agree.

The pipeline must place video and card items onto a coherent output timeline. It must distinguish source timestamps,
clip-local annotation times, and reel output timestamps. Switching items must preserve intended durations and audio/video
alignment without carrying a clip's tracking or annotations into another item. Source audio and the eventual card audio
policy must fit that same timeline. Export must finish encoding and finalize the output container before publication.

New native GStreamer plugins are a valid design option if existing components do not provide the needed behavior.
Potential responsibilities include generating card frames or compositing timed text, arrows, boxes, and logos onto GPU
surfaces. Reuse existing rendering libraries and media elements where they fit; the number of plugins, their boundaries,
and whether sequencing belongs in an element, bin, or native controller remain open. No new plugin is required merely
to satisfy this document.

This direction is a larger export-orchestration change than adding controls to the current dialog. A dedicated native
reel pipeline is a candidate for containing that work while reusing the established decode, GPU processing, preview,
and encode components. It does not require deciding to move ordinary playback into the UI process. Assess sequencing,
seek/flush/EOS behavior, audio continuity, and resource ownership after the pending editor change before selecting the
architecture. Implementation is now based on the published-archive editor from PR #247.

## Existing code and implementation constraints

These are implementation pointers and constraints, not a completed design.

- [Current Highlights behavior](docs/highlights.md), [HighlightPlan](src/apps/hstream-ui/HighlightPlan.h), and
  [HighlightsDialog](src/apps/hstream-ui/HighlightsDialog.cpp) describe and implement the current interval-based editor.
  The editor now reads published archives and saves game-local `highlights.json`; the new native reel controller
  replaces the earlier FFmpeg clip encoding/joining path.
- The [telemetry database](docs/telemetry-database.md) and [schema](src/libs/recording/schema.sql) already contain recorded
  track IDs, bounding boxes, frame timestamps, run identities, seek/reset boundaries, and geometry references. Existing
  telemetry is the preferred candidate for moving annotations, subject to validating its coverage and source binding.
  Its use is a design preference, not a requirement to introduce new full-game analysis.
- A recorded track selection must be tied to its recorded run and time/geometry context. A fresh clip export cannot
  assume that a newly generated runtime track ID identifies the same person. Recorded stitched coordinates must be
  transformed into the actual preview/export crop and rotation at the correct frame. Referee availability and continuity
  need verification on real recordings.
- Keep video GPU-resident through preview and export. Reuse existing GPU overlay/rendering paths where appropriate;
  do not introduce steady-state video readback for drawing annotations. Loading a user-supplied logo is a separate asset
  operation.
- The implementation must remain native C++/Qt/GStreamer-based and must not depend on Python. Native pipeline components
  should own video decode, annotation/card rendering, sequencing, re-encoding, and final container muxing. Existing FFmpeg
  use elsewhere does not establish a dependency for this new path. Any use of auxiliary media inspection tools is a
  separate design decision; it must not substitute for the native reel rendering/export pipeline.
- Intermission reference:
  [HockeyMOMWeb video_clipper.py](/home/colivier/src/HockeyMOMWeb/hockeymomweb/cli/video_clipper.py).
  Its `create_text_video` creates a black screen with centered white text and silent audio. Cards use a configurable
  duration, defaulting to three seconds; zero disables them. The script inserts cards labeled with the supplied text and
  clip number before clips, including the first, and concatenates the encoded segments using stream copy. This is a
  visual/behavior reference only. Its Python implementation, FFmpeg generation/concatenation strategy, and defaults are
  not the chosen implementation approach for the new editor.

## Open design decisions

- Exact controls for placing annotations and editing their time ranges. Short blocks on a clip timeline are a useful
  candidate, but the UI must be reconciled with the upcoming editor change.
- Whether a new marker defaults to two seconds, and the default blink cadence. "A couple of seconds" and blinking
  yellow arrows are the intended starting experience; no fixed cadence has been agreed.
- How to handle missing telemetry, a missing referee track, a gap, or an identity switch during an annotation. The
  chosen behavior must be visible and correctable; it must not silently attach to a different person or create new cues.
- How to support precise hand/contact-point motion: manual position keyframes, separate point tracking, or another
  correctable method. Whole-person tracking is insufficient to promise automatic hand tracking.
- Whether annotation times are stored relative to the source or clip, and how trim edits preserve their intended event
  alignment. Card insertion and reel reordering must never alter that alignment.
- Card audio behavior and defaults, logo formats/storage, behavior when assets are missing, and whether optional bulk
  intermission insertion is worth adding. Silent cards are supported by the reference script but are not a decided audio
  policy for this editor.
- Exact native pipeline graph, process ownership, plugin boundaries, and persistence integration after the pending
  editor change. Decide how to share rendering between preview/export, generate GPU card surfaces, sequence source and
  output timestamps, handle audio across item boundaries, and drain/finalize the encoder and muxer. Broader identity
  tracking, point tracking, or preview ownership changes should also be assessed for architectural cost.

Automatic highlight discovery, automatic whistle/penalty detection, automatic placement or repetition of markers,
continuous full-game player identity, automatic player-following camera crops, season-wide clip libraries, music editing,
and social-format exports are outside the requirements agreed here. They should not become prerequisites for manually
authored cues, title cards, and intermissions.

## Acceptance scenarios for the future implementation

1. Add a blinking yellow arrow for the first two seconds of a clip. It follows the selected player, then disappears in
   both preview and export. Playback continues without further cues unless the user adds them.
2. Add a second and third marker later in that clip. Each follows its own user-selected target for its chosen interval.
   Remove one and verify the other intervals and markers are unaffected.
3. Combine fixed, heavy yellow "Holding penalty" text with a moving arrow. Adjust the arrow's thickness and color
   independently of the text, and correct its placement at the contact point.
4. Place blinking "WHISTLE!" text at a manually selected time before a late hit. Verify its start, blink behavior, and
   disappearance, with tracking off. Repeat with the text attached to a referee where a valid track is available.
5. Add a colored box and switch between fixed and tracked placement without changing its active time range.
6. Insert a matchup card with two team names and logos. Require a valid date and render it as `YYYY-MM-DD`. Preview the
   card alone, then preview and export it as part of the reel with the same layout and duration.
7. Insert cards at selected clip boundaries, including a repeated matchup card, while leaving other clips adjacent.
   Reorder and remove cards; the table, preview, and exported sequence must agree.
8. Change an intermission's duration and move a clip to a different table position. Its annotations still identify the
   same moments within the footage.
9. Save and reopen a mixed reel. Its cards, assets, date, annotation styling, tracking selections, timing, and ordering
   remain editable and reproduce the saved result. An existing interval-only plan still opens correctly.
10. Exercise tracking gaps, different recorded runs, and changed output geometry. Verify the implementation's explicit
    correction/failure behavior and ensure it never silently transfers a marker to another person.
11. Preview and export a reel containing video, a title card, another video, and an intermission through the native
    pipeline. Verify card/video durations, annotation timing, audio/video alignment, cancellation, and final-container
    completion. The reel's decode/render/encode/mux path must work without launching FFmpeg jobs to produce or assemble it.

## Implementation decisions after PR #247

The implementation uses schema 2 while reading schema 1 plans. Cards and clips are ordinary ordered rows. Cues retain
absolute game/source times through trims, card insertion, and reordering; rendering clips their visibility to the
selected footage. Defaults are a two-second yellow arrow and a 250 ms on/off blink phase, with no automatic repeats.
Manual keyframes interpolate positions for detail corrections. Recorded tracks retain compact normalized observations
and their run/source/geometry/seek/reset identity, plus the explicitly verified archive and timeline binding. Missing
observations hide a cue after 100 ms. Referees need a recorded track or manual keyframes.

Cards use silent audio. Raster logos are imported into game-local content-addressed PNG assets. Matchup dates require
valid YYYY-MM-DD entry; a known date in the game identifier can prefill it. The shared native reel controller uses
NVIDIA decode, GPU compositing, sequencing, hardware encode, and MP4 mux. Static asset rasterization/upload is bounded
and separate from GPU-resident video. Preview and export share that renderer, with scaled preview geometry. The
existing desktop platform boundary remains x86/NVIDIA/X11; Jetson does not build the Qt UI. See docs/highlights.md for
the implemented authoring controls, limits, tracking binding, and publication behavior.
