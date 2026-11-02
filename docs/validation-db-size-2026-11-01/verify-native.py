"""Verify this telemetry pass's fresh baseline and pinned source/build identities."""
from pathlib import Path
import hashlib,json,subprocess,sys
root=Path(__file__).resolve().parent
subprocess.run([sys.executable,str(root/'collect_evidence.py')],check=True)
cases=json.loads((root/'native-evidence.json').read_text())
assert len(cases)==1 and cases[0]['name']=='baseline'
c=cases[0]
assert c['exit_code']==0 and c['complete'] and c['completion_count']==1
assert all(c['binary_matches'].values()) and all(c['host_matches'].values())
assert all(c['startup_matches'].values()) and c['keys_present']==[1,5]
assert len(c['outputs'])==3 and all(o['matches_source'] for o in c['outputs'])
assert all(v==0 for v in c['pending_custody'].values())
assert not any(c['receipt_clock_errors'].values()) and not any(c['initialization_errors'].values())
project=root.parents[1]
for file,digest in json.loads((root/'source-hashes.json').read_text()).items():
    assert hashlib.sha256((project/file).read_bytes()).hexdigest()==digest,file
build=Path('/tmp/bpsec-sql-ownership-20261101/build')
for file,digest in json.loads((root/'binary-fingerprints.json').read_text()).items():
    assert hashlib.sha256((build/file).read_bytes()).hexdigest()==digest,file
print('Verified fresh baseline, five runtimes, output hashes, drained custody, endpoint keys and pinned binaries/sources.')
