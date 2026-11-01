/* seekdb <-> hipVS bridge: plain-C CAGRA kNN; all cuVS/ROCm deps live here. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "seekdb_cuvs_bridge.h"
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/cagra.h>
#include <dlpack/dlpack.h>
#ifdef __HIP_PLATFORM_AMD__
#include <cuvs/cuda_runtime.h>
#else
#include <cuda_runtime.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <limits.h>
#include <pthread.h>
#include "sk_cpu_affinity.h"

typedef struct {
  void *query, *ids, *dist;
  size_t query_bytes, ids_bytes, dist_bytes;
} sk_workspace;

typedef struct {
  cuvsResources_t        res;
  cuvsCagraIndexParams_t ip;
  cuvsCagraIndex_t       idx;
  long n, dim;
  pthread_mutex_t mutex;
  cuvsCagraSearchParams_t search_params;
  size_t default_itopk;
  sk_workspace workspace;
} sk_cuvs_index;

static int sk_bytes(long rows, long cols, size_t width, size_t *bytes)
{
  if (rows <= 0 || cols <= 0 || (size_t)rows > SIZE_MAX / width / (size_t)cols) return 0;
  *bytes = (size_t)rows * (size_t)cols * width;
  return 1;
}

static void sk_release(sk_cuvs_index *h, sk_workspace *w)
{
  if (w->query) cuvsRMMFree(h->res, w->query, w->query_bytes);
  if (w->ids) cuvsRMMFree(h->res, w->ids, w->ids_bytes);
  if (w->dist) cuvsRMMFree(h->res, w->dist, w->dist_bytes);
  *w = (sk_workspace){NULL, NULL, NULL, 0, 0, 0};
}

/* Allocate before releasing the old buffer: failure leaves a usable cache. */
static int sk_reserve(sk_cuvs_index *h, void **ptr, size_t *capacity, size_t bytes)
{
  void *replacement = NULL;
  if (*capacity >= bytes) return 1;
  if (cuvsRMMAlloc(h->res, &replacement, bytes) != CUVS_SUCCESS) return 0;
  if (*ptr) cuvsRMMFree(h->res, *ptr, *capacity);
  *ptr = replacement;
  *capacity = bytes;
  return 1;
}

static void* sk_core_build(const float* base, long n, long dim)
{
  size_t bytes;
  if (!base || n > UINT_MAX || !sk_bytes(n, dim, sizeof(float), &bytes)) return NULL;
  sk_cuvs_index* h = (sk_cuvs_index*)calloc(1, sizeof(*h));
  if (!h) return NULL;
  if (pthread_mutex_init(&h->mutex, NULL) != 0) { free(h); return NULL; }
  sk_cpu_scope affinity = sk_compact_begin();
  if (cuvsResourcesCreate(&h->res) != CUVS_SUCCESS) { sk_compact_end(&affinity); pthread_mutex_destroy(&h->mutex); free(h); return NULL; }

  DLManagedTensor dset; long ds[2] = {n, dim};
  dset.dl_tensor.data = (void*)base;
  dset.dl_tensor.device.device_type = kDLCPU; dset.dl_tensor.device.device_id = 0;
  dset.dl_tensor.ndim = 2;
  dset.dl_tensor.dtype.code = kDLFloat; dset.dl_tensor.dtype.bits = 32; dset.dl_tensor.dtype.lanes = 1;
  dset.dl_tensor.shape = ds; dset.dl_tensor.strides = 0; dset.dl_tensor.byte_offset = 0;

  if (cuvsCagraIndexParamsCreate(&h->ip) != CUVS_SUCCESS) goto fail_res;
  h->ip->build_algo = NN_DESCENT;
  h->ip->graph_degree = 128;
  h->ip->intermediate_graph_degree = 256;
  h->ip->nn_descent_niter = 100;
  if (cuvsCagraIndexCreate(&h->idx)      != CUVS_SUCCESS) goto fail_ip;
  if (cuvsCagraBuild(h->res, h->ip, &dset, h->idx) != CUVS_SUCCESS) goto fail_idx;
  h->n = n; h->dim = dim;
  sk_compact_end(&affinity);
  return h;

fail_idx: cuvsCagraIndexDestroy(h->idx);
fail_ip:  cuvsCagraIndexParamsDestroy(h->ip);
fail_res: cuvsResourcesDestroy(h->res); sk_compact_end(&affinity); pthread_mutex_destroy(&h->mutex); free(h);
  fprintf(stderr, "[seekdb_cuvs] build failed: %s\n", cuvsGetLastErrorText());
  return NULL;
}

