#!/usr/bin/env python3
"""Authenticated custody simulation with durable receipts and retries."""
import argparse
import glob
import os
import socket
import tempfile
import threading
import time
from pathlib import Path

from cryptography.exceptions import InvalidTag
from secure_records import (MAX_SEGMENT_BYTES, MAX_SEGMENT_PARTS, Codec, GroundStore, durable_write, load_key,
                            record_id, sync_directory, validate_bundle)

REJECTED = (ValueError, TypeError, KeyError, InvalidTag)


class RelayStore:
    """Own a durable encrypted spool and pending records; callers serialize use.

    Callers must authenticate records before accept/acknowledge.
    """
    def __init__(self, directory, codec):
        """Load/validate spool records; fail on old formats or filename mismatch."""
        self.directory = Path(directory)
        self.directory.mkdir(parents=True, exist_ok=True)
        sync_directory(self.directory.parent)
        self.codec = codec
        self.pending = {}
        for path in self.directory.glob("*.cbor"):
            blob = path.read_bytes()
            message = codec.decode(blob)
            validate_bundle(message)
            if path.name != record_id(message) + ".cbor":
                raise ValueError(f"unsupported or mismatched spool file: {path}")
            self.pending[record_id(message)] = blob

    def accept(self, message, blob):
        """Persist decoded blob before custody ACK; reject conflicting duplicates."""
        validate_bundle(message)
        identity = record_id(message)
        if identity in self.pending and self.codec.decode(self.pending[identity]) != message:
            raise ValueError("conflicting duplicate")
        durable_write(self.directory / (identity + ".cbor"), blob)
        self.pending[identity] = blob

    def acknowledge(self, message):
        """Release a pending record after its removal is durable.

        A directory sync failure leaves the record pending in memory. Repeating
        the ACK retries the sync even when the previous unlink succeeded.
        Return False for an unknown or already completed acknowledgement.
        """
        if message.get("kind") != "ground_ack":
            raise ValueError("invalid acknowledgement")
        identity = record_id(message)
        if identity not in self.pending:
            return False
        (self.directory / (identity + ".cbor")).unlink(missing_ok=True)
        sync_directory(self.directory)
        del self.pending[identity]
        return True


def exchange(sock, blob, address, expected, codec, attempts=20):
    """Retry with 0.5-second windows until peer fields match; raise on exhaustion."""
    for _ in range(attempts):
        sock.sendto(blob, address)
        deadline = time.monotonic() + 0.5
        while time.monotonic() < deadline:
            sock.settimeout(max(0.001, deadline - time.monotonic()))
            try:
                response, sender = sock.recvfrom(65535)
                message = codec.decode(response)
                if sender == address and all(message.get(k) == v for k, v in expected.items()):
                    return
            except socket.timeout:
                break
            except REJECTED:
                continue
    raise RuntimeError("custody acknowledgement timed out")


