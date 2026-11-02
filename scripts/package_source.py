#!/usr/bin/env python3
"""Package committed source, including pinned submodules, without Git metadata or runtime files."""
import argparse
import hashlib
import json
import subprocess
import zipfile
from pathlib import Path


def git(repo, *args):
    """Read one repository without printing credential-bearing configuration."""
    return subprocess.check_output(['git', '-C', str(repo), *args])


def repositories(root):
    """Walk committed gitlinks and require matching initialized working checkouts."""
    pending = [(root, '')]
    while pending:
        repo, prefix = pending.pop(0)
        commit = git(repo, 'rev-parse', 'HEAD').decode().strip()
        entries = []
        for record in git(repo, 'ls-tree', '-rz', 'HEAD').split(b'\0'):
            if not record:
                continue
            metadata, name = record.split(b'\t', 1)
            mode, kind, oid = metadata.decode().split()
            name = name.decode('utf-8')
            if Path(name).is_absolute() or '..' in Path(name).parts or '\\' in name:
                raise RuntimeError('Unsafe repository path')
            if kind == 'commit':
                child = repo / name
                if git(child, 'rev-parse', 'HEAD').decode().strip() != oid:
                    raise RuntimeError(f'Submodule differs from pinned commit: {prefix}{name}')
                pending.append((child, prefix + name + '/'))
            elif kind == 'blob':
                entries.append((mode, oid, prefix + name))
        yield repo, prefix, commit, entries


def package(root, output):
    """Write exact committed blobs, retaining executable and symlink modes."""
    output.parent.mkdir(parents=True, exist_ok=True)
    manifest = {'repositories': [], 'files': {}}
    with zipfile.ZipFile(output, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for repo, prefix, commit, entries in repositories(root):
            manifest['repositories'].append({'path': prefix.rstrip('/') or '.', 'commit': commit})
            process = subprocess.Popen(['git', '-C', str(repo), 'cat-file', '--batch'], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
            try:
                for mode, oid, name in entries:
                    process.stdin.write((oid + '\n').encode())
                    process.stdin.flush()
                    returned, kind, length = process.stdout.readline().split()
                    if returned.decode() != oid or kind != b'blob':
                        raise RuntimeError('Unexpected Git object')
                    data = process.stdout.read(int(length))
                    if len(data) != int(length) or process.stdout.read(1) != b'\n':
                        raise RuntimeError('Truncated Git object')
                    item = zipfile.ZipInfo('bpsec-bplib-integration/' + name, date_time=(2026, 11, 2, 0, 0, 0))
                    item.create_system = 3
                    item.external_attr = int(mode, 8) << 16
                    item.compress_type = zipfile.ZIP_DEFLATED
                    archive.writestr(item, data)
                    manifest['files'][name] = hashlib.sha256(data).hexdigest()
            except BaseException:
                process.kill()
                raise
            finally:
                process.stdin.close()
                process.stdout.close()
                status = process.wait()
            if status != 0:
                raise RuntimeError('Git object export failed')
        item = zipfile.ZipInfo('bpsec-bplib-integration/SOURCE_MANIFEST.json', date_time=(2026, 11, 2, 0, 0, 0))
        item.create_system = 3
        item.external_attr = 0o100644 << 16
        item.compress_type = zipfile.ZIP_DEFLATED
        archive.writestr(item, json.dumps(manifest, indent=2) + '\n')
    print(f'Packaged {len(manifest["files"])} committed files from {len(manifest["repositories"])} repositories: {output}')


def main():
    """Parse the publication checkout and output archive paths."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--output', type=Path, default=Path('dist/bpsec-bplib-integration-complete-source.zip'))
    args = parser.parse_args()
    package(args.root.resolve(), args.output.resolve())


if __name__ == '__main__':
    main()