static int sk_core_search(void* handle, const float* query, long nq, long topk,
                       unsigned int* out_ids, float* out_dist)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  size_t qb, ib, db;
  if (!h || !query || !out_ids || topk > h->n || (size_t)topk > SIZE_MAX / 4 ||
      !sk_bytes(nq, h->dim, sizeof(float), &qb) ||
      !sk_bytes(nq, topk, sizeof(unsigned), &ib) ||
      !sk_bytes(nq, topk, sizeof(float), &db)) return 1;
  pthread_mutex_lock(&h->mutex);
  int rc = 0;
  sk_workspace temporary = {NULL, NULL, NULL, 0, 0, 0};
  /* Keep at most 16 MiB per buffer per index; large batch calls are transient. */
  const size_t cache_limit = 16UL * 1024 * 1024;
  sk_workspace *w = (qb <= cache_limit && ib <= cache_limit && db <= cache_limit) ? &h->workspace : &temporary;
  if (!sk_reserve(h, &w->query, &w->query_bytes, qb)) { rc=10; goto done; }
  if (!sk_reserve(h, &w->ids, &w->ids_bytes, ib)) { rc=11; goto done; }
  if (!sk_reserve(h, &w->dist, &w->dist_bytes, db)) { rc=12; goto done; }
  if (cudaMemcpy(w->query, query, qb, cudaMemcpyDefault) != cudaSuccess) { rc=13; goto done; }
  {
    DLManagedTensor qt, nt, dt; long qs[2]={nq,h->dim}, nsh[2]={nq,topk}, dsh[2]={nq,topk};
    qt.dl_tensor.data=w->query; qt.dl_tensor.device.device_type=kDLCUDA; qt.dl_tensor.device.device_id=0;
    qt.dl_tensor.ndim=2; qt.dl_tensor.dtype.code=kDLFloat; qt.dl_tensor.dtype.bits=32; qt.dl_tensor.dtype.lanes=1;
    qt.dl_tensor.shape=qs; qt.dl_tensor.strides=0; qt.dl_tensor.byte_offset=0;
    nt=qt; nt.dl_tensor.data=w->ids; nt.dl_tensor.dtype.code=kDLUInt; nt.dl_tensor.shape=nsh;
    dt=qt; dt.dl_tensor.data=w->dist; dt.dl_tensor.dtype.code=kDLFloat; dt.dl_tensor.shape=dsh;
    if (!h->search_params) {
      if (cuvsCagraSearchParamsCreate(&h->search_params) != CUVS_SUCCESS) { rc=14; goto done; }
      h->default_itopk = h->search_params->itopk_size;
    }
    {
      size_t required_itopk = (size_t)topk * 4;
      if (required_itopk < 512) required_itopk = 512;
      h->search_params->itopk_size = h->default_itopk > required_itopk ? h->default_itopk : required_itopk;
      h->search_params->search_width = 4;
    }
    cuvsFilter f; f.type=NO_FILTER; f.addr=(uintptr_t)0;
    if (cuvsCagraSearch(h->res, h->search_params, h->idx, &qt, &nt, &dt, f) != CUVS_SUCCESS) { rc=15; goto done; }
    if (cudaMemcpy(out_ids, w->ids, ib, cudaMemcpyDefault) != cudaSuccess) { rc=16; goto done; }
    if (out_dist && cudaMemcpy(out_dist, w->dist, db, cudaMemcpyDefault) != cudaSuccess) { rc=17; goto done; }
  }
done:
  sk_release(h, &temporary);
  pthread_mutex_unlock(&h->mutex);
  if (rc) fprintf(stderr, "[seekdb_cuvs] search rc=%d: %s\n", rc, cuvsGetLastErrorText());
  return rc;
}

static void sk_core_free(void* handle)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) return;
  sk_release(h, &h->workspace);
  if (h->search_params) cuvsCagraSearchParamsDestroy(h->search_params);
  cuvsCagraIndexDestroy(h->idx);
  cuvsCagraIndexParamsDestroy(h->ip);
  cuvsResourcesDestroy(h->res);
  pthread_mutex_destroy(&h->mutex);
  free(h);
}

static int sk_core_serialize(void* handle, const char* filename)
{
  sk_cuvs_index* index = (sk_cuvs_index*)handle;
  if (!index || !filename) return 1;
  pthread_mutex_lock(&index->mutex);
  int result = cuvsCagraSerialize(index->res, filename, index->idx, true);
  pthread_mutex_unlock(&index->mutex);
  if (result != CUVS_SUCCESS) {
    fprintf(stderr, "[seekdb_cuvs] serialize failed: %s\n", cuvsGetLastErrorText());
    return 1;
  }
  return 0;
}

