"""Read-only environment capture. Does not install, build, start or stop anything."""
import argparse
import platform
import shutil
import subprocess
from pathlib import Path
from datetime import datetime, timezone

from metrics import write_json


def capture(argv, timeout=25):
    try:
        p = subprocess.run(argv, capture_output=True, text=True, errors="replace", timeout=timeout)
        return {"command": argv, "returncode": p.returncode, "stdout": p.stdout[-40000:], "stderr": p.stderr[-8000:]}
    except (OSError, subprocess.TimeoutExpired) as exc:
        return {"command": argv, "unavailable": str(exc)}


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--home", default="/seekdb_workspace")
    p.add_argument("--source", help="optional source checkout path")
    p.add_argument("--out", default="runs/preflight.json")
    a = p.parse_args()
    report = {"utc": datetime.now(timezone.utc).isoformat(), "platform": platform.platform(),
              "python": platform.python_version(), "checks": {}, "rocm_version_files": {},
              "note": "Library linkage is not proof that GPU construction actually executed."}
    for name, args in {"cpu": ["lscpu"], "memory": ["free", "-h"], "disk": ["df", "-h"],
                       "gpu": ["amd-smi", "static"], "gpu_memory": ["amd-smi", "metric", "--mem-usage"],
                       "rocm_agents": ["rocminfo"], "compiler": ["hipcc", "--version"],
                       "cxx": ["c++", "--version"], "cmake": ["cmake", "--version"]}.items():
        executable = shutil.which(args[0])
        report["checks"][name] = capture([executable, *args[1:]]) if executable else {"unavailable": args[0]}
    for name in ("/opt/rocm/.info/version", "/opt/rocm/.info/version-dev"):
        path = Path(name)
        if path.is_file():
            report["rocm_version_files"][name] = path.read_text(errors="replace")[:2000]
    root = Path(a.home)
    report["assets"] = {rel: (root / rel).exists() for rel in ("bin/seekdb", "bridge/libseekdb_cuvs_bridge.so", "scripts", "datasets")}
    for rel in ("bin/seekdb", "bridge/libseekdb_cuvs_bridge.so"):
        if (root / rel).is_file() and shutil.which("ldd"):
            report["checks"][rel] = capture(["ldd", str(root / rel)])
    if a.source:
        report["source_commit"] = capture(["git", "-C", a.source, "rev-parse", "HEAD"])
        report["source_changes"] = capture(["git", "-C", a.source, "status", "--short"])
    write_json(a.out, report)
    print("Environment evidence saved:", a.out)


if __name__ == "__main__":
    main()
