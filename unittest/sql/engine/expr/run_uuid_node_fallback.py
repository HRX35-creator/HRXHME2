"""Compile exact UUID production method bodies with small dependency adapters.

Linux syscall wrappers inject failures. This focused test does not compile the
SQL engine, the native ObLatch implementation, or a complete seekdb server.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import tempfile
from pathlib import Path


def block(source: str, start: str, end: str) -> str:
    first = source.index(start)
    last = source.index(end, first)
    return source[first:last]


def function(source: str, name: str) -> str:
    start = source.index(name)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    # These selected methods contain no braces in strings or block comments.
    while depth:
        if source[end] == "{":
            depth += 1
        elif source[end] == "}":
            depth -= 1
        end += 1
    return source[start:end] + "\n"


def generate(source: str, header: str) -> tuple[str, list[dict]]:
    declarations = []
    for name in ["class ObBigEndian", "struct ObUUIDNode", "class ObUUIDTime", "class UuidCommon"]:
        first = header.index(name)
        end = header.index("\n};", first) + len("\n};")
        declarations.append(header[first:end].replace("private:", "public:").replace("private :", "public :"))
    declarations.append("""class ObExprUuid {
public:
  static int calc(unsigned char *scratch);
  static int gen_server_uuid(char *server_uuid, const int64_t uuid_len);
  static ObUUIDNode *uuid_node;
};""")
    pieces = [
        ("ObBigEndian::put_uint16", function(source, "int ObBigEndian::put_uint16")),
        ("ObBigEndian::put_uint32", function(source, "int ObBigEndian::put_uint32")),
        ("ObUUIDNode constructor/helper/init", block(source, "ObUUIDNode::ObUUIDNode()", "int ObUUIDTime::get_time")),
        ("ObUUIDTime::get_time", function(source, "int ObUUIDTime::get_time")),
        ("ObUUIDTime::time_now", function(source, "int ObUUIDTime::time_now")),
        ("ObUUIDTime::reset_clock_seq", function(source, "void ObUUIDTime::reset_clock_seq")),
        ("UuidCommon::bin2uuid", function(source, "int UuidCommon::bin2uuid")),
        ("ObExprUuid::calc", function(source, "int ObExprUuid::calc(")),
        ("static generator state", block(source, "uint64_t ObUUIDTime::lasttime_", "int ObExprUuid::eval_uuid")),
        ("ObExprUuid::gen_server_uuid", function(source, "int ObExprUuid::gen_server_uuid")),
    ]
    generated = "namespace oceanbase { namespace sql {\n" + "\n".join(declarations)
    for _, text in pieces:
        generated += "\n" + text
    generated += "\n}}\n"
    inventory = [{"production_body": name, "sha256": hashlib.sha256(text.encode()).hexdigest()} for name, text in pieces]
    return generated, inventory


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=Path(__file__).resolve().parents[4])
    parser.add_argument("--header-root", type=Path)
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    args = parser.parse_args()
    source_path = args.source_root / "src/sql/engine/expr/ob_expr_uuid.cpp"
    header_path = (args.header_root or args.source_root) / "src/sql/engine/expr/ob_expr_uuid.h"
    source = source_path.read_text(encoding="utf-8")
    header = header_path.read_text(encoding="utf-8")
    generated, inventory = generate(source, header)
    temp = tempfile.TemporaryDirectory(prefix="seekdb-uuid-1480-") if args.build_dir is None else None
    build = (args.build_dir or Path(temp.name)).resolve()
    build.mkdir(parents=True, exist_ok=True)
    include = build / "uuid_production.inc"
    include.write_text(generated, encoding="utf-8")
    exe = build / "uuid_node_fallback_test"
    command = [args.cxx, "-std=c++17", "-O0", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-address", "-pthread",
               "-I" + str(build), str(Path(__file__).with_name("test_uuid_node_fallback.cpp")),
               "-Wl,--wrap=socket,--wrap=ioctl,--wrap=getrandom,--wrap=close,--wrap=clock_gettime", "-o", str(exe)]
    compilation = subprocess.run(command, text=True, capture_output=True, timeout=60)
    (build / "compile.log").write_text(compilation.stdout + compilation.stderr, encoding="utf-8")
    record = {
        "source_sha256": hashlib.sha256(source_path.read_bytes()).hexdigest(),
        "header_sha256": hashlib.sha256(header_path.read_bytes()).hexdigest(),
        "compile_command": command, "compile_exit": compilation.returncode,
        "production_bodies": inventory,
        "scope": "exact production UUID method bodies; test adapters for dependency types/logging/allocator/lock",
        "full_seekdb_build": False, "rebuilt_server_sql_test": False,
    }
    if compilation.returncode:
        print(compilation.stdout + compilation.stderr)
        (build / "result.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
        return compilation.returncode
    execution = subprocess.run([str(exe)], text=True, capture_output=True, timeout=30)
    print(execution.stdout + execution.stderr, end="")
    (build / "test.log").write_text(execution.stdout + execution.stderr, encoding="utf-8")
    record["test_exit"] = execution.returncode
    record["summary"] = re.findall(r"SUMMARY.*", execution.stdout)
    if execution.returncode == 0:
        nodes = []
        for _ in range(8):
            child = subprocess.run([str(exe), "--node-only"], text=True, capture_output=True, timeout=5)
            if child.returncode or not re.fullmatch(r"[0-9a-f]{12}\n", child.stdout):
                raise RuntimeError("Independent process node generation failed: " + child.stdout + child.stderr)
            nodes.append(child.stdout.strip())
        assert len(set(nodes)) == 8
        assert all(int(node[:2], 16) & 1 for node in nodes)
        record["independent_processes_real_entropy"] = {"count": 8, "unique_nodes": 8, "multicast": True}
        print("PASS 8 independent process initializations with real entropy")
    (build / "result.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    return execution.returncode


if __name__ == "__main__":
    raise SystemExit(main())
