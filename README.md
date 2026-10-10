# rk3588 QEMU port

## Host packages required (Debian 13)

Installed so far (all via `apt-get install -y`):

| Package        | Why                                              |
|----------------|--------------------------------------------------|
| bison          | U-Boot kconfig build (zconf parser)              |
| flex           | U-Boot kconfig build (lexer)                     |
| swig           | U-Boot python libs (dtc/pylibfdt)                |
| python3-dev   | U-Boot python libs build headers                 |
| libssl-dev     | U-Boot tools (fit image signing: openssl/evp.h)  |
| libgnutls28-dev| U-Boot tools (mkeficapsule: gnutls/gnutls.h)     |
| gdb            | QEMU host backtraces on abort (no target gdb needed) |
| gdb-multiarch  | AArch64 guest debugging of the P8 DDR blob (plain gdb has no aarch64 target) |
| python3-venv   | QEMU configure creates its meson venv (ensurepip)                             |

Toolchain: `aarch64-linux-gnu-gcc` (preinstalled; used for rk and U-Boot
cross builds via `CROSS_COMPILE=aarch64-linux-gnu-`).

## Portable AppImage

```sh
make appimage        # -> rk3588-qemu-x86_64.AppImage (~32 MB)
```

Packages `build/qemu/qemu-system-aarch64` with its shared libraries
(`linuxdeploy`) and the pc-bios datadir (`AppDir/usr/share/qemu`, found at
runtime via QEMU's relocated-path rewriting), then wraps it with
`appimagetool`. Requires both tools on `PATH`; the recipe sets
`APPIMAGE_EXTRACT_AND_RUN=1` so they also work on hosts without `/dev/fuse`.

Run it like the built binary:

```sh
./rk3588-qemu-x86_64.AppImage -M rk3588 -nographic -serial mon:stdio \
    -bios rk/genbook_demo.img
# fuse-less host: prepend APPIMAGE_EXTRACT_AND_RUN=1 or use
# ./rk3588-qemu-x86_64.AppImage --appimage-extract-and-run ...
```
