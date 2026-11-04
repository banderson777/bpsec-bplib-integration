#!/usr/bin/env python3
"""Build, prepare, run, and verify the five-process BPNode/cFS Linux lab.

Use separate Linux or Windows/WSL terminals for `node`. The controller never
loads endpoint keys into relay processes. Runtime state is isolated per run.
"""
import argparse
import errno
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import time

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


def prepare(directory, build_dir, segments, chunk):
    """Create a new recording with isolated nodes, keys, inputs, and manifest."""
    if directory.exists(): raise ValueError('run directory already exists; use a new path or resume with node')
    files = sorted(segments.glob('segment-*.ts'))
    if not files: raise ValueError('no segment-*.ts input files')
    if len(files) > 100000: raise ValueError('at most 100000 segments are supported')
    if not 1 <= chunk <= 4096: raise ValueError('chunk size must be 1..4096')
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
    """Wait for authenticated completion and compare every output segment hash."""
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


def main():
    """Dispatch lab build, preparation, per-session launch, and verification."""
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    builder = sub.add_parser('build'); builder.add_argument('--build-dir', type=Path, default=ROOT / 'demo/build')
    preparer = sub.add_parser('prepare'); preparer.add_argument('--build-dir', type=Path, default=ROOT / 'demo/build')
    preparer.add_argument('--run-dir', type=Path, required=True); preparer.add_argument('--segment-dir', type=Path, required=True)
    preparer.add_argument('--chunk-bytes', type=int, default=4096)
    runner = sub.add_parser('node'); runner.add_argument('--run-dir', type=Path, required=True)
    runner.add_argument('--node', type=int, choices=range(1, 6), required=True); runner.add_argument('--port-base', type=int, default=4600)
    checker = sub.add_parser('verify'); checker.add_argument('--run-dir', type=Path, required=True)
    checker.add_argument('--timeout', type=float, default=300)
    args = parser.parse_args()
    if hasattr(args, 'build_dir'): args.build_dir = args.build_dir.resolve()
    if hasattr(args, 'run_dir'): args.run_dir = args.run_dir.resolve()
    if args.command == 'build': build(args.build_dir)
    elif args.command == 'prepare': prepare(args.run_dir, args.build_dir, args.segment_dir, args.chunk_bytes)
    elif args.command == 'verify': verify(args.run_dir, args.timeout)
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
