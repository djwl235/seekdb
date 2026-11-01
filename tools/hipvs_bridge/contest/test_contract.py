"""TEST_ONLY fixtures for SQL metric validation; not a GPU or dataset run."""
import unittest
from compare_reports import BASE,compare
from run_sql_benchmark import validate_ids,recall,DDL


class ContractTests(unittest.TestCase):
    def reports(self):
        records=[]
        for ds,v,n in [('sift','baseline',1),('sift','candidate',2),('hidden','candidate',3),('hidden','baseline',4)]:
            records.append(dict(dataset=ds,variant=v,ordinal=n,source_commit=BASE,
                input_hashes={'fixture':'TEST_ONLY'},state='SQL_MEASURED_GPU_AND_RESOURCE_GATES_PENDING',
                nq=4096,rows=250000,build_to_first_query_seconds=30 if v=='baseline' else 10,
                query={str(k):dict(qps=100 if v=='baseline' else 150,recall=.99) for k in [10,100]}))
        return records
    def test_scoring_and_unknown_gpu(self):
        r=compare(self.reports())
        self.assertEqual(r['official_score'],'UNKNOWN'); self.assertEqual(r['gpu_path'],'UNKNOWN')
        self.assertAlmostEqual(r['local_weighted_build_ratio'],3)
        self.assertAlmostEqual(r['conditional_performance_points_if_all_external_gates_pass']['build'],14)
    def test_order_identity_and_frozen_baseline(self):
        for field,value in [('ordinal',99),('source_commit','0'*40),('input_hashes',{})]:
            r=self.reports();r[0][field]=value
            with self.assertRaises(ValueError):compare(r)
    def test_quality_delta_and_regression(self):
        r=self.reports();r[1]['query']['10']['recall']=.98
        self.assertFalse(compare(r)['datasets']['sift']['quality']['10']['baseline_delta'])
        r=self.reports();r[1]['query']['10']['qps']=50
        self.assertFalse(compare(r)['datasets']['sift']['query_gate'])
    def test_zero_recall_is_failed_quality_not_missing_measurement(self):
        r=self.reports();r[1]['query']['10']['recall']=0
        self.assertFalse(compare(r)['datasets']['sift']['quality']['10']['absolute'])
    def test_bad_measurement(self):
        for value in [0,float('nan'),float('inf'),True]:
            r=self.reports();r[0]['build_to_first_query_seconds']=value
            with self.assertRaises(ValueError):compare(r)
    def test_id_integrity_and_recall(self):
        self.assertEqual(validate_ids([(3,),(4,)],2,{3,4}),[3,4])
        for rows in [[(3,),(3,)],[(3,)],[(3,),(5,)]]:
            with self.assertRaises(ValueError):validate_ids(rows,2,{3,4})
        self.assertEqual(recall([3,4],[3,5],2),.5)
        self.assertIn('lib=cuvs',DDL);self.assertIn('ef_search=64',DDL)


if __name__=='__main__':unittest.main()
