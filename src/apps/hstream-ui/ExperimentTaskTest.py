#!/usr/bin/env python3
"""Exercise the real native task/admission protocol without GPU allocation."""

import tempfile, subprocess, json, os, pathlib, hashlib, shutil, sys

temporary = tempfile.TemporaryDirectory(prefix="hstream-native-task-")
root = pathlib.Path(temporary.name)
game = root / "game"
(game / "_hockeymon").mkdir(parents=True)
# A fresh Auto game has not saved Left/Right roles yet. Each workflow successor
# must retain its discoverable chapters before orientation can establish them.
media = ["GX010106.MP4", "GX020106.MP4", "GX010107.MP4", "GX020107.MP4"]
for name in media:
    (game / name).write_text(name + " fixture")
(game / "_hockeymon/config.yaml").write_text(
    "game:\n  stitching:\n    frame_offsets: {left: 0, right: 0}\n"
)
workflow = "workflow-native-test"
directory = game / "_hockeymon/workflows" / workflow
directory.mkdir(parents=True)
binary = pathlib.Path(sys.argv[1]).resolve()
capabilities = json.loads(
    subprocess.check_output([str(binary), "--capabilities"], text=True)
)
expected_backend = sys.argv[2]
assert capabilities["gpu_backend"] == expected_backend
assert capabilities["gpu_operations_supported"] == (expected_backend == "nvidia")


def call(action, data, expected=0):
    p = root / (action + ".json")
    p.write_text(json.dumps(data))
    r = subprocess.run(
        [str(binary), "--admission", action, "--request", str(p)],
        capture_output=True,
        text=True,
        timeout=30,
    )
    assert r.returncode == expected, (r.returncode, r.stdout, r.stderr)
    return json.loads(r.stdout)


owner = call(
    "acquire",
    dict(
        game=str(game),
        kind="workflow",
        id=workflow,
        token="native-test-token",
        workflow_directory=str(directory),
    ),
)
assert (
    call(
        "acquire",
        dict(
            game=str(game),
            kind="workflow",
            id=workflow,
            token="native-test-token",
            workflow_directory=str(directory),
        ),
    )
    == owner
)
assert (
    call(
        "acquire",
        dict(
            game=str(game),
            kind="workflow",
            id=workflow,
            token="different-token",
            workflow_directory=str(directory),
        ),
        1,
    )["code"]
    == "conflict"
)
attempt = directory / "attempts/attempt-create"
attempt.mkdir(parents=True)
request = dict(
    schema=1,
    workflow_id=workflow,
    plan_digest="f" * 64,
    stage_id="prepare",
    attempt_id=attempt.name,
    epoch=1,
    operation="experiment-create",
    root=str(attempt),
    game=str(game),
    inputs=dict(
        settings=dict(control_points=30, frame_count=1, stitch_frame_time="00:00:00"),
        sequence=1,
    ),
    runtime=dict(storage_namespace="test-storage"),
    result_path=str(attempt / "native-result.json"),
    owner=dict(game=str(game), token="native-test-token"),
)
p = attempt / "request.json"
p.write_text(json.dumps(request))
r = subprocess.run(
    [str(binary), "--request", str(p)], capture_output=True, text=True, timeout=30
)
assert r.returncode == 0, (r.stdout, r.stderr)
result = json.loads((attempt / "native-result.json").read_text())
assert result["status"] == "succeeded"
for art in result["artifacts"]:
    data = (attempt / art["path"]).read_bytes()
    assert art["sha256"] == hashlib.sha256(data).hexdigest() and art["size"] == len(
        data
    )
assert (game / "_hockeymon/config.yaml").read_text().startswith("game:")
assert not result["outputs"]["materialized"]
for name in media:
    alias = pathlib.Path(result["outputs"]["workspace"]["game_directory"]) / name
    assert alias.is_symlink() and alias.resolve() == game / name
if expected_backend == "amd":
    unsupported = directory / "attempts/unsupported-gpu"
    unsupported.mkdir()
    gpu_request = dict(
        request,
        operation="experiment-materialize",
        root=str(unsupported),
        attempt_id=unsupported.name,
        result_path=str(unsupported / "native-result.json"),
        inputs={"workspace": result["outputs"]["workspace"]},
    )
    p = unsupported / "request.json"
    p.write_text(json.dumps(gpu_request))
    rejected = subprocess.run(
        [str(binary), "--request", str(p)],
        capture_output=True,
        text=True,
        timeout=30,
        env=dict(os.environ, CUDA_VISIBLE_DEVICES="0"),
    )
    assert (
        rejected.returncode != 0 and "AMD automatic calibration" in rejected.stdout
    ), (rejected.stdout, rejected.stderr)
    assert not (unsupported / "native-result.json").exists()
