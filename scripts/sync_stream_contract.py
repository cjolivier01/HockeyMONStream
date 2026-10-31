"""Generate browser startup controls from reviewed native configuration families.

Run with --check in compatibility CI. Review a regenerated diff before deploying
the web service and matching HStream distribution together.
"""

import argparse
import hashlib
import json
import re
import subprocess
from datetime import time
from pathlib import Path

import yaml

# The keys are the native canonical mapping used by HStreamWindow when saving.
# Presentation, bounds and unit conversion are read from the same Qt specs.
CAMERA_KEYS = {
    "Zoom_In_Aggressiveness": "rink.camera.zoom_in_aggressiveness",
    "Stop_Direction_Change_Delay_Frames": "rink.camera.stop_on_dir_change_delay",
    "Cancel_Stop_On_Opposite_Direction": "rink.camera.cancel_stop_on_opposite_dir",
    "Stop_Cancel_Hysteresis_Frames": "rink.camera.stop_cancel_hysteresis_frames",
    "Stop_Delay_Cooldown_Frames": "rink.camera.stop_delay_cooldown_frames",
    "Time_To_Dest_Speed_Limit_Frames": "rink.camera.time_to_dest_speed_limit_frames",
    "Overshoot_Stop_Delay_Frames": "rink.camera.breakaway_detection.overshoot_stop_delay_count",
    "Post_Nonstop_Stop_Delay_Frames": "rink.camera.breakaway_detection.post_nonstop_stop_delay_count",
    "Overshoot_Speed_Ratio_x100": "rink.camera.breakaway_detection.overshoot_scale_speed_ratio",
}


def camera_specs(root):
    source = (root / "src/libs/tracking/CameraControlSpecs.h").read_text()
    pattern = re.compile(
        r'\{\s*"(?P<id>[^"\n]+)"\s*,\s*"(?P<label>[^"\n]+)"\s*,'
        r"\s*(?P<minimum>-?\d+)\s*,\s*(?P<maximum>-?\d+)\s*,"
        r'\s*default_value\("(?P=id)"\)'
        r'(?:\s*,\s*"(?P<runtime_key>[^"\n]+)"\s*,\s*(?P<divisor>\d+))?\s*\}'
    )
    result = {}
    for match in pattern.finditer(source):
        spec = match.groupdict()
        spec["minimum"] = int(spec["minimum"])
        spec["maximum"] = int(spec["maximum"])
        spec["divisor"] = int(spec["divisor"] or 1)
        result[spec["id"]] = spec
    if not CAMERA_KEYS.keys() <= result.keys():
        raise ValueError(
            "Qt camera specifications changed; review the canonical mappings."
        )
    return result


def calibration_stages(root):
    source = (root / "src/apps/hstream-ui/HStreamWindow.cpp").read_text()
    table = re.search(r"kCalibrationStages\[\]\s*=\s*\{(.*?)\n\};", source, re.DOTALL)
    if table is None:
        raise ValueError("Qt calibration stages changed; review the progress dialog")
    stages = [
        {"id": stage, "label": label, "activity": activity}
        for stage, label, activity in re.findall(
            r'\{"([^"\n]+)",\s*"([^"\n]+)",\s*"([^"\n]+)"\}', table.group(1)
        )
    ]
    if len(stages) != 8 or len({stage["id"] for stage in stages}) != len(stages):
        raise ValueError("Review the native calibration stage mapping")
    return stages


def tone_specs(root):
    source = (root / "src/libs/program/ToneControls.h").read_text()
    pattern = re.compile(
        r'\{\s*"([^"\n]+)"\s*,\s*"([^"\n]+)"\s*,\s*(\d+)\s*,\s*(\d+)\s*,'
        r'\s*"([^"\n]+)"\s*,\s*(\d+)\s*\}'
    )
    controls = [
        {
            "id": identifier,
            "label": label,
            "minimum": int(lower),
            "maximum": int(upper),
            "property": prop,
            "divisor": int(divisor),
            "step": 1,
        }
        for identifier, label, lower, upper, prop, divisor in pattern.findall(source)
    ]
    if len(controls) != 3 or len({item["id"] for item in controls}) != 3:
        raise ValueError(
            "Native tone specifications changed; review the browser mapping"
        )
    return controls


def rotation_specs(root):
    source = (root / "src/libs/program/RotationControls.h").read_text()
    qt = (root / "src/apps/hstream-ui/HStreamWindow.cpp").read_text()
    table = qt.split("const std::vector<CameraSliderSpec> stitch_controls = {", 1)[
        1
    ].split("\n  };", 1)[0]
    labels = dict(re.findall(r'\{\s*"([^"\n]+)"\s*,\s*"([^"\n]+)"', table))
    controls = [
        {
            "id": identifier,
            "label": labels[identifier],
            "minimum": int(lower),
            "maximum": int(upper),
            "step": 1,
        }
        for identifier, lower, upper in re.findall(
            r'\{\s*"([^"\n]+)"\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*\}', source
        )
    ]
    if len(controls) != 4 or len({item["id"] for item in controls}) != 4:
        raise ValueError(
            "Native rotation specifications changed; review the browser mapping"
        )
    # Qt owns two independent docks: its post-stitch angle affects the panorama,
    # while the linked edge angles affect the Program crop. Keep this grouping
    # in the generated contract instead of treating one runtime command as one
    # visual control group.
    if (
        "crop_controls(stitch_controls.begin() + 1, stitch_controls.end())" not in qt
        or "rotation_controls = {stitch_controls.front()}" not in qt
        or 'tab_icon("Crop Rotation"), "Crop Rotation"' not in qt
    ):
        raise ValueError("Qt rotation groups changed; review the browser placement")
    for control in controls:
        post_stitch = control["id"] == "Stitch_Rotate_Degrees"
        control["qt_stage"] = "stitched" if post_stitch else "program"
        control["qt_tab"] = "Rotation" if post_stitch else "Crop Rotation"
    return controls