def stage_segment(path, staged, session, segment, chunk, codec):
    """Stage one segment's authenticated records and return their lengths in wire order.

    Check the receipt and encoded-byte limits before the first custody exchange.
    Keep only one chunk and its encoding in memory. The caller owns the seekable
    temporary file and discards it on failure.
    """
    lengths = []
    with Path(path).open("rb") as source:
        size = os.fstat(source.fileno()).st_size
        count = max(1, (size + chunk - 1) // chunk)
        if count > MAX_SEGMENT_PARTS:
            raise ValueError("segment exceeds receipt limit")
        if size > MAX_SEGMENT_BYTES:
            raise ValueError("segment exceeds encoded byte budget")
        for part in range(count):
            expected = min(chunk, size - part * chunk)
            raw = source.read(expected)
            if len(raw) != expected:
                raise ValueError("segment shrank during staging")
            message = dict(kind="bundle", session=session, segment=segment, part=part,
                           count=count, seq=(segment << 32) | part, payload=raw)
            validate_bundle(message)
            blob = codec.encode(message)
            if staged.tell() + len(blob) > MAX_SEGMENT_BYTES:
                raise ValueError("segment exceeds encoded byte budget")
            staged.write(blob)
            lengths.append(len(blob))
        if source.read(1):
            raise ValueError("segment grew during staging")
    staged.seek(0)
    return lengths


def moon(port, files, chunk, codec):
    """Send each validated segment in order and wait for custody and final ACKs."""
    if not 1 <= chunk <= 60000:
        raise ValueError("chunk bytes must be between 1 and 60000")
    session = os.urandom(16)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as tx:
        for segment, path in enumerate(files):
            with tempfile.TemporaryFile() as staged:
                lengths = stage_segment(path, staged, session, segment, chunk, codec)
                for part, length in enumerate(lengths):
                    exchange(tx, staged.read(length), ("127.0.0.1", port),
                             dict(kind="custody_ack", session=session, seq=(segment << 32) | part), codec)
        exchange(tx, codec.encode(dict(kind="eof", session=session)), ("127.0.0.1", port),
                 dict(kind="eof_ack", session=session), codec)


def relay(port, ground_port, store, codec, ready, stop):
    """Persist before ACK, forward/retry pending bytes, and await ground receipts.

    Signal ready after bind and run until stop. Storage errors propagate to the
    worker wrapper; malformed/authentication-failed records are ignored.
    """
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind(("127.0.0.1", port)); sock.settimeout(0.05); ready.set()
        eof = None
        next_retry = 0
        while not stop.is_set():
            if time.monotonic() >= next_retry:
                for blob in list(store.pending.values()):
                    sock.sendto(blob, ("127.0.0.1", ground_port))
                next_retry = time.monotonic() + 0.2
            try:
                blob, sender = sock.recvfrom(65535)
                message = codec.decode(blob)
                kind = message.get("kind")
                if kind == "ground_ack" and sender == ("127.0.0.1", ground_port):
                    store.acknowledge(message)
                elif kind == "bundle":
                    store.accept(message, blob)
                    sock.sendto(codec.encode(dict(kind="custody_ack", session=message["session"],
                                                  seq=message["seq"])), sender)
                    sock.sendto(blob, ("127.0.0.1", ground_port))
                elif kind == "eof":
                    record_id(dict(message, seq=0))
                    eof = (message["session"], sender)
            except socket.timeout:
                pass
            except REJECTED:
                continue
            # Remain alive until the source has received this final ACK.
            if eof and not store.pending:
                sock.sendto(codec.encode(dict(kind="eof_ack", session=eof[0])), eof[1])


def ground(port, relay_port, store, codec, ready, stop):
    """Accept authenticated relay chunks durably before sending ground ACKs."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind(("127.0.0.1", port)); sock.settimeout(0.05); ready.set()
        while not stop.is_set():
            try:
                blob, sender = sock.recvfrom(65535)
                if sender != ("127.0.0.1", relay_port):
                    continue
                message = codec.decode(blob)
                store.accept(message, blob)
                sock.sendto(codec.encode(dict(kind="ground_ack", session=message["session"],
                                              seq=message["seq"])), sender)
            except socket.timeout:
                pass
            except REJECTED:
                continue


def main():
    """Run custody workers, retain the spool key, and surface worker errors."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--segment-dir", default="moon-segments")
    parser.add_argument("--chunk-bytes", type=int, default=900)
    parser.add_argument("--moon-port", type=int, default=14570)
    parser.add_argument("--ground-port", type=int, default=24570)
    parser.add_argument("--spool", default="relay-spool-v2")
    parser.add_argument("--output-dir", default="ground-segments")
    parser.add_argument("--key-file", help="persistent 32-byte key; defaults to SPOOL/demo.key")
    args = parser.parse_args()
    if not 1 <= args.chunk_bytes <= 60000:
        parser.error("chunk bytes must be between 1 and 60000")
    files = sorted(glob.glob(os.path.join(args.segment_dir, "segment-*.ts")))
    if not files:
        parser.error("no MPEG-TS segments found")
    codec = Codec(load_key(args.key_file or Path(args.spool) / "demo.key"))
    relay_store = RelayStore(args.spool, codec)
    ground_store = GroundStore(args.output_dir, codec)
    stop = threading.Event(); errors = []
    def worker(function, *arguments):
        """Record a relay or ground failure and signal the other worker to stop."""
        try:
            function(*arguments)
        except BaseException as exc:
            errors.append(exc); stop.set()
    threads = []
    try:
        for function, arguments in [(ground, (args.ground_port, args.moon_port, ground_store)),
                                    (relay, (args.moon_port, args.ground_port, relay_store))]:
            ready = threading.Event()
            thread = threading.Thread(target=worker, args=(function, *arguments, codec, ready, stop))
            thread.start(); threads.append(thread)
            if not ready.wait(2):
                raise RuntimeError("node startup failed")
        moon(args.moon_port, files, args.chunk_bytes, codec)
    finally:
        stop.set()
        for thread in threads:
            thread.join(2)
    if errors:
        raise RuntimeError("custody worker failed") from errors[0]
    print("Custody transfer complete: all chunks durably accepted at ground")


if __name__ == "__main__":
    main()
