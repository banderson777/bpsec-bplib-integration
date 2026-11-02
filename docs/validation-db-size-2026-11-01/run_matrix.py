import json, os, signal, subprocess, time
from pathlib import Path
root=Path('/tmp/bpsec-size-20261101')
cases=json.loads((root/'cases.json').read_text())
results=[]
for case in cases:
    start=time.monotonic()
    with open(case['log'],'wb') as log:
        p=subprocess.Popen(case['command'],stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
        try: code=p.wait(timeout=450)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid,signal.SIGTERM)
            try: p.wait(timeout=15)
            except subprocess.TimeoutExpired: os.killpg(p.pid,signal.SIGKILL); p.wait()
            code=124
    case.update(exit_code=code,seconds=round(time.monotonic()-start,2))
    results.append(case)
    (root/'results.json').write_text(json.dumps(results,indent=2))
    print(case['name'],code,case['seconds'],flush=True)
    if code: raise SystemExit(code)
