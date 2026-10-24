#!/usr/bin/env python3
"""Correlate DE_GPU_ACQUIRE_TRACE=1 logs with DE_HOST_FRAME_TRACE (DENG-605).

All host frames, including cold frames, contribute to statistics. Overlapping calls
are evidence of concurrency, not proof that a call blocked that host phase. GPU
pass statistics and callback frame numbers are deliberately not used for attribution.
Standard library only. Exit 2 for malformed/incomplete attribution input.
"""

import argparse
import bisect
import csv
import hashlib
import json
import math
from pathlib import Path
import re
import statistics


def stats(values):
    values = sorted(values)
    if not values:
        return None
    n = len(values)
    return dict(count=n, mean_ms=statistics.mean(values) / 1e6,
                p95_ms=values[math.ceil(n * .95) - 1] / 1e6,
                p99_ms=values[math.ceil(n * .99) - 1] / 1e6,
                max_ms=values[-1] / 1e6,
                over_50ms=sum(v > 50_000_000 for v in values),
                over_100ms=sum(v > 100_000_000 for v in values))


def parse_records(log, marker, fields):
    records = []
    for line in log.splitlines():
        if marker not in line:
            continue
        payload = line.split(marker, 1)[1]
        pairs = dict(re.findall(r"\b(\w+)=(-?\d+)(?=\s|$)", payload))
        try:
            record = {key: int(pairs[key]) for key in fields}
        except KeyError as exc:
            raise ValueError(f"{marker}: missing integer {exc.args[0]}") from exc
        if any(value < 0 for key, value in record.items() if key != "result"):
            raise ValueError(f"{marker}: negative time/count")
        records.append(record)
    return records


def analyze(host_text, log, slow_ns=50_000_000):
    host = []
    for row in csv.DictReader(host_text.splitlines()):
        try:
            record = {key: int(value) for key, value in row.items()}
            if any(value < 0 for value in record.values()):
                raise ValueError("negative host time/count")
            if record['end_ns'] < record['start_ns']:
                raise ValueError("host end before start")
            record['frame']
        except (KeyError, TypeError, ValueError) as exc:
            raise ValueError(f"malformed host frame: {row}") from exc
        if host and record['start_ns'] <= host[-1]['start_ns']:
            raise ValueError("host starts must increase strictly")
        host.append(record)
    if not host:
        raise ValueError("no host frames")
    acquire = parse_records(log, 'acquire_trace ',
                            ('start_ns', 'validate_ns', 'collect_ns', 'backend_ns',
                             'pending', 'reclaimed', 'result'))
    native = sorted(parse_records(log, 'drawable_trace ',
                                  ('start_ns', 'duration_ns', 'found')),
                    key=lambda item: item['start_ns'])
    starts = [item['start_ns'] for item in native]
    matches = [0] * len(native)
    for item in acquire:
        item['backend_start_ns'] = (item['start_ns'] + item['validate_ns']
                                    + item['collect_ns'])
        item['end_ns'] = item['backend_start_ns'] + item['backend_ns']
        first = bisect.bisect_left(starts, item['backend_start_ns'])
        last = bisect.bisect_right(starts, item['end_ns'])
        for i in range(first, last):
            if native[i]['start_ns'] + native[i]['duration_ns'] <= item['end_ns']:
                matches[i] += 1
    errors = []
    if not acquire:
        errors.append('no acquire records; enable DE_GPU_ACQUIRE_TRACE=1 and GPU info logging')
    if not native:
        errors.append('no native drawable records; native attribution unavailable')
    if any(count != 1 for count in matches):
        errors.append('native intervals must belong to exactly one backend interval')

    def overlaps(start, end, item, duration_key=None):
        item_end = (item['start_ns'] + item[duration_key]
                    if duration_key else item['end_ns'])
        return item['start_ns'] < end and item_end > start

    def correlate(row, end):
        return dict(host=row, interval_end_ns=end,
                             overlapping_acquires=[item for item in acquire
                                 if overlaps(row['start_ns'], end, item)],
                             overlapping_drawables=[item for item in native
                                 if overlaps(row['start_ns'], end, item,
                                             'duration_ns')])
    slow = [correlate(row, row['end_ns']) for row in host
            if row['end_ns'] - row['start_ns'] > slow_ns]
    slow_intervals = [correlate(a, b['start_ns']) for a, b in zip(host, host[1:])
                      if b['start_ns'] - a['start_ns'] > slow_ns]
    return dict(valid=not errors, errors=errors,
                scope='All host frames; CPU monotonic wall times. Overlap is not causation.',
                host_work=stats([r['end_ns'] - r['start_ns'] for r in host]),
                host_start_intervals=stats([b['start_ns'] - a['start_ns']
                                           for a, b in zip(host, host[1:])]),
                validation=stats([r['validate_ns'] for r in acquire]),
                collection=stats([r['collect_ns'] for r in acquire]),
                backend=stats([r['backend_ns'] for r in acquire]),
                next_drawable=stats([r['duration_ns'] for r in native]),
                acquire_failures=[r for r in acquire if r['result'] != 0],
                missing_drawables=[r for r in native if r['found'] == 0],
                unmatched_native=sum(n == 0 for n in matches),
                ambiguous_native=sum(n > 1 for n in matches),
                slow_host_threshold_ns=slow_ns, slow_host_frames=slow,
                slow_host_start_intervals=slow_intervals)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', type=Path, required=True)
    parser.add_argument('--log', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    try:
        host_data, log_data = args.host.read_bytes(), args.log.read_bytes()
        report = analyze(host_data.decode(), log_data.decode())
        report['inputs'] = {
            'host': dict(path=str(args.host.resolve()),
                         sha256=hashlib.sha256(host_data).hexdigest()),
            'log': dict(path=str(args.log.resolve()),
                        sha256=hashlib.sha256(log_data).hexdigest())}
        args.out.write_text(json.dumps(report, indent=2) + '\n')
    except (OSError, ValueError, UnicodeError) as exc:
        parser.error(str(exc))
    print(json.dumps({key: report[key] for key in
                      ('valid', 'errors', 'host_work', 'host_start_intervals',
                       'collection', 'backend', 'next_drawable')}))
    return 0 if report['valid'] else 2


if __name__ == '__main__':
    raise SystemExit(main())
