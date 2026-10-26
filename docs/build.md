# Building from the source repository

`main` is a source checkout. Ready-to-use Windows packages, including Python and the compiler, are available from [Releases](https://github.com/Apolinario-coder/MushroomMakerRecomp/releases). The repository does not track compiled applications, generated game code, portable binaries or user data.

## Launcher

On 64-bit Windows with .NET Framework 4.8:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build-launcher.ps1
```

Output: `build/launcher/MushroomMakerRecomp.exe`. This is only the compiled launcher; assemble the complete application before running it.

## Windows package

Install Python 3.13. Download a Windows release ZIP containing the inventoried portable tools, then run:

```powershell
python tools/package_release.py --portable-from "D:\Downloads\MushroomMakerRecomp-v1.0.0-Windows-x64.zip" --output dist/MushroomMakerRecomp-Windows-x64.zip
```

The tool validates every portable dependency against its embedded SHA-256 manifest, rebuilds the launcher and packages the current source tree. The ZIP has the application layout expected by the launcher: `setup.py`, `src/`, `portable/`, launcher resources, assets and defaults. The repository's `runtime/` maps to `src/` in the application. Build products remain ignored under `build/` and `dist/`.

Portable dependencies are reused as distributed; changing a host C implementation also requires rebuilding its `host-support.a` dependency with the same toolchain before packaging. This packaging command rebuilds the launcher, not the prebuilt host support archive. Runtime build scripts and game-derived code remain compiled locally by Prepare game.

No game files or generated game library are included in the package. Extract the complete ZIP and use Prepare game with your own supported 3.0.3 files. The separate `control.nacp` export is not required.

## Developer preparation

For local preparation from the source tree, install Python dependencies from `tools/requirements.txt` and provide MSYS2 UCRT64 with Clang, LLD, SDL3, Vulkan headers and glslang/SPIRV libraries:

```powershell
python -m pip install -r tools/requirements.txt
python tools/setup.py prepare --dump "D:\My game files" --toolchain "C:\msys64\ucrt64"
```

Generated game code, compiled objects, saves, settings and logs are stored in the existing user profile. The setup script supports both this repository layout and the packaged application layout.

## Checks

```powershell
python -m unittest discover -s tools/tests -v
```

These checks cover the source-only layout, NACP-independent file discovery, the optimized retry-build selection and portable package extraction rejection/verification. Full gameplay requires the user's game files and is not exercised by CI.
