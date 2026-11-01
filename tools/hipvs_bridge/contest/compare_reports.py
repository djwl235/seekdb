"""Summarize local V5 pairs; never award an official score or infer GPU proof."""
import argparse
import json
import math
from pathlib import Path

BASE='634888d9b037153fa143d2c913ba4b25503e1d62'


def compare(reports):
    keyed={(r['dataset'],r['variant']):r for r in reports}
    if len(reports)!=4 or len(keyed)!=4:
        raise ValueError('exactly four distinct SIFT/hidden baseline/candidate reports required')
    result={'official_score':'UNKNOWN','gpu_path':'UNKNOWN','vram_gate':'UNKNOWN',
            'host_resource_gate':'UNKNOWN','cold_cache_and_same_hardware':'EXTERNAL_EVIDENCE_REQUIRED',
            'datasets':{}, 'note':'Numbers below are conditional local SQL metrics, not an official result.'}
    for dataset,weight,order in [('sift',.4,(1,2)),('hidden',.6,(4,3))]:
        b,c=(keyed[(dataset,v)] for v in ['baseline','candidate'])
        if (b['ordinal'],c['ordinal'])!=order: raise ValueError('paired execution order mismatch')
        if b['source_commit']!=BASE: raise ValueError('baseline is not frozen V5')
        if b['input_hashes']!=c['input_hashes']: raise ValueError('pair input identities differ')
        if b.get('dataset_role')!=c.get('dataset_role'):raise ValueError('pair dataset roles differ')
        if b.get('dim',128 if dataset=='sift' else 256)!=(128 if dataset=='sift' else 256) or c.get('dim',128 if dataset=='sift' else 256)!=(128 if dataset=='sift' else 256):
            raise ValueError('wrong dataset dimensions')
        for r in [b,c]:
            if r['state']!='SQL_MEASURED_GPU_AND_RESOURCE_GATES_PENDING' or r['nq']!=4096 or r['rows']!=250000:
                raise ValueError('incomplete SQL measurement')
            for number in [r['build_to_first_query_seconds']]+[r['query'][str(k)]['qps'] for k in [10,100]]:
                if type(number) not in [int,float] or not math.isfinite(number) or number<=0:
                    raise ValueError('nonpositive/nonfinite metrics')
            for k in [10,100]:
                number=r['query'][str(k)]['recall']
                if type(number) not in [int,float] or not math.isfinite(number) or not 0<=number<=1:
                    raise ValueError('invalid recall')
        quality={}
        for k,floor in [(10,.95 if dataset=='sift' else .90),(100,.95 if dataset=='sift' else .85)]:
            br,cr=b['query'][str(k)]['recall'],c['query'][str(k)]['recall']
            if br>1 or cr>1: raise ValueError('recall exceeds 1')
            quality[str(k)]={'absolute':cr>=floor,'baseline_delta':cr>=br-.005}
        qr=math.sqrt(math.prod(c['query'][str(k)]['qps']/b['query'][str(k)]['qps'] for k in [10,100]))
        result['datasets'][dataset]={'weight':weight,'quality':quality,'query_gate':qr>=.9,
            'dataset_role':b.get('dataset_role','LEGACY_UNATTESTED'),
            'query_ratio':qr,'build_ratio':b['build_to_first_query_seconds']/c['build_to_first_query_seconds']}
    build=math.prod(d['build_ratio']**d['weight'] for d in result['datasets'].values())
    query=math.prod(d['query_ratio']**d['weight'] for d in result['datasets'].values())
    result['local_weighted_build_ratio']=build; result['local_weighted_query_ratio']=query
    result['conditional_performance_points_if_all_external_gates_pass']={
        'build':14*max(0,min(1,math.log(build)/math.log(3))),
        'query':2*max(0,min(1,math.log(query)/math.log(1.5)))}
    return result


if __name__=='__main__':
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('report',nargs=4,type=Path); ap.add_argument('--out',type=Path,required=True)
    a=ap.parse_args()
    r=compare([json.loads(p.read_text()) for p in a.report])
    with a.out.open('x') as f: json.dump(r,f,indent=2);f.write('\n')
