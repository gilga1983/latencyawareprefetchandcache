#!/usr/bin/env python3
import argparse
import json
import subprocess
import tempfile
import time
import urllib.request
from pathlib import Path

import zstandard as zstd

BASE = "https://cache-datasets.s3.amazonaws.com/cache_dataset_oracleGeneral/"
TRACES = {
    "msr_hm_0": ("MSR", "2007_msr/msr_hm_0.oracleGeneral.zst"),
    "msr_prn_0": ("MSR", "2007_msr/msr_prn_0.oracleGeneral.zst"),
    "msr_prn_1": ("MSR", "2007_msr/msr_prn_1.oracleGeneral.zst"),
    "msr_proj_0": ("MSR", "2007_msr/msr_proj_0.oracleGeneral.zst"),
    "msr_proj_1": ("MSR", "2007_msr/msr_proj_1.oracleGeneral.zst"),
    "msr_proj_2": ("MSR", "2007_msr/msr_proj_2.oracleGeneral.zst"),
    "msr_proj_4": ("MSR", "2007_msr/msr_proj_4.oracleGeneral.zst"),
    "msr_prxy_0": ("MSR", "2007_msr/msr_prxy_0.oracleGeneral.zst"),
    "msr_prxy_1": ("MSR", "2007_msr/msr_prxy_1.oracleGeneral.zst"),
    "msr_src1_0": ("MSR", "2007_msr/msr_src1_0.oracleGeneral.zst"),
    "msr_src1_1": ("MSR", "2007_msr/msr_src1_1.oracleGeneral.zst"),
    "msr_usr_1": ("MSR", "2007_msr/msr_usr_1.oracleGeneral.zst"),
    "msr_usr_2": ("MSR", "2007_msr/msr_usr_2.oracleGeneral.zst"),
    "msr_web_2": ("MSR", "2007_msr/msr_web_2.oracleGeneral.zst"),
    "w90": ("CloudPhysics", "2015_cloudphysics/w90.oracleGeneral.bin.zst"),
    "w91": ("CloudPhysics", "2015_cloudphysics/w91.oracleGeneral.bin.zst"),
    "w92": ("CloudPhysics", "2015_cloudphysics/w92.oracleGeneral.bin.zst"),
    "w93": ("CloudPhysics", "2015_cloudphysics/w93.oracleGeneral.bin.zst"),
    "w94": ("CloudPhysics", "2015_cloudphysics/w94.oracleGeneral.bin.zst"),
    "w95": ("CloudPhysics", "2015_cloudphysics/w95.oracleGeneral.bin.zst"),
}

# Frozen demand-only byte-LRU results from the submitted FAST evaluation.
PAPER = {
    "msr_hm_0": (3993316, 0.634662270654),
    "msr_prn_0": (5585886, 0.702595255256),
    "msr_prn_1": (11233411, 0.290607011530),
    "msr_proj_0": (4224524, 0.568453392619),
    "msr_proj_1": (23639742, 0.078316506162),
    "msr_proj_2": (29266482, 0.012189268256),
    "msr_proj_4": (6465639, 0.057061490751),
    "msr_prxy_0": (12518968, 0.956884305479),
    "msr_prxy_1": (168638964, 0.990481814155),
    "msr_src1_0": (37415613, 0.015413886176),
    "msr_src1_1": (45746222, 0.030515481694),
    "msr_usr_1": (45283980, 0.133793164823),
    "msr_usr_2": (10570046, 0.143539961889),
    "msr_web_2": (5175368, 0.003808618054),
    "w90": (4493515, 0.232112277360),
    "w91": (4316605, 0.436925778476),
    "w92": (4284658, 0.169992797558),
    "w93": (3351357, 0.134939369336),
    "w94": (4118188, 0.037890450849),
    "w95": (3937240, 0.652779866099),
}

REC_BYTES = 24


def materialize(name: str, limit: int, path: Path):
    _, rel = TRACES[name]
    url = BASE + rel
    max_bytes = None if limit <= 0 else limit * REC_BYTES
    written = 0
    with urllib.request.urlopen(url, timeout=120) as raw, path.open("wb") as out:
        with zstd.ZstdDecompressor().stream_reader(raw) as reader:
            while max_bytes is None or written < max_bytes:
                want = 1024 * 1024
                if max_bytes is not None:
                    want = min(want, max_bytes - written)
                chunk = reader.read(want)
                if not chunk:
                    break
                out.write(chunk)
                written += len(chunk)
    if written % REC_BYTES:
        raise RuntimeError(f"{name}: decompressed stream ended mid-record ({written} bytes)")
    records = written // REC_BYTES
    if limit > 0 and records != limit:
        raise RuntimeError(f"{name}: requested {limit} records, got {records}")
    return records, url


