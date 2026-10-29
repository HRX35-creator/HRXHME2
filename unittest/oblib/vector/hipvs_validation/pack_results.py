"""Create a local evidence archive with checksums. No upload and no broad directory scan."""
import argparse
import hashlib
import json
import tarfile
from pathlib import Path


def pack(run_dir, output):
    root = Path(run_dir).resolve(strict=True)
    output = Path(output).resolve()
    if output.exists() or output.is_relative_to(root):
        raise ValueError("archive must be new and outside the input directory")
    allowed = {".json", ".jsonl", ".csv", ".log", ".txt", ".sql", ".png"}
    entries = []
    for path in sorted(root.rglob("*")):
        if path.is_symlink():
            raise ValueError("symlinks are not permitted in evidence archives")
        if path.is_file() and path.suffix.lower() in allowed and not any(part.startswith(".") for part in path.relative_to(root).parts):
            if not path.resolve().is_relative_to(root):
                raise ValueError("file escapes evidence directory")
            entries.append(path)
    output.parent.mkdir(parents=True, exist_ok=True)
    checksums = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in entries}
    with tarfile.open(output, "w:gz") as tar:
        for path in entries:
            tar.add(path, arcname="evidence/" + path.relative_to(root).as_posix(), recursive=False)
    checkpath = Path(str(output) + ".sha256.json")
    checkpath.write_text(json.dumps({"archive_sha256": hashlib.sha256(output.read_bytes()).hexdigest(), "files": checksums}, indent=2), encoding="utf-8")
    return checksums


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("run_dir")
    parser.add_argument("output")
    args = parser.parse_args()
    print("Packed", len(pack(args.run_dir, args.output)), "files. Download archive AND its .sha256.json before releasing the instance.")
