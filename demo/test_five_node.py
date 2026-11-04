"""Run the five real cFS processes against a small deterministic file transfer.

Requires a completed five_node.py build and permission to bind loopback sockets.
Logs and runtime databases remain in --run-dir for inspection on success/failure.
"""
import argparse
import hashlib
import json
from contextlib import closing
import os
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
    """Launch downstream first, verify reconstructed bytes, and stop all nodes."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--run-dir', type=Path, required=True)
    parser.add_argument('--port-base', type=int, default=14600)
    parser.add_argument('--timeout', type=float, default=90)
    parser.add_argument('--drop-ccs-node', type=int, choices=range(1, 6),
                        help='drop one outgoing CCS at this node')
    parser.add_argument('--storage-fail-node', type=int, choices=range(2, 6),
                        help='inject a SQLite full error on bundle insert at this node')
    parser.add_argument('--wrong-key', action='store_true',
                        help='use a different node 5 key and assert authenticated delivery fails')
    parser.add_argument('--omit-bcb', action='store_true',
                        help='send unprotected payloads and assert node 5 rejects them')
    parser.add_argument('--tamper-ciphertext', action='store_true',
                        help='flip ciphertext in transit and recalculate the BP payload CRC')
    parser.add_argument('--restart-node5-after-receipt', action='store_true',
                        help='restart node 5 once after its first durable chunk receipt')
    parser.add_argument('--audit-ivs-across-node1-restart', action='store_true',
                        help='audit source IV uniqueness before and after restarting node 1')
    args = parser.parse_args()
    if sum((args.drop_ccs_node is not None, args.storage_fail_node is not None, args.wrong_key, args.omit_bcb,
            args.tamper_ciphertext,
            args.audit_ivs_across_node1_restart)) > 1:
        parser.error('choose one fault injection per run')
    run = args.run_dir.resolve()
    if run.exists():
        if not run.is_dir():
            raise FileExistsError(f'run path is not a directory: {run}')
        if any(run.iterdir()):
            raise FileExistsError(f'run directory already exists and is not empty: {run}')
    else:
        run.mkdir(parents=True, exist_ok=True)
    segments = run / 'segments'
    segments.mkdir(exist_ok=True)
    for index, size in enumerate((9000, 17, 8000)):
        (segments / f'segment-{index:06d}.ts').write_bytes(bytes((i + index) % 256 for i in range(size)))
    runtime = run / 'runtime'
    prepare(runtime, args.build_dir.resolve(), segments, 4096)
    if args.restart_node5_after_receipt:
        (runtime / 'node1/input/release').unlink()
    if args.wrong_key:
        key_path = runtime / 'node5/video.key'
        key = bytearray(key_path.read_bytes())
        key[0] ^= 1
        key_path.write_bytes(key)
    processes = []
    logs = []
    source_restarted = False
    ivs_before_restart = 0
    iv_audit = runtime / 'node1/bcb-ivs.log'
    try:
        for number in range(5, 0, -1):
            log = (run / f'node{number}.log').open('wb')
            logs.append(log)
            command = ['stdbuf', '-oL', '-eL', sys.executable, str(Path(__file__).with_name('five_node.py')),
                       'node', '--run-dir', str(runtime), '--node', str(number), '--port-base', str(args.port_base)]
            env = dict(os.environ)
            if args.drop_ccs_node == number:
                env['BPNODE_DEMO_DROP_CCS'] = '1'
            if args.storage_fail_node == number:
                env['BPNODE_DEMO_STORAGE_FAIL'] = '1'
            if args.omit_bcb and number == 1:
                env['BPNODE_DEMO_OMIT_BCB'] = '1'
            if args.tamper_ciphertext and number == 1:
                env['BPNODE_DEMO_TAMPER_CIPHERTEXT'] = '1'
            if args.audit_ivs_across_node1_restart and number == 1:
                env['BPNODE_DEMO_IV_AUDIT'] = str(iv_audit)
                env['BPNODE_DEMO_FEED_DELAY_MS'] = '50'
            if args.restart_node5_after_receipt and number == 1:
                env.update(BPNODE_DEMO_FEED_GATE='1', BPNODE_DEMO_FEED_BATCH='1',
                           BPNODE_DEMO_FEED_DELAY_MS='1000')
            processes.append(subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, env=env))
            # Let the first host create OSAL's shared RAM directory before
            # other hosts can race its initial stat/mkdir sequence.
            if len(processes) == 1:
                wait_for_nodes({number: processes[0]}, run)
        wait_for_nodes({5 - i: process for i, process in enumerate(processes)}, run)
        deadline = time.monotonic() + args.timeout
        complete = runtime / 'node5/output/complete'
        if args.restart_node5_after_receipt:
            gate = runtime / 'node1/input/release'
            gate.write_text('feed first record\n')
            while not custody_count(runtime, 1):
                check_nodes({5 - i: process for i, process in enumerate(processes)}, run)
                if time.monotonic() >= deadline:
                    raise RuntimeError('source did not store the first gated record')
                time.sleep(0.01)
            gate.unlink()  # Pause further ADUs until the incomplete receiver restarts.

        receiver_restarted = False
        drained_since = None
        if args.storage_fail_node is not None:
            time.sleep(min(args.timeout, 20))
            check_nodes({5 - i: process for i, process in enumerate(processes)}, run)
            upstream = args.storage_fail_node - 1
            upstream_pending = custody_count(runtime, upstream)
            rejected_pending = custody_count(runtime, args.storage_fail_node)
            if complete.exists() or not upstream_pending or upstream_pending <= 0 or rejected_pending != 0:
                raise RuntimeError(f'storage failure did not withhold custody: upstream={upstream_pending}, receiver={rejected_pending}')
            print(f'PASS: node {args.storage_fail_node} storage failure withheld custody; node {upstream} retains {upstream_pending} record(s)')
            return
        if args.wrong_key or args.omit_bcb or args.tamper_ciphertext:
            time.sleep(min(args.timeout, 15))
            receiver_log = (run / 'node5.log').read_text(errors='replace')
            upstream_pending = custody_count(runtime, 4)
            expected_hook = ('DEMO fault injection: altered ciphertext with valid BP payload CRC' if args.tamper_ciphertext
                             else 'DEMO fault injection: omitted BCB at node=1')
            source_log = (run / 'node1.log').read_text(errors='replace')
            if (complete.exists() or 'DEMO security rejected bundle at node=5' not in receiver_log or
                    not upstream_pending or (not args.wrong_key and expected_hook not in source_log) or
                    any(process.poll() is not None for process in processes)):
                raise RuntimeError(f'security fault was not rejected safely: node4 custody={upstream_pending}; inspect {run}')
            if any((runtime / 'node5/output').glob('ground-*.ts')) and not args.tamper_ciphertext:
                raise RuntimeError('rejected bundles unexpectedly published video segments')
            if args.wrong_key:
                print(f'PASS: node 5 rejected a different endpoint key; no output, node 4 retains {upstream_pending} record(s)')
            elif args.tamper_ciphertext:
                if (runtime / 'node5/output/ground-000000.ts').exists():
                    raise RuntimeError('tampered segment unexpectedly published')
                print(f'PASS: node 5 rejected altered ciphertext after valid BP CRC verification; no tampered segment or completion, node 4 retains {upstream_pending} record(s)')
            else:
                print(f'PASS: node 5 rejected missing BCB, published no output, node 4 retains {upstream_pending} record(s)')
            return
        while True:
            if args.audit_ivs_across_node1_restart and not source_restarted:
                try:
                    audited = iv_audit.read_text().splitlines()
                except FileNotFoundError:
                    audited = []
                if audited:
                    ivs_before_restart = len(audited)
                    process = processes[4]  # nodes are launched 5, 4, ..., 1
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
                    _, env = node_command(runtime, 1, args.port_base)
                    env['BPNODE_DEMO_IV_AUDIT'] = str(iv_audit)
                    env['BPNODE_DEMO_FEED_DELAY_MS'] = '50'
                    log = (run / 'node1.log').open('ab')
                    logs.append(log)
                    log_offset = log.tell()
                    processes[4] = subprocess.Popen(
                        ['stdbuf', '-oL', '-eL', sys.executable,
                         str(Path(__file__).with_name('five_node.py')), 'node',
                         '--run-dir', str(runtime), '--node', '1', '--port-base', str(args.port_base)],
                        stdout=log, stderr=subprocess.STDOUT, env=env)
                    processes[4].log_offset = log_offset
                    wait_for_nodes({5 - i: process for i, process in enumerate(processes)}, run)
                    source_restarted = True
            if args.restart_node5_after_receipt and not receiver_restarted and any(
                (runtime / 'node5/receipts').glob('*.adu')):
                process = processes[0]  # nodes are launched 5, 4, ..., 1
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
                receipts = sorted((runtime / 'node5/receipts').glob('*.adu'))
                expected = json.loads((runtime / 'manifest.json').read_text())['records']
                if complete.exists() or not 0 < len(receipts) < expected:
                    raise RuntimeError('receiver restart missed the incomplete-receipt checkpoint')
                (run / 'node5-restart-checkpoint.json').write_text(json.dumps(dict(
                    receipts={p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in receipts},
                    complete_before_restart=False, expected_records=expected), indent=2))
                _, env = node_command(runtime, 5, args.port_base)
                log = (run / 'node5.log').open('ab')
                logs.append(log)
                log_offset = log.tell()
                processes[0] = subprocess.Popen(
                    ['stdbuf', '-oL', '-eL', sys.executable,
                     str(Path(__file__).with_name('five_node.py')), 'node',
                     '--run-dir', str(runtime), '--node', '5', '--port-base', str(args.port_base)],
                    stdout=log, stderr=subprocess.STDOUT, env=env)
                processes[0].log_offset = log_offset
                wait_for_nodes({5 - i: process for i, process in enumerate(processes)}, run)
                receiver_restarted = True
                (runtime / 'node1/input/release').write_text('resume remaining records\n')
            check_nodes({5 - i: process for i, process in enumerate(processes)}, run)
            custody_pending = False
            for node in range(1, 6):
                database = runtime / f'node{node}/bplib-storage.db'
                try:
                    with closing(sqlite3.connect(f'file:{database}?mode=ro', uri=True, timeout=1)) as db:
                        pending = db.execute(
                            'SELECT count(*) FROM bundle_data WHERE is_custodial=1'
                        ).fetchone()[0]
                    custody_pending |= pending != 0
                except sqlite3.Error:
                    custody_pending = True
            if complete.exists() and not custody_pending:
                if drained_since is None:
                    drained_since = time.monotonic()
                if time.monotonic() - drained_since >= 6:
                    break
            else:
                drained_since = None
            if time.monotonic() >= deadline:
                raise RuntimeError(f'five-node transfer/custody completion timed out; inspect {run}')
            time.sleep(0.2)
        verify(runtime, 0)
        node5_log = (run / 'node5.log').read_text(errors='replace')
        if node5_log.count('DEMO complete:') != 1:
            raise RuntimeError('completion marker was not idempotent; inspect node5.log')
        if args.drop_ccs_node is not None:
            log_text = (run / f'node{args.drop_ccs_node}.log').read_text(errors='replace')
            if 'DEMO fault injection: dropped one CCS' not in log_text:
                raise RuntimeError('requested CCS drop hook did not fire')
        if args.restart_node5_after_receipt:
            if not receiver_restarted or node5_log.rfind('DEMO complete:') < node5_log.rfind('DEMO node=5 recovered '):
                raise RuntimeError('node 5 did not complete from incomplete receipts after restart')
        if args.audit_ivs_across_node1_restart:
            ivs = iv_audit.read_text().splitlines()
            if (not source_restarted or len(ivs) <= ivs_before_restart or
                    any(len(iv) != 24 for iv in ivs) or len(set(ivs)) != len(ivs)):
                raise RuntimeError(f'source IV audit failed across restart: restarted={source_restarted}, before={ivs_before_restart}, count={len(ivs)}, unique={len(set(ivs))}')
            print(f'PASS: {len(ivs)} protected bundles used unique 96-bit source IVs across node 1 restart')
        print('PASS: all five nodes released accepted custody after CCS processing')
    finally:
        for process in processes:
            if process.poll() is None: process.terminate()
        for process in processes:
            try: process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        for log in logs: log.close()


if __name__ == '__main__':
    main()
