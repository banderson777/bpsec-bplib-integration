import json, os, signal, subprocess, time
from pathlib import Path
root=Path('/tmp/bpsec-sender-20261030')
cases=json.loads((root/'cases.json').read_text())
deadline=time.monotonic()+600
while not (root/'native-final-ready').exists():
    if time.monotonic()>deadline: raise SystemExit('native build did not finish')
    time.sleep(2)
# Startup script is the generated demo configuration, copied after installation.
import shutil
build=Path('/tmp/bpsec-ccs-20261030/build')
shutil.copy2(build/'mission/cfe_es_startup.scr',build/'install/cpu1/cf/cfe_es_startup.scr')
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
