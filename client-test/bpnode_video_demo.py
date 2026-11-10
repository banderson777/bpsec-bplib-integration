#!/usr/bin/env python3
"""Three-node authenticated UDP video simulation.

The transport is deliberately small and deterministic so it can run without
cFS. Each datagram is an AES-GCM protected CBOR record, not a BPSec bundle.
Replace the source/relay/sink
threads with BPNode UDP CLAs when running under cFS.
"""
import argparse, os, socket, threading, time
from pathlib import Path
from secure_records import Codec, load_key

def source(port, count, size, input_path=None, codec=None):
    """Send opaque file chunks or deterministic frames, then authenticated EOF."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    if input_path:
        raw = open(input_path, "rb").read()
        chunks = [raw[i:i + size] for i in range(0, len(raw), size)]
    else:
        chunks = [bytes(((seq * 17 + i) % 256 for i in range(size))) for seq in range(count)]
    for seq, frame in enumerate(chunks):
        bundle = {"bp": 7, "src": "ipn:moon.1", "dst": "ipn:ground.1",
                  "seq": seq, "total": len(chunks), "payload": frame}
        sock.sendto(codec.encode(bundle), ("127.0.0.1", port))
        print(f"[moon] generated encrypted frame {seq} ({len(frame)} bytes)")
    sock.sendto(codec.encode({"eof": True}), ("127.0.0.1", port))
    sock.close()

def relay(in_port, out_port, codec):
    """Authenticate loopback datagrams and forward unchanged bytes until EOF."""
    rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rx.bind(("127.0.0.1", in_port))
    tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    while True:
        data, _ = rx.recvfrom(65535)
        msg = codec.decode(data)
        tx.sendto(data, ("127.0.0.1", out_port))
        if msg.get("eof"):
            print("[relay] forwarded end-of-stream")
            break
        print(f"[relay] forwarded bundle seq={msg['seq']} ({len(data)} bytes)")
    rx.close(); tx.close()

def ground(port, expected, output_path=None, live=False, codec=None):
    """Deduplicate/order chunks, optionally writing contiguous live output.

    Fail incomplete transfers at EOF. Output is truncated for a new recording;
    this path has no durable custody or retransmission.
    """
    rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rx.bind(("127.0.0.1", port))
    received = set(); chunks = {}; next_seq = 0
    live_out = open(output_path, "wb") if output_path and live else None
    while True:
        data, _ = rx.recvfrom(65535)
        msg = codec.decode(data)
        if msg.get("eof"):
            break
        frame = msg["payload"]
        if type(msg["seq"]) is not int or not 0 <= msg["seq"] < expected or msg["total"] != expected:
            raise ValueError("invalid sequence metadata")
        if msg["seq"] in received:
            continue
        received.add(msg["seq"])
        chunks[msg["seq"]] = frame
        if live_out:
            while next_seq in chunks:
                live_out.write(chunks.pop(next_seq)); live_out.flush(); next_seq += 1
        # Console display: checksum and a compact luminance bar.
        avg = sum(frame) // len(frame)
        bar = " .:-=+*#%@"[avg * 10 // 256]
        print(f"[ground] decrypted/displayed frame {msg['seq']}: {bar * 32} sha={sum(frame) % 65536:04x}")
    rx.close()
    if live_out: live_out.close()
    if len(received) != expected:
        raise RuntimeError(f"expected {expected} frames, got {len(received)}")
    if output_path and not live:
        with open(output_path, "wb") as out:
            for seq in sorted(chunks): out.write(chunks[seq])
        print(f"[ground] reassembled video: {output_path} ({sum(map(len, chunks.values()))} bytes)")
    print(f"[ground] video complete: {len(received)} frames displayed")

def main():
    """Parse CLI, share a fresh session/key, run workers, and check completion."""
    p = argparse.ArgumentParser()
    p.add_argument("--frames", type=int, default=5)
    p.add_argument("--frame-bytes", type=int, default=256)
    p.add_argument("--input-video", help="video file to transmit instead of synthetic frames")
    p.add_argument("--output-video", help="reassembled output file at ground")
    p.add_argument("--live-output", action="store_true",
                   help="write decrypted chunks immediately (use fragmented MP4/MPEG-TS)")
    p.add_argument("--moon-port", type=int, default=14556)
    p.add_argument("--ground-port", type=int, default=24556)
    p.add_argument("--key-file", help="optional private 32-byte key file")
    a = p.parse_args()
    if not 1 <= a.frame_bytes <= 60000 or a.frames < 0:
        p.error("invalid frame size or count")
    codec = Codec(load_key(a.key_file), session=os.urandom(16))
    if a.input_video:
        input_path = Path(a.input_video)
        if not input_path.is_file() and not input_path.is_absolute():
            input_path = Path(__file__).resolve().parent / input_path
        if not input_path.is_file():
            raise SystemExit(f"input video not found: {a.input_video}\n"
                             f"looked in {input_path}")
        a.input_video = str(input_path)
        a.frames = (input_path.stat().st_size + a.frame_bytes - 1) // a.frame_bytes
    errors = []
    def worker(function, *arguments):
        """Run a relay or ground worker and pass any failure back to the main thread."""
        try:
            function(*arguments)
        except BaseException as exc:
            errors.append(exc)
    g = threading.Thread(target=worker, args=(ground, a.ground_port, a.frames, a.output_video, a.live_output, codec), daemon=True)
    r = threading.Thread(target=worker, args=(relay, a.moon_port, a.ground_port, codec), daemon=True)
    g.start(); r.start(); time.sleep(0.05)
    source(a.moon_port, a.frames, a.frame_bytes, a.input_video, codec)
    r.join(5); g.join(5)
    if r.is_alive() or g.is_alive(): raise SystemExit("node test timed out")
    if errors: raise RuntimeError("video worker failed") from errors[0]

if __name__ == "__main__": main()
