/* TEST_ONLY host shim. No GPU work, quality or throughput claim is possible. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
typedef struct { int device_type, device_id; } DLDevice;
typedef struct { uint8_t code, bits; uint16_t lanes; } DLDataType;
typedef struct { void *data; DLDevice device; int ndim; DLDataType dtype; int64_t *shape; int64_t *strides; uint64_t byte_offset; } DLTensor;
typedef struct { DLTensor dl_tensor; void *manager_ctx; void *deleter; } DLManagedTensor;
enum { kDLCPU=1, kDLCUDA=2, kDLFloat=2, kDLUInt=1, CUVS_SUCCESS=0, NN_DESCENT=1, NO_FILTER=0, MULTI_CTA=1, AUTO=3, cudaSuccess=0, cudaMemcpyDefault=4 };
typedef int cuvsError_t;
typedef void *cuvsResources_t;
typedef struct { int build_algo, graph_degree, intermediate_graph_degree, nn_descent_niter; } *cuvsCagraIndexParams_t;
typedef struct { size_t itopk_size, search_width; int algo; } *cuvsCagraSearchParams_t;
typedef struct { int64_t n, dim; } *cuvsCagraIndex_t;
typedef struct { int type; uintptr_t addr; } cuvsFilter;
int cuvsResourcesCreate(cuvsResources_t *); int cuvsResourcesDestroy(cuvsResources_t);
int cuvsStreamSync(cuvsResources_t);
int cudaGetDevice(int *); int cudaSetDevice(int);
int cuvsCagraIndexParamsCreate(cuvsCagraIndexParams_t *); int cuvsCagraIndexParamsDestroy(cuvsCagraIndexParams_t);
int cuvsCagraIndexCreate(cuvsCagraIndex_t *); int cuvsCagraIndexDestroy(cuvsCagraIndex_t);
int cuvsCagraBuild(cuvsResources_t,cuvsCagraIndexParams_t,DLManagedTensor *,cuvsCagraIndex_t);
int cuvsRMMAlloc(cuvsResources_t,void **,size_t); int cuvsRMMFree(cuvsResources_t,void *,size_t);
int cuvsCagraSearchParamsCreate(cuvsCagraSearchParams_t *); int cuvsCagraSearchParamsDestroy(cuvsCagraSearchParams_t);
int cuvsCagraSearch(cuvsResources_t,cuvsCagraSearchParams_t,cuvsCagraIndex_t,DLManagedTensor *,DLManagedTensor *,DLManagedTensor *,cuvsFilter);
int cudaMemcpy(void *,const void *,size_t,int);
int cuvsCagraSerialize(cuvsResources_t,const char *,cuvsCagraIndex_t,bool);
int cuvsCagraDeserialize(cuvsResources_t,const char *,cuvsCagraIndex_t);
int cuvsCagraIndexGetSize(cuvsCagraIndex_t,int64_t *); int cuvsCagraIndexGetDims(cuvsCagraIndex_t,int64_t *);
const char *cuvsGetLastErrorText(void);
