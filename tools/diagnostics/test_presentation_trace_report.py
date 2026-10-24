"""Presentation evidence checks; no GPU or desktop required."""
import unittest

from presentation_trace_report import analyze, callbacks
from test_acquire_trace_report import HOST, ACQUIRE, DRAWABLE


def records(layer=1, ident=2, queued=1_000_000_000):
    key = f'layer={layer} id={ident} queued_ns={queued}'
    return (f'present_enqueue {key} transaction=0 sync=1 count=3\n'
            f'present_scheduled {key} end_ns={queued + 10_000_000}\n'
            f'present_completed {key} end_ns={queued + 12_000_000} status=4 gpu_ms=2\n'
            f'drawable_presented {key} end_ns={queued + 20_000_000} actual_time=1\n')


class PresentationReportTest(unittest.TestCase):
    def test_complete_callbacks_and_cold_stall_retained(self):
        report = analyze(HOST, ACQUIRE + DRAWABLE + records())
        self.assertTrue(report['valid'])
        self.assertEqual(report['acquire']['host_work']['max_ms'], 1034)
        self.assertEqual(report['presentation']['enqueue_to_callback']
                         ['present_completed']['max_ms'], 12)
        self.assertEqual(len(report['slow_drawables']), 1)
        nearby = report['slow_drawables'][0]['nearby_presentations']
        self.assertEqual(len(nearby), 1)
        self.assertIn('drawable_presented', nearby[0]['unobserved_at_acquire_start'])

    def test_reused_ids_layers_and_out_of_order_logging(self):
        log = records() + records(queued=2_000_000_000) + records(layer=3)
        events, report = callbacks('\n'.join(reversed(log.splitlines())))
        self.assertTrue(report['valid'])
        self.assertEqual(len(events), 3)
        self.assertEqual(report['event_counts']['drawable_presented'], 3)

    def test_missing_duplicate_or_orphan_callbacks_are_incomplete(self):
        for log in ('', records().replace('present_enqueue', 'unknown'),
                    '\n'.join(records().splitlines()[:-1]), records() * 2):
            with self.subTest(log=log):
                self.assertFalse(callbacks(log)[1]['valid'])

    def test_malformed_negative_and_backwards_records_rejected(self):
        for log in (records().replace('layer=1', 'layer=bad'),
                    records().replace('id=2', 'id=-2'),
                    records().replace('end_ns=1010000000', 'end_ns=1'),
                    records().replace('status=4', 'status=bad'),
                    records().replace('layer=1', 'layer=1 layer=2')):
            with self.subTest(log=log), self.assertRaises(ValueError):
                callbacks(log)

    def test_command_failure_retained_without_hiding_other_events(self):
        _, report = callbacks(records().replace('status=4', 'status=5'))
        self.assertTrue(report['valid'])  # Complete failure evidence is still valid evidence.
        self.assertEqual(len(report['noncompleted_command_buffers']), 1)

    def test_callback_execution_order_is_not_assumed(self):
        # CPU scheduled/completed handlers can be observed out of chronological order.
        _, report = callbacks(records().replace('end_ns=1010000000', 'end_ns=1030000000'))
        self.assertTrue(report['valid'])

    def test_loading_stall_outside_host_frames_is_retained(self):
        log = (ACQUIRE.replace('start_ns=1001000000', 'start_ns=10001000000')
               + DRAWABLE.replace('start_ns=1005000000', 'start_ns=10005000000')
               + records(queued=10_000_000_000))
        report = analyze(HOST, log)
        self.assertTrue(report['valid'])
        self.assertEqual(report['acquire']['slow_host_frames'][0]['overlapping_drawables'], [])
        self.assertEqual(len(report['slow_drawables']), 1)

    def test_fast_native_call_does_not_clear_slow_host(self):
        report = analyze(HOST, ACQUIRE + DRAWABLE.replace('duration_ns=1000000000',
                                                        'duration_ns=1000') + records())
        self.assertTrue(report['valid'])
        self.assertEqual(report['slow_drawables'], [])
        self.assertEqual(report['acquire']['host_work']['max_ms'], 1034)


if __name__ == '__main__':
    unittest.main()
