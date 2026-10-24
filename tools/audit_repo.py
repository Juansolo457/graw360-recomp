"""Check what git would publish for game material: run before every push (CI runs it too).

    python -I tools/audit_repo.py                 every file in the index: tracked plus staged
    python -I tools/audit_repo.py --history       every file in every commit (a report: history
                                                  isn't rewritten by anything here)
    python -I tools/audit_repo.py --install-hook  run it from .git/hooks/pre-push too (optional)

It reads git's own blobs (git cat-file), never the working tree, so local junctions to game data
or the toolchain can't hide or add anything. It rejects:
  - folders that only ever hold game material or builds (private/, <port>/assets, generated
    translations under <port>/generated/<name>/, logs, saves, caches, shader dumps, warm-up lists,
    build output, the toolchain and the SDK checkout);
  - game file types and executable or archive payloads, by extension and by the first bytes
    (XEX2, PE/MZ, ELF, ZIP, 7z, RAR, LithTech LTAR, DDS, Bink, Xbox packed resources);
  - recompiled game code (DEFINE_REX_FUNC(sub_...) functions, or many "// lis r.." instruction
    comments), shader warm-up lists, the retail XEX key (by its SHA-256, which is all this file
    holds), symlinks, submodules and anything over 2 MB, except the reviewed exceptions below.

This catches the common accidents, not every copyright question: a clean run doesn't make a
change legally safe. Releases stay source only: never attach builds, game files or the private
test packages (private/) to a release. Same script in fear360-recomp and graw360-recomp.
"""
import hashlib
import os
import re
import subprocess
import sys

# Folders that only hold game material, data derived from it, builds or local state.
PROHIBITED_PATHS = [
    (r'(^|/)private/', 'private/ (the disc, decrypted images, captures, test packages)'),
    (r'^game/', 'game/ (game files)'),
    (r'^port[^/]*/assets(/|$)', '<port>/assets (the game files)'),
    (r'^port[^/]*/generated/[^/]+/', 'generated translation of the game code'),
    (r'^port[^/]*/(logs|out|true)(/|$)', 'logs, build output or shader dumps'),
    (r'^port[^/]*/(user|cache)[^/]*(/|$)', 'saves, settings or shader caches'),
    (r'^port[^/]*/warmup(/|$)', 'shader warm-up list (derived from the game)'),
    (r'^(\.tools|third_party)(/|$)', 'toolchain or SDK checkout'),
    (r'(^|/)(ppc|shaders_generated)/', 'generated code folder'),
]
# Game files and binaries, by extension.
PROHIBITED_EXTENSIONS = {
    '.iso', '.xex', '.xbe', '.god', '.stfs', '.gamedb', '.arch00', '.bik', '.big', '.xpr',
    '.xwb', '.xsb', '.fxo', '.wld', '.cat', '.exe', '.dll', '.lib', '.obj', '.pdb', '.ilk',
    '.dmp', '.zip', '.7z', '.rar', '.bin',
}
# First bytes of executables, archives and game formats.
SIGNATURES = [
    (b'XEX2', 'Xbox 360 executable (XEX2)'),
    (b'MZ', 'Windows executable (MZ/PE)'),
    (b'\x7fELF', 'ELF executable'),
    (b'PK\x03\x04', 'ZIP archive'),
    (b'7z\xbc\xaf\x27\x1c', '7z archive'),
    (b'Rar!', 'RAR archive'),
    (b'LTAR', 'LithTech archive'),
    (b'DDS ', 'DDS texture'),
    (b'BIK', 'Bink video'),
    (b'KB2', 'Bink 2 video'),
    (b'XPR2', 'Xbox packed resource'),
    (b'WBND', 'Xbox wave bank'),
]
MAX_SIZE = 2 * 1024 * 1024
# The retail XEX key, as SHA-256 of its 32 lowercase hex digits. Only the fingerprint is kept.
KEY_FINGERPRINTS = {'69145c70912088cefb8b5e301d65e427f416a4764edfdeb60c932597525abd1f'}
HEX32 = re.compile(rb'(?<![0-9A-Fa-f])[0-9A-Fa-f]{32}(?![0-9A-Fa-f])')
TRANSLATED_FUNCTION = re.compile(rb'DEFINE_REX_FUNC\(sub_[0-9A-Fa-f]{8}\)')
INSTRUCTION_COMMENT = re.compile(rb'^\s*// (lis|addi|lwz|stw|bl|mtctr|rlwinm) ', re.M)
WARMUP_LIST = b'R"WARMUP(' + b'warmup '  # in two parts, so this file doesn't match itself

# Reviewed exceptions: (path regex, rule it is exempt from, why).
EXCEPTIONS = [
    (r'^port[^/]*/generated/rexglue\.cmake$', 'path',
     'rexglue init build scaffolding (BSD-3-Clause), not a translation'),
    (r'^port[^/]*/src/native/shaders/[a-z_]+_cs\.h$', 'any',
     'SPIR-V generated from the open-source GLSL beside it (fear-nx/Project8Recomp, GPL)'),
    (r'^patches/rexglue-sdk-nx/0000-fear-nx-native-vulkan-SDK-patch\.patch$', 'size',
     "fear-nx's GPL SDK patch: source code, kept so the SDK can be rebuilt"),
]


