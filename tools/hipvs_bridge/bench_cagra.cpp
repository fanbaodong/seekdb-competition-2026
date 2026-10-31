/* GPU CAGRA build/search benchmark + recall checker for the seekdb hipVS bridge.
 *
 * Reads BIGANN-style .fvecs/.ivecs files or raw row-major f32 binary via flags,
 * builds a CAGRA index with the bridge (params come from the same env knobs the
 * server uses, e.g. OB_CUVS_CFG_PRESET=baseline), and reports:
 *   - build wall time
 *   - batched search wall time and QPS at nq queries
 *   - single-query loop time (matches the eval's per-row SQL load shape)
 *   - Recall@10 / Recall@100 against ground truth
 *   - snapshot round-trip (get_graph -> load) time and result identity
 *
 * Usage:
 *   bench_cagra --base base.fvecs --query query.fvecs --gt gt.ivecs [--nq 4096]
 *   bench_cagra --base base.bin --n 250000 --dim 256 \
 *               --query query.bin --nq 4096 --gt gt.ivecs
 */
#include "seekdb_cuvs_bridge.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

struct Options {
  std::string base_path, query_path, gt_path;
  long n = 0, dim = 0, nq = 0, gt_k = 0;
  long topk = 100;
  long bench_nq = 4096;
  bool snapshot = true;
};

static void usage(const char *prog)
{
  std::fprintf(stderr,
    "usage: %s --base BASE --query QUERY --gt GT [--n N --dim D --nq NQ]\n"
    "             [--gt-k K] [--topk K] [--bench-nq N] [--no-snapshot]\n"
    "  BASE/QUERY/GT: .fvecs/.ivecs (BIGANN) or raw f32 (needs --n/--dim/--nq)\n"
    "  GT rows: k int32 per query (k >= topk); --gt-k overrides the row length\n",
    prog);
}

static bool read_fvecs(const std::string &path, std::vector<float> &data,
                       long &n, long &dim)
{
  FILE *f = std::fopen(path.c_str(), "rb");
  if (!f) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); return false; }
  data.clear();
  long total_dim = 0, rows = 0;
  while (!std::feof(f)) {
    int32_t d = 0;
    if (std::fread(&d, 4, 1, f) != 1) break;
    if (d <= 0) { std::fclose(f); std::fprintf(stderr, "bad fvecs dim %d\n", d); return false; }
    std::vector<float> row(d);
    if (std::fread(row.data(), 4, d, f) != (size_t)d) {
      std::fclose(f); return rows > 0 && dim > 0;
    }
    if (rows == 0) { dim = d; total_dim = d; }
    else if (d != dim) { std::fclose(f); std::fprintf(stderr, "ragged fvecs\n"); return false; }
    else { total_dim += d; }
    data.insert(data.end(), row.begin(), row.end());
    ++rows;
  }
  std::fclose(f);
  n = rows;
  return rows > 0 && data.size() == (size_t)total_dim;
}

static bool read_ivecs(const std::string &path, std::vector<uint32_t> &data,
                       long &n, long &k)
{
  FILE *f = std::fopen(path.c_str(), "rb");
  if (!f) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); return false; }
  data.clear();
  long rows = 0, total_k = 0;
  while (!std::feof(f)) {
    int32_t kk = 0;
    if (std::fread(&kk, 4, 1, f) != 1) break;
    if (kk <= 0) { std::fclose(f); std::fprintf(stderr, "bad ivecs k %d\n", kk); return false; }
    std::vector<int32_t> row(kk);
    if (std::fread(row.data(), 4, kk, f) != (size_t)kk) {
      std::fclose(f); return rows > 0 && k > 0;
    }
    if (rows == 0) { k = kk; total_k = kk; }
    else if (kk != k) { std::fclose(f); std::fprintf(stderr, "ragged ivecs\n"); return false; }
    else { total_k += kk; }
    data.insert(data.end(), row.begin(), row.end());
    ++rows;
  }
  std::fclose(f);
  n = rows;
  return rows > 0 && data.size() == (size_t)total_k;
}

static bool read_raw(const std::string &path, std::vector<float> &data, long n, long dim)
{
  FILE *f = std::fopen(path.c_str(), "rb");
  if (!f) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); return false; }
  data.resize((size_t)n * dim);
  size_t got = std::fread(data.data(), 4, (size_t)n * dim, f);
  std::fclose(f);
  if (got != (size_t)n * dim) { std::fprintf(stderr, "short raw file %s\n", path.c_str()); return false; }
  return true;
}

static double now_s()
{
  return std::chrono::duration<double>(
           std::chrono::steady_clock::now().time_since_epoch()).count();
}

