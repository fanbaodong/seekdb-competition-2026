/* seekdb <-> hipVS bridge: plain-C CAGRA kNN; all cuVS/ROCm deps live here. */
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
#include <string.h>

typedef struct {
  cuvsResources_t        res;
  cuvsCagraIndexParams_t ip;
  cuvsCagraIndex_t       idx;
  long n, dim;
  int  graph_degree;
} sk_cuvs_index;

/* Build/search knobs. The eval's query load is 4096 single-row SQL statements,
 * each dominated by CPU/SQL overhead (~7ms) while one GPU search costs tens of
 * microseconds. So the graph is built cheaper than the V5 baseline and recall
 * is recovered in the search phase (itopk/search_width), which is nearly free
 * in wall-clock terms. Dim-aware defaults: 256d data needs a stronger graph
 * than 128d to stay within the recall gates. Env vars override the defaults so
 * the tuning harness can sweep without recompiling; OB_CUVS_CFG_PRESET=baseline
 * restores the exact V5 baseline params for A/B comparison. */
typedef struct {
  size_t   graph_degree;
  size_t   intermediate_degree;
  size_t   niter;
  size_t   search_width;
  size_t   itopk_min;
  size_t   itopk_mult;
  uint32_t num_random_samplings;
} sk_cuvs_cfg;

static long sk_env_l(const char *name, long dflt)
{
  const char *v = getenv(name);
  if (!v || !*v) return dflt;
  char *end = NULL;
  long x = strtol(v, &end, 10);
  return (end && *end == '\0' && x > 0) ? x : dflt;
}

static sk_cuvs_cfg sk_cfg(long dim)
{
  sk_cuvs_cfg c;
  const char *preset = getenv("OB_CUVS_CFG_PRESET");
  if (preset != NULL && strcmp(preset, "baseline") == 0) {
    c.graph_degree = 128;
    c.intermediate_degree = 256;
    c.niter = 100;
    c.search_width = 4;
    c.itopk_min = 512;
    c.itopk_mult = 4;
    c.num_random_samplings = 1;
  } else if (dim > 128) {
    c.graph_degree = 96;
    c.intermediate_degree = 192;
    c.niter = 60;
    c.search_width = 10;
    c.itopk_min = 1024;
    c.itopk_mult = 8;
    c.num_random_samplings = 2;
  } else {
    c.graph_degree = 64;
    c.intermediate_degree = 128;
    c.niter = 35;
    c.search_width = 8;
    c.itopk_min = 1024;
    c.itopk_mult = 8;
    c.num_random_samplings = 2;
  }
  c.graph_degree         = (size_t)sk_env_l("OB_CUVS_GRAPH_DEGREE", (long)c.graph_degree);
  c.intermediate_degree  = (size_t)sk_env_l("OB_CUVS_INTERMEDIATE_DEGREE", (long)c.intermediate_degree);
  c.niter                = (size_t)sk_env_l("OB_CUVS_NNDESCENT_NITER", (long)c.niter);
  c.search_width         = (size_t)sk_env_l("OB_CUVS_SEARCH_WIDTH", (long)c.search_width);
  c.itopk_min            = (size_t)sk_env_l("OB_CUVS_ITOPK_MIN", (long)c.itopk_min);
  c.itopk_mult           = (size_t)sk_env_l("OB_CUVS_ITOPK_MULT", (long)c.itopk_mult);
  c.num_random_samplings = (uint32_t)sk_env_l("OB_CUVS_NUM_RANDOM_SAMPLINGS",
                                              (long)c.num_random_samplings);
  if (c.intermediate_degree < c.graph_degree) c.intermediate_degree = c.graph_degree;
  return c;
}

static void sk_make_tensor(DLManagedTensor *t, void *data, int device_type,
                           int code, int bits, const long *shape)
{
  memset(t, 0, sizeof(*t));
  t->dl_tensor.data = data;
  t->dl_tensor.device.device_type = device_type;
  t->dl_tensor.device.device_id = 0;
  t->dl_tensor.ndim = 2;
  t->dl_tensor.dtype.code = code;
  t->dl_tensor.dtype.bits = bits;
  t->dl_tensor.dtype.lanes = 1;
  t->dl_tensor.shape = (int64_t *)shape;
  t->dl_tensor.strides = NULL;
  t->dl_tensor.byte_offset = 0;
}

