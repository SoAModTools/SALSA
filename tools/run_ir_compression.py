"""Capture a GC US IR checkpoint once, then compare codecs in isolated Release workers."""
import argparse,hashlib,json,os,statistics,subprocess,tempfile
from datetime import datetime,timezone
from pathlib import Path

CODECS=["lodepng","zstd-1","zstd-3","lz4","libdeflate-1","libdeflate-6"]
def save(path,value):
 with path.open("x",encoding="utf-8") as out:json.dump(value,out,indent=2);out.write("\n")
def main():
 parser=argparse.ArgumentParser(description=__doc__)
 parser.add_argument("--manifest",type=Path,required=True,help="Local GC US inventory from run_ir_corpus.py --prepare-only")
 parser.add_argument("--repetitions",type=int,default=3)
 parser.add_argument("--seed",type=int,default=1)
 parser.add_argument("--edits",type=int,default=1000)
 args=parser.parse_args()
 if not 1<=args.repetitions<=10 or not 0<=args.seed<2**64 or not 0<=args.edits<=100000:parser.error("Invalid repetition/seed/edit count")
 root=Path(__file__).resolve().parents[1];exe=root/"bin/x64/Release/SalsaIrEvaluation.exe"
 identity=hashlib.sha256(exe.read_bytes()).hexdigest()
 output=root/"private/ir-compression"/datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ");output.mkdir(parents=True)
 receipt=root/"bin/ir-codecs/receipt.json"
 if not receipt.exists():raise RuntimeError("Build optional codecs and rebuild SalsaIrEvaluation Release first")
 save(output/"dependencies.json",json.loads(receipt.read_text()))
 print("Reports:",output,flush=True)
 with tempfile.TemporaryDirectory(prefix="salsa-ir-compression-") as temporary:
  environment=dict(os.environ,TEMP=temporary,TMP=temporary);capture=Path(temporary)/"checkpoint"
  def run(name,arguments):
   path=output/(name+".json")
   with (output/(name+".log")).open("x",encoding="utf-8") as log:
    result=subprocess.run([str(exe),*arguments,"--report",str(path)],env=environment,stdout=log,stderr=subprocess.STDOUT)
   if result.returncode:raise RuntimeError(f"{name} failed ({result.returncode}); inspect {path}. No comparison accepted.")
   report=json.loads(path.read_text())
   if not report.get("complete") or report.get("disposition")!="passed" or report["build"]["binary_sha256"]!=identity or report["build"]["configuration"]!="Release":raise RuntimeError("Incomplete, mixed-build or non-Release result")
   return report
  captured=run("capture",["corpus","--manifest",str(args.manifest.resolve()),"--candidate","paged","--seed",str(args.seed),"--edits",str(args.edits),"--capture-directory",str(capture)])
  save(output/"capture-manifest.json",captured["capture"])
  print("Checkpoint captured once; comparing identical chunks.",flush=True)
  reports=[];order=[]
  for repetition in range(args.repetitions):
   shift=(repetition*2)%len(CODECS)
   for codec in CODECS[shift:]+CODECS[:shift]:
    name=f"{codec}-{repetition}";report=run(name,["compression","--capture",str(capture/"capture.json"),"--codec",codec])
    if report["codec"]!=codec:raise RuntimeError("Unexpected codec result")
    reports.append(report);order.append(name);print("Completed",name,flush=True)
  for field in ("capture_sha256","input_bytes","source"):
   if any(r[field]!=reports[0][field] for r in reports):raise RuntimeError(f"Compression inputs disagree on {field}")
  groups=[]
  for codec in CODECS:
   selected=[r for r in reports if r["codec"]==codec]
   if len({r["codec_version"] for r in selected})!=1:raise RuntimeError("Mixed codec versions")
   group={"codec":codec,"version":selected[0]["codec_version"],"runs":len(selected)}
   for field in ("compressed_bytes","compressed_capacity_bytes","compress_ns","decompress_ns","codec_setup_ns"):
    values=[int(r[field]) for r in selected];group[field]={"median":statistics.median(values),"min":min(values),"max":max(values),"observations":values}
   group["peak_private_bytes"]={"max":max(int(r["final_memory"]["peak_pagefile_bytes"]) for r in selected)}
   group["input_private_bytes"]={"median":statistics.median(int(r["input_memory"]["private_bytes"]) for r in selected)}
   groups.append(group)
  save(output/"comparison.json",{"format":"jahorta.salsa.ir-compression-comparison","version":1,"complete":True,"build_sha256":identity,"input_bytes":reports[0]["input_bytes"],"capture_sha256":reports[0]["capture_sha256"],"groups":groups,"order":order,"scope":reports[0]["scope"]})
 print("All codecs reproduced the exact checkpoint bytes. Temporary binary capture removed.",flush=True)
if __name__=="__main__":main()
