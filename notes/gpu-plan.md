# virtio-gpu plan — REVISED 2026-10-14 (was: DEFERRED 2026-10-13)
# STATUS: EXECUTED 2026-10-14 — G0/G2/G3'/G4' COMPLETE, EXIT BAR MET

Revised scope (user 2026-10-14): Path B, guest-side only. Success bar =
Konsole open on the Plasma desktop, works-at-all (no speed gate).
Host virgl (H2) is a conditional unblocker, not in-scope by default.

## RESULT (2026-10-14, g4-final run)
Exit bar MET: Konsole open on Plasma desktop via virtio-gpu scanout.
- kwin_wayland scanning out card0 (virtio_gpu): plane-0 fb=46
  allocated by=kwin_wayland, XR24 1920x1080, crtc-0 active.
- plasmashell + konsole stable >60s (konsole PID settled at ~5% CPU).
- Guest env combo (written to /home/opi/.config/environment.d/99-vgpu.conf
  and /etc/environment.d/99-vgpu.conf, picked up on sddm restart):
  MESA_LOADER_DRIVER_OVERRIDE=zink
  VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json
  KWIN_DRM_DEVICES=/dev/dri/card0
- lavapipe (llvmpipe) Vulkan confirmed working via Wayland WSI
  (vkcube: "Selected GPU 0: llvmpipe (LLVM 21.1.8, 128 bits)").
- kwin zink path fails ("ZINK: failed to choose pdev") and falls back
  to software glamor — but scanout through virtio-gpu works fine in
  sw mode.  H2 (host virgl) NOT needed.
- B2 (behavioral IOMMU windows) implemented and verified; B3 (pm-domain
  pwr_off tracking) reverted (broke rockchip-pm-domain probe, see
  rk3588-stubs.c comment).  G0 baseline now stable (~90-100s login).

Goal: a guest program renders on the virtio-gpu card AND the result is
visible (scanout), ending with Konsole open on the Plasma session
presented through the virtio console.

Known facts that shaped this revision:
- konsole, vkcube, glmark2, and libvulkan_lvp (lavapipe) are all
  present in the KDE image (raw-scan verified 2026-10-14).
- The most recent desktop boot (run-desktop, /tmp/opencode/
  p16-armbian-b1-smp.txt, 2026-10-14 00:10) got STUCK: serial auto-
  login at ~95s, network-online reached, then serial output went
  silent at kernel time 233s amid a burst of
  `rk_iommu ... FORCE_RESET command timed out` (x4) and
  `pm-domain ... failed to set domain off` (the P16 B2/B3 deferred-
  probe storm).  QEMU (pid 719753) burned CPU until killed; left as a
  zombie under opencode-as-PID-1.  First full desktop boot AFTER the
  P16 SMP/PMU/MPIDR changes landed.  EXT4 journal was already known
  damaged by a prior force-kill and was e2fsck'd before that run —
  that run was itself force-killed mid-boot, so the journal may be
  dirty again.

---

## 1. State today (as of plan.md 5e/5f + P16 A/B1, boots kde-vgpu2..6)

Working:
- Machine: `-M rk3588,vgpu=on` creates FOUR virtio-mmio transports at
  `0xfb000000+i*0x1000`, SPIs 92-95 (net@0xfb003000 = SPI 98 per B1 note;
  see src/rk3588.c `rk3588_create_virtio_gpu()`), force-legacy=false,
  overlap priority 1 over the `gpu` storage stub.  GPU itself comes
  from CLI: `-device virtio-gpu-device[,xres=,yres=]` attached via
  qbus_realize prepending (ARM-virt pattern).
- Attach order is REVERSED vs CLI: first `-device` lands at the HIGHEST
  transport.  Current run-desktop order: net → 0xfb003000, gpu →
  0xfb002000, keyboard → 0xfb001000, tablet → 0xfb000000.
