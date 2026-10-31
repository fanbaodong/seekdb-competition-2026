#ifndef SEEKDB_CUVS_BRIDGE_H
#define SEEKDB_CUVS_BRIDGE_H
#ifdef __cplusplus
extern "C" {
#endif
/* One-shot: build a CAGRA index over base (n x dim, row-major f32) on the GPU and
 * search queries (nq x dim), writing top-k uint32 neighbor ids to out_ids
 * (length nq*topk). Returns 0 on success, non-zero on error. */
int seekdb_cuvs_cagra_knn(const float* base, long n, long dim,
                          const float* query, long nq, long topk,
                          unsigned int* out_ids);

/* Build-once / search-many (used by seekdb's ob_vsag_adaptor GPU path).
 *  - seekdb_cuvs_build: builds a CAGRA index on the GPU from base (n x dim,
 *    row-major f32) and returns an opaque handle (NULL on error). cuVS copies
 *    the dataset to the device, so `base` may be freed after this returns.
 *  - seekdb_cuvs_search: runs top-k search for nq queries (row-major f32, dim
 *    cols), writing uint32 row offsets to out_ids[nq*topk] and (if non-NULL) L2
 *    distances to out_dist[nq*topk]. Returns 0 on success.
 *  - seekdb_cuvs_free: releases the handle.
 * All ROCm/cuVS state lives inside the .so; seekdb only ever sees these C symbols. */
void* seekdb_cuvs_build(const float* base, long n, long dim);
int   seekdb_cuvs_search(void* handle, const float* query, long nq, long topk,
                         unsigned int* out_ids, float* out_dist);
void  seekdb_cuvs_free(void* handle);

/* Graph snapshot (OBCUVS03): seekdb persists the pruned CAGRA graph (n x degree
 * uint32, row-major) next to the ids and fp32 vectors it already stores. The
 * dataset is NOT duplicated inside the graph blob. On load the index is
 * reconstructed with cuvsCagraIndexFromArgs, which copies graph+dataset to the
 * device, so the caller may free its buffers once seekdb_cuvs_load returns.
 *  - seekdb_cuvs_get_graph: returns the actual graph degree (cuVS may round the
 *    requested degree), or -1 on error. With host_graph != NULL also copies the
 *    graph device->host into host_graph[n*degree]. Call with NULL first to size
 *    the buffer.
 *  - seekdb_cuvs_load: builds a searchable index from a host graph + fp32
 *    vectors, returns an opaque handle (NULL on error). */
long  seekdb_cuvs_get_graph(void* handle, uint32_t* host_graph, long n);
void* seekdb_cuvs_load(const uint32_t* graph, long n, long degree,
                       const float* vecs, long dim);
#ifdef __cplusplus
}
#endif
#endif
