# third_party

Nothing is vendored here. `setup.sh` downloads and builds into this folder (git-ignored):

| what | version | where from | used for |
|---|---|---|---|
| vkd3d (only `libvkd3d-shader`, static) | 2.1, SHA-256 `7510146aff2adfb4ae07ab890701a607e5ff7c66e57100cfc9f630ec92eeda6a` | https://dl.winehq.org/vkd3d/source/vkd3d-2.1.tar.xz | D3D9 shader bytecode to SPIR-V (LGPL-2.1-or-later) |
| Unicorn Engine (Linux only) | 2.1.4 | https://github.com/unicorn-engine/unicorn | x86 emulator that runs the packer stub; macOS uses the Homebrew package (must be 2.x) |

On macOS the other libraries (Unicorn, FFmpeg, SPIRV-Cross) come from Homebrew; on Linux from the distribution
(see `setup.sh`). See `NOTICE` for licences.
