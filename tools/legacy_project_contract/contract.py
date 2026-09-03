from __future__ import annotations

import contextlib
import copy
import hashlib
import io
import json
import os
import pickle
import pickletools
import sys
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any, Callable, Iterable


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
LEGACY_PYTHON_ROOT = REPOSITORY_ROOT / "legacy" / "python"
if str(LEGACY_PYTHON_ROOT) not in sys.path:
    sys.path.insert(0, str(LEGACY_PYTHON_ROOT))

from SALSA.Project.project_container import (  # noqa: E402
    SCTInstruction,
    SCTLink,
    SCTParameter,
    SCTProject,
    SCTScript,
    SCTSection,
)


CONTRACT_VERSION = "legacy-prj-v7-characterization-1"
SUPPORTED_PROJECT_VERSION = 7
SUPPORTED_PICKLE_PROTOCOL = 4


class UnsafePickleError(pickle.UnpicklingError):
    pass


class ResourceLimitError(RuntimeError):
    pass


class CharacterizationCancelled(RuntimeError):
    pass


@dataclass(frozen=True)
class CharacterizationLimits:
    max_input_bytes: int = 1 << 30
    max_scripts: int = 1 << 9
    max_sections: int = 1 << 16
    max_instructions: int = 1 << 20
    max_parameters: int = 1 << 22
    max_links: int = 1 << 22
    max_strings: int = 1 << 18
    max_raw_bytes: int = 1 << 27
    max_value_depth: int = 256
    max_text_bytes: int = 1 << 26


FIELD_DISPOSITIONS = {
    "SCTProject": {
        "scts": "semantic",
        "file_name": "run-local",
        "filepath": "run-local-discard",
        "global_variables": "authoring",
        "version": "contract",
        "inst_id_colors": "authoring",
    },
    "SCTScript": {
        "folded_sects": "authoring",
        "name": "semantic",
        "index": "preserved-raw",
        "header": "preserved-raw",
        "sects": "semantic",
        "sect_tree": "authoring",
        "sect_list": "semantic-order",
        "inst_locations": "derived-recompute",
        "links": "semantic",
        "footer": "semantic",
        "strings": "semantic",
        "string_groups": "authoring",
        "string_locations": "derived-recompute",
        "string_garbage": "preserved-raw",
        "unused_sections": "advisory",
        "errors": "advisory",
        "error_sections": "advisory",
        "variables": "authoring",
        "section_num": "derived-recompute",
    },
    "SCTSection": {
        "name": "semantic",
        "length": "derived-recompute",
        "absolute_offset": "derived-recompute",
        "insts": "semantic",
        "inst_tree": "authoring",
        "inst_list": "semantic-order",
        "inst_errors": "advisory",
        "errors": "advisory",
        "strings": "semantic",
        "insts_used": "derived-recompute",
        "garbage": "preserved-raw",
        "string": "semantic",
        "jump_loops": "derived-recompute",
        "internal_sections_inst": "derived-recompute",
        "internal_sections_curs": "derived-recompute",
        "is_compound": "semantic",
        "type": "semantic",
    },
    "SCTInstruction": {
        "ID": "identity",
        "base_id": "semantic",
        "absolute_offset": "derived-recompute",
        "skip_refresh": "semantic",
        "delay_param": "semantic",
        "errors": "advisory",
        "links_out": "semantic",
        "links_in": "derived-recompute",
        "params": "semantic",
        "l_params": "semantic",
        "condition": "derived-recompute",
        "synopsis": "derived-recompute",
        "ungrouped_position": "derived-recompute",
        "my_goto_uuids": "authoring",
        "my_master_uuids": "authoring",
        "label": "authoring",
        "encode_inst": "authoring",
    },
    "SCTParameter": {
        "ID": "identity",
        "type": "semantic",
        "link": "semantic",
        "errors": "advisory",
        "analyze_log": "advisory",
        "value": "semantic",
        "formatted_value": "derived-recompute",
        "raw_bytes": "preserved-raw",
        "linked_string": "semantic",
        "override": "semantic",
        "arithmetic_value": "derived-recompute",
    },
    "SCTLink": {
        "type": "semantic",
        "script": "semantic",
        "origin": "derived-recompute",
        "origin_trace": "semantic",
        "target": "derived-recompute",
        "target_trace": "semantic",
        "ID": "identity",
    },
}


EXPECTED_ATTRIBUTES = {
    SCTProject: frozenset(FIELD_DISPOSITIONS["SCTProject"]),
    SCTScript: frozenset(FIELD_DISPOSITIONS["SCTScript"]),
    SCTSection: frozenset(FIELD_DISPOSITIONS["SCTSection"]),
    SCTInstruction: frozenset(FIELD_DISPOSITIONS["SCTInstruction"]),
    SCTParameter: frozenset(FIELD_DISPOSITIONS["SCTParameter"]),
    SCTLink: frozenset(FIELD_DISPOSITIONS["SCTLink"]),
}


