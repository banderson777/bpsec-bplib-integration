"""Focused tests for streaming ADU encoding and the growing HLS playlist."""
from pathlib import Path
import hashlib
import json
import struct
import tempfile
import unittest
from unittest.mock import patch

from five_node import (
    file_completion_record, file_marker, live_playlist, segment_records,
    stream_file_segments, verify,
)


class StreamingRecords(unittest.TestCase):
    """Check the byte layout shared by the watcher and BPNode receiver."""

    def test_file_marker_carries_session_and_sequence(self):
        session = bytes(range(16))
        marker = file_marker(session, 12)
        self.assertEqual(marker[:4], b'BPF1')
        self.assertEqual(marker[4:20], session)
        self.assertEqual(struct.unpack('!I', marker[20:]), (12,))

    def test_file_completion_record_carries_archive_range(self):
        session = bytes(range(16))
        marker = file_completion_record(session, 3, 8, 5)
        self.assertEqual(marker[:4], b'BPE2')
        self.assertEqual(marker[4:20], session)
        self.assertEqual(struct.unpack('!III', marker[20:]), (3, 8, 5))

    def test_records_carry_unknown_total_and_exact_chunk_metadata(self):
        session = bytes(range(16))
        records = list(segment_records(session, 7, b'abcdefghij', 4))

        self.assertEqual(len(records), 3)
        for part, record in enumerate(records):
            self.assertEqual(record[:4], b'BPD1')
            self.assertEqual(record[4:20], session)
            segment, index, count, total, length = struct.unpack('!IIIII', record[20:40])
            self.assertEqual((segment, index, count, total), (7, part, 3, 0))
            self.assertEqual(len(record[40:]), length)
        self.assertEqual(b''.join(record[40:] for record in records), b'abcdefghij')

    def test_empty_segment_still_has_one_record(self):
        records = list(segment_records(bytes(16), 0, b'', 4096))
        self.assertEqual(len(records), 1)
        self.assertEqual(struct.unpack('!IIIII', records[0][20:40]), (0, 0, 1, 0, 0))


class GrowingPlaylist(unittest.TestCase):
    """Check elapsed timing and publication order without a live player."""

    def test_ffmpeg_timestamps_become_segment_durations(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            (output / 'segments.csv').write_text(
                'segment-000000.ts,0.000000,1.250000\n'
                'segment-000001.ts,1.250000,2.000000\n'
            )
            (output / 'segment-000000.ts').write_bytes(b'first')
            (output / 'segment-000001.ts').write_bytes(b'second')
            with patch('five_node.subprocess.run'):
                segments = stream_file_segments(output / 'clip.mp4', output, 1.0)
            self.assertEqual(segments, [(b'first', 1.25), (b'second', 0.75)])

    def test_playlist_exposes_only_published_segments_then_ends(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            entries = [
                dict(segment=0, duration=2.0),
                dict(segment=1, duration=1.3, discontinuity=True),
            ]
            (output / 'ground-000000.ts').write_bytes(b'first')

            partial = live_playlist(output, entries, 2.0, False).decode()
            self.assertIn('ground-000000.ts', partial)
            self.assertNotIn('ground-000001.ts', partial)
            self.assertNotIn('#EXT-X-ENDLIST', partial)

            (output / 'ground-000001.ts').write_bytes(b'second')
            complete = live_playlist(output, entries, 2.0, True).decode()
            self.assertIn('#EXT-X-DISCONTINUITY', complete)
            self.assertIn('#EXTINF:1.300,', complete)
            self.assertIn('#EXT-X-ENDLIST', complete)


class VideoArchives(unittest.TestCase):
    """Check verification of the sink's per-video archive against its segments."""

    def test_verify_checks_archives_against_received_segments(self):
        with tempfile.TemporaryDirectory() as temporary:
            run = Path(temporary)
            output = run / 'node5/output'
            videos = output / 'videos'
            videos.mkdir(parents=True)
            segments = [b'first-ts-segment', b'second-ts-segment']
            entries = []
            for segment_id, data in enumerate(segments):
                (output / f'ground-{segment_id:06d}.ts').write_bytes(data)
                entries.append(dict(
                    segment=segment_id,
                    size=len(data),
                    sha256=hashlib.sha256(data).hexdigest(),
                ))
            (videos / 'video-000001.ts').write_bytes(b''.join(segments))
            (output / 'complete').write_text('complete')
            (run / 'manifest.json').write_text(json.dumps(dict(
                segments=entries,
                files=[dict(file_number=1, first_segment=0, segment_count=2,
                            source='clip.mp4', accepted_by_node1=True)],
            )))

            verify(run, timeout=0)


if __name__ == '__main__':
    unittest.main(verbosity=2)
