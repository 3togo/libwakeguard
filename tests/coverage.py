# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libwakeguard contributors
"""Run GCC/gcov line coverage, merging the independent fault-injection targets.

Requires the same Python GObject and D-Bus tools as the integration suite.
This measures lines, not branches or native OS/power-policy correctness.
"""
import gzip
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / 'build-coverage'


def run(arguments, **kwargs):
    subprocess.run(arguments, check=True, **kwargs)


run(['cmake', '-S', str(ROOT), '-B', str(BUILD), '-DWAKEGUARD_BUILD_TESTS=ON',
     '-DWAKEGUARD_REQUIRE_DBUS_TESTS=ON', '-DWAKEGUARD_TEST_PYTHON=' + sys.executable,
     '-DCMAKE_BUILD_TYPE=Debug',
     '-DCMAKE_CXX_FLAGS=--coverage -O0 -fprofile-update=atomic',
     '-DCMAKE_C_FLAGS=--coverage -O0 -fprofile-update=atomic',
     '-DCMAKE_EXE_LINKER_FLAGS=--coverage'])
run(['cmake', '--build', str(BUILD), '--parallel', '4'])
# Drop only data emitted into this script's build directory, so stale counters
# cannot distort results after changes to code or compiler instrumentation.
for counters in BUILD.rglob('*.gcda'):
    counters.unlink()
run(['ctest', '--test-dir', str(BUILD), '--output-on-failure'])
output_dir = BUILD / 'gcov'
output_dir.mkdir(exist_ok=True)
covered = {}
for notes in sorted(BUILD.rglob('*.gcno')):
    if notes.parent.name != 'src':
        continue
    result = subprocess.run(['gcov', '--json-format', str(notes)], cwd=output_dir,
        capture_output=True, text=True, check=True)
    # A gcov profile mismatch can exit successfully but invalidate counts.
    if result.stderr:
        raise RuntimeError(result.stderr)
    for output in output_dir.glob('*.gcov.json.gz'):
        data = json.loads(gzip.decompress(output.read_bytes()))
        for entry in data['files']:
            filename = Path(entry['file'])
            if filename.parent != ROOT / 'src':
                continue
            lines = covered.setdefault(filename.name, {})
            for line in entry['lines']:
                number = line['line_number']
                lines[number] = lines.get(number, 0) + line['count']
        output.unlink()
report = {}
for filename, lines in sorted(covered.items()):
    missed = sorted(number for number, count in lines.items() if not count)
    report[filename] = {'hit': len(lines) - len(missed), 'lines': len(lines), 'missed': missed}
    print(f'{filename}: {len(lines) - len(missed)}/{len(lines)} lines; missed: {missed}')
(BUILD / 'coverage-summary.json').write_text(json.dumps(report, indent=2) + '\n')
