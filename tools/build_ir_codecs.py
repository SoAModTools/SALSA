"""Provision optional, pinned Release compression libraries for SalsaIrEvaluation.
Run from an elevated shell. Downloads/builds stay ignored; no submodules change.
"""
import hashlib, io, json, subprocess, urllib.request, zipfile
from pathlib import Path
from xml.sax.saxutils import escape

ROOT=Path(__file__).resolve().parents[1]
PACKAGES=[
 ("facebook/zstd","v1.5.7","7897bc5d620580d9b7cd3539c44b59d78f3657d33663fe97a145e07b4ebd69a4","build/cmake",["ZSTD_BUILD_SHARED=OFF","ZSTD_BUILD_PROGRAMS=OFF","ZSTD_BUILD_TESTS=OFF","ZSTD_MULTITHREAD_SUPPORT=OFF"]),
 ("lz4/lz4","v1.10.0","3224b4c80f351f194984526ef396f6079bd6332dd9825c72ac0d7a37b3cdc565","build/cmake",["BUILD_SHARED_LIBS=OFF","BUILD_STATIC_LIBS=ON","LZ4_BUILD_CLI=OFF"]),
 ("ebiggers/libdeflate","v1.24","1e33358dc545a3258af213e2fb0ebb84e6d970bb098d774c33bdacfbdde4a087","",["LIBDEFLATE_BUILD_SHARED_LIB=OFF","LIBDEFLATE_BUILD_GZIP=OFF","LIBDEFLATE_BUILD_TESTS=OFF"])]

def main():
 cmake=Path("C:/Program Files/Microsoft Visual Studio/18/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe")
 prefix=ROOT/"bin/ir-codecs";work=ROOT/"private/ir-compression-deps";work.mkdir(parents=True,exist_ok=True)
 # Do not leave stale success metadata if provisioning fails.
 props=prefix/"CompressionDependencies.props"
 if props.exists(): props.unlink()
 receipt=[]
 for repo,tag,expected,subdir,options in PACKAGES:
  url=f"https://codeload.github.com/{repo}/zip/refs/tags/{tag}"
  data=urllib.request.urlopen(url,timeout=60).read()
  if hashlib.sha256(data).hexdigest()!=expected: raise ValueError(f"Source hash mismatch for {repo}")
  source=work/expected;source.mkdir(exist_ok=True)
  with zipfile.ZipFile(io.BytesIO(data)) as archive:
   for item in archive.infolist():
    if not (source/item.filename).resolve().is_relative_to(source.resolve()):raise ValueError("Archive path escapes source directory")
   archive.extractall(source)
  directory=next(p for p in source.iterdir() if p.is_dir())
  build=work/(repo.split('/')[-1]+"-build")
  subprocess.run([str(cmake),"-S",str(directory/subdir),"-B",str(build),"-G","Visual Studio 18 2026","-A","x64","-T","v145",f"-DCMAKE_INSTALL_PREFIX={prefix}","-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL","-DCMAKE_POLICY_DEFAULT_CMP0091=NEW",*["-D"+x for x in options]],check=True)
  subprocess.run([str(cmake),"--build",str(build),"--config","Release","--parallel"],check=True)
  subprocess.run([str(cmake),"--install",str(build),"--config","Release"],check=True)
  receipt.append({"repository":repo,"tag":tag,"archive_sha256":expected})
 libs=sorted((prefix/"lib").glob("*.lib"))
 if len(libs)!=3:raise ValueError(f"Expected three static libraries, found {libs}")
 receipt.append({"libraries":{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in libs}})
 (prefix/"receipt.json").write_text(json.dumps(receipt,indent=2)+"\n")
 includes=escape(str(prefix/"include"));dependencies=escape(';'.join(map(str,libs)))
 props.write_text(f'<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003"><ItemDefinitionGroup><ClCompile><AdditionalIncludeDirectories>{includes};%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories><PreprocessorDefinitions>SALSA_IR_CODECS;%(PreprocessorDefinitions)</PreprocessorDefinitions></ClCompile><Link><AdditionalDependencies>{dependencies};%(AdditionalDependencies)</AdditionalDependencies></Link></ItemDefinitionGroup></Project>')
 print("Compression dependencies ready:",props)
if __name__=="__main__":main()