def precision_spec(root):
    source = (root / "src/apps/hstream-ui/HStreamWindow.cpp").read_text()
    match = re.search(
        r'add_mirrored_checkbox\("Use_10_Bit_Grading", "([^"]+)"\)', source
    )
    if not match or "checkbox->setTristate(true)" not in source:
        raise ValueError("Qt precision control changed; review the three-state mapping")
    labels = re.search(r"\(([^/]+) / ([^/]+) / ([^,]+), next run\)", match[1])
    if not labels:
        raise ValueError("Qt precision state labels changed")
    return {
        "label": match[1],
        "choices": dict(zip(("auto", "1", "0"), labels.groups(), strict=True)),
    }


def source_set_contract(root):
    source = (root / "src/apps/hstream-ui/HStreamWindow.cpp").read_text()
    block = source.split("bool is_auto_chapter_file(", 1)[1].split("\n}", 1)[0]
    patterns = {
        name: json.loads(literal)
        for name, literal in re.findall(
            r'static const QRegularExpression (\w+)\(("(?:[^"\\]|\\.)*")\);',
            block,
        )
    }
    if set(patterns) != {"gopro", "insta360", "left_right"}:
        raise ValueError("Native Auto source recognizers changed; review grouping.")
    family = source.split("QString auto_file_family(", 1)[1].split("\n}", 1)[0]
    family_patterns = {
        name: json.loads(literal)
        for name, literal in re.findall(
            r'static const QRegularExpression (\w+)\(("(?:[^"\\]|\\.)*")\);',
            family,
        )
    }
    if family_patterns != patterns:
        raise ValueError("Native Auto grouping differs from its filename admission.")
    slices = {
        name: [int(start), int(length)]
        for name, start, length in re.findall(
            r'return "(gopro|insta360)-" \+ file_name.mid\((\d+), (\d+)\);', family
        )
    }
    if (
        set(slices) != {"gopro", "insta360"}
        or "lr_match.captured(1).toLower()" not in family
    ):
        raise ValueError("Native Auto family identities changed; review grouping.")
    video = source.split("bool is_video_file(", 1)[1].split("\n}", 1)[0]
    extensions = re.findall(r'suffix == "([a-z0-9]+)"', video)
    if not extensions:
        raise ValueError("Native source formats are unavailable.")
    return {
        "auto_patterns": patterns,
        "family_slices": slices,
        "extensions": extensions,
    }


def detector_catalog(root, baseline):
    source = (root / "src/apps/hstream-ui/DetectorOptions.cpp").read_text()
    match = re.search(r"kPrecisions\{([^}]+)\}", source)
    if not match:
        raise ValueError("Native detector precision catalog changed")
    precisions = re.findall(r'"([^"\n]+)"', match[1])
    choices = {
        "": "Use saved detector configuration",
        **{mode: mode.upper() for mode in precisions},
    }
    models, paths = [], []
    for row in baseline["hstream_ui"]["detector_models"]:
        model = {
            "id": row["id"],
            "label": row["label"],
            "recipes": {},
            "preparation_model": {
                "config_infer_yolov8_hockey": "yolov8m",
                "config_infer_yolov8s_hockey": "yolov8s",
            }[row["config_prefix"]],
        }
        for precision in precisions:
            filename = (
                row["config_prefix"]
                + ("" if precision == "fp32" else "_" + precision)
                + ".yaml"
            )
            path = "configs/" + filename
            inference = yaml.safe_load((root / path).read_text())
            model["recipes"][precision] = {
                "config": filename,
                "engine": inference["property"]["model-engine-file"],
            }
            paths.append(path)
        models.append(model)
    configured = baseline["pipeline"]["primary-gie"]["config-file"]
    selected = next(
        (
            {"model": model["id"], "precision": mode}
            for model in models
            for mode, recipe in model["recipes"].items()
            if recipe["config"] == configured
        ),
        None,
    )
    if (
        selected is None
        or not models
        or len({row["id"] for row in models}) != len(models)
    ):
        raise ValueError("Native baseline detector selection is not in its catalog")
    return {"models": models, "precisions": choices, "baseline": selected}, paths


def model_catalog(root):
    source = (root / "src/libs/player_analytics/ModelCatalog.cpp").read_text()
    contracts = [
        json.loads(document)
        for document in re.findall(
            r'R"MODEL\((.*?)\)MODEL"',
            (root / "src/libs/player_analytics/ModelCatalogData.inc").read_text(),
            re.DOTALL,
        )
    ]
    topology = {item["model_id"]: item.get("topology_id") for item in contracts}
    result = {}
    for feature, identifier, label in re.findall(
        r'\{\s*"(pose|jersey|action|team|reid)"\s*,\s*"([^"\n]+)"\s*,\s*"([^"\n]+)"',
        source,
    ):
        result.setdefault(feature, []).append(
            {"id": identifier, "label": label, "topology": topology.get(identifier)}
        )
    if set(result) != {"pose", "jersey", "action", "team", "reid"}:
        raise ValueError("Native model catalog changed; review the model selections.")
    return result


def player_controls(root):
    source = (root / "src/apps/hstream-ui/PlayerAnalyticsControls.cpp").read_text()
    key = r'(?:(kAnalytics) \+ )?"([^"\n]+)"'
    pattern = (
        r"addFlag\(\s*"
        + key
        + r'\s*,\s*(?:kAnalytics \+ "[^"\n]+"|"[^"\n]*"|\{\})\s*,\s*"([^"\n]+)"\s*,\s*"([^"\n]+)"'
    )
    result = {}
    for prefix, suffix, identifier, label in re.findall(pattern, source):
        result[("pipeline.player-analytics." if prefix else "") + suffix] = {
            "qt_control_id": identifier,
            "label": label,
        }
    pattern = r"addModel\(\s*" + key + r'\s*,\s*"([^"\n]+)"\s*,\s*"([^"\n]+)"'
    for prefix, suffix, identifier, label in re.findall(pattern, source):
        result[("pipeline.player-analytics." if prefix else "") + suffix] = {
            "qt_control_id": identifier,
            "label": label,
        }
    for key, identifier, label in (
        ("game.home_is_dark", "playerHomeSide", "Home sweater"),
        (
            "pipeline.player-analytics.pose.rate-hz",
            "playerPoseRate",
            "Pose updates per second / fps",
        ),
        (
            "pipeline.player-analytics.jersey.roi-mode",
            "playerJerseyRoiMode",
            "Jersey crop",
        ),
    ):
        if f'setObjectName("{identifier}")' not in source:
            raise ValueError(
                f"Qt player control {identifier} changed; review its web mapping"
            )
        result[key] = {"qt_control_id": identifier, "label": label}
    return result