static void* sk_core_deserialize(const char* filename, long n, long dim)
{
  size_t bytes;
  if (!filename || n > UINT_MAX || !sk_bytes(n, dim, sizeof(float), &bytes)) return NULL;
  sk_cuvs_index* index = (sk_cuvs_index*)calloc(1, sizeof(*index));
  if (!index) return NULL;
  if (pthread_mutex_init(&index->mutex, NULL) != 0) { free(index); return NULL; }
  if (cuvsResourcesCreate(&index->res) != CUVS_SUCCESS) goto fail;
  if (cuvsCagraIndexParamsCreate(&index->ip) != CUVS_SUCCESS) goto fail_res;
  if (cuvsCagraIndexCreate(&index->idx) != CUVS_SUCCESS) goto fail_ip;
  if (cuvsCagraDeserialize(index->res, filename, index->idx) != CUVS_SUCCESS) goto fail_idx;
  {
    int64_t actual_n = 0;
    int64_t actual_dim = 0;
    if (cuvsCagraIndexGetSize(index->idx, &actual_n) != CUVS_SUCCESS ||
        cuvsCagraIndexGetDims(index->idx, &actual_dim) != CUVS_SUCCESS ||
        actual_n != n || actual_dim != dim) {
      fprintf(stderr, "[seekdb_cuvs] snapshot shape mismatch: %ld x %ld, expected %ld x %ld\n",
              (long)actual_n, (long)actual_dim, n, dim);
      goto fail_idx;
    }
  }
  index->n = n;
  index->dim = dim;
  return index;
fail_idx:
  cuvsCagraIndexDestroy(index->idx);
fail_ip:
  cuvsCagraIndexParamsDestroy(index->ip);
fail_res:
  cuvsResourcesDestroy(index->res);
fail:
  fprintf(stderr, "[seekdb_cuvs] deserialize failed: %s\n", cuvsGetLastErrorText());
  pthread_mutex_destroy(&index->mutex);
  free(index);
  return NULL;
}

/* All GPU objects, streams and their teardown live on one joinable large-stack
 * worker per index. The existing outer adaptor's large-stack boundary remains.
 * The control lock is separate from sk_cuvs_index::mutex: core calls never run
 * while this lock is held. A single slot bounds queued work and caller buffers
 * remain borrowed only until the synchronous submit returns. */
typedef struct sk_worker sk_worker;
typedef int (*sk_job_fn)(sk_worker *, void *);
struct sk_worker {
  pthread_t thread;
  pthread_mutex_t control;
  pthread_cond_t changed;
  void *core;
  sk_job_fn job;
  void *argument;
  unsigned active;
  int device, startup_done, startup_rc, stopping, busy, ready, done, result;
};

static void *sk_worker_main(void *opaque)
{
  sk_worker *w = (sk_worker *)opaque;
  int rc = cudaSetDevice(w->device) == cudaSuccess ? 0 : 1;
  pthread_mutex_lock(&w->control);
  w->startup_rc = rc;
  w->startup_done = 1;
  pthread_cond_broadcast(&w->changed);
  if (rc) { pthread_mutex_unlock(&w->control); return NULL; }
  for (;;) {
    while (!w->ready && !w->stopping) pthread_cond_wait(&w->changed, &w->control);
    if (!w->ready && w->stopping) break;
    sk_job_fn job = w->job;
    void *argument = w->argument;
    w->ready = 0;
    pthread_mutex_unlock(&w->control);
    rc = job(w, argument);
    /* Even an unsuccessful API may have enqueued work. Never release borrowed
     * host pointers, change jobs or tear resources down before the stream ends. */
    if (w->core && cuvsStreamSync(((sk_cuvs_index *)w->core)->res) != CUVS_SUCCESS) {
      fprintf(stderr, "[seekdb_cuvs] worker stream sync failed: %s\n", cuvsGetLastErrorText());
      if (!rc) rc = 18;
    }
    pthread_mutex_lock(&w->control);
    w->result = rc;
    w->done = 1;
    pthread_cond_broadcast(&w->changed);
  }
  pthread_mutex_unlock(&w->control);
  if (w->core) sk_core_free(w->core);
  return NULL;
}

