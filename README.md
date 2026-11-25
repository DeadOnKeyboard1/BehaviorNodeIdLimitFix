# Behavior Node ID Limit Fix (`2.5.3`)

[![Platform: Windows](https://img.shields.io/badge/Platform-Windows-737373?style=for-the-badge)](https://learn.microsoft.com/en-us/windows/)
[![Skyrim: 1.7.104.0](https://img.shields.io/badge/Skyrim-1.7.104.0-007ec6?style=for-the-badge)](#target-runtimes--compatibility-status)
[![Edition: Universal SE / AE](https://img.shields.io/badge/Edition-Universal%20SE%20%2F%20AE-00599C?style=for-the-badge)](#target-runtimes--compatibility-status)
[![SKSE: 2.3.1](https://img.shields.io/badge/SKSE-2.3.1-e87039?style=for-the-badge)](https://skse.silverlock.org/)
[![License: GPL-3.0](https://img.shields.io/badge/License-GPL--3.0-78b900?style=for-the-badge)](LICENSE)

Behavior Node ID Limit Fix is a native C++23 SKSE64 plugin by DeadOnKeyboard (`DoK`) that mitigates Havok state-machine ID collisions in large Skyrim SE/AE behavior graphs. It validates and compacts eligible graphs safely, detects/rebuilds Havok transition maps against live invariants, deduplicates immutable graph plans in memory, and caches verified ID plans and topologies across restarts.

## Target Runtimes & Compatibility Status

* **Mandatory Primary Target:** **Skyrim AE 1.7.104.0** (SKSE64 2.3.1, Address Library Format 5)
* **Universal Flat-Rim Baseline:** Single SE + AE DLL (`ENABLE_SKYRIM_SE=1`, `ENABLE_SKYRIM_AE=1`, `ENABLE_SKYRIM_VR=0`; `StructsPost629` + `AddressLibraryV5`)

| Skyrim Runtime | Matching SKSE | Evidence & Status |
| :--- | :--- | :--- |
| **1.7.104.0** (Primary AE) | **2.3.1** | Statically verified (both call hooks, child vtables, and full Havok insert routine); **User confirmed** |
| **1.6.1170.0** (Steam AE) | **2.2.8** | Statically verified (locally unpacked analysis copy + Address Library); **Requires in-game test** |
| **1.5.97.0** (SE) | **2.0.20** | Statically verified (locally unpacked analysis copy + Address Library); **Requires in-game test** |

*Skyrim VR is not targeted. For unsupported/unverified map writers (e.g. GOG 1.6.1179.0), the plugin falls back safely without mutating the native transition table.*

## End-User Installation

Place `BehaviorNodeIdLimitFix.dll`, `BehaviorNodeIdLimitFix.ini` (and optionally `BehaviorNodeIdLimitFix.pdb` for crash analysis) under `Data/SKSE/Plugins/` (or install the release archive directly in **Vortex** / **MO2**). No `.esp`/`.esl` or original external assets are required.

See [CHANGELOG.md](CHANGELOG.md) and [docs/GraphSlimming.md](docs/GraphSlimming.md) for INI options, caching paths (`Data/SKSE/Plugins/BehaviorNodeIdLimitFix/Cache`), and technical details.

## Build From Source

Use an x64 MSVC C++23 developer environment (MSVC 19.51+), CMake (`>= 3.21`), Ninja, and Git:

* **[CommonLibSSE-NG v16.7.0](https://github.com/alandtse/CommonLibSSE-NG/tree/0174fa6c22c59a1bbd89dd01fae6d09d3f2aaef8)** pinned at commit `0174fa6c22c59a1bbd89dd01fae6d09d3f2aaef8` (`SE` + `AE` enabled)
* **[MinHook v1.3.4](https://github.com/TsudaKageyu/minhook/tree/v1.3.4)** (for `hde64` instruction decoding)
* Dependency prefix providing `directxmath`, `directxtk`, `fmt`, `rapidcsv`, and `spdlog` built for x64 Release dynamic CRT (`x64-windows-static-md`)

```powershell
cmake --preset build-release `
  -DBUILD_TESTING=ON `
  -DCOMMONLIB_ROOT="path/to/CommonLibSSE-NG-v16.7.0" `
  -DFETCHCONTENT_SOURCE_DIR_HDE64="path/to/minhook-v1.3.4" `
  -DCMAKE_PREFIX_PATH="path/to/deps"

cmake --build --preset release --parallel
ctest --test-dir build/review-1.0.1 --output-on-failure
```

## License & Attribution

* Primary License: **GNU GPL v3.0 or later (`GPL-3.0-or-later`)** (see [LICENSE](LICENSE))
* Additional Permissions: CommonLibSSE-NG v16.7.0 **Modding Exception** and **GPL-3.0 Linking Exception** (see [EXCEPTIONS.md](EXCEPTIONS.md))
* Third-Party Notices: See [NOTICE](NOTICE) (including HDE64 / MinHook BSD-2-Clause and original CommonLibSSE MIT attribution)
