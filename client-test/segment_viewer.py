#!/usr/bin/env python3
"""Wait for ground MPEG-TS segments and display them continuously."""
import argparse, os, shutil, subprocess, threading, time

def main():
    """Poll contiguous ground-NNNNNN.ts files from zero and optionally archive.

    Default playback starts one player per segment. Persistent playback pipes
    through FFmpeg to one player, buffering --buffer-segments files initially.
    No EOF is inferred; stop explicitly when the feed has ended.
    """
    p = argparse.ArgumentParser(); p.add_argument("--dir", default="ground-segments")
    p.add_argument("--archive-dir", help="move displayed segments here after playback")
    p.add_argument("--player", default="ffplay"); p.add_argument("--poll", type=float, default=.2)
    p.add_argument("--persistent", action="store_true",
                   help="pipe all segments to one player (requires continuous timestamps)")
    p.add_argument("--buffer-segments", type=int, default=3,
                   help="segments to queue before starting persistent playback")
    a = p.parse_args(); os.makedirs(a.dir, exist_ok=True)
    if a.archive_dir: os.makedirs(a.archive_dir, exist_ok=True)
    player = None
    remux = None
    if a.persistent:
        try:
            # Keep one player open, but normalize TS continuity/timestamps first.
            remux = subprocess.Popen(["ffmpeg", "-loglevel", "error", "-probesize", "32k", "-analyzeduration", "100000", "-fflags", "+genpts+discardcorrupt", "-f", "mpegts", "-i", "-", "-c", "copy", "-muxdelay", "0", "-muxpreload", "0", "-f", "mpegts", "-"], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
            player = subprocess.Popen([a.player, "-probesize", "32k", "-analyzeduration", "100000", "-fflags", "+nobuffer+genpts+discardcorrupt", "-flags", "low_delay", "-sync", "ext", "-f", "mpegts", "-i", "-"], stdin=remux.stdout)
        except OSError as exc:
            raise SystemExit(f"cannot start persistent viewer (requires ffmpeg and {a.player}): {exc}")
    seq = 0
    print(f"[viewer] waiting for segments in {os.path.abspath(a.dir)}")
    try:
        if a.persistent and a.buffer_segments > 0:
            print(f"[viewer] buffering {a.buffer_segments} segments before playback")
            while not all(os.path.isfile(os.path.join(a.dir, f"ground-{i:06d}.ts")) for i in range(a.buffer_segments)):
                time.sleep(a.poll)
        while player is None or player.poll() is None:
            path = os.path.join(a.dir, f"ground-{seq:06d}.ts")
            if os.path.isfile(path):
                if player is None:
                    try:
                        subprocess.run([a.player, "-autoexit", "-fflags", "nobuffer", path], check=False)
                    except OSError as exc:
                        raise SystemExit(f"cannot start viewer {a.player!r}: {exc}")
                else:
                    with open(path, "rb") as src:
                        while True:
                            block = src.read(64 * 1024)
                            if not block: break
                            if remux: remux.stdin.write(block); remux.stdin.flush()
                            else: player.stdin.write(block); player.stdin.flush()
                if a.archive_dir:
                    shutil.move(path, os.path.join(a.archive_dir, os.path.basename(path)))
                    print(f"[viewer] displayed and archived segment {seq}")
                else:
                    print(f"[viewer] displayed segment {seq}")
                seq += 1
            else: time.sleep(a.poll)
    finally:
        if player is not None and player.poll() is None:
            if remux: remux.stdin.close(); remux.wait()
            else: player.stdin.close()
            player.wait()

if __name__ == "__main__": main()
