# Windows x64 port of blurrt (cross-compiled on macOS)

The Windows changes are now merged into port/harness (winport.h included) and port/work/interp. work/win holds only build scripts, launcher and cross-built deps; there is no private harness copy. build_harness.sh compiles port/harness + port/web/src + port/work/interp directly and writes work/win/blurrt.exe.
All changes are `#ifdef _WIN32`-guarded except two portable ones (marked *).

## Build
1. Toolchain: llvm-mingw 20261020 (ucrt, macOS universal) unpacked in `~/tools/llvmmingw` (x86_64-w64-mingw32-clang; supports musttail). Free, from github.com/mstorsjo/llvm-mingw.
2. Deps (static, in `deps/`, sources built in the scratchpad; recipes below):
   - unicorn 2.1.4: `cmake -DCMAKE_TOOLCHAIN_FILE=<mingw toolchain> -DUNICORN_ARCH=x86 -DUNICORN_BUILD_SHARED=OFF` -> deps/lib/libunicorn.a
   - ffmpeg 7.1.1 minimal static: `--enable-cross-compile --target-os=mingw32 --arch=x86_64 --disable-everything --disable-x86asm --disable-programs --disable-network --disable-autodetect --disable-avdevice --disable-avfilter --disable-swscale --enable-decoder=bink,binkaudio_dct,binkaudio_rdft --enable-demuxer=bink` (all bink_ff.c needs; no DLLs)
   - SPIRV-Cross vulkan-sdk-1.4.363.0: `-DSPIRV_CROSS_SHARED=OFF -DSPIRV_CROSS_STATIC=ON -DSPIRV_CROSS_CLI=OFF -DSPIRV_CROSS_ENABLE_C_API=ON` (C++ static, libc++)
   - vkd3d-shader 2.1 (third_party/vkd3d-2.1.tar.xz): `CC=x86_64-w64-mingw32-clang CPPFLAGS=-I/opt/homebrew/include SONAME_LIBVULKAN=vulkan-1.dll ./configure --host=x86_64-w64-mingw32 --enable-static --disable-shared` (SONAME_LIBVULKAN skips the libvulkan probe, vulkan headers come from brew), then `make libvkd3d-shader.la`. Needs bison >= 3: copy the pre-generated `hlsl.tab.[ch] hlsl.yy.c preproc.tab.[ch] preproc.yy.c` and `vkd3d_version.h` from the macOS build tree into the new tree (mac /usr/bin/bison is 2.3). Result: libvkd3d-shader.a + libvkd3d-common.a copied to deps/vkd3d/lib, headers to deps/vkd3d/include.
   - **deps/vkd3d/include/vkd3d_types.h is patched**: `VKD3D_IMPORT` defined empty (else every call needs `__imp_`, static link fails).
