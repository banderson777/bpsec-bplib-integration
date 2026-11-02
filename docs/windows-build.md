# Windows host build

For the five-node deployment, use five separate Windows Terminal sessions in
the same WSL2 distribution and follow the build and per-node commands in the
[five-node runbook](five-node-video-test.md). That procedure runs Linux cFS
executables; consult the runbook's current validation status. The native Windows
library build below does not build the five-node cFS mission.

BPLib can be built as a host library on Windows with CMake and either Visual
Studio or MinGW. The TCP CLA adapter uses Winsock2 and `WSAPoll`; it does not
require POSIX file descriptors.

These commands target the BPLib host library, not the complete integration smoke
project. Install/configure a Windows NASA OSAL package first and provide its
`NasaOsal_DIR` if CMake cannot discover it. See
[build and validation](build-and-test.md) for dependency and test boundaries.

From a Visual Studio developer prompt:

```bat
cmake -S bplib -B build-windows -G "Visual Studio 17 2022" ^
  -A x64 -DBPLIB_OS_LAYER=OSAL -DBPLIB_BUILD_TEST_TOOLS=OFF ^
  -DBPLIB_ENABLE_UNIT_TESTS=OFF
cmake --build build-windows --config Release
```

The standalone TCP CLA adapter is intended to be compiled into a host-side
application or BPNode integration target. Link that target with `ws2_32`.
The adapter initializes Winsock when `BPLib_TCPCLA_InitConfig()` is called and
uses `SOCKET`-sized handles internally.

BPNode remains a cFS application. Its Windows host build therefore depends on
the cFS/OSAL Windows BSP and a Windows-compatible PSP module. The custom
POSIX UDP PSP module is not selected by the TCP-only configuration; use the
TCP CLA adapter for Windows host networking. cFS flight targets are unchanged.

The existing POSIX smoke programs are excluded from a Windows configure because
they use `pthread` directly. A Windows-native test runner should use C11
threads or the Windows thread API and can reuse the same adapter library.
The complete integration project still uses Linux dependency search paths and
GNU-style compile flags. Winsock compatibility in the adapter does not by itself
make that CMake project or the enabled pthread-based video sink Windows-native.
