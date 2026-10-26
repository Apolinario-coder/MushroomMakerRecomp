"""Prepare user-owned 3.0.3 files and start the locally compiled Windows runtime."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import time
import zipfile
import tempfile

ROOT = Path(__file__).resolve().parent
def data_directory(local):
    current,legacy=local/'MushroomMakerRecomp',local/'MarioMaker2PC'
    if current.exists() or not legacy.exists():return current
    try:
        if os.name=='nt':
            import msvcrt
            with (legacy/'session.lock').open('a+b') as guard:
                if not os.fstat(guard.fileno()).st_size:guard.write(b'0');guard.flush()
                guard.seek(0);msvcrt.locking(guard.fileno(),msvcrt.LK_NBLCK,1)
                guard.seek(0);msvcrt.locking(guard.fileno(),msvcrt.LK_UNLCK,1)
        # Windows cannot rename the directory while our own lock-file handle is open.
        legacy.rename(current)
    except OSError:
        # Keep using the legacy profile if a game session or file handle blocks migration.
        return current if current.exists() else legacy
    return current


DATA = data_directory(Path(os.environ.get('LOCALAPPDATA', Path.home())))
MAIN_SHA = '8d838d99cd02493f1de1d0a40bb578492d66419c803a724e27054c60dd60e9af'
SDK_SHA = '754cf4b581d749fbe3d457cf66497b141ef994cf592d247919d64a37dcc30d27'
FONTS = ('nintendo_udsg-r_std_003.ttf', 'nintendo_udsg-r_org_zh-cn_003.ttf',
         'nintendo_udsg-r_ext_zh-cn_003.ttf', 'nintendo_udjxh-db_zh-tw_003.ttf',
         'nintendo_udsg-r_ko_003.ttf', 'nintendo_ext_003.ttf', 'nintendo_ext2_003.ttf')


def progress(stage, completed, total, step, steps=8):
    print('MMR_PROGRESS '+json.dumps({'stage':stage,'completed':completed,'total':total,'step':step,'steps':steps}), flush=True)


def message(text):
    print('MMR_MESSAGE '+text, flush=True)


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def locate_game(dump):
    """Find extracted components without moving or copying the user's files."""
    names = {name: [] for name in ('main', 'sdk', 'main.npdm')}
    romfs = []
    def scan_error(error):
        raise ValueError('Cannot search game folder: '+str(error))
    for folder, dirs, files in os.walk(dump, onerror=scan_error):
        folder = Path(folder)
        for name in files:
            if name.lower() in names:
                names[name.lower()].append(folder/name)
        if folder.name.lower() == 'pack':
            entries = {name.lower() for name in files}
            if {'layoutnetwork.pack', 'layoutstatic.pack'} <= entries:
                romfs.append(folder.parent)
    missing = [name for name, paths in names.items() if not paths]
    if not romfs:
        missing.append('RomFS (Pack/LayoutNetwork.pack and Pack/LayoutStatic.pack)')
    if missing:
        raise ValueError('Required game components are missing inside '+str(dump)+':\n'+'\n'.join(missing))
    if len(romfs) != 1:
        raise ValueError('Multiple RomFS folders found. Choose a narrower game folder:\n'+'\n'.join(map(str, sorted(romfs))))
    # Prefer supported executables when base and updated exports are both present.
    found = {'romfs': romfs[0]}
    for name, expected in (('main', MAIN_SHA), ('sdk', SDK_SHA)):
        paths = sorted(names[name], key=lambda p: (len(p.parts), str(p).lower()))
        found[name] = next((p for p in paths if digest(p) == expected), paths[0])
    npdms = names['main.npdm']
    paired = next((p for p in npdms if p.parent == found['main'].parent), None)
    if paired is None and len(npdms) != 1:
        raise ValueError('Multiple main.npdm files found. Keep the one from the updated ExeFS with main.')
    found['main.npdm'] = paired or npdms[0]
    return found