ALLOWED_GLOBALS = {
    ("SALSA.Project.project_container", "SCTProject"): SCTProject,
    ("SALSA.Project.project_container", "SCTScript"): SCTScript,
    ("SALSA.Project.project_container", "SCTSection"): SCTSection,
    ("SALSA.Project.project_container", "SCTInstruction"): SCTInstruction,
    ("SALSA.Project.project_container", "SCTParameter"): SCTParameter,
    ("SALSA.Project.project_container", "SCTLink"): SCTLink,
    ("builtins", "bytearray"): bytearray,
}


class RestrictedProjectUnpickler(pickle.Unpickler):
    def find_class(self, module: str, name: str) -> Any:
        allowed = ALLOWED_GLOBALS.get((module, name))
        if allowed is None:
            raise UnsafePickleError(f"pickle global is not allowed: {module}.{name}")
        return allowed

    def persistent_load(self, pid: object) -> Any:
        raise UnsafePickleError("persistent pickle identifiers are not allowed")


def _sha256_file(path: Path, cancel: Callable[[], bool] | None) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            if cancel is not None and cancel():
                raise CharacterizationCancelled("characterization cancelled")
            digest.update(chunk)
    return digest.hexdigest().upper()


def _preflight_pickle(path: Path, cancel: Callable[[], bool] | None) -> None:
    forbidden = {"EXT1", "EXT2", "EXT4", "PERSID", "BINPERSID"}
    with path.open("rb") as stream:
        for index, (opcode, _, _) in enumerate(pickletools.genops(stream)):
            if opcode.name in forbidden:
                raise UnsafePickleError(
                    f"pickle opcode is not allowed: {opcode.name}"
                )
            if index % 8192 == 0 and cancel is not None and cancel():
                raise CharacterizationCancelled("characterization cancelled")


def _diagnostic(
    code: str,
    severity: str,
    scope: str,
    path: str,
    message: str,
    *,
    discardable: bool = False,
) -> dict[str, Any]:
    record_id = hashlib.sha256(
        f"{code}\0{scope}\0{path}".encode("utf-8")
    ).hexdigest()[:16]
    return {
        "id": record_id,
        "code": code,
        "severity": severity,
        "scope": scope,
        "path": path,
        "message": message,
        "discardable": discardable,
    }


class _Budget:
    def __init__(self, limits: CharacterizationLimits, disabled: bool) -> None:
        self.limits = limits
        self.disabled = disabled
        self.values = {
            "scripts": 0,
            "sections": 0,
            "instructions": 0,
            "parameters": 0,
            "links": 0,
            "strings": 0,
            "rawBytes": 0,
        }

    def consume(self, name: str, amount: int = 1) -> None:
        self.values[name] += amount
        if self.disabled:
            return
        maximum = {
            "scripts": self.limits.max_scripts,
            "sections": self.limits.max_sections,
            "instructions": self.limits.max_instructions,
            "parameters": self.limits.max_parameters,
            "links": self.limits.max_links,
            "strings": self.limits.max_strings,
            "rawBytes": self.limits.max_raw_bytes,
        }[name]
        if self.values[name] > maximum:
            raise ResourceLimitError(
                f"resource limit exceeded for {name}: {self.values[name]} > {maximum}"
            )


def _check_attributes(obj: object, path: str) -> list[dict[str, Any]]:
    expected = EXPECTED_ATTRIBUTES[type(obj)]
    actual = frozenset(vars(obj))
    diagnostics: list[dict[str, Any]] = []
    for name in sorted(expected - actual):
        diagnostics.append(_diagnostic(
            "missing-field", "error", "project" if type(obj) is SCTProject else "script",
            f"{path}.{name}", f"required field is missing: {name}"))
    for name in sorted(actual - expected):
        diagnostics.append(_diagnostic(
            "unexpected-field", "error", "project" if type(obj) is SCTProject else "script",
            f"{path}.{name}", f"unexpected field is not part of the official v7 shape: {name}"))
    return diagnostics


def _is_int(value: object) -> bool:
    return type(value) is int


def _validate_inert_value(
    value: object,
    path: str,
    limits: CharacterizationLimits,
    depth: int = 0,
    active: set[int] | None = None,
) -> list[dict[str, Any]]:
    if depth > limits.max_value_depth:
        raise ResourceLimitError(
            f"resource limit exceeded for value depth at {path}"
        )
    if value is None or type(value) in (bool, int, float):
        return []
    if type(value) is str:
        if len(value.encode("utf-8", errors="surrogatepass")) > limits.max_text_bytes:
            raise ResourceLimitError(f"text value exceeds limit at {path}")
        return []
    if type(value) in (bytes, bytearray):
        return []
    if type(value) not in (list, tuple, dict, set, frozenset):
        return [_diagnostic(
            "non-inert-value", "error", "script", path,
            f"value has unsupported runtime type: {type(value).__module__}.{type(value).__name__}")]

    active = set() if active is None else active
    identity = id(value)
    if identity in active:
        return [_diagnostic(
            "cyclic-value", "error", "script", path,
            "cyclic containers are not valid inert metadata")]
    active.add(identity)
    diagnostics: list[dict[str, Any]] = []
    if type(value) is dict:
        for index, (key, child) in enumerate(value.items()):
            diagnostics.extend(_validate_inert_value(
                key, f"{path}.key[{index}]", limits, depth + 1, active))
            diagnostics.extend(_validate_inert_value(
                child, f"{path}[{index}]", limits, depth + 1, active))
    else:
        for index, child in enumerate(value):
            diagnostics.extend(_validate_inert_value(
                child, f"{path}[{index}]", limits, depth + 1, active))
    active.remove(identity)
    return diagnostics