- DTB `dts/rk3588s-orangepi-5-vgpu-boot.dts` →
  `images/armbian/rk3588s-orangepi-5-vgpu-boot.dtb`: virtio,mmio nodes
  at DT ROOT (flattened Armbian tree has no /soc): net@fb003000,
  tablet@fb000000 (keeps `#cooling-cells` + phandle 0xf0 for the
  thermal zone), keyboard@fb001000, gpu@fb002000.  NOTE older node
  names in the dtb may still be misleading (gpu@fb000000 used to host
  the TABLET); Q2 rename rides with next DTB regen.
- Guest kernel 6.18: CONFIG_VIRTIO_MMIO=y; virtio_gpu probed at
  `fb000000.gpu` (or fb002000 depending on ordering) → card0 +
  renderD128 (`DRIVER_RENDER` unconditional), dumb_create + shmem GEM
  + PRIME export work, `fb0: virtio_gpudrmfb` registered.  No 3D
  contexts (`has_virgl_3d=false`).
- Input proven end-to-end (boot6): virtio-keyboard + virtio-tablet via
  qemu_input; Ctrl+Alt+T opened Plasma launcher, typed text + mouse
  moves registered.
- Full Plasma 6 Wayland session runs, presenting through the MODELLED
  VOP2 (rockchip card) with virtio serving as render-node peer.
- P16 A (SMP): 8 CPUs up at 0.09s (was 36s/1-CPU).  P16 B1: virtio-net
  + SLIRP, eth0 10.0.2.15, NM-wait-online inactive.

Not working:
- The virtio card is never modeset by any guest client: `dri/0` state
  shows `crtc-0 enable=0 active=0`, `Virtual-1 crtc=(null)` → QEMU's
  virtio-gpu QemuConsole stays BLACK.  Userspace can open renderD128
  but nothing ever displays through the virtio scanout.
- Guest GL stack fails: all DRI drivers are symlinks to
  `libdril_dri.so` (Mesa 26.0.3); chain in
  logs/p15-kde-plasma-wayland.txt:641-647:
  `MESA-LOADER failed to retrieve device information` → `could not
  connect vdrm` → `Error opening virtio-gpu device for Asahi native
  context` → `ZINK: failed to choose pdev` → `egl: failed to create
  dri2 screen`.  NOTE this is kwin_wayland_wrapper's specific failure;
  a plain es2_info/zink probe on the render node has NOT been tried
  yet (G2 is exactly that probe).
- Desktop-boot fragility (NEW, see stuck-boot evidence above): the
  00:10 run froze at ~233s in the B2/B3 deferred-probe storm.  Do not
  start GPU retarget work until a baseline boot is proven stable.
- Host container: NO /dev/dri, no DISPLAY/WAYLAND_DISPLAY, no /dev/fuse,
  no host Vulkan ICDs → virgl impossible as-is (egl-headless needs a DRM
  render node: qemu/util/drm.c qemu_drm_rendernode_open fails → abort).
- Build: CONFIG_GTK/X11/OPENGL/VNC/DBUS on; SDL/spice off; no
  rutabaga/gfxstream; libvirglrenderer linked but `virtio-gpu-gl-device`
  requires display_opengl (`-display gtk,gl=on` or egl-headless).
  AppImage bundles libvirglrenderer1 already.

## 2. Chosen scope (user 2026-10-14)

Path B, GUEST-SIDE ONLY: prove scanout, fix guest software GL, retarget
kwin onto the virtio card via KWIN_DRM_DEVICES (NOT a blacklist —
reversible), launch Konsole.  H2 (host virgl) only as a conditional
unblocker if G2/G3′ hit an unfixable virtio-GL wall.  Exit bar:
Konsole window visible, works-at-all, no speed gate.

## 3. Execution steps

### G0 — housekeeping + baseline stability (REQUIRED first, ~30 min)
1. `pkill -9 qemu-system-aarch64; pkill -9 rk3588-qemu-x86` — clear
   zombies/strays (opencode-as-PID-1 does not reap orphans).
