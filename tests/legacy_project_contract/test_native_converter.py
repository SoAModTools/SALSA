from __future__ import annotations

import json
import pickle
import subprocess
import tempfile
import unittest
import zlib
from pathlib import Path

from tools.legacy_project_contract import contract


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
CONVERTER = REPOSITORY_ROOT / "bin" / "x64" / "Debug" / "SalsaLegacyConverter.exe"


def make_project(*scripts: contract.SCTScript, version: int = 7) -> contract.SCTProject:
    project = contract.SCTProject()
    project.version = version
    project.scts = {script.name: script for script in scripts}
    return project


def make_script(name: str = "A001A") -> contract.SCTScript:
    return contract.SCTScript(name)


class NativeLegacyConverterTests(unittest.TestCase):
    def setUp(self) -> None:
        if not CONVERTER.is_file():
            self.skipTest("Build SalsaLegacyConverter before running native converter tests")
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def write_project(self, name: str, project: object) -> Path:
        path = self.root / name
        with path.open("wb") as stream:
            pickle.dump(project, stream, protocol=4)
        return path

    def convert(self, source: Path, destination: Path, *options: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [str(CONVERTER), "convert", str(source), str(destination), *options],
            cwd=REPOSITORY_ROOT,
            capture_output=True,
            text=True,
            check=False,
        )

    def test_empty_v7_project_produces_a_valid_deterministic_capsule(self) -> None:
        source = self.write_project("project.prj", make_project())
        first = self.root / "first.salsa-legacy"
        second = self.root / "second.salsa-legacy"
        result1 = self.convert(source, first)
        result2 = self.convert(source, second)
        self.assertEqual(0, result1.returncode, result1.stdout + result1.stderr)
        self.assertEqual(0, result2.returncode, result2.stdout + result2.stderr)
        manifest1 = json.loads((first / "capsule.json").read_text(encoding="utf-8"))
        manifest2 = json.loads((second / "capsule.json").read_text(encoding="utf-8"))
        self.assertEqual("ready", manifest1["status"])
        self.assertEqual(manifest1["capsuleId"], manifest2["capsuleId"])
        files1 = {
            path.relative_to(first): path.read_bytes()
            for path in first.rglob("*") if path.is_file()
        }
        files2 = {
            path.relative_to(second): path.read_bytes()
            for path in second.rglob("*") if path.is_file()
        }
        self.assertEqual(files1, files2)

    def test_v6_is_rejected_with_resave_direction_and_no_capsule(self) -> None:
        source = self.write_project("old.prj", make_project(version=6))
        destination = self.root / "old.salsa-legacy"
        result = self.convert(source, destination)
        self.assertEqual(4, result.returncode)
        self.assertIn("Open and resave", result.stdout)
        self.assertFalse(destination.exists())

    def test_script_local_shape_failure_finalizes_action_required_inventory(self) -> None:
        script = make_script()
        script.development_only_field = "not official"
        source = self.write_project("broken-script.prj", make_project(script))
        destination = self.root / "broken-script.salsa-legacy"
        result = self.convert(source, destination)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        manifest = json.loads((destination / "capsule.json").read_text(encoding="utf-8"))
        self.assertEqual("action-required", manifest["status"])
        self.assertEqual("failed", manifest["scripts"][0]["status"])

    def test_script_is_written_as_filtered_typed_ir_and_spool_is_removed(self) -> None:
        script = make_script()
        section = contract.SCTSection()
        section.name = "main"
        instruction = contract.SCTInstruction()
        instruction.base_id = 1
        parameter = contract.SCTParameter(0, "int")
        parameter.value = 42
        instruction.params[0] = parameter
        section.add_instruction(instruction)
        script.add_section(section)
        script.strings["message"] = "hello"
        script.string_groups["dialog"] = ["message"]
        source = self.write_project("typed.prj", make_project(script))
        destination = self.root / "typed.salsa-legacy"

        result = self.convert(source, destination)

        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        manifest = json.loads((destination / "capsule.json").read_text(encoding="utf-8"))
        self.assertEqual(2, manifest["schemaVersion"])
        self.assertEqual(1, manifest["scripts"][0]["sections"])
        self.assertEqual(1, manifest["scripts"][0]["instructions"])
        self.assertEqual(1, manifest["scripts"][0]["parameters"])
        record = zlib.decompress(
            (destination / manifest["scripts"][0]["path"]).read_bytes()
        )
        for retained in (b"string_groups", b"sections", b"sidecar"):
            self.assertIn(retained, record)
        for recomputed in (
            b"absolute_offset", b"inst_locations", b"string_locations",
            b"formatted_value", b"arithmetic_value", b"synopsis",
        ):
            self.assertNotIn(recomputed, record)
        self.assertFalse(any(self.root.glob("*.pickle-spool-*.tmp")))

    def test_optional_original_is_inert_and_does_not_change_capsule_identity(self) -> None:
        source = self.write_project("project.prj", make_project())
        ordinary = self.root / "ordinary.salsa-legacy"
        retained = self.root / "retained.salsa-legacy"
        self.assertEqual(0, self.convert(source, ordinary).returncode)
        self.assertEqual(0, self.convert(source, retained, "--retain-original").returncode)
        first = json.loads((ordinary / "capsule.json").read_text(encoding="utf-8"))
        second = json.loads((retained / "capsule.json").read_text(encoding="utf-8"))
        self.assertEqual(first["capsuleId"], second["capsuleId"])
        self.assertEqual(source.read_bytes(), (retained / "evidence" / "source.prj").read_bytes())

    def test_unexpected_global_and_trailing_data_are_rejected(self) -> None:
        global_source = self.write_project("global.prj", Path("not-allowed"))
        trailing_source = self.write_project("trailing.prj", make_project())
        with trailing_source.open("ab") as stream:
            stream.write(b"trailing")
        self.assertEqual(4, self.convert(global_source, self.root / "global-out").returncode)
        self.assertEqual(4, self.convert(trailing_source, self.root / "trailing-out").returncode)

    def test_unicode_source_and_destination_paths_are_supported(self) -> None:
        source = self.write_project("航海 project.prj", make_project())
        destination = self.root / "cápsule 航海.salsa-legacy"
        result = self.convert(source, destination)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertTrue((destination / "capsule.json").is_file())

    def test_non_protocol_4_pickle_is_rejected(self) -> None:
        source = self.root / "protocol-5.prj"
        with source.open("wb") as stream:
            pickle.dump(make_project(), stream, protocol=5)
        destination = self.root / "protocol-5.salsa-legacy"
        result = self.convert(source, destination)
        self.assertEqual(4, result.returncode)
        self.assertIn("protocol 4", result.stdout)
        self.assertFalse(destination.exists())


if __name__ == "__main__":
    unittest.main()
