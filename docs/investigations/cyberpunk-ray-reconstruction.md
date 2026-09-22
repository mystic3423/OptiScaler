# Cyberpunk 2077: Ray Reconstruction

## Owner report (2026-09-19)

Enabling RR produced heavy noise across the image. OptiScaler appeared to switch
from DLSS to FSR. Switching its output back to DLSS did not crash, but the noise
remained. Logging was disabled; the reason for the switch is not established.

Game directory: `C:\Program Files (x86)\GOG Galaxy\Games\Cyberpunk 2077\bin\x64`.
The on-disk `nvngx_dlssd.dll` file version was `310.9.1.0`. This is not confirmation
of the DLL actually loaded by NGX (including possible OTA selection).

## Code inspection and diagnostics

DX12 NGX creation routes RayReconstruction requests to the DLSSD backend. At the
initial diagnostic stage it created one full-frame feature with no RR tiling. Backend
availability/initialization failures can fall back to FSR 2.1.2. DLSSD is stored
as DLSS in the upscaler configuration, so the configuration label is insufficient
to establish whether RR is active.

Added Debug-level `RR diagnostic:` entries to `DLSSDFeature_Dx12.cpp`:

- Incoming and effective creation parameters, OptiScaler handle, dimensions,
  flags, denoise/roughness/depth modes, and output-subrect setting.
- First evaluation and input-size changes, capped at eight snapshots per feature,
  plus one snapshot at the first evaluation failure.
- Color/output/depth/MV, albedos, normals/roughness, diffuse/specular hit distances,
  reflection MVs, exposure, pre-transparency color, and DOF guide resources.
  Logs distinguish failed parameter reads, null resources, and supplied values.
- Resource allocation descriptions, supplied subrect bases where mapped, MV
  scales, jitter, exposure scalars, reset, and raw 4x4 camera-matrix storage rows.
- Explicit RR creation failure and first evaluation success messages. The
  feature provider reports GPU capability/path availability when RR is unavailable.

