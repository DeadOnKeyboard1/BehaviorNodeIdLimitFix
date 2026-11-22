# Behavior Node ID Limit Fix 2.5.0

Behavior Node ID Limit Fix is a native SKSE plugin by DeadOnKeyboard. It mitigates Havok state-machine ID collisions in large Skyrim SE/AE behavior graphs. The plugin validates and compacts eligible graphs, and caches ID plans and topology data for subsequent loads.

## Runtime requirements

Install the SKSE build and the regular Address Library for your Skyrim runtime. The plugin is self-contained and uses the behavior files already present in your game.

| Skyrim runtime | Matching SKSE | Compatibility evidence |
| --- | --- | --- |
| **1.7.104.0** | **2.3.1** | Primary target, hooks and Havok map writer statically verified |
| 1.6.1170.0 | 2.2.8 | Hooks and Havok map writer statically verified |
| 1.5.97.0 | 2.0.20 | Hooks and Havok map writer statically verified |

The 2.5.0 build passed 467 automated checks and the maintainer confirmed that it works in-game. The confirmation did not identify the runtime used for the test.

Place `BehaviorNodeIdLimitFix.dll` and `BehaviorNodeIdLimitFix.ini` under `Data/SKSE/Plugins`. The INI controls offline graph preparation, the disk cache and performance logging. See the [changelog](CHANGELOG.md) and [technical documentation](docs/GraphSlimming.md) for details.

## Build from source

Use an x64 MSVC developer shell with C++23 support, CMake, Ninja and Git. The tested build used MSVC 19.51 and CMake 3.31.8. Prepare the following dependencies in an external directory or the ignored `deps` directory:

- [CommonLibSSE-NG 16.7.0](https://github.com/alandtse/CommonLibSSE-NG/tree/0174fa6c22c59a1bbd89dd01fae6d09d3f2aaef8) at `0174fa6c22c59a1bbd89dd01fae6d09d3f2aaef8`. The project configures the dependency for SE and AE.
- [MinHook v1.3.4](https://github.com/TsudaKageyu/minhook/tree/v1.3.4), which supplies the instruction decoder used by the build.
- A dependency prefix containing `directxmath`, `directxtk`, `fmt`, `rapidcsv` and `spdlog`. Use compatible x64 Release libraries with the MSVC dynamic CRT. For vcpkg use `x64-windows-static-md`, with the baseline `ee12231b20c95013c6638d845d04c91559a1d1ff` from the pinned CommonLib manifest.

From the repository root, set `$prefix` to the installed dependency prefix and run:

```powershell
$commonlib = (Resolve-Path 'deps/CommonLibSSE-NG').Path
$minhook = (Resolve-Path 'deps/minhook').Path
cmake --preset build-release "-DCOMMONLIB_ROOT=$commonlib" `
    "-DFETCHCONTENT_SOURCE_DIR_HDE64=$minhook" "-DCMAKE_PREFIX_PATH=$prefix"
cmake --build --preset release
ctest --test-dir build/review-1.0.1 --output-on-failure
```

The Release DLL is written to `build/review-1.0.1`. CMake builds the plugin from the source in this repository and uses the dependencies from their configured locations.

## License

This project is licensed under [GPL-3.0-or-later](LICENSE). CommonLibSSE-NG is built from the pinned revision above under its own terms, including its [additional permissions](LICENSES/CommonLibSSE-NG-EXCEPTIONS.md).