2. Check + repair the KDE image journal: `e2fsck -fn` on the rootfs
   partition (GPT part0 'rootfs' @ LBA 32768).  If dirty beyond
   repair, re-extract from the .xz (pristine) and re-apply the known
   prep (root pw empty, sddm autologin opi — see plan.md 5d/5f).
3. Baseline boot: stock `scripts/run-desktop.py` (no GPU changes) driven
   by a kde-vgpu6-style driver script with HARD timeouts on every wait
   (no interactive tty — that shape hung the user).  Watchdog: no
   serial output for 3 min → declare hang, capture `-d unimp,
   guest_errors` tail + any pm-domain/iommu storm lines, kill QEMU.
4. Success: Plasma session up (loginctl session, kwin+plasmashell
   processes), two screendumps of the DEFAULT (VOP2) console differ
   (live pixels), 0 hard hangs.  If it hangs again at the ~233s storm,
   STOP — that is a P16 B2/B3 bug, not a GPU bug; either fix B2/B3
   first or boot with the rockchip drm/iommu nodes deferred via bootarg
   to get a stable baseline (document whichever route taken).

### G1 — fbcon scanout proof (OPTIONAL, ~5 min, one boot)
- Boot with `fbcon=map:0` (virtio registers fb0 first) or bind at
  runtime: `echo 1 > /sys/class/vtconsole/vtcon*/bind`.
- Kernel text must appear on the virtio console.  Verify with monitor
  `screendump /tmp/opencode/x-fbcon.ppm vgpu` (DEFAULT console index 0
  is VOP2 — virtio must be named) or VNC Ctrl+Alt+2 / GTK `vgpu` tab.
- Proves: guest KMS → SET_SCANOUT → qemu_console_set_surface → display.
  (QEMU side already proven: hw/display/virtio-gpu.c:1691/1694.)
- Skippable if time-boxed; G2/G3′ do not depend on it.

### G2 — software GL on the virtio render node (~15 min, serial-only)
- From serial after multi-user (NO full session needed): run on
  renderD128 (whichever /dev/dri/renderD128 is the virtio node —
  confirm via `basename $(readlink -f /sys/class/dri/renderD128/device)`
  containing virtio):
  `es2_info` / `glxinfo` / `vkcube` with
  `VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json` +
  `MESA_LOADER_DRIVER_OVERRIDE=zink` (or `LIBGL_ALWAYS_SOFTWARE=1`).
- lavapipe ICD IS installed in the image (`lvp_icd.json`,
  `libvulkan_lvp.so`) → dril→zink should get a context via CPU Vulkan.
  vkcube + glmark2 are also present for a quick visual/score sanity
  check once a context exists (score not gated this round).
- Success criterion: EGL context created, glxgears/es2_info output
  sane, vkcube paints (even to a virtual surface).  Document the
  working env combo in plan.md 5e.
- IF the vdrm/Asahi-native-context chain blocks EVERY context (the
  kwin failure at logs/p15-kde-plasma-wayland.txt:641-647 may or may
  not reproduce outside kwin): trigger H2 before continuing to G3′.

### G3′ — retarget kwin via KWIN_DRM_DEVICES (replaces blacklist; ~30 min)
- Write `KWIN_DRM_DEVICES=/dev/dri/card0` (virtio; re-confirm which
  card is virtio at runtime) into the sddm session environment:
  `/etc/sddm.conf.d/99-vgpu-kwin.conf` with
  `[X11]` / `[Wayland]` Env=… or a drop-in under `/etc/environment.d/`
  — pick whichever sddm 0.21+ actually honors (test both if unsure),
  then `systemctl restart sddm`.
- rockchipdrm STAYS LOADED (reversible; VOP2 tab/console still exists
  as fallback).  No blacklist.
- Verify: `/sys/kernel/debug/dri/0/state` (or whichever dri node is
  virtio) → crtc-0 `active=1`, connector `Virtual-1 crtc=crtc-0`;
  non-black `screendump ... vgpu`; two dumps N sec apart differ (live
  pixels: clock/cursor).