def qt_dependencies(root):
    source = (root / "src/apps/hstream-ui/HStreamWindow.cpp").read_text()
    return {
        identifier: {"control": control, "operator": operator, "value": int(value)}
        for identifier, control, operator, value in re.findall(
            r'enable_row\("([^"\n]+)", cameraControlValue\("([^"\n]+)"\) (==|!=|>) (\d+)\);',
            source,
        )
    }


def qt_layout(root):
    source = (root / "src/apps/hstream-ui/HStreamWindow.cpp").read_text()
    main = re.findall(r'preview_tabs_->addTab\([^\n]*, "([^"\n]+)"\);', source)
    camera_count = re.search(
        r"for \(int camera_index = 0; camera_index < (\d+);", source
    )
    camera_title = re.search(
        r'preview_tabs_->addTab\(camera, (?:tab_icon\([^\n]*?\), )?QString\("([^"\n]+)"\)',
        source,
    )
    rotation = source.index("control_tabs->addTab(add_slider_tab(rotation_controls")
    program = re.findall(
        r'control_tabs->addTab\([^\n]*, "([^"\n]+)"\);', source[:rotation]
    )
    stitched = re.findall(
        r'control_tabs->addTab\([^\n]*, "([^"\n]+)"\);', source[rotation:]
    )
    stitched.extend(re.findall(r'add_stitching_tab\("[^"\n]+", "([^"\n]+)"\)', source))
    if (
        len(main) != 3
        or not camera_count
        or not camera_title
        or not program
        or not stitched
    ):
        raise ValueError(
            "Qt workspace tab construction changed; review the browser layout contract."
        )
    main[2:2] = [
        camera_title[1].replace("%1", str(i + 1)) for i in range(int(camera_count[1]))
    ]
    icon_source = (root / "src/apps/hstream-ui/ActionIcons.h").read_text()
    tab_icons = {
        label: icon.lower()
        for label, icon in re.findall(
            r'TabIconSpec\{"([^"\n]+)", ActionIcon::(\w+)\}', icon_source
        )
    }
    if not set(main + program + stitched) <= tab_icons.keys():
        raise ValueError("Every native workspace tab must name its shared action icon.")
    return {
        "main_tabs": main,
        "stage_tabs": {"program": program, "stitched": stitched},
        "camera_tabs": {
            "controls": ["Optics"],
            "visibility": "selected-sources",
            "order": ["left", "right"],
            "panorama_label": "Panorama",
        },
        "tab_icons": tab_icons,
    }


def projection_parameter_specs(root):
    source = (root / "src/libs/stitching/StitchingAlgorithms.cpp").read_text()
    result = {}
    for projection, table in (
        ("albers-equal-area-conic", "Albers"),
        ("biplane", "Biplane"),
        ("triplane", "Triplane"),
        ("general-panini", "GeneralPanini"),
    ):
        block = re.search(rf"k{table}Parameters = \{{(.*?)\n\}};", source, re.DOTALL)
        if block is None:
            raise ValueError("Native projection parameter table changed")
        definitions = []
        for name, label, minimum, maximum, default, description in re.findall(
            r'\{\s*"([^"\n]+)",\s*"([^"\n]+)",\s*(-?[\d.]+),'
            r'\s*(-?[\d.]+),\s*(-?[\d.]+),\s*((?:"[^"\n]*"\s*)+)\}',
            block.group(1),
        ):
            definitions.append(
                {
                    "name": name,
                    "label": label,
                    "minimum": float(minimum),
                    "maximum": float(maximum),
                    "type": "boolean" if name == "corners" else "number",
                    "description": "".join(
                        json.loads(value)
                        for value in re.findall(r'"[^"\n]*"', description)
                    ),
                }
            )
        if not definitions:
            raise ValueError("Native projection parameters are unavailable")
        result[projection] = definitions
    return result


def stitching_control_policies(root):
    source = (root / "src/libs/stitching/StitchingAlgorithms.cpp").read_text()
    qt = (root / "src/apps/hstream-ui/HStreamWindow.cpp").read_text()
    projections = {
        enum: (name, label)
        for enum, name, label in re.findall(
            r'\{StitchProjection::(\w+),\s*"([^"]+)",\s*"([^"]+)",', source
        )
    }
    body = source.split(
        "absl::StatusOr<double> MaximumStitchProjectionHorizontalFov(", 1
    )[1].split("absl::Status ValidateStitchProjectionHorizontalFov", 1)[0]
    body = re.sub(r"//[^\n]*", "", body)
    fixed = {}
    for cases, maximum in re.findall(
        r"((?:\s*case StitchProjection::\w+:)+)\s*return ([\d.]+);", body
    ):
        for enum in re.findall(r"StitchProjection::(\w+)", cases):
            fixed[projections[enum][0]] = float(maximum)
    linear = {}
    for enum, ceiling, multiplier, offset in re.findall(
        r"case StitchProjection::(kBiplane|kTriplane):\s*return std::min\(([\d.]+),\s*(?:([\d.]+) \* )?parameters\[0\] \+ ([\d.]+)\);",
        body,
    ):
        linear[projections[enum][0]] = {
            "ceiling": float(ceiling),
            "multiplier": float(multiplier or 1),
            "offset": float(offset),
        }

    def numbers(pattern):
        match = re.search(pattern, body)
        if not match:
            raise ValueError("Native dynamic projection limit changed")
        return [float(item) for item in match.groups()]

    panini = {
        "angle": numbers(r"kMaximumProjectionAngle = ([\d.]+) \* kPi / 180.0;")[0],
        "scale": numbers(
            r"compression_scale = \(([\d.]+) - parameters\[0\]\) / ([\d.]+);"
        ),
        "compression": numbers(
            r"compression = ([\d.]+) / \(compression_scale \+ ([\d.]+)\) - ([\d.]+) / ([\d.]+);"
        ),
    }
    fresh = re.search(
        r"projection_fov_controls_projection_\.isEmpty\(\) \? default_projection_framing_\.horizontal_fov : ([\d.]+)",
        qt,
    )
    if not fresh or len(fixed) != 19 or len(linear) != 2:
        raise ValueError("Native projection FOV policy catalog changed")

    def choices(widget):
        found = re.findall(rf'{widget}->addItem\("([^"]+)", "([^"]+)"\);', qt)
        if not found:
            raise ValueError("Native stitching choices changed")
        return {value: label for label, value in found}

    return {
        "projection_labels": {name: label for name, label in projections.values()},
        "fov": {
            "fixed": fixed,
            "linear": linear,
            "general_panini": panini,
            "new_projection": float(fresh[1]),
        },
        "matcher_labels": choices("control_point_matcher_combo_"),
        "backend_labels": choices("mapping_backend_combo_"),
        "resolution_labels": choices("control_point_resolution_combo_"),
        "fixed_resolutions": dict(
            zip(
                ["akaze-hamming", "loftr", "dedode-lightglue"],
                re.search(
                    r'const QString size = matcher == "akaze-hamming" \? "([^"]+)"\s*: matcher == "loftr"\s*\? "([^"]+)"\s*: "([^"]+)";',
                    qt,
                ).groups(),
                strict=True,
            )
        ),
    }


