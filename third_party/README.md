# Third-party components

This directory preserves component license notices and the Python dependency origin record. Open replacement fonts retain their own licenses and provenance in `assets/fonts_open/`.

Redistributed Python, Clang, SDL, glslang and other compiled tools are supplied with the Windows release, not committed to the main source tree. Their versions and hashes remain inventoried in the release ZIP's `portable/manifest.json`, `portable/python-origin.json` and `portable/toolchain.zip` origin metadata.

`tools/package_release.py` verifies the portable manifest before reusing those dependencies from an existing Windows release. It rebuilds the launcher and includes the current runtime and setup sources. It does not bundle generated game code or game files.
