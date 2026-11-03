#!/usr/bin/env python3
"""The public Linux / Steam Deck release zip: the three programs and their libraries, the launcher
and settings (tools/linux/), the README, tools/xdvdfs.py to unpack the player's disc, and the
license texts. No game files: the player unpacks their own disc into graw360/game.

    python3 -I tools/make_release_linux.py --version 0.6 --commit <commit> out.zip
        [--build-root <checkout with the deck-amd64 builds>] [--config relwithdebinfo]

Build all three first: tools/build-deck.sh, with --project port and --project port-mp. Attach the
zip to the GitHub release; never commit it (CONTRIBUTING.md, "Releases").
"""
import argparse
import glob
import os
import stat
import zipfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOP = 'graw360'
SDK = os.path.join('third_party', 'rexglue-sdk-bd833a2-deck')
LICENSE_NAMES = ('LICENSE', 'LICENSE.txt', 'LICENSE.md', 'COPYING', 'COPYING.LGPLv2.1')
DISC_NOTE = """Unpack your own Xbox 360 disc image here, from the folder above:

  python3 -I xdvdfs.py "<your GRAW ISO>" extract game

This folder should then hold default.xex and Yeti_Final_Xenon.exe directly. See README.txt.
"""


def text_file(path):
    """A text file with LF line endings, whichever checkout it came from."""
    with open(path, 'rb') as f:
        return f.read().replace(b'\r\n', b'\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('out')
    parser.add_argument('--version', required=True)
    parser.add_argument('--commit', required=True)
    parser.add_argument('--build-root', default=REPO)
    parser.add_argument('--config', default='relwithdebinfo')
    args = parser.parse_args()

    def build(project):
        return os.path.join(args.build_root, project, 'out', 'build', f'deck-amd64-{args.config}')

    # (path in the zip, source path or None, bytes or None, executable)
    files = []
    for project, program in (('port', 'graw'), ('port-mp', 'grawmp'), ('port-yeti', 'grawyeti')):
        path = os.path.join(build(project), program)
        if not os.path.exists(path):
            raise SystemExit(f'{path} is missing: run tools/build-deck.sh --project {project}')
        files.append((f'linux/{program}', path, None, True))
    libraries = sorted(glob.glob(os.path.join(build('port-yeti'), '*.so*')))
    if not libraries:
        raise SystemExit(f'No libraries in {build("port-yeti")}')
    for library in libraries:
        files.append((f'linux/{os.path.basename(library)}', library, None, True))

    linux = os.path.join(REPO, 'tools', 'linux')
    for name, executable in (('graw-linux.sh', True), ('deck-sensors.py', True),
                             ('grawyeti.toml', False), ('grawmp.toml', False)):
        files.append((f'linux/{name}', None, text_file(os.path.join(linux, name)), executable))
    files.append(('README.txt', None,
                  text_file(os.path.join(REPO, 'tools', 'release', 'README-Linux.txt')), False))
    files.append(('xdvdfs.py', None, text_file(os.path.join(REPO, 'tools', 'xdvdfs.py')), False))
    files.append(('game/PUT YOUR DISC HERE.txt', None, DISC_NOTE.encode(), False))
    files.append(('VERSION.txt', None,
                  f'GRAW 360 Recompiled {args.version} (Linux), commit {args.commit}\n'.encode(),
                  False))

    for name in ('LICENSE', 'NOTICE'):
        files.append((name, None, text_file(os.path.join(REPO, name)), False))
    for path in sorted(glob.glob(os.path.join(REPO, 'LICENSES', '*'))):
        files.append((f'LICENSES/{os.path.basename(path)}', None, text_file(path), False))
    thirdparty = os.path.join(args.build_root, SDK, 'thirdparty')
    found = 0
    for library in sorted(os.listdir(thirdparty)):
        for name in LICENSE_NAMES:
            path = os.path.join(thirdparty, library, name)
            if os.path.isfile(path):
                files.append((f'third-party-licenses/{library}/{name}', None, text_file(path), False))
                found += 1
    if not found:
        raise SystemExit(f'No third-party licenses under {thirdparty}')

    with zipfile.ZipFile(args.out, 'w', zipfile.ZIP_DEFLATED) as z:
        for arc, source, data, executable in files:
            if source:
                with open(source, 'rb') as f:
                    data = f.read()
            info = zipfile.ZipInfo(f'{TOP}/{arc}', date_time=(2026, 1, 1, 0, 0, 0))
            info.create_system = 3  # Unix, so external_attr's mode is honored
            info.external_attr = (stat.S_IFREG | (0o755 if executable else 0o644)) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(info, data)
    print(f'{args.out}: {len(files)} files, {os.path.getsize(args.out) / 2**20:.0f} MB')


if __name__ == '__main__':
    main()
