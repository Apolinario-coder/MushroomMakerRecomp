#!/usr/bin/env python3
"""Build the MM2 port with the MSYS2/UCRT64 toolchain (clang 22 + mingw-w64) instead of MSVC.

    python build_mm2.py all [jobs]        gen -> objects -> mm2_game.dll -> mm2_native.exe (+ glslang, SDL3)
    python build_mm2.py compile [jobs]    gen_eh_o2\\*.c -> obj_eh_o1\\*.obj
    python build_mm2.py dll               obj_eh_o1\\*.obj + game_shim_eh.c -> mm2_game.dll (+ .dll.a)
    python build_mm2.py exe               host sources -> mm2_native.exe
    python build_mm2.py glslang           vk_glslang_shim.c + glslang -> ssbu_glslang.dll
    python build_mm2.py sync              copy SDL3/pthread DLLs beside the exe

Why this file exists: build.py / build_opt.py drive clang with the MSVC ABI (link.exe flags, SDL3.lib, a
Windows SDK). This one keeps their structure and the object/DLL split but uses the GNU ABI: GNU-style link
flags, the import library the linker itself writes (-Wl,--out-implib), mingw's SDL3, and the Windows import
libraries that ship with mingw-w64. The optimiser ladder (a function over 20000 statements unoptimised, a
180 s per-file limit with fallback to -O0) is build_opt.py's, reused unchanged.
"""
import concurrent.futures
import glob
import json
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
MSYS = os.environ.get('MSYS64', r'C:\msys64\ucrt64')
CLANG = os.environ.get('RECOMP_CLANG', os.path.join(MSYS, 'bin', 'clang.exe'))
CXX = os.environ.get('RECOMP_CXX', os.path.join(MSYS, 'bin', 'clang++.exe'))

GEN = os.path.join(HERE, os.environ.get('RECOMP_GEN', 'gen_eh_o2'))
OBJ = os.path.join(HERE, os.environ.get('RECOMP_OBJ', 'obj_eh_o1'))
DLL = os.environ.get('RECOMP_DLL', 'mm2_game.dll')
GAME_IMPLIB = os.environ.get('RECOMP_IMPLIB', 'lib' + DLL[:-4] + '.dll.a')
SHIM = os.environ.get('RECOMP_SHIM', 'game_shim_eh.c')
EXE = os.environ.get('RECOMP_EXE', 'mm2_native.exe')
GLSLANG_DLL = 'ssbu_glslang.dll'
PREBUILT = os.environ.get('RECOMP_PREBUILT')
LINK_FLAGS = ['-fuse-ld=lld']

MSYS_INC = os.path.join(MSYS, 'include')
MSYS_LIB = os.path.join(MSYS, 'lib')
SDL_INC = os.path.join(MSYS_INC, 'SDL3')

# same list as build.py (the host runtime; not the lifted game code)
HOST_SOURCES = ['pc_course_world.c', 'pc_prompts.c', 'pc_menu.c', 'hostrt.c', 'boot.c', 'nnsdk.c', 'libcxx.c', 'os_threads.c', 'nvn_stub.c', 'nvn_gl.c', 'nvn_vk.c', 'mx_translate.c', 'tex_decode.c', 'nnfs.c', 'clib.c', 'nntime.c', 'nnhid.c', 'nnpad_sdl.c', 'sdkdata.c', 'display.c', 'nnpl.c', 'nnfontll.c', 'nnmovie.c', 'nnmisc.c', 'nnaudio.c', 'nnro.c', 'nro_registry.c', 'nnmovie_mf.c', 'nniostream.c', 'nncxxrt.c', 'nncxxlib.c', 'nnlibc2.c',
                'sdklift_gen.c', 'sdklift_rt.c', 'nntransfer.c', 'nnsdk2.c', 'nnmii.c', 'nnapplet.c', 'nnalbum.c', 'nnnet.c', 'nnfsdir.c', 'nnunwind.c', 'nvn_sdk.c', 'nvn_c4.c', 'nvn_c5.c', 'hitch.c', 'gfx_options.c', 'debug_mode.c', 'r1_state.c', 'gamedll.c']
HOST_FLAGS = ['-O1', '-g0' if PREBUILT else '-g', '-w', '-fno-strict-aliasing', '-I', HERE, '-I', GEN,
              '-Dfn_table=(*game_fn_table_p)', '-Dfn_table_count=(*game_fn_table_count_p)',
              '-Ddyn_syms=(*game_dyn_syms_p)', '-Ddyn_sym_count=(*game_dyn_sym_count_p)',
              '-I', SDL_INC, '-I', MSYS_INC]
