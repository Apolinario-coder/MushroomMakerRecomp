# Mushroom Maker Recomp

Experimental Windows PC port project for **Super Mario Maker 2, version 3.0.3**, using static AArch64-to-C recompilation and a native PC compatibility layer.

## Current status

This repository currently publishes project documentation. **No playable release or downloadable v1.0.0 package has been published.** Local development packages exist, but their redistribution review is incomplete.

The local implementation contains a Windows launcher, translation tools, C/C++ runtime sources, Vulkan graphics support, input and audio integration, PC settings, and Course World service integration. These describe the local development tree; they are not a promise that those components are available from this repository.

Compatibility, performance and stability remain experimental. No new build or gameplay test was performed for this documentation update. A Rust migration is a future goal; the current packaging is based on C/C++ and Python components.

## How the project works

1. The user supplies supported, legally obtained, complete game files with update 3.0.3 already applied.
2. Local translation tools convert supported AArch64 game code to C.
3. A compiler builds the game-derived code on the user's PC.
4. The native PC runtime provides graphics, input, audio and operating-system compatibility.

The intended distribution supplies PC-side tools and support components. Game-derived code and game libraries must be generated locally from the user's files.

## Availability and requirements

Windows x86-64 is the locally packaged platform. The development documentation specifies Windows 10/11, .NET Framework 4.8 and a Vulkan 1.3-capable GPU. Tested minimum RAM, storage and performance requirements have not been established in this publication review.

Installation instructions will accompany a verified release. Do not download unrelated packages or assume that a standalone launcher is a complete game package.

## Game files and intellectual property

No Nintendo game assets, ROMs, firmware, encryption keys, extracted executable data, generated game libraries or gameplay screenshots are included in this repository update. Instructions for obtaining copyrighted game files are not provided.

Users must supply their own legally obtained game files. Nintendo names identify the game targeted by the project; this project is not affiliated with or endorsed by Nintendo.

## Licenses and third-party components

The local package contains third-party components and inherited runtime sources. Their original notices and applicable licenses must be preserved, and redistribution obligations must be checked before publishing source or binaries. No blanket license is asserted over third-party work.

The local package's `NOTICE.md` explicitly requires confirming the provenance and license of inherited runtime sources before publication. Its README also states that recorded dependency versions and checksums do not establish a completed license-compliance audit.

Publication of v1.0.0 remains pending:

- Confirming the origin and applicable licenses of all inherited runtime sources.
- Verifying binary-to-source correspondence and any required corresponding-source distribution.
- Reviewing third-party toolchain, library and replacement-font redistribution requirements.
- Checking the final archive for proprietary game content, private keys, personal data and unexpected files.
- Validating the exact final package and recording its checksums and test results.

## Acknowledgments

The recovered development documentation credits LAS Recomp, Sky Recomp, OpenSmash-Ultimate/ssbu-recomp and TheGreatRambler's MariOver, along with the authors of the bundled libraries and tools. Attribution does not replace license compliance.

## Reporting issues

Describe your hardware, the affected menu or course, and reproduction steps. Review logs for personal information before sharing. Do not attach game files, encryption keys or proprietary assets.