static sk_worker *sk_worker_create(void)
{
  sk_worker *w = (sk_worker *)calloc(1, sizeof(*w));
  pthread_attr_t attr;
  int rc;
  if (!w) return NULL;
  if (cudaGetDevice(&w->device) != cudaSuccess) { free(w); return NULL; }
  if (pthread_mutex_init(&w->control, NULL)) { free(w); return NULL; }
  if (pthread_cond_init(&w->changed, NULL)) { pthread_mutex_destroy(&w->control); free(w); return NULL; }
  if (pthread_attr_init(&attr)) goto fail;
  rc = pthread_attr_setstacksize(&attr, 32UL * 1024 * 1024);
  if (!rc) rc = pthread_create(&w->thread, &attr, sk_worker_main, w);
  pthread_attr_destroy(&attr);
  if (rc) goto fail;
  pthread_mutex_lock(&w->control);
  while (!w->startup_done) pthread_cond_wait(&w->changed, &w->control);
  rc = w->startup_rc;
  pthread_mutex_unlock(&w->control);
  if (!rc) return w;
  pthread_join(w->thread, NULL);
fail:
  pthread_cond_destroy(&w->changed);
  pthread_mutex_destroy(&w->control);
  free(w);
  return NULL;
}

static int sk_worker_submit(sk_worker *w, sk_job_fn job, void *argument)
{
  if (!w) return 1;
  pthread_mutex_lock(&w->control);
  ++w->active; /* Includes callers waiting for the one bounded slot. */
  while (w->busy && !w->stopping) pthread_cond_wait(&w->changed, &w->control);
  if (w->stopping) {
    --w->active;
    pthread_cond_broadcast(&w->changed);
    pthread_mutex_unlock(&w->control);
    return 1;
  }
  w->busy = 1;
  w->done = 0;
  w->argument = argument;
  w->job = job;
  w->ready = 1;
  pthread_cond_broadcast(&w->changed);
  while (!w->done) pthread_cond_wait(&w->changed, &w->control);
  int rc = w->result;
  w->argument = NULL;
  w->busy = 0;
  --w->active;
  pthread_cond_broadcast(&w->changed);
  pthread_mutex_unlock(&w->control);
  return rc;
}

void seekdb_cuvs_free(void *handle)
{
  sk_worker *w = (sk_worker *)handle;
  if (!w) return;
  /* As for the original C handle API, ownership must prevent new calls after
   * free begins. Already registered executing/queued callers are drained. */
  pthread_mutex_lock(&w->control);
  w->stopping = 1;
  pthread_cond_broadcast(&w->changed);
  while (w->active) pthread_cond_wait(&w->changed, &w->control);
  pthread_mutex_unlock(&w->control);
  pthread_join(w->thread, NULL);
  pthread_cond_destroy(&w->changed);
  pthread_mutex_destroy(&w->control);
  free(w);
}

typedef struct { const float *base; const char *filename; long n, dim; } sk_open_args;
static int sk_open_job(sk_worker *w, void *opaque)
{
  sk_open_args *a = (sk_open_args *)opaque;
  w->core = a->filename ? sk_core_deserialize(a->filename, a->n, a->dim)
                        : sk_core_build(a->base, a->n, a->dim);
  return w->core ? 0 : 1;
}

static void *sk_open(const float *base, const char *filename, long n, long dim)
{
  size_t bytes;
  if ((!base && !filename) || n > UINT_MAX || !sk_bytes(n, dim, sizeof(float), &bytes)) return NULL;
  sk_worker *w = sk_worker_create();
  if (!w) return NULL;
  sk_open_args a = {base, filename, n, dim};
  if (sk_worker_submit(w, sk_open_job, &a)) { seekdb_cuvs_free(w); return NULL; }
  return w;
}

void *seekdb_cuvs_build(const float *base, long n, long dim)
{ return sk_open(base, NULL, n, dim); }

void *seekdb_cuvs_deserialize(const char *filename, long n, long dim)
{ return sk_open(NULL, filename, n, dim); }

typedef struct { const float *query; long nq, topk; unsigned *ids; float *dist; } sk_search_args;
static int sk_search_job(sk_worker *w, void *opaque)
{
  sk_search_args *a = (sk_search_args *)opaque;
  return sk_core_search(w->core, a->query, a->nq, a->topk, a->ids, a->dist);
}

int seekdb_cuvs_search(void *handle, const float *query, long nq, long topk,
                       unsigned *ids, float *dist)
{
  sk_search_args a = {query, nq, topk, ids, dist};
  return sk_worker_submit((sk_worker *)handle, sk_search_job, &a);
}

static int sk_serialize_job(sk_worker *w, void *filename)
{ return sk_core_serialize(w->core, (const char *)filename); }

int seekdb_cuvs_serialize(void *handle, const char *filename)
{ return sk_worker_submit((sk_worker *)handle, sk_serialize_job, (void *)filename); }

int seekdb_cuvs_cagra_knn(const float* base, long n, long dim,
                          const float* query, long nq, long topk,
                          unsigned int* out_ids)
{
  void* h = seekdb_cuvs_build(base, n, dim);
  if (!h) return 10;
  int rc = seekdb_cuvs_search(h, query, nq, topk, out_ids, NULL);
  seekdb_cuvs_free(h);
  return rc;
}
