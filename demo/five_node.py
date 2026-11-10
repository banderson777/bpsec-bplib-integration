#!/usr/bin/env python3
"""Build, prepare, run, and verify the five-process BPNode/cFS Linux lab.

Use separate Linux or Windows/WSL terminals for `node`. The controller never
loads endpoint keys into relay processes. Runtime state is isolated per run.
"""
import argparse
import csv
import errno
import functools
import fcntl
import hashlib
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import json
import math
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parents[1]


def build(directory):
    """Build the endpoint bridge and a dedicated native cFS mission."""
    directory.mkdir(parents=True, exist_ok=True)
    subprocess.run(['cmake', '-S', str(ROOT / 'integration'), '-B', str(directory / 'integration')], check=True)
    subprocess.run(['cmake', '--build', str(directory / 'integration'), '--target', 'bplib_bsl_security', '-j4'], check=True)
    mission = directory / 'mission'
    if not mission.exists(): shutil.copytree(ROOT / 'cfs-buildconfig', mission)
    (mission / 'targets.cmake').write_text('''set(MISSION_NAME "FiveNode")
set(SPACECRAFT_ID 0xb9)
set(MISSION_CPUNAMES cpu1)
set(cpu1_PROCESSORID 1)
set(cpu1_PLATFORM default)
set(cpu1_PSP_MODULELIST udpsock_intf)
set(MISSION_GLOBAL_APPLIST bpnode bplib sch_lab)
''')
    original = (ROOT / 'cfs-buildconfig/global_build_options.cmake').read_text()
    security = directory / 'integration/libbplib_bsl_security.so'
    (mission / 'global_build_options.cmake').write_text(original + f'''
set(BPNODE_ENABLE_FIVE_NODE_DEMO ON)
set(BPNODE_DEMO_SECURITY_LIBRARY "{security.as_posix()}")
add_compile_definitions(BPLIB_STOR_INSERTBATCHSIZE=1)
if(IS_CFS_ARCH_BUILD)
  find_package(qcbor 1.6 CONFIG REQUIRED HINTS "{ROOT.as_posix()}/BSL/testroot/usr")
  get_target_property(QCBOR_INCLUDE_DIRS qcbor::qcbor INTERFACE_INCLUDE_DIRECTORIES)
  add_library(qcbor INTERFACE IMPORTED)
  target_link_libraries(qcbor INTERFACE qcbor::qcbor)
endif()
''')
    (mission / 'bpnode_platform_cfg.h').write_text(f'''#include "{ROOT.as_posix()}/bpnode/config/default_bpnode_platform_cfg.h"
#undef BPNODE_CLA_IN_STACK_SIZE
#undef BPNODE_CLA_OUT_STACK_SIZE
#define BPNODE_CLA_IN_STACK_SIZE 262144
#define BPNODE_CLA_OUT_STACK_SIZE 262144
''')
    (mission / 'cfe_es_startup.scr').write_text('''CFE_LIB, bplib, BPLib_PrintVersion, BP_LIB, 0, 0, 0x0, 0;
CFE_APP, bpnode, BPNode_AppMain, BPNODE, 20, 262144, 0x0, 0;
''')
    (mission / 'tables/sch_lab_table.c').write_text('''#include "cfe_tbl_filedef.h"
#include "sch_lab_tbl.h"
#include "cfe_sb.h"
#include "bpnode_msgids.h"
SCH_LAB_ScheduleTable_t Schedule = {
    .TickRate = 10,
    .Config = {{CFE_SB_MSGID_WRAP_VALUE(BPNODE_WAKEUP_MID), 1, 0}}
};
CFE_TBL_FILEDEF(Schedule, SCH_LAB.Schedule, Five node schedule, sch_lab_table.tbl)
''')
    env = dict(os.environ)
    env['CFS_APP_PATH'] = ':'.join(str(path) for path in [ROOT, ROOT / 'bpnode/psp_mod', ROOT / 'cfs-bundle/apps', ROOT / 'cfs-bundle/libs', ROOT / 'cfs-bundle/psp/fsw/modules', ROOT / 'cfs-bundle/cfe/modules'])
    subprocess.run(['cmake', '-S', str(ROOT / 'cfs-bundle/cfe'), '-B', str(directory / 'cfs'), '-G', 'Unix Makefiles', '-DSIMULATION=native', '-DMISSIONCONFIG=five_node', f'-DMISSION_DEFS={mission}', f'-DCMAKE_INSTALL_PREFIX={directory / "install"}'], env=env, check=True)
    subprocess.run(['cmake', '--build', str(directory / 'cfs'), '--target', 'mission-all', '-j4'], env=env, check=True)
    subprocess.run(['cmake', '--build', str(directory / 'cfs'), '--target', 'mission-install'], env=env, check=True)
    shutil.copy2(mission / 'cfe_es_startup.scr', directory / 'install/cpu1/cf/cfe_es_startup.scr')