def run_one(binary: Path, name: str, limit: int, capacity: int, latencies: str, time_model: str, work: Path):
    trace = work / f"{name}.oracle"
    t0 = time.monotonic()
    records, url = materialize(name, limit, trace)
    download_s = time.monotonic() - t0
    cmd = [
        str(binary), "--trace", str(trace), "--name", name,
        "--capacity", str(capacity), "--latencies-us", latencies,
        "--time-model", time_model,
    ]
    if limit > 0:
        cmd += ["--limit", str(limit)]
    t1 = time.monotonic()
    cp = subprocess.run(cmd, text=True, capture_output=True, check=False)
    run_s = time.monotonic() - t1
    trace.unlink(missing_ok=True)
    if cp.returncode:
        raise RuntimeError(f"{name}: trace_bench failed\nSTDOUT:\n{cp.stdout[-4000:]}\nSTDERR:\n{cp.stderr[-4000:]}")
    lines = [x for x in cp.stdout.splitlines() if x.strip().startswith("{")]
    if not lines:
        raise RuntimeError(f"{name}: no JSON output")
    result = json.loads(lines[-1])
    result["dataset"] = TRACES[name][0]
    result["source_url"] = url
    result["download_seconds"] = download_s
    result["simulation_seconds"] = run_s

    zero = next(s for s in result["scenarios"] if s["latency_us"] == 0)
    if zero["resident_hits"] != result["instant_lru_hits"]:
        raise RuntimeError(f"{name}: zero-latency hits differ from immediate reference")
    if abs(zero["resident_hit_ratio"] - result["instant_lru_hit_ratio"]) > 1e-14:
        raise RuntimeError(f"{name}: zero-latency ratio differs from immediate reference")

    expected_requests, expected_hr = PAPER[name]
    result["paper_requests"] = expected_requests
    result["paper_lru_hit_ratio"] = expected_hr
    result["paper_lru_hit_ratio_delta"] = result["instant_lru_hit_ratio"] - expected_hr
    if limit <= 0:
        if result["requests"] != expected_requests:
            raise RuntimeError(f"{name}: full trace request count {result['requests']} != paper {expected_requests}")
        if abs(result["instant_lru_hit_ratio"] - expected_hr) > 5.1e-5:
            raise RuntimeError(
                f"{name}: LRU hit ratio {result['instant_lru_hit_ratio']:.12f} != paper {expected_hr:.12f}"
            )
    return result


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default="./trace_bench")
    ap.add_argument("--traces", nargs="+", choices=TRACES, default=list(TRACES))
    ap.add_argument("--limit", type=int, default=1000000,
                    help="requests per trace; 0 means full trace")
    ap.add_argument("--capacity", type=int, default=256 * 1024 * 1024)
    ap.add_argument("--latencies-us", default="0,1000,5000,10000,20000")
    ap.add_argument("--time-model", choices=("spread", "raw"), default="spread")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    binary = Path(args.binary).resolve()
    out = Path(args.out).resolve()
    out.parent.mkdir(parents=True, exist_ok=True)
    results = []
    with tempfile.TemporaryDirectory(prefix="fast-lru-") as td:
        work = Path(td)
        for name in args.traces:
            print(f"RUN {name}", flush=True)
            row = run_one(binary, name, args.limit, args.capacity, args.latencies_us, args.time_model, work)
            results.append(row)
            print(json.dumps({
                "trace": name,
                "requests": row["requests"],
                "instant_lru_hit_ratio": row["instant_lru_hit_ratio"],
                "paper_delta": row["paper_lru_hit_ratio_delta"],
                "latency_scenarios": [
                    {
                        "latency_us": s["latency_us"],
                        "resident_hit_ratio": s["resident_hit_ratio"],
                        "coalesced_ratio": s["demand_coalesced_ratio"],
                        "aat_us": s["average_access_time_us"],
                    } for s in row["scenarios"]
                ],
            }), flush=True)

    payload = {
        "capacity_bytes": args.capacity,
        "limit": args.limit,
        "latencies_us": args.latencies_us,
        "time_model": args.time_model,
        "results": results,
    }
    out.write_text(json.dumps(payload, indent=2) + "\n")
    print(f"WROTE {out}", flush=True)


if __name__ == "__main__":
    main()
