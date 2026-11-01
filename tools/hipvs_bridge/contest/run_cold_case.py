"""One real, fresh-process V5 SQL run with PID/library/resource evidence.

Requires a prepared GPU environment; no container rental or account operations.
Every output directory must be new. Persistent data are retained, never reused.
"""
import argparse
import datetime
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import time

from run_sql_benchmark import sha

HERE=Path(__file__).resolve().parent
BASE='634888d9b037153fa143d2c913ba4b25503e1d62'

def find_server(binary, directory):
    for entry in Path('/proc').iterdir():
        if not entry.name.isdigit(): continue
        try:
            if (entry/'exe').resolve()!=binary: continue
            args=(entry/'cmdline').read_bytes().split(b'\0')
            if str(directory).encode() in args:return int(entry.name)
        except OSError:pass
    return None

def stop_owned(pid,binary,directory):
    if not pid:return
    proc=Path('/proc/%d/stat'%pid)
    try:
        if proc.read_text().split()[2]=='Z':return
    except FileNotFoundError:return
    if find_server(binary,directory)!=pid:raise RuntimeError('server PID identity changed; refuse signal')
    try:os.kill(pid,signal.SIGTERM)
    except ProcessLookupError:return
    for _ in range(300):
        try:
            if proc.read_text().split()[2]=='Z':return
        except FileNotFoundError:return
        time.sleep(.1)
    # Revalidate before escalation; a recycled PID must never receive a signal.
    if find_server(binary,directory)!=pid:raise RuntimeError('server PID identity changed; refuse escalation')
    try:os.kill(pid,signal.SIGKILL)
    except ProcessLookupError:return

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--root',type=Path,required=True);p.add_argument('--binary',type=Path,required=True)
    p.add_argument('--bridge',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
    p.add_argument('--dataset',choices=['sift','hidden'],required=True)
    p.add_argument('--dataset-role',choices=['public-sift','diagnostic-256','official-hidden'],required=True)
    p.add_argument('--variant',choices=['baseline','candidate'],required=True)
    p.add_argument('--ordinal',type=int,required=True);p.add_argument('--port',type=int,required=True)
    for name in ['base-tsv','queries-f32','ids-json','truth-json']:p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--card-device',type=Path,required=True)
    p.add_argument('--profiler',action='store_true',help='diagnostic profiled run; do not mix with unprofiled timing pairs')
    a=p.parse_args()
    def interrupted(_signal,_frame):raise SystemExit('controlled case timeout/termination')
    signal.signal(signal.SIGTERM,interrupted)
    import pymysql
    root=a.root.resolve(strict=True);binary=a.binary.resolve(strict=True);bridge=a.bridge.resolve(strict=True)
    commit=subprocess.check_output(['git','-C',str(root),'rev-parse','HEAD'],text=True).strip()
    subprocess.run(['git','-C',str(root),'merge-base','--is-ancestor',BASE,'HEAD'],check=True)
    dirty=subprocess.check_output(['git','-C',str(root),'status','--porcelain'])
    if dirty:raise ValueError('require exact clean committed source before real paired runs')
    if a.variant=='baseline' and commit!=BASE:raise ValueError('wrong frozen baseline')
    with socket.socket() as probe:probe.bind(('127.0.0.1',a.port))
    a.out.mkdir(parents=True,exist_ok=False)
    out=a.out.resolve();directory=out/'instance';directory.mkdir()
    environment=os.environ.copy()
    environment.update(LD_LIBRARY_PATH=str(bridge.parent)+':/opt/hipvs/lib:/opt/rocm/lib',TELEMETRY_ENABLED='false')
    for key in ['OB_VSAG_USE_CUVS','OB_VSAG_TRACE','OB_VSAG_TRACE_FILE']:environment.pop(key,None)
    command=[str(binary),'--nodaemon','--port',str(a.port),'--data-dir',str(directory/'store'),
             '--base-dir',str(directory),'--parameter','__min_full_resource_pool_memory=2147483648',
             '--parameter','memory_budget=80G','--parameter','datafile_size=20G',
             '--parameter','cpu_count=24','--parameter','local_ip=127.0.0.1']
    if a.profiler:
        command=['/opt/rocm/bin/rocprofv3','--kernel-trace','--hip-trace','--memory-copy-trace',
                 '--output-format','csv','--output-directory',str(out/'profiler'),'--',*command]
    state=dict(state='STARTING',source_commit=commit,binary_sha256=sha(binary),bridge_sha256=sha(bridge),
               command=command,started_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
               fresh_directory=str(directory),profiler=a.profiler,gpu_path='UNKNOWN',official_score='UNKNOWN')
    def save(): (out/'lifecycle.json').write_text(json.dumps(state,indent=2)+'\n')
    pid=None;sampler=None;connection=None
    with (out/'server.log').open('x') as log:
        process=subprocess.Popen(command,env=environment,cwd=directory,stdin=subprocess.DEVNULL,
                                 stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
    state['launcher_pid']=process.pid;save()
    try:
        deadline=time.monotonic()+180
        while time.monotonic()<deadline:
            if process.poll() is not None:raise RuntimeError('server/profiler exited during startup')
            pid=find_server(binary,directory)
            if pid:
                try:
                    connection=pymysql.connect(host='127.0.0.1',port=a.port,user=os.environ.get('AMD_DB_USER','root'),
                        password=os.environ.get('AMD_DB_PASSWORD',''),autocommit=True,local_infile=True,read_timeout=300)
                    break
                except pymysql.MySQLError:pass
            time.sleep(.2)
        if connection is None:raise TimeoutError('source server startup')
        state['server_pid']=pid
        maps=Path('/proc/%d/maps'%pid).read_text();(out/'loaded-maps.txt').write_text(maps)
        # bridge may dlopen only during DDL: verify again after actual queries.
        sampler=subprocess.Popen([sys.executable,str(HERE/'sample_resources.py'),'--pid',str(pid),
            '--binary',str(binary),'--card-device',str(a.card_device),'--out',str(out/'resources.jsonl')])
        database='amd_v5_case_%d'%a.ordinal
        with connection.cursor() as cursor:
            cursor.execute('CREATE DATABASE '+database);cursor.execute('USE '+database)
            cursor.execute('CREATE TABLE items(id BIGINT PRIMARY KEY,v VECTOR(%d))'%(128 if a.dataset=='sift' else 256))
            cursor.execute('SET ob_query_timeout=300000000')
            cursor.execute("LOAD DATA LOCAL INFILE %s INTO TABLE items FIELDS TERMINATED BY '\\t' LINES TERMINATED BY '\\n' (id,v)",(str(a.base_tsv.resolve(strict=True)),))
        connection.close();connection=None
        state.update(state='IMPORTED',base_tsv_sha256=sha(a.base_tsv));save()
        benchmark=[sys.executable,str(HERE/'run_sql_benchmark.py'),'--dataset',a.dataset,
            '--dataset-role',a.dataset_role,'--variant',a.variant,'--ordinal',str(a.ordinal),
            '--database',database,'--port',str(a.port),'--source-commit',commit,
            '--seekdb-binary',str(binary),'--bridge',str(bridge),'--out',str(out/'measurement')]
        for name in ['base_tsv','queries_f32','ids_json','truth_json']:
            benchmark.extend(['--'+name.replace('_','-'),str(getattr(a,name).resolve(strict=True))])
        with (out/'benchmark.log').open('x') as log:
            measured=subprocess.run(benchmark,env=environment,stdout=log,stderr=subprocess.STDOUT)
        state['benchmark_returncode']=measured.returncode
        if measured.returncode:raise RuntimeError('SQL measurement failed')
        maps=Path('/proc/%d/maps'%pid).read_text();(out/'loaded-maps-after-ddl.txt').write_text(maps)
        loaded=[line.split()[-1] for line in maps.splitlines() if 'libseekdb_cuvs_bridge' in line]
        if not loaded or any(Path(path).resolve()!=bridge for path in loaded):raise ValueError('runtime loaded another/missing bridge')
        fixed=Path('/opt/hipvs/lib/libcuvs_c.so').resolve(strict=True)
        libraries=[line.split()[-1] for line in maps.splitlines() if 'libcuvs_c.so' in line]
        if not libraries or any(Path(path).resolve()!=fixed for path in libraries):raise ValueError('wrong/missing fixed hipVS library mapping')
        state['fixed_library_sha256']=sha(fixed)
        for logfile in [out/'server.log',directory/'log/seekdb.log']:
            if logfile.exists() and 'lazily built VSAG fallback' in logfile.read_text(errors='replace'):
                raise RuntimeError('cpu_fallback_observed in GPU SQL case')
        if sampler.poll() is not None:raise RuntimeError('resource sampler failed')
        state.update(state='LOCAL_SQL_COMPLETE_EXTERNAL_GPU_GATES_PENDING',loaded_bridge_verified=True)
    except BaseException as error:
        state.update(state='FAILED',error=repr(error));raise
    finally:
        cleanup_errors=[]
        try:
            if connection:connection.close()
            if pid:stop_owned(pid,binary,directory)
            elif process.poll() is None:process.terminate()
        except Exception as error:cleanup_errors.append(repr(error))
        try:
            if sampler:
                try:sampler.wait(timeout=5)
                except subprocess.TimeoutExpired:sampler.terminate();sampler.wait(timeout=5)
            # If identity verification refused a signal, preserve the process for
            # manual diagnosis rather than killing a potentially unrelated PID.
            if not cleanup_errors:
                try:process.wait(timeout=30)
                except subprocess.TimeoutExpired:process.kill();process.wait()
        except Exception as error:cleanup_errors.append(repr(error))
        state.update(finished_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                     launcher_exit=process.poll(),cleanup_errors=cleanup_errors)
        if cleanup_errors:state['state']='FAILED_CLEANUP_REQUIRED'
        save()
        if cleanup_errors and sys.exc_info()[0] is None:raise RuntimeError('cleanup failed; inspect lifecycle.json')

if __name__=='__main__':main()
