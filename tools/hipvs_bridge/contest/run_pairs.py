"""Execute four cold cases in fixed V5 order, retaining all failures."""
import argparse
import datetime
import json
from pathlib import Path
import subprocess
import sys
import time
from run_sql_benchmark import sha

ORDER=[('sift','baseline',1),('sift','candidate',2),('hidden','candidate',3),('hidden','baseline',4)]

def validate_plan(plan):
    if [(x['dataset'],x['variant'],x['ordinal']) for x in plan['cases']]!=ORDER:
        raise ValueError('exact SIFT B1/C2, 256-d C3/B4 order required')
    for dataset in ['sift','hidden']:
        pair=[case for case in plan['cases'] if case['dataset']==dataset]
        for field in ['base_tsv','queries_f32','ids_json','truth_json','dataset_role','card_device']:
            if pair[0][field]!=pair[1][field]:raise ValueError('paired input/environment differs: '+field)
    for case in plan['cases']:
        if case.get('profiler',False):raise ValueError('run profiler diagnostics separately from timing pairs')

def verify_inputs(case,hashes):
    for key in ['base_tsv','queries_f32','ids_json','truth_json']:
        path=case[key]
        if path not in hashes or sha(Path(path))!=hashes[path]:
            raise ValueError('input changed since plan creation: '+path)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--plan',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
    a=p.parse_args();plan=json.loads(a.plan.read_text());validate_plan(plan)
    a.out.mkdir(parents=True,exist_ok=False)
    state=dict(state='RUNNING',official_score='UNKNOWN',gpu_path='UNKNOWN',cases=[],
               started_utc=datetime.datetime.now(datetime.timezone.utc).isoformat())
    started=time.monotonic()
    try:
        for case in plan['cases']:
            verify_inputs(case,plan.get('input_sha256',{}))
            directory=a.out/('%d-%s-%s'%(case['ordinal'],case['dataset'],case['variant']))
            command=[sys.executable,str(Path(__file__).with_name('run_cold_case.py')),'--out',str(directory)]
            for key,value in case.items():
                if key=='profiler':continue
                command+=['--'+key.replace('_','-'),str(value)]
            row=dict(command=command,state='RUNNING');state['cases'].append(row)
            (a.out/'suite.json').write_text(json.dumps(state,indent=2))
            with (a.out/('%d-launch.log'%case['ordinal'])).open('x') as log:
                process=subprocess.Popen(command,stdout=log,stderr=subprocess.STDOUT)
                try:code=process.wait(timeout=max(1,900-(time.monotonic()-started)))
                except subprocess.TimeoutExpired:
                    process.terminate()
                    try:process.wait(timeout=60)
                    except subprocess.TimeoutExpired:
                        receipt=directory/'lifecycle.json'
                        if receipt.exists():
                            from run_cold_case import stop_owned
                            server=json.loads(receipt.read_text())
                            stop_owned(server.get('server_pid'),Path(case['binary']).resolve(),Path(server['fresh_directory']))
                        process.kill();process.wait()
                    raise TimeoutError('900s local suite wall budget expired; source server cleanup requested')
            row.update(returncode=code,state='PASSED' if code==0 else 'FAILED')
            if code:raise RuntimeError('case failed; preserve directory and do not reuse its graph')
        state['state']='LOCAL_PAIRS_COMPLETE_EXTERNAL_HARD_GATES_PENDING'
    except BaseException as error:
        state.update(state='FAILED',error=repr(error));raise
    finally:
        state.update(wall_seconds=time.monotonic()-started,finished_utc=datetime.datetime.now(datetime.timezone.utc).isoformat())
        (a.out/'suite.json').write_text(json.dumps(state,indent=2))

if __name__=='__main__':main()
