"""Sample a known server and AMD sysfs VRAM; does not prove GPU computation."""
import argparse
import json
from pathlib import Path
import time

def snapshot(pid, card):
    status = Path('/proc/%d/status' % pid).read_text()
    fields = dict(line.split(':',1) for line in status.splitlines() if ':' in line)
    if 'VmRSS' not in fields:raise ProcessLookupError('process exited/zombie')
    return dict(epoch_ns=time.time_ns(),pid=pid,
                rss_bytes=int(fields['VmRSS'].split()[0])*1024,
                hwm_bytes=int(fields['VmHWM'].split()[0])*1024,
                cpus_allowed=fields['Cpus_allowed_list'].strip(),
                vram_used_bytes=int((card/'mem_info_vram_used').read_text()),
                vram_total_bytes=int((card/'mem_info_vram_total').read_text()))

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--pid',type=int,required=True);p.add_argument('--card-device',type=Path,required=True)
    p.add_argument('--binary',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
    p.add_argument('--interval',type=float,default=.1)
    a=p.parse_args()
    if (Path('/proc/%d/exe'%a.pid).resolve()!=a.binary.resolve() or
        not a.card_device.resolve().is_relative_to(Path('/sys/devices'))):
        raise SystemExit('refuse unverified process or non-sysfs device')
    with a.out.open('x') as f:
        while Path('/proc/%d'%a.pid).exists():
            try: row=snapshot(a.pid,a.card_device)
            except (FileNotFoundError,ProcessLookupError):break
            f.write(json.dumps(row)+'\n');f.flush();time.sleep(a.interval)
