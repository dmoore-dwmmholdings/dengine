"""Evidence-integrity regressions; run with python3 -m unittest discover -s tools/diagnostics."""

import unittest

from acquire_trace_report import analyze


HOST = ('frame,start_ns,end_ns,pump_ns,extract_kick_ns\n'
        '0,1000000000,2034000000,1000000,1032000000\n'
        '1,2035000000,2050000000,0,15000000\n')
ACQUIRE = ('acquire_trace start_ns=1001000000 collect_ns=2000000 '
           'backend_ns=1020000000 pending=4 reclaimed=2 result=0 validate_ns=1000000\n')
DRAWABLE = ('drawable_trace start_ns=1005000000 duration_ns=1000000000 '
            'layer=0x123 size=2560x1600 found=1\n')


class AcquireReportTest(unittest.TestCase):
    def test_cold_stall_preserved_and_native_interval_attributed(self):
        report = analyze(HOST, DRAWABLE + ACQUIRE)
        self.assertTrue(report['valid'])
        self.assertEqual(report['host_work']['max_ms'], 1034)
        self.assertEqual(report['host_work']['over_100ms'], 1)
        self.assertEqual(report['next_drawable']['max_ms'], 1000)
        self.assertEqual(report['collection']['max_ms'], 2)
        self.assertEqual(report['host_start_intervals']['max_ms'], 1035)
        slow = report['slow_host_frames']
        self.assertEqual(len(slow), 1)
        self.assertEqual(slow[0]['host']['frame'], 0)
        self.assertEqual(len(slow[0]['overlapping_drawables']), 1)

    def test_fast_native_call_does_not_clear_slow_host(self):
        log = DRAWABLE.replace('duration_ns=1000000000', 'duration_ns=8000000')
        report = analyze(HOST, log + ACQUIRE)
        self.assertTrue(report['valid'])
        self.assertEqual(report['next_drawable']['max_ms'], 8)
        self.assertEqual(report['host_work']['max_ms'], 1034)

    def test_validation_and_collection_not_part_of_native_backend(self):
        log = DRAWABLE.replace('start_ns=1005000000', 'start_ns=1002000000')
        report = analyze(HOST, log + ACQUIRE)
        self.assertFalse(report['valid'])
        self.assertEqual(report['unmatched_native'], 1)

    def test_missing_duplicate_and_truncated_records_rejected(self):
        self.assertFalse(analyze(HOST, '')['valid'])
        self.assertFalse(analyze(HOST, DRAWABLE + ACQUIRE * 2)['valid'])
        with self.assertRaisesRegex(ValueError, 'missing integer validate_ns'):
            analyze(HOST, ACQUIRE.replace(' validate_ns=1000000', ''))
        with self.assertRaises(ValueError):
            analyze(HOST + '2,broken\n', ACQUIRE)

    def test_backend_acquire_without_native_call_is_allowed(self):
        # Reusing an acquired image returns it without calling nextDrawable again.
        log = ACQUIRE.replace('start_ns=1001000000', 'start_ns=3000000000')
        self.assertTrue(analyze(HOST, DRAWABLE + ACQUIRE + log)['valid'])

    def test_failures_and_native_timeout_retained(self):
        report = analyze(HOST, DRAWABLE.replace('found=1', 'found=0')
                         + ACQUIRE.replace('result=0', 'result=-7'))
        self.assertTrue(report['valid'])
        self.assertEqual(len(report['acquire_failures']), 1)
        self.assertEqual(len(report['missing_drawables']), 1)

    def test_invalid_host_order_and_time_rejected(self):
        for host in ('frame,start_ns,end_ns\n',
                     'frame,start_ns,end_ns\n0,-2,1\n',
                     'frame,start_ns,end_ns\n0,2,1\n',
                     'frame,start_ns,end_ns\n0,2,3\n1,1,2\n'):
            with self.subTest(host=host), self.assertRaises(ValueError):
                analyze(host, DRAWABLE + ACQUIRE)

    def test_gap_between_fast_host_frames_is_not_hidden(self):
        host = ('frame,start_ns,end_ns\n'
                '0,1000000000,1001000000\n1,2035000000,2036000000\n')
        report = analyze(host, DRAWABLE + ACQUIRE)
        self.assertEqual(report['slow_host_frames'], [])
        self.assertEqual(len(report['slow_host_start_intervals']), 1)
        self.assertEqual(len(report['slow_host_start_intervals'][0]
                             ['overlapping_drawables']), 1)


if __name__ == '__main__':
    unittest.main()
