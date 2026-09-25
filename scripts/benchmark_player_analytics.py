#!/usr/bin/env python3
"""Alternate explicitly specified player-analytics runs; retain logs and raw metrics.

Use private, completed game artifacts. Each JSON case supplies an argv list, cwd,
and optional environment overrides. No shell is invoked. Engine construction,
calibration and profiler runs must be completed separately before measuring.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import signal
import statistics
import subprocess
import time


def query(command):
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=3)
        return result.stdout.strip() if result.returncode == 0 else None
    except (OSError, subprocess.TimeoutExpired):
        return None


def process_ids(pid, proc_root=Path('/proc')):
    pending, seen = [pid], set()
    parent_children = None
    while pending and len(seen) < 128:
        current = pending.pop()
        if current in seen:
            continue
        seen.add(current)
        try:
            children = (proc_root / str(current) / 'task' / str(current) / 'children').read_text().split()
            pending.extend(int(child) for child in children)
        except (OSError, ValueError):
            # Some Jetson kernels omit task/children. Build one PPID index for
            # this sample, handling spaces and parentheses inside stat's comm.
            if parent_children is None:
                parent_children = {}
                for path in proc_root.iterdir():
                    if not path.name.isdigit():
                        continue
                    try:
                        fields = (path / 'stat').read_text().rpartition(') ')[2].split()
                        parent_children.setdefault(int(fields[1]), []).append(int(path.name))
                    except (OSError, ValueError, IndexError):
                        pass
            pending.extend(parent_children.get(current, []))
    return seen


def memory(pid, smi):
    pids = process_ids(pid)
    rss = 0
    for process in pids:
        try:
            match = re.search(r'^VmRSS:\s+(\d+) kB$', Path(f'/proc/{process}/status').read_text(), re.M)
            if match:
                rss += int(match[1])
        except OSError:
            pass
    gpu = None
    if smi:
        output = query([smi, '--query-compute-apps=pid,used_memory', '--format=csv,noheader,nounits'])
        amounts = []
        for line in (output or '').splitlines():
            fields = line.split(',')
            if len(fields) == 2 and fields[0].strip().isdigit() and int(fields[0]) in pids:
                try:
                    amounts.append(float(fields[1]))
                except ValueError:
                    pass
        if amounts:
            gpu = sum(amounts)
    return rss / 1024, gpu


def stop(process):
    # This process group belongs solely to this benchmark case.
    # The leader can already have exited while a child still owns GPU work.
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        process.wait(timeout=5)
        return
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        pass
    # A profiler/wrapper can exit before its worker. Kill remaining members of
    # this exclusively owned process group even if its original leader exited.
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    if process.poll() is None:
        process.wait(timeout=5)


def digest(path):
    with path.open('rb') as stream:
        checksum = hashlib.sha256()
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            checksum.update(chunk)
        return checksum.hexdigest()


def counters(content, prefix):
    return [dict((key, int(value)) for key, value in re.findall(r'([a-z-]+)=(\d+)', line))
            for line in content.splitlines()
            if line.startswith(prefix) and ('frames=' in line or 'renders=' in line)]


def inspect_log(content, case):
    analytics = counters(content, 'HSTREAM_PLAYER_ANALYTICS ')
    overlays = counters(content, 'HSTREAM_PLAYER_OVERLAY ')
    failures = []
    for label, rows, expected in [('analytics', analytics, case.get('expect_analytics')),
                                  ('overlay', overlays, case.get('expect_overlay'))]:
        if expected is not None and bool(rows) != expected:
            failures.append(f'{label} activity differs from case expectation')
        for key, minimum in case.get(f'minimum_{label}_counters', {}).items():
            if sum(row.get(key, 0) for row in rows) < minimum:
                failures.append(f'insufficient {label} {key}')
        for key, maximum in case.get(f'maximum_{label}_counters', {}).items():
            values = [row[key] for row in rows if key in row]
            if not values or max(values) > maximum:
                failures.append(f'excess or missing {label} {key}')
    for key, minimum in case.get('minimum_counters', {}).items():
        if sum(row.get(key, 0) for row in analytics) < minimum:
            failures.append(f'insufficient analytics {key}')
    for pattern in case.get('required_log_patterns', []):
        if not re.search(pattern, content):
            failures.append(f'missing log pattern: {pattern}')
    for pattern in case.get('forbidden_log_patterns', []):
        if re.search(pattern, content):
            failures.append(f'forbidden log pattern: {pattern}')
    clean = re.sub(r'\x1b\[[0-9;]*m', '', content)
    detectors = sorted(set(re.findall(r'deserialized trt engine from :([^\r\n]+)', clean)))
    if 'detector_engine' in case:
        expected = str(Path(case['detector_engine']).resolve())
        if [str(Path(path.strip()).resolve()) for path in detectors] != [expected]:
            failures.append('loaded detector engine identity differs from pinned engine')
    if case.get('expect_overlay'):
        for row in overlays:
            if not 0 < row.get('launches', 0) <= row.get('renders', -1):
                failures.append('overlay launch count is zero or exceeds render count')
            if row.get('upload-bytes', 0) > row.get('renders', 0) * 2 * 1024 * 1024 + 147456:
                failures.append('overlay upload exceeds bounded command/atlas budget')
    return analytics, overlays, detectors, failures


def hashes(paths):
    return {str(Path(path).absolute()): digest(Path(path)) for path in paths}


def loaded_libraries(pid, runtime_root):
    result = set()
    for child in process_ids(pid):
        try:
            for line in Path(f'/proc/{child}/maps').read_text().splitlines():
                parts = line.split(maxsplit=5)
                if len(parts) == 6 and parts[5].startswith('/') and '.so' in parts[5]:
                    if parts[5].startswith(str(runtime_root) + '/') or any(
                            piece in parts[5] for piece in ('/gst-plugins/', '/bazel-out/', '/hstream/')):
                        result.add(parts[5])
        except OSError:
            pass
    return result


def run(case, repetition, directory, timeout, warmup_samples, smi):
    name = case['name']
    command = case['command']
    if not re.fullmatch(r'[a-zA-Z0-9_-]+', name) or not isinstance(command, list) or not command:
        raise ValueError('Each case needs a simple name and a nonempty argv list')
    if not all(isinstance(argument, str) for argument in command):
        raise ValueError('Command arguments must be strings')
    cwd = Path(case['cwd']).resolve(strict=True)
    executable = Path(command[0])
    if not executable.is_absolute():
        executable = cwd / executable
    executable = executable.resolve(strict=True)
    command = [str(executable), *command[1:]]
    env = dict(os.environ, **case.get('env', {}))
    log = directory / f'{repetition:02d}-{name}.log'
    rss, gpu, libraries = [], [], set()
    executable_digest = digest(executable)
    before = hashes(case.get('monitor_artifacts', []))
    started = time.monotonic()
    timed_out = False
    with log.open('w') as output:
        process = subprocess.Popen(command, cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                                   stdout=output, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            while process.poll() is None:
                host_memory, gpu_memory = memory(process.pid, smi)
                libraries.update(loaded_libraries(process.pid, cwd))
                rss.append(host_memory)
                if gpu_memory is not None:
                    gpu.append(gpu_memory)
                if time.monotonic() - started >= timeout:
                    timed_out = True
                    stop(process)
                    break
                time.sleep(.5)
        finally:
            stop(process)
    elapsed = time.monotonic() - started
    content = log.read_text(errors='replace')
    # Zero means no completed output in that interval, including possible
    # steady-state stalls. Discard only the explicitly requested leading warmup
    # observations; never silently remove zero intervals from the measurement.
    fps = [float(value) for value in re.findall(r'\*\*PERF:\s+([0-9.]+)\s+\(', content)]
    samples = fps[warmup_samples:]
    analytics, overlays, detectors, failures = inspect_log(content, case)
    after = hashes(case.get('monitor_artifacts', []))
    if before != after:
        failures.append('monitored artifacts changed during run')
    if digest(executable) != executable_digest:
        failures.append('executable changed during measurement')
    if process.returncode or timed_out:
        failures.append('process failed or timed out (including shutdown)')
    if not samples:
        failures.append('insufficient measured FPS samples after warmup')
    return {'name': name, 'repetition': repetition, 'command': command, 'cwd': str(cwd),
            'executable_sha256': executable_digest, 'returncode': process.returncode,
            'timed_out': timed_out, 'wall_seconds_including_startup_shutdown': elapsed,
            'fps_samples': fps, 'warmup_samples_discarded': warmup_samples,
            'median_output_fps': statistics.median(samples) if samples else None,
            'peak_process_tree_rss_mib': max(rss, default=None),
            'peak_process_tree_gpu_mib': max(gpu, default=None),
            'analytics_counters': analytics, 'overlay_counters': overlays,
            'detector_load_paths': detectors, 'artifact_hashes_before': before,
            'artifact_hashes_after': after,
            'loaded_libraries': {path: digest(Path(path)) if Path(path).is_file() else None
                                 for path in sorted(libraries)},
            'log': str(log), 'failures': failures}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--spec', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True, help='New directory for immutable evidence')
    parser.add_argument('--rounds', type=int, default=3)
    parser.add_argument('--timeout', type=float, default=600)
    parser.add_argument('--warmup-samples', type=int, default=1)
    parser.add_argument('--cases', help='Optional comma-separated case names')
    parser.add_argument('--validate-only', action='store_true', help='CPU-only spec/artifact validation')
    args = parser.parse_args()
    if args.rounds < 1 or args.timeout <= 0 or args.warmup_samples < 0:
        parser.error('Invalid round, timeout or warmup bound')
    spec = json.loads(args.spec.read_text())
    cases = spec['cases']
    if args.cases:
        selected = set(args.cases.split(','))
        if selected - {case['name'] for case in cases}:
            parser.error('Unknown case name')
        cases = [case for case in cases if case['name'] in selected]
    if not cases or len({case['name'] for case in cases}) != len(cases):
        parser.error('Specify cases with unique names')
    if args.validate_only:
        print(json.dumps({'case_names': [case['name'] for case in cases],
                          'artifacts': hashes(spec.get('artifacts', []))}, indent=2))
        return
    args.output = args.output.absolute()
    args.output.mkdir(parents=True, exist_ok=False)
    smi = shutil.which('nvidia-smi')
    report = {'spec': spec, 'platform': platform.platform(), 'gpu': query([
        smi, '--query-gpu=name,driver_version,power.limit', '--format=csv,noheader']) if smi else None,
        'memory_note': 'GPU memory is process-tree NVML memory when available. RSS is host memory, '
                       'not a substitute for GPU allocation measurements on Jetson.',
        'latency_note': 'FPS measures completed output throughput; these logs do not measure frame latency.',
        'artifacts': hashes(spec.get('artifacts', [])),
        'runs': []}
    for repetition in range(args.rounds):
        # Reverse order every other round to expose warmup/thermal order effects.
        for case in cases if repetition % 2 == 0 else reversed(cases):
            result = run(case, repetition, args.output, args.timeout, args.warmup_samples, smi)
            report['runs'].append(result)
            (args.output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
            print(json.dumps(result), flush=True)
            if result['failures']:
                raise SystemExit('Invalid benchmark run; inspect retained evidence')
    report['artifacts_after'] = hashes(spec.get('artifacts', []))
    report['artifacts_unchanged'] = report['artifacts_after'] == report['artifacts']
    (args.output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    if not report['artifacts_unchanged']:
        raise SystemExit('Artifacts changed across benchmark suite')


if __name__ == '__main__':
    main()
