#include "seekdb_cuvs_bridge.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

static int check_roundtrip(const std::vector<float> &vectors, long count, long dimension)
{
  void *index = seekdb_cuvs_build(vectors.data(), count, dimension);
  if (!index) { std::fprintf(stderr, "build failed\n"); return 3; }
  long degree = seekdb_cuvs_get_graph(index, nullptr, count);
  if (degree <= 0) { std::fprintf(stderr, "get_graph (degree) failed: %ld\n", degree); return 4; }
  std::vector<unsigned int> graph(static_cast<size_t>(count) * degree);
  if (seekdb_cuvs_get_graph(index, graph.data(), count) != degree) {
    std::fprintf(stderr, "get_graph (copy) failed\n");
    return 5;
  }
  // graph may be queried for the top-1 of the first vector: it must be itself
  unsigned int ids_ref[10]; float dist_ref[10];
  if (seekdb_cuvs_search(index, vectors.data(), 1, 10, ids_ref, dist_ref) != 0) {
    std::fprintf(stderr, "search on built index failed\n");
    return 6;
  }
  seekdb_cuvs_free(index);

  void *loaded = seekdb_cuvs_load(graph.data(), count, degree, vectors.data(), dimension);
  if (!loaded) { std::fprintf(stderr, "load failed\n"); return 7; }
  unsigned int ids[10]; float dist[10];
  int result = seekdb_cuvs_search(loaded, vectors.data(), 1, 10, ids, dist);
  seekdb_cuvs_free(loaded);
  if (result != 0) { std::fprintf(stderr, "search on loaded index failed\n"); return 8; }
  for (int i = 0; i < 10; ++i) {
    if (ids[i] != ids_ref[i] || std::fabs(dist[i] - dist_ref[i]) > 0.001f) {
      std::fprintf(stderr, "roundtrip mismatch at %d: %u/%u %g/%g\n",
                   i, ids[i], ids_ref[i], dist[i], dist_ref[i]);
      return 9;
    }
  }
  // shape mismatch must be rejected
  void *invalid = seekdb_cuvs_load(graph.data(), count - 1, degree, vectors.data(), dimension);
  if (invalid) { seekdb_cuvs_free(invalid); return 10; }
  if (ids[0] != 0 || std::fabs(dist[0]) > 0.0001f) return 11;
  std::printf("roundtrip: n=%ld dim=%ld degree=%ld top1=%u distance=%g\n",
              count, dimension, degree, ids[0], dist[0]);
  return 0;
}

int main()
{
  const long count = 10000;
  const long dimension = 32;
  std::vector<float> vectors(count * dimension);
  for (long row = 0; row < count; ++row) {
    for (long column = 0; column < dimension; ++column) {
      vectors[row * dimension + column] = std::sin(float(row * 31 + column * 17));
    }
  }
  int rc = check_roundtrip(vectors, count, dimension);
  if (rc != 0) return rc;
  std::vector<float> wide(count * 64);
  for (long row = 0; row < count; ++row) {
    for (long column = 0; column < 64; ++column) {
      wide[row * 64 + column] = std::cos(float(row * 7 + column * 3));
    }
  }
  return check_roundtrip(wide, count, 64);
}