def portable_toolchain():
    """Unpack the included compiler into the private cache, never into the game dump."""
    portable = ROOT/'portable'
    manifest = json.loads((portable/'manifest.json').read_text())
    archive = portable/'toolchain.zip'
    expected = manifest['files']['toolchain.zip']
    target = DATA/'toolchains'/expected[:16]
    if (target/'ready.json').is_file():
        progress('Preparing included tools',1,1,2)
        return target
    progress('Preparing included tools',0,0,2)
    if digest(archive) != expected:
        raise ValueError('The included compiler archive is damaged. Extract the release again.')
    target.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='unpack-', dir=target.parent) as temporary:
        stage = Path(temporary)/'compiler'
        stage.mkdir()
        with zipfile.ZipFile(archive) as source:
            members = manifest['toolchain_files']
            if len(source.namelist()) != len(members) or set(source.namelist()) != set(members):
                raise ValueError('The included compiler inventory does not match its archive.')
            total=len(members)
            progress('Preparing included tools',0,total,2)
            for index, (name, checksum) in enumerate(members.items(),1):
                relative = Path(name)
                if relative.is_absolute() or '..' in relative.parts or ':' in name or '\\' in name:
                    raise ValueError('Invalid compiler archive path.')
                path = stage/relative
                path.parent.mkdir(parents=True, exist_ok=True)
                with source.open(name) as src, path.open('wb') as dst:
                    shutil.copyfileobj(src, dst)
                if digest(path) != checksum:
                    raise ValueError('Damaged compiler component: '+name)
                if index%40==0 or index==total:
                    progress('Preparing included tools',index,total,2)
        (stage/'ready.json').write_text(json.dumps({'archive_sha256':expected}))
        stage.rename(target)
    return target


def build_jobs():
    """Leave memory for Windows while using available CPU cores for compilation."""
    jobs = min(16, os.cpu_count() or 2)
    if os.name == 'nt':
        import ctypes
        class Memory(ctypes.Structure):
            _fields_ = [('length',ctypes.c_ulong),('load',ctypes.c_ulong)] + [(name,ctypes.c_ulonglong) for name in ('total','available','page_total','page_available','virtual_total','virtual_available','extended')]
        memory = Memory();memory.length = ctypes.sizeof(memory)
        if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(memory)):
            jobs = min(jobs, max(1, int((memory.available-2*1024**3)/(768*1024**2))))
    return max(1, jobs)


def validate(dump, toolchain=None):
    progress('Checking game files',0,0,1)
    game = locate_game(dump)
    required = []
    required += [ROOT/'assets/fonts_open'/name for name in FONTS]
    if toolchain is None:
        required += [ROOT/'portable'/name for name in ('python/python.exe','manifest.json','toolchain.zip','prebuilt/host-support.a','prebuilt/ssbu_glslang.dll')]
    else:
        required += [toolchain/'bin'/name for name in ('clang.exe', 'clang++.exe', 'SDL3.dll', 'libwinpthread-1.dll')]
        required += [toolchain/'include/SDL3/SDL.h', toolchain/'include/vulkan/vulkan.h',
                     toolchain/'lib/libglslang.a', toolchain/'lib/libSDL3.dll.a']
    missing = [str(p) for p in required if not p.is_file()]
    if missing:
        raise ValueError('Required files are missing:\n' + '\n'.join(missing))
    for name, expected in (('main', MAIN_SHA), ('sdk', SDK_SHA)):
        if digest(game[name]) != expected:
            raise ValueError('The ExeFS '+name+' does not match the supported 3.0.3 dump. Check the update and export again.')
    for name in FONTS:
        with (ROOT/'assets/fonts_open'/name).open('rb') as font:
            magic = font.read(4)
        if magic not in (b'\x00\x01\x00\x00', b'OTTO', b'ttcf'):
            raise ValueError('The bundled replacement font is invalid: '+name)
    try:
        from cryptography.hazmat.primitives.ciphers import Cipher
        from PIL import Image
    except ImportError:
        raise ValueError('The bundled helper dependencies are missing. Extract the full release again.') from None
    progress('Checking game files',1,1,1)
    message('Your game files are ready for preparation.')
    return game


def run(command, cwd, env):
    print('Running '+Path(command[0]).name+' '+Path(command[1]).name, flush=True)
    started = time.monotonic()
    subprocess.run([str(x) for x in command], cwd=cwd, env=env, check=True)
    print('Stage finished in %.1f seconds.' % (time.monotonic()-started), flush=True)


