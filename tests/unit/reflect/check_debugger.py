"""Check actual generated formats and, when requested, their native LLDB behavior."""
import argparse
import ast
from pathlib import Path
import subprocess
import shutil
import tempfile
import xml.etree.ElementTree as ET


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--folder", type=Path, required=True)
    parser.add_argument("--generator", type=Path, required=True)
    parser.add_argument("--fixture", type=Path)
    parser.add_argument("--lldb", type=Path)
    args = parser.parse_args()
    xml = args.folder / "de_generated_test_game.natvis"
    py = args.folder / "de_generated_test_game.py"
    root = ET.fromstring(xml.read_bytes())
    ns = {"n": "http://schemas.microsoft.com/vstudio/debugger/natvis/2010"}
    all_types = root.findall("n:Type", ns)
    types = {t.attrib["Name"]: t for t in all_types}
    assert len(types) == len(all_types), "Duplicate Natvis types"
    assert "de_vec3" in types and "DebugHealth" in types
    fields = [t.attrib["Name"] for t in types["DebugHealth"].findall("n:Expand/n:Item", ns)]
    assert fields == ["current", "shield", "position", "inventory"], fields
    tree = ast.parse(py.read_text())
    table = next(ast.literal_eval(n.value) for n in tree.body if isinstance(n, ast.Assign)
                 and any(isinstance(t, ast.Name) and t.id == "TYPES" for t in n.targets))
    assert table["de_vec3"] == (12, ["x", "y", "z"])
    assert table["DebugHealth"][1] == fields
    assert list(table) == sorted(table)
    with tempfile.TemporaryDirectory(prefix="deng586 café space ") as temp:
        x, p = Path(temp) / "types.natvis", Path(temp) / "types.py"
        subprocess.run([args.generator, x, p], check=True)
        assert x.read_bytes() == xml.read_bytes() and p.read_bytes() == py.read_bytes()
        refused = subprocess.run([args.generator, x, p], capture_output=True)
        assert refused.returncode != 0
        assert x.read_bytes() == xml.read_bytes() and p.read_bytes() == py.read_bytes()
    if args.lldb:
        assert args.fixture
        commands = [
            "settings set target.disable-aslr false",
            'command script import "%s"' % py.resolve().as_posix(),
            'target create "%s"' % args.fixture.resolve().as_posix(),
            "breakpoint set --name debug_stop", "run",
            "target variable debug_position debug_rotation debug_matrix debug_health debug_text debug_entity",
            "script t=lldb.debugger.GetSelectedTarget(); "
            "v=lambda n:t.FindFirstGlobalVariable(n).GetSummary(); "
            "assert all(s in v('debug_position') for s in ('x=1', 'y=2', 'z=3')), v('debug_position'); "
            "assert 'w=1' in v('debug_rotation'), v('debug_rotation'); "
            "assert '(1, 0, 0, 4)' in v('debug_matrix'), v('debug_matrix'); "
            "assert all(s in v('debug_health') for s in ('current=75', 'shield=25', 'position=')), v('debug_health'); "
            "assert 'café' in v('debug_text') and r'\\n' in v('debug_text'), v('debug_text'); "
            "assert v('debug_entity')=='entity 7 gen 2', v('debug_entity'); "
            "assert '3 elements' in v('debug_array') and '3 elements' in v('debug_map'); "
            "assert v('debug_invalid')=='<invalid string>'; "
            "assert v('debug_long').endswith('…') and len(v('debug_long'))<20000; "
            "print('DENG586_LLDB_OK')",
            "process kill",
        ]
        with tempfile.TemporaryDirectory(prefix="deng586-lldb-") as temp:
            # The fixture is self-contained. Launch it from test scratch, without requiring
            # the debuggee to access a Documents checkout during dyld startup on macOS.
            executable = Path(temp) / args.fixture.name
            shutil.copy2(args.fixture, executable)
            commands[2] = 'target create "%s"' % executable.as_posix()
            file = Path(temp) / "commands.lldb"
            file.write_text("\n".join(commands) + "\n", encoding="utf-8")
            run = subprocess.run([args.lldb, "--batch", "--no-lldbinit", "-s", file],
                                 capture_output=True, text=True, timeout=60)
        print(run.stdout)
        print(run.stderr)
        assert run.returncode == 0 and "DENG586_LLDB_OK\n" in run.stdout
    print("Generated reflection visualizers verified")


if __name__ == "__main__":
    main()
