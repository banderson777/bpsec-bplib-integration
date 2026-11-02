"""Verify the complete native matrix's persisted outputs and module identities."""
from pathlib import Path
import json
import hashlib
import subprocess
import sys
root=Path(__file__).resolve().parent
subprocess.run([sys.executable,str(root/'collect_evidence.py')],check=True)
cases=json.loads((root/'native-evidence.json').read_text())
assert len(cases)==12 and len({c['name'] for c in cases})==12
for case in cases:
    name=case['name']
    case['receipt_clock_errors']={str(n):(Path(case['run_dir'])/f'node{n}.log').read_text(errors='replace').count('Status = -27') for n in range(1,6)}
    assert not any(case['receipt_clock_errors'].values()), name
    assert case['exit_code']==0 and all(case['binary_matches'].values())
    assert all(case['host_matches'].values()) and all(case['startup_matches'].values())
    assert case['keys_present']==[1,5]
    assert all(output['matches_source'] for output in case['outputs'])
    if name in ('wrong-key','missing-bcb','tampered-ciphertext'):
        assert not case['complete'] and case['pending_custody']['4']>0
        if name=='tampered-ciphertext':
            assert all(output['segment']!=0 for output in case['outputs'])
        else:
            assert not case['outputs']
    elif name=='storage-failure':
        assert not case['complete'] and not case['outputs']
        assert case['pending_custody']['1']==7 and case['pending_custody']['2']==0
    else:
        assert case['complete']
        assert len(case['outputs'])==(1 if name.startswith('outage-') or name=='duplicate-replay' else 3)
        if name.startswith('outage-') or name=='duplicate-replay':
            # Quiescence is asserted while the nodes are alive. Termination can
            # finalize in-flight duplicates; preserve their committed custody.
            assert 'custody stayed drained for six seconds on all running nodes' in case['test_output']
            assert all(count==0 for count in case['live_custody_checkpoint'].values())
            assert set(case['live_custody_checkpoint'])=={'2','3','4','5'}
        else:
            assert all(count==0 for count in case['pending_custody'].values())
    assert not any(case['initialization_errors'].values()), name
    if name=='duplicate-replay':
        replay=case['duplicate_replay']
        assert len(replay['original_ids'])==9 and replay['original_ids']==replay['shutdown_retained_ids']
        assert set(replay['live_custody'])=={'1','2','3','4','5'} and all(v==0 for v in replay['live_custody'].values())
        assert replay['output_unchanged'] and replay['completion_count']==case['completion_count']==1
        assert replay['unique_ivs']==case['iv_count']==case['iv_unique'] and replay['unique_ivs']>=27
    if name=='node5-restart':
        assert case['node5_ready_count']>=2 and case['completion_after_last_ready']
        checkpoint=case['receiver_checkpoint']
        assert checkpoint['complete_before_restart'] is False
        assert 0<len(checkpoint['receipts'])<checkpoint['expected_records']
        for file,digest in checkpoint['receipts'].items():
            assert hashlib.sha256((Path(case['run_dir'])/'runtime/node5/receipts'/file).read_bytes()).hexdigest()==digest
    if name=='source-iv-restart':
        manifest=json.loads((Path(case['run_dir'])/'runtime/manifest.json').read_text())
        source_log=(Path(case['run_dir'])/'node1.log').read_text(errors='replace')
        assert source_log.count('DEMO node=1 recovered ')>=2
        assert case['iv_count']>=manifest['records']+1 and case['iv_count']==case['iv_unique']
        assert 'unique 96-bit source IVs across node 1 restart' in case['test_output']
        values=(Path(case['run_dir'])/'runtime/node1/bcb-ivs.log').read_text().splitlines()
        assert all(len(value)==24 and len(bytes.fromhex(value))==12 for value in values)
build=Path('/tmp/bpsec-ccs-20261030/build')
binaries=json.loads((root/'binary-fingerprints.json').read_text())
for file,digest in binaries.items():
    assert hashlib.sha256((build/file).read_bytes()).hexdigest()==digest,file
# Production sources and affected tests must still match the recorded snapshot.
project=root.parents[1]
for file,digest in json.loads((root/'source-hashes.json').read_text()).items():
    assert hashlib.sha256((project/file).read_bytes()).hexdigest()==digest,file
(root/'native-evidence.json').write_text(json.dumps(cases,indent=2))
print('Verified all 12 cases, 60 node runtimes (both bplib and BPNode modules), endpoint-only keys, output hashes, custody, restarts, retained duplicate IDs, immutable output and unique 96-bit IVs.')