void* seekdb_cuvs_build(const float* base, long n, long dim)
{
  sk_cuvs_index* h = (sk_cuvs_index*)calloc(1, sizeof(*h));
  if (!h) return NULL;
  if (cuvsResourcesCreate(&h->res) != CUVS_SUCCESS) { free(h); return NULL; }

  DLManagedTensor dset;
  long ds[2] = {n, dim};
  sk_make_tensor(&dset, (void*)base, kDLCPU, kDLFloat, 32, ds);

  if (cuvsCagraIndexParamsCreate(&h->ip) != CUVS_SUCCESS) goto fail_res;
  {
    sk_cuvs_cfg cfg = sk_cfg(dim);
    h->ip->build_algo = NN_DESCENT;
    h->ip->graph_degree = cfg.graph_degree;
    h->ip->intermediate_graph_degree = cfg.intermediate_degree;
    h->ip->nn_descent_niter = cfg.niter;
    h->graph_degree = (int)cfg.graph_degree;
  }
  if (cuvsCagraIndexCreate(&h->idx)      != CUVS_SUCCESS) goto fail_ip;
  if (cuvsCagraBuild(h->res, h->ip, &dset, h->idx) != CUVS_SUCCESS) goto fail_idx;
  h->n = n; h->dim = dim;
  return h;

fail_idx: cuvsCagraIndexDestroy(h->idx);
fail_ip:  cuvsCagraIndexParamsDestroy(h->ip);
fail_res: cuvsResourcesDestroy(h->res); free(h);
  fprintf(stderr, "[seekdb_cuvs] build failed: %s\n", cuvsGetLastErrorText());
  return NULL;
}

int seekdb_cuvs_search(void* handle, const float* query, long nq, long topk,
                       unsigned int* out_ids, float* out_dist)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) return 1;
  int rc = 0;
  float* qd = 0; unsigned* nd = 0; float* dd = 0; cuvsCagraSearchParams_t sp = 0;
  if (cuvsRMMAlloc(h->res, (void**)&qd, sizeof(float)*nq*h->dim)  != CUVS_SUCCESS) { rc=10; goto done; }
  if (cuvsRMMAlloc(h->res, (void**)&nd, sizeof(unsigned)*nq*topk) != CUVS_SUCCESS) { rc=11; goto done; }
  if (cuvsRMMAlloc(h->res, (void**)&dd, sizeof(float)*nq*topk)    != CUVS_SUCCESS) { rc=12; goto done; }
  if (cudaMemcpy(qd, query, sizeof(float)*nq*h->dim, cudaMemcpyDefault) != cudaSuccess) { rc=13; goto done; }
  {
    DLManagedTensor qt, nt, dt;
    long qs[2]={nq,h->dim}, nsh[2]={nq,topk}, dsh[2]={nq,topk};
    sk_make_tensor(&qt, qd, kDLCUDA, kDLFloat, 32, qs);
    nt = qt; nt.dl_tensor.data = nd; nt.dl_tensor.dtype.code = kDLUInt; nt.dl_tensor.shape = nsh;
    dt = qt; dt.dl_tensor.data = dd; dt.dl_tensor.dtype.code = kDLFloat; dt.dl_tensor.shape = dsh;
    if (cuvsCagraSearchParamsCreate(&sp) != CUVS_SUCCESS) { rc=14; goto done; }
    {
      sk_cuvs_cfg cfg = sk_cfg(h->dim);
      size_t itopk = (size_t)topk * cfg.itopk_mult;
      if (itopk < cfg.itopk_min) itopk = cfg.itopk_min;
      if (sp->itopk_size < itopk) sp->itopk_size = itopk;
      sp->search_width = cfg.search_width;
      sp->num_random_samplings = cfg.num_random_samplings;
    }
    cuvsFilter f; f.type=NO_FILTER; f.addr=(uintptr_t)0;
    if (cuvsCagraSearch(h->res, sp, h->idx, &qt, &nt, &dt, f) != CUVS_SUCCESS) { rc=15; goto done; }
    if (cudaMemcpy(out_ids, nd, sizeof(unsigned)*nq*topk, cudaMemcpyDefault) != cudaSuccess) { rc=16; goto done; }
    if (out_dist) cudaMemcpy(out_dist, dd, sizeof(float)*nq*topk, cudaMemcpyDefault);
  }
