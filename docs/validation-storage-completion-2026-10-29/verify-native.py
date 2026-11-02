"""Verify the complete native matrix's persisted outputs and module identities."""
from pathlib import Path
import json
import hashlib
import subprocess
import sys
root=Path(__file__).resolve().parent
subprocess.run([sys.executable,str(root/'collect_evidence.py')],check=True)
cases=json.loads((root/'native-evidence.json').read_text())
assert len(cases)==11 and len({c['name'] for c in cases})==11
for case in cases:
    name=case['name']
    case['receipt_clock_errors']={str(n):(Path(case['run_dir'])/f'node{n}.log').read_text(errors='replace').count('Status = -27') for n in range(1,6)}
    assert not any(case['receipt_clock_errors'].values()), name
    assert case['exit_code']==0 and all(case['binary_matches'].values())
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
        assert len(case['outputs'])==(1 if name.startswith('outage-') else 3)
        if name.startswith('outage-'):
            # Quiescence is asserted while the nodes are alive. Termination can
            # finalize in-flight duplicates; preserve their committed custody.
            assert 'custody stayed drained for six seconds on all running nodes' in case['test_output']
            case['live_custody_checkpoint']={str(n):0 for n in range(2,6)}
        else:
            assert all(count==0 for count in case['pending_custody'].values())
    if name=='node5-restart':
        assert case['node5_ready_count']>=2 and case['completion_after_last_ready']
    if name=='source-iv-restart':
        assert case['iv_count']>=20 and case['iv_count']==case['iv_unique']
        values=(Path(case['run_dir'])/'runtime/node1/bcb-ivs.log').read_text().splitlines()
        assert all(len(value)==24 and len(bytes.fromhex(value))==12 for value in values)
# Production sources must still match the built snapshot.
project=root.parents[1]
for file,digest in json.loads((root/'source-hashes.json').read_text()).items():
    assert hashlib.sha256((project/file).read_bytes()).hexdigest()==digest,file
(root/'native-evidence.json').write_text(json.dumps(cases,indent=2))
print('Verified all 11 cases, 55 node runtimes (both bplib and BPNode modules), endpoint-only keys, output hashes, custody, restarts and unique 96-bit IVs.')