def source_identity(mode='-O0'):
    identity = hashlib.sha256()
    for p in sorted((ROOT/'src').rglob('*')):
        if p.is_file():
            identity.update(p.relative_to(ROOT).as_posix().encode())
            identity.update(p.read_bytes())
    for p in sorted((ROOT/'assets/fonts_open').rglob('*')):
        if p.is_file():
            identity.update(p.relative_to(ROOT).as_posix().encode())
            identity.update(p.read_bytes())
    identity.update(b'mm2-portable-build-v1')
    identity.update(mode.encode())
    if (ROOT/'portable/manifest.json').is_file():
        identity.update((ROOT/'portable/manifest.json').read_bytes())
    identity.update((ROOT/'requirements.txt').read_bytes())
    return identity.hexdigest()[:16]


def prepare(dump, toolchain=None):
    started = time.monotonic()
    game = validate(dump, toolchain)
    bundled = toolchain is None
    if bundled:
        toolchain = portable_toolchain()
    # Rebuild when any shipped source changes; generated/private files never live in ROOT.
    mode = os.environ.get('RECOMP_OPT', '-O0')
    workspace = DATA/'runtime'/source_identity(mode)
    host = workspace/'host'
    ready = workspace/'ready.json'
    env = {k:v for k,v in os.environ.items() if not k.startswith(('SSBU_', 'RECOMP_', 'MM2_'))}
    env.update(MSYS64=str(toolchain), RECOMP_CLANG=str(toolchain/'bin/clang.exe'),
               RECOMP_CXX=str(toolchain/'bin/clang++.exe'),
               RECOMP_GEN=str(workspace/'gen'), RECOMP_OBJ=str(workspace/'obj'),
               RECOMP_DLL='mm2_303_game.dll', RECOMP_EXE='mm2_303_diag.exe',
               RECOMP_MAIN_ELF=str(workspace/'main.elf'), RECOMP_SDK_ELF=str(workspace/'sdk.elf'),
               RECOMP_OPT=mode,
               PYTHONUTF8='1', PYTHONUNBUFFERED='1', PYTHONDONTWRITEBYTECODE='1')
    env['PATH'] = str(toolchain/'bin')+os.pathsep+env.get('PATH','')
    if (ROOT/'portable/prebuilt/host-support.a').is_file():
        env['RECOMP_PREBUILT'] = str(ROOT/'portable/prebuilt')
    if not ready.exists():
        host.mkdir(parents=True, exist_ok=True)
        shutil.copytree(ROOT/'src/host', host, dirs_exist_ok=True)
        shutil.copytree(ROOT/'src/lifter', workspace/'lifter', dirs_exist_ok=True)
        shutil.copytree(ROOT/'src/sdk', host, dirs_exist_ok=True)
        progress('Preparing game files',0,2,3)
        for index, name in enumerate(('main', 'sdk'),1):
            run([sys.executable, ROOT/'src/nso2elf.py', game[name], workspace/(name+'.elf')], workspace, env)
            progress('Preparing game files',index,2,3)
        shutil.copy2(game['main.npdm'], workspace/'main.npdm')
        progress('Preparing support files',0,3,4)
        run([sys.executable, host/'derive_imports.py', workspace/'main.elf', host/'imports_all.txt'], host, env)
        progress('Preparing support files',1,3,4)
        run([sys.executable, host/'gen_sdkdata.py'], host, env)
        progress('Preparing support files',2,3,4)
        run([sys.executable, host/'sdklift.py', 'gen'], host, env)
        progress('Preparing support files',3,3,4)
        jobs = build_jobs()
        print('Using %d build workers based on available CPU and memory.' % jobs, flush=True)
        progress('Preparing your game',0,0,5)
        run([sys.executable, workspace/'lifter/recomp_eh.py', workspace/'gen', str(jobs)], workspace, env)
        run([sys.executable, host/'build_mm2.py', 'all', str(jobs)], host, env)
        ready.write_text(json.dumps({'main_sha256': MAIN_SHA, 'sdk_sha256': SDK_SHA, 'generated_code_optimization':mode}))
    else:
        message('Using your prepared game files.')
    progress('Ready to play',1,1,8)
    message('Preparation finished in %.1f seconds.' % (time.monotonic()-started))
    return host, env, workspace


