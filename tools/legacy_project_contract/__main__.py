from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from .contract import (
    CharacterizationCancelled,
    CharacterizationLimits,
    ResourceLimitError,
    characterize_project,
    write_json_atomic,
)


def _common_options(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--endian", choices=("little", "big"))
    parser.add_argument("--exclude-script", action="append", default=[])
    parser.add_argument("--discard-metadata", action="append", default=[])
    parser.add_argument("--disable-resource-limits", action="store_true")


def _characterize(path: Path, args: argparse.Namespace) -> tuple[dict, int]:
    return characterize_project(
        path,
        endian=args.endian,
        excluded_scripts=args.exclude_script,
        discarded_metadata=args.discard_metadata,
        limits=CharacterizationLimits(),
        disable_resource_limits=args.disable_resource_limits,
    )


def _project_files(directory: Path) -> list[Path]:
    return sorted(directory.glob("*.prj"), key=lambda path: path.name.casefold())


def _run_inspect(args: argparse.Namespace) -> int:
    report, exit_code = _characterize(args.project, args)
    if args.output:
        write_json_atomic(args.output, report)
    else:
        json.dump(report, sys.stdout, indent=2, sort_keys=True, ensure_ascii=False)
        sys.stdout.write("\n")
    return exit_code


def _corpus_report(args: argparse.Namespace) -> tuple[dict, int]:
    entries = []
    overall = 0
    for path in _project_files(args.directory):
        report, exit_code = _characterize(path, args)
        entries.append({"filename": path.name, "exitCode": exit_code, "report": report})
        if exit_code not in (0, 4):
            overall = exit_code
    return {"projects": entries}, overall


def _run_inspect_corpus(args: argparse.Namespace) -> int:
    report, exit_code = _corpus_report(args)
    write_json_atomic(args.output, report)
    return exit_code


def _run_verify_corpus(args: argparse.Namespace) -> int:
    report, _ = _corpus_report(args)
    try:
        with args.expected.open("r", encoding="utf-8") as stream:
            expected = json.load(stream)
    except (OSError, json.JSONDecodeError) as error:
        print(f"unable to read expectations: {error}", file=sys.stderr)
        return 2
    if report != expected:
        print("private corpus differs from its expected characterization", file=sys.stderr)
        return 5
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="legacy-project-contract",
        description="Characterize official final legacy SALSA version-7 projects.",
    )
    commands = parser.add_subparsers(dest="command", required=True)

    inspect = commands.add_parser("inspect", help="characterize one project")
    inspect.add_argument("project", type=Path)
    inspect.add_argument("--output", type=Path)
    _common_options(inspect)
    inspect.set_defaults(handler=_run_inspect)

    inspect_corpus = commands.add_parser(
        "inspect-corpus", help="write a deterministic private-corpus expectation file")
    inspect_corpus.add_argument("directory", type=Path)
    inspect_corpus.add_argument("--output", type=Path, required=True)
    _common_options(inspect_corpus)
    inspect_corpus.set_defaults(handler=_run_inspect_corpus)

    verify_corpus = commands.add_parser(
        "verify-corpus", help="compare the private corpus with reviewed expectations")
    verify_corpus.add_argument("directory", type=Path)
    verify_corpus.add_argument("--expected", type=Path, required=True)
    _common_options(verify_corpus)
    verify_corpus.set_defaults(handler=_run_verify_corpus)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return args.handler(args)
    except FileNotFoundError as error:
        print(str(error), file=sys.stderr)
        return 2
    except PermissionError as error:
        print(str(error), file=sys.stderr)
        return 2
    except CharacterizationCancelled:
        print("characterization cancelled", file=sys.stderr)
        return 6
    except ResourceLimitError as error:
        print(str(error), file=sys.stderr)
        return 6
    except KeyboardInterrupt:
        print("characterization cancelled", file=sys.stderr)
        return 6


if __name__ == "__main__":
    raise SystemExit(main())
