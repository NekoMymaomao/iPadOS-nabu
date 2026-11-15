#!/usr/bin/env python3
"""Relink the recorded object list, swapping the storage driver object.

The prelinked kernel is a static image: every driver object is placed at a
fixed virtual address chosen by the link order.  Reordering the list would
move unrelated kexts and invalidate the report, so the order is kept in
link-order.json and only the named objects are substituted.

Usage:
    relink.py --kernel <extended/kernel.native-apfs.macho> \\
              --prelink-report <extended/prelink-report.json> \\
              --output <dir> \\
              --replace NabuDataBlock.o=/path/to/NabuSystemDisk.o
"""
import argparse
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--kernel', type=Path, required=True)
    ap.add_argument('--prelink-report', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--replace', action='append', default=[],
                    help='name.o=/path/to/replacement.o (repeatable)')
    ap.add_argument('--objects-dir', type=Path, default=ROOT / 'objects')
    ap.add_argument('--extra-object', action='append', default=[],
                    help='append an object that is not in the recorded list')
    a = ap.parse_args()

    order = json.loads((ROOT / 'link-order.json').read_text())
    swaps = {}
    for item in a.replace:
        name, _, path = item.partition('=')
        if not name.endswith('.o') or not path:
            raise SystemExit(f'--replace expects name.o=path, got {item!r}')
        if Path(path).name in order['objects']:
            raise SystemExit(f'{name} is already in the recorded list; '
                             'relinking it twice would publish the kext twice')
        swaps[name] = Path(path)

    command = [sys.executable, str(ROOT / 'tools/link_nabu_data_block.py'),
               '--kernel', str(a.kernel),
               '--prelink-report', str(a.prelink_report),
               '--reference-object', str(a.objects_dir / 'IOBlockStorageDevice-reference.o'),
               '--aes-reference-object', str(a.objects_dir / 'AESReference.o'),
               '--usb-reference-object', str(a.objects_dir / 'NativeUsbReference.o'),
               '--output', str(a.output)]
    if order.get('reuse_executable_gaps'):
        command.append('--reuse-executable-gaps')

    for name in order['objects']:
        path = swaps.pop(name, None) or (a.objects_dir / name)
        if not path.exists():
            raise SystemExit(f'missing object {name} (looked for {path})')
        command += ['--object', str(path)]
    if swaps:
        raise SystemExit('recorded list has no slot for: ' + ', '.join(swaps))
    for extra in a.extra_object:
        command += ['--object', extra]

    print(' '.join(command), file=sys.stderr)
    result = subprocess.run(command)
    sys.exit(result.returncode)


if __name__ == '__main__':
    main()
