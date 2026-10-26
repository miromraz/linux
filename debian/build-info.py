#!/usr/bin/env python3
"""Check the sunfish toolchain and record provenance in Debian packages."""

import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys


def command(*args):
    return subprocess.check_output(args, text=True).strip()


def toolchains():
    result = {}
    for role, compiler in (('target', 'aarch64-linux-gnu-gcc-14'),
                           ('host', 'gcc-14')):
        version = command(compiler, '-dumpfullversion', '-dumpversion')
        if version.split('.')[0] != '14':
            raise ValueError(f'{role} compiler must be GCC 14, got {version}')
        target = command(compiler, '-dumpmachine')
        if role == 'target' and target != 'aarch64-linux-gnu':
            raise ValueError(f'target compiler must target aarch64-linux-gnu, got {target}')
        result[role] = {
            'command': compiler,
            'version': version,
            'machine': target,
            'description': command(compiler, '--version').splitlines()[0],
        }
    return result


def source_commit():
    # OBS exports omit .git and supply this file instead. Do not accidentally
    # resolve a parent checkout when building an archive inside another repo.
    info = Path('_scmsync.obsinfo')
    if info.is_file():
        match = re.search(r'^commit:\s*([0-9a-f]{40})\s*$',
                          info.read_text(), re.MULTILINE)
        if not match:
            raise ValueError('invalid commit in _scmsync.obsinfo')
        return match.group(1)
    if Path('.git').exists():
        return command('git', 'rev-parse', '--verify', 'HEAD')
    return None


def sha256(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def main():
    compilers = toolchains()
    if sys.argv[1:] == ['--check-toolchain']:
        print('sunfish: GCC 14 target and host compilers verified')
        return
    if len(sys.argv) != 2:
        raise ValueError('usage: build-info.py --check-toolchain | OUTPUT.json')

    baseline = Path('debian/kernel-base').read_text().strip()
    if not re.fullmatch(r'[0-9a-f]{40}', baseline):
        raise ValueError('invalid debian/kernel-base')
    release = Path('include/config/kernel.release').read_text().strip()
    if release != '7.1.0-sm7150':
        raise ValueError(f'unexpected kernel release: {release}')
    report = {
        'package_version': command('dpkg-parsechangelog', '-S', 'Version'),
        'kernel_release': release,
        'kernel_base_commit': baseline,
        'source_commit': source_commit(),
        'compilers': compilers,
        'sha256': {
            str(path): sha256(path) for path in (
                Path('.config'),
                Path('arch/arm64/boot/Image'),
                Path('arch/arm64/boot/dts/qcom/sm7150-google-sunfish.dtb'),
            )
        },
    }
    output = Path(sys.argv[1])
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    try:
        main()
    except (OSError, subprocess.CalledProcessError, ValueError) as error:
        sys.exit(f'sunfish build metadata: {error}')
