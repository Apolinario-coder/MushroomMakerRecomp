"""Build a Windows release from the source tree and an existing portable dependency bundle."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
WORKERS = ('build_open_symbols.py', 'community.py', 'mariover-LICENSE.txt', 'mariover.py',
           'mariover_keytables.py', 'mm2_course_crypto.py', 'native_catalogue.py', 'native_course.py')


def unpack_portable(archive, stage):
    """Verify only inventoried portable members, rejecting unsafe or duplicate paths."""
    with zipfile.ZipFile(archive) as source:
        manifests = [n for n in source.namelist() if n.endswith('/portable/manifest.json') or n == 'portable/manifest.json']
        if len(manifests) != 1:
            raise ValueError('Select a release ZIP containing one portable/manifest.json.')
        member = manifests[0]
        prefix = member[:-len('manifest.json')]
        manifest = json.loads(source.read(member))
        for relative, expected in manifest['files'].items():
            path = PurePosixPath(relative)
            if path.is_absolute() or '..' in path.parts or '\\' in relative or ':' in relative:
                raise ValueError('Unsafe portable member: '+relative)
            name = prefix+relative
            if source.namelist().count(name) != 1:
                raise ValueError('Missing or duplicated portable member: '+relative)
            data = source.read(name)
            if hashlib.sha256(data).hexdigest() != expected:
                raise ValueError('Portable checksum mismatch: '+relative)
            target = stage/'portable'/relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        (stage/'portable/manifest.json').write_bytes(source.read(member))


def package(archive, output):
    output = Path(output).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists():
        raise ValueError('Choose a new output path; the existing archive will not be overwritten.')
    with tempfile.TemporaryDirectory(prefix='mmr-package-') as temp:
        stage = Path(temp)/'MushroomMakerRecomp'
        stage.mkdir()
        for name in ('assets', 'defaults', 'docs', 'third_party'):
            shutil.copytree(ROOT/name, stage/name, ignore=shutil.ignore_patterns('__pycache__', '*.pyc'))
        shutil.copytree(ROOT/'runtime', stage/'src', ignore=shutil.ignore_patterns('__pycache__', '*.pyc'))
        for name in ('README.md', 'NOTICE.md', 'build-launcher.ps1'):
            shutil.copy2(ROOT/name, stage/name)
        shutil.copy2(ROOT/'tools/setup.py', stage/'setup.py')
        shutil.copy2(ROOT/'tools/requirements.txt', stage/'requirements.txt')
        for source in (ROOT/'tools/launcher').iterdir():
            if source.is_file():
                shutil.copy2(source, stage/source.name)
        (stage/'tools').mkdir()
        for name in WORKERS:
            shutil.copy2(ROOT/'tools'/name, stage/'tools'/name)
        unpack_portable(archive, stage)
        subprocess.run(['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                        str(ROOT/'build-launcher.ps1'), '-OutputDirectory', str(stage)], check=True)
        manifest = {p.relative_to(stage).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                    for p in sorted(stage.rglob('*')) if p.is_file()}
        (stage/'release-manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
        # A temporary archive prevents failed builds from appearing as completed output.
        partial = Path(temp)/'release.zip'
        with zipfile.ZipFile(partial, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as target:
            for path in sorted(stage.rglob('*')):
                if path.is_file():
                    target.write(path, 'MushroomMakerRecomp/'+path.relative_to(stage).as_posix())
        with zipfile.ZipFile(partial) as check:
            if check.testzip() is not None:
                raise ValueError('Release ZIP failed verification.')
            for name, expected in manifest.items():
                if hashlib.sha256(check.read('MushroomMakerRecomp/'+name)).hexdigest() != expected:
                    raise ValueError('Release checksum mismatch: '+name)
        shutil.copy2(partial, output)
    checksum = hashlib.sha256(output.read_bytes()).hexdigest()
    output.with_suffix('.zip.sha256').write_text(checksum+'  '+output.name+'\n')
    print('Verified release:', output)
    print('SHA-256:', checksum)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--portable-from', type=Path, required=True, help='Existing Windows release ZIP with its inventoried portable tools.')
    parser.add_argument('--output', type=Path, required=True, help='New Windows release ZIP.')
    args = parser.parse_args()
    package(args.portable_from, args.output)


if __name__ == '__main__':
    main()