def _raw_byte_count(value: object, active: set[int] | None = None) -> int:
    if type(value) in (bytes, bytearray):
        return len(value)
    if type(value) not in (list, tuple, dict, set, frozenset):
        return 0
    active = set() if active is None else active
    identity = id(value)
    if identity in active:
        return 0
    active.add(identity)
    if type(value) is dict:
        total = sum(
            _raw_byte_count(key, active) + _raw_byte_count(child, active)
            for key, child in value.items()
        )
    else:
        total = sum(_raw_byte_count(child, active) for child in value)
    active.remove(identity)
    return total


def _remove_tree_reference(value: object, instruction_id: str) -> object:
    if type(value) is list:
        return [
            normalized
            for item in value
            if (normalized := _remove_tree_reference(item, instruction_id)) is not None
        ]
    if type(value) is dict:
        result = {}
        for key, child in value.items():
            if type(key) is str and key.split("|", 1)[0] == instruction_id:
                continue
            normalized = _remove_tree_reference(child, instruction_id)
            if normalized is not None:
                result[key] = normalized
        return result
    if value == instruction_id:
        return None
    return value


def _normalize_script(script: SCTScript, script_key: str) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    for section_key, section in list(script.sects.items()):
        if type(section) is not SCTSection or type(section.insts) is not dict:
            continue
        placeholders = [
            instruction_id
            for instruction_id, instruction in section.insts.items()
            if type(instruction) is SCTInstruction
            and getattr(instruction, "base_id", object()) is None
        ]
        for instruction_id in placeholders:
            linked = False
            for candidate in section.insts.values():
                if type(candidate) is not SCTInstruction:
                    continue
                for link in list(getattr(candidate, "links_in", [])) + list(getattr(candidate, "links_out", [])):
                    if type(link) is SCTLink and (
                        instruction_id in getattr(link, "origin_trace", [])
                        or instruction_id in (getattr(link, "target_trace", None) or [])
                    ):
                        linked = True
            if linked:
                records.append({
                    "code": "placeholder-normalization-blocked",
                    "script": script_key,
                    "entity": f"{section_key}/{instruction_id}",
                })
                continue
            section.insts.pop(instruction_id)
            if instruction_id in section.inst_list:
                section.inst_list.remove(instruction_id)
            section.inst_tree = _remove_tree_reference(
                section.inst_tree, instruction_id)
            for index, remaining_id in enumerate(section.inst_list):
                if remaining_id in section.insts:
                    section.insts[remaining_id].ungrouped_position = index
            records.append({
                "code": "placeholder-instruction-removed",
                "script": script_key,
                "entity": f"{section_key}/{instruction_id}",
            })

    if type(script.strings) is dict:
        for string_id, value in list(script.strings.items()):
            if type(value) is str and "\\h" not in value:
                script.strings[string_id] = "\\h()" + value
                records.append({
                    "code": "string-header-inserted",
                    "script": script_key,
                    "entity": str(string_id),
                })
    return records


def _validate_parameter(
    parameter: object,
    path: str,
    budget: _Budget,
    limits: CharacterizationLimits,
) -> list[dict[str, Any]]:
    if type(parameter) is not SCTParameter:
        return [_diagnostic(
            "invalid-parameter-type", "error", "script", path,
            f"expected SCTParameter, found {type(parameter).__name__}")]
    budget.consume("parameters")
    diagnostics = _check_attributes(parameter, path)
    if diagnostics:
        return diagnostics
    if not _is_int(parameter.ID):
        diagnostics.append(_diagnostic(
            "invalid-parameter-id", "error", "script", f"{path}.ID",
            "parameter ID must be an integer"))
    if type(parameter.type) is not str:
        diagnostics.append(_diagnostic(
            "invalid-parameter-kind", "error", "script", f"{path}.type",
            "parameter type must be a string"))
    if type(parameter.raw_bytes) is not bytearray:
        diagnostics.append(_diagnostic(
            "invalid-raw-bytes", "error", "script", f"{path}.raw_bytes",
            "raw parameter bytes must be a bytearray"))
    else:
        budget.consume("rawBytes", len(parameter.raw_bytes))
    diagnostics.extend(_validate_inert_value(parameter.value, f"{path}.value", limits))
    diagnostics.extend(_validate_inert_value(parameter.analyze_log, f"{path}.analyze_log", limits))
    budget.consume("rawBytes", _raw_byte_count(parameter.value))
    if parameter.override is not None and type(parameter.override) is not bytearray:
        diagnostics.append(_diagnostic(
            "invalid-override", "error", "script", f"{path}.override",
            "parameter override must be a bytearray or null"))
    elif type(parameter.override) is bytearray:
        budget.consume("rawBytes", len(parameter.override))
    if parameter.link is not None and type(parameter.link) is not SCTLink:
        diagnostics.append(_diagnostic(
            "invalid-parameter-link", "error", "script", f"{path}.link",
            "parameter link must be an SCTLink or null"))
    return diagnostics


