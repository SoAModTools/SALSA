"""Opt-in GameCube US whole-program IR survey. No gtest or GUI.

Run from SALSA. Location configuration may be read from the private corpus
repository; neither test code nor reports are written there.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
import tempfile
import statistics
import subprocess
from datetime import datetime, timezone
from pathlib import Path


def digest_file(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def write_new(path: Path, value: object) -> None:
    with path.open("x", encoding="utf-8") as stream:
        json.dump(value, stream, indent=2)
        stream.write("\n")


def summarize(reports: list[dict]) -> dict:
    if not reports or any(r.get("disposition") != "passed" or not r.get("complete") for r in reports):
        raise ValueError("Incomplete corpus run: no architecture comparison is accepted")
    for field in ("inventory_sha256", "host", "text_convention", "seed", "requested_edits",
                  "trace_sha256", "content_sha256", "query_digest", "skipped_edits", "eligible"):
        if any(r[field] != reports[0][field] for r in reports):
            raise ValueError(f"Corpus comparison disagrees on {field}")
    if len({r["build"]["binary_sha256"] for r in reports}) != 1:
        raise ValueError("Corpus reports come from different executables")
    groups = []
    for candidate in sorted({r["candidate"] for r in reports}):
        selected = [r for r in reports if r["candidate"] == candidate]
        group = {"candidate": candidate, "runs": len(selected)}
        for key in ("load_to_ready_ns", "native_file_read_ns", "native_parse_ns", "native_import_ns",
                    "normalization_ns", "source_hash_ns", "index_and_storage_ns", "checkpoint_capture_encode_ns",
                    "compress_ns", "reload_to_ready_ns"):
            values = [int(r[key]) for r in selected]
            group[key] = {"median": statistics.median(values), "max": max(values), "observations": values}
        for phase in ("baseline_memory", "after_import_memory", "ready_memory", "after_edits_memory",
                      "history_released_memory", "reloaded_memory"):
            group[phase] = {
                key: {"median": statistics.median(int(r[phase][key]) for r in selected),
                      "max": max(int(r[phase][key]) for r in selected)}
                for key in ("private_bytes", "working_set_bytes", "peak_working_set_bytes", "peak_pagefile_bytes")
            }
        edits = [int(s["edit_ns"]) for r in selected for s in r["samples"]]
        group["edits"] = {"samples": len(edits), "median_ns": statistics.median(edits) if edits else None,
                          "max_ns": max(edits) if edits else None}
        groups.append(group)
    return {"format": "jahorta.salsa.ir-corpus-comparison", "version": 1, "complete": True,
            "inventory_sha256": reports[0]["inventory_sha256"], "groups": groups,
            "scope": "Same seeded edits and parsed corpus across storage candidates; direct index coverage only. Process peaks are cumulative; reload coexists with original host. No runtime-playability or full-story claim."}


def main() -> None:
    repository = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--locations-manifest", type=Path,
                        default=repository.parent / "soamodtools-private-corpus-testing" / "corpus-locations.json")
    parser.add_argument("--corpus-root", type=Path, help="Explicit override for the gc_us dataset root")
    parser.add_argument("--candidates", nargs="+", choices=("dense", "paged", "section-local"),
                        default=["dense", "paged", "section-local"])
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--edits", type=int, default=1000)
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--memory-mb", type=int, default=4096)
    parser.add_argument("--timeout-ms", type=int, default=600000)
    parser.add_argument("--text-convention", choices=("shift-jis-byte-7f", "windows-1252-byte-7f", "shift-jis-8140"),
                        default="shift-jis-byte-7f")
    parser.add_argument("--prepare-only", action="store_true", help="Write the local inventory without loading scripts")
    args = parser.parse_args()
    if not 0 <= args.seed < 2**64 or not 0 <= args.edits <= 100000 or not 1 <= args.repetitions <= 10:
        parser.error("Seed, edit count or repetition count is outside the supported range")
    if len(set(args.candidates)) != len(args.candidates):
        parser.error("Candidate list must be unique")
    if not 32 <= args.memory_mb <= 65536 or not 1 <= args.timeout_ms <= 3600000:
        parser.error("Invalid worker limits")
    root = args.corpus_root
    if root is None:
        locations = json.loads(args.locations_manifest.read_text(encoding="utf-8-sig"))
        root = Path(locations["datasets"]["gc_us"])
    root = root.resolve(strict=True)
    if not root.is_dir():
        parser.error("Corpus root must be a directory")
    if any((parent / ".git").exists() for parent in (root, *root.parents)):
        parser.error("Raw corpus inputs must be outside repositories")
    paths = sorted((p for p in root.rglob("*") if p.is_file() and p.suffix.lower() == ".sct"),
                   key=lambda p: p.relative_to(root).as_posix().casefold())
    for path in paths:
        if not path.resolve().is_relative_to(root):
            parser.error("A script symlink escapes the corpus root")
    assets = [p.relative_to(root).as_posix() for p in paths]
    if not assets or len({a.casefold() for a in assets}) != len(assets):
        parser.error("The corpus is empty or contains duplicate asset keys")
    output = repository / "private" / "ir-corpus" / datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    if output.resolve().is_relative_to(root):
        parser.error("Output would be inside the read-only corpus")
    output.mkdir(parents=True, exist_ok=False)
    manifest = output / "manifest.json"
    write_new(manifest, {"format": "jahorta.salsa.ir-corpus-manifest", "version": 1, "dataset": "gc_us",
                         "root": str(root), "text_convention": args.text_convention,
                         "expected_scripts": len(assets), "assets": assets})
    print(f"Inventory: {len(assets)} scripts. Local reports: {output}", flush=True)
    if args.prepare_only:
        return
    executable = repository / "bin" / "x64" / "Release" / "SalsaIrEvaluation.exe"
    executable_hash = digest_file(executable)
    reports, order = [], []
    for repetition in range(args.repetitions):
        shift = repetition % len(args.candidates)
        candidates = args.candidates[shift:] + args.candidates[:shift]
        if repetition % 2:
            candidates.reverse()
        for candidate in candidates:
            name = f"{candidate}-{repetition}"
            report_path = output / f"{name}.json"
            with tempfile.TemporaryDirectory(prefix="salsa-ir-corpus-") as temporary, (output / f"{name}.log").open("x", encoding="utf-8") as log:
                environment = dict(os.environ, TEMP=temporary, TMP=temporary)
                result = subprocess.run([str(executable), "corpus", "--manifest", str(manifest),
                                         "--candidate", candidate, "--seed", str(args.seed),
                                         "--edits", str(args.edits), "--memory-mb", str(args.memory_mb),
                                         "--timeout-ms", str(args.timeout_ms), "--report", str(report_path)],
                                        stdout=log, stderr=subprocess.STDOUT, check=False, env=environment)
            order.append(report_path.name)
            write_new(output / f"run-{len(order)}.json", {"binary_sha256": executable_hash, "order": list(order)})
            if result.returncode:
                raise RuntimeError(f"Run failed ({result.returncode}); inspect {report_path} and its log. No comparison accepted.")
            report = json.loads(report_path.read_text(encoding="utf-8"))
            if report["build"]["configuration"] != "Release" or report["build"]["binary_sha256"] != executable_hash:
                raise RuntimeError("Executable changed or is not a Release build")
            reports.append(report)
            print(f"Completed {name}", flush=True)
    write_new(output / "comparison.json", summarize(reports))
    print("Complete corpus and edit results agree across runs.", flush=True)


if __name__ == "__main__":
    main()