done:
  if (sp) cuvsCagraSearchParamsDestroy(sp);
  if (dd) cuvsRMMFree(h->res, dd, sizeof(float)*nq*topk);
  if (nd) cuvsRMMFree(h->res, nd, sizeof(unsigned)*nq*topk);
  if (qd) cuvsRMMFree(h->res, qd, sizeof(float)*nq*h->dim);
  if (rc) fprintf(stderr, "[seekdb_cuvs] search rc=%d: %s\n", rc, cuvsGetLastErrorText());
  return rc;
}

void seekdb_cuvs_free(void* handle)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) return;
  cuvsCagraIndexDestroy(h->idx);
  cuvsCagraIndexParamsDestroy(h->ip);
  cuvsResourcesDestroy(h->res);
  free(h);
}

long seekdb_cuvs_get_graph(void* handle, uint32_t* host_graph, long n)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h || n != h->n) return -1;
  DLManagedTensor g;
  memset(&g, 0, sizeof(g));
  if (cuvsCagraIndexGetGraph(h->idx, &g) != CUVS_SUCCESS) return -1;
  long degree = -1;
  if (g.dl_tensor.data != NULL && g.dl_tensor.ndim == 2 &&
      g.dl_tensor.shape[0] == n && g.dl_tensor.shape[1] > 0 &&
      g.dl_tensor.dtype.code == kDLUInt && g.dl_tensor.dtype.bits == 32 &&
      (g.dl_tensor.strides == NULL ||
       (g.dl_tensor.strides[1] == 1 && g.dl_tensor.strides[0] == g.dl_tensor.shape[1]))) {
    degree = (long)g.dl_tensor.shape[1];
    h->graph_degree = (int)degree;
    if (host_graph != NULL &&
        cudaMemcpy(host_graph, g.dl_tensor.data,
                   sizeof(uint32_t) * (size_t)n * (size_t)degree,
                   cudaMemcpyDeviceToHost) != cudaSuccess) {
      degree = -1;
    }
  }
  if (g.deleter) g.deleter(&g);
  return degree;
}

void* seekdb_cuvs_load(const uint32_t* graph, long n, long degree,
                       const float* vecs, long dim)
{
  if (!graph || !vecs || n <= 0 || dim <= 0 || degree <= 0) return NULL;
  sk_cuvs_index* h = (sk_cuvs_index*)calloc(1, sizeof(*h));
  if (!h) return NULL;
  if (cuvsResourcesCreate(&h->res) != CUVS_SUCCESS) { free(h); return NULL; }
  if (cuvsCagraIndexParamsCreate(&h->ip) != CUVS_SUCCESS) goto fail_res;
  if (cuvsCagraIndexCreate(&h->idx) != CUVS_SUCCESS) goto fail_ip;
  {
    /* Host tensors: IndexFromArgs copies graph+dataset to the device and owns
     * the copies, so the caller may release its buffers right after. */
    DLManagedTensor gt, dt;
    long gs[2] = {n, degree}, ds[2] = {n, dim};
    sk_make_tensor(&gt, (void*)graph, kDLCPU, kDLUInt, 32, gs);
    sk_make_tensor(&dt, (void*)vecs, kDLCPU, kDLFloat, 32, ds);
    if (cuvsCagraIndexFromArgs(h->res, L2Expanded, &gt, &dt, h->idx) != CUVS_SUCCESS) {
      goto fail_idx;
    }
  }
  h->n = n; h->dim = dim; h->graph_degree = (int)degree;
  return h;

fail_idx: cuvsCagraIndexDestroy(h->idx);
fail_ip:  cuvsCagraIndexParamsDestroy(h->ip);
fail_res: cuvsResourcesDestroy(h->res);
  fprintf(stderr, "[seekdb_cuvs] load failed: %s\n", cuvsGetLastErrorText());
  free(h);
  return NULL;
}

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
