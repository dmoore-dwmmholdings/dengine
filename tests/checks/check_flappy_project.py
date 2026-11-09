#!/usr/bin/env python3
"""DIDE-242: validate the committed example's project contract without running a game."""
import copy
import json
from pathlib import Path
import sys
import unittest

try:
    import tomllib
except ModuleNotFoundError:
    print("flappy_project: SKIPPED (Python 3.11 tomllib required)")
    sys.exit(77)

ROOT = Path(__file__).resolve().parents[2]
PROJECT = ROOT / "examples/flappy/Flappy.dproj"


def require(condition, message):
    if not condition:
        raise ValueError(message)


def validate(project, presets):
    directory = PROJECT.parent
    require((directory / project["project"]["root"]).resolve() == ROOT, "workspace root must be engine checkout")
    require(project["project"]["engine"] == "dengine", "engine project kind")
    require(project["build"]["target"] == "flappy", "Flappy build target")
    require(project["build"]["default_configuration"] == "Dev", "default Dev configuration")
    configurations = project["configuration"]
    require([c["name"] for c in configurations] == ["Debug", "Dev", "Release", "Shipping"], "four build configurations")
    available = {p["name"] for p in presets["configurePresets"]}
    for configuration in configurations:
        mode = configuration["name"].lower()
        expected = {"windows": f"windows-clang-cl-{mode}", "macos": f"macos-{mode}", "linux": f"linux-clang-{mode}"}
        require(configuration["preset"] == expected, f"platform presets for {mode}")
        require(all(p in available for p in expected.values()), f"presets exist for {mode}")
    runs = project["run"]
    require(len(runs) == 2, "two run targets")
    session = next((r for r in runs if r["name"] == "Flappy (engine session)"), None)
    require(session is not None and session.get("kind") == "engine" and session.get("target") == "flappy", "engine session run")
    require(session.get("configurations") == ["Dev"], "editor service requires Dev")
    native = next((r for r in runs if r["name"] == "Flappy"), None)
    require(native is not None and native.get("kind", "native") == "native" and native.get("target") == "flappy", "native Flappy run")
    folders = {folder["name"]: folder for folder in project["folder"]}
    require({"Source", "Config", "Docs"} <= folders.keys(), "required virtual folders")
    for folder in folders.values():
        for item in folder.get("files", []):
            path = item["path"] if isinstance(item, dict) else item
            require((directory / path).is_file(), f"missing project entry: {path}")
    docs = folders["Docs"]["files"]
    require({"path": "README.md", "open": "preview"} in docs, "README preview intent")
    require(project["open"].get("*.md") == "preview", "Markdown open rule")
    startup = project["startup"]
    require(startup["run"] == "Flappy (engine session)" and startup["lead"] == "world", "startup run and lead")
    require("main.c" in startup["open"] and "flappy.scene" in startup["open"], "startup source and scene")
    require(all((directory / p).is_file() for p in startup["open"]), "startup files exist")


def inputs():
    return tomllib.loads(PROJECT.read_text(encoding="utf-8")), json.loads((ROOT / "CMakePresets.json").read_text(encoding="utf-8"))


class ContractTests(unittest.TestCase):
    def test_actual_project(self):
        validate(*inputs())

    def rejects(self, change):
        project, presets = inputs()
        project = copy.deepcopy(project)
        change(project, presets)
        with self.assertRaises(ValueError):
            validate(project, presets)

    def test_wrong_workspace_root(self):
        self.rejects(lambda p, _: p["project"].update(root="."))

    def test_wrong_platform_preset(self):
        self.rejects(lambda p, _: p["configuration"][0]["preset"].update(windows="macos-debug"))

    def test_removed_engine_preset(self):
        self.rejects(lambda _, p: p.update(configurePresets=[x for x in p["configurePresets"] if x["name"] != "macos-dev"]))

    def test_missing_source(self):
        self.rejects(lambda p, _: p["folder"][0]["files"].append("missing-fixture-source.c"))

    def test_unavailable_session_configuration(self):
        self.rejects(lambda p, _: p["run"][0].update(configurations=["Shipping"]))

    def test_missing_native_target(self):
        self.rejects(lambda p, _: p["run"][1].update(target="missing-game"))

    def test_removed_source_folder(self):
        self.rejects(lambda p, _: p.update(folder=[f for f in p["folder"] if f["name"] != "Source"]))


if __name__ == "__main__":
    if sys.argv[1:] == ["--self-test"]:
        unittest.main(argv=[sys.argv[0]])
    elif len(sys.argv) == 1:
        validate(*inputs())
        print("flappy_project: configuration, run, file and startup contracts passed")
    else:
        raise SystemExit("usage: check_flappy_project.py [--self-test]")
