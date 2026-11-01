// TEST_ONLY: execute the actual bridge against a host fault-injection shim.
#include "mock_api.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <climits>
#include <map>
#include <thread>
#include <vector>
#include <atomic>
#include "../seekdb_cuvs_bridge.c"
static std::map<void *,size_t> allocations;
static int alloc_calls=0, params_calls=0, fail_alloc=-1, copies=0, fail_copy=-1;
static int resources=0, indexes=0, index_params=0, search_params=0;
static int fail_stage=0;
static std::atomic<int> active{0};
static std::atomic<bool> hold_search{false};
static std::map<void *,std::thread::id> resource_threads;
static thread_local int current_device=-1;
int cudaGetDevice(int *device) { if(fail_stage==8)return 1; *device=3;return 0; }
int cudaSetDevice(int device) { if(fail_stage==9)return 1;current_device=device;return 0; }
int cuvsResourcesCreate(cuvsResources_t *r) {
  assert(current_device==3);
  pthread_attr_t attr;void *address;size_t size;
  assert(!pthread_getattr_np(pthread_self(),&attr));
  assert(!pthread_attr_getstack(&attr,&address,&size) && size>=32UL*1024*1024);
  pthread_attr_destroy(&attr);
  if(fail_stage==1) return 1; *r=malloc(1); ++resources;
  resource_threads[*r]=std::this_thread::get_id();return 0;
}
int cuvsResourcesDestroy(cuvsResources_t r) {
  assert(resource_threads.at(r)==std::this_thread::get_id());resource_threads.erase(r);
  free(r); --resources; return 0;
}
int cuvsStreamSync(cuvsResources_t r) {
  assert(resource_threads.at(r)==std::this_thread::get_id());return fail_stage==7 ? 1 : 0;
}
int cuvsCagraIndexParamsCreate(cuvsCagraIndexParams_t *p) { if(fail_stage==2) return 1; *p=(cuvsCagraIndexParams_t)calloc(1,sizeof(**p)); ++index_params; return 0; }
int cuvsCagraIndexParamsDestroy(cuvsCagraIndexParams_t p) { free(p); --index_params; return 0; }
int cuvsCagraIndexCreate(cuvsCagraIndex_t *p) { if(fail_stage==3) return 1; *p=(cuvsCagraIndex_t)calloc(1,sizeof(**p)); ++indexes; return 0; }
int cuvsCagraIndexDestroy(cuvsCagraIndex_t p) { free(p); --indexes; return 0; }
int cuvsCagraBuild(cuvsResources_t,cuvsCagraIndexParams_t p,DLManagedTensor *d,cuvsCagraIndex_t i) {
  assert(p->build_algo==NN_DESCENT && p->graph_degree==128 && p->intermediate_graph_degree==256 && p->nn_descent_niter==100);
  if(fail_stage==4) return 1;
  i->n=d->dl_tensor.shape[0]; i->dim=d->dl_tensor.shape[1]; return 0;
}
int cuvsRMMAlloc(cuvsResources_t,void **p,size_t n) { if (++alloc_calls==fail_alloc) return 1; *p=malloc(n); assert(*p); allocations[*p]=n; return 0; }
int cuvsRMMFree(cuvsResources_t,void *p,size_t n) { assert(allocations.at(p)==n); allocations.erase(p); free(p); return 0; }
int cuvsCagraSearchParamsCreate(cuvsCagraSearchParams_t *p) { if(fail_stage==5) return 1; *p=(cuvsCagraSearchParams_t)calloc(1,sizeof(**p)); (*p)->itopk_size=64; ++params_calls; ++search_params; return 0; }
int cuvsCagraSearchParamsDestroy(cuvsCagraSearchParams_t p) { free(p); --search_params; return 0; }
int cuvsCagraSearch(cuvsResources_t r,cuvsCagraSearchParams_t p,cuvsCagraIndex_t,DLManagedTensor *,DLManagedTensor *n,DLManagedTensor *d,cuvsFilter) {
  if(fail_stage==6) return 1;
  assert(resource_threads.at(r)==std::this_thread::get_id() && current_device==3);
  assert(++active==1); std::this_thread::yield();
  while(hold_search.load()) std::this_thread::yield();
  const long k=n->dl_tensor.shape[1], count=n->dl_tensor.shape[0]*k;
  assert(p->itopk_size==(size_t)(k*4>512?k*4:512) && p->search_width==4);
  for(long j=0;j<count;++j) { ((unsigned *)n->dl_tensor.data)[j]=(unsigned)(j%k); ((float *)d->dl_tensor.data)[j]=(float)(j%k); }
  --active; return 0;
}
int cudaMemcpy(void *d,const void *s,size_t n,int) { if (++copies==fail_copy) return 1; memcpy(d,s,n); return 0; }
int cuvsCagraSerialize(cuvsResources_t,const char *,cuvsCagraIndex_t,bool) { return 0; }
int cuvsCagraDeserialize(cuvsResources_t,const char *,cuvsCagraIndex_t i) { i->n=1000;i->dim=8;return 0; }
int cuvsCagraIndexGetSize(cuvsCagraIndex_t i,int64_t *n) { *n=i->n;return 0; }
int cuvsCagraIndexGetDims(cuvsCagraIndex_t i,int64_t *d) { *d=i->dim;return 0; }
const char *cuvsGetLastErrorText(void) { return "TEST_ONLY injected error"; }
static void clean() { assert(allocations.empty() && resource_threads.empty() && !resources && !indexes && !index_params && !search_params); }
int main() {
  assert(sk_quota_budget(2400000,100000)==24);
  assert(sk_quota_budget(50000,100000)==1);
  assert(sk_quota_budget(-1,100000)==0 && sk_quota_budget(1,0)==0);
  cpu_set_t allowed,chosen,before,after;
  CPU_ZERO(&allowed); CPU_SET(2,&allowed);CPU_SET(5,&allowed);CPU_SET(8,&allowed);
  assert(sk_compact_mask(&allowed,2,&chosen) && CPU_COUNT(&chosen)==2 && CPU_ISSET(2,&chosen) && CPU_ISSET(5,&chosen));
  assert(!sk_compact_mask(&allowed,3,&chosen) && !sk_compact_mask(&allowed,0,&chosen));
  assert(sched_getaffinity(0,sizeof(before),&before)==0);
  float base[8]={},q[8]={}; unsigned ids[1000]; float dist[1000];
  assert(!seekdb_cuvs_build(nullptr,1000,8)); assert(!seekdb_cuvs_build(base,0,8));
  assert(!seekdb_cuvs_build(base,LONG_MAX,LONG_MAX));
  for(int stage=8;stage<=9;++stage) { fail_stage=stage;assert(!seekdb_cuvs_build(base,1000,8));fail_stage=0;clean(); }
  for(int stage=1;stage<=4;++stage) {
    fail_stage=stage; assert(!seekdb_cuvs_build(base,1000,8)); fail_stage=0; clean();
    assert(sched_getaffinity(0,sizeof(after),&after)==0 && CPU_EQUAL(&before,&after));
  }
  void *h=seekdb_cuvs_build(base,1000,8); assert(h);
  assert(sched_getaffinity(0,sizeof(after),&after)==0 && CPU_EQUAL(&before,&after));
  for(int i=0;i<1000;++i) assert(!seekdb_cuvs_search(h,q,1,10,ids,dist));
  assert(alloc_calls==3 && params_calls==1); assert(ids[9]==9 && dist[9]==9);
  assert(!seekdb_cuvs_search(h,q,1,100,ids,dist)); assert(alloc_calls==5);
  assert(!seekdb_cuvs_search(h,q,1,300,ids,dist));
  assert(!seekdb_cuvs_search(h,q,1,10,ids,dist)); // reset itopk after a larger call
  assert(seekdb_cuvs_search(h,q,0,10,ids,dist));
  assert(seekdb_cuvs_search(h,q,LONG_MAX,10,ids,dist));
  assert(seekdb_cuvs_search(h,q,1,1001,ids,dist));
  assert(seekdb_cuvs_search(h,q,1,10,nullptr,dist));
  fail_copy=copies+3; assert(seekdb_cuvs_search(h,q,1,10,ids,dist)==17); fail_copy=-1;
  assert(!seekdb_cuvs_search(h,q,1,10,ids,nullptr));
  std::vector<std::thread> threads;
  for(int t=0;t<4;++t) threads.emplace_back([&]{ unsigned x[10];float y[10];for(int i=0;i<100;++i) assert(!seekdb_cuvs_search(h,q,1,10,x,y)); });
  for(auto &t:threads) t.join(); seekdb_cuvs_free(h); clean();
  h=seekdb_cuvs_build(base,1000,8);
  assert(!seekdb_cuvs_search(h,q,1,10,ids,dist));
  const auto retained=allocations;
  std::vector<float> large_query(600000*8), large_dist(600000);
  std::vector<unsigned> large_ids(600000);
  assert(!seekdb_cuvs_search(h,large_query.data(),600000,1,large_ids.data(),large_dist.data()));
  assert(allocations==retained); // >16 MiB request must not remain in cache
  seekdb_cuvs_free(h); clean();
  for(int offset=1;offset<=3;++offset) {
    h=seekdb_cuvs_build(base,1000,8); fail_alloc=alloc_calls+offset;
    assert(seekdb_cuvs_search(h,q,1,10,ids,dist)==9+offset); fail_alloc=-1;
    assert(!seekdb_cuvs_search(h,q,1,10,ids,dist)); seekdb_cuvs_free(h); clean();
  }
  h=seekdb_cuvs_build(base,1000,8); fail_stage=5;
  assert(seekdb_cuvs_search(h,q,1,10,ids,dist)==14); fail_stage=0;
  assert(!seekdb_cuvs_search(h,q,1,10,ids,dist)); fail_stage=6;
  assert(seekdb_cuvs_search(h,q,1,10,ids,dist)==15); fail_stage=0;
  assert(!seekdb_cuvs_search(h,q,1,10,ids,dist)); seekdb_cuvs_free(h); clean();
  h=seekdb_cuvs_build(base,1000,8);fail_stage=7;
  assert(seekdb_cuvs_search(h,q,1,10,ids,dist)==18);fail_stage=0;
  assert(!seekdb_cuvs_search(h,q,1,10,ids,dist));
  assert(!seekdb_cuvs_serialize(h,"mock"));seekdb_cuvs_free(h);clean();
  // Destroy must wait for the executing job and drain registered slot waiters.
  h=seekdb_cuvs_build(base,1000,8);hold_search=true;
  threads.clear();std::atomic<int> completed{0};
  for(int i=0;i<4;++i) threads.emplace_back([&]{ unsigned x[10];float y[10];
    int rc=seekdb_cuvs_search(h,q,1,10,x,y);assert(rc==0 || rc==1);++completed; });
  auto *worker=(sk_worker *)h;
  for(;;) {
    pthread_mutex_lock(&worker->control);unsigned entered=worker->active;
    pthread_mutex_unlock(&worker->control);
    if(entered==4 && active.load()==1)break;std::this_thread::yield();
  }
  std::thread destroy([&]{seekdb_cuvs_free(h);});
  for(;;) {
    pthread_mutex_lock(&worker->control);bool stopping=worker->stopping;
    pthread_mutex_unlock(&worker->control);if(stopping)break;std::this_thread::yield();
  }
  hold_search=false;
  for(auto &t:threads)t.join();destroy.join();assert(completed==4);clean();
  assert(!seekdb_cuvs_deserialize("mock",1001,8)); clean();
  h=seekdb_cuvs_deserialize("mock",1000,8); assert(h);
  assert(!seekdb_cuvs_search(h,q,1,10,ids,dist)); seekdb_cuvs_free(h); clean();
  puts("TEST_ONLY PASS: bounded worker/device/stack ownership, sync errors, shutdown drains active/waiters, allocation reuse/growth, exact parameters, invalid shapes, copy/allocation errors, retry, concurrency, reload cleanup. GPU/Recall/performance NOT TESTED.");
}
