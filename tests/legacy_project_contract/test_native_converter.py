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


def result_event(result: subprocess.CompletedProcess[str]) -> dict[str, object]:
    return json.loads(result.stdout.splitlines()[-1])


def capsule_files(root: Path) -> dict[Path, bytes]:
    return {
        path.relative_to(root): path.read_bytes()
        for path in root.rglob("*") if path.is_file()
    }


def make_populated_scripts(count: int) -> list[contract.SCTScript]:
    scripts: list[contract.SCTScript] = []
    for script_index in range(count):
        script = make_script(f"A{script_index:03d}A")
        for section_index in range(3):
            section = contract.SCTSection()
            section.name = f"section_{section_index}"
            instruction = contract.SCTInstruction()
            instruction.base_id = script_index + section_index
            parameter = contract.SCTParameter(section_index, "int")
            parameter.value = script_index * 10 + section_index
            instruction.params[section_index] = parameter
            section.add_instruction(instruction)
            script.add_section(section)
        script.strings["message"] = f"script {script_index}"
        scripts.append(script)
    return scripts


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
        files1 = capsule_files(first)
        files2 = capsule_files(second)
        self.assertEqual(files1, files2)
        first_event = result_event(result1)
        self.assertEqual("auto", first_event["scriptWorkers"]["requested"])
        self.assertEqual(0, first_event["scriptWorkers"]["used"])

    def test_worker_counts_produce_byte_identical_capsules_and_ids(self) -> None:
        source = self.write_project(
            "parallel.prj", make_project(*make_populated_scripts(6)))
        capsules: list[dict[Path, bytes]] = []
        capsule_ids: list[str] = []
        for workers in ("1", "2", "3", "4", "auto"):
            destination = self.root / f"workers-{workers}.salsa-legacy"
            result = self.convert(
                source, destination, "--script-workers", workers)
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            event = result_event(result)
            expected = min(4 if workers == "auto" else int(workers), 6)
            self.assertEqual(expected, event["scriptWorkers"]["used"])
            capsules.append(capsule_files(destination))
            capsule_ids.append(event["capsuleId"])
        self.assertTrue(all(files == capsules[0] for files in capsules[1:]))
        self.assertEqual(1, len(set(capsule_ids)))

    def test_workers_are_clamped_to_script_count_and_shared_values_are_safe(self) -> None:
        first, second = make_populated_scripts(2)
        shared = ("frame_delay", "Non-numeric frame delay given")
        first.errors.append(shared)
        second.errors.append(shared)
        source = self.write_project("shared.prj", make_project(first, second))
        serial = self.root / "shared-serial.salsa-legacy"
        parallel = self.root / "shared-parallel.salsa-legacy"
        serial_result = self.convert(source, serial, "--script-workers", "1")
        parallel_result = self.convert(source, parallel, "--script-workers", "4")
        self.assertEqual(0, serial_result.returncode, serial_result.stdout + serial_result.stderr)
        self.assertEqual(0, parallel_result.returncode,
                         parallel_result.stdout + parallel_result.stderr)
        self.assertEqual(2, result_event(parallel_result)["scriptWorkers"]["used"])
        self.assertEqual(capsule_files(serial), capsule_files(parallel))

    def test_parallel_progress_is_serialized_and_monotonic(self) -> None:
        source = self.write_project(
            "progress.prj", make_project(*make_populated_scripts(5)))
        result = self.convert(
            source, self.root / "progress.salsa-legacy", "--script-workers", "4")
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        events = [json.loads(line) for line in result.stdout.splitlines()]
        for phase in ("script", "write"):
            phase_events = [event for event in events
                            if event.get("type") == "progress"
                            and event.get("phase") == phase]
            self.assertEqual(list(range(6)),
                             [event["completed"] for event in phase_events])
            self.assertTrue(all(event["total"] == 5 for event in phase_events))

    def test_invalid_script_worker_values_are_rejected_by_cli(self) -> None:
        source = self.write_project("project.prj", make_project())
        for value in ("0", "5", "many"):
            destination = self.root / f"invalid-{value}.salsa-legacy"
            result = self.convert(source, destination, "--script-workers", value)
            self.assertEqual(2, result.returncode)
            self.assertFalse(destination.exists())
        missing = self.convert(source, self.root / "missing.salsa-legacy",
                               "--script-workers")
        self.assertEqual(2, missing.returncode)

    def test_v6_is_rejected_with_resave_direction_and_no_capsule(self) -> None:
        source = self.write_project("old.prj", make_project(version=6))
        destination = self.root / "old.salsa-legacy"
        result = self.convert(source, destination, "--script-workers", "4")
        self.assertEqual(4, result.returncode)
        self.assertIn("Open and resave", result.stdout)
        self.assertFalse(destination.exists())

    def test_script_local_shape_failure_finalizes_action_required_inventory(self) -> None:
        script = make_script()
        script.development_only_field = "not official"
        source = self.write_project("broken-script.prj", make_project(script))
        destination = self.root / "broken-script.salsa-legacy"
        result = self.convert(source, destination, "--script-workers", "4")
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
        event = result_event(result)
        self.assertEqual("result", event["type"])
        self.assertEqual(
            {"readProject", "normalizeScripts", "analyzeScripts", "encodeScripts",
             "encodeCpu", "compressOutputCpu", "convertScripts", "finalize", "total"},
            set(event["timingsMs"]),
        )
        self.assertGreaterEqual(
            event["timingsMs"]["total"],
            sum(
                event["timingsMs"][phase]
                for phase in ("readProject", "convertScripts", "finalize")
            ),
        )
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
        self.assertFalse(any(destination.rglob("*.part")))

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
