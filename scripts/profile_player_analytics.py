#!/usr/bin/env python3
"""Profile a spec case, or inspect an Nsight SQLite export without running GPU work."""
import argparse
from collections import Counter, defaultdict
import json
import os
from pathlib import Path
import re
import shutil
import sqlite3
import subprocess

from benchmark_player_analytics import digest, hashes, inspect_log, stop

REDUCERS = {'DecodeKernel': ('pose', 204), 'JerseyDecode': ('jersey', 8), 'ActionReduce': ('action', 8)}


def reduction(name):
    if 'player_analytics' not in name:
        return None
    for kernel, identity in REDUCERS.items():
        if kernel in name:
            return identity
    return None


def device_to_host(direction):
    normalized = re.sub(r'[^a-z0-9]', '', direction.lower())
    return normalized in ('dtoh', 'd2h', 'devicetohost', 'unifieddevicetohost')


def analyze(database, required=('pose', 'jersey', 'action'), batch=8, expect_overlay=False):
    connection = sqlite3.connect(f'file:{database}?mode=ro', uri=True)
    connection.row_factory = sqlite3.Row
    columns = {row[1] for row in connection.execute('PRAGMA table_info(CUPTI_ACTIVITY_KIND_KERNEL)')}
    name_column = 'demangledName' if 'demangledName' in columns else 'shortName'
    kernels = [dict(row) for row in connection.execute(f'''
        SELECT k.start, k.end, k.deviceId, k.contextId, k.streamId, s.value AS name
        FROM CUPTI_ACTIVITY_KIND_KERNEL k JOIN StringIds s ON s.id=k.{name_column}
        ORDER BY k.start''')]
    copies = [dict(row) for row in connection.execute('''
        SELECT c.start, c.end, c.deviceId, c.contextId, c.streamId, c.bytes,
               e.label AS direction, c.correlationId
        FROM CUPTI_ACTIVITY_KIND_MEMCPY c JOIN ENUM_CUDA_MEMCPY_OPER e ON e.id=c.copyKind
        ORDER BY c.start''')]
    connection.close()
    key = lambda row: (row['deviceId'], row['contextId'], row['streamId'])
    streams = {key(kernel) for kernel in kernels if reduction(kernel['name'])}
    overlay_kernels = [kernel for kernel in kernels
                       if 'draw_display::analytics::detail::' in kernel['name']
                       and re.search(r'\bDraw(?:<[^>]+>)?\(', kernel['name'])]
    overlay_streams = {key(kernel) for kernel in overlay_kernels}
    grouped = defaultdict(list)
    for kernel in kernels:
        if key(kernel) in streams:
            grouped[key(kernel)].append((kernel['start'], 'kernel', kernel))
    for copy in copies:
        if key(copy) in streams:
            grouped[key(copy)].append((copy['start'], 'copy', copy))
    failures, compact, other = [], [], []
    counts, reducer_counts = Counter(), Counter()
    for stream, events in grouped.items():
        preceding, active = None, False
        for _, kind, event in sorted(events, key=lambda event: event[0]):
            if kind == 'kernel':
                preceding = reduction(event['name'])
                if preceding:
                    active = True
                    reducer_counts[preceding[0]] += 1
                continue
            if not device_to_host(event['direction']):
                continue
            if not active:
                other.append(dict(event, phase='before-first-analytics-reduction'))
                continue
            if preceding is None:
                failures.append(f'Unattributed D2H on analytics stream {stream}: {event["bytes"]} bytes')
                other.append(dict(event, phase='active-unattributed'))
                continue
            feature, item_bytes = preceding
            size = event['bytes']
            if size <= 0 or size % item_bytes or size > batch * item_bytes:
                failures.append(f'{feature} D2H outside compact result bound: {size} bytes')
            compact.append(dict(event, feature=feature, item_bytes=item_bytes, batch=size // item_bytes))
            counts[feature] += 1
            preceding = None  # One compact result transfer per reducer invocation.
    for feature in required:
        if counts[feature] == 0:
            failures.append(f'No compact {feature} result transfer observed')
        if counts[feature] != reducer_counts[feature]:
            failures.append(f'{feature} reducer/transfer count mismatch')
    overlay_d2h = []
    if overlay_kernels:
        first = min(kernel['start'] for kernel in overlay_kernels)
        for copy in copies:
            if key(copy) in overlay_streams and copy['start'] >= first:
                if device_to_host(copy['direction']):
                    overlay_d2h.append(copy)
    if expect_overlay and not overlay_kernels:
        failures.append('No Program overlay raster kernels captured')
    if overlay_d2h:
        failures.append('D2H captured on the active Program overlay stream; attribution needs investigation')
    global_histogram = Counter((row['direction'], row['bytes']) for row in copies)
    return {'database': str(database), 'database_sha256': digest(Path(database)),
            'analytics_streams': [list(stream) for stream in sorted(streams)],
            'compact_d2h_counts': dict(counts), 'reduction_kernel_counts': dict(reducer_counts),
            'compact_d2h_max_bytes': max((row['bytes'] for row in compact), default=0),
            'compact_d2h_total_bytes': sum(row['bytes'] for row in compact),
            'compact_d2h': compact, 'other_analytics_stream_d2h': other,
            'overlay_kernel_count': len(overlay_kernels),
            'overlay_streams': [list(stream) for stream in sorted(overlay_streams)],
            'overlay_stream_d2h': overlay_d2h,
            'all_copy_size_histogram': [{'direction': kind, 'bytes': size, 'count': count}
                                        for (kind, size), count in sorted(global_histogram.items())],
            'failures': failures,
            'interpretation': 'Dedicated analytics streams are identified by the named GPU reduction kernels. '
                              'Every subsequent D2H must immediately follow its reducer and fit B<=8 compact '
                              'results: pose 204 bytes/player; jersey/action 8 bytes/player. Other pipeline '
                              'streams include pre-existing detector/tracker transfers and are retained separately.'}


def run_profile(command, cwd, env, log, timeout):
    process = subprocess.Popen(command, cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                               stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    try:
        return process.wait(timeout=timeout)
    finally:
        stop(process)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--database', type=Path)
    parser.add_argument('--spec', type=Path)
    parser.add_argument('--case', default='all3-draw')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--nsys', help='Nsight Systems executable (default: nsys found on PATH)')
    parser.add_argument('--timeout', type=float, default=600, help='Bound capture and export time in seconds')
    args = parser.parse_args()
    args.output = args.output.absolute()
    if args.timeout <= 0:
        parser.error('Timeout must be positive')
    case = None
    before = None
    if args.database:
        database = args.database
        args.output.mkdir(parents=True, exist_ok=True)
    else:
        if not args.spec:
            parser.error('Actual profiling needs --spec')
        nsys = args.nsys or shutil.which('nsys')
        if not nsys:
            parser.error('Nsight Systems not found; install it or pass --nsys')
        args.output.mkdir(parents=True, exist_ok=False)
        spec = json.loads(args.spec.read_text())
        case = next(case for case in spec['cases'] if case['name'] == args.case)
        before = hashes(spec['artifacts'])
        prefix = args.output / args.case
        command = [nsys, 'profile', '--sample=none', '--cpuctxsw=none',
                   '--trace=cuda,nvtx', '--cuda-memory-usage=true', '--force-overwrite=false',
                   '--export=sqlite', f'--output={prefix}', *case['command']]
        invocation = {'command': command, 'case': case, 'artifacts_before': before,
                      'nsys_version': subprocess.check_output([nsys, '--version'], text=True).strip()}
        (args.output / 'invocation.json').write_text(json.dumps(invocation, indent=2) + '\n')
        with (args.output / 'profile.log').open('w') as log:
            try:
                returncode = run_profile(command, case['cwd'], dict(os.environ, **case.get('env', {})),
                                         log, args.timeout)
            except subprocess.TimeoutExpired:
                raise SystemExit(f'Profiler exceeded {args.timeout} seconds; its process group was stopped')
        if returncode:
            raise SystemExit(f'Profiler failed: {returncode}')
        database = prefix.with_suffix('.sqlite')
    report = analyze(database, required=case['features'] if case else ('pose', 'jersey', 'action'),
                     expect_overlay=case['expect_overlay'] if case else False)
    if case:
        content = (args.output / 'profile.log').read_text(errors='replace')
        analytics, overlays, detectors, failures = inspect_log(content, case)
        report.update(analytics_counters=analytics, overlay_counters=overlays, detector_load_paths=detectors)
        report.update(artifact_hashes_before=before, artifact_hashes_after=hashes(spec['artifacts']))
        if report['artifact_hashes_before'] != report['artifact_hashes_after']:
            report['failures'].append('Artifacts changed while profiling')
        report['failures'].extend(failures)
        if case['expect_overlay'] and report['overlay_kernel_count'] != sum(row.get('launches', 0) for row in overlays):
            report['failures'].append('Program overlay kernel count differs from logged launch count')
        for feature in case['features']:
            enqueues = sum(row.get(f'{feature}-enqueues', 0) for row in analytics)
            if enqueues != report['compact_d2h_counts'].get(feature, 0):
                report['failures'].append(f'{feature} logged enqueue count differs from captured compact transfer count')
    (args.output / 'transfers.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({key: value for key, value in report.items() if key not in ('compact_d2h', 'all_copy_size_histogram')}))
    if report['failures']:
        raise SystemExit('Transfer validation failed; inspect raw profile')


if __name__ == '__main__':
    main()