def initialize_run(directory, build_dir):
    """Create isolated node runtimes and a shared session/key for a new run."""
    if directory.exists(): raise ValueError('run directory already exists; use a new path or resume with node')
    installed = build_dir / 'install/cpu1'
    if not (installed / 'core-cpu1').is_file(): raise ValueError('build the demo first')
    directory.mkdir(parents=True, mode=0o700)
    session = os.urandom(16); key = os.urandom(32)
    for number in range(1, 6):
        node_dir = directory / f'node{number}'
        shutil.copytree(installed, node_dir)
        (node_dir / 'session').write_bytes(session)
        if number in (1, 5):
            with open(os.open(node_dir / 'video.key', os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600), 'wb') as stream: stream.write(key)
    inputs = directory / 'node1/input'; inputs.mkdir()
    # Optional BPNODE_DEMO_FEED_GATE fault tests remove this marker before
    # launch and recreate it to release the source at a chosen checkpoint.
    (inputs / 'release').write_text('ready\n')
    return session, inputs


def write_json_atomic(path, value):
    """Replace a JSON file atomically so readers see the old or new document.

    The caller must serialize writers to the same path. This is a visibility
    guarantee for live readers, not a power-loss durability guarantee.
    """
    temporary = path.with_name(path.name + '.tmp')
    temporary.write_text(json.dumps(value, indent=2))
    os.replace(temporary, path)


def prepare(directory, build_dir, segments, chunk):
    """Create a new finite recording with isolated nodes, keys, and queued ADUs."""
    files = sorted(segments.glob('segment-*.ts'))
    if not files: raise ValueError('no segment-*.ts input files')
    if len(files) > 100000: raise ValueError('at most 100000 segments are supported')
    if not 1 <= chunk <= 4096: raise ValueError('chunk size must be 1..4096')
    session, inputs = initialize_run(directory, build_dir)
    manifest = []; sequence = 0
    for segment, path in enumerate(files):
        data = path.read_bytes()
        pieces = [data[i:i + chunk] for i in range(0, len(data), chunk)] or [b'']
        if len(pieces) > 65536: raise ValueError('segment has too many parts')
        manifest.append(dict(segment=segment, size=len(data), sha256=hashlib.sha256(data).hexdigest()))
        for part, piece in enumerate(pieces):
            record = b'BPD1' + session + struct.pack('!IIIII', segment, part, len(pieces), len(files), len(piece)) + piece
            (inputs / f'{sequence:08d}.adu').write_bytes(record); sequence += 1
    (inputs / f'{sequence:08d}.adu').write_bytes(b'BPE1' + session + struct.pack('!IIIII', len(files), 0, 0, len(files), 0))
    (directory / 'manifest.json').write_text(json.dumps(dict(session=session.hex(), segments=manifest, records=sequence + 1, build=str(build_dir)), indent=2))
    print(f'Prepared {len(files)} segments / {sequence + 1} records in {directory}')


def prepare_stream(directory, build_dir):
    """Create a run whose source ADU queue remains open until the watcher ends."""
    session, inputs = initialize_run(directory, build_dir)
    (inputs / 'stream').write_text('streaming input\n')
    (inputs / 'file-ack').write_bytes(bytes(4))
    write_json_atomic(directory / 'manifest.json', dict(
        session=session.hex(), segments=[], files=[], records=0, build=str(build_dir), streaming=True
    ))
    print(f'Prepared streaming run in {directory}')


