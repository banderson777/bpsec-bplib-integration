"""Authenticated demo records and durable storage helpers (not BPSec wire format)."""
import os
import tempfile
from pathlib import Path

import cbor2
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

# Bound per-segment receipt metadata and encoded disk usage, including CBOR overhead.
MAX_SEGMENT_PARTS = 4096
MAX_SEGMENT_BYTES = 64 * 1024 * 1024
CONTEXT = b"bpnode-video-demo-v2"


def load_key(path=None):
    """Use an ephemeral run key, or exclusively create/reuse a private key file."""
    if path is None:
        return AESGCM.generate_key(bit_length=256)
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    except FileExistsError:
        key = path.read_bytes()
    else:
        key = AESGCM.generate_key(bit_length=256)
        with os.fdopen(fd, "wb") as stream:
            stream.write(key)
            stream.flush()
            os.fsync(stream.fileno())
        sync_directory(path.parent)
    if len(key) != 32:
        raise ValueError("key file must contain exactly 32 bytes")
    return key


class Codec:
    """Authenticate CBOR maps with random nonces and optional session binding.

    Wire bytes are nonce (12 bytes) plus AES-GCM ciphertext/tag, using CONTEXT
    as associated data. Consumers must validate message-specific fields.
    """
    def __init__(self, key, session=None):
        """Use an AES-GCM key and optionally insert/check a session ID."""
        self.aes = AESGCM(key)
        self.session = session

    def encode(self, message):
        """Return authenticated CBOR bytes with a fresh nonce; borrow the map."""
        if self.session is not None:
            message = dict(message, session=self.session)
        nonce = os.urandom(12)
        # All metadata, payload, and control fields are authenticated together.
        return nonce + self.aes.encrypt(nonce, cbor2.dumps(message), CONTEXT)

    def decode(self, blob):
        """Authenticate bytes and return a map; check size and optional session.

        Raises InvalidTag on authentication failure, ValueError for invalid
        size/type/session, or CBOR parsing errors. Does not validate map schema.
        """
        if len(blob) < 28 or len(blob) > 65507:
            raise ValueError("invalid record size")
        message = cbor2.loads(self.aes.decrypt(blob[:12], blob[12:], CONTEXT))
        if not isinstance(message, dict):
            raise ValueError("record must be a map")
        if self.session is not None and message.get("session") != self.session:
            raise ValueError("record belongs to another run")
        return message


def integer(value, maximum=(1 << 64) - 1):
    """Return an unsigned bounded int; reject bool and invalid values."""
    if type(value) is not int or not 0 <= value <= maximum:
        raise ValueError("invalid unsigned integer")
    return value


def record_id(message):
    """Validate session/sequence and return their safe filename identity."""
    session = message.get("session")
    if not isinstance(session, bytes) or len(session) != 16:
        raise ValueError("invalid session")
    seq = integer(message.get("seq"))
    return f"{session.hex()}-{seq}"


def sync_directory(path):
    """fsync a POSIX directory entry; Windows skips this operation."""
    if os.name != "nt":
        fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)


def durable_write(path, data):
    """Flush/fsync bytes, atomically replace path, then sync its directory.

    Create parents and clean temporary files on error. Propagate I/O failures;
    replacement overwrites previous destination contents.
    """
    durable_write_parts(path, (data,))


def durable_write_parts(path, parts):
    """Atomically publish a stream of byte chunks and discard partial output on error."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".pending-", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as stream:
            for data in parts:
                stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        sync_directory(path.parent)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def validate_bundle(message):
    """Validate custody schema and return (segment, part, count).

    Require a 16-byte session, unsigned IDs, consistent packed sequence,
    kind='bundle', and byte payload. Invalid metadata raises ValueError.
    """
    record_id(message)
    segment = integer(message.get("segment"), (1 << 32) - 1)
    part = integer(message.get("part"), 65535)
    count = integer(message.get("count"), 65536)
    if count > MAX_SEGMENT_PARTS:
        raise ValueError("segment exceeds receipt limit")
    if count == 0 or part >= count or message["seq"] != (segment << 32) | part:
        raise ValueError("invalid part/count/sequence")
    if message.get("kind") != "bundle" or not isinstance(message.get("payload"), bytes):
        raise ValueError("invalid bundle")
    return segment, part, count


class GroundStore:
    """Keep authenticated chunks durably, including across crashes/replays."""
    def __init__(self, directory, codec):
        """Create receipt/output storage and borrow the authenticated codec."""
        self.directory = Path(directory)
        self.directory.mkdir(parents=True, exist_ok=True)
        sync_directory(self.directory.parent)
        self.codec = codec

    def accept(self, message, blob):
        """Persist authenticated custody bytes before the caller sends an ACK.

        message must be codec.decode(blob). Publish complete segments and retain
        receipts for replay. Raise on conflicts/storage failure. Isolate output
        directories because segment filenames are shared across sessions.
        """
        segment, part, count = validate_bundle(message)
        session = message["session"].hex()
        chunks = self.directory / ".custody" / session / str(segment)
        chunks.mkdir(parents=True, exist_ok=True)
        # Persist new directory entries before acknowledging files within them.
        sync_directory(chunks.parent)
        sync_directory(chunks.parent.parent)
        sync_directory(self.directory)
        target = chunks / f"{part}.cbor"
        if target.exists() and self.codec.decode(target.read_bytes()) != message:
            raise ValueError("conflicting duplicate")
        stored_bytes = 0
        stored_parts = 0
        with os.scandir(chunks) as receipts:
            for receipt in receipts:
                if receipt.name.endswith(".cbor") and receipt.name != target.name:
                    if stored_parts == 0:
                        prior = self.codec.decode(Path(receipt.path).read_bytes())
                        if prior["count"] != count:
                            raise ValueError("inconsistent segment")
                    stored_bytes += receipt.stat().st_size
                    stored_parts += 1
        if stored_bytes + len(blob) > MAX_SEGMENT_BYTES:
            raise ValueError("segment exceeds encoded byte budget")
        durable_write(target, blob)
        if stored_parts + 1 == count:
            def payloads():
                """Read receipts in sequence and authenticate each before yielding its payload."""
                for index in range(count):
                    record = self.codec.decode((chunks / f"{index}.cbor").read_bytes())
                    if (validate_bundle(record) != (segment, index, count)
                            or record["session"] != message["session"]):
                        raise ValueError("inconsistent segment")
                    yield record["payload"]

            durable_write_parts(self.directory / f"ground-{segment:06d}.ts", payloads())