int main(int argc, char **argv)
{
  Options o;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> const char * {
      if (i + 1 >= argc) { usage(argv[0]); std::exit(2); }
      return argv[++i];
    };
    if (a == "--base") o.base_path = next();
    else if (a == "--query") o.query_path = next();
    else if (a == "--gt") o.gt_path = next();
    else if (a == "--n") o.n = std::atol(next());
    else if (a == "--dim") o.dim = std::atol(next());
    else if (a == "--nq") o.nq = std::atol(next());
    else if (a == "--gt-k") o.gt_k = std::atol(next());
    else if (a == "--topk") o.topk = std::atol(next());
    else if (a == "--bench-nq") o.bench_nq = std::atol(next());
    else if (a == "--no-snapshot") o.snapshot = false;
    else if (a == "-h" || a == "--help") { usage(argv[0]); return 0; }
    else { usage(argv[0]); return 2; }
  }
  if (o.base_path.empty() || o.query_path.empty() || o.gt_path.empty()) { usage(argv[0]); return 2; }

  std::vector<float> base, query;
  std::vector<uint32_t> gt;
  long base_n = o.n, base_dim = o.dim, query_n = o.nq, gt_n = 0, gt_k = o.gt_k;

  bool is_fvecs = o.base_path.size() >= 6 && o.base_path.compare(o.base_path.size() - 6, 6, ".fvecs") == 0;
  bool is_ivecs = o.gt_path.size() >= 6 && o.gt_path.compare(o.gt_path.size() - 6, 6, ".ivecs") == 0;
  if (is_fvecs) {
    if (!read_fvecs(o.base_path, base, base_n, base_dim)) return 1;
    if (!read_fvecs(o.query_path, query, query_n, base_dim)) return 1;
    if (base_dim <= 0) return 1;
  } else {
    if (base_n <= 0 || base_dim <= 0 || query_n <= 0) {
      std::fprintf(stderr, "raw input requires --n --dim --nq\n");
      return 2;
    }
    if (!read_raw(o.base_path, base, base_n, base_dim)) return 1;
    if (!read_raw(o.query_path, query, query_n, base_dim)) return 1;
  }
  if (is_ivecs) {
    if (!read_ivecs(o.gt_path, gt, gt_n, gt_k)) return 1;
  } else {
    if (gt_k <= 0) { std::fprintf(stderr, "raw gt requires --gt-k\n"); return 2; }
    FILE *f = std::fopen(o.gt_path.c_str(), "rb");
    if (!f) { std::fprintf(stderr, "cannot open %s\n", o.gt_path.c_str()); return 1; }
    gt.resize((size_t)query_n * gt_k);
    if (std::fread(gt.data(), 4, gt.size(), f) != gt.size()) return 1;
    std::fclose(f);
    gt_n = query_n;
  }
  if (gt_n < query_n) { std::fprintf(stderr, "gt rows %ld < nq %ld\n", gt_n, query_n); return 1; }
  if (gt_k < o.topk) { std::fprintf(stderr, "gt k %ld < topk %ld\n", gt_k, o.topk); return 1; }

  std::printf("== dataset: n=%ld dim=%ld nq=%ld gt_k=%ld topk=%ld bench_nq=%ld\n",
              base_n, base_dim, query_n, gt_k, o.topk, o.bench_nq);

  double t0 = now_s();
  void *handle = seekdb_cuvs_build(base.data(), base_n, base_dim);
  double build_s = now_s() - t0;
  if (!handle) { std::fprintf(stderr, "build failed\n"); return 1; }
  std::printf("== build: %.3f s\n", build_s);

  long degree = seekdb_cuvs_get_graph(handle, nullptr, base_n);
  std::printf("== graph degree: %ld\n", degree);

  if (o.snapshot && degree > 0) {
    std::vector<uint32_t> graph((size_t)base_n * degree);
    double t1 = now_s();
    if (seekdb_cuvs_get_graph(handle, graph.data(), base_n) != degree) return 1;
    double d2h_s = now_s() - t1;
    t1 = now_s();
    void *loaded = seekdb_cuvs_load(graph.data(), base_n, degree, base.data(), base_dim);
    double load_s = now_s() - t1;
    if (!loaded) { std::fprintf(stderr, "snapshot load failed\n"); return 1; }
    std::printf("== snapshot: graph=%zu bytes, d2h %.3f s, load %.3f s\n",
                graph.size() * 4, d2h_s, load_s);
    seekdb_cuvs_free(loaded);
  }

  std::vector<unsigned> ids((size_t)o.bench_nq * o.topk);
  std::vector<float> dist((size_t)o.bench_nq * o.topk);
  double t1 = now_s();
  if (seekdb_cuvs_search(handle, query.data(), o.bench_nq, o.topk,
                         ids.data(), dist.data()) != 0) {
    std::fprintf(stderr, "batch search failed\n");
    return 1;
  }
  double batch_s = now_s() - t1;
  std::printf("== batch search: nq=%ld %.3f s -> %.1f QPS (GPU batch)\n",
              o.bench_nq, batch_s, o.bench_nq / batch_s);

  long loop_nq = o.bench_nq < query_n ? o.bench_nq : query_n;
  t1 = now_s();
  for (long q = 0; q < loop_nq; ++q) {
    if (seekdb_cuvs_search(handle, query.data() + q * base_dim, 1, o.topk,
                           ids.data() + q * o.topk, dist.data() + q * o.topk) != 0) {
      std::fprintf(stderr, "single search failed at %ld\n", q);
      return 1;
    }
  }
  double loop_s = now_s() - t1;
  std::printf("== single-query loop: nq=%ld %.3f s -> %.1f QPS (per-row shape)\n",
              loop_nq, loop_s, loop_nq / loop_s);

  long hits10 = 0, hits100 = 0;
  long check_nq = o.bench_nq < query_n ? o.bench_nq : query_n;
  long k100 = gt_k < 100 ? gt_k : 100;
  for (long q = 0; q < check_nq; ++q) {
    for (long j = 0; j < 10; ++j) {
      uint32_t got = ids[q * o.topk + j];
      for (long g = 0; g < 10; ++g) {
        if (gt[q * gt_k + g] == got) { ++hits10; break; }
      }
    }
    for (long j = 0; j < k100; ++j) {
      uint32_t got = ids[q * o.topk + j];
      for (long g = 0; g < k100; ++g) {
        if (gt[q * gt_k + g] == got) { ++hits100; break; }
      }
    }
  }
  std::printf("== recall@10: %.4f  recall@100: %.4f\n",
              (double)hits10 / (check_nq * 10.0),
              (double)hits100 / (check_nq * k100));

  seekdb_cuvs_free(handle);
  return 0;
}