def _validate_link(link: object, path: str, budget: _Budget) -> list[dict[str, Any]]:
    if type(link) is not SCTLink:
        return [_diagnostic(
            "invalid-link-type", "error", "script", path,
            f"expected SCTLink, found {type(link).__name__}")]
    diagnostics = _check_attributes(link, path)
    if diagnostics:
        return diagnostics
    if type(link.type) is not str or link.type not in ("Jump", "Switch", "String", "Footer"):
        diagnostics.append(_diagnostic(
            "invalid-link-kind", "error", "script", f"{path}.type",
            f"unsupported link kind: {link.type!r}"))
    if type(link.script) is not str:
        diagnostics.append(_diagnostic(
            "invalid-link-script", "error", "script", f"{path}.script",
            "link script must be a string"))
    if type(link.origin_trace) is not list or not all(type(v) in (str, int) for v in link.origin_trace):
        diagnostics.append(_diagnostic(
            "invalid-origin-trace", "error", "script", f"{path}.origin_trace",
            "link origin trace must contain only strings and integers"))
    if link.target_trace is not None and (
        type(link.target_trace) is not list
        or not all(type(v) in (str, int) for v in link.target_trace)
    ):
        diagnostics.append(_diagnostic(
            "invalid-target-trace", "error", "script", f"{path}.target_trace",
            "link target trace must be null or contain only strings and integers"))
    return diagnostics


