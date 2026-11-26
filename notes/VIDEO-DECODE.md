# RK3588 hardware video decode on FHS — feasibility assessment

Status: assessment only (no code changes). Date: 2026-10-26.
Scope: how hard is it to get hardware video decoding working for the FHS media
player (`fl-media-player`) on Orange Pi 5B / RK3588.

## 1. Status quo

* The player decodes **everything through libavcodec** (`avcodec_send_packet` /
  `receive_frame`, demuxer + decoder + audio threads in `src/player.c`).
* Its Linux hwdec backend (`mobile/depends/fl-media-player/src/hwdec/linux.c`)
  supports **VAAPI or CUDA only**. On RK3588 neither exists → every attempt
  fails over to software decode + GL YUV upload. That is today's behavior on
  the board.
* Presentation is already zero-copy-capable: `vaapi.c` exports dmabufs via
  `vaExportSurfaceHandle(DRM_PRIME_2)` → `platform_create_pixmap_from_dmabuf()`
  (`src/platform/fiatlux.c:316`), a generic dmabuf import Fiat Lux composites.
  **Any future path that yields DRM_PRIME frames reuses this path.**
* FHS delivers FFmpeg through the **platform sysext** (`os/extensions/layers.yml`
  → `ffmpeg-libs` from RPM Fusion, unpinned), not the player RPM (nfpm ships
  only the binary). The player is *linked* against rpmfusion `ffmpeg-devel`.
* The OS image has **no media/decode stack in the rpm lock** — no ffmpeg, no
  mpp, no rga, no libva beyond what the sysext layers pull.
* Server↔player already reports a `hardware_decoded` status field, and
  `i18n/en.json` has a dormant `"transcoding_acceleration_rkmpp"` string —
  RKMPP was clearly anticipated, but no code uses it yet.

### Kernel facts (the good part)

The pinned Fedora kernel (`kernel-7.2.11-200.fc44.aarch64`, extracted from the
updates repo) ships:

* `CONFIG_VIDEO_ROCKCHIP_VDEC=m` — mainline **rkvdec2** driver
  (`rockchip,rk3588-vdec`, H.264 + HEVC, **stateless V4L2 request API**)
* `CONFIG_VIDEO_HANTRO_ROCKCHIP=y` (AV1/VP8-ish stateful path),
  `CONFIG_VIDEO_ROCKCHIP_RGA=m`, `V4L2_MEM2MEM_DEV=m`
