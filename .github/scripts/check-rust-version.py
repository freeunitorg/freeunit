#!/usr/bin/env python3
#
# Check every package of a Cargo project with the Rust release that its
# "rust-version" names, so that a dependency update which raises the floor
# fails here instead of in a build that pins an older toolchain.
#
# Usage: check-rust-version.py <directory> [<target>]
#
# A package without "rust-version" is an error.

import os
import subprocess
import sys
import tomllib


def load(path):
    with open(path, 'rb') as f:
        return tomllib.load(f)


def run(cmd):
    print('+ ' + ' '.join(cmd), flush=True)
    return subprocess.run(cmd).returncode


def main():
    root = sys.argv[1]
    target = ['--target', sys.argv[2]] if len(sys.argv) > 2 else []
    manifest = os.path.join(root, 'Cargo.toml')
    top = load(manifest)
    workspace = top.get('workspace', {})

    dirs = [os.path.join(root, m) for m in workspace.get('members', [])]
    if 'package' in top:
        dirs.insert(0, root)

    failed = False

    for d in dirs:
        path = os.path.join(d, 'Cargo.toml')
        package = load(path)['package']
        version = package.get('rust-version')

        if isinstance(version, dict) and version.get('workspace'):
            version = workspace.get('package', {}).get('rust-version')

        if not isinstance(version, str):
            print(f'::error file={path}::{package["name"]} declares no '
                  'rust-version')
            failed = True
            continue

        if (run(['rustup', 'toolchain', 'install', version,
                 '--profile', 'minimal', *target]) != 0
            or run(['cargo', f'+{version}', 'check', '--locked',
                    '--manifest-path', manifest, '-p', package['name'],
                    *target]) != 0):
            print(f'::error file={path}::{package["name"]} does not build '
                  f'with Rust {version}, its rust-version')
            failed = True

    sys.exit(1 if failed else 0)


main()