def _validate_script(
    script_key: str,
    script: object,
    budget: _Budget,
    limits: CharacterizationLimits,
    cancel: Callable[[], bool] | None,
) -> tuple[list[dict[str, Any]], dict[str, int]]:
    diagnostics: list[dict[str, Any]] = []
    counts = {
        "sections": 0,
        "instructions": 0,
        "parameters": 0,
        "links": 0,
        "strings": 0,
        "suppressedInstructions": 0,
        "rawBytes": 0,
    }
    before = dict(budget.values)
    if type(script) is not SCTScript:
        return ([_diagnostic(
            "invalid-script-type", "error", "script", f"project.scts[{script_key!r}]",
            f"expected SCTScript, found {type(script).__name__}")], counts)
    budget.consume("scripts")
    script_path = f"project.scts[{script_key!r}]"
    diagnostics.extend(_check_attributes(script, script_path))
    if diagnostics:
        return diagnostics, counts
    if type(script_key) is not str or type(script.name) is not str or script_key != script.name:
        diagnostics.append(_diagnostic(
            "script-key-name-mismatch", "error", "script", f"{script_path}.name",
            f"project key {script_key!r} must exactly match the stored script name {script.name!r}"))
    if type(script.sects) is not dict:
        diagnostics.append(_diagnostic(
            "invalid-sections", "error", "script", f"{script_path}.sects",
            "script sections must be a dictionary"))
        return diagnostics, counts
    if type(script.sect_list) is not list or len(script.sect_list) != len(set(script.sect_list)):
        diagnostics.append(_diagnostic(
            "invalid-section-order", "error", "script", f"{script_path}.sect_list",
            "section order must be a duplicate-free list"))
    elif any(name not in script.sects for name in script.sect_list):
        diagnostics.append(_diagnostic(
            "unknown-ordered-section", "error", "script", f"{script_path}.sect_list",
            "section order refers to a missing section"))

    seen_instruction_ids: set[str] = set()
    referenced_links: dict[int, SCTLink] = {}
    for section_key, section in script.sects.items():
        if cancel is not None and cancel():
            raise CharacterizationCancelled("characterization cancelled")
        section_path = f"{script_path}.sects[{section_key!r}]"
        if type(section) is not SCTSection:
            diagnostics.append(_diagnostic(
                "invalid-section-type", "error", "script", section_path,
                f"expected SCTSection, found {type(section).__name__}"))
            continue
        budget.consume("sections")
        diagnostics.extend(_check_attributes(section, section_path))
        if set(vars(section)) != set(EXPECTED_ATTRIBUTES[SCTSection]):
            continue
        if type(section_key) is not str or section.name != section_key:
            diagnostics.append(_diagnostic(
                "section-key-name-mismatch", "error", "script", f"{section_path}.name",
                "section dictionary key must exactly match the stored section name"))
        if type(section.insts) is not dict or type(section.inst_list) is not list:
            diagnostics.append(_diagnostic(
                "invalid-instruction-container", "error", "script", section_path,
                "section instructions must be a dictionary with a list order"))
            continue
        if len(section.inst_list) != len(set(section.inst_list)):
            diagnostics.append(_diagnostic(
                "duplicate-instruction-order", "error", "script", f"{section_path}.inst_list",
                "instruction order contains duplicates"))
        if set(section.inst_list) != set(section.insts):
            diagnostics.append(_diagnostic(
                "instruction-order-mismatch", "error", "script", f"{section_path}.inst_list",
                "instruction order and instruction dictionary must contain the same IDs"))
        for instruction_id, instruction in section.insts.items():
            instruction_path = f"{section_path}.insts[{instruction_id!r}]"
            if type(instruction) is not SCTInstruction:
                diagnostics.append(_diagnostic(
                    "invalid-instruction-type", "error", "script", instruction_path,
                    f"expected SCTInstruction, found {type(instruction).__name__}"))
                continue
            budget.consume("instructions")
            diagnostics.extend(_check_attributes(instruction, instruction_path))
            if set(vars(instruction)) != set(EXPECTED_ATTRIBUTES[SCTInstruction]):
                continue
            if type(instruction_id) is not str or instruction.ID != instruction_id:
                diagnostics.append(_diagnostic(
                    "instruction-key-id-mismatch", "error", "script", f"{instruction_path}.ID",
                    "instruction dictionary key must exactly match the instruction ID"))
            if instruction_id in seen_instruction_ids:
                diagnostics.append(_diagnostic(
                    "duplicate-instruction-id", "error", "script", instruction_path,
                    "instruction IDs must be unique within a script"))
            seen_instruction_ids.add(instruction_id)
            if instruction.base_id is not None and (
                not _is_int(instruction.base_id) or not 0 <= instruction.base_id < 266
            ):
                diagnostics.append(_diagnostic(
                    "invalid-base-instruction", "error", "script", f"{instruction_path}.base_id",
                    "base instruction ID must be null or an integer from 0 through 265"))
            if instruction.encode_inst is False:
                counts["suppressedInstructions"] += 1
            if type(instruction.params) is not dict or type(instruction.l_params) is not list:
                diagnostics.append(_diagnostic(
                    "invalid-parameter-container", "error", "script", instruction_path,
                    "instruction parameters must use the v7 dictionary/list containers"))
                continue
            for parameter_id, parameter in instruction.params.items():
                diagnostics.extend(_validate_parameter(
                    parameter, f"{instruction_path}.params[{parameter_id!r}]", budget, limits))
            if instruction.delay_param is not None:
                diagnostics.extend(_validate_parameter(
                    instruction.delay_param, f"{instruction_path}.delay_param", budget, limits))
            for loop_index, loop in enumerate(instruction.l_params):
                if type(loop) is not dict:
                    diagnostics.append(_diagnostic(
                        "invalid-loop-parameters", "error", "script",
                        f"{instruction_path}.l_params[{loop_index}]",
                        "loop parameter entry must be a dictionary"))
                    continue
                for parameter_id, parameter in loop.items():
                    diagnostics.extend(_validate_parameter(
                        parameter,
                        f"{instruction_path}.l_params[{loop_index}][{parameter_id!r}]",
                        budget,
                        limits,
                    ))
            for direction, links in (("links_in", instruction.links_in), ("links_out", instruction.links_out)):
                if type(links) is not list:
                    diagnostics.append(_diagnostic(
                        "invalid-instruction-links", "error", "script",
                        f"{instruction_path}.{direction}", "instruction links must be a list"))
                    continue
                for link_index, link in enumerate(links):
                    if type(link) is not SCTLink:
                        diagnostics.append(_diagnostic(
                            "invalid-instruction-link", "error", "script",
                            f"{instruction_path}.{direction}[{link_index}]",
                            "instruction link entry must be an SCTLink"))
                    else:
                        referenced_links[id(link)] = link
            for parameter in list(instruction.params.values()) + (
                [instruction.delay_param] if instruction.delay_param is not None else []
            ):
                if type(parameter) is SCTParameter and type(parameter.link) is SCTLink:
                    referenced_links[id(parameter.link)] = parameter.link
            for loop in instruction.l_params:
                if type(loop) is dict:
                    for parameter in loop.values():
                        if type(parameter) is SCTParameter and type(parameter.link) is SCTLink:
                            referenced_links[id(parameter.link)] = parameter.link

        if type(section.garbage) in (dict, list, tuple, set, frozenset, bytes, bytearray):
            budget.consume("rawBytes", _raw_byte_count(section.garbage))

    if type(script.links) is not list:
        diagnostics.append(_diagnostic(
            "invalid-script-links", "error", "script", f"{script_path}.links",
            "script links must be a list"))
    else:
        for index, link in enumerate(script.links):
            if type(link) is not SCTLink:
                diagnostics.append(_diagnostic(
                    "invalid-script-link", "error", "script",
                    f"{script_path}.links[{index}]", "script link entry must be an SCTLink"))
            else:
                referenced_links[id(link)] = link
    for index, link in enumerate(referenced_links.values()):
        budget.consume("links")
        diagnostics.extend(_validate_link(link, f"{script_path}.linkRecords[{index}]", budget))
        if link.script != script.name:
            diagnostics.append(_diagnostic(
                "link-script-mismatch", "error", "script",
                f"{script_path}.linkRecords[{index}].script",
                "link script must match its owning script"))
        for trace_name, trace in (("origin_trace", link.origin_trace), ("target_trace", link.target_trace)):
            if trace is None or len(trace) < 2 or type(trace[0]) is not str or type(trace[1]) is not str:
                continue
            if link.type in ("Jump", "Switch") and (
                trace[0] not in script.sects
                or trace[1] not in getattr(script.sects.get(trace[0]), "insts", {})
            ):
                diagnostics.append(_diagnostic(
                    "unresolved-link-trace", "error", "script",
                    f"{script_path}.linkRecords[{index}].{trace_name}",
                    "jump or switch link trace does not resolve within the owning script"))
    if type(script.strings) is not dict:
        diagnostics.append(_diagnostic(
            "invalid-strings", "error", "script", f"{script_path}.strings",
            "script strings must be a dictionary"))
    else:
        budget.consume("strings", len(script.strings))
        for string_id, value in script.strings.items():
            if type(string_id) is not str or type(value) is not str:
                diagnostics.append(_diagnostic(
                    "invalid-string-entry", "error", "script",
                    f"{script_path}.strings[{string_id!r}]",
                    "string IDs and values must both be strings"))
            elif len(value.encode("utf-8", errors="surrogatepass")) > limits.max_text_bytes:
                raise ResourceLimitError(f"text value exceeds limit at {script_path}.strings")
    diagnostics.extend(_validate_inert_value(script.index, f"{script_path}.index", limits))
    diagnostics.extend(_validate_inert_value(script.footer, f"{script_path}.footer", limits))
    diagnostics.extend(_validate_inert_value(script.string_groups, f"{script_path}.string_groups", limits))
    diagnostics.extend(_validate_inert_value(script.string_locations, f"{script_path}.string_locations", limits))
    diagnostics.extend(_validate_inert_value(script.variables, f"{script_path}.variables", limits))
    budget.consume("rawBytes", _raw_byte_count(script.header))
    budget.consume("rawBytes", _raw_byte_count(script.string_garbage))

    for name in counts:
        if name in ("suppressedInstructions",):
            continue
        source_name = name
        counts[name] = budget.values[source_name] - before[source_name]
    return diagnostics, counts


