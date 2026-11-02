"""Recheck native runtime evidence; requires the /tmp run named in native-result.json."""
from pathlib import Path
from contextlib import closing
import hashlib
import json
import sqlite3

ROOT=Path(__file__).resolve().parent
PROJECT=ROOT.parents[1]
result=json.loads((ROOT/'native-result.json').read_text())
assert result['exit_code']==0
run=Path(result['run_dir']); runtime=run/'runtime'
expected=json.loads((ROOT/'binary-hashes.json').read_text())
evidence=dict(scenario=result['scenario'],seconds=result['seconds'],exit_code=0,
    runtime=str(runtime),complete=(runtime/'node5/output/complete').exists(),
    keys_present=[n for n in range(1,6) if (runtime/f'node{n}/video.key').exists()],
    outputs=[],module_matches={},pending_custody={},receipt_clock_errors={})
assert evidence['complete'] and evidence['keys_present']==[1,5]
manifest=json.loads((runtime/'manifest.json').read_text())
for entry in manifest['segments']:
    data=(runtime/f'node5/output/ground-{entry["segment"]:06d}.ts').read_bytes()
    digest=hashlib.sha256(data).hexdigest()
    assert digest==entry['sha256'] and len(data)==entry['size']
    evidence['outputs'].append(dict(segment=entry['segment'],bytes=len(data),sha256=digest))
assert len(evidence['outputs'])==3
for number in range(1,6):
    evidence['module_matches'][str(number)]={name:hashlib.sha256((runtime/f'node{number}/cf'/name).read_bytes()).hexdigest()==expected['install/cpu1/cf/'+name] for name in ['bplib.so','bpnode.so']}
    assert all(evidence['module_matches'][str(number)].values())
    with closing(sqlite3.connect('file:'+str(runtime/f'node{number}/bplib-storage.db')+'?mode=ro',uri=True)) as db:
        evidence['pending_custody'][str(number)]=db.execute('SELECT count(*) FROM bundle_data WHERE is_custodial=1 AND egress_attempted=0').fetchone()[0]
    log=(run/f'node{number}.log').read_text(errors='replace')
    assert 'Error initializing BPLib' not in log
    evidence['receipt_clock_errors'][str(number)]=log.count('Status = -27')
assert not any(evidence['receipt_clock_errors'].values())
evidence['dropped_ccs_markers']=(run/'node2.log').read_text(errors='replace').count('DEMO fault injection: dropped one CCS')
assert evidence['dropped_ccs_markers']==1
controller=Path(result['log']).read_text(errors='replace')
assert 'PASS: all five nodes released accepted custody after CCS processing' in controller
evidence['live_custody_drain_verified']=True
for path,digest in json.loads((ROOT/'source-hashes.json').read_text()).items():
    assert hashlib.sha256((PROJECT/path).read_bytes()).hexdigest()==digest,path
(ROOT/'native-evidence.json').write_text(json.dumps(evidence,indent=2))
print('Verified source hashes, both modules at five nodes, output hashes, endpoint-only keys, one dropped CCS, and live custody drain.')
print('Post-stop pending custody:',evidence['pending_custody'])