# mingw-w64 import libraries (Windows), SDL3 from MSYS2. opengl32/advapi32/ws2_32 are the libs that the
# runtime asks for with #pragma comment(lib, ...), which MSVC honours and the mingw driver does not.
WIN_LIBS = ['-lshell32', '-luser32', '-lgdi32', '-lwinmm', '-lmfplat', '-lmfreadwrite', '-lmfuuid', '-lole32', '-lshlwapi',
            '-lversion', '-lsetupapi', '-limm32', '-lopengl32', '-ladvapi32', '-lws2_32', '-luuid']
GEN_HEADERS = ['runtime.h', 'neon.h', 'fpexact.h']


def progress(stage, completed, total, step):
    print('MMR_PROGRESS '+json.dumps({'stage':stage,'completed':completed,'total':total,'step':step,'steps':8}), flush=True)


def say(*a):
    print(*a, flush=True)


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, errors='replace', **kw)


# ---- lifted game code: gen_eh_o2\*.c -> obj_eh_o1\*.obj (unity batches at -O0, or build_opt.py's ladder) -----
UNITY_BATCH = int(os.environ.get('RECOMP_UNITY_BATCH', '16'))


def cmd_compile(jobs):
    import build_opt
    build_opt.CLANG = CLANG                                     # the MSVC path in build.py/build_opt.py is swapped for mingw clang
    build_opt.OPT = os.environ.get('RECOMP_OPT', '-O1')
    def compile_unit(name):
        src = name if os.path.isabs(name) else os.path.join(GEN, name)
        base = os.path.basename(src)
        obj = os.path.join(OBJ, base[:-2] + '.obj')
        code, output, seconds, note = build_opt.compile_file(
            src, obj, ['-w', '-fno-strict-aliasing', '-I', HERE, '-I', GEN])
        if note:
            say('%s: %s' % (base, note))
        return base, code, output, seconds

    os.makedirs(OBJ, exist_ok=True)
    all_c = sorted(n for n in os.listdir(GEN) if n.endswith('.c'))
    if build_opt.OPT == '-O0' and UNITY_BATCH > 1:
        chunks = [n for n in all_c if n.startswith('c_')]
        others = [n for n in all_c if not n.startswith('c_')]
        unity_dir = os.path.join(OBJ, 'unity_src')
        os.makedirs(unity_dir, exist_ok=True)
        units = []
        for n in others:
            src = os.path.join(GEN, n)
            units.append((n, src, [src], os.path.getsize(src)))
        for i in range(0, len(chunks), UNITY_BATCH):
            batch = chunks[i:i + UNITY_BATCH]
            uname = 'unity_%03d.c' % (i // UNITY_BATCH)
            usrc = os.path.join(unity_dir, uname)
            bpaths = [os.path.join(GEN, cf) for cf in batch]
            content = ''.join('#include "%s"\n' % p.replace('\\', '/') for p in bpaths)
            if not os.path.exists(usrc) or open(usrc).read() != content:
                with open(usrc, 'w') as f:
                    f.write(content)
            units.append((uname, usrc, bpaths, sum(os.path.getsize(p) for p in bpaths)))
        wanted = {u[0][:-2] + '.obj' for u in units}
        for n in os.listdir(OBJ):
            if n.endswith('.obj') and n not in wanted:
                os.remove(os.path.join(OBJ, n))
        units.sort(key=lambda u: u[3], reverse=True)
        newest = max(os.path.getmtime(os.path.join(HERE, h)) for h in GEN_HEADERS if os.path.exists(os.path.join(HERE, h)))
        todo = [(uname, usrc) for uname, usrc, deps, _ in units
                if not os.path.exists(os.path.join(OBJ, uname[:-2] + '.obj'))
                or os.path.getmtime(os.path.join(OBJ, uname[:-2] + '.obj')) < max([newest] + [os.path.getmtime(p) for p in deps])]
        say('sources: %d (%d unity units)   to compile: %d   jobs: %d' % (len(all_c), len(units), len(todo), jobs))
        progress('Building your game',len(units)-len(todo),len(units),6)
        t0 = time.time()
        failed, done = [], 0
        with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
            futs = {pool.submit(compile_unit, usrc): uname for uname, usrc in todo}
            for fut in concurrent.futures.as_completed(futs):
                _, code, out, secs = fut.result()
                uname = futs[fut]
                done += 1
                if code:
                    failed.append(uname)
                    say('FAILED %s\n%s' % (uname, out))
                progress('Building your game',len(units)-len(todo)+done-len(failed),len(units),6)
                if done % 10 == 0 or done == len(todo):
                    say('  %d/%d   %.0fs' % (done, len(todo), time.time() - t0))
        build_opt.report(OBJ)
        say('compiled: %d   failed: %d   elapsed: %.0fs' % (len(todo) - len(failed), len(failed), time.time() - t0))
        return 1 if failed else 0
    sources = sorted(all_c, key=lambda n: os.path.getsize(os.path.join(GEN, n)), reverse=True)
    newest = max(os.path.getmtime(os.path.join(HERE, h)) for h in GEN_HEADERS if os.path.exists(os.path.join(HERE, h)))
    todo = [n for n in sources
            if not os.path.exists(os.path.join(OBJ, n[:-2] + '.obj'))
            or os.path.getmtime(os.path.join(OBJ, n[:-2] + '.obj')) < max(os.path.getmtime(os.path.join(GEN, n)), newest)]
    say('sources: %d   to compile: %d   jobs: %d' % (len(sources), len(todo), jobs))
    progress('Building your game',len(sources)-len(todo),len(sources),6)
    t0 = time.time()
    failed, done = [], 0
    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        futs = {pool.submit(compile_unit, n): n for n in todo}
        for fut in concurrent.futures.as_completed(futs):
            name, code, out, secs = fut.result()
            done += 1
            if code:
                failed.append(name)
                say('FAILED %s\n%s' % (name, out))
            progress('Building your game',len(sources)-len(todo)+done-len(failed),len(sources),6)
            if done % 50 == 0 or done == len(todo):
                say('  %d/%d   %.0fs' % (done, len(todo), time.time() - t0))
    build_opt.report(OBJ)
    say('compiled: %d   failed: %d   elapsed: %.0fs' % (len(todo) - len(failed), len(failed), time.time() - t0))
    return 1 if failed else 0


# ---- game DLL ---------------------------------------------------------------------------------------------
def cmd_dll():
    progress('Finishing game setup',0,4,7)
    objs = sorted(os.path.join(OBJ, n) for n in os.listdir(OBJ) if n.endswith('.obj'))
    if not objs:
        say('no objects: run compile first')
        return 1
    dll = os.path.join(HERE, DLL)
    rsp = os.path.join(HERE, DLL + '.rsp')
    with open(rsp, 'w') as f:
        f.write('\n'.join('"%s"' % o.replace('\\', '/') for o in objs))
    t0 = time.time()
    r = run([CLANG] + LINK_FLAGS + ['-shared', '-O1', '-g0' if PREBUILT else '-g', '-w', '-fno-strict-aliasing', '-I', HERE, '-I', GEN, SHIM, '@' + rsp,
             '-o', dll, '-Wl,--out-implib,' + os.path.join(HERE, GAME_IMPLIB), '-Wl,-Map,' + dll + '.map'])
    if r.returncode:
        say((r.stdout + r.stderr)[-4000:])
        return 1
    say('linked %s: %.0f MB in %.0fs (+ %s)' % (DLL, os.path.getsize(dll) / 1048576.0, time.time() - t0, GAME_IMPLIB))
    progress('Finishing game setup',1,4,7)
    return 0


# ---- host runtime + exe -----------------------------------------------------------------------------------
def compile_host(job):
    src, obj = job
    r = run([CLANG, '-c'] + HOST_FLAGS + [src, '-o', obj])
    return src, r.returncode, (r.stdout + r.stderr)[-3000:]


def cmd_exe(jobs=16, link=True):
    objdir = os.path.join(HERE, 'obj_host', os.path.splitext(EXE)[0])
    os.makedirs(objdir, exist_ok=True)
    jobs_list = []
    sources = ['sdkdata.c', 'sdklift_gen.c'] if PREBUILT else HOST_SOURCES
    for n in sources:
        src, obj = os.path.join(HERE, n), os.path.join(objdir, n[:-2] + '.obj')
        deps = [src]
        if n in ('nvn_vk.c','nvn_gl.c','pc_prompts.c','nnfontll.c'):
            deps += [os.path.join(HERE,h) for h in ('pc_prompts.h','pc_prompt_refs.h')]
        if n in ('hostrt.c','pc_course_world.c','display.c','nnhid.c'):
            deps += [os.path.join(HERE,'pc_course_world.h')]
        if n == 'pc_course_world.c':
            deps += [os.path.join(HERE,'pc_mii_silhouette.h')]
        if n == 'nvn_vk.c':
            deps += glob.glob(os.path.join(HERE, 'nvn_vk*.c')) + glob.glob(os.path.join(HERE, 'pc_post*.*')) + glob.glob(os.path.join(HERE, 'pc_framegen*.*')) + glob.glob(os.path.join(HERE, 'pc_dynamic*.*')) + glob.glob(os.path.join(HERE, 'pc_video*.*')) + glob.glob(os.path.join(HERE, 'pc_post_vendor', '*.h'))
        if n in ('pc_menu.c', 'pc_prompts.c', 'nnhid.c', 'nnpad_sdl.c', 'display.c', 'hostrt.c', 'nnaudio.c', 'nnmisc.c', 'boot.c', 'nvn_vk.c', 'nvn_gl.c'):
            deps += [os.path.join(HERE, h) for h in ('pc_menu.h', 'nnpad.h', 'gfx_options.h')]
        if not os.path.exists(obj) or os.path.getmtime(obj) < max(os.path.getmtime(p) for p in deps):
            jobs_list.append((src, obj))
    say('host sources to compile: %d' % len(jobs_list))
    failed = 0
    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        for src, code, err in pool.map(compile_host, jobs_list):
            if code:
                failed += 1
                say('FAILED %s\n%s' % (os.path.basename(src), err))
    if failed:
        return 1
    if not link:
        return 0
    objs = [os.path.join(objdir, n[:-2] + '.obj') for n in sources]
    if PREBUILT:
        objs += ['-Wl,--whole-archive', os.path.join(PREBUILT, 'host-support.a'), '-Wl,--no-whole-archive']
    out = os.path.join(HERE, EXE)
    t0 = time.time()
    r = run([CLANG] + LINK_FLAGS + objs + [os.path.join(HERE, 'game-icon.res'), os.path.join(HERE, GAME_IMPLIB)] + WIN_LIBS + ['-L', MSYS_LIB, '-lSDL3',
             '-Wl,-Map,' + out + '.map', '-o', out])
    if r.returncode:
        say((r.stdout + r.stderr)[-6000:])
        return 1
    say('linked %s: %.1f MB in %.0fs' % (EXE, os.path.getsize(out) / 1048576.0, time.time() - t0))
    progress('Finishing game setup',2,4,7)
    return 0


# ---- glslang shim DLL (loaded at run time with LoadLibrary) -----------------------------------------------
def cmd_glslang():
    if PREBUILT:
        shutil.copy2(os.path.join(PREBUILT, GLSLANG_DLL), os.path.join(HERE, GLSLANG_DLL))
        say('Using the included shader compiler.')
        progress('Finishing game setup',3,4,7)
        return 0
    out = os.path.join(HERE, GLSLANG_DLL)
    libs = [os.path.join(MSYS_LIB, n) for n in ('libglslang.a', 'libglslang-default-resource-limits.a',
                                                'libSPIRV-Tools-opt.a', 'libSPIRV-Tools.a', 'libSPIRV.a')]
    r = run([CXX, '-x', 'c', '-shared', '-O2', '-w', '-I', MSYS_INC, os.path.join(HERE, 'vk_glslang_shim.c'), '-x', 'none'] + libs +
            ['-o', out, '-static-libstdc++', '-static-libgcc', '-Wl,--out-implib,' + out[:-4] + '.dll.a'])
    if r.returncode:
        say((r.stdout + r.stderr)[-4000:])
        return 1
    say('linked %s: %.1f MB' % (GLSLANG_DLL, os.path.getsize(out) / 1048576.0))
    progress('Finishing game setup',3,4,7)
    return 0


def cmd_sync():
    for name in ('SDL3.dll', 'libwinpthread-1.dll', 'libgcc_s_seh-1.dll'):
        shutil.copy2(os.path.join(PREBUILT or os.path.join(MSYS, 'bin'), name), os.path.join(HERE, name))
        say('copied', name)
    licenses = os.path.join(HERE, 'licenses')
    os.makedirs(licenses, exist_ok=True)
    shutil.copy2(os.path.join(MSYS, 'share', 'licenses', 'libwinpthread', 'COPYING'),
                 os.path.join(licenses, 'libwinpthread-COPYING.txt'))
    progress('Finishing game setup',4,4,7)


if __name__ == '__main__':
    cmd = sys.argv[1] if len(sys.argv) > 1 else 'all'
    jobs = int(sys.argv[2]) if len(sys.argv) > 2 and sys.argv[2].isdigit() else 16
    if cmd == 'compile':
        sys.exit(cmd_compile(jobs))
    if cmd == 'dll':
        sys.exit(cmd_dll())
    if cmd == 'exe':
        sys.exit(cmd_exe(jobs))
    if cmd == 'glslang':
        sys.exit(cmd_glslang())
    if cmd == 'sync':
        sys.exit(cmd_sync())
    if cmd == 'all':
        if globals().get('PREBUILT'):
            # SDK objects come from the user's files; compile them while the main game builds.
            with concurrent.futures.ThreadPoolExecutor(1) as pool:
                host = pool.submit(cmd_exe, 1, False)
                rc = cmd_compile(max(1, jobs-1)) or cmd_dll()
                host_rc = host.result()
            if rc or host_rc:
                sys.exit(rc or host_rc)
            actions = (lambda: cmd_exe(jobs), cmd_glslang, cmd_sync)
        else:
            actions = (lambda: cmd_compile(jobs), cmd_dll, lambda: cmd_exe(jobs), cmd_glslang, cmd_sync)
        for action in actions:
            rc = action() or 0
            if rc:
                sys.exit(rc)
        sys.exit(0)
    say('unknown command %r' % cmd)
    sys.exit(2)