# A prepare successor must retain identity while owning separate mutable files.
second = directory / "attempts/attempt-snapshot"
second.mkdir(parents=True)
request.update(
    root=str(second),
    attempt_id=second.name,
    stage_id="snapshot",
    result_path=str(second / "native-result.json"),
    inputs={"workspace": result["outputs"]["workspace"]},
)
p = second / "request.json"
p.write_text(json.dumps(request))
r = subprocess.run(
    [str(binary), "--request", str(p)], capture_output=True, text=True, timeout=30
)
assert r.returncode == 0, (r.stdout, r.stderr)
copy = json.loads((second / "native-result.json").read_text())["outputs"]["workspace"]
assert copy["invalidation_id"] == result["outputs"]["workspace"]["invalidation_id"]
assert copy["game_directory"] != result["outputs"]["workspace"]["game_directory"]
for name in media:
    alias = pathlib.Path(copy["game_directory"]) / name
    assert alias.is_symlink() and alias.resolve() == game / name
original = (
    pathlib.Path(result["outputs"]["workspace"]["game_directory"])
    / "_hockeymon/config.yaml"
)
original_bytes = original.read_bytes()
(pathlib.Path(copy["game_directory"]) / "_hockeymon/config.yaml").write_text(
    "successor: modified\n"
)
assert original.read_bytes() == original_bytes
# A descriptor cannot authorize a sibling workflow, path traversal or stale owner.
request["inputs"]["workspace"]["game_directory"] = str(game)
p.write_text(json.dumps(request))
r = subprocess.run(
    [str(binary), "--request", str(p)], capture_output=True, text=True, timeout=30
)
assert r.returncode != 0

call("cancel", dict(game=str(game), token="native-test-token", revision=1))

# Catalog commit recovery only reads stored identity. Missing output files must
# not force another adoption or prevent a later Stop from reconciling it.
store = game / "_hockeymon/stitching-experiments"
catalog_key = "sessions/status-fixture/candidate"
(store / catalog_key).mkdir(parents=True)
(store / "owner.yaml").write_text(json.dumps(dict(version=1, game_directory=str(game))))
missing = directory / "attempts/accepted-before-response-loss/work/missing-candidate"
descriptor = dict(
    root=str(missing.parent),
    game_directory=str(missing),
    game_id=missing.name,
    invalidation_id="accepted-generation",
    settings=dict(control_points=30, frame_count=1, stitch_frame_time="00:00:00"),
)
row = dict(
    key=catalog_key,
    owner="catalog-generation",
    settings=dict(control_points=30, frame_count=1, reference="00:00:00"),
    state="queued",
    selection="",
    artifact_generation="",
    process_session=0,
    process_token="",
    reservation_token="",
    workflow_id=workflow,
    workflow_active=True,
    workflow_directory=str(directory),
    sequence=1,
    baseline_sequence=0,
    selection_owner_sequence=0,
    baseline_workspace_key="",
    selection_owner_workspace_key="",
    saved_selection="",
    scan_duration_seconds=60,
    requires_ice_mask=False,
    failure="",
    revision=1,
)
index = dict(version=1, experiments=[row], selected_by_count={}, reservations={})
query = dict(
    game=str(game),
    token="native-test-token",
    workflow_id=workflow,
    workflow_directory=str(directory),
    attempt_id="accepted-before-response-loss",
    catalog_key=catalog_key,
    workspace=descriptor,
)
for action, field in (("adopt", "result"), ("adopt-selection", "selection")):
    query["adoption"] = action
    (store / "index.yaml").write_text(json.dumps(index))
    assert call("adoption-status", query) == {"adopted": False}
    row[field + "_attempt_id"] = query["attempt_id"]
    row[field + "_workspace"] = dict(
        game_directory=descriptor["game_directory"], owner=descriptor["invalidation_id"]
    )
    (store / "index.yaml").write_text(json.dumps(index))
    before = (store / "index.yaml").stat()
    assert call("adoption-status", query) == {"adopted": True}
    after = (store / "index.yaml").stat()
    assert (before.st_ino, before.st_mtime_ns, before.st_ctime_ns) == (
        after.st_ino,
        after.st_mtime_ns,
        after.st_ctime_ns,
    )
    assert (
        "different workflow result"
        in call("adoption-status", dict(query, attempt_id="different-attempt"), 1)[
            "error"
        ]
    )
    changed = dict(descriptor, invalidation_id="different-generation")
    assert (
        "different workflow result"
        in call("adoption-status", dict(query, workspace=changed), 1)["error"]
    )
assert not missing.exists()
shutil.rmtree(store)
call(
    "release",
    dict(game=str(game), token="native-test-token", revision=2, stopped_and_clean=True),
)
assert call("inspect", dict(game=str(game)))["owner"] is None
print(
    "Admission conflict/idempotency, CPU prepare, portable artifacts and release passed"
)
