"""Convert supplied FP32 arrays and compute a chunked exact FP64-L2 oracle.

Tool-side NumPy only; no seekdb build dependency. A development256-d set remains
diagnostic and is never promoted to the private Hidden250K dataset.
"""
import argparse
import json
import os
from pathlib import Path
from run_sql_benchmark import sha

def nearest(base, queries, ids, k=100):
    import numpy as np
    norms=np.einsum('ij,ij->i',base,base)
    scores=norms[None,:]+np.einsum('ij,ij->i',queries,queries)[:,None]-2*(queries@base.T)
    output=[]
    for row in scores:
        threshold=np.partition(row,k-1)[k-1]
        choices=np.flatnonzero(row<=threshold)
        order=np.lexsort((ids[choices],row[choices]))[:k]
        output.append([int(x) for x in ids[choices[order]]])
    return output

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--base-npy',type=Path,required=True);p.add_argument('--queries-npy',type=Path,required=True)
    p.add_argument('--ids-json',type=Path,required=True);p.add_argument('--provenance',required=True)
    p.add_argument('--out',type=Path,required=True)
    a=p.parse_args()
    for name in ['OPENBLAS_NUM_THREADS','MKL_NUM_THREADS','OMP_NUM_THREADS']:os.environ[name]='1'
    import numpy as np
    base=np.load(a.base_npy,mmap_mode='r',allow_pickle=False)
    queries=np.load(a.queries_npy,mmap_mode='r',allow_pickle=False)
    ids_list=json.loads(a.ids_json.read_text())
    if (base.dtype.kind!='f' or base.dtype.itemsize!=4 or queries.dtype.kind!='f' or queries.dtype.itemsize!=4
        or base.shape[0]!=250000 or base.ndim!=2 or base.shape[1] not in [128,256]
        or queries.shape!=(4096,base.shape[1]) or len(ids_list)!=250000
        or any(type(x) is not int or not -(1<<63)<=x<(1<<63) for x in ids_list) or len(set(ids_list))!=250000):
        raise ValueError('require250K supplied FP32 rows,4096 same-dimension queries, distinct int64 IDs')
    if not np.isfinite(base).all() or not np.isfinite(queries).all():raise ValueError('nonfinite vectors')
    a.out.mkdir(parents=True,exist_ok=False)
    ids=np.array(ids_list,dtype=np.int64)
    (a.out/'ids.json').write_text(json.dumps(ids_list))
    np.asarray(queries,dtype='<f4').tofile(a.out/'queries.f32')
    with (a.out/'base.tsv').open('x') as f:
        for identity,row in zip(ids_list,base):
            f.write(str(identity)+'\t['+','.join(format(float(value),'.9g') for value in row)+']\n')
    double_base=np.asarray(base,dtype=np.float64)
    truth=[]
    for start in range(0,4096,32):
        truth.extend(nearest(double_base,np.asarray(queries[start:start+32],dtype=np.float64),ids))
    (a.out/'truth.json').write_text(json.dumps(truth))
    receipt=dict(provenance=a.provenance,rows=250000,nq=4096,dim=base.shape[1],
        oracle='FP64 squared L2 on the exact supplied FP32 coordinates; ties by int64 ID',
        formal_hidden_validation='NOT_ESTABLISHED',
        input_sha256={str(path):sha(path) for path in [a.base_npy,a.queries_npy,a.ids_json]},
        output_sha256={path.name:sha(path) for path in a.out.iterdir() if path.is_file()})
    (a.out/'data-receipt.json').write_text(json.dumps(receipt,indent=2))

if __name__=='__main__':main()
