"""Bind actual GPU-side roots/data to the fixed cold-case order before timing."""
import argparse
import json
from pathlib import Path
from run_pairs import ORDER,validate_plan
from run_sql_benchmark import sha

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ['baseline-root','candidate-root','artifacts','data-root','card-device','out']:
        p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--first-port',type=int,default=2981)
    a=p.parse_args();cases=[];inputs={}
    for dataset,variant,ordinal in ORDER:
        root=getattr(a,variant+'_root').resolve(strict=True)
        data=(a.data_root/('sift' if dataset=='sift' else 'diagnostic-256')).resolve(strict=True)
        case=dict(root=str(root),binary=str(root/'build_release/src/observer/seekdb'),
            bridge=str((a.artifacts/variant/'libseekdb_cuvs_bridge.so').resolve(strict=True)),
            dataset=dataset,dataset_role='public-sift' if dataset=='sift' else 'diagnostic-256',
            variant=variant,ordinal=ordinal,port=a.first_port+ordinal-1,card_device=str(a.card_device.resolve(strict=True)))
        for key,name in [('base_tsv','base.tsv'),('queries_f32','queries.f32'),('ids_json','ids.json'),('truth_json','truth.json')]:
            path=(data/name).resolve(strict=True);case[key]=str(path)
            inputs[str(path)]=sha(path)
        if not Path(case['binary']).is_file():raise ValueError('source-built binary missing')
        cases.append(case)
    plan=dict(cases=cases,input_sha256=inputs,official_score='UNKNOWN',
              hidden_gate='NOT_TESTED_DIAGNOSTIC_SUBSTITUTE_ONLY')
    validate_plan(plan)
    with a.out.open('x') as f:json.dump(plan,f,indent=2)

if __name__=='__main__':main()
