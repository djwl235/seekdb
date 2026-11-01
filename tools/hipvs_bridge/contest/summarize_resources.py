"""Conservative local resource comparison; GPU path remains an external gate."""
import argparse
import json
from pathlib import Path

def summarize(path):
    rows=[json.loads(line) for line in path.read_text().splitlines() if line]
    if len(rows)<2:raise ValueError('insufficient resource samples')
    return dict(samples=len(rows),peak_vram_bytes=max(x['vram_used_bytes'] for x in rows),
                peak_host_rss_bytes=max(x['hwm_bytes'] for x in rows),
                sample_period_seconds=(rows[-1]['epoch_ns']-rows[0]['epoch_ns'])/1e9)

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--baseline',type=Path,required=True);p.add_argument('--candidate',type=Path,required=True)
    p.add_argument('--out',type=Path,required=True);a=p.parse_args()
    b,c=summarize(a.baseline),summarize(a.candidate)
    result=dict(baseline=b,candidate=c,gpu_path='UNKNOWN',official_score='UNKNOWN',
        note='100ms polling can miss peaks; confirm with external profiler/provider peak counters',
        sampled_physical_vram_gate=c['peak_vram_bytes']<=45_000_000_000,
        sampled_relative_vram_gate=c['peak_vram_bytes']<=1.25*b['peak_vram_bytes'],
        sampled_host_memory_gate=c['peak_host_rss_bytes']<=96_000_000_000)
    with a.out.open('x') as f:json.dump(result,f,indent=2)
