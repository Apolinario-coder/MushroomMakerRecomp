# Changelog

## Unreleased

- Keep main focused on sources, documentation and open assets. Compiled launchers and portable dependencies remain in release downloads.
- Organize runtime sources under `runtime/`, launcher sources under `tools/launcher/` and preserved component notices under `third_party/`.
- Add a verified Windows packaging command and source-layout checks.
- Preserve the launcher without a separate `control.nacp` requirement and the optimized Course World death/retry build path.

## 1.0.0 maintenance

- Remove the separate `control.nacp` requirement; supported executable hashes still identify version 3.0.3.
- Repair the compiler helper used during preparation.
- Optimize 17 heap/resource construction units used by course reconstruction after death while retaining fast compilation for other units.

See the [release downloads](https://github.com/Apolinario-coder/MushroomMakerRecomp/releases) for packaged applications and release-specific validation notes.