def git(*args, data=None):
    return subprocess.run(['git', *args], input=data, capture_output=True, check=True).stdout


def exempt(path, rule):
    return any(re.search(pattern, path) and kind in (rule, 'any') for pattern, kind, _ in EXCEPTIONS)


def index_blobs():
    """(mode, sha, path) of every file in the index: what the next commit would contain."""
    out = []
    for entry in git('ls-files', '-s', '-z').split(b'\0'):
        if entry:
            meta, path = entry.split(b'\t', 1)
            mode, sha, _stage = meta.split()
            out.append((mode.decode(), sha.decode(), path.decode('utf-8', 'replace')))
    return out


def history_blobs():
    """(mode, sha, path) of every blob reachable from any ref, under each path it had."""
    out = []
    for line in git('rev-list', '--objects', '--all').decode('utf-8', 'replace').splitlines():
        sha, _, path = line.partition(' ')
        if path:
            out.append(('?', sha, path))
    types = git('cat-file', '--batch-check=%(objecttype)',
                data=''.join(f'{sha}\n' for _, sha, _ in out).encode()).decode().split()
    return [blob for blob, kind in zip(out, types) if kind == 'blob']


def read_blobs(shas):
    """{sha: content} through one git cat-file --batch."""
    raw = git('cat-file', '--batch', data=''.join(f'{s}\n' for s in shas).encode())
    contents, pos = {}, 0
    while pos < len(raw):
        header_end = raw.index(b'\n', pos)
        sha, _kind, size = raw[pos:header_end].split()
        size = int(size)
        contents[sha.decode()] = raw[header_end + 1:header_end + 1 + size]
        pos = header_end + 1 + size + 1
    return contents


def problems(mode, path, data):
    found = []
    low = path.lower()
    if mode == '120000':
        found.append(('path', 'symlink (could point at game data)'))
    if mode == '160000':
        found.append(('path', 'submodule'))
    for pattern, why in PROHIBITED_PATHS:
        if re.search(pattern, low):
            found.append(('path', why))
    ext = os.path.splitext(low)[1]
    if ext in PROHIBITED_EXTENSIONS:
        found.append(('extension', f'{ext} file (game data, binary or archive)'))
    if data is None:
        return found
    for magic, why in SIGNATURES:
        if data.startswith(magic) and (magic != b'MZ' or b'\0' in data[:512]):
            found.append(('signature', why))
    if len(data) > MAX_SIZE:
        found.append(('size', f'{len(data) / 2**20:.1f} MB (over {MAX_SIZE // 2**20} MB)'))
    if TRANSLATED_FUNCTION.search(data) or len(INSTRUCTION_COMMENT.findall(data)) >= 20:
        found.append(('content', 'recompiled game code'))
    if WARMUP_LIST in data:
        found.append(('content', 'shader warm-up list (derived from the game)'))
    for token in HEX32.findall(data):
        if hashlib.sha256(token.lower()).hexdigest() in KEY_FINGERPRINTS:
            found.append(('content', 'the retail XEX key'))
            break
    return [(rule, why) for rule, why in found if not exempt(path, rule)]


def install_hook():
    hook = os.path.join(git('rev-parse', '--git-path', 'hooks').decode().strip(), 'pre-push')
    if os.path.exists(hook):
        sys.exit(f'{hook} exists already; add "python -I tools/audit_repo.py || exit 1" to it.')
    with open(hook, 'w', newline='\n') as f:
        f.write('#!/bin/sh\n# tools/audit_repo.py: no game material in what is pushed.\n'
                'python -I tools/audit_repo.py || exit 1\n')
    os.chmod(hook, 0o755)
    print(f'wrote {hook}')


def main():
    os.chdir(git('rev-parse', '--show-toplevel').decode().strip())
    if '--install-hook' in sys.argv:
        install_hook()
        return 0
    history = '--history' in sys.argv
    blobs = history_blobs() if history else index_blobs()
    wanted = sorted({sha for mode, sha, _ in blobs if mode not in ('160000',)})
    contents = read_blobs(wanted) if wanted else {}
    failures = []
    for mode, sha, path in blobs:
        for _rule, why in problems(mode, path, contents.get(sha)):
            failures.append((path, sha, why))
    scope = 'every commit' if history else 'the index (tracked and staged files)'
    if not failures:
        print(f'audit_repo: {len(blobs)} files in {scope}: no game material found.')
        return 0
    print(f'audit_repo: {len(failures)} problem(s) in {scope}:')
    for path, sha, why in sorted(set(failures)):
        print(f'  {path}  [{sha[:10]}]  {why}')
    if history:
        print('These are in history. Removing them from the tree does not remove them from the '
              'commits; rewriting history is a separate decision.')
    else:
        print('Unstage or remove these before committing (git rm --cached keeps a local copy).')
    return 1


if __name__ == '__main__':
    sys.exit(main())
