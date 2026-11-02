"""Exercise a broken hop and relay recovery in the five-process BPNode lab.

Each invocation creates a fresh run. It holds source records until the processes
start, leaves one receiving node offline, waits for the upstream relay to accept
custody, then stops the source and relay before restoring the receiver and relay.
"""
import argparse
from contextlib import closing
import json
import hashlib
from pathlib import Path
import sqlite3
import subprocess
import sys
import time

from five_node import check_nodes, node_command, prepare, verify, wait_for_nodes


def custody_count(runtime, node):
    database = runtime / f'node{node}/bplib-storage.db'
    try:
        with closing(sqlite3.connect(f'file:{database}?mode=ro', uri=True, timeout=1)) as db:
            return db.execute('SELECT count(*) FROM bundle_data WHERE is_custodial=1').fetchone()[0]
    except sqlite3.Error:
        return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--run-dir', type=Path, required=True)
    parser.add_argument('--edge', choices=('2-3', '3-4', '4-5'), required=True)
    parser.add_argument('--port-base', type=int, default=15600)
    parser.add_argument('--timeout', type=float, default=90)
    parser.add_argument('--replay-retained-duplicates', action='store_true',
                        help='retain original source bundles and test duplicate replay through a second relay restart (2-3 only)')
    args = parser.parse_args()
    if args.replay_retained_duplicates and args.edge != '2-3':
        parser.error('--replay-retained-duplicates requires --edge 2-3')
    run = args.run_dir.resolve()
    run.mkdir(parents=True, exist_ok=False)
    source = run / 'segments'
    source.mkdir()
    (source / 'segment-000000.ts').write_bytes(bytes((i * 29) % 256 for i in range(1024)))
    runtime = run / 'runtime'
    prepare(runtime, args.build_dir.resolve(), source, 128)
    (runtime / 'node1/input/release').unlink()
    left, right = map(int, args.edge.split('-'))
    processes = {}
    logs = {}

    def launch(number):
        command, env = node_command(runtime, number, args.port_base)
        if args.replay_retained_duplicates and number == 2:
            env['BPNODE_DEMO_HOLD_CCS_UNTIL'] = str(runtime / 'node2/allow-ccs')
        if args.replay_retained_duplicates and number == 1:
            env['BPNODE_DEMO_IV_AUDIT'] = str(runtime / 'node1/bcb-ivs.log')
        if number == 1:
            env.update(BPNODE_DEMO_FEED_GATE='1', BPNODE_DEMO_FEED_BATCH='1',
                       BPNODE_DEMO_FEED_DELAY_MS='100')
        log = (run / f'node{number}.log').open('ab')
        logs[number] = log
        log_offset = log.tell()
        processes[number] = subprocess.Popen(['stdbuf', '-oL', '-eL', *command], cwd=runtime / f'node{number}',
                                             env=env, stdout=log, stderr=subprocess.STDOUT)
        processes[number].log_offset = log_offset
        # Initialize OSAL's common RAM directory once before parallel hosts.
        if len(processes) == 1:
            wait_for_nodes(processes, run)

    def stop(number):
        process = processes.pop(number, None)
        if process and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()

    def wait_drained():
        """Observe a stable zero-custody checkpoint on every current live host."""
        deadline = time.monotonic() + args.timeout
        drained_since = None
        while True:
            check_nodes(processes, run)
            pending = {number: custody_count(runtime, number) for number in processes}
            if all(count == 0 for count in pending.values()):
                if drained_since is None:
                    drained_since = time.monotonic()
                if time.monotonic() - drained_since >= 6:
                    return pending
            else:
                drained_since = None
            if time.monotonic() >= deadline:
                raise RuntimeError(f'custody did not drain after recovery: {pending}')
            time.sleep(0.2)

    def pending_ids(number):
        """Read durable, unegressed custodial bundle identifiers without modifying SQL."""
        with closing(sqlite3.connect(f'file:{runtime}/node{number}/bplib-storage.db?mode=ro', uri=True)) as db:
            return sorted(row[0] for row in db.execute('SELECT bundle_id FROM bundle_data WHERE is_custodial=1 AND egress_attempted=0'))

    try:
        # Keep the receiving endpoint of this hop down to create a real TCP
        # outage; its upstream relay remains running and stores accepted data.
        for number in range(5, 0, -1):
            if number != right:
                launch(number)
        wait_for_nodes(processes, run)
        (runtime / 'node1/input/release').write_text('release source\n')
        expected_records = json.loads((runtime / 'manifest.json').read_text())['records']
        deadline = time.monotonic() + args.timeout
        accepted = False
        while time.monotonic() < deadline:
            check_nodes(processes, run)
            held = custody_count(runtime, left)
            if held is not None and held >= expected_records:
                accepted = True
                break
            time.sleep(0.1)
        if not accepted:
            raise RuntimeError(f'{args.edge} checkpoint not reached; relay custody={held}; inspect {run}')
        stop(1)
        stop(left)
        launch(right)
        launch(left)
        wait_for_nodes(processes, run)
        complete = runtime / 'node5/output/complete'
        deadline = time.monotonic() + args.timeout
        while not complete.exists() and time.monotonic() < deadline:
            check_nodes(processes, run)
            time.sleep(0.2)
        if not complete.exists():
            raise RuntimeError(f'{args.edge} did not recover before timeout; inspect {run}')
        verify(runtime, 0)
        first_drain = wait_drained()
        (run / 'live-custody-drain.json').write_text(json.dumps(first_drain, indent=2))
        if args.replay_retained_duplicates:
            original_ids = pending_ids(1)
            if len(original_ids) != expected_records:
                raise RuntimeError(f'expected retained original source bundles, got {original_ids}')
            before = {p.name: (p.stat().st_mtime_ns, p.read_bytes()) for p in (runtime / 'node5/output').iterdir() if p.name == 'complete' or p.name.startswith('ground-')}
            key_digest = hashlib.sha256((runtime / 'node1/video.key').read_bytes()).hexdigest()
            stop(right)
            (runtime / 'node1/input/release').unlink()
            launch(1)  # Input gate stays closed: retransmit SQL rows, never re-feed ADUs.
            wait_for_nodes(processes, run)
            deadline = time.monotonic() + args.timeout
            while pending_ids(left) != original_ids:
                check_nodes(processes, run)
                if time.monotonic() >= deadline:
                    raise RuntimeError('relay did not retain the original duplicate bundle IDs')
                time.sleep(0.1)
            stop(1)
            stop(left)
            retained_ids = pending_ids(left)
            if retained_ids != original_ids:
                raise RuntimeError('duplicate IDs changed while shutting down the relay')
            (runtime / 'node2/allow-ccs').write_text('release acknowledgements\n')
            launch(right)
            launch(left)
            wait_for_nodes(processes, run)
            with (run / f'node{left}.log').open('rb') as current_log:
                current_log.seek(processes[left].log_offset)
                recovery_log = current_log.read().decode(errors='replace')
            if f'DEMO node={left} recovered {expected_records} custody records' not in recovery_log:
                raise RuntimeError('relay did not report recovering the retained duplicate records')
            launch(1)
            wait_for_nodes(processes, run)
            final_drain = wait_drained()
            verify(runtime, 0)
            after = {p.name: (p.stat().st_mtime_ns, p.read_bytes()) for p in (runtime / 'node5/output').iterdir() if p.name == 'complete' or p.name.startswith('ground-')}
            if after != before or (run / 'node5.log').read_text(errors='replace').count('DEMO complete:') != 1:
                raise RuntimeError('duplicate replay republished ground output or completion')
            ivs = (runtime / 'node1/bcb-ivs.log').read_text().splitlines()
            if len(ivs) < expected_records * 3 or len(ivs) != len(set(ivs)) or any(len(iv) != 24 for iv in ivs):
                raise RuntimeError('duplicate replay did not preserve source IV uniqueness')
            if hashlib.sha256((runtime / 'node1/video.key').read_bytes()).hexdigest() != key_digest:
                raise RuntimeError('duplicate replay replaced the source key')
            (run / 'duplicate-replay.json').write_text(json.dumps(dict(original_ids=original_ids,
                shutdown_retained_ids=retained_ids, live_custody=final_drain,
                unique_ivs=len(ivs), completion_count=1, output_unchanged=True), indent=2))
            print(f'PASS: replayed {len(original_ids)} original duplicate IDs through relay shutdown/restart; all five nodes drained, output unchanged, {len(ivs)} unique IVs')
        print('PASS: custody stayed drained for six seconds on all running nodes after outage recovery')
        print(f'PASS: {args.edge} outage retained {held} custody records; relay {left} restarted with source stopped and completed transfer')
    finally:
        # Stop upstream before downstream so teardown does not create a new outage.
        for number in sorted(processes):
            stop(number)
        for log in logs.values():
            log.close()


if __name__ == '__main__':
    main()
