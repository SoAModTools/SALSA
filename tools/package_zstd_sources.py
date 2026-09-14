"""Package pinned zstd notices and the exact compiled source subset for a release."""
import argparse
import hashlib
import json
import shutil
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--release-directory", type=Path, required=True)
    args = parser.parse_args()
    dependency = ROOT / "third-party/zstd"
    manifest = json.loads((dependency / "SOURCE.json").read_text(encoding="utf-8"))
    for name, digest in manifest["files"].items():
        path = (dependency / name).resolve()
        if not path.is_relative_to(dependency.resolve()):
            raise ValueError("Source manifest path escapes dependency")
        if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            raise ValueError(f"Vendored source changed without updated manifest: {name}")
    destination = args.release_directory.resolve()
    destination.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(ROOT / "THIRD_PARTY_NOTICES.md", destination / "THIRD_PARTY_NOTICES.md")
    shutil.copyfile(dependency / "LICENSE", destination / "ZSTD-LICENSE.txt")
    with zipfile.ZipFile(destination / "zstd-1.5.7-salsa-sources.zip", "w", zipfile.ZIP_DEFLATED) as archive:
        for name in sorted([*manifest["files"], "SOURCE.json"]):
            archive.write(dependency / name, "third-party/zstd/" + name)
        for name in ["src/SalsaCore/SalsaCore.vcxproj", "build/Salsa.Common.props", "Directory.Build.props", "THIRD_PARTY_NOTICES.md"]:
            archive.write(ROOT / name, name)
    print("Packaged zstd source and notices in", destination)

if __name__ == "__main__":
    main()