def launch(host, env, workspace, dump):
    game = locate_game(dump)
    (DATA/'config').mkdir(parents=True, exist_ok=True)
    (DATA/'saves').mkdir(exist_ok=True)
    (DATA/'cache/vk').mkdir(parents=True, exist_ok=True)
    (DATA/'mods').mkdir(exist_ok=True)
    # Course World workers locate tools and media cache through SSBU_PROJECT_ROOT.
    shutil.copytree(ROOT/'tools', DATA/'tools', dirs_exist_ok=True)
    for source in (ROOT/'defaults').glob('*.ini'):
        target = DATA/'config'/source.name
        if not target.exists():
            shutil.copy2(source, target)
    env.update(SSBU_PYTHON=sys.executable, SSBU_PROJECT_ROOT=str(DATA), SSBU_ROMFS=str(game['romfs']),
               SSBU_USERDATA=str(DATA/'saves'), SSBU_SHOW='1', SSBU_VOLUME='100', SSBU_TITLE='MushroomMakerRecomp',
               SSBU_SDK_ELF=str(workspace/'sdk.elf'),
               SSBU_OPEN_FONTS=str(ROOT/'assets/fonts_open'), SSBU_MODS=str(DATA/'mods'),
               SSBU_CONFIG=str(DATA/'config/controllers.ini'),
               SSBU_GRAPHICS_CONFIG=str(DATA/'config/graphics.ini'),
               SSBU_VK_CACHE=str(DATA/'cache/vk'), SSBU_VK_WORKERS='2',
               SSBU_VK_PREWARM='0', SSBU_VK_PREP='0', SSBU_VK_GPL='0',
               SSBU_VK_CSCAN='0', SSBU_VK_LOOKAHEAD='0', SSBU_SYNC_SUBMIT='1', SSBU_VK_CBKEEP='0')
    with (DATA/'last-run.log').open('wb') as output:
        while True:
            child = subprocess.run([str(host/'mm2_303_diag.exe'), str(workspace/'main.elf'), '0'],
                                   cwd=host, env=env, stdout=output, stderr=subprocess.STDOUT)
            if child.returncode != 10:
                break
    if child.returncode:
        raise RuntimeError('The game stopped with code '+str(child.returncode)+'. See '+str(DATA/'last-run.log'))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('validate', 'prepare', 'play'))
    parser.add_argument('--dump', type=Path, required=True)
    parser.add_argument('--toolchain', type=Path, help='Developer override; the release uses its included compiler by default.')
    args = parser.parse_args()
    dump = args.dump.resolve()
    toolchain = args.toolchain.resolve() if args.toolchain else None
    if args.action == 'validate':
        validate(dump, toolchain)
        progress('Files checked',1,1,1,1)
    else:
        DATA.mkdir(parents=True, exist_ok=True)
        # An OS-held lock prevents two launchers from building/running the same private installation.
        import msvcrt
        with (DATA/'session.lock').open('a+b') as lock:
            lock.seek(0)
            if not lock.read(1):
                lock.write(b'0'); lock.flush()
            lock.seek(0)
            try:
                msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
            except OSError:
                raise RuntimeError('Another preparation or game session is already running.')
            host, env, workspace = prepare(dump, toolchain)
            if args.action == 'play':
                message('Your game is starting. Close the game to return here.')
                launch(host, env, workspace, dump)
            else:
                message('Your game is ready. Choose Play to start.')
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print('Diagnostic: '+str(error), file=sys.stderr, flush=True)
        detail=str(error).lower()
        if isinstance(error,OSError) and getattr(error,'errno',None)==28:
            friendly='There is not enough free disk space. Free space on the build drive and try again.'
        elif 'multiple romfs' in detail:
            friendly='Several game copies were found. Choose a folder containing only the copy you want to use.'
        elif 'missing' in detail:
            friendly='Some required files are missing. Select your complete game folder with update 3.0.3, and check that the full launcher package is extracted.'
        elif isinstance(error,ValueError):
            friendly='The selected files could not be verified. Check that your complete game files include update 3.0.3.'
        elif 'another preparation' in detail:
            friendly='Another preparation or game session is already running. Close it before continuing.'
        else:
            friendly='Your game could not be prepared. Open the saved report for details, then try again.'
        print('Setup failed: '+friendly, file=sys.stderr, flush=True)
        raise SystemExit(1)