This is a targeted resource list, not a complete audit of all optional RR inputs.
Allocation dimensions do not establish active sampling regions. Diagnostics only
read parameters/resources and do not change NGX rendering parameters or tile SR.
RR-only parameter names were checked against NVIDIA's
[RR definitions](https://github.com/NVIDIA/DLSS/blob/main/include/nvsdk_ngx_defs_dlssd.h).

The installed INI was backed up and changed to `LogToFile=true`, `LogLevel=1`,
`SingleFile=false`. Each launch writes `OptiScaler_<number>.log` beside the DLL
with the existing default filename. New source diagnostics require the new DLL.

## First diagnostic session (2026-09-19)

Owner launched with RR still enabled from the previous session, observed the
immediate FSR fallback, then manually selected DLSS. Inspected the installed
`OptiScaler_1758802909100.log` (02:10:59-02:12:30):

- Line 2259: OptiScaler intercepted a Ray Reconstruction creation request.
- Lines 2270-2306: incoming/effective dimensions were 3840 x 720 -> 11520 x 2160,
  with no tiling. Flags=11, LowResMV=true, HDR=true, inverted depth=true,
  jittered MV=false, auto exposure=false, quality=3 (Ultra Performance), denoise
  mode=1, packed roughness mode=1, hardware depth=1, output subrects enabled.
- Line 2307, 02:11:10.648: native RR creation failed with `0xBAD0000D`.
  The vendored SDK identifies this as `NVSDK_NGX_Result_FAIL_OutOfGPUMemory`.
- Line 2308: initialization failure explicitly triggered FSR 2.1.2 fallback.
- Line 87099, 02:12:06.952: manual backend change to DLSS. The subsequent
  `DLSSFeatureDx12` path created ordinary SR tiles 0/1/2 successfully (lines
  87241, 87330, 87419), each outputting 3840 pixels. This was not an RR retry.
- No RR evaluation snapshot or first-success message occurred. The resource/MV/
  matrix diagnostics therefore have not yet captured RR evaluation inputs.

This establishes interception and the fallback sequence. The sustained noise is
consistent with ordinary SR/FSR receiving an RR-oriented noisy input without RR
denoising; the log does not inspect pixel contents or independently prove that
image-quality explanation. `OutOfGPUMemory` alone does not distinguish exhausted
VRAM from an internal allocation problem at the full output dimensions. Streamline
reported about 8.10 GB total VRAM used shortly before the creation attempt; that
is not a measurement of peak memory demand during the failed call.

Next useful control: enable RR at 3840 x 2160 with the same DLL/settings and
capture its successful creation/evaluation, if possible. Compare with the failed
Surround request before implementing the three-handle RR path. No RR tiling or
agent GPU validation has been performed; this evidence is from the owner's run.

## Single-monitor control (2026-09-19)

Inspected `OptiScaler_1764624146732.log`. It begins at the previous Surround
dimensions, repeats the full-frame RR creation failure, then switches to 4K:

- 02:22:14: RR created successfully (OptiHandle 1000001, native handle 2),
  reporting DLSSD v310.9.1. Its first evaluation succeeds at line 110811.
- The RR feature directly consumes 1280 x 720 input and produces 3840 x 2160
  output (Ultra Performance). No ordinary DLSS SR feature creation appears in
  this log. Denoise mode=1 corresponds to NVIDIA's DLUnified mode: RR includes
  upscaling in this integration, rather than requiring a separate SR feature.
- 02:23:05: backend recreation explicitly selects DLSSD, creates native handle
  3, and evaluates successfully again (line 153480). No RR evaluation failures
  were found. The first 4K instance had effective AutoExposure=true (flags 75);
  the second has AutoExposure=false (flags 11), matching the original request.
  Thus this session is not a perfectly isolated resolution-only comparison.
- Captured input allocations for color, depth, MVs, diffuse/specular albedo,
  normals, and specular hit distance are all 1280 x 720; output is 3840 x 2160.
  LowResMV=true; MV scales are 1280 and 720. Logged populated subrect bases are
  zero. Roughness mode is packed, with no separate roughness texture.
- Specular hit distance and both camera matrices are supplied; reflection MVs
  are null. This makes camera-projection correspondence relevant to RR tiling.
  Diffuse hit distance, exposure texture, pre-transparency color, and DOF guide
  are null in these snapshots. This is not an exhaustive optional-input audit.
- Stale render-subrect width 3840 appears during the first 4K creation but is
  correctly 1280 by evaluation. Record this without treating it as an evaluation
  failure; the native call succeeded.

The owner's observation that RR performs the upscaling agrees with this trace
and NVIDIA's [DLUnified definition](https://github.com/NVIDIA/DLSS/blob/main/include/nvsdk_ngx_defs_dlssd.h).
This provides a working RR API baseline at one tile's intended dimensions. It
supports investigating a width-dependent failure but does not prove RR's
internal doubled-width check. Next implementation is three RR handles with
their own histories and correctly addressed guides/projection, not RR followed
by three additional SR handles. Successful API results are not agent GPU capture
or independent image-quality validation. The SR width gate itself was not
exercised by successful RR calls, since RR uses its separate backend.

## Build verification

Release x64 compiled and linked with zero errors (61 warnings in the existing
project). Used `/p:PreBuildEventUseInBuild=false` to skip the previously documented
metadata event issue; generated headers and build scripts were not changed.
Packaging still printed a missing auxiliary copy path. The diagnostic DLL was
produced; this does not validate the complete auxiliary distribution or runtime RR.

Installed the diagnostic build as the game's `dxgi.dll` while Cyberpunk was
closed. Previous DLL: `dxgi.dll.before-rr-diagnostics-20260919-020914.bak`.
SHA256 matched both built copies (`OptiScaler.dll` and `dxgi.dll`) and the
installed DLL: `1565897AF8D14D18C05D8C03738EADBF9C588E160B0A97EC2DC0191A6AB1EBE1`.

Follow-up: added the owner's quick `TargetWidth() < 8192` single-feature SR
selection in DX11/DX12, preserving existing tiling selection for wider outputs.
Release x64 built successfully with the same prebuild-event skip and existing
packaging warning. Installed this build for the single-monitor control test;
the previous diagnostic DLL is backed up as
`dxgi.dll.before-output-width-check-20260919-021951.bak`. Built/installed hashes
matched: `72DD0C269DF154E2692DE38383134BB71C3CCD76534FD7DFFE5591D2A5C4A4F3`.
RR diagnostics remain enabled. In-game validation of the width check is pending.

## Experimental tiled RR implementation (2026-09-19)

Edited the DX12 RR source directly; no unapplied patch. `TargetWidth() < 8192`
uses the existing single feature. Wider valid layouts create three independent
RayReconstruction features; SR requests retain their existing SR backend. DX12
backend reselection now preserves the original game feature ID after fallback
or resolution-driven recreation, so selecting DLSS on an RR handle retries RR.

`DLSSDTiling.h` defines RR guide offsets separately from SR. Albedos,
normals/roughness, hit distances, ray directions, and mapped optional color guides
use render-space origins plus the caller's bases. Core color/depth/MV/output
offsets use the existing scope. Active resource regions are checked against
allocation bounds. Cleanup restores parameters and output/projection pointers
on failure too. Tiling leaves jitter, exposure, and MV displacement scales intact.

For the captured row-major, row-vector convention, each projection uses
`clip.x' = (fullWidth / tileWidth) * clip.x +
((fullWidth - 2 * tileOrigin - tileWidth) / tileWidth) * clip.w`.
Other clip components and world-to-view are unchanged. This gives the same rays
in a tile-local coordinate system; whether the DLL expects this interpretation
with subrect inputs remains an in-game validation point.

Uneven inputs use the SR integer-crop geometry: cached expanded RR handles,
one temporary UAV output, and integer copies to the monitor regions. It retains
approximate alignment and no blending. Histories reset on input-size changes,
path changes, and failed frames. Extra histories/scratch output increase VRAM use.

Initial supported contract: render-resolution MVs and camera matrices, matching
Cyberpunk's captured hit-distance inputs. Supplied reflection MVs, 3D MVs,
high-resolution depth, view-space positions, legacy ray-tracing hit distance,
and separate output alpha are explicitly rejected because their addressing/copy
contracts are not implemented. DX11/Vulkan RR are unchanged.

Validation: Release x64 compiled/linked with zero errors and existing warnings,
using the documented prebuild-event skip. CPU checks pass the 81,910 existing
geometry cases plus RR matrix correspondence, guide offsets with nonzero bases,
unchanged MV/jitter/exposure/view values, absent optional inputs, and restoration
after simulated early exits at each tile. These tests do not load NGX or exercise
GPU resource states. Routing and native-handle cleanup were inspected in source,
not exercised in a mock NGX runtime. The new header is in the VS project/filter.

Next test: 11520 x 2160 with RR enabled, initially using the captured Ultra
Performance setup. Check three distinct RR handle IDs and successful evaluation,
then inspect stationary detail, slow pans, reflections, cross-monitor motion, and
RR on/off transitions. Single-monitor RR and ordinary SR should retain their
respective paths. Tiled RR GPU validation is pending.

Installed the experimental DLL with the game closed. Previous DLL backup:
`dxgi.dll.before-rr-tiling-20260919-031121.bak`. Built `OptiScaler.dll`, built
`dxgi.dll`, and installed `dxgi.dll` SHA256 matched:
`8F392A314CB7933AC54698C5F0FA1BA0C515DC2D166EDCEF4E478322577DA3B6`.

## First tiled run: duplicated/ghost images (2026-09-19)

Owner reports the game runs, but a chimney on the middle monitor appears as
squeezed ghost copies near the middle of the other monitors. This is an image
quality failure despite successful native calls. Whether the duplicates persist
when stationary has been asked but not yet established.

`OptiScaler_1797016139873.log` begins with successful untiled 4K RR, then creates
Surround RR at 03:15:38. Three distinct native handles (2/3/4) are created, each
1280 x 720 -> 3840 x 2160. The first frame logs render origins 0/1280/2560,
output origins 0/3840/7680, reset=true, and successful evaluation. No integer crop
was used. Supplied core/guide input allocations are 3840 x 720; MV scales remain
3840/720. Those facts do not establish how NGX sampled each guide internally.

The previous resource snapshots were before per-tile modifications. Added
first-frame readbacks after offsets/projection/output changes, immediately before
each NGX evaluation, to distinguish the intended geometry from effective params.

Controlled experiment: preserve the game's original projection pointer/matrix,
keeping the existing resource offsets and MV scales. The prior off-center crop
was mathematically consistent but not established as RR's expected subrect-camera
contract. Projection and ignored/misinterpreted guide subrects are competing
hypotheses; this change is not a confirmed fix. The crop helper remains available
in code for comparison; CPU scope tests now cover both modes. All 81,910 geometry
cases and RR checks pass. No additional graphics setting was changed.

If this test still repeats geometry, investigate actual guide sampling with a
GPU capture or a separate tightly sized input-copy experiment; successful Set/Get
readback alone cannot prove that the DLL uses a subrect parameter.

Release x64 built with zero errors. Installed the projection-control DLL with
the game closed, retaining the previous build as
`dxgi.dll.before-rr-projection-control-20260919-032051.bak`. Built/installed SHA256:
`2EF6EFF01516F3E3A144ADF8129ECC235B1BC6826A6A93F7C09B00ACC7A8EC46`.
Owner comparison is pending; this is not a validated image-quality fix.

## Projection-control result and external input research (2026-09-19)

Owner now reports a modest improvement, but stationary repeated images persist,
along with heavy motion ghosting and grain while moving. This build does not
resolve the image-quality failure. Latest log: `OptiScaler_1801525470509.log`.
Effective parameter snapshots immediately before each NGX call show core inputs,
albedos, normals, and specular hit distance at X bases 0/1280/2560, with render
subrect 1280 x 720 and MV scales 3840/720. Both recorded RR instances create
distinct native handles and evaluate successfully. These are parameter-table
readbacks, not verification of the proprietary DLL's actual resource sampling.

Primary source inspected: [ZachHembree/OptiScaler, ffx-denoise-experimental](https://github.com/ZachHembree/OptiScaler/tree/ffx-denoise-experimental), pinned
at `18ac686ace352be17bb7f482a3212afeb25a81d8`. Downloaded public text source into
temporary storage for inspection; no external code was imported into this fork.

- `OptiScaler/upscalers/fsr31/FSRDFeature_Dx12.cpp`,
  `PrepareDenoiseConvInput`: reads color, depth, MVs, normals, packed/separate
  roughness, diffuse/specular albedos, optional bias mask and specular hit distance,
  plus camera matrices. This corroborates our basic Cyberpunk guide inventory.
- `PrepareDenoiserInput`: converts the incoming pixel-displacement scale into
  AMD UV units by dividing by render dimensions, and converts jitter into NDC.
  These are API conversions, not evidence that our NVIDIA MV scale should be
  divided by the tile count. It also derives camera basis, aspect, FOV, and motion.
- `OptiScaler/shaders/fsrd_preprocess/precompile/FSRDInputConv.hlsl`: reads
  corresponding guides using the same integer pixel coordinate; explicitly packs
  normals/roughness and radiance, and derives a depth-motion component from camera
  transforms. Includes guide/MV/depth/hit-distance visualizations useful as a
  debugging pattern. Its comments identify Cyberpunk's normals as world-space;
  this was not independently verified by a GPU capture here.
- The inspected feature/conversion path uses full render dimensions and has no
  tile-origin or NGX subrect-base handling. It is a useful input-format reference,
  not a demonstrated solution for splitting a wide frame into RR instances.
  Its AMD-specific packing and heuristic signal separation should not be copied
  into an NVIDIA-to-NVIDIA path.

NVIDIA's [Omniverse RTX Renderer 109.0.4 release notes](https://docs.omniverse.nvidia.com/materials-and-rendering/latest/rtx-renderer-release-notes/109_0_4.html)
explicitly report adding per-tile dimensions to RR, with improved tiled rendering
compatibility and reduced moving border noise (OMPE-78022). This establishes that
NVIDIA has worked on tiled RR. The note provides no NGX implementation, parameter
recipe, or mapping to Cyberpunk's loaded v310.9.1 DLL. It does not establish that
upgrading a game DLL would fix this case. Public DLSSD headers expose guide
subrect bases; those declarations alone do not establish correct behavior in
every DLL. No reusable public game-side NVIDIA RR tiling implementation was found
in this search.

Recommended next experiment, not implemented in this research pass: physically
copy each tile's core/guide input regions into tile-sized textures, evaluate into
a separate tile-sized output with zero subrect bases, then copy into the monitor
region. Keep pixel displacement and jitter units intact; validate the camera
projection against those local pixels. This removes shared-texture addressing
ambiguity and permits guide visualization/capture before RR. It costs additional
copies/VRAM and still needs resource-state/lifetime handling. Treat success or
failure as evidence about addressing, not proof of a particular internal bug.
Independent histories and boundary quality remain separate validation work.

Only investigation documentation changed in this research pass; the installed
projection-control DLL remains the same. No new build or GPU validation occurred.

## Isolated-texture diagnostic implemented and installed (2026-09-19)

Owner authorized this as temporary debugging only, explicitly excluded from the
intended final implementation. Current branch is `dlss-rr-tiling`, split from
the separate `dlss-tiling` SR work. Keep them separate until RR works correctly.
Both repository AGENTS.md and the OptiScaler-only parent `../AGENTS.md` record
this; the parent reminder survives branch switches. No branch merge/backport.

Direct source edits to `DLSSDFeature_Dx12.cpp/.h` and `DLSSDTiling.h` implement
`DebugIsolatedTextures = true`. The previous shared-resource/game-projection
control remains available by disabling that compile-time diagnostic switch.

- For each tile, copy every present supported core/guide input to its own exact
  render-sized texture. A compute shader uses integer `Load` with original XY
  bases plus tile origin, without filtering/rescaling or MV value conversion.
  This handles Cyberpunk's R32_TYPELESS depth via an R32_FLOAT SRV/output; partial
  CopyTextureRegion on depth-stencil resources is not used.
- Each tile has a private UAV output sized to the feature. Output subrect support
  is disabled at creation. Local input/output bases are zero; evaluated dimensions
  are local too. Finished output regions are copied into the game output. Uneven
  inputs retain the existing expanded/integer-crop geometry.
- Use the existing off-center tile projection with these local pixels. World-to-view,
  jitter, exposure and MV pixel-displacement scales remain unchanged. This tests
  a self-contained tile contract, not just one parameter relative to the prior build.
- Resource pointers, both input bases, dimensions, output, projection and reset
  restore after evaluation/failure. Original full-frame input pointers are saved
  once per frame so later tiles cannot accidentally copy from tile zero's copy.
- Restrict debug input formats to audited float/UNORM mappings. Unsupported inputs
  fail explicitly. Source resources stay in their existing shader-readable states;
  transitions affect only the private copies and the existing output-copy path.
- Cache private textures and immutable descriptor heaps until feature destruction.
  Descriptor entries retain source references to prevent pointer-reuse/stale-view
  bugs. This deliberately costs extra memory, including old DRS allocations, and
  shader dispatches. It is not a production memory/performance design.
- First-frame logs include `RR DEBUG ISOLATED TEXTURES`, per-resource source
  rectangles, local allocation sizes, `projection=tile-local`, and `isolatedCopies=true`.

Validation completed before deployment:

- Release x64 build: zero errors, 16 existing warnings; prebuild metadata event
  skipped as previously documented. Existing missing packaging-copy path remains.
- CPU suite: 81,910 geometry cases and RR projection/scope checks pass, plus copy
  pointer/XY/dimension restoration after simulated failure at every binding across
  three tiles. MV/jitter/exposure values remain unchanged.
- `tests/dlssd_copy_dx12_tests.cpp`: actual debug HLSL compiled/dispatched on D3D12
  WARP. Readbacks match exact pixel bytes for three independent tiles with padded
  sources and nonzero XY bases, for formats 10 (RGBA16_FLOAT), 28 (RGBA8_UNORM),
  41 (R32_FLOAT) and 39 (R32_TYPELESS depth with depth-stencil allocation flag).
  D3D12 debug layer reported no warnings/errors. This does not exercise NGX,
  production descriptor-cache lifecycle, NVIDIA hardware, or game rendering.

The first installation attempt was rejected by automatic approval review due to
a usage limit; the game DLL was not replaced. The owner's subsequent report that
the full copy looked unchanged came from `OptiScaler_1817339972967.log` (03:48–04:01).
That log still shows `projection=game`, full input allocations and no isolated-copy
markers. The installed DLL hash was still `2EF6EFF0...`, confirming this was another
projection-control run, not a test of the isolated-copy build.

After confirming the game was closed and the exact source/target hashes, retried
the normal reviewed installation successfully. Previous build retained as:
`dxgi.dll.before-rr-isolated-copy-20260919-131516.bak` in the game bin/x64 directory.
Built `OptiScaler.dll`, built `dxgi.dll`, and installed `dxgi.dll` SHA256:
`88DB9C7C53D7333F347B77619E13F367282867C4106E867115EACBA738A23FCD`.

Actual owner test of the isolated-copy build is pending. Start with unchanged
Surround/Ultra Performance/RR settings, compare stationary duplicates first, then
slow pans, motion trails and grain. No image-quality fix has been established.

## First actual copy-build run: missing packed HDR format (2026-09-19)

Owner reports the upscaler crashes/displays no image. Installed hash confirmed
the isolated-copy build `88DB9C7C...` was present. New log:
`OptiScaler_2159216991425.log`. At 13:20:06 all three native RR handles created,
then first evaluation logged `RR DEBUG unsupported input Color format=26 flags=5`.
The debug path returned failure before dispatching copies or evaluating a tile;
Streamline subsequently reported NGX failure `0xbad00000`. This repeats every
frame. The log ends with shutdown/detach; it does not establish a GPU/device crash.

This run uses R11G11B10_FLOAT (DXGI format 26) for color AND output, whereas the
earlier capture used RGBA16_FLOAT. Other captured inputs remain depth=39,
MVs/normals=10, albedos=28, hit distance=41. The debug whitelist omitted the packed
HDR format; this was an implementation omission, not evidence against tiling.

Added R11G11B10_FLOAT to the float-format copy mapping. It retains that format for
private color and output resources. Added a WARP test with independently varying
packed RGB mantissas/exponents. All five formats (10/28/41/39/26) pass exact-byte
readback across three tiles with nonzero XY offsets, and the D3D12 debug layer
reports no warnings/errors. No camera, MV, tiling geometry or RR settings changed.
This addresses the known input rejection; in-game copy/RR evaluation and image
quality still require owner validation.

Release x64 rebuilt successfully (zero errors; existing warnings and packaging
copy-path message). Installed the corrected build with Cyberpunk closed, preserving
the rejected-format build as `dxgi.dll.before-rr-format-fix-20260919-132306.bak`.
Built/packaged/installed DLL hashes matched:
`68165B438A88D82275135BD7DBBF180FC7C9DD12C015FF361870BBDE98CA3835`.
The previous game-projection backup is also still available. No rollback was
needed after identifying and correcting the concrete format rejection. Owner
test of this corrected copy build is pending.

## Owner success and first optimization (2026-09-19)

Owner ran corrected reference `68165B438A88D82275135BD7DBBF180FC7C9DD12C015FF361870BBDE98CA3835`
and reports no more major motion ghosting, grain, or repeated ghost textures.
Residual issues: occasional surfaces flicker/turn black/look corrupted, and one
smoke effect occasionally becomes more transparent then returns to normal. The
owner explicitly deferred those issues and requested optimization of this success.
This is owner-observed image quality, not agent visual/GPU-capture validation.

`OptiScaler_2167964786672.log` confirms isolated copies and tile-local projection,
native handles 1/2/3 and later 5/6/7, each 1280x720 -> 3840x2160, with first-frame
success and no matched RR copy/evaluation error markers. There is also a successful
single-feature RR instance between the tiled instances. Captured inputs include
Color format 26, Depth 39 copied to 41, MVs/Normals 10, albedos 28, hit distance 41,
and optional bias mask 61, ColorBeforeParticles 10 and subsurface-scattering guide
54. These optional guides are physically copied too. No timing series isolating
copy cost was found; log timestamps include CPU/logging overhead, not GPU timings.

Preserved the reference before editing in `x64/rr-reference-68165B43/`: exact DLL,
latest successful log, a snapshot of all modified/untracked source files, and a
manifest with branch/base commit. This is not a clean full repository snapshot.

First controlled optimization: `DebugPrivateOutput=false`, while
`DebugIsolatedTextures=true` still copies all inputs. For divisible render widths,
enable output subrects at creation and write each feature directly into its monitor
region of the game's output. Keep local Width/Height/OutWidth/OutHeight, local
projection, isolated guides, jitter and MV scales as in the successful reference.
Skip allocating the three private output textures and skip the three final copies.
For uneven input widths, keep the existing private expanded-output/integer-crop
path. Log `privateOutput=false` on the direct path; restore the reference behavior
by setting the flag true (or use the saved reference DLL).

At the captured format 26 (4 bytes/pixel), removed output texture payload is
11520*2160*4 = 99,532,800 bytes, about 94.9 MiB, plus elimination of three output
copies (roughly 199 MB/frame logical read+write traffic). Allocation overhead,
compression/caches, driver behavior and actual timing are not included. No FPS
or GPU-time improvement is established yet. This is one experiment toward removing
the temporary copy path, not approval to keep copies in the final design.

CPU geometry/parameter-restoration checks pass after this change. Input shader is
unchanged from the tested reference. Owner A/B comparison is pending. Only after
direct output preserves quality should we progressively test original input
resources again, retaining local dimensions/projection and checking each group.
The initial success changed several aspects together, so it does not establish
that every guide copy is required or that an NGX subrect bug is the sole cause.

Release x64 build succeeded with zero errors (existing warnings/packaging message).
Installed direct-output experiment with game closed; built/installed SHA256:
`CCFB55404214999F0F4904C438B5B75DBE0BD2F7155E23FE2CCD8D91395A7F2F`.
The successful isolated reference is also backed up beside the game as
`dxgi.dll.rr-good-isolated-reference-20260919-134443.bak`. Parent AGENTS.md now
records the owner's successful reference result. Next comparison: same scene and
settings, stationary duplicated details, slow pans/ghosting/grain; optionally note
FPS/upscaler GPU time for both builds. Do not call this optimized variant validated
until the owner tests it.

## Direct-output success; staged input-copy removal (2026-09-19)

Owner reports the direct-output build still looks as good as the successful
isolated reference. Installed/built hashes match `CCFB5540...`. Latest log
`OptiScaler_2176104318828.log` confirms `privateOutput=false`, tile-local projection,
native handles 1/2/3, 1280x720 -> 3840x2160 per tile and successful evaluation.
No matched RR copy/tile-evaluation errors. Surface/smoke investigations remain
deferred; this run does not establish a measured performance gain.

Preserved this second successful reference in `x64/rr-reference-CCFB5540/`, including
DLL, modified/untracked source snapshot, log and branch/base-commit manifest.

Next controlled test changes only the core input resource delivery:
`DebugCopyCoreInputs=false` leaves Color, Depth and MotionVectors bound to the
original full-frame resources, with caller bases plus each tile's render origin.
Retains local feature/evaluation dimensions and projection, original MV displacement
scales/jitter, direct output, and copies of ALL auxiliary/RR guides (including bias,
albedos, normals/roughness, hit distance and optional effect guides). Set the flag
true to recover the direct-output reference. No AMD backend or SR branch changes.

This removes nine shader copy dispatches per frame. At captured Ultra Performance
formats, private core-input payload removed is 3840*720*(4+4+8) = 44,236,800 bytes
(about 42.2 MiB), excluding allocation overhead. This is a size calculation, not
a measured VRAM/GPU-time reduction. Removing guide copies is intentionally deferred
until owner validation of this step, to narrow any regression to the core group.

CPU suite passes 81,910 geometry cases and existing RR checks, plus a mixed
direct/copied input test with nonzero XY bases, unchanged source identities/scales,
and restoration after simulated failure at each tile. Copy shader is unchanged.
Owner test of the core-direct variant is pending.

Release x64 build succeeded. Installed with Cyberpunk closed; built/installed
SHA256 `5B5ACC4A98758BADDC361BC526438A6426E5839E578DBF834D2F295B579D86F0`.
Successful previous build retained beside game as
`dxgi.dll.rr-good-direct-output-20260919-135546.bak`. Next log should show
`copyCoreInputs=false`, `RR DEBUG direct input` for Color/Depth/MotionVectors,
and continued copy markers for guides. Compare stationary details and slow motion
at unchanged settings; this build's image quality is not yet established.

## Core-direct regression; isolate motion vectors (2026-09-19)

Owner reports motion ghosting returned on center/right monitors in `5B5ACC4A...`,
while the left monitor still looks as before. Installed hash confirms that build.
Log `OptiScaler_2182657339962.log` shows original Color/Depth/MotionVectors bound
at bases 0/1280/2560, active 1280x720 per tile, unchanged MV.Scale.X=3840, local
projection, direct output, copied guides, distinct handles 1/2/3 and successful RR
evaluation. Correct Set/Get values do not prove native sampling. Tile zero's
zero origin can hide addressing errors; MV addressing is the leading hypothesis,
but color/depth were changed in the same test and remain alternative causes.

Next controlled source change: `DebugCopyMotionVectors=true` while
`DebugCopyCoreInputs=false`. Copy only MVs again from the correct tile region to
a local texture with zero bases; leave color/depth direct. Auxiliary guide copies,
local dimensions/projection and direct output stay unchanged. MV scales and jitter
are untouched: copying changes sampling coordinates, not vector displacement units.
This adds three MV-copy dispatches back, retaining removal of six color/depth-copy
dispatches relative to the all-input-copy reference. No performance gain or final
need for MV copies is established. The goal remains eliminating diagnostic copies
after understanding the native contract.

CPU geometry/scope checks pass, including the mixed direct/copied parameter path.
Owner test of this MV-only restoration is pending. If ghosting disappears, it
implicates direct MV delivery in this setup; if not, test depth/color separately
against saved successful reference CCFB5540 instead of changing vector scales.

Release x64 succeeded; installed with Cyberpunk closed, hashes matched:
`B0973BAD78B2E5D0A8847D29B83B5DA8C463B07D3A28885C13807FCF0B031DA8`.
Previous core-direct build backed up as
`dxgi.dll.before-rr-mv-copy-isolation-20260919-140229.bak`. Next log should show
`copyMotionVectors=true`, copied MVs with zero local bases, direct color/depth
with nonzero tile bases. Owner comparison pending; earlier good references retained.

## MV-copy restoration succeeds (2026-09-19)

Owner reports this version seems good again. Installed and built SHA256 both
match `B0973BAD78B2E5D0A8847D29B83B5DA8C463B07D3A28885C13807FCF0B031DA8`.
`OptiScaler_2186987933154.log` confirms `copyCoreInputs=false`,
`copyMotionVectors=true`, `privateOutput=false`, original color/depth bases
0/1280/2560, MVs copied from those regions to zero-origin local textures,
tile-local projection, independent handles 1/2/3 and successful RR evaluation.

The controlled comparison supports direct MV resource delivery as the cause of
the center/right motion regression in this setup: restoring only MV copies
recovered owner-reported quality while color/depth remained direct. It does not
identify the internal reason (subrect interpretation, dimensions, or another
native assumption), and does not prove copies must remain in the final design.
No vector-value scale was changed. At the captured formats, direct color/depth
remove six dispatches and about 21.1 MiB of private texture payload relative to
all-input copies; direct output also remains successful. These are structural
savings, not measured FPS/GPU time.

Saved newest successful DLL, modified/untracked source snapshot, log and manifest
in `x64/rr-reference-B0973BAD/`. Current installed DLL is retained. No source or
binary change in this confirmation pass; only reference artifacts/context updated.
Next useful optimization: selectively test RR guide inputs without copies while
keeping the now-isolated MV behavior fixed. Investigate native MV subrect handling
separately before attempting a final copy-free path. Minor surface/smoke issues
remain deferred per the owner.

## Material-guide copy removal and MV-space hypothesis (2026-09-19)

Owner proposed that RR might sample MVs in the opposite pre/post-upscale space to
SR, and authorized proceeding with the next optimization. Checked NVIDIA's public
[RR helper](https://raw.githubusercontent.com/NVIDIA/DLSS/main/include/nvsdk_ngx_helpers_dlssd_d3d.h)
and [Streamline RR guide](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_RR.md).
The helper exposes an MV subrect base and sets the same NGX MV-subrect/scale keys;
its scale fields convert vectors to pixel space. No reversed convention is
documented there. The RR guide describes low-resolution input support. Current
game evidence is render-sized 3840x720 MVs, LowResMV=true, scales 3840/720. Output
tile origins 3840/7680 would not fit that input allocation without an additional
mapping. An internal coordinate-space/texture-extent mismatch remains plausible;
successful local MV copies do not establish which internal operation is wrong.
No MV scale or offset-space experiment is combined with the guide test.

Source changes: `DebugCopyMaterialGuides=false` passes original diffuse albedo,
specular albedo, normals and optional separate roughness via their documented
render-space subrects. Color/depth/output remain direct as in B0973BAD. MV,
hit-distance, bias and other effect-guide copies remain enabled. Packed roughness
continues traveling in the normals alpha channel. Set this flag true to restore
the latest successful reference. At the captured packed-roughness layout, this
removes nine more copy dispatches and roughly 42.2 MiB of private input payload;
these are structural counts, not performance measurements.

CPU geometry and mixed direct/copied input restoration checks pass with this
policy, including nonzero caller bases. Shader and camera/MV values are unchanged.
Existing B0973BAD source/DLL/log reference remains available. Owner comparison of
stationary geometry, surface detail/reflections, and motion is pending.

Release x64 built successfully. Installed with game closed; built/installed hash:
`ED8107AC612EB4FE3912259434E0497BB00BF5BCB2AF65E12237E6B67DB76630`.
Successful previous DLL retained beside game as
`dxgi.dll.rr-good-mv-copy-20260919-141152.bak`. Expected markers:
`copyMaterialGuides=false`, direct albedo/normal inputs with per-tile bases,
and continued MV copies. Owner test pending.

## Interrupted-session cleanup (2026-09-19)

Rechecked branch, source policy, build result, installed/built hashes, reference
artifacts and latest log. The material-guide test was already implemented, built,
CPU-tested and installed before the interruption. No build/deployment work remained
unfinished. A later build timestamp (14:16:21) is present; current built/installed
DLLs both hash to `66BF72AE699EAACAA3A883D4E25C4C707EEBAAB9C28418F31ACCC78FB780C627`,
rather than the earlier `ED8107AC...` deployment. Exact cause of the rebuild is not
established; do not attribute it to a specific person or assume binary equivalence.

New log `OptiScaler_2194605875892.log` explicitly confirms
`copyCoreInputs=false`, `copyMotionVectors=true`, `copyMaterialGuides=false`,
`privateOutput=false`, direct material-guide bases 0/1280/2560 and successful RR
evaluations. This establishes that the intended test path ran, not its image quality.
Owner visual feedback about the material-guide variant remains pending. The saved
B0973BAD reference remains the newest owner-confirmed good state.

Only context notes updated during cleanup. Source changes, reference snapshots,
backups and installed DLL were preserved. No additional optimization or branch
merge was performed. Git whitespace check passed (existing CRLF warnings only).

## Skin artifacts during lighting transitions (2026-09-20)

Owner reports a jarring moving static-like texture on human skin, including player
hands, when moving between lit and shadowed areas (for example an underpass).
Asked whether this began with material-guide copy removal; owner is unsure.
Do not treat this as a confirmed new regression or assume the earlier minor
surface artifacts had a different cause. Installed DLL before this comparison
hashes to `66BF72AE699EAACAA3A883D4E25C4C707EEBAAB9C28418F31ACCC78FB780C627`.
No newer log than `OptiScaler_2194605875892.log` is present at inspection.

That log confirms direct albedos/normals and copied
`DLSSD.ScreenSpaceSubsurfaceScatteringGuide` (format 54, 1280x720 per tile).
Successful calls and submitted offsets do not verify the sampled guide contents
or temporal reconstruction. Material-guide addressing and the SSS path are
investigation candidates; neither is an established cause of the skin pattern.

Controlled direct source edit: restore `DebugCopyMaterialGuides=true`, returning
to B0973BAD's input policy while keeping direct color/depth/output and the working
MV copies. Projection, MV displacement scales, copy shader and SSS guide handling
are unchanged. All copies remain temporary diagnostics. Owner should repeat the
same lighting transition with hands/skin visible and unchanged quality settings.
If this improves the skin, isolate albedo versus packed normal/roughness next;
if unchanged, compare the same scene with ordinary untiled 3840x2160 RR and audit
SSS guide delivery before assigning a cause. This control's visual result is pending.

Release x64 build passed (61 warnings, zero errors; metadata prebuild skipped for
the previously documented script issue). Existing CPU suite passed 81,910 geometry
cases, RR projection/guide checks and mixed-input/failure parameter restoration.
The unchanged copy shader was not retested on WARP; no NGX/GPU visual validation
was performed by the agent. Installed with Cyberpunk closed; packaged OptiScaler,
packaged dxgi and installed dxgi hashes match:
`A483E43E372A0998ABA68994136C8E230971D64139AD4F813647A499B00C96A1`.
Previous DLL backed up beside the game as
`dxgi.dll.before-rr-skin-material-control-20260920-214712.bak` and hash-verified.
Next run should log `copyMaterialGuides=true` with copied albedos/normals.

## Material copies reduce skin static; untiled comparison is clean (2026-09-21)

Owner reports A483E43E reduces skin static from several seconds to approximately
one second when entering/leaving shadows. It remains perceptible. The owner also
tested ordinary 3840x2160 and reports no static there. Installed hash confirms
A483E43E. This supports a contribution from direct material-guide delivery; it
does not isolate albedo versus packed normal/roughness or explain the residual.

Latest log `OptiScaler_3780482330374.log` confirms a 3840x720 -> 11520x2160 tiled
RR instance at 10:21:31 with core copies off, MV/material copies on and direct
output. Native handles 2/3/4 use input origins 0/1280/2560. At 10:23:15 it creates
ordinary 1280x720 -> 3840x2160 RR, native handle 5. First evaluations succeed in
both modes. Per-tile input dimensions match the untiled input dimensions, although
the overall frame/FOV differs. Both snapshots show AutoExposure=false, null
ExposureTexture, pre-exposure/exposure scale 1; this is a first-frame observation,
not a trace through the shadow transition. Tiled SSS copies use format 54.

Preserved A483E43E DLL, latest log and the two relevant source files under
`x64/rr-reference-A483E43E/` (partial source snapshot). Next controlled source
change restores `DebugCopyCoreInputs=true`, copying color and depth again. All
material/MV/effect guide copies stay enabled, with direct output and unchanged
camera, vector scales, shader, history logic and SSS guide delivery. This isolates
the remaining direct core inputs before attributing the residual to SSS. It
matches the earlier all-input-copy/direct-output policy, but has no new owner
visual result yet. Do not call it a confirmed fix or a final copy-based design.

Added the SSS guide and before/after-SSS color resources to existing bounded
Debug-level snapshots for both untiled and tiled RR. Future logs will expose
resource presence, allocation/format and base coordinates; this does not read
texture contents. Expanded the actual-copy-shader WARP readback test to cover
R16_FLOAT (SSS) and R8_UNORM (bias), previously untested formats in the captured
input set. Seven formats now pass exact byte comparisons for three distinct crops
from padded sources with nonzero XY bases; D3D12 debug layer reports no warnings
or errors. CPU suite passes 81,910 geometries and existing RR/scope-restoration
checks. WARP is software D3D12 testing, not native NGX or NVIDIA/game validation.

Owner comparison: repeat the same shadow transition at 11520x2160 with unchanged
quality settings. If residual static improves, split color/depth tests next. If
unchanged, use the new SSS snapshots to audit correspondence against untiled RR;
private-output isolation and temporal/camera differences remain candidates too.

Release x64 build passed with 16 warnings and zero errors, using the documented
metadata-prebuild skip. Installed with Cyberpunk closed; packaged OptiScaler/dxgi
and installed dxgi SHA256 match:
`BFA0510ACFC6CD6AD6F6FF233E3B3BA06320866F843EBF49A0B76F9CDAC46589`.
Verified A483E43E backup beside game:
`dxgi.dll.before-rr-skin-core-control-20260921-102822.bak`.
Next log should show `copyCoreInputs=true`, `copyMotionVectors=true`,
`copyMaterialGuides=true`, `privateOutput=false` and SSS resource snapshots.

## All-input-copy control resolves reported skin artifact (2026-09-21)

Owner reports BFA0510A now looks good, as expected, and asks whether copy-free RR
remains possible. Built/installed DLL hashes still match BFA0510A. Latest log
`OptiScaler_3786087900714.log` confirms all input copies, direct output, tile-local
projection and independent handles 2/3/4 at input origins 0/1280/2560. First tiled
evaluation succeeds. SSS guide is present; before/after-SSS color resources are
null in both the initial untiled and tiled snapshots. No new rendering changes
were made during this confirmation pass.

This supports direct color/depth delivery contributing to the residual artifact
in the previous mixed-input configuration. The experiment changed both together,
so it does not isolate which one matters. Nor does it establish that native RR
requires copies: local textures simultaneously change resource extent, origin and
delivery through the copy pass. Submitted subrect values do not prove native
sampling behavior. Direct output remains working in this owner test. Earlier
minor surface/smoke issues are not exhaustively retested by this report.

Saved DLL, latest log and modified/untracked source snapshot under
`x64/rr-reference-BFA0510A/`; source snapshot is not a full checkout and precedes
this documentation update. Keep this working reference while investigating direct
inputs one at a time. Next useful work is to isolate color versus depth, then
inspect actual resource sampling/state/extent behavior with a GPU capture if
available. A correct copy-free path remains unproven; if some native inputs cannot
use full-frame subrects, selective or cheaper copies are a fallback to discuss,
not a decision to retain diagnostic copies in the final design.

## Isolate direct color with copied depth (2026-09-21)

Owner authorized the next change. Direct source edits split the combined core
copy switch into `DebugCopyColor` and `DebugCopyDepth`. Current test sets color
false and depth true, leaving MV/material/effect copies enabled and output direct.
Only color delivery changes from the successful BFA0510A policy. It uses the
existing full-frame color resource with the caller base plus each tile's render
origin. Depth remains a local copied texture. Projection, dimensions, MV scales,
SSS handling, shader and history logic are unchanged. Logging now names color and
depth separately. Set color true to restore the reference policy.

Existing CPU suite passes 81,910 geometries and RR projection/guide/mixed-resource
restoration checks with this policy. Copy shader was not changed or retested.
Owner should repeat the same shadow transition at 11520x2160 with unchanged
quality settings. Returning static implicates direct color in this configuration;
if it stays clean, test direct depth separately with color copied, against BFA0510A.
This test alone cannot establish why native direct sampling behaves differently.

Release x64 passed (16 warnings, zero errors; existing metadata-prebuild skip).
Installed with Cyberpunk closed. Packaged OptiScaler/dxgi and installed dxgi hashes
match `EC8ECAB37EE13839737DA460235176E96ACA845B6893669174467131F0356CB9`.
Verified working BFA0510A backup:
`dxgi.dll.before-rr-color-direct-20260921-103720.bak` beside game; full working
reference remains in `x64/rr-reference-BFA0510A/`. Expected log markers:
`copyColor=false`, `copyDepth=true`, `copyMotionVectors=true`,
`copyMaterialGuides=true`, `privateOutput=false`. Owner result pending.

## Direct color reproduces static; evaluate input-dimension hypothesis (2026-09-21)

Owner reports brief static returns with EC8ECAB3, then converges to the expected
appearance. Color delivery is therefore a contributor in this controlled setup;
depth has not been tested independently and may also contribute. Installed hash
matches EC8ECAB3. Log `OptiScaler_3797610842322.log` confirms original color resource
3840x720, format 26, one mip/sample, flags 0x5; direct color bases are 0/1280/2560
with active 1280x720 tiles. Other captured inputs remain copied. Snapshot of this
regressed DLL/log and two source files is in `x64/rr-reference-EC8ECAB3/`.

Copy and bypass use the same caller Color resource and intended region: the copy
shader Loads pixel + caller base + tile render origin and stores it into a local
texture. It performs no filtering or deliberate exposure/color conversion.
Format 26 is retained; existing WARP checks preserve finite test pixels exactly.
This does not prove the native DLL samples direct color identically. Differences
include resource extent, origin, identity/flags and the extra copy pass.

Audited NVIDIA's current public
[RR D3D helper](https://raw.githubusercontent.com/NVIDIA/DLSS/main/include/nvsdk_ngx_helpers_dlssd_d3d.h).
It uses the same Color resource, color-subrect base and active render-subrect keys.
Its evaluation helper does not assign generic Width/Height; those are assigned
at feature creation. No alternate color-offset key or mandatory input-copy switch
was found. Do not claim the current offset submission proves native correctness.

One source-level difference from our working SR direct path: RR's diagnostic
CopyScope sets generic Width/Height to tile dimensions even with original color
bound, while SR leaves caller dimensions intact. Controlled new test preserves
the captured caller Width/Height for direct color via
`DebugDirectColorFrameDimensions=true`. For this capture, Width will be 3840
instead of 1280; active render subrect remains 1280x720, with bases 0/1280/2560.
Output Width/Height remain tile-local and feature creation is unchanged. No other
resource, scales, camera, shader or history handling changes. This is an empirical
compatibility hypothesis, not a claim that Width means allocation width in NGX.
Setting the flag false restores the previous test; copying color restores the
working input policy and disables this dimension experiment automatically.

CPU checks pass all 81,910 geometries and now exercise both local and preserved
input dimensions in mixed-input evaluation, including unchanged tile regions and
restoration on each simulated tile failure. No native NGX/GPU verification was
performed by the agent. Owner was also asked whether the symptom affects the left
tile (zero origin) as well as center/right; response is optional and pending.

Release x64 passed (16 warnings, zero errors; existing metadata-prebuild skip).
Installed with Cyberpunk closed; packaged OptiScaler/dxgi and installed dxgi hashes
match `51698FBD5B82A566B92DB62D9C6BE57B3AA2A19FB3C95A35D56AF1AEB3C4BF38`.
Verified previous DLL backup:
`dxgi.dll.before-rr-color-frame-dimensions-20260921-105700.bak` beside game.
Working BFA0510A reference remains preserved. Expected markers:
`copyColor=false`, other input copies true, `directColorFrameDimensions=true`,
tile-effective Width=3840 and active render width=1280 at Ultra Performance.
Owner visual comparison is pending; no fix is claimed.

## Input-dimension test remains imperfect; full-frame color clone control (2026-09-21)

Owner reports static remains in 51698FBD, usually for a split second but sometimes
longer, and seems less noticeable. No controlled duration measurement exists, so
do not call the dimension experiment a verified improvement. Installed hash still
matches 51698FBD. Latest log `OptiScaler_3806342564230.log` confirms Width=3840
at each evaluation, direct color bases 0/1280/2560 and successful first evaluation.

Next direct source diagnostic adds `DebugFullFrameColorCopy=true` while keeping
`DebugCopyColor=false` and preserved caller dimensions. Copies the full source
allocation, including padding, once before all RR calls, into a cached shared
texture. All three tiles bind this same clone using their original full-frame
coordinates. Unlike the working local-copy reference, this retains full color
extent and nonzero offsets. Other inputs/output/camera/scale/history behavior
remain unchanged. Clone uses the same typed format but UAV-only resource flags
(original captured color also permits render-target use); source state handling
is unchanged. Identity, allocation flags/layout and the extra pass are all possible
factors if it improves. This is not a synchronization-only experiment.

The cached clone and immutable descriptor/source references persist until feature
destruction as in existing debug copies. Single-mip source required; existing
format/region checks still apply. Once-per-frame copy covers the allocation rather
than just the active frame so caller bases and padding keep their correspondence.
Binding scopes restore resource identity, XY bases and dimensions on failure.
Tile-local color copies take precedence if `DebugCopyColor=true`, restoring the
working BFA0510A policy. Disabling full-frame color copy reproduces 51698FBD.

If static persists on the full-size clone, texture extent/subrect interpretation
remains a lead. If it disappears, the original resource's delivery/flags/state or
the extra pass becomes a stronger lead. Neither result identifies the internal
NGX cause by itself. Goal remains correct bypass; this is a temporary diagnostic.

CPU suite passes 81,910 geometries and mixed local/full-frame binding/restoration
checks with nonzero caller bases and both dimension policies. WARP readback now
tests three cropped regions plus a full-allocation copy in all seven captured
formats; byte comparisons pass and D3D12 debug layer reports no warnings/errors.
These checks do not exercise native NGX or production game resource lifetimes.

Release x64 passed (16 warnings, zero errors; documented metadata-prebuild skip).
Installed with game closed; packaged OptiScaler/dxgi and installed dxgi SHA256:
`C811E200284397049CB3D0176B0B8A845B15D76665C285177AE32715326BA79C`.
Verified previous DLL backup:
`dxgi.dll.before-rr-full-frame-color-20260921-111134.bak` beside game.
Expected markers: `fullFrameColorCopy=true`, one shared full-frame copy, tile
bindings with bases 0/1280/2560 at Ultra Performance; active regions still
1280x720. Working BFA0510A reference preserved. Owner comparison pending.

## Full-width clone fails and costs performance; restore local color (2026-09-21)

Owner reports C811E200 does not fix the artifact and reduces observed performance
from 55 to 45 FPS (10 FPS). This is owner gameplay measurement, not an instrumented
controlled benchmark. Do not attribute the entire loss to copy bandwidth or claim
restored FPS without another owner run. Installed hash matched C811E200. Latest
log `OptiScaler_3811760150061.log` confirms a single 3840x720 format-26 color clone
shared by all three evaluations at bases 0/1280/2560 and successful first evaluation.

Both the original full-width color and a full-width clone produce the artifact;
the known-good local-copy policy does not in owner testing. Replacing the game
resource alone was insufficient. This strengthens investigation of full-width
extent/subrect handling, but does not prove a specific native sampling operation
is wrong. The clone experiment retained preserved generic input dimensions, unlike
the working local-copy reference; do not conflate those variables. Original game
resource state alone is a less convincing explanation after the clone result.

Preserved failed DLL/log and three source files in `x64/rr-reference-C811E200/`
(partial source snapshot). Direct source edits restore `DebugCopyColor=true`,
disable `DebugFullFrameColorCopy`, and disable `DebugDirectColorFrameDimensions`.
Depth/MV/material/effect copies remain enabled, output stays direct. This returns
to BFA0510A's input/dimension policy without discarding unrelated uncommitted work.
Experimental code remains disabled for reproduction. No additional speculative
rendering experiment is combined with the rollback. Native GPU capture of color
sampling is the next useful evidence; no capture has been performed by the agent.
All diagnostic copies remain outside the intended final design.

Existing CPU suite passes 81,910 geometries and RR/binding/restoration checks with
the restored policy. Shader unchanged; no repeat WARP run needed for flag changes.

Release x64 passed (16 warnings, zero errors; documented metadata-prebuild skip).
Installed with game closed; packaged OptiScaler/dxgi and installed dxgi SHA256:
`1FD61D79CE3A2B2E19216FB2FCDBD24217BD1E278FC131544BE649BE021E5263`.
This is a rebuild of the restored policy, not the exact BFA0510A reference binary.
Verified failed-build backup:
`dxgi.dll.before-rr-restore-local-color-20260921-111807.bak` beside game.
Expected markers: all input copy switches true, `fullFrameColorCopy=false`,
`directColorFrameDimensions=false`, `privateOutput=false`. Owner recheck of
image quality and FPS remains pending; no performance recovery claimed yet.

## Exact known-good rollback requested and completed (2026-09-21)

Owner asks why copies look better and explicitly requests the known working
state. The working local textures have zero bases and exact tile extents, whereas
the failing paths retain full-width color and subrect addressing. This difference
is supported by code/logs; its native mechanism remains unproven. No further
rendering experiment was introduced.

Restored the exact saved BFA0510A DLL, not a rebuild of its policy. Installed and
both packaged DLLs were hash-verified against:
`BFA0510ACFC6CD6AD6F6FF233E3B3BA06320866F843EBF49A0B76F9CDAC46589`.
Restored `DLSSDTiling.h`, `DLSSDFeature_Dx12.cpp`, `DLSSDFeature_Dx12.h`,
`tests/dlss_tiling_tests.cpp`, and `tests/dlssd_copy_dx12_tests.cpp` byte-for-byte
from the saved source snapshot. Other source files in that snapshot already
matched; unrelated uncommitted work was preserved. Documentation retains history.
The later experimental split-color/dimension/full-frame-copy code is no longer
in active source. All input copies are enabled; output remains direct.

Backed up previous source/tests/docs and packaged DLLs to
`x64/rr-before-exact-restore-20260921-112558/`, and installed DLL beside the game as
`dxgi.dll.before-exact-BFA0510A-restore-20260921-112629.bak`; verified backups.
Game was closed at installation. CPU suite passes 81,910 geometries and existing
RR projection/addressing/restoration checks. No DLL rebuild or new native GPU
test was performed; game quality evidence remains the owner's earlier BFA0510A
test. Current restored state is intended for use as the working baseline.
