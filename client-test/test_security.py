"""Regress authentication, keys/nonces, durable custody/replay, and demo CLIs.

Run directly with Python 3 and client-test requirements installed. Socket tests
need local networking permission; temporary directories isolate stored records.
"""
import os
import socket
import subprocess
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from unittest.mock import patch

from cryptography.exceptions import InvalidTag
from secure_records import Codec, GroundStore, load_key, record_id
from custody_video_demo import RelayStore, ground, relay, moon
import continuous_video_demo as continuous


class SecurityTests(unittest.TestCase):
    def setUp(self):
        """Give each test a temporary store and a fresh shared record key."""
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.key = load_key()
        self.codec = Codec(self.key)

    def bundle(self, part=0, count=2):
        """Build one deterministic chunk with zero-based part and total part count."""
        return dict(kind="bundle", session=b"s" * 16, seq=part, segment=0,
                    part=part, count=count, payload=f"part{part}".encode())

    def test_new_keys_and_nonces_across_runs(self):
        """Check fresh random keys and nonces, plus stable reuse of a private key file."""
        self.assertNotEqual(load_key(), load_key())
        path = self.root / "key"
        self.assertEqual(load_key(path), load_key(path))
        if os.name != "nt": self.assertEqual(path.stat().st_mode & 0o777, 0o600)
        a = Codec(self.key).encode(self.bundle())
        b = Codec(self.key).encode(self.bundle())
        self.assertNotEqual(a[:12], b[:12])

    def test_all_fields_and_controls_authenticated(self):
        """Flip every byte of data and control envelopes and require tag rejection."""
        for message in [self.bundle(), dict(kind="eof"), dict(kind="ground_ack", seq=1),
                        dict(kind="custody_ack", seq=1)]:
            blob = self.codec.encode(message)
            self.assertEqual(self.codec.decode(blob), message)
            # Modifications anywhere in the nonce/ciphertext/tag fail closed.
            for index in range(len(blob)):
                altered = bytearray(blob); altered[index] ^= 1
                with self.assertRaises(InvalidTag): self.codec.decode(bytes(altered))
            with self.assertRaises(InvalidTag): Codec(load_key()).decode(blob)

    def test_previous_run_records_rejected(self):
        """Reject a valid authenticated record when its session belongs to another run."""
        previous = Codec(self.key, session=b"a" * 16)
        current = Codec(self.key, session=b"b" * 16)
        with self.assertRaises(ValueError): current.decode(previous.encode(dict(eof=True)))

    def test_path_traversal_and_unknown_ack_cannot_delete(self):
        """Reject malformed ACK identifiers and preserve files outside the spool."""
        store = RelayStore(self.root / "spool", self.codec)
        victim = self.root / "outside.cbor"; victim.write_bytes(b"keep")
        for seq in ("../outside", str(victim)[:-5], -1, True, 1 << 64):
            with self.assertRaises(ValueError):
                store.acknowledge(dict(kind="ground_ack", session=b"s" * 16, seq=seq))
        self.assertFalse(store.acknowledge(dict(kind="ground_ack", session=b"s" * 16, seq=1)))
        self.assertEqual(victim.read_bytes(), b"keep")

    def test_matching_ack_releases_only_pending_transfer(self):
        """Release a stored transfer once; treat an identical later ACK as a no-op."""
        store = RelayStore(self.root / "spool", self.codec)
        msg = self.bundle(); store.accept(msg, self.codec.encode(msg))
        ack = dict(kind="ground_ack", session=msg["session"], seq=msg["seq"])
        self.assertTrue(store.acknowledge(ack))
        self.assertFalse(store.acknowledge(ack))
        self.assertFalse(list(store.directory.glob("*.cbor")))

    def test_recovery_missing_part_and_duplicate(self):
        """Recover a persisted part after restart and reject a conflicting duplicate."""
        ground_dir = self.root / "ground"
        store = GroundStore(ground_dir, self.codec)
        last = self.bundle(1)
        store.accept(last, self.codec.encode(last))
        self.assertFalse((ground_dir / "ground-000000.ts").exists())
        # Simulate restart after an acknowledged part, then receive the missing part.
        store = GroundStore(ground_dir, Codec(self.key))
        first = self.bundle()
        store.accept(first, self.codec.encode(first))
        store.accept(last, self.codec.encode(last))
        self.assertEqual((ground_dir / "ground-000000.ts").read_bytes(), b"part0part1")
        with self.assertRaises(ValueError):
            changed = dict(first, payload=b"changed")
            store.accept(changed, self.codec.encode(changed))

    def test_storage_failure_prevents_ack_and_relay_recovery(self):
        """Require durable writes before accepting custody and reopen the successful spool."""
        store = RelayStore(self.root / "spool", self.codec)
        message = self.bundle(); blob = self.codec.encode(message)
        with patch("secure_records.os.fsync", side_effect=OSError("disk failure")):
            with self.assertRaises(OSError): store.accept(message, blob)
        self.assertFalse(store.pending)
        store.accept(message, blob)
        recovered = RelayStore(store.directory, Codec(self.key))
        self.assertEqual(recovered.pending[record_id(message)], blob)
        with patch("secure_records.os.fsync", side_effect=OSError("disk failure")):
            with self.assertRaises(OSError): GroundStore(self.root / "ground", self.codec).accept(message, blob)

    def test_restart_sessions_do_not_collide(self):
        """Keep identical sequence numbers from distinct sessions in separate spool entries."""
        store = RelayStore(self.root / "spool", self.codec)
        for session in (b"a" * 16, b"b" * 16):
            message = dict(self.bundle(), session=session)
            store.accept(message, self.codec.encode(message))
        self.assertEqual(len(store.pending), 2)

    @staticmethod
    def port():
        """Ask the OS for a loopback UDP port; it is no longer reserved on return."""
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.bind(("127.0.0.1", 0))
            return sock.getsockname()[1]

    def continuous_receive(self, records, directory):
        """Feed authenticated packets to the receiver without real sockets and check cleanup."""
        codec = Codec(self.key, session=b"c" * 16)
        packets = [(codec.encode(record), ("127.0.0.1", 1)) for record in records]
        with patch.object(continuous.socket, "socket") as factory:
            factory.return_value.recvfrom.side_effect = packets
            continuous.ground(0, directory, None, codec)
            factory.return_value.close.assert_called_once()

    def test_continuous_missing_segments(self):
        """Reject EOF when any declared segment is absent, incomplete, or misnumbered."""
        records = [dict(segment=i, part=0, count=1, payload=bytes([i])) for i in range(3)]
        cases = [[records[i] for i in range(3) if i != missing] for missing in range(3)]
        cases += [[], [dict(records[0], count=2)], [records[0], records[1], dict(records[2], segment=3)]]
        for index, case in enumerate(cases):
            with self.subTest(case=index), self.assertRaisesRegex(RuntimeError, "incomplete"):
                self.continuous_receive(case + [dict(eof=True, segments=3)], self.root / str(index))

    def test_continuous_eof_schema(self):
        """Reject invalid segment counts but allow EOF for a run with no segments."""
        for value in (None, True, -1, "0", 0.0):
            with self.subTest(value=value), self.assertRaises(ValueError):
                eof = dict(eof=True)
                if value is not None: eof["segments"] = value
                self.continuous_receive([eof], self.root)
        self.continuous_receive([dict(eof=True, segments=0)], self.root)

    def test_continuous_reordering_duplicates_and_empty_segment(self):
        """Reassemble out-of-order chunks and preserve a zero-byte segment across replay."""
        source = self.root / "source.ts"; source.write_bytes(b"abcdef")
        empty = self.root / "empty.ts"; empty.write_bytes(b"")
        codec = Codec(self.key, session=b"c" * 16)
        with patch.object(continuous.socket, "socket") as factory:
            continuous.moon(1, [source, empty], 3, codec)
            records = [codec.decode(call.args[0]) for call in factory.return_value.sendto.call_args_list]
        self.assertEqual(records[-1]["segments"], 2)
        self.assertEqual(records[-2]["payload"], b"")
        self.continuous_receive(list(reversed(records[:-1])) + [records[0], records[-1]], self.root / "output")
        self.assertEqual((self.root / "output/ground-000000.ts").read_bytes(), b"abcdef")
        self.assertEqual((self.root / "output/ground-000001.ts").read_bytes(), b"")

    def test_custody_end_to_end_with_lost_ack(self):
        """Drop a ground ACK on loopback and verify retransmission drains the relay spool."""
        relay_port, ground_port = self.port(), self.port()
        while ground_port == relay_port: ground_port = self.port()
        relay_store = RelayStore(self.root / "spool", self.codec)
        ground_store = GroundStore(self.root / "ground", self.codec)
        source = self.root / "segment-000000.ts"; source.write_bytes(b"abcdefghij")
        stop = threading.Event(); errors = []; threads = []
        real_send = socket.socket.sendto
        dropped = []
        def lossy_send(sock, data, address):
            """Drop only the first ground ACK and report success to its sender."""
            message = self.codec.decode(data)
            if message.get("kind") == "ground_ack" and not dropped:
                dropped.append(True)
                return len(data)
            return real_send(sock, data, address)
        def run(function, *args):
            """Run a network worker and request shutdown if it raises."""
            try: function(*args)
            except BaseException as exc: errors.append(exc); stop.set()
        with patch.object(socket.socket, "sendto", lossy_send):
            try:
                for fn, args in [(ground, (ground_port, relay_port, ground_store)),
                                 (relay, (relay_port, ground_port, relay_store))]:
                    ready = threading.Event()
                    thread = threading.Thread(target=run, args=(fn, *args, self.codec, ready, stop))
                    thread.start(); threads.append(thread)
                    self.assertTrue(ready.wait(2))
                moon(relay_port, [source], 3, self.codec)
            finally:
                stop.set()
                for thread in threads: thread.join(2)
        self.assertFalse(errors)
        self.assertTrue(dropped)
        self.assertFalse(relay_store.pending)
        self.assertEqual((self.root / "ground/ground-000000.ts").read_bytes(), source.read_bytes())

    def test_all_demo_entrypoints_roundtrip(self):
        """Run each public demo CLI and compare its output bytes with the input."""
        scripts = Path(__file__).resolve().parent
        source_dir = self.root / "segments"; source_dir.mkdir()
        source = source_dir / "segment-000000.ts"; source.write_bytes(b"demo video data" * 4)
        cases = [
            ("bpnode_video_demo.py", ["--input-video", str(source), "--output-video", str(self.root / "video.bin"), "--frame-bytes", "20"], self.root / "video.bin"),
            ("continuous_video_demo.py", ["--segment-dir", str(source_dir), "--output-dir", str(self.root / "continuous"), "--chunk-bytes", "20"], self.root / "continuous/ground-000000.ts"),
            ("custody_video_demo.py", ["--segment-dir", str(source_dir), "--output-dir", str(self.root / "custody"), "--spool", str(self.root / "spool"), "--chunk-bytes", "20"], self.root / "custody/ground-000000.ts")]
        for script, args, output in cases:
            port1, port2 = self.port(), self.port()
            while port2 == port1: port2 = self.port()
            proc = subprocess.run([sys.executable, str(scripts / script), *args,
                                   "--moon-port", str(port1), "--ground-port", str(port2)],
                                  capture_output=True, text=True, timeout=20)
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            self.assertEqual(output.read_bytes(), source.read_bytes())


if __name__ == "__main__":
    unittest.main(verbosity=2)