@contextlib.contextmanager
def _legacy_working_directory() -> Iterable[None]:
    original = Path.cwd()
    os.chdir(LEGACY_PYTHON_ROOT)
    try:
        yield
    finally:
        os.chdir(original)


def _encode_script(script: SCTScript, endian: str) -> dict[str, Any]:
    with _legacy_working_directory(), contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
        from SALSA.BaseInstructions.bi_facade import BaseInstLibFacade
        from SALSA.Scripts.script_encoder import SCTEncoder

        base_instructions = BaseInstLibFacade()
        working = copy.deepcopy(script)
        encoder = SCTEncoder(
            script=working,
            base_insts=base_instructions,
            endian=endian,
            update_inst_pos=True,
        )
        encoded = encoder.encode_sct_file(
            use_garbage=True,
            combine_footer_links=False,
            add_spurious_refresh=True,
        )
        advisories = []
        errors = []
        for value in working.errors:
            if not (type(value) is tuple and len(value) >= 4 and value[0] == "Encoding"):
                continue
            recovered_offsets = [
                offset
                for offset, (target, trace) in encoder.string_links.items()
                if value[1] == "String"
                and value[2] == f"No string {target}"
                and value[3] == ":".join(trace)
                and offset in encoder.footer_links
                and encoder.footer_links[offset][0] == target
                and encoder.footer_links[offset][1] == trace
                and encoder.sct_body[offset:offset + len(encoder._placeholder)]
                != encoder._placeholder
            ]
            if recovered_offsets:
                advisories.append({
                    "code": "legacy-footer-header-lookup-recovered",
                    "message": str(value),
                    "trace": value[3],
                    "value": value[2][len("No string "):],
                })
            else:
                errors.append(str(value))
        if errors:
            raise ValueError("; ".join(errors))
        return {
            "status": "accepted",
            "endian": endian,
            "size": len(encoded),
            "sha256": hashlib.sha256(encoded).hexdigest().upper(),
            "advisories": advisories,
        }


