from __future__ import annotations

import json
import os
import pickle
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
if str(REPOSITORY_ROOT) not in sys.path:
    sys.path.insert(0, str(REPOSITORY_ROOT))

from tools.legacy_project_contract import CharacterizationLimits, characterize_project
from tools.legacy_project_contract import contract


def write_project(path: Path, project: object) -> None:
    with path.open("wb") as stream:
        pickle.dump(project, stream, protocol=4)


def make_instruction(base_id: int = 9) -> contract.SCTInstruction:
    instruction = contract.SCTInstruction()
    instruction.set_inst_id(base_id)
    return instruction


def make_script(name: str = "A001A") -> contract.SCTScript:
    script = contract.SCTScript(name)
    section = contract.SCTSection()
    section.set_name("start")
    instruction = make_instruction()
    section.add_instruction(instruction)
    section.inst_tree = [instruction.ID]
    script.add_section(section)
    script.sect_tree = [section.name]
    return script


def make_project(*scripts: contract.SCTScript) -> contract.SCTProject:
    project = contract.SCTProject()
    for script in scripts:
        project.scts[script.name] = script
    return project


class LegacyProjectContractTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def characterize(self, project: object, **kwargs):
        path = self.root / "project.prj"
        write_project(path, project)
        return characterize_project(path, **kwargs)

    def test_accepts_empty_official_v7_project_deterministically(self) -> None:
        report1, exit1 = self.characterize(make_project())
        report2, exit2 = self.characterize(make_project())

        self.assertEqual(0, exit1)
        self.assertEqual(0, exit2)
        self.assertEqual("accepted", report1["status"])
        self.assertEqual(report1, report2)
        self.assertEqual(0, report1["project"]["scriptCount"])
        self.assertEqual(contract.FIELD_DISPOSITIONS, report1["fieldDispositions"])

    def test_accepts_multi_script_project_and_inventories_content(self) -> None:
        first = make_script("A001A")
        second = make_script("B002B")
        second.strings["dialog"] = "\\h()Hello"
        second.sects["start"].insts[second.sects["start"].inst_list[0]].encode_inst = False
        project = make_project(first, second)
        project.global_variables["IntVar"][4] = "Coins"
        project.inst_id_colors[9] = "#112233"

        report, exit_code = self.characterize(project)

        self.assertEqual(0, exit_code)
        self.assertEqual(2, report["project"]["scriptCount"])
        self.assertEqual(1, report["project"]["globalVariableAliasCount"])
        self.assertEqual(1, report["project"]["instructionColorCount"])
        self.assertEqual(1, report["scripts"][1]["counts"]["strings"])
        self.assertEqual(1, report["scripts"][1]["counts"]["suppressedInstructions"])

    def test_inventories_links_opaque_values_raw_evidence_and_authoring_metadata(self) -> None:
        script = make_script("A001A")
        section = script.sects["start"]
        origin = section.insts[section.inst_list[0]]
        target = make_instruction(12)
        section.add_instruction(target)
        section.inst_tree.append(target.ID)
        link = contract.SCTLink(
            "Jump",
            script.name,
            0,
            [section.name, origin.ID, 0],
            4,
            [section.name, target.ID],
        )
        origin.links_out.append(link)
        target.links_in.append(link)
        script.links.append(link)
        parameter = contract.SCTParameter(0, "scpt")
        parameter.value = {"opaque": bytearray(b"value")}
        parameter.raw_bytes = bytearray(b"raw")
        parameter.override = bytearray(b"override")
        parameter.link = link
        parameter.linked_string = "shared"
        origin.params[0] = parameter
        section.garbage["tail"] = bytearray(b"section-garbage")
        script.header = bytearray(b"header")
        script.strings["shared"] = "\\h()Shared"
        script.string_groups["dialog"] = ["shared"]
        script.string_locations["shared"] = "dialog"
        script.string_garbage["shared"] = bytearray(b"string-garbage")
        script.folded_sects["original"] = section.name
        script.variables.append("IntVar: 4")

        report, exit_code = self.characterize(make_project(script))

        self.assertEqual(0, exit_code)
        counts = report["scripts"][0]["counts"]
        self.assertEqual(1, counts["links"])
        self.assertEqual(1, counts["parameters"])
        self.assertEqual(1, counts["strings"])
        self.assertGreaterEqual(counts["rawBytes"], 49)

    def test_applies_only_final_v7_load_normalizations(self) -> None:
        script = make_script()
        section = script.sects["start"]
        placeholder = make_instruction()
        placeholder.base_id = None
        section.insts[placeholder.ID] = placeholder
        section.inst_list.append(placeholder.ID)
        section.inst_tree.append(placeholder.ID)
        script.strings["dialog"] = "Hello"

        report, exit_code = self.characterize(make_project(script))

        self.assertEqual(0, exit_code)
        codes = [item["code"] for item in report["normalizations"]]
        self.assertEqual(
            ["placeholder-instruction-removed", "string-header-inserted"],
            sorted(codes),
        )
        self.assertEqual(1, report["scripts"][0]["counts"]["instructions"])

    def test_rejects_every_older_version_with_actionable_v6_message(self) -> None:
        for version in range(1, 7):
            with self.subTest(version=version):
                project = make_project()
                project.version = version
                report, exit_code = self.characterize(project)
                self.assertEqual(4, exit_code)
                self.assertEqual("unsupported-project-version", report["diagnostics"][0]["code"])
                if version == 6:
                    self.assertEqual(
                        "Project schema version 6 is not supported. Open this project in the final "
                        "legacy SALSA, save a new copy as version 7, then import the new copy.",
                        report["diagnostics"][0]["message"],
                    )

    def test_rejects_missing_and_newer_versions(self) -> None:
        missing = make_project()
        del missing.version
        missing_report, missing_exit = self.characterize(missing)
        newer = make_project()
        newer.version = 8
        newer_report, newer_exit = self.characterize(newer)

        self.assertEqual(4, missing_exit)
        self.assertEqual(4, newer_exit)
        self.assertIsNone(missing_report["input"]["declaredVersion"])
        self.assertEqual(8, newer_report["input"]["declaredVersion"])

    def test_rejects_missing_extra_and_development_fields_at_owner_scope(self) -> None:
        project = make_project()
        project.development_value = 12
        report, exit_code = self.characterize(project)
        self.assertEqual(5, exit_code)
        self.assertEqual("unexpected-field", report["diagnostics"][0]["code"])
        self.assertEqual("project", report["diagnostics"][0]["scope"])

        script = make_script()
        del script.footer
        report, exit_code = self.characterize(make_project(script))
        self.assertEqual(5, exit_code)
        self.assertEqual("failed", report["scripts"][0]["status"])
        self.assertEqual("script", report["scripts"][0]["diagnostics"][0]["scope"])

    def test_failed_script_can_be_explicitly_excluded(self) -> None:
        good = make_script("GOOD")
        bad = make_script("BAD")
        del bad.footer
        project = make_project(good, bad)

        unresolved, unresolved_exit = self.characterize(project)
        accepted, accepted_exit = self.characterize(project, excluded_scripts=["BAD"])

        self.assertEqual(5, unresolved_exit)
        self.assertEqual("action-required", unresolved["status"])
        self.assertEqual(0, accepted_exit)
        self.assertEqual("accepted", accepted["status"])
        statuses = {item["key"]: item["status"] for item in accepted["scripts"]}
        self.assertEqual({"GOOD": "accepted", "BAD": "excluded"}, statuses)
        self.assertTrue(accepted["scripts"][1]["diagnostics"])

    def test_inconsistent_known_metadata_requires_explicit_discard(self) -> None:
        project = make_project()
        project.inst_id_colors = {999: "#112233"}
        report, exit_code = self.characterize(project)
        record_id = report["diagnostics"][0]["id"]

        accepted, accepted_exit = self.characterize(
            project, discarded_metadata=[record_id])

        self.assertEqual(5, exit_code)
        self.assertTrue(report["diagnostics"][0]["discardable"])
        self.assertEqual(0, accepted_exit)
        self.assertEqual("accepted", accepted["status"])

    def test_encoder_failure_is_script_local_and_excludable(self) -> None:
        project = make_project(make_script("BROKEN"), make_script("GOOD"))
        original = contract._encode_script

        def encode(script, endian):
            if script.name == "BROKEN":
                raise ValueError("synthetic encoding failure")
            return {"status": "accepted", "endian": endian, "size": 1, "sha256": "00"}

        with mock.patch.object(contract, "_encode_script", side_effect=encode):
            report, exit_code = self.characterize(
                project, endian="little", excluded_scripts=["BROKEN"])

        self.assertEqual(0, exit_code)
        self.assertEqual("excluded", report["scripts"][0]["status"])
        self.assertEqual("failed", report["scripts"][0]["encoding"]["status"])
        self.assertEqual("accepted", report["scripts"][1]["encoding"]["status"])
        self.assertIsNotNone(original)

    def test_footer_literal_header_lookup_is_recovered_not_failed(self) -> None:
        script = make_script("FOOTER")
        section = script.sects["start"]
        instruction = section.insts[section.inst_list[0]]
        instruction.base_id = 24
        parameter = contract.SCTParameter(0, "int|footer|string")
        parameter.linked_string = "\\h()Footer literal"
        instruction.params[0] = parameter

        report, exit_code = self.characterize(
            make_project(script), endian="little")

        self.assertEqual(0, exit_code)
        script_report = report["scripts"][0]
        self.assertEqual("accepted", script_report["status"])
        self.assertEqual("accepted", script_report["encoding"]["status"])
        self.assertEqual(
            "legacy-footer-header-lookup-recovered",
            script_report["encoding"]["advisories"][0]["code"],
        )
        self.assertEqual("warning", script_report["diagnostics"][0]["severity"])

    def test_broken_link_is_script_local_and_excludable(self) -> None:
        script = make_script("BROKEN")
        origin = script.sects["start"].insts[script.sects["start"].inst_list[0]]
        link = contract.SCTLink(
            "Jump", script.name, 0, ["start", origin.ID, 0], 4,
            ["missing-section", "missing-instruction"])
        origin.links_out.append(link)
        script.links.append(link)

        unresolved, unresolved_exit = self.characterize(make_project(script))
        accepted, accepted_exit = self.characterize(
            make_project(script), excluded_scripts=[script.name])

        self.assertEqual(5, unresolved_exit)
        self.assertEqual("unresolved-link-trace", unresolved["scripts"][0]["diagnostics"][-1]["code"])
        self.assertEqual(0, accepted_exit)
        self.assertEqual("excluded", accepted["scripts"][0]["status"])

    def test_rejects_unexpected_pickle_global_without_executing_it(self) -> None:
        marker = self.root / "marker.txt"

        class Payload:
            def __reduce__(self):
                return os.system, (f'echo executed > "{marker}"',)

        path = self.root / "unsafe.prj"
        write_project(path, Payload())
        report, exit_code = characterize_project(path)

        self.assertEqual(3, exit_code)
        self.assertEqual("unsafe-pickle", report["diagnostics"][0]["code"])
        self.assertFalse(marker.exists())

    def test_rejects_malformed_trailing_and_wrong_protocol_pickles(self) -> None:
        malformed = self.root / "malformed.prj"
        malformed.write_bytes(b"\x80\x04broken")
        malformed_report, malformed_exit = characterize_project(malformed)

        trailing = self.root / "trailing.prj"
        write_project(trailing, make_project())
        with trailing.open("ab") as stream:
            stream.write(b"trailing")
        trailing_report, trailing_exit = characterize_project(trailing)

        protocol_five = self.root / "protocol5.prj"
        with protocol_five.open("wb") as stream:
            pickle.dump(make_project(), stream, protocol=5)
        protocol_report, protocol_exit = characterize_project(protocol_five)

        self.assertEqual(3, malformed_exit)
        self.assertEqual("malformed-pickle", malformed_report["diagnostics"][0]["code"])
        self.assertEqual(3, trailing_exit)
        self.assertEqual("unsafe-pickle", trailing_report["diagnostics"][0]["code"])
        self.assertEqual(3, protocol_exit)
        self.assertEqual("unsupported-pickle-protocol", protocol_report["diagnostics"][0]["code"])

    def test_rejects_pickle_extension_opcodes_during_preflight(self) -> None:
        extension = self.root / "extension.prj"
        extension.write_bytes(b"\x80\x04\x82\x01.")

        report, exit_code = characterize_project(extension)

        self.assertEqual(3, exit_code)
        self.assertEqual("unsafe-pickle", report["diagnostics"][0]["code"])
        self.assertIn("EXT1", report["diagnostics"][0]["message"])

    def test_enforces_and_can_disable_logical_resource_limits(self) -> None:
        project = make_project(make_script())
        limits = CharacterizationLimits(max_scripts=0)

        with self.assertRaises(contract.ResourceLimitError):
            self.characterize(project, limits=limits)
        report, exit_code = self.characterize(
            project, limits=limits, disable_resource_limits=True)

        self.assertEqual(0, exit_code)
        self.assertEqual("accepted", report["status"])
        self.assertIsNone(report["limits"])

    def test_input_size_limit_returns_a_resource_report(self) -> None:
        project = make_project()
        path = self.root / "project.prj"
        write_project(path, project)

        report, exit_code = characterize_project(
            path, limits=CharacterizationLimits(max_input_bytes=1))

        self.assertEqual(6, exit_code)
        self.assertEqual("cancelled", report["status"])
        self.assertEqual("resource-limit", report["diagnostics"][0]["code"])

    def test_cancellation_interrupts_before_unpickling(self) -> None:
        path = self.root / "project.prj"
        write_project(path, make_project(make_script()))

        with self.assertRaises(contract.CharacterizationCancelled):
            characterize_project(path, cancel=lambda: True)

    def test_cli_smoke_writes_deterministic_json_atomically(self) -> None:
        project = self.root / "project.prj"
        output = self.root / "report.json"
        write_project(project, make_project())

        completed = subprocess.run(
            [
                sys.executable,
                "-m",
                "tools.legacy_project_contract",
                "inspect",
                str(project),
                "--output",
                str(output),
            ],
            cwd=REPOSITORY_ROOT,
            capture_output=True,
            text=True,
            check=False,
        )

        self.assertEqual(0, completed.returncode, completed.stderr)
        with output.open("r", encoding="utf-8") as stream:
            report = json.load(stream)
        self.assertEqual("accepted", report["status"])
        self.assertFalse(output.with_name(output.name + ".tmp").exists())

    def test_expected_corpus_rejections_make_verification_succeed(self) -> None:
        corpus = self.root / "corpus"
        corpus.mkdir()
        accepted = make_project()
        rejected = make_project()
        rejected.version = 6
        write_project(corpus / "accepted.prj", accepted)
        write_project(corpus / "rejected.prj", rejected)
        expectations = self.root / "expectations.json"

        inspect = subprocess.run(
            [
                sys.executable,
                "-m",
                "tools.legacy_project_contract",
                "inspect-corpus",
                str(corpus),
                "--output",
                str(expectations),
            ],
            cwd=REPOSITORY_ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        verify = subprocess.run(
            [
                sys.executable,
                "-m",
                "tools.legacy_project_contract",
                "verify-corpus",
                str(corpus),
                "--expected",
                str(expectations),
            ],
            cwd=REPOSITORY_ROOT,
            capture_output=True,
            text=True,
            check=False,
        )

        self.assertEqual(0, inspect.returncode, inspect.stderr)
        self.assertEqual(0, verify.returncode, verify.stderr)


if __name__ == "__main__":
    unittest.main()
