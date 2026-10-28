#!/usr/bin/env python3
"""Link and run the actual seekdb string-diff batch decoder test."""
import argparse
import resource
import shlex
import subprocess
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser()
parser.add_argument('build_dir', type=Path)
build = parser.parse_args().build_dir.resolve()
suite = Path(__file__).resolve().parent
flags = {}
for line in (build / 'src/storage/CMakeFiles/ob_storage.dir/flags.make').read_text().splitlines():
    if ' = ' in line:
        key, value = line.split(' = ', 1)
        flags[key] = shlex.split(value)
options = flags['CXX_DEFINES'] + flags['CXX_INCLUDES'] + flags['CXX_FLAGS']
link = shlex.split((build / 'src/observer/CMakeFiles/seekdb.dir/link.txt').read_text())
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
with tempfile.TemporaryDirectory(prefix='fts-string-decoder-') as folder:
    folder = Path(folder)
    obj = folder / 'test.o'
    subprocess.run([link[0], *options, '-c', str(suite / 'test_string_diff_batch.cpp'),
                    '-o', str(obj)], cwd=root, check=True)
    link = [item for item in link if not item.endswith(('CMakeFiles/ob_main.dir/main.cpp.o',
                                                        'CMakeFiles/ob_main.dir/ob_command_line_parser.cpp.o'))]
    link.insert(1, str(obj))
    binary = folder / 'decoder_test'
    link[link.index('-o') + 1] = str(binary)
    subprocess.run(link, cwd=build / 'src/observer', check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
    print(result.stdout, result.stderr, end='', flush=True)
    raise SystemExit(result.returncode)