def qt_location(key):
    if key.startswith("stitching."):
        if (
            key == "stitching.max_output_width"
            or "projection" in key
            or key.startswith(("stitching.camera_", "stitching.input_lens."))
        ):
            return "stitched", "Projection"
        if "rink_config" in key or "rink_mask" in key:
            return "stitched", "Rink"
        if "blend" in key:
            return "stitched", "Color & Precision"
        return "stitched", "Alignment"
    if key.startswith("camera.color."):
        return "stitched", "Color & Precision"
    if key.startswith(("pipeline.player-analytics.", "pipeline.tracker.reid")):
        return "program", "Players"
    if key.startswith(("team.", "plot.")) or key == "game.home_is_dark":
        return "program", "Players"
    if key.startswith("runtime."):
        return "program", "Runtime"
    if ".color." in key:
        return "program", "Color"
    if "breakaway_detection" in key or any(
        part in key for part in ("max_speed", "max_accel")
    ):
        return "program", "Motion"
    return "program", "Tracking"


def generate(root, *, native_revision=None, include_revision=True):
    # Release builders use git archives without .git; the caller supplies the
    # revision recorded by the immutable snapshot, never a nearby checkout.
    if not include_revision:
        native_revision = None
    elif native_revision is None:
        native_revision = subprocess.check_output(
            ["git", "-C", str(root), "rev-parse", "HEAD"], text=True
        ).strip()
    if include_revision and not re.fullmatch(r"[0-9a-f]{40}", native_revision):
        raise ValueError("Native revision must be a full Git commit SHA")
    baseline = yaml.safe_load((root / "configs/baseline.yaml").read_text())
    implicit_policies = re.findall(
        r'return auto_(?:fov|lens)\.value_or\(configuration == "([^"]+)"\);',
        (root / "src/libs/stitching/GameConfig.h").read_text(),
    )
    if len(implicit_policies) != 2 or len(set(implicit_policies)) != 1:
        raise ValueError(
            "Native implicit optics policies changed; review legacy inheritance"
        )
    fields = []
    specs = camera_specs(root)
    tones = tone_specs(root)
    rotations = rotation_specs(root)
    precision = precision_spec(root)
    models = model_catalog(root)
    players = player_controls(root)
    dependencies = qt_dependencies(root)
    by_key = {key: specs[identifier] for identifier, key in CAMERA_KEYS.items()}

    def value(key):
        node = baseline
        for part in key.split("."):
            node = node[part]
        return node

    def boolean_value(key):
        try:
            selected = value(key)
        except KeyError as error:
            raise ValueError(f"Missing required baseline value {key}.") from error
        if not isinstance(selected, bool):
            raise TypeError(f"{key} must be a boolean baseline value.")
        return selected

    def add(key, group, default=None, choices=None, minimum=0, maximum=100, label=None):
        if default is None:
            default = value(key)
        kind = (
            "choice"
            if choices
            else (
                "boolean"
                if type(default) is bool
                else "integer" if type(default) is int else "number"
            )
        )
        item = {
            "key": key,
            "group": group,
            "label": label
            or key.split(".")[-1].replace("_", " ").replace("-", " ").capitalize(),
            "type": kind,
            "default": default,
        }
        if choices:
            item["choices"] = choices
        elif kind != "boolean":
            item.update(minimum=minimum, maximum=maximum)
        stage, tab = qt_location(key)
        item.update(qt_stage=stage, qt_tab=tab)
        if key in by_key:
            spec = by_key[key]
            item.update(
                label=spec["label"],
                qt_control_id=spec["id"],
                runtime_key=spec["runtime_key"],
                divisor=spec["divisor"],
                qt_order=list(specs).index(spec["id"]) + 3,
                qt_slider=not (spec["minimum"] == 0 and spec["maximum"] == 1),
            )
            if kind != "boolean":
                item.update(
                    minimum=spec["minimum"] / spec["divisor"],
                    maximum=spec["maximum"] / spec["divisor"],
                    step=1 / spec["divisor"],
                )
        fields.append(item)

    framing = (root / "src/apps/hstream-ui/RinkFramingControls.cpp").read_text()

    def framing_array(name):
        match = re.search(rf"{name}\[\]\s*\{{(.*?)\}};", framing, re.DOTALL)
        if not match:
            raise ValueError("Native rink framing control catalog changed")
        return re.findall(r'"([^"\n]+)"', match.group(1))

    keys, labels, names = (framing_array(name) for name in ("keys", "labels", "names"))
    if len(keys) != 4 or len(labels) != 4 or len(names) != 4:
        raise ValueError("Expected the four native rink framing choices")
    for order, (key, label, identifier) in enumerate(zip(keys, labels, names)):
        full_key = "pipeline.ds-playtracker.private-properties." + key
        default = value(full_key)
        if type(default) is not int or default not in (0, 1):
            raise ValueError("Expected canonical 0/1 rink framing baseline")
        add(full_key, "Rink framing", default=bool(default), label=label)
        fields[-1].update(
            qt_stage="program",
            qt_tab="Rink framing",
            qt_control_id=identifier,
            qt_order=order,
            rink_framing_control=True,
        )

    mini_map_source = (root / "src/apps/hstream-ui/HStreamWindow.cpp").read_text()
    size_section = mini_map_source.split(
        'rink_map_size_combo_->setObjectName("rinkMiniMapSizeCombo");', 1
    )[1].split("rink_map_position_combo_ =", 1)[0]
    sizes = {
        value: label
        for label, value in re.findall(
            r'(?:pair)?\{"([^"]+)", "([^"]+)"\}', size_section
        )
    }
    positions = {
        value: label
        for label, value in re.findall(
            r'rink_map_position_combo_->addItem\("([^"]+)", "([^"]+)"\)',
            mini_map_source,
        )
    }
    if set(sizes) != {"icon", "compact", "standard", "large"} or set(positions) != {
        "upper-right",
        "upper-left",
        "lower-left",
    }:
        raise ValueError("Native mini-map choices changed; review the shared controls")
    preview_rink_map = dict(value("hstream_ui.preview_rink_map"))
    if (
        set(preview_rink_map) != {"enable", "size", "position"}
        or type(preview_rink_map["enable"]) is not bool
        or preview_rink_map["size"] not in sizes
        or preview_rink_map["position"] not in positions
    ):
        raise ValueError("Invalid required preview mini-map baseline")
    for key, label, identifier, choices in (
        (
            "enable",
            "Include rink mini-map in encoded outputs",
            "outputRinkMapCheck",
            None,
        ),
        ("size", "Output map size", "outputRinkMapSizeCombo", sizes),
        ("position", "Output map position", "outputRinkMapPositionCombo", positions),
    ):
        add(
            "output.rink-map." + key,
            "Rink mini-map",
            choices=list(choices) if choices else None,
            label=label,
        )
        fields[-1].update(
            qt_stage="stitched",
            qt_tab="Rink",
            qt_control_id=identifier,
            qt_order=1100 + len(fields),
        )
        if choices:
            fields[-1]["choice_labels"] = choices

    add(
        "rink.scoreboard.position",
        "Program scoreboard",
        choices=["upper-left", "upper-center", "upper-right"],
        label="Scoreboard position",
    )
    fields[-1].update(
        qt_control_id="scoreboardPositionCombo",
        qt_stage="program",
        qt_tab="Runtime",
        choice_labels={
            "upper-left": "Upper left",
            "upper-center": "Upper center",
            "upper-right": "Upper right",
        },
    )

    for key, choices in {
        "control_point_matcher": [
            "akaze-hamming",
            "superpoint-lightglue",
            "dedode-lightglue",
            "loftr",
        ],
        "control_point_resolution": ["auto", "native", "1k", "2k"],
        "mapping_backend": ["nona", "opencv-magsac", "opencv-affine-ransac"],
        "projection": re.findall(
            r'\{StitchProjection::\w+,\s*"([^"]+)"',
            (root / "src/libs/stitching/StitchingAlgorithms.cpp").read_text(),
        ),
        "camera_config": list(baseline["stitching"]["camera_configs"]),
    }.items():
        add("stitching." + key, "Stitching and cameras", choices=choices)
    rink_config = value("stitching.rink_config")
    add(
        "stitching.rink_config",
        "Stitching and cameras",
        "" if rink_config is None else rink_config,
        ["", *baseline["stitching"]["rink_configs"]],
    )
    add(
        "stitching.control_point_limit",
        "Stitching and cameras",
        label="Control points",
        minimum=10,
        maximum=5000,
    )
    add(
        "stitching.calibration_frame_count",
        "Stitching and cameras",
        minimum=1,
        maximum=16,
    )
    add("stitching.run_autooptimizer", "Stitching and cameras")
    for key, label in (
        ("epipolar_ransac", "Epipolar RANSAC"),
        ("homography_consistency", "Homography consistency"),
    ):
        add(
            "stitching.control_point_filtering." + key,
            "Stitching and cameras",
            label=label,
        )
    optics = baseline["stitching"]["camera_configs"]["manual"]
    for axis, maximum in (("horizontal", 359.999), ("vertical", 180)):
        add(
            f"stitching.camera_fov.{axis}_fov",
            "Manual camera optics",
            float(optics[f"{axis}_fov"]),
            minimum=0.001,
            maximum=maximum,
            label=f"Camera {axis} field of view (degrees)",
        )
    for key, choices in (
        ("projection", ["rectilinear", "full-frame-fisheye"]),
        ("optimization", ["auto", "refine", "fixed"]),
    ):
        add(
            f"stitching.input_lens.{key}",
            "Manual camera optics",
            optics["input_lens"][key],
            choices,
        )
    for key, limit, label in (
        ("distortion", 1, "Lens distortion (a, b, c)"),
        ("center_offset", 0.5, "Optical center offsets (fractions of width, height)"),
    ):
        values = optics["input_lens"][key]
        fields.append(
            {
                "key": f"stitching.input_lens.{key}",
                "group": "Manual camera optics",
                "label": label,
                "type": "vector",
                "default": values,
                "minimum": [-limit] * len(values),
                "maximum": [limit] * len(values),
            }
        )
    add(
        "stitching.blend_feather_fraction",
        "Stitching and cameras",
        label="Feather width",
        minimum=0,
        maximum=1,
    )
    add("stitching.max_blend_levels", "Stitching and cameras", minimum=1, maximum=16)
    add("stitching.projection_framing.auto_fov", "Projection")
    add(
        "stitching.projection_framing.horizontal_fov",
        "Projection",
        minimum=0.01,
        maximum=360,
    )
    fields[-1]["type"] = "number"
    add("stitching.projection_framing.auto_canvas", "Projection")
    add("stitching.projection_framing.auto_crop", "Projection")
    width = value("stitching.max_output_width")
    add(
        "stitching.max_output_width",
        "Projection",
        0 if width is None else int(width),
        minimum=0,
        maximum=2147483647,
        label="Max stitched width",
    )
    projection_specs = projection_parameter_specs(root)
    for projection, values in baseline["stitching"]["projection_parameters"].items():
        parameters = projection_specs[projection]
        if len(parameters) != len(values):
            raise ValueError("Native projection defaults and parameters differ")
        fields.append(
            {
                "key": f"stitching.projection_parameters.{projection}",
                "group": "Projection",
                "label": projection.replace("-", " ").title() + " parameters",
                "type": "vector",
                "default": values,
                "minimum": [item["minimum"] for item in parameters],
                "maximum": [item["maximum"] for item in parameters],
                "components": parameters,
            }
        )
    add(
        "stitching.sync_method",
        "Stitching and cameras",
        choices=["auto", "audio", "imu"],
    )

    def scalars(node, prefix, group):
        for name, val in node.items():
            key = prefix + "." + name
            if key in {
                "rink.camera.zoom_in_aggressiveness",
                "rink.camera.follower_box_scale_width",
                "rink.camera.follower_box_scale_height",
                "rink.camera.fixed_edge_rotation_angle",
            }:
                continue
            if isinstance(val, dict):
                scalars(val, key, group)
            elif type(val) in (bool, int, float):
                low, high = (-10, 10) if "exposure" in name else (0, 200)
                add(key, group, minimum=low, maximum=high)

    for identifier in (
        "Apply_To_Fast_Box",
        "Apply_To_Follower_Box",
        "Max_Speed_X_x10",
        "Max_Speed_Y_x10",
        "Max_Accel_X_x10",
        "Max_Accel_Y_x10",
    ):
        spec = specs[identifier]
        add(
            "hstream_ui.camera_controls." + identifier,
            "Tracking and motion",
            minimum=spec["minimum"],
            maximum=spec["maximum"],
            label=spec["label"],
        )
        fields[-1].update(
            qt_control_id=identifier,
            qt_stage="program",
            qt_tab="Motion" if identifier.startswith("Max_") else "Tracking",
            qt_order=list(specs).index(identifier) + 3,
            qt_slider=spec["maximum"] != 1,
        )
        if spec["maximum"] == 1:
            fields[-1]["qt_toggle"] = True

    scalars(baseline["rink"]["camera"], "rink.camera", "Tracking and motion")
    scalars(baseline["rink"]["tracking"], "rink.tracking", "Tracking and motion")
    scalars(baseline["plot"], "plot", "Overlays")
    scalars(baseline["camera"]["color"], "camera.color", "Input color")
    fields.append(
        {
            "key": "rink.camera.fixed_edge_rotation_angle",
            "group": "Rotation",
            "label": "Fixed-edge rotation",
            "type": "rotation",
            "default": value("rink.camera.fixed_edge_rotation_angle"),
            "composite_control": True,
            "qt_stage": "program",
            "qt_tab": "Crop Rotation",
        }
    )
    add(
        "runtime.gpu_memory_profile",
        "GPU and analytics",
        value("runtime.gpu_memory_profile"),
        ["auto", "standard", "low"],
        label="GPU memory",
    )
    runtime_source = (root / "src/apps/hstream-ui/HStreamWindow.cpp").read_text()
    labels = {
        value: label
        for label, value in re.findall(
            r'gpu_memory_profile_combo_->addItem\("([^"\n]+)", "([^"\n]+)"\)',
            runtime_source,
        )
    }
    if set(labels) != {"standard", "low"}:
        raise ValueError("Qt GPU memory choices changed; review session semantics")
    fields[-1].update(
        session_control=True,
        qt_stage="program",
        qt_tab="Runtime",
        qt_control_id="gpuMemoryProfileCombo",
        choice_labels=labels,
    )
    for feature in ("pose", "jersey", "action"):
        add(
            f"pipeline.player-analytics.{feature}.enable",
            "GPU and analytics",
            False,
            label={
                "pose": "Estimate pose",
                "jersey": "Read jersey numbers",
                "action": "Recognize activities",
            }[feature],
        )
    add(
        "pipeline.player-analytics.jersey.roi-mode",
        "GPU and analytics",
        "bbox",
        ["bbox", "pose"],
    )
    add(
        "pipeline.tracker.reid-enable",
        "GPU and analytics",
        False,
        label="Appearance matching",
    )
    for feature, selections in models.items():
        key = (
            "pipeline.tracker.reid-model"
            if feature == "reid"
            else f"pipeline.player-analytics.{feature}.model"
        )
        add(
            key,
            "GPU and analytics",
            selections[0]["id"],
            [model["id"] for model in selections] + ["custom"],
            label={"reid": "Appearance", "action": "Activity"}.get(
                feature, feature.capitalize()
            )
            + " model",
        )
        fields[-1]["choice_labels"] = {
            **{model["id"]: model["label"] for model in selections},
            "custom": "Custom…",
        }
    fields.append(
        {
            "key": "game.home_is_dark",
            "group": "GPU and analytics",
            "label": "Home sweater",
            "type": "nullable_boolean",
            "default": value("game.home_is_dark"),
            "choices": ["", "true", "false"],
            "choice_labels": {
                "": "Not set (badges read DARK and LIGHT)",
                "true": "Home wears dark",
                "false": "Home wears light",
            },
        }
    )
    fields.append(
        {
            "key": "pipeline.player-analytics.pose.rate-hz",
            "group": "GPU and analytics",
            "label": "Pose updates per second / fps",
            "type": "rate",
            "default": value("pipeline.player-analytics.pose.rate-hz"),
            "minimum": 0,
            "maximum": 240,
        }
    )
    add("team.enabled", "GPU and analytics", label="Label home and away")
    add(
        "pipeline.player-analytics.pose.continuous-drawing",
        "GPU and analytics",
        label="Keep pose visible between updates",
    )
    for spec in tones:
        key = "hstream_ui.camera_controls." + spec["id"]
        add(
            key,
            "Tone controls",
            minimum=spec["minimum"],
            maximum=spec["maximum"],
            label=spec["label"],
        )
        fields[-1].update(
            qt_control_id=spec["id"],
            qt_stage="program",
            qt_tab="Color",
            qt_order={
                "Bring_Up_Shadows": 0,
                "Exposure_x100": 1,
                "Lift_Shadow_Black_Point": 2,
            }[spec["id"]],
            qt_slider=spec["maximum"] != 1,
            qt_toggle=spec["maximum"] == 1,
            tone_control=True,
            qt_mirror_order={
                "Bring_Up_Shadows": 0,
                "Lift_Shadow_Black_Point": 1,
                "Exposure_x100": 2,
            }[spec["id"]],
        )
        if spec["maximum"] == 1:
            fields[-1]["enabled_when"] = {
                "control": "Bring_Up_Shadows",
                "operator": ">",
                "value": 0,
            }
    precision_key = "pipeline.hmstitcher.properties.high-bit-depth"
    precision_default = str(value(precision_key)).strip().lower()
    precision_default = {"true": "1", "false": "0"}.get(
        precision_default, precision_default
    )
    if precision_default not in precision["choices"]:
        raise ValueError("Baseline precision must be auto, true, false, 1, or 0")
    add(
        precision_key,
        "Tone controls",
        default=precision_default,
        choices=list(precision["choices"]),
        label=precision["label"],
    )
    fields[-1].update(
        qt_control_id="Use_10_Bit_Grading",
        qt_stage="program",
        qt_tab="Color",
        qt_order=3,
        qt_mirror_order=3,
        qt_tristate=True,
        choice_labels=precision["choices"],
    )
    tracking_extras = {
        "rink.tracking.cam_ignore_largest_count": (
            "Ignore_Largest_Count",
            "Ignore largest players (0 = off)",
            0,
            2147483647,
        ),
        "rink.tracking.cam_ignore_oversized": (
            "Ignore_Oversized_Players",
            "Ignore oversized players",
            1,
            None,
        ),
        "rink.tracking.cam_oversized_percent": (
            "Oversized_Player_Percent",
            "Larger than other players' average (%)",
            2,
            1.7976931348623157e308,
        ),
    }
    # Vector fields have the same location metadata as scalar controls.
    for field in fields:
        if field["key"] in players:
            field.update(players[field["key"]])
        if field["key"] in tracking_extras:
            identifier, label, order, maximum = tracking_extras[field["key"]]
            field.update(qt_control_id=identifier, label=label, qt_order=order)
            if maximum is not None:
                field.update(minimum=0, maximum=maximum)
            if identifier == "Oversized_Player_Percent":
                field["type"] = "number"
            if identifier == "Ignore_Largest_Count" and not value(
                "rink.tracking.cam_ignore_largest"
            ):
                field["default"] = 0
        if "qt_stage" not in field:
            field["qt_stage"], field["qt_tab"] = qt_location(field["key"])
        if field.get("qt_control_id") in dependencies:
            field["enabled_when"] = dependencies[field["qt_control_id"]]
    sources = [
        "configs/baseline.yaml",
        "src/apps/hstream-ui/CameraControlSpecs.h",
        "src/apps/hstream-ui/RinkFramingControls.cpp",
        "src/libs/tracking/CameraControlSpecs.h",
        "src/libs/program/ToneControls.h",
        "src/libs/program/ToneControls.cpp",
        "src/libs/program/RotationControls.h",
        "src/libs/program/RotationControls.cpp",
        "src/apps/hstream-ui/PlayerAnalyticsControls.cpp",
        "src/libs/stitching/StitchingAlgorithms.cpp",
        "src/libs/stitching/InputLens.cpp",
        "src/libs/stitching/ControlPointMatcher.cpp",
        "src/libs/stitching/GameConfig.cpp",
        "src/libs/stitching/GameConfig.h",
        "src/libs/player_analytics/ModelCatalog.cpp",
        "src/libs/player_analytics/ModelCatalogData.inc",
        "src/libs/player_analytics/Config.cpp",
        "src/libs/player_analytics/Types.h",
        "src/libs/player_analytics/PreparedModelValidation.cpp",
    ]

    def time_seconds(key):
        text = value(key)
        if not isinstance(text, str) or not re.fullmatch(
            r"(?:[01][0-9]|2[0-3]):[0-5][0-9]:[0-5][0-9](?:\.[0-9]{3})?", text
        ):
            raise ValueError(f"{key} must be HH:MM:SS or HH:MM:SS.mmm.")
        selected = time.fromisoformat(text)
        if selected.microsecond % 1000 or selected.tzinfo is not None:
            raise ValueError(f"{key} must match Qt's millisecond time editor.")
        milliseconds = (
            (selected.hour * 60 + selected.minute) * 60 + selected.second
        ) * 1000 + selected.microsecond // 1000
        return milliseconds / 1000

    detectors, detector_sources = detector_catalog(root, baseline)
    sources.extend(detector_sources)
    highlight_base_name = value("hstream_ui.highlights.base_name")
    if not isinstance(highlight_base_name, str) or not re.fullmatch(
        r"[A-Za-z0-9][A-Za-z0-9 _-]{0,63}", highlight_base_name
    ):
        raise ValueError(
            "Invalid native hstream_ui.highlights.base_name baseline value"
        )
    int8_samples = value("hstream_ui.int8_preparation.samples")
    int8_source = "src/apps/hstream-ui/Int8PreparationDialog.cpp"
    sources.extend([int8_source, "src/apps/hstream-ui/Int8PreparationDialog.h"])
    bounds = re.search(
        r"count_->setRange\((\d+), (\d+)\)", (root / int8_source).read_text()
    )
    if bounds is None:
        raise ValueError("Native INT8 preparation bounds are unavailable")
    minimum, maximum = map(int, bounds.groups())
    if type(int8_samples) is not int or not minimum <= int8_samples <= maximum:
        raise ValueError(
            "Invalid native hstream_ui.int8_preparation.samples baseline value"
        )
    return {
        "int8_preparation": {
            "samples": int8_samples,
            "minimum": minimum,
            "maximum": maximum,
            "play_when_ready": boolean_value(
                "hstream_ui.int8_preparation.play_when_ready"
            ),
        },
        "version": 1,
        "native_revision": native_revision,
        "sources": {
            name: hashlib.sha256((root / name).read_bytes()).hexdigest()
            for name in sources
        },
        "fields": fields,
        "basic_defaults": {
            "network_routes": value("hstream_ui.network_routes"),
            "archive_routes": value("hstream_ui.archive_routes"),
            "export_archive_routes": value("hstream_ui.export_archive_routes"),
            "start_seconds": time_seconds("hstream_ui.playback_start_time"),
            "calibration_seconds": time_seconds("stitching.stitch_frame_time"),
            "crop": boolean_value("hstream_ui.show_crop_dialog"),
            "leveling": boolean_value("hstream_ui.show_leveling_dialog"),
            "scoreboard": boolean_value("hstream_ui.show_scoreboard_dialog"),
            "playback_realtime": value("hstream_ui.playback_realtime"),
            "blend": value("stitching.blend_mode"),
            "rotation": value("stitching.post_stitch_rotate_degrees"),
            "zoom": value("rink.camera.zoom_in_aggressiveness"),
            "frame_scale": value("rink.camera.follower_box_scale_width"),
            "telemetry": value("hstream_ui.drivegpt_database.enabled"),
        },
        "camera_controls": list(specs.values()),
        "tone_controls": tones,
        "rotation_controls": rotations,
        "calibration_stages": calibration_stages(root),
        "highlight_defaults": {"base_name": highlight_base_name},
        "stitching_experiments": baseline["hstream_ui"]["stitching_experiments"],
        "stitching_camera_profiles": baseline["stitching"]["camera_configs"],
        "stitching_input_cameras": baseline["stitching"]["input_cameras"],
        "stitching_control_policies": stitching_control_policies(root),
        "stitching_camera_automation": {
            **baseline["stitching"]["camera_automation"],
            "implicit_automatic_configuration": implicit_policies[0],
        },
        "camera_experiments": baseline["camera_experiments"],
        "camera_experiment_controls": [
            {
                **item,
                "group": (
                    "Players"
                    if item["id"].startswith(("Ignore_", "Oversized_"))
                    else (
                        "Motion"
                        if item["id"].startswith(
                            ("Max_", "Overshoot_", "Post_Nonstop_")
                        )
                        else "Tracking"
                    )
                ),
                "default": baseline["camera_experiments"]["controls"][item["id"]],
                "maximum": (
                    float.fromhex("0x1.fffffffffffffp+1023")
                    if item["id"] == "Oversized_Player_Percent"
                    else item["maximum"]
                ),
            }
            for item in specs.values()
            if item["runtime_key"]
        ],
        "runtime_controls": baseline["hstream_ui"]["runtime_controls"],
        "preview_overlays": baseline["preview_overlays"],
        "preview_rink_map": {
            "defaults": preview_rink_map,
            "sizes": sizes,
            "positions": positions,
        },
        "source_sets": source_set_contract(root),
        "models": models,
        "detector": detectors,
        "prepared_models": {
            "protocol": "prepared-model-validation-v1",
            "minimum_batch": int(
                re.search(
                    r"kMaximumBatch = (\d+)",
                    (root / "src/libs/player_analytics/Types.h").read_text(),
                ).group(1)
            ),
        },
        "qt_layout": qt_layout(root),
        "qt_dependencies": dependencies,
        "qt_sources": {
            name: hashlib.sha256((root / name).read_bytes()).hexdigest()
            for name in (
                "src/apps/hstream-ui/HStreamWindow.cpp",
                "src/apps/hstream-ui/CameraOpticsWidget.cpp",
                "src/apps/hstream-ui/CameraOpticsWidget.h",
                "src/apps/hstream-ui/ValueControlWheel.cpp",
                "src/apps/hstream-ui/ValueControlWheel.h",
                "src/apps/hstream-ui/main.cpp",
                "src/apps/hstream-ui/DetectorOptions.cpp",
                "src/apps/hstream-ui/DetectorOptions.h",
                "src/apps/hstream-ui/RuntimeOptions.cpp",
                "src/apps/hstream-ui/RuntimeOptions.h",
                "src/apps/hstream-ui/CameraControlSpecs.h",
                "src/apps/hstream-ui/RinkFramingControls.cpp",
                "src/libs/tracking/CameraControlSpecs.h",
                "src/libs/program/ToneControls.h",
                "src/libs/program/ToneControls.cpp",
                "src/libs/program/RotationControls.h",
                "src/libs/program/RotationControls.cpp",
                "src/apps/hstream-ui/PlayerAnalyticsControls.cpp",
                "src/apps/hstream-ui/RinkFramingControls.cpp",
                "src/apps/hstream-ui/PipelineInspectorWidget.cpp",
                "src/apps/hstream-ui/HighlightsDialog.cpp",
                "src/apps/hstream-ui/HighlightItemEditor.cpp",
                "src/apps/hstream-ui/StitchingExperimentDialog.cpp",
                "src/apps/hstream-ui/BackendOptions.cpp",
                "src/apps/hstream-ui/ActionIcons.cpp",
                "src/apps/hstream-ui/ActionIcons.h",
            )
        },
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hstream-root", type=Path, required=True)
    parser.add_argument("--check", action="store_true")
    parser.add_argument(
        "--output", type=Path, help="Destination JSON (required by native builds)"
    )
    parser.add_argument(
        "--native-revision", help="Full source revision for an archive without .git"
    )
    parser.add_argument(
        "--config-checksums",
        type=Path,
        help="Write sha256sum checks for the installed configs",
    )
    args = parser.parse_args()
    if args.check and args.config_checksums:
        parser.error("--config-checksums is only available when generating")
    destination = args.output or (
        Path(__file__).resolve().parents[1] / "django_app/stream/native_contract.json"
    )
    document = generate(args.hstream_root, native_revision=args.native_revision)
    if args.check:
        previous = json.loads(destination.read_text())
        # Unrelated native commits do not invalidate the control contract.
        document["native_revision"] = previous["native_revision"]
        if previous != document:
            raise SystemExit(
                "Native startup contract changed. Regenerate and review the controls."
            )
    else:
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(json.dumps(document, indent=2) + "\n")
        if args.config_checksums:
            args.config_checksums.parent.mkdir(parents=True, exist_ok=True)
            args.config_checksums.write_text(
                "".join(
                    f"{digest}  {name}\n"
                    for name, digest in sorted(document["sources"].items())
                    if name.startswith("configs/")
                )
            )


if __name__ == "__main__":
    main()
