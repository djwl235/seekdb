"""TEST_ONLY execution-contract checks, never GPU evidence."""
import copy
import unittest
from run_pairs import validate_plan,verify_inputs,ORDER
from run_sql_benchmark import validate_ids
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from run_cold_case import find_server,stop_owned

class ExecutionTests(unittest.TestCase):
    def plan(self):
        return {'cases':[dict(dataset=d,variant=v,ordinal=n,base_tsv=d+'/base.tsv',queries_f32=d+'/q.f32',
            ids_json=d+'/ids.json',truth_json=d+'/truth.json',dataset_role='public-sift' if d=='sift' else 'diagnostic-256',
            card_device='/sys/class/drm/card0/device') for d,v,n in ORDER]}
    def test_fixed_order(self):
        validate_plan(self.plan())
        p=self.plan();p['cases'][0],p['cases'][1]=p['cases'][1],p['cases'][0]
        with self.assertRaises(ValueError):validate_plan(p)
    def test_base_vector_identity(self):
        p=self.plan();p['cases'][1]['base_tsv']='different.tsv'
        with self.assertRaises(ValueError):validate_plan(p)
    def test_hidden_diagnostic_role(self):
        p=self.plan();p['cases'][3]['dataset_role']='official-hidden'
        with self.assertRaises(ValueError):validate_plan(p)
    def test_same_gpu_device(self):
        p=self.plan();p['cases'][1]['card_device']='/sys/class/drm/card1/device'
        with self.assertRaises(ValueError):validate_plan(p)
    def test_no_profile_timing_mix(self):
        p=self.plan();p['cases'][2]['profiler']=True
        with self.assertRaises(ValueError):validate_plan(p)
    def test_no_lossy_id_conversion(self):
        for value in [3.5,True,'3']:
            with self.assertRaises(ValueError):validate_ids([(value,)],1,{3})
    def test_inputs_bound_before_each_case(self):
        from run_sql_benchmark import sha
        with tempfile.TemporaryDirectory(prefix='amd-TEST_ONLY-input-') as name:
            path=Path(name)/'input';path.write_bytes(b'original')
            case={key:str(path) for key in ['base_tsv','queries_f32','ids_json','truth_json']}
            hashes={str(path):sha(path)};verify_inputs(case,hashes)
            path.write_bytes(b'changed')
            with self.assertRaises(ValueError):verify_inputs(case,hashes)
    def test_owned_process_lifecycle_only(self):
        with tempfile.TemporaryDirectory(prefix='amd-TEST_ONLY-lifecycle-') as name:
            directory=Path(name)
            binary=Path(sys.executable).resolve()
            child=subprocess.Popen([sys.executable,'-c','import time;time.sleep(60)',name])
            try:
                import time
                deadline=time.monotonic()+3
                while find_server(binary,directory)!=child.pid and time.monotonic()<deadline:time.sleep(.01)
                self.assertEqual(find_server(binary,directory),child.pid)
                with self.assertRaises(RuntimeError):stop_owned(os.getpid(),binary,directory)
                stop_owned(child.pid,binary,directory)
                child.wait(timeout=3)
                self.assertNotEqual(child.returncode,0)
                # An already-reaped server is a successful cleanup, not a new
                # error that can mask its original failure.
                stop_owned(child.pid,binary,directory)
            finally:
                if child.poll() is None:child.kill();child.wait()
    def test_exact_oracle_ties_and_noncontiguous_ids(self):
        import numpy as np
        from prepare_data import nearest
        base=np.array([[0.,0.],[1.,0.],[-1.,0.],[4.,0.]],dtype=np.float64)
        self.assertEqual(nearest(base,np.array([[0.,0.]]),np.array([10,30,20,40]),3),[[10,20,30]])

if __name__=='__main__':unittest.main()
