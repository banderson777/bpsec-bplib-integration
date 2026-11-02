#!/usr/bin/env python3
"""Segmented continuous-feed DTN video demo (moon -> relay -> ground)."""
import argparse, glob, os, socket, subprocess, threading, time
from secure_records import Codec, load_key

def make_segments(src, out_dir, seconds):
    """Run FFmpeg segmentation with continuous timestamps; propagate errors.

    Existing matching files can be overwritten. Use a fresh directory to avoid
    stale trailing segments from an earlier, longer input.
    """
    os.makedirs(out_dir, exist_ok=True)
    cmd = ["ffmpeg", "-y", "-i", src, "-c:v", "libx264", "-preset", "veryfast",
           "-g", "30", "-f", "segment", "-segment_time", str(seconds),
           "-segment_format", "mpegts", "-reset_timestamps", "0",
           os.path.join(out_dir, "segment-%06d.ts")]
    subprocess.run(cmd, check=True)

def moon(port, files, chunk_size, codec):
    """Send each segment's encrypted chunks over loopback UDP, then EOF."""
    tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    for seg, path in enumerate(files):
        with open(path, "rb") as stream:
            data = stream.read()
        chunks = [data[i:i + chunk_size] for i in range(0, len(data), chunk_size)] or [b""]
        for part, chunk in enumerate(chunks):
            msg = {"bp": 7, "src": "ipn:moon.1", "dst": "ipn:ground.1",
                   "segment": seg, "part": part, "count": len(chunks), "payload": chunk}
            tx.sendto(codec.encode(msg), ("127.0.0.1", port))
            print(f"[moon] segment {seg} chunk {part}/{len(chunks)-1} encrypted")
            time.sleep(0.01)
    tx.sendto(codec.encode({"eof": True, "segments": len(files)}), ("127.0.0.1", port)); tx.close()

def relay(in_port, out_port, codec):
    """Authenticate and forward unchanged datagrams until authenticated EOF."""
    rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); rx.bind(("127.0.0.1", in_port))
    tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    while True:
        data, _ = rx.recvfrom(65535); msg = codec.decode(data); tx.sendto(data, ("127.0.0.1", out_port))
        if msg.get("eof"): print("[relay] end-of-stream forwarded"); break
    rx.close(); tx.close()

def ground(port, out_dir, player, codec):
    """Assemble parts and publish/play segments in completion order.

    Atomically replace files; player startup failure falls back to disk.
    EOF must declare the exact segment count; missing/partial segments fail.
    Empty segments are represented by one empty part. No retry or inter-segment
    ordering exists.
    """
    rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); rx.bind(("127.0.0.1", port))
    os.makedirs(out_dir, exist_ok=True)
    pending = {}; counts = {}; completed = set(); next_seg = 0; proc = None
    if player:
        try:
            proc = subprocess.Popen([player, "-fflags", "nobuffer", "-flags", "low_delay", "-f", "mpegts", "-i", "-"], stdin=subprocess.PIPE)
        except (FileNotFoundError, OSError) as exc:
            print(f"[ground] unable to start player {player!r}: {exc}; saving segments instead")
    while True:
        data, _ = rx.recvfrom(65535); msg = codec.decode(data)
        if msg.get("eof"):
            expected = msg.get("segments")
            break
        seg, part = msg["segment"], msg["part"]
        count = msg["count"]
        if any(type(v) is not int for v in (seg, part, count)) or not (0 <= seg and 0 <= part < count <= 65536):
            raise ValueError("invalid segment metadata")
        if seg in completed: continue
        if counts.setdefault(seg, count) != count: raise ValueError("inconsistent segment count")
        chunk = msg["payload"]
        pending.setdefault(seg, {})[part] = chunk
        if len(pending[seg]) == count:
            assembled = b"".join(pending[seg][i] for i in sorted(pending[seg]))
            pending.pop(seg)
            completed.add(seg); counts.pop(seg)
            final_path = os.path.join(out_dir, f"ground-{seg:06d}.ts")
            temp_path = final_path + ".part"
            with open(temp_path, "wb") as f: f.write(assembled)
            os.replace(temp_path, final_path)
            if proc:
                proc.stdin.write(assembled); proc.stdin.flush()
            print(f"[ground] segment {seg} complete ({len(assembled)} bytes), displayed live")
            next_seg += 1
    if proc: proc.stdin.close(); proc.wait()
    rx.close(); print(f"[ground] continuous feed stopped after {next_seg} segments")
    if type(expected) is not int or expected < 0:
        raise ValueError("EOF requires a nonnegative integer segment count")
    # Compare cardinality and bounds without allocating range(expected).
    if pending or len(completed) != expected or any(seg >= expected for seg in completed):
        raise RuntimeError("incomplete segments at EOF")

def main():
    """Prepare segments, share a run session/key, run workers, and check errors."""
    p = argparse.ArgumentParser(); p.add_argument("--input-video"); p.add_argument("--segment-dir", default="moon-segments")
    p.add_argument("--segment-seconds", type=float, default=1.0); p.add_argument("--chunk-bytes", type=int, default=900)
    p.add_argument("--player", default=None, help="ffplay executable; omit to save ground segments only")
    p.add_argument("--moon-port", type=int, default=14560); p.add_argument("--ground-port", type=int, default=24560)
    p.add_argument("--key-file", help="optional private 32-byte key file")
    p.add_argument("--output-dir", default="ground-segments")
    a = p.parse_args()
    if not 1 <= a.chunk_bytes <= 60000: p.error("invalid chunk size")
    codec = Codec(load_key(a.key_file), session=os.urandom(16))
    if a.input_video: make_segments(a.input_video, a.segment_dir, a.segment_seconds)
    files = sorted(glob.glob(os.path.join(a.segment_dir, "segment-*.ts")))
    if not files: raise SystemExit("no MPEG-TS segments found; provide --input-video or --segment-dir")
    errors = []
    def worker(function, *arguments):
        try: function(*arguments)
        except BaseException as exc: errors.append(exc)
    g = threading.Thread(target=worker, args=(ground, a.ground_port, a.output_dir, a.player, codec), daemon=True); g.start()
    r = threading.Thread(target=worker, args=(relay, a.moon_port, a.ground_port, codec), daemon=True); r.start(); time.sleep(.1)
    moon(a.moon_port, files, a.chunk_bytes, codec); r.join(10); g.join(10)
    if r.is_alive() or g.is_alive(): raise SystemExit("continuous feed timed out")
    if errors: raise RuntimeError("continuous worker failed") from errors[0]

if __name__ == "__main__": main()
