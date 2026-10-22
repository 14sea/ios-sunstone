#!/usr/bin/env python3
"""Check script syntax, local documentation links and active patch syntax.

  python3 tests/check-project.py [--source-root ../work/xnu]

With --source-root, replay all active patches on that checkout's original XNU
commit and compare every affected file. The three upstream providers normally
staged by CI's shell hooks must already exist there. Their patched files are
reversed first, so this checks patch consistency without downloading anything.
It does not replace fetching the pinned providers, a kernel build, or device tests.
"""
import argparse
import io
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PROVIDERS = ('bsd/pthread/sweet_libpthread', 'libkern/crypto/sweet_bearssl', 'bsd/sweet_hfs')


def git(*args, cwd=ROOT):
    return subprocess.check_output(['git', *args], cwd=cwd)


def scripts_and_docs():
    paths = git('ls-files', '--cached', '--others', '--exclude-standard', '-z').decode().split('\0')
    counts = {'Python': 0, 'shell': 0, 'plist': 0, 'Markdown': 0}
    for name in sorted(set(filter(None, paths))):
        path = ROOT / name
        if not path.is_file():
            continue
        if path.suffix == '.py':
            compile(path.read_bytes(), name, 'exec')
            counts['Python'] += 1
        elif path.suffix == '.sh':
            interpreter = 'sh' if path.read_text().splitlines()[0] == '#!/bin/sh' else 'bash'
            subprocess.run([interpreter, '-n', str(path)], check=True)
            counts['shell'] += 1
        elif path.suffix == '.plist':
            plistlib.loads(path.read_bytes())
            counts['plist'] += 1
        elif path.suffix == '.md':
            for target in re.findall(r'(?<!!)\[[^\]]+\]\(([^)]+)\)', path.read_text()):
                if '://' in target or target.startswith(('#', 'mailto:')):
                    continue
                if not (path.parent / target.split('#')[0]).exists():
                    raise ValueError(f'{name}: missing local link target: {target}')
            counts['Markdown'] += 1
    print('Syntax/links:', ', '.join(f'{count} {kind}' for kind, count in counts.items()), flush=True)


def check_patches(source):
    patches = sorted((ROOT / 'xnu/patches').glob('*.patch'))
    paths = set()
    for patch in patches:
        for line in git('apply', '--numstat', str(patch)).decode().splitlines():
            paths.add(line.split('\t', 2)[2])
    print(f'Active series: {len(patches)} valid patches, {len(paths)} affected files', flush=True)
    if source is None:
        return
    source = source.resolve()
    base = git('rev-list', '--max-parents=0', 'HEAD', cwd=source).decode().strip()
    tracked = set(git('ls-tree', '-r', '--name-only', base, cwd=source).decode().splitlines())
    provider_paths = sorted(p for p in paths if any(p.startswith(d + '/') for d in PROVIDERS))
    with tempfile.TemporaryDirectory(prefix='sunstone-series-') as directory:
        stage = Path(directory)
        archive = git('archive', base, '--', *sorted(paths & tracked), cwd=source)
        with tarfile.open(fileobj=io.BytesIO(archive)) as tree:
            # Paths are selected from the local Git tree, not a downloaded tarball.
            if hasattr(tarfile, 'data_filter'):
                tree.extractall(stage, filter='data')
            else:
                tree.extractall(stage)
        for provider in PROVIDERS:
            if not (source / provider).is_dir():
                raise ValueError(f'missing staged provider: {source / provider}')
            shutil.copytree(source / provider, stage / provider)
        # The local providers already contain project patches. Recover hook output.
        for patch in reversed(patches):
            git('apply', '-R', *['--include=' + p for p in provider_paths], str(patch), cwd=stage)
        for patch in patches:
            git('apply', str(patch), cwd=stage)
        differences = []
        for name in sorted(paths):
            actual, rebuilt = source / name, stage / name
            if actual.exists() != rebuilt.exists() or (
                    actual.exists() and actual.read_bytes() != rebuilt.read_bytes()):
                differences.append(name)
        if differences:
            raise ValueError('patch/source differences:\n' + '\n'.join(differences))
    print(f'Full replay matches all {len(paths)} files (XNU base {base[:12]})', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path)
    args = parser.parse_args()
    scripts_and_docs()
    check_patches(args.source_root)


if __name__ == '__main__':
    main()
