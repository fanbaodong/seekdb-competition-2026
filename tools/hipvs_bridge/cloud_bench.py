#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Radeon Cloud one-shot A/B GPU benchmark for the seekdb hipVS bridge.

Upload this single file (or paste it into a Jupyter cell) on a seekdb-hipvs
Radeon Cloud instance and run it. It will:
  1. sanity-check the GPU / ROCm / hipVS toolchain
  2. download the bridge sources (pinned commit) from GitHub
  3. build libseekdb_cuvs_bridge.so + test_persistence + bench_cagra
  4. run the persistence round-trip correctness test on the GPU
  5. generate synthetic 256d / 250k data (gaussian + clustered) and exact
     top-100 ground truth for 1000 queries
  6. run bench_cagra per dataset, twice: OB_CUVS_CFG_PRESET=baseline
     (exact V5 params) vs tuned defaults
  7. print a compact JSON summary for pasting back

Budget: ~10-20 minutes end to end on a gfx1100-class GPU.
"""
import json
import os
import shutil
import subprocess
import sys
import time
import urllib.request

BRANCH = "amd-v5-optimize"
COMMIT = "297437439fa433c6e6a4ee0b10a4f24810dbf880"
HIPVS_FROZEN = "f96a663d0a4441d6f5344bd3ef552b71ae7b370b"
RAW = ("https://raw.githubusercontent.com/fanbaodong/seekdb-competition-2026/"
       "%s/tools/hipvs_bridge") % COMMIT
FILES = ["build.sh", "seekdb_cuvs_bridge.c", "seekdb_cuvs_bridge.h",
         "bench_cagra.cpp", "test_persistence.cpp"]

WORK = os.path.expanduser("/workspace/cuvs-bench")
BRIDGE_SRC = os.path.join(WORK, "bridge-src")
BRIDGE_OUT = os.path.join(WORK, "bridge")
DATA = os.path.join(WORK, "data")
RESULTS = {}

N, DIM = 250000, 256
GT_NQ = 1000   # exact top-100 computed for the first 1000 queries
GT_K = 100


def sh(cmd, check=True, env=None):
    print("+ " + cmd, flush=True)
    return subprocess.run(cmd, shell=True, check=check, env=env)


def download(url, dest):
    print("downloading %s" % url, flush=True)
    urllib.request.urlretrieve(url, dest)


def step(msg):
    print("\n================ %s ================" % msg, flush=True)


def env_check():
    step("1/7 environment check")
    sh("rocm-smi --showproductname 2>/dev/null | head -6 || true", check=False)
    hipvs_lib = "/opt/hipvs/lib/libcuvs_c.so"
    hipvs_hdr = "/opt/hipvs/include/cuvs/core/c_api.h"
    print("libcuvs_c.so:", os.path.isfile(hipvs_lib), flush=True)
    print("cuvs headers:", os.path.isfile(hipvs_hdr), flush=True)
    hipcc = shutil.which("hipcc") or "/opt/rocm/bin/hipcc"
    print("hipcc:", hipcc, "executable:", os.access(hipcc, os.X_OK), flush=True)
    if not os.path.isfile(hipvs_lib):
        sys.exit("FATAL: libcuvs_c.so missing under /opt/hipvs/lib")
    return hipcc, hipvs_hdr


def ensure_headers(hipvs_hdr):
    """The template may ship only the prebuilt lib, not the headers. Fall back
    to a shallow clone of the FROZEN hipVS commit (headers must match the
    prebuilt lib, which was built from exactly this commit)."""
    if os.path.isfile(hipvs_hdr):
        return "/opt/hipvs"
    root = os.path.join(WORK, "hipvs-frozen")
    if not os.path.isdir(root):
        sh("git clone --depth 1 --branch %s "
           "https://github.com/AMD-Ecosystem/hipVS %s" % (HIPVS_FROZEN, root))
    return root


def fetch_and_build(hipcc, hipvs_root):
    step("2/7 fetch pinned bridge sources")
    os.makedirs(BRIDGE_SRC, exist_ok=True)
    for name in FILES:
        download("%s/%s" % (RAW, name), os.path.join(BRIDGE_SRC, name))
    os.chmod(os.path.join(BRIDGE_SRC, "build.sh"), 0o755)

    step("3/7 build bridge + bench tools")
    os.makedirs(BRIDGE_OUT, exist_ok=True)
    env = dict(os.environ)
    env["HIPCC"] = hipcc
    env["HIPVS_ROOT"] = hipvs_root
    sh("%s/build.sh %s/libseekdb_cuvs_bridge.so --bench"
       % (BRIDGE_SRC, BRIDGE_OUT), env=env)


def ld_env():
    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = "%s:/opt/hipvs/lib:/opt/rocm/lib" % BRIDGE_OUT
    return env


def run_persistence():
    step("4/7 persistence round-trip (GPU correctness)")
    env = ld_env()
    r = sh("%s/test_persistence" % BRIDGE_OUT, check=False, env=env)
    RESULTS["persistence"] = "PASS" if r.returncode == 0 else "FAIL(rc=%d)" % r.returncode


def make_data():
    step("5/7 synthetic data + exact ground truth")
    import numpy as np
    os.makedirs(DATA, exist_ok=True)
    rng = np.random.default_rng(1234)

    def write_fvecs(path, arr):
        with open(path, "wb") as f:
            for row in arr:
                f.write(np.int32(len(row)).tobytes())
                f.write(row.tobytes())

    def write_ivecs(path, arr):
        with open(path, "wb") as f:
            for row in arr:
                f.write(np.int32(len(row)).tobytes())
                f.write(row.tobytes())

    # (a) gaussian uniform: hardest recall case (distance concentration)
    gauss = rng.standard_normal((N, DIM)).astype(np.float32)
    # (b) clustered, SIFT-like: 1000 centers + gaussian spread
    ncent = 1000
    centers = rng.uniform(-1.0, 1.0, size=(ncent, DIM)).astype(np.float32)
    clust = np.empty((N, DIM), dtype=np.float32)
    per = N // ncent
    for i in range(ncent):
        lo = i * per
        hi = N if i == ncent - 1 else (i + 1) * per
        clust[lo:hi] = (centers[i]
                        + 0.35 * rng.standard_normal((hi - lo, DIM))).astype(np.float32)
    query = rng.standard_normal((GT_NQ, DIM)).astype(np.float32)

    write_fvecs(os.path.join(DATA, "gauss_base.fvecs"), gauss)
    write_fvecs(os.path.join(DATA, "clust_base.fvecs"), clust)
    write_fvecs(os.path.join(DATA, "query.fvecs"), query)

    # exact top-100 for the first GT_NQ queries, float64 precision, in blocks
    print("computing ground truth (%d queries x top-%d)..." % (GT_NQ, GT_K), flush=True)
    bs = 64
    for name, arr in (("gauss", gauss), ("clust", clust)):
        gt = np.empty((GT_NQ, GT_K), dtype=np.int32)
        arr64 = arr.astype(np.float64)
        b2 = np.einsum("ij,ij->i", arr64, arr64)
        for s in range(0, GT_NQ, bs):
            qb = query[s:s + bs].astype(np.float64)
            d2 = b2[None, :] - 2.0 * (qb @ arr64.T)
            d2 += np.einsum("ij,ij->i", qb, qb)[:, None]
            idx = np.argpartition(d2, GT_K - 1, axis=1)[:, :GT_K]
            gt[s:s + bs] = idx.astype(np.int32)
            if s % 256 == 0:
                print("  gt %s %d/%d" % (name, s, GT_NQ), flush=True)
        write_ivecs(os.path.join(DATA, name + "_gt.ivecs"), gt)
        print("  gt %s done" % name, flush=True)
    del gauss, clust, query


def run_bench(tag, preset, base_name):
    env = ld_env()
    if preset:
        env["OB_CUVS_CFG_PRESET"] = "baseline"
    cmd = ("%s/bench_cagra --base %s/%s_base.fvecs --query %s/query.fvecs "
           "--gt %s/%s_gt.ivecs --bench-nq %d"
           % (BRIDGE_OUT, DATA, base_name, DATA, DATA, base_name, GT_NQ))
    print("\n----- %s (%s) -----" % (tag, base_name), flush=True)
    r = subprocess.run(cmd, shell=True, env=env, capture_output=True, text=True)
    sys.stdout.write(r.stdout)
    if r.returncode != 0:
        sys.stdout.write(r.stderr)
        RESULTS[tag + "_" + base_name] = "FAIL"
        return None
    out = {}
    for line in r.stdout.splitlines():
        line = line.strip()
        if line.startswith("== build:"):
            out["build_s"] = float(line.split(":")[1].rstrip("s").strip())
        if line.startswith("== graph degree:"):
            out["degree"] = int(line.split(":")[1])
        if line.startswith("== batch search:"):
            out["batch_qps"] = float(line.split()[-4])
        if line.startswith("== single-query loop:"):
            out["single_qps"] = float(line.split()[-4])
        if "recall@10:" in line:
            out["recall10"] = float(line.split("recall@10:")[1].split()[0])
            out["recall100"] = float(line.split("recall@100:")[1].split()[0])
    RESULTS[tag + "_" + base_name] = out
    return out


def main():
    t0 = time.time()
    hipcc, hipvs_hdr = env_check()
    hipvs_root = ensure_headers(hipvs_hdr)
    fetch_and_build(hipcc, hipvs_root)
    run_persistence()
    make_data()
    step("6/7 A/B benches (baseline preset vs tuned defaults)")
    for base_name in ("gauss", "clust"):
        run_bench("baseline", True, base_name)
        run_bench("tuned", False, base_name)
    step("7/7 summary")
    RESULTS["elapsed_s"] = round(time.time() - t0, 1)
    RESULTS["commit"] = COMMIT
    print(json.dumps(RESULTS, indent=2, default=str), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