def segment_records(session, segment_id, data, chunk):
    """Encode a segment as streaming BPD1 records with an unknown final count."""
    part_count = max(1, (len(data) + chunk - 1) // chunk)
    for part in range(part_count):
        piece = data[part * chunk:(part + 1) * chunk]
        yield b'BPD1' + session + struct.pack(
            '!IIIII', segment_id, part, part_count, 0, len(piece)
        ) + piece


def file_marker(session, file_number):
    """Create a local-only node-1 queue marker following one input file."""
    return b'BPF1' + session + struct.pack('!I', file_number)


def file_completion_record(session, file_number, first_segment, segment_count):
    """Create the protected end-of-video record consumed by node 5."""
    return b'BPE2' + session + struct.pack('!III', file_number, first_segment, segment_count)


def stream_file_segments(source, temporary_directory, segment_duration):
    """Transcode one stable input into a list of (MPEG-TS bytes, duration) pairs.

    FFmpeg must be on PATH. Its output is staged under temporary_directory;
    segment bytes are loaded into memory. A failed transcode raises
    subprocess.CalledProcessError, and an empty result raises ValueError.
    """
    output_pattern = temporary_directory / 'segment-%06d.ts'
    segment_list = temporary_directory / 'segments.csv'
    subprocess.run([
        'ffmpeg', '-hide_banner', '-loglevel', 'error', '-i', str(source), '-an',
        '-c:v', 'mpeg2video', '-force_key_frames', f'expr:gte(t,n_forced*{segment_duration})',
        '-f', 'segment', '-segment_time', str(segment_duration),
        '-segment_list', str(segment_list), '-segment_list_type', 'csv', str(output_pattern),
    ], check=True)
    segments = []
    with segment_list.open(newline='') as stream:
        for row in csv.reader(stream):
            if len(row) != 3:
                raise ValueError(f'invalid FFmpeg segment list row: {row!r}')
            path = Path(row[0])
            if not path.is_absolute():
                path = temporary_directory / path
            # FFmpeg lists start and end timestamps on a shared time axis.
            duration = float(row[2]) - float(row[1])
            if not math.isfinite(duration) or duration <= 0:
                raise ValueError(f'invalid FFmpeg segment duration for {path}')
            segments.append((path.read_bytes(), duration))
    if not segments:
        raise ValueError(f'ffmpeg produced no video segments for {source}')
    return segments


def watch_folder(directory, watch_directory, pattern, poll_interval, stable_seconds,
                 segment_duration, chunk):
    """Queue stable input files in modification-time order for one streaming run.

    Use one watcher per run. Intervals and segment duration are in seconds;
    chunk is the maximum record payload in bytes (1–4096). A source is renamed
    only after node 1 acknowledges its queue marker. Ctrl+C while idle queues
    EOF, while an interrupted file requires a fresh run.
    """
    if poll_interval <= 0 or stable_seconds < 0 or segment_duration <= 0:
        raise ValueError('poll interval and segment duration must be positive; stable time cannot be negative')
    if not 1 <= chunk <= 4096:
        raise ValueError('chunk size must be 1..4096')
    if not watch_directory.is_dir():
        raise ValueError(f'watch directory does not exist: {watch_directory}')
    manifest_path = directory / 'manifest.json'
    manifest = json.loads(manifest_path.read_text())
    if not manifest.get('streaming'):
        raise ValueError('run is not streaming; create it with prepare-stream')
    inputs = directory / 'node1/input'
    if (inputs / 'eof').exists():
        raise ValueError('stream input is already closed')
    session = bytes.fromhex(manifest['session'])
    state_path = directory / 'watch-state.json'
    state = json.loads(state_path.read_text()) if state_path.exists() else {'processed': []}
    processed = set(state['processed'])
    seen = {}
    record_number = int(manifest['records'])
    ack_path = inputs / 'file-ack'

    def acknowledged_file_number():
        raw = ack_path.read_bytes()
        if len(raw) != 4:
            raise RuntimeError(f'invalid node-1 file acknowledgement: {ack_path}')
        return int.from_bytes(raw, 'big')

    for file_entry in manifest.get('files', []):
        if file_entry.get('accepted_by_node1'):
            original = watch_directory / file_entry['source']
            sent_path = original.with_name(original.name + '.sent')
            if original.exists() and not sent_path.exists():
                os.replace(original, sent_path)
            processed.add(file_entry['identity'])
    write_json_atomic(state_path, {'processed': sorted(processed)})
    if manifest.get('files') and not manifest['files'][-1].get('accepted_by_node1'):
        raise RuntimeError('stream has a file not acknowledged by node 1; discard this run and prepare a fresh stream')

    def file_identity(path, stat):
        return f'{path.resolve()}|{stat.st_size}|{stat.st_mtime_ns}'

    def next_stable_file():
        candidates = sorted(
            (path for path in watch_directory.glob(pattern) if path.is_file()),
            key=lambda path: (path.stat().st_mtime_ns, path.name),
        )
        now = time.monotonic()
        for path in candidates:
            stat = path.stat()
            identity = file_identity(path, stat)
            if identity in processed:
                continue
            signature = (stat.st_size, stat.st_mtime_ns)
            previous = seen.get(identity)
            if previous is None or previous[0] != signature:
                seen[identity] = (signature, now)
                return None
            if now - previous[1] < stable_seconds:
                return None
            return path, identity, signature
        return None

    def queue_record(data):
        nonlocal record_number
        path = inputs / f'{record_number:08d}.adu'
        temporary = path.with_suffix('.tmp')
        temporary.write_bytes(data)
        os.replace(temporary, path)
        record_number += 1

    def process_file(path, identity, signature):
        nonlocal manifest
        sent_path = path.with_name(path.name + '.sent')
        if sent_path.exists():
            raise FileExistsError(f'sent marker already exists: {sent_path}')
        with tempfile.TemporaryDirectory(prefix='bpsec-watch-', dir=str(directory)) as temp_name:
            encoded_segments = stream_file_segments(path, Path(temp_name), segment_duration)
            after = path.stat()
            if (after.st_size, after.st_mtime_ns) != signature:
                raise RuntimeError(f'input changed while it was being transcoded: {path}')
            first_segment = len(manifest['segments'])
            if first_segment + len(encoded_segments) > 100000:
                raise ValueError('stream exceeds the 100000-segment limit')
            file_number = len(manifest.setdefault('files', [])) + 1
            file_entry = dict(
                file_number=file_number,
                source=path.name,
                first_segment=first_segment,
                segment_count=len(encoded_segments),
                identity=identity,
                size=signature[0],
                mtime_ns=signature[1],
                accepted_by_node1=False,
            )
            manifest['files'].append(file_entry)
            write_json_atomic(manifest_path, manifest)
            for offset, (data, duration) in enumerate(encoded_segments):
                segment_id = first_segment + offset
                pieces = [data[index:index + chunk] for index in range(0, len(data), chunk)] or [b'']
                if len(pieces) > 65536:
                    raise ValueError(f'{path} produced a segment with too many parts')
                manifest['segments'].append(dict(
                    segment=segment_id,
                    size=len(data),
                    sha256=hashlib.sha256(data).hexdigest(),
                    duration=duration,
                    discontinuity=offset == 0 and segment_id > 0,
                    source=path.name,
                ))
                manifest['records'] += len(pieces)
                for record in segment_records(session, segment_id, data, chunk):
                    queue_record(record)
                write_json_atomic(manifest_path, manifest)
            queue_record(file_completion_record(
                session, file_number, first_segment, len(encoded_segments)
            ))
            manifest['records'] += 1
            queue_record(file_marker(session, file_number))
            manifest['records'] += 1
            write_json_atomic(manifest_path, manifest)
        while acknowledged_file_number() < file_number:
            time.sleep(0.1)
        file_entry['accepted_by_node1'] = True
        write_json_atomic(manifest_path, manifest)
        os.replace(path, sent_path)
        processed.add(identity)
        write_json_atomic(state_path, {'processed': sorted(processed)})
        print(f'Node 1 accepted {path.name}; renamed to {sent_path.name} ({len(encoded_segments)} segment(s))', flush=True)

    print(f'Watching {watch_directory} for {pattern}; oldest stable files are queued first.', flush=True)
    processing_file = False
    try:
        while True:
            candidate = next_stable_file()
            if candidate:
                processing_file = True
                process_file(*candidate)
                processing_file = False
            else:
                time.sleep(poll_interval)
    except KeyboardInterrupt:
        if processing_file:
            raise RuntimeError('watcher interrupted while processing a file; discard this run and prepare a fresh stream') from None
        segment_count = len(manifest['segments'])
        eof_record = b'BPE1' + session + struct.pack('!IIIII', segment_count, 0, 0, segment_count, 0)
        queue_record(eof_record)
        manifest['records'] = record_number
        manifest['streaming'] = False
        write_json_atomic(manifest_path, manifest)
        (inputs / 'eof').write_text('end of stream\n')
        print(f'Input closed: {segment_count} segment(s), {record_number} record(s) queued.', flush=True)


def node_command(directory, number, port_base):
    """Return the native cFS invocation and environment for one isolated node."""
    manifest = json.loads((directory / 'manifest.json').read_text())
    env = dict(os.environ, BPNODE_DEMO_NODE=str(number), BPNODE_DEMO_PORT_BASE=str(port_base), CFE_RAM_MOUNT=f'/ram{number}')
    libraries = [Path(manifest['build']) / 'integration', ROOT / 'BSL/build/default/src', ROOT / 'BSL/testroot/usr/lib/x86_64-linux-gnu']
    env['LD_LIBRARY_PATH'] = ':'.join(str(path) for path in libraries) + ':' + env.get('LD_LIBRARY_PATH', '')
    return ['./core-cpu1', '-C', str(number), '-N', f'node{number}'], env


def check_nodes(processes, log_directory):
    """Check native host/app health and return whether every BPNode initialized."""
    ready = True
    for number, process in processes.items():
        if process.poll() is not None:
            raise RuntimeError(f'node {number} exited with {process.returncode}; inspect {log_directory}')
        log = log_directory / f'node{number}.log'
        text = ''
        if log.exists():
            with log.open('rb') as stream:
                stream.seek(getattr(process, 'log_offset', 0))
                try:
                    text = stream.read().decode(errors='replace')
                except OSError as exc:
                    if exc.errno in (errno.ENODATA, errno.EAGAIN, getattr(errno, 'EWOULDBLOCK', errno.EAGAIN)):
                        text = ''
                    else:
                        raise
        if 'Error initializing BPLib' in text:
            raise RuntimeError(f'BPNode application initialization failed on node {number}; inspect {log}')
        ready = ready and f'DEMO node={number} recovered ' in text
    return ready


def wait_for_nodes(processes, log_directory, timeout=30):
    """Wait for application readiness before querying live custody databases."""
    deadline = time.monotonic() + timeout
    while not check_nodes(processes, log_directory):
        if time.monotonic() >= deadline:
            raise RuntimeError(f'BPNode application startup timed out; inspect {log_directory}')
        time.sleep(0.1)


def verify(directory, timeout):
    """Wait up to timeout seconds for completion and verify the manifest outputs.

    Compare segment lengths and hashes, then check each archive against the
    bytewise concatenation of its segments. A timeout of zero checks immediately.
    """
    manifest = json.loads((directory / 'manifest.json').read_text())
    output = directory / 'node5/output'
    deadline = time.monotonic() + timeout
    while not (output / 'complete').exists():
        if time.monotonic() >= deadline: raise RuntimeError('MOC completion timed out; inspect node logs and retained databases')
        time.sleep(0.2)
    for entry in manifest['segments']:
        data = (output / f'ground-{entry["segment"]:06d}.ts').read_bytes()
        if len(data) != entry['size'] or hashlib.sha256(data).hexdigest() != entry['sha256']:
            raise RuntimeError(f'segment {entry["segment"]} differs from source')
    print(f'PASS: {len(manifest["segments"])} segments match source byte for byte')
    for file_entry in manifest.get('files', []):
        if not file_entry.get('accepted_by_node1'):
            raise RuntimeError(f'node 1 did not acknowledge {file_entry["source"]}')
        archive = output / 'videos' / f'video-{file_entry["file_number"]:06d}.ts'
        first = file_entry['first_segment']
        count = file_entry['segment_count']
        with archive.open('rb') as stream:
            for segment_id in range(first, first + count):
                data = (output / f'ground-{segment_id:06d}.ts').read_bytes()
                if stream.read(len(data)) != data:
                    raise RuntimeError(f'video archive {archive.name} differs at segment {segment_id}')
            if stream.read(1):
                raise RuntimeError(f'video archive {archive.name} has unexpected trailing bytes')
    if manifest.get('files'):
        print(f'PASS: {len(manifest["files"])} per-video archives match their received segments')


def live_playlist(output, entries, segment_duration, complete):
    """Build an HLS event playlist from currently published stream segments."""
    durations = [float(entry.get('duration', segment_duration)) for entry in entries]
    lines = [
        '#EXTM3U',
        '#EXT-X-VERSION:3',
        f'#EXT-X-TARGETDURATION:{math.ceil(max([segment_duration] + durations))}',
        '#EXT-X-MEDIA-SEQUENCE:0',
        '#EXT-X-PLAYLIST-TYPE:EVENT',
    ]
    published = 0
    for entry, duration in zip(entries, durations):
        segment_id = entry['segment']
        filename = f'ground-{segment_id:06d}.ts'
        if not (output / filename).is_file():
            break
        if entry.get('discontinuity'):
            lines.append('#EXT-X-DISCONTINUITY')
        lines.extend((f'#EXTINF:{duration:.3f},', filename))
        published += 1
    if published == len(entries) and complete:
        lines.append('#EXT-X-ENDLIST')
    return ('\n'.join(lines) + '\n').encode()


def view(directory, player, segment_duration, startup_timeout):
    """Play complete node-5 segments as they arrive, via a local HLS playlist."""
    manifest_path = directory / 'manifest.json'
    if not manifest_path.is_file():
        raise ValueError(f'run manifest not found: {manifest_path}')
    if segment_duration <= 0 or startup_timeout < 0:
        raise ValueError('segment duration must be positive and startup timeout cannot be negative')
    player_path = shutil.which(player)
    if not player_path:
        raise FileNotFoundError(f'{player} not found; install FFmpeg with ffplay available on PATH')

    output = directory / 'node5/output'
    deadline = time.monotonic() + startup_timeout if startup_timeout else None
    while True:
        manifest = json.loads(manifest_path.read_text())
        entries = manifest['segments']
        first_segment = output / f'ground-{entries[0]["segment"]:06d}.ts' if entries else None
        if first_segment and first_segment.is_file():
            break
        if (output / 'complete').is_file():
            raise RuntimeError('node 5 completed without publishing a video segment')
        if deadline is not None and time.monotonic() >= deadline:
            raise TimeoutError(f'node 5 did not publish the first segment within {startup_timeout:g} seconds')
        time.sleep(0.1)

    class LivePlaylistHandler(SimpleHTTPRequestHandler):
        def __init__(self, *args, **kwargs):
            super().__init__(*args, directory=str(output), **kwargs)

        def do_GET(self):
            if urlsplit(self.path).path != '/live.m3u8':
                return super().do_GET()
            latest = json.loads(manifest_path.read_text())
            content = live_playlist(
                output, latest['segments'], segment_duration, (output / 'complete').is_file()
            )
            self.send_response(200)
            self.send_header('Content-Type', 'application/vnd.apple.mpegurl')
            self.send_header('Cache-Control', 'no-cache, no-store, must-revalidate')
            self.send_header('Content-Length', str(len(content)))
            self.end_headers()
            self.wfile.write(content)

        def log_message(self, format, *args):
            pass

    handler = functools.partial(LivePlaylistHandler)
    server = ThreadingHTTPServer(('127.0.0.1', 0), handler)
    server_thread = threading.Thread(target=server.serve_forever, daemon=True)
    process = None
    server_thread.start()
    playlist_url = f'http://127.0.0.1:{server.server_port}/live.m3u8'
    print(f'Live viewer: {playlist_url}', flush=True)
    try:
        process = subprocess.Popen([
            player_path, '-autoexit', '-window_title', 'BPNode Node 5 Live Video', playlist_url
        ])
        return_code = process.wait()
        if return_code:
            raise RuntimeError(f'{player} exited with status {return_code}')
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            process.wait()
        server.shutdown()
        server.server_close()
        server_thread.join()


def main():
    """Dispatch lab build, preparation, per-session launch, and verification."""
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    builder = sub.add_parser('build'); builder.add_argument('--build-dir', type=Path, default=ROOT / 'demo/build')
    preparer = sub.add_parser('prepare'); preparer.add_argument('--build-dir', type=Path, default=ROOT / 'demo/build')
    preparer.add_argument('--run-dir', type=Path, required=True); preparer.add_argument('--segment-dir', type=Path, required=True)
    preparer.add_argument('--chunk-bytes', type=int, default=4096)
    stream_preparer = sub.add_parser('prepare-stream'); stream_preparer.add_argument('--build-dir', type=Path, default=ROOT / 'demo/build')
    stream_preparer.add_argument('--run-dir', type=Path, required=True)
    watcher = sub.add_parser('watch'); watcher.add_argument('--run-dir', type=Path, required=True)
    watcher.add_argument('--watch-dir', type=Path, required=True)
    watcher.add_argument('--pattern', default='*.mp4')
    watcher.add_argument('--poll-interval', type=float, default=1.0)
    watcher.add_argument('--stable-seconds', type=float, default=2.0)
    watcher.add_argument('--segment-duration', type=float, default=2.0)
    watcher.add_argument('--chunk-bytes', type=int, default=4096)
    runner = sub.add_parser('node'); runner.add_argument('--run-dir', type=Path, required=True)
    runner.add_argument('--node', type=int, choices=range(1, 6), required=True); runner.add_argument('--port-base', type=int, default=4600)
    checker = sub.add_parser('verify'); checker.add_argument('--run-dir', type=Path, required=True)
    checker.add_argument('--timeout', type=float, default=300)
    viewer = sub.add_parser('view'); viewer.add_argument('--run-dir', type=Path, required=True)
    viewer.add_argument('--player', default='ffplay')
    viewer.add_argument('--segment-duration', type=float, default=2.0)
    viewer.add_argument('--startup-timeout', type=float, default=300)
    args = parser.parse_args()
    if hasattr(args, 'build_dir'): args.build_dir = args.build_dir.resolve()
    if hasattr(args, 'run_dir'): args.run_dir = args.run_dir.resolve()
    if hasattr(args, 'watch_dir'): args.watch_dir = args.watch_dir.resolve()
    if args.command == 'build': build(args.build_dir)
    elif args.command == 'prepare': prepare(args.run_dir, args.build_dir, args.segment_dir, args.chunk_bytes)
    elif args.command == 'prepare-stream': prepare_stream(args.run_dir, args.build_dir)
    elif args.command == 'watch': watch_folder(
        args.run_dir, args.watch_dir, args.pattern, args.poll_interval, args.stable_seconds,
        args.segment_duration, args.chunk_bytes
    )
    elif args.command == 'verify': verify(args.run_dir, args.timeout)
    elif args.command == 'view': view(args.run_dir, args.player, args.segment_duration, args.startup_timeout)
    else:
        command, env = node_command(args.run_dir, args.node, args.port_base)
        os.chdir(args.run_dir / f'node{args.node}')
        # Keep the advisory lock across exec; a second launcher must not open
        # the same SQLite database or cFS shared-memory resources.
        lock = os.open('node.lock', os.O_CREAT | os.O_RDWR, 0o600)
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise SystemExit('this node runtime is already in use')
        os.set_inheritable(lock, True)
        os.execvpe(command[0], command, env)

if __name__ == '__main__':
    main()