- Console note: default screendump / VNC view still points at VOP2
  (index 0); always dump by name `vgpu` for the virtio console.
- IF kwin fails to modeset virtio or crashes without a GL context
  (G2 env applied) → trigger H2, then retry.

### G4′ — Konsole on the virtio-presented session (the actual user goal)
- Konsole is confirmed present in the image.  Launch WITHOUT needing
  the GUI launcher first (more reliable than the boot6 Ctrl+Alt+T
  path): from serial,
  `sudo -u opi DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus
  XDG_RUNTIME_DIR=/run/user/1000 kstart konsole` (or `qdbus
  org.kde.plasmashell /ShellService org.kde.Shell.loadScript …` —
  try kstart first).  Fallback: inject Ctrl+Alt+T + type "konsole" +
  Enter via qemu_input (proven pattern in boot6).
- Verify: konsole window visible in `screendump ... vgpu`; typed
  characters appear (send a `echo KONSOLE-OK` via the window if
  focused, or just the window title/prompt presence is enough for
  works-at-all).  Record any kwin-env needed (e.g.
  `KWIN_FORCE_SW_CURSOR=1`) in plan.md.

### H2 — conditional host virgl (only if G2/G3′ blocked on virtio GL)
- qemu.patch: fall back to `EGL_PLATFORM_SURFACELESS_MESA` + host
  llvmpipe in `ui/egl-helpers.c` when `qemu_drm_rendernode_open()`
  fails; then `-display egl-headless,gl=on -device
  virtio-gpu-gl-device`.  Most likely host-side path that works in
  this container (no SDL built, no DRI3 on Xvfb).  Regenerate patch
  per Rule #1 file list.  Adds real 3D; removes the "no host GL" dead
  end.  gfxstream/rutabaga stays out (too heavy).

## 4. Verification harness
Reuse the driver pattern (`scripts/run-desktop.py` + boot driver
scripts in /tmp/opencode/kde-vgpu*.py):
1. boot to session (or multi-user for G2), 2. guest probes (`dri/0
   state`, `es2_info`/`vkcube`), 3. `screendump` of BOTH consoles
   (`vgpu` + default), 4. liveness diff (unique-color count + pixel-diff
   vs a dump N seconds later), 5. paste evidence into plan.md.
Every driver wait gets a hard timeout; a 3-min serial-silence watchdog
aborts the run and captures diagnostics instead of hanging.

## 5. Related polish (deferred with this file)
- Q1 (QEMU): make the virtio console default when vgpu=on — defer
  rk3588_display_init() so VOP2 console isn't index 0; small but
  touches src/.  Out of current scope.
- Q2 (trivial): rename DTB nodes (gpu@fb000000→tablet,
  tablet@fb002000→gpu; keep thermal phandle on a node).  Purely
  cosmetic; do together with any future DTB regeneration.
- plan.md 5e loose ends: `busctl --user kwin supportInformation`
  retry; dump ESmart2 CTRL regs 0xfdd91c00 to identify kwin's scanout
  window; VOP2 dsp_hold timeout on DPMS (listed in P16 B6).
- P16 B2 (behavioral IOMMU) and B3 (pm-domain off path) — these are
  the suspected root cause of the 2026-10-14 stuck boot; if G0's
  baseline still hangs in that storm, promoting B2/B3 above GPU work
  may be necessary (user call).

## 6. Exit evidence bar (when executed)
Per Rule #6: paste output of — non-black `screendump ... vgpu` (PPM
with structured content + color count + live diff vs later dump)
showing the Konsole window, `dri/<virtio>` state with crtc active, and
a working guest GL context (`es2_info` output).  Then update plan.md
(5e completion note + scope line) and rebuild/smoke the AppImage
(Rule #8) only if any ./src change occurred (none expected for pure
guest-side work; H2 would touch qemu.patch instead).
