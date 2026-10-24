#!/usr/bin/env python3
"""Correlate opt-in Metal presentation callbacks with host/acquisition traces (DENG-605).

Run with DE_GPU_PRESENT_TRACE=1, DE_GPU_ACQUIRE_TRACE=1 and DE_HOST_FRAME_TRACE
set to a CSV path; capture both stdout and stderr. Callback observation can change
scheduling. CPU callback times are not GPU execution times or proof of display
visibility. Incomplete shutdown callbacks do not prove a leak. Exit 2 for incomplete
or malformed evidence. Standard library only.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re

from acquire_trace_report import analyze as analyze_acquire, parse_records, stats


KINDS = ('present_enqueue', 'present_scheduled', 'present_completed', 'drawable_presented')


def callbacks(log):
    events, errors = {}, []
    for line in log.splitlines():
        # stdout and stderr can join a complete record to an unrelated text
        # prefix; do not require a word boundary before its intact marker.
        match = re.search(r'(' + '|'.join(KINDS) + r')\s+(.*)', line)
        if not match:
            continue
        kind, payload = match.groups()
        pairs = re.findall(r'\b(\w+)=([^\s]+)', payload)
        fields = dict(pairs)
        if len(fields) != len(pairs):
            raise ValueError(f'{kind}: duplicate fields')
        required = ('layer', 'id', 'queued_ns') + (() if kind == 'present_enqueue' else ('end_ns',))
        try:
            record = {key: int(fields[key]) for key in required}
            if kind == 'present_completed':
                record['status'] = int(fields['status'])
        except (KeyError, ValueError) as exc:
            raise ValueError(f'{kind}: missing/malformed integer field') from exc
        if any(value < 0 for value in record.values()):
            raise ValueError(f'{kind}: negative time/identifier/status')
        if record.get('end_ns', record['queued_ns']) < record['queued_ns']:
            raise ValueError(f'{kind}: callback precedes enqueue time')
        key = tuple(record[key] for key in ('layer', 'id', 'queued_ns'))
        item = events.setdefault(key, {})
        if kind in item:
            errors.append(f'duplicate {kind} for {key}')
        else:
            item[kind] = record
    if not events:
        errors.append('no presentation records; enable DE_GPU_PRESENT_TRACE=1 and capture stderr')
    for key, item in events.items():
        missing = [kind for kind in KINDS if kind not in item]
        if missing:
            errors.append(f'incomplete {key}: missing {", ".join(missing)}')
    latencies = {
        kind: stats([item[kind]['end_ns'] - key[2] for key, item in events.items()
                     if kind in item and 'present_enqueue' in item])
        for kind in KINDS[1:]}
    failures = [item['present_completed'] for item in events.values()
                if 'present_completed' in item and item['present_completed']['status'] != 4]
    return events, dict(valid=not errors, errors=errors, presentations=len(events),
                        event_counts={kind: sum(kind in item for item in events.values())
                                      for kind in KINDS},
                        enqueue_to_callback=latencies, incomplete=sum(len(item) != 4
                                                                     for item in events.values()),
                        noncompleted_command_buffers=failures)


def analyze(host_text, log):
    acquire = analyze_acquire(host_text, log)
    events, presentation = callbacks(log)
    slow = []
    # Only native nextDrawable calls establish this interval; host overlap alone
    # does not establish that presentation blocked a host phase.
    native = {(item['start_ns'], item['duration_ns'], item['found'])
              for item in parse_records(log, 'drawable_trace ',
                                        ('start_ns', 'duration_ns', 'found'))
              if item['duration_ns'] > 50_000_000}
    for start, duration, found in sorted(native):
        end = start + duration
        nearby = []
        for key, item in events.items():
            queued = key[2]
            shown = item.get('drawable_presented', {}).get('end_ns')
            if queued <= end and (queued >= start - 200_000_000 or shown is None or shown >= start):
                nearby.append(dict(layer=key[0], id=key[1], queued_ns=queued, events=item,
                                   unobserved_at_acquire_start=[kind for kind in KINDS[1:]
                                       if kind not in item or item[kind]['end_ns'] > start]))
        slow.append(dict(start_ns=start, duration_ns=duration, found=found,
                         nearby_presentations=sorted(nearby, key=lambda item: item['queued_ns'])))
    return dict(valid=acquire['valid'] and presentation['valid'], acquire=acquire,
                presentation=presentation, slow_drawables=slow,
                scope='All host frames retained. Callback times are CPU observations; '
                      'nearby events do not prove causation or OS visibility. '
                      'Missing shutdown callbacks do not prove resource retention.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ('host', 'log', 'out'):
        parser.add_argument('--' + option, type=Path, required=True)
    args = parser.parse_args()
    try:
        host, log = args.host.read_bytes(), args.log.read_bytes()
        report = analyze(host.decode(), log.decode())
        report['inputs'] = {
            'host': dict(path=str(args.host.resolve()), sha256=hashlib.sha256(host).hexdigest()),
            'log': dict(path=str(args.log.resolve()), sha256=hashlib.sha256(log).hexdigest())}
        args.out.write_text(json.dumps(report, indent=2) + '\n')
    except (OSError, ValueError, UnicodeError) as exc:
        parser.error(str(exc))
    print(json.dumps(dict(valid=report['valid'], presentation=report['presentation'])))
    return 0 if report['valid'] else 2


if __name__ == '__main__':
    raise SystemExit(main())
