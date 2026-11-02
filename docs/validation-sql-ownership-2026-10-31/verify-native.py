"""Verify the complete native matrix's persisted outputs and module identities."""
from pathlib import Path
import json
import hashlib
import subprocess
import sys
root=Path(__file__).resolve().parent
subprocess.run([sys.executable,str(root/'collect_evidence.py')],check=True)
cases=json.loads((root/'native-evidence.json').read_text())
assert len(cases)==5 and {c['name'] for c in cases}=={'baseline','node5-restart','duplicate-replay','lost-ccs','storage-failure'}
for case in cases:
    name=case['name']
    assert case['exit_code']==0
    assert all(case['binary_matches'].values()) and all(case['host_matches'].values())
    assert all(case['startup_matches'].values()) and case['keys_present']==[1,5]
    assert all(o['matches_source'] for o in case['outputs'])
    assert not any(case['receipt_clock_errors'].values())
    assert not any(case['initialization_errors'].values())
    if name=='storage-failure':
        assert not case['complete'] and not case['outputs'] and case['completion_count']==0
        assert case['pending_custody']['1']==7 and case['pending_custody']['2']==0
        continue
    assert case['complete'] and len(case['outputs'])==(1 if name=='duplicate-replay' else 3)
    assert case['completion_count']==1
    if name=='lost-ccs':
        assert 'DEMO fault injection: dropped one CCS' in (Path(case['run_dir'])/'node2.log').read_text(errors='replace')
    assert not any(case['receipt_clock_errors'].values())
    assert not any(case['initialization_errors'].values())
    assert 'custody' in case['test_output']
    if name=='duplicate-replay':
        checkpoint=case['duplicate_replay']
        assert len(checkpoint['original_ids'])==9 and checkpoint['original_ids']==checkpoint['shutdown_retained_ids']
        assert set(checkpoint['live_custody'])=={'1','2','3','4','5'}
        assert all(v==0 for v in checkpoint['live_custody'].values())
        assert checkpoint['output_unchanged'] and checkpoint['completion_count']==1
        assert checkpoint['unique_ivs']==case['iv_count']==case['iv_unique'] and checkpoint['unique_ivs']>=27
        values=(Path(case['run_dir'])/'runtime/node1/bcb-ivs.log').read_text().splitlines()
        assert all(len(value)==24 and len(bytes.fromhex(value))==12 for value in values)
    else:
        assert all(v==0 for v in case['pending_custody'].values())
    if name=='node5-restart':
        assert case['node5_ready_count']>=2 and case['completion_after_last_ready']
        checkpoint=case['receiver_checkpoint']
        assert not checkpoint['complete_before_restart'] and 0<len(checkpoint['receipts'])<checkpoint['expected_records']
        for file,digest in checkpoint['receipts'].items():
            assert hashlib.sha256((Path(case['run_dir'])/'runtime/node5/receipts'/file).read_bytes()).hexdigest()==digest
build=Path('/tmp/bpsec-sql-ownership-20261101/build')
binaries=json.loads((root/'binary-fingerprints.json').read_text())
for file,digest in binaries.items():
    assert hashlib.sha256((build/file).read_bytes()).hexdigest()==digest,file
# Production sources and affected tests must still match the recorded snapshot.
project=root.parents[1]
for file,digest in json.loads((root/'source-hashes.json').read_text()).items():
    assert hashlib.sha256((project/file).read_bytes()).hexdigest()==digest,file
(root/'native-evidence.json').write_text(json.dumps(cases,indent=2))
print('Verified all 5 native cases, 25 runtimes, output hashes, custody/restart checkpoints, immutable replay, endpoint keys and pinned binaries/sources.')