def _minimal_report(path: Path, size: int, sha256: str, protocol: int | None) -> dict[str, Any]:
    return {
        "contractVersion": CONTRACT_VERSION,
        "input": {
            "filename": path.name,
            "size": size,
            "sha256": sha256,
            "pickleProtocol": protocol,
            "declaredVersion": None,
        },
        "status": "rejected",
        "project": None,
        "normalizations": [],
        "scripts": [],
        "diagnostics": [],
        "fieldDispositions": FIELD_DISPOSITIONS,
        "limits": None,
    }


def characterize_project(
    input_path: str | Path,
    *,
    endian: str | None = None,
    excluded_scripts: Iterable[str] = (),
    discarded_metadata: Iterable[str] = (),
    limits: CharacterizationLimits | None = None,
    disable_resource_limits: bool = False,
    cancel: Callable[[], bool] | None = None,
) -> tuple[dict[str, Any], int]:
    path = Path(input_path)
    active_limits = limits or CharacterizationLimits()
    size = path.stat().st_size
    if not disable_resource_limits and size > active_limits.max_input_bytes:
        report = _minimal_report(path, size, "", None)
        report["diagnostics"].append(_diagnostic(
            "resource-limit", "error", "project", "input",
            f"input is {size} bytes; maximum is {active_limits.max_input_bytes}"))
        report["status"] = "cancelled"
        report["limits"] = asdict(active_limits)
        return report, 6

    sha256 = _sha256_file(path, cancel)
    with path.open("rb") as stream:
        prefix = stream.read(2)
    protocol = prefix[1] if len(prefix) == 2 and prefix[0] == 0x80 else None
    report = _minimal_report(path, size, sha256, protocol)
    report["limits"] = None if disable_resource_limits else asdict(active_limits)
    if protocol != SUPPORTED_PICKLE_PROTOCOL:
        report["diagnostics"].append(_diagnostic(
            "unsupported-pickle-protocol", "error", "project", "input",
            f"official final v7 projects require pickle protocol {SUPPORTED_PICKLE_PROTOCOL}; found {protocol!r}"))
        return report, 3

    try:
        _preflight_pickle(path, cancel)
        with path.open("rb") as stream:
            project = RestrictedProjectUnpickler(stream).load()
            if stream.read(1):
                raise UnsafePickleError("trailing data after the pickle payload is not allowed")
    except UnsafePickleError as error:
        report["diagnostics"].append(_diagnostic(
            "unsafe-pickle", "error", "project", "input", str(error)))
        return report, 3
    except (pickle.UnpicklingError, EOFError, AttributeError, ValueError, TypeError) as error:
        report["diagnostics"].append(_diagnostic(
            "malformed-pickle", "error", "project", "input", str(error)))
        return report, 3

    version = getattr(project, "version", None)
    report["input"]["declaredVersion"] = version if _is_int(version) else None
    if version != SUPPORTED_PROJECT_VERSION:
        if _is_int(version) and version < SUPPORTED_PROJECT_VERSION:
            message = (
                f"Project schema version {version} is not supported. Open this project in the final "
                "legacy SALSA, save a new copy as version 7, then import the new copy."
            )
        elif _is_int(version):
            message = (
                f"Project schema version {version} is newer than the supported final legacy version 7."
            )
        else:
            message = "The project does not declare a supported schema version."
        report["diagnostics"].append(_diagnostic(
            "unsupported-project-version", "error", "project", "project.version", message))
        return report, 4
    if type(project) is not SCTProject:
        report["diagnostics"].append(_diagnostic(
            "invalid-project-type", "error", "project", "project",
            f"expected SCTProject, found {type(project).__name__}"))
        return report, 5

    project_diagnostics = _check_attributes(project, "project")
    if project_diagnostics:
        report["diagnostics"].extend(project_diagnostics)
        return report, 5
    if type(project.scts) is not dict:
        report["diagnostics"].append(_diagnostic(
            "invalid-script-map", "error", "project", "project.scts",
            "project scripts must be a dictionary"))
        return report, 5

    discarded = set(discarded_metadata)
    if type(project.global_variables) is not dict or set(project.global_variables) != {
        "BitVar", "IntVar", "ByteVar", "FloatVar"
    }:
        diagnostic = _diagnostic(
            "invalid-global-variables", "error", "project", "project.global_variables",
            "global variables do not have the official v7 four-map shape", discardable=True)
        report["diagnostics"].append(diagnostic)
    else:
        for variable_type, values in project.global_variables.items():
            if type(values) is not dict:
                report["diagnostics"].append(_diagnostic(
                    "invalid-global-variable-map", "error", "project",
                    f"project.global_variables.{variable_type}",
                    "variable aliases must be a dictionary", discardable=True))
    if type(project.inst_id_colors) is not dict or any(
        not _is_int(key) or not 0 <= key < 266 or type(value) is not str
        for key, value in getattr(project, "inst_id_colors", {}).items()
    ):
        report["diagnostics"].append(_diagnostic(
            "invalid-instruction-colors", "error", "project", "project.inst_id_colors",
            "instruction colors must map instruction IDs 0 through 265 to strings",
            discardable=True))

    invalid_discard_ids = discarded - {
        diagnostic["id"] for diagnostic in report["diagnostics"] if diagnostic["discardable"]
    }
    for record_id in sorted(invalid_discard_ids):
        report["diagnostics"].append(_diagnostic(
            "unknown-metadata-discard", "error", "project", f"discard[{record_id}]",
            "metadata discard ID does not identify a current discardable diagnostic"))

    budget = _Budget(active_limits, disable_resource_limits)
    excluded = set(excluded_scripts)
    seen_casefolded: dict[str, str] = {}
    script_reports: list[dict[str, Any]] = []
    all_normalizations: list[dict[str, Any]] = []
    for ordinal, (script_key, script) in enumerate(project.scts.items()):
        if cancel is not None and cancel():
            raise CharacterizationCancelled("characterization cancelled")
        if type(script_key) is str:
            folded = script_key.casefold()
            if folded in seen_casefolded:
                report["diagnostics"].append(_diagnostic(
                    "duplicate-script-stem", "error", "project", f"project.scts[{script_key!r}]",
                    f"script stem collides case-insensitively with {seen_casefolded[folded]!r}"))
            else:
                seen_casefolded[folded] = script_key
        normalizations = _normalize_script(script, script_key) if type(script) is SCTScript else []
        all_normalizations.extend(normalizations)
        diagnostics, counts = _validate_script(
            script_key, script, budget, active_limits, cancel)
        for normalization in normalizations:
            if normalization["code"] == "placeholder-normalization-blocked":
                diagnostics.append(_diagnostic(
                    "placeholder-normalization-blocked", "error", "script",
                    normalization["entity"],
                    "final v7 placeholder cleanup could not safely remove a linked placeholder"))
        encoding: dict[str, Any] = {"status": "not-run", "endian": endian}
        if not any(item["severity"] == "error" for item in diagnostics) and endian is not None:
            try:
                encoding = _encode_script(script, endian)
                for advisory in encoding.get("advisories", []):
                    diagnostics.append(_diagnostic(
                        advisory["code"],
                        "warning",
                        "script",
                        f"project.scts[{script_key!r}].{advisory['trace']}",
                        advisory["message"],
                    ))
            except Exception as error:
                encoding = {"status": "failed", "endian": endian, "message": str(error)}
                diagnostics.append(_diagnostic(
                    "legacy-encoder-failure", "error", "script",
                    f"project.scts[{script_key!r}]", str(error)))
        failed = any(item["severity"] == "error" for item in diagnostics)
        if script_key in excluded:
            status = "excluded"
        elif failed:
            status = "failed"
        else:
            status = "accepted"
        script_reports.append({
            "ordinal": ordinal,
            "key": script_key,
            "storedName": getattr(script, "name", None),
            "status": status,
            "counts": counts,
            "encoding": encoding,
            "diagnostics": diagnostics,
        })

    unknown_exclusions = excluded - set(project.scts)
    for script_key in sorted(unknown_exclusions):
        report["diagnostics"].append(_diagnostic(
            "unknown-script-exclusion", "error", "project", f"exclude[{script_key}]",
            "excluded script key is not present in the project"))

    report["normalizations"] = sorted(
        all_normalizations,
        key=lambda value: (str(value["script"]).casefold(), value["entity"], value["code"]),
    )
    report["scripts"] = script_reports
    report["project"] = {
        "scriptCount": len(project.scts),
        "acceptedScriptCount": sum(item["status"] == "accepted" for item in script_reports),
        "excludedScriptCount": sum(item["status"] == "excluded" for item in script_reports),
        "failedScriptCount": sum(item["status"] == "failed" for item in script_reports),
        "globalVariableAliasCount": sum(
            len(values) for values in project.global_variables.values()
            if type(values) is dict
        ) if type(project.global_variables) is dict else 0,
        "instructionColorCount": len(project.inst_id_colors) if type(project.inst_id_colors) is dict else 0,
        "totals": budget.values,
    }

    unresolved_discardable = any(
        diagnostic["discardable"] and diagnostic["id"] not in discarded
        for diagnostic in report["diagnostics"]
    )
    project_errors = any(
        diagnostic["severity"] == "error" and not (
            diagnostic["discardable"] and diagnostic["id"] in discarded
        )
        for diagnostic in report["diagnostics"]
    )
    script_failures = any(item["status"] == "failed" for item in script_reports)
    if unresolved_discardable or project_errors or script_failures:
        report["status"] = "action-required"
        return report, 5
    report["status"] = "accepted"
    return report, 0


def write_json_atomic(path: str | Path, value: object) -> None:
    destination = Path(path)
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_name(destination.name + ".tmp")
    try:
        with temporary.open("w", encoding="utf-8", newline="\n") as stream:
            json.dump(value, stream, indent=2, sort_keys=True, ensure_ascii=False)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, destination)
    finally:
        if temporary.exists():
            temporary.unlink()