* The 5B DTB (also Fedora's, `dtb/rockchip/rk3588s-orangepi-5b.dtb`) has
  `vdec0@fdc38000` + `vdec1@fdc40000` enabled with iommus + power-domains.

So the **kernel side largely exists** and only needs `modprobe rockchip-vdec`
(+ first-boot binding validation on real hardware). There is **no MPP in
mainline** (`/dev/mpp_service` is Rockchip BSP-only).

## 2. The wall: FFmpeg packaging

Three viable paths exist. All of them hit FFmpeg packaging:

* **rpmfusion does not enable rkmpp** — no `--enable-rkmpp`, no
  `rockchip_mpp` BuildRequires in its `ffmpeg.spec` (its hwaccel set is
  vaapi/vdpau/nvenc/nvdec/vpl/omx-mmal).
* Upstream FFmpeg (master *and* release/9.0 — exactly what rpmfusion 9.0.2
  builds) **does** contain rkmpp: `libavcodec/rkmppdec.c`
  (`h264_rkmpp`/`hevc_rkmpp`/`vp9_rkmpp`…), configure `--enable-rkmpp`
  (needs `rockchip_mpp >= 1.3.8` + `--enable-libdrm`).
* The rkmpp decoders do **not** use an `AVHWDeviceType` — they emit
  `AV_PIX_FMT_DRM_PRIME` directly and set `AV_CODEC_CAP_AVOID_PROBING`
  (must be selected **by decoder name**, not via hw_device_ctx probing).
* FFmpeg has **no stateless v4l2-request decoders upstream** (only the
  stateful `*_v4l2m2m`, which cannot drive rkvdec2), so the mainline-kernel
  path needs an out-of-tree patch stack.

## 3. The three paths

### Path A — Rockchip MPP (`*_rkmpp` decoders) — best architectural fit

Stack: vendor MPP kernel module → `librockchip_mpp` → FFmpeg `--enable-rkmpp`
→ player selects `h264_rkmpp`/`hevc_rkmpp` by name → DRM_PRIME frames →
existing dmabuf pixmap import → Fiat Lux composites.

| Piece | State | Work |
|---|---|---|
| MPP kernel driver (`/dev/mpp_service`) | **absent** — BSP-only, not in Fedora | build against `kernel-7.2.11-200.fc44` as akmod (like `kmod-nvidia` in `os/manifest.yml`) or DKMS; udev/group so the `fhs:fhs-media` unit can open it. **Highest risk:** BSP drivers vs 7.2.x API churn |
| `librockchip_mpp` ≥ 1.3.8 | no Fedora/rpmfusion package | new RPM built from `rockchip-linux/mpp`; ship runtime lib in `platform-media` layer **+ devel at build time** (CI/build-base) |
| FFmpeg with `--enable-rkmpp` | rpmfusion won't do it | rebuild ffmpeg-libs spec (mind `libavcodec-free` Conflicts/split) substituted into the platform media layer, **or** a private FFmpeg inside the player sysext with renamed sonames/RPATH. CI and runtime must agree |
| Player patch | none exist | ~2 files: rkmpp branch in `hwdec/linux.c` (name-based select, no hw_device_ctx/get_format, cf. `hwdec/android.c:122`) + new `video_renderer/drm_prime.c` cloned from `vaapi.c`'s export path; `meson.build` line. Fallback logic (`reopen_video_in_software` etc.) already generic |

Coverage: H.264/HEVC/VP9 (what `rkmppdec.c` implements), 8K-capable on RK3588.

### Path B — mainline rkvdec2 (`rockchip-vdec`) + v4l2-request FFmpeg

Kernel + DTB **already done**. Remaining: `modprobe rockchip-vdec` and verify
binding on hardware (cheap first experiment), but FFmpeg must gain the
out-of-tree **v4l2-request patch series** (H.264/HEVC stateless) — i.e. all of
Path A's packaging work **plus a maintained patch stack** on every FFmpeg
bump, plus the same player DRM_PRIME renderer. More maintenance than A for
less codec coverage; only wins if staying BSP-driver-free matters.

### Path C — `libva-v4l2-request` VAAPI shim (least player work)

An out-of-tree VA-API driver over the mainline stateless driver, dropped into
the `platform-video` layer (`LIBVA_DRIVERS_PATH` is already set by
`fl-media-player.service`). **Zero player changes** — the existing VAAPI +
dmabuf-export path works as-is. Risks: experimental driver, HDR/10-bit and
modifier-export validation, and it inherits Path B's codec set.

## 4. Recommendation

1. **Short term / cheapest proof:** on the board, `modprobe rockchip-vdec`
   and confirm the mainline driver binds (`v4l2-compliance` or a stock
   `ffmpeg` v4l2 request-API test if available). This validates kernel+DTB
   regardless of chosen path — do this first, it costs an SSH session.
2. **Primary bet: Path A (MPP).** It has the widest codec/quality coverage,
   the player-side change is small and self-contained (~2 files + meson, on a
   backend abstraction that already exists), and the DRM_PRIME renderer work
   is shared with any other path. The real gamble is scoped early: build the
   MPP module against the pinned Fedora kernel *before* touching FFmpeg — if
   that fails, fall back to B/C which need no BSP module.
3. Sequencing: (a) MPP module spike vs (b) `rockchip-vdec` bind test in
   parallel → (c) FFmpeg build strategy decision (platform-layer rebuild vs
   private sysext FFmpeg) → (d) player patch (same for all paths) →
   (e) validate `hardware_decoded=true` + playback via the existing IPC status.

## 5. Change-point inventory (for sizing)

* `os/manifest.yml` — new kmod/RPM package entry (A only)
* new package spec under `os/` (MPP module + `librockchip_mpp`) (A)
* `os/extensions/platform/build.sh` + `layers.yml` — custom FFmpeg into
  `platform-media` (A/B) or VA driver into `platform-video` (C); note the
  dnf-closure check forces every pulled dependency into `layers.yml`
* `fl-media-player/mise.toml` `deps` task / build-base container — devel
  packages for linking (A/B/C differ)
* `mobile/depends/fl-media-player/src/hwdec/linux.c` + new
  `src/video_renderer/drm_prime.c` + `meson.build` (A/B; C = zero)
* `deploy/fl-media-player.service` — device group for `/dev/mpp_service` (A)
* `os/selinux/fl_media_player.te` — domain is already `permissive`, likely no
  policy change needed (AVCs still logged)

Estimated relative effort: **C < A < B** for packaging-included total cost,
but **A < C < B** for long-term maintainability once the MPP module question
is settled. Player-side work is identical (~small) for A/B and zero for C.