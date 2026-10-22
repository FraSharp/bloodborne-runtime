# SPDX-License-Identifier: GPL-2.0-or-later
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from mst_passes import complete_frame_rows


def assignment(cb, frame, present=True):
    return {'process': ('', 'bb-probe'), 'cmdbuffer-id': (str(cb), ''),
            'frame-number': (str(frame), ''), 'has-present': ('1' if present else '0', '')}


def interval(cb, start):
    return {'process': ('', 'bb-probe'), 'cmdbuffer-id': (str(cb), ''),
            'start': (str(start), ''), 'duration': ('5', '')}


class CompleteFramesTests(unittest.TestCase):
    def setUp(self):
        # Two command buffers in frame 2, one with the presentation marker.
        self.assignments = [assignment(1, 1), assignment(2, 2, False), assignment(20, 2),
                            assignment(3, 3), assignment(4, 4)]
        self.rows = [interval(1, 10), interval(2, 20), interval(20, 25),
                     interval(3, 30), interval(4, 40)]

    def test_boundary_frames_and_multiple_command_buffers(self):
        selected, count = complete_frame_rows(self.rows, self.assignments)
        self.assertEqual(count, 2)
        self.assertEqual([r['cmdbuffer-id'][0] for r in selected], ['2', '20', '3'])

    def test_missing_middle_frame_fails(self):
        assignments = [assignment(1, 1), assignment(2, 2), assignment(3, 4), assignment(4, 5)]
        with self.assertRaisesRegex(ValueError, 'missing frame assignments'):
            complete_frame_rows(self.rows, assignments)

    def test_unknown_gpu_work_inside_fails(self):
        with self.assertRaisesRegex(ValueError, 'unassigned GPU intervals'):
            complete_frame_rows(self.rows + [interval(99, 22)], self.assignments)

    def test_unknown_boundary_work_is_excluded(self):
        selected, count = complete_frame_rows(self.rows + [interval(99, 0)], self.assignments)
        self.assertEqual(count, 2)
        self.assertEqual(len(selected), 3)

    def test_conflicting_assignment_fails(self):
        with self.assertRaisesRegex(ValueError, 'conflicting'):
            complete_frame_rows(self.rows, self.assignments + [assignment(2, 3)])

    def test_other_process_work_is_ignored(self):
        other = interval(99, 22)
        other['process'] = ('', 'another-process')
        selected, count = complete_frame_rows(self.rows + [other], self.assignments)
        self.assertEqual(count, 2)
        self.assertEqual(len(selected), 3)


if __name__ == '__main__':
    unittest.main()
