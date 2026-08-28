#!/usr/bin/env python3
"""Generate the deterministic source mirror SHA256 manifest without Git."""

from __future__ import annotations

import argparse
import hashlib
import pathlib


ROOT_FILES = {
    ".gitignore",
    "CMakeLists.txt",
    "INDEX.md",
    "README.md",
}
SOURCE_DIRECTORIES = {
    "apps",
    "config",
    "doc",
    "docs",
    "include",
    "modules",
    "scripts",
    "src",
    "tests",
    "tools",
}
EXCLUDED_PARTS = {"__pycache__", "tmp"}
EXCLUDED_NAMES = {".DS_Store"}


def source_paths(root: pathlib.Path, output: pathlib.Path) -> list[pathlib.Path]:
    root = root.resolve()
    output = output.resolve()
    paths = []
    for path in root.rglob("*"):
        relative = path.relative_to(root)
        in_source_scope = (
            (len(relative.parts) == 1 and relative.name in ROOT_FILES)
            or (
                len(relative.parts) > 1
                and relative.parts[0] in SOURCE_DIRECTORIES
                and not any(
                    part in EXCLUDED_PARTS or part.startswith("build")
                    for part in relative.parts
                )
            )
        )
        if path.is_symlink() and in_source_scope:
            raise ValueError("source tree contains symlink: %s" % relative)
        if (
            not path.is_file()
            or path.resolve() == output
            or path.name in EXCLUDED_NAMES
        ):
            continue
        if len(relative.parts) == 1:
            if relative.name in ROOT_FILES:
                paths.append(path)
            continue
        if relative.parts[0] not in SOURCE_DIRECTORIES:
            continue
        if any(part in EXCLUDED_PARTS or part.startswith("build")
               for part in relative.parts):
            continue
        paths.append(path)
    return sorted(paths, key=lambda item: item.relative_to(root).as_posix())


def generate(root: pathlib.Path, output: pathlib.Path) -> None:
    root = root.resolve()
    lines = []
    for path in source_paths(root, output):
        relative = path.relative_to(root).as_posix()
        lines.append(
            f"{hashlib.sha256(path.read_bytes()).hexdigest()}  ./{relative}\n"
        )
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix(output.suffix + ".tmp")
    temporary.write_text("".join(lines))
    temporary.replace(output)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=pathlib.Path, default=pathlib.Path("."))
    parser.add_argument(
        "--output",
        type=pathlib.Path,
        default=pathlib.Path(
            "config/testing/atfp-throughput-source-manifest.sha256"
        ),
    )
    args = parser.parse_args()
    output = args.output
    if not output.is_absolute():
        output = args.root / output
    generate(args.root, output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