3. `./build_recomp.sh [jobs]` : gen_p2 (referenced in place) -> recomp/obj_win (same flags + sed PRE as build_fast.sh, ~2 min at -j10), and gen_x extra.c/extra_table.c -> recomp/obj_wx (these provide `rt_table_x`; the Makefile links ../recomp/obj_x/*.o, so any other Docker/Linux flow must build gen_x too or the link fails with undefined `rt_table_x`).
4. `./build_harness.sh` -> work/win/blurrt.exe (157 MB with -g, 133 MB stripped; fully static, imports only KERNEL32, WS2_32, IPHLPAPI, bcrypt, UCRT api-ms-win-crt-*: Windows 10+).
   Flags: `-include winport.h -D_FILE_OFFSET_BITS=64 (64-bit off_t for fseeko/ftello/stat) -DBLUR_COLD -DBLUR_G_CHECK -DBLUR_NO_BUDGET`, libs `-lws2_32 -liphlpapi -lbcrypt -lpthread(winpthreads) -lc++ ...`.
5. `./pack_win.sh [--no-game]` -> stage_win/BlurLAN (+ BlurLAN-win.zip with game data unless --no-game). Layout: bin\blurrt.exe, app\web + app\imports.tsv, vendor\msvcr90.dll, game\BLUR, Start.bat, Start.ps1, README.txt.

## Source changes (merge back as-is, all guarded)
- `winport.h` (new, force-included on Windows): mkdir(p,mode)->_mkdir, popen/pclose, setenv, localtime_r/gmtime_r, memmem, CLOCK_MONOTONIC_RAW, minimal fnmatch (+FNM_CASEFOLD).
- emu.h * : `CPU_RELAX()` macro (x86 `pause`, else arm `yield`); hostcommon.c / rt.c use it instead of the hard-coded arm64 `yield` asm (that did not assemble on any x86 target, incl. the amd64 Linux build). Also `Handle.dirp[1024]` under _WIN32 (FindFirstFile directory; mingw dirent has no d_type).
- main.c: `<sys/mman.h>` -> windows.h, `timeBeginPeriod(1)` at start; guest memory = `VirtualAlloc(0x7000000000, 4GB+64K, MEM_RESERVE|MEM_COMMIT)`. No MAP_NORESERVE on Windows, so this takes 4 GB of commit charge (pages are only touched on use). If that fails (low commit limit) it falls back to reserve-only plus a vectored exception handler that commits 256 KB chunks on first guest access (host-side fread into never-touched guest pages is not covered in that mode). argv[1] directory split also accepts `\`.
- rt.c: execinfo/mman includes guarded; coroutine stack mmap (aarch64-only path, never used on x86) stubbed; BLUR_WATCHW/WATCHR write-watch (SIGSEGV+mprotect) stubbed out (`ww_arm/ww_rearm` empty); SIGUSR2 spin-dump handler (`spin_sig`, pthread_kill) compiled out. rt_v1.c untouched (obsolete, not built).
- rtprof.c: getrusage -> GetProcessTimes. snapshot.c: sys/mman include guarded (not used).
- net.c: winsock2 (`read/write/close` -> recv/send/closesocket macros, `writev` -> WSASend, `strcasestr`, WSAStartup in net_start, no SIGPIPE, **no SO_REUSEADDR on Windows** (it allows port hijack there)). Server still binds 127.0.0.1 only.
- wsock_hle.c: winsock2/ws2tcpip/iphlpapi includes, `set_nonblock()` (ioctlsocket FIONBIO / fcntl), WSAStartup, SO_REUSEADDR skipped, `SIO_UDP_CONNRESET` disabled on the mesh socket (otherwise ICMP unreachable becomes WSAECONNRESET on recvfrom), interface enumeration `mesh_enum()` via GetAdaptersAddresses: picks ethernet/wifi with a gateway and private range, penalises Hyper-V/WSL/VirtualBox/VMware/Docker adapters, directed broadcast from OnLinkPrefixLength.
- hle2.c: fnmatch include guarded; FindNextFile dir test via stat() (no d_type); localtime_r/gmtime_r via winport.h.
- interp/interp_glue.c: `SIGUSR1` fallback define.
- Makefile (harness): not used; build_harness.sh replaces it. Linux's `-fsigned-char` is irrelevant (x86 char is signed).

## Tested
- Everything compiles and links. Smoke test under Wine Devel 11.18 (macOS, x86_64 via Rosetta, use `Contents/Resources/wine/bin/wine`; the `Contents/MacOS/wine` launcher drops argv): game loaded, Unicorn + recompiled path entered, GetAdaptersAddresses mesh picked the right NIC (192.168.0.113 -> 10.77.0.113), HTTP server serves index.html/renderer.js (200), websocket client received 297 MB / 6290 frames, 807k draws in the first 24 s, clean exit via BLUR_SECONDS. Packaged layout (stripped exe in bin\, cwd app\, backslash game path) also verified.
- NOT tested: real Windows (no Windows machine here), Start.bat/Start.ps1 (no PowerShell on the Mac; syntax not machine-checked), firewall rule / UAC elevation, real LAN mesh between two PCs, Chrome/Edge flags, the VEH commit fallback, low-commit-limit behaviour.

## Risks
- Commit charge: 4 GB commit at startup; on a PC with small pagefile + low RAM VirtualAlloc may fail -> VEH fallback (less tested). 
- 0x7000000000 must be free at process start; ASLR normally leaves the low 128 TB sparse, but a hostile AV/injection DLL could land there (BLUR_G_CHECK aborts with a clear message).
- Windows Defender / SmartScreen may flag an unsigned 133 MB exe; Start.ps1 runs Unblock-File on bin/app/vendor, user may still see SmartScreen once.
- Windows 7/8 need the UCRT update (api-ms-win-crt-*).
- The mesh uses UDP 47801 only; Windows may classify the network as "Public": the rule is added with profile=any.
- Timing: main() calls `timeBeginPeriod(1)` (winmm) so usleep/waits are 1 ms granular; clock_gettime is winpthreads/QPC. Frame pacing on real Windows is unverified.
- Console subsystem exe; launcher hides the window (stderr -> blur.log).

## Launcher notes
- Firewall rule uses New-NetFirewallRule (inbound UDP, program-scoped, all profiles) via an elevated `powershell -EncodedCommand`, not netsh: no quoting issues with spaces in the path. Marker `.firewall_done` stores the exe path; re-prompts if the folder moves.
- Browser args are passed through Start-Process as an array, so the user-data-dir argument is explicitly wrapped in quotes (paths with spaces).
