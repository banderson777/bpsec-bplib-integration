"""Collect read-only native runtime evidence for this recorded build."""
import hashlib
import json
from pathlib import Path
import sqlite3
from contextlib import closing

root=Path(__file__).resolve().parent
results=json.loads((root/'results.json').read_text())
binaries=json.loads((root/'binary-fingerprints.json').read_text())
for case in results:
    run=Path(case.get('run_dir',root/case['name'])); runtime=run/'runtime'
    manifest=json.loads((runtime/'manifest.json').read_text())
    case['complete']=(runtime/'node5/output/complete').exists()
    case['custody']={}
    case['pending_custody']={}
    for n in range(1,6):
        db=runtime/f'node{n}/bplib-storage.db'
        try:
            with closing(sqlite3.connect(f'file:{db}?mode=ro',uri=True)) as connection:
                case['custody'][str(n)]=connection.execute('SELECT count(*) FROM bundle_data WHERE is_custodial=1').fetchone()[0]
                case['pending_custody'][str(n)]=connection.execute('SELECT count(*) FROM bundle_data WHERE is_custodial=1 AND egress_attempted=0').fetchone()[0]
        except sqlite3.Error as exc:
            case['custody'][str(n)]=str(exc)
    case['binary_matches'] = {str(n): all(hashlib.sha256((runtime/f'node{n}/cf'/name).read_bytes()).hexdigest() == binaries['install/cpu1/cf/'+name] for name in ['bplib.so','bpnode.so']) for n in range(1,6)}
    case['host_matches']={str(n):hashlib.sha256((runtime/f'node{n}/core-cpu1').read_bytes()).hexdigest()==binaries['install/cpu1/core-cpu1'] for n in range(1,6)}
    case['startup_matches']={str(n):hashlib.sha256((runtime/f'node{n}/cf/cfe_es_startup.scr').read_bytes()).hexdigest()==binaries['install/cpu1/cf/cfe_es_startup.scr'] for n in range(1,6)}
    node5log=(run/'node5.log').read_text(errors='replace')
    case['completion_count']=node5log.count('DEMO complete:')
    case['node5_ready_count']=node5log.count('DEMO node=5 recovered ')
    case['completion_after_last_ready']=node5log.rfind('DEMO complete:') > node5log.rfind('DEMO node=5 recovered ')
    case['keys_present']=[n for n in range(1,6) if (runtime/f'node{n}/video.key').exists()]
    case['outputs']=[]
    for entry in manifest['segments']:
        output=runtime/f'node5/output/ground-{entry["segment"]:06d}.ts'
        if output.exists():
            data=output.read_bytes()
            digest=hashlib.sha256(data).hexdigest()
            case['outputs'].append(dict(segment=entry['segment'],bytes=len(data),sha256=digest,matches_source=digest==entry['sha256'] and len(data)==entry['size']))
    ivs=runtime/'node1/bcb-ivs.log'
    if ivs.exists():
        values=ivs.read_text().splitlines()
        case['iv_count']=len(values);case['iv_unique']=len(set(values))
    case['test_output']=Path(case.get('log',root/(case['name']+'.log'))).read_text(errors='replace')
    checkpoint=run/'live-custody-drain.json'
    if checkpoint.exists(): case['live_custody_checkpoint']=json.loads(checkpoint.read_text())
    receiver_checkpoint=run/'node5-restart-checkpoint.json'
    if receiver_checkpoint.exists(): case['receiver_checkpoint']=json.loads(receiver_checkpoint.read_text())
    replay=run/'duplicate-replay.json'
    if replay.exists(): case['duplicate_replay']=json.loads(replay.read_text())
    case['proof_logs']={str(n):[line for line in (run/f'node{n}.log').read_text(errors='replace').splitlines() if any(marker in line for marker in [' custody records','DEMO complete:','DEMO source queued','DEMO fault injection:',' persisted'])][-200:] for n in range(1,6)}
    case['receipt_clock_errors']={str(n):(run/f'node{n}.log').read_text(errors='replace').count('Status = -27') for n in range(1,6)}
    case['initialization_errors']={str(n):(run/f'node{n}.log').read_text(errors='replace').count('Error Initializing BPLib') for n in range(1,6)}
(root/'native-evidence.json').write_text(json.dumps(results,indent=2))
print(json.dumps([{k:case[k] for k in ['name','exit_code','seconds','complete','custody','keys_present']} for case in results],indent=2))
