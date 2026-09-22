# Context for this fork

This fork, `mystic3423/OptiScaler`, exists to make DLSS Super Resolution work
at **11520 x 2160**, using three 4K monitors in NVIDIA Surround. Read this
context before proposing changes to the tiling implementation. This is an
experimental workaround with working game builds, not a finished general solution.

## Branch separation (owner instruction, 2026-09-19)

- `dlss-tiling` is the separate DLSS Super Resolution work.
- `dlss-rr-tiling` is the experimental Ray Reconstruction work split from it.
  The owner also refers to this as the Ray Regeneration work; this branch's
  current implementation targets NVIDIA RR, not AMD Ray Regeneration.
- Keep these efforts separate until RR works correctly. Always inspect the
  current branch and working tree; do not assume RR changes or test results
  apply to `dlss-tiling`, and do not merge/backport experiments automatically.
- The isolated input/output copy path is a temporary diagnostic, explicitly
  not intended for the final implementation. Source edits are direct, not patches.
- Branch-independent reminder: `../AGENTS.md` (OptiScaler-only section).

## Current RR handoff

Exact known-good restoration (2026-09-21, owner request): installed and packaged
DLLs are the saved BFA0510A binary, SHA256
`BFA0510ACFC6CD6AD6F6FF233E3B3BA06320866F843EBF49A0B76F9CDAC46589`.
Restored the three RR tiling source files and two test files byte-for-byte from
`x64/rr-reference-BFA0510A/source/`. No rebuild replaced the saved DLL. CPU suite
passes. All captured inputs use tile-sized copies, output is direct; active flags
are `DebugCopyCoreInputs=true`, `DebugCopyMotionVectors=true`,
`DebugCopyMaterialGuides=true`, `DebugPrivateOutput=false`. Later split-color,
full-width-copy and preserved-dimension experiment code has been removed from
active source by this restoration. Earlier uncommitted work remains intact.

Owner reported BFA0510A looked as expected and resolved the skin static in their
shadow-transition test. Subsequent direct-color experiments brought it back;
full-width color copying also failed and owner reported 55 -> 45 FPS. These
results implicate full-width/subrect delivery but do not prove the native cause
or that copies are inherently required. Copies remain a temporary diagnostic,
not the intended final design. No new experiment is active. Earlier minor
surface/smoke artifacts are not exhaustively retested by the skin result.

Previous source/tests/docs and packaged DLLs are preserved in
`x64/rr-before-exact-restore-20260921-112558/`. Failed experiment references and
chronological evidence remain in the Cyberpunk investigation note. Keep RR on
`dlss-rr-tiling`, separate from SR. Do not silently re-enable the failed bypass
experiments or overwrite this working baseline with an old build artifact.

## Why tiling exists

- The owner found that the DX11/DX12 DLSS path checks doubled output dimensions
  against the 16384 texture-dimension limit. This rejects widths above 8192,
  including 11520. Treat the specific internal DLSS check as the owner's
  investigation; do not claim it was independently established for every DLL.
- DLSS to a smaller full-frame output followed by spatial upscaling worked,
  but visibly reduced quality and required awkward configuration.
- Bypassing the check did not reliably allow full Surround resolution. In
  Spider-Man Remastered, the owner observed a boundary around 16384 / 1.5
  (10922.67 pixels), below 11520. This is an observed game-specific threshold,
  not proof of a particular internal allocation or a universal safe limit.
- NVIDIA received a bug report months before this work and asked a follow-up
  question, but there is no known fix or timeline.

## Chosen approach

Create three independent DLSS SR features, each outputting 3840 x 2160. Each
feature reads a horizontal subrect of the existing full-frame input resources
and writes its own region of the shared output texture. Output subrect support
is enabled at feature creation. Divisible input widths use this direct path,
with no overlap or final spatial upscale. Uneven input widths now use the
experimental expanded-output/integer-crop path described below. The final
output boundaries align with the physical monitor edges.

The owner has successfully run this approach, including Ultra Performance.
Do not describe Ultra Performance as unsupported just because its input is small.

| Example | Full input | Input per tile | Output per tile |
| --- | --- | --- | --- |
| Quality | 7680 x 1440 | 2560 x 1440 | 3840 x 2160 |
| Performance | 5760 x 1080 | 1920 x 1080 | 3840 x 2160 |
| Ultra Performance | 3840 x 720 | 1280 x 720 | 3840 x 2160 |

Input sizes must follow the game's active render dimensions. These examples
are not the only intended sizes. SR tiling covers DX11/DX12. Experimental DX12
RR tiling was added on 2026-09-19; the owner reports the isolated-copy reference
eliminates major ghosting/grain/duplicate images, with minor surface/smoke artifacts
remaining. Direct-output optimization is under test. DX11/Vulkan RR and Frame
Generation tiling are not implemented.

## Current implementation and decisions

Snapshot: 2026-09-17. Inspect the working tree and Git history before acting;
some changes may still be uncommitted or may have evolved since this note.

- Main files: `OptiScaler/upscalers/dlss/DLSSTiling.h`, `DLSSFeature.h`,
  `DLSSFeature_Dx11.cpp`, and `DLSSFeature_Dx12.cpp` in the same directory.
- `TileCountFromEnv()` intentionally returns **3** immediately. The environment
  variable implementation is leftover scaffolding. The owner explicitly wants
  the hardcoded debug hotfix preserved for now. A future OptiScaler option is
  preferred over an environment variable; do not implement that prematurely.
- As of 2026-09-19, DX11/DX12 SR creation uses the ordinary single-feature path
  when `TargetWidth() < 8192`, per the owner's requested quick output-width check.
  At 8192 and above, the existing three-tile layout attempt/fallback remains.
  The helper's hardcoded three is preserved. The DX12 RR backend now uses the
  same width selection, with its own RR tile handles and guide/projection handling.
- Both APIs now pass `LowResMV()` to `ApplyTile()`.
- Both evaluation paths recalculate input tiles every frame using current
  render dimensions, while retaining the DLSS handles and output layout.
  This addresses stale input offsets after dynamic-resolution changes.
- DX12 tile-creation failure now restores full-frame dimensions after releasing
  successfully created tile handles, before reporting failure.
- `dlss-tiling-dynamic-resolution.patch` was a proposal artifact; its changes
  are already present in the working tree at this snapshot. Check before applying it.
- `OptiScaler/OptiScaler.vcxproj` copies the built DLL to `dxgi.dll` beside
  `OptiScaler.dll`. Release x64 packaging puts both in `x64/Release/a`.
  The copy target was executed and the DLL hashes matched.

## Remaining correctness work

### Motion vectors and auxiliary inputs

The owner observed motion artifacts on the middle/right monitors while the
left looked better. Tile zero starts at zero in either resolution space, so
an incorrect MV offset can be hidden there.

- Render-resolution MVs use the tile's input X offset; output-resolution MVs
  use its output X offset. Preserve any base offset originally supplied.
- Cropping does not itself change pixel displacement. Do not blindly divide
  `MV.Scale.X` by three. Resource sampling coordinates and vector value scaling
  are separate concerns.
- Check incoming feature flags against effective `LowResMV()` and configuration
  overrides. `DisplayResolution` can override the incoming MVLowRes flag in
  `IFeature::SetInitParameters()`.
- Deferred diagnostics: log the MV texture description, active render/output
  sizes, original subrect bases, effective flags, per-tile offsets, and MV scales.
  Validate bounds. Allocation dimensions alone cannot prove active resolution:
  games may use padded textures or subrects. Use a GPU capture when ambiguous.
- Audit optional input resources against their actual resolution contracts;
  do not assume the existing table of auxiliary-resource offsets is proven correct.
- Preserve game jitter, exposure, reset signals, and restore modified parameters
  after evaluation, including failure paths.

### Input widths not divisible by three

The owner explicitly selected **experimental integer crop**, accepting small
alignment errors instead of adding a resampling pass. Implemented directly in
DX11/DX12 source on 2026-09-17; in-game validation is pending.

- `BuildFrameTiles()` retains the equal split for divisible input widths.
  Output width must still divide by three. Uneven inputs use equal expanded
  input widths, a fixed output width of monitor width + 16 (3856 at triple 4K),
  and integer crops copied into the three final 3840-pixel regions.
- Expanded input regions stay inside the full active frame. The outermost
  frame edges are anchored; middle crop alignment rounds to an output pixel.
  This approximates fractional boundaries, not exact full-frame geometry.
  CPU checks measured up to 2.996 output pixels of crop-edge position error
  over the tested 1x-3x upscale range. There is no seam blending.
- Render-resolution MV inputs use the expanded render origin; display-resolution
  MV inputs use a separately rounded/clamped expanded output-space origin.
  MV scales remain unchanged. Display-resolution sampling correspondence is
  also approximate; motion and auxiliary-resource contracts need GPU validation.
- Three direct handles are created initially, with nominal floor(input/3)
  width when startup is uneven. No frame is evaluated using that rounded full
  width: evaluation always recomputes the actual frame layout. Three expanded
  handles are created lazily and retained alongside the direct handles.
- One temporary output per format/size is reused for the three sequential
  evaluations/copies. Cached resources remain until feature destruction to
  avoid releasing resources still referenced by submitted work. Extra histories
  and the temporary output increase VRAM use after encountering an uneven frame.
- Histories reset on path switches, failed tiled frames, and render-size changes
  in the cropped path. Frequent uneven DRS changes may reduce temporal quality.
  The divisible path keeps its existing dynamic-resolution handling.
- Output resources, input bases, output X/Y bases, reset flags, and render
  subrect dimensions are restored after evaluation, including failures.
- The Release x64 build succeeded with the prebuild metadata event skipped
  after its existing Set-Content error left two empty generated headers; those
  headers were restored. No build-script change was made for this issue.
- `tests/dlss_tiling_tests.cpp` passed 81,910 CPU geometry cases, both MV modes,
  and DX11/DX12 parameter-scope restoration checks. This is not GPU/NGX testing.
  See `docs/investigations/dlss-integer-crop.md` for behavior and testing notes.

### Seams, histories, and testing

- Independent histories can produce artifacts when objects cross boundaries.
  Monitor bezels do not guarantee these are invisible. The uneven-width crop
  path adds a small input overlap for alignment, but general overlap/blending
  to improve seams remains future work.
- Verify distinct feature handles and actual DLL behavior if history mixing is
  suspected. Do not equate successful addressing with fully validated image quality.
- The owner reported working tiled builds and was beginning Gotham Knights
  testing at this snapshot. Do not claim those tests have passed yet.
- A mock NGX harness reproduced the original dynamic-size and DX12 cleanup
  bugs. Modified functions compiled, but patched harness execution was not
  conclusively verified. No agent GPU validation or complete test matrix was done.
- Useful tests: moving objects on all monitors, boundary crossings, gameplay/menu
  transitions, scene cuts, dynamic input sizes, both MV modes, and Ultra Performance.

### Deferred investigation: Spider-Man Remastered cutscene performance

See [the Spider-Man investigation note](docs/investigations/spider-man-cutscene-performance.md)
for measurements, settings, existing evidence, and pending tests. Latest owner
readings: about 80 FPS in gameplay versus 24 in cutscenes, roughly 70% GPU use
in both, and DLSS/upscaler time below 4 ms in both. FG and V-Sync are off;
the OptiScaler cap is 116. Disabling DOF did not fix it. The owner suspects a
tiling regression, but the cause is unconfirmed. Limiter isolation and a controlled
tiling comparison are pending. Resume when the owner is ready.

### Trails in the Sky 2nd Chapter: motion blur and secondary DLAA pass

See [the Trails investigation note](docs/investigations/trails-sky-motion-blur.md).
The owner reports blur during slow pans. An older-build log shows a main
5760 x 1080 -> 11520 x 2160 DX11 feature and a separate 1536 x 1536 feature,
both tiled. The owner suspects the square feature is the map's optional DLAA;
this is unconfirmed. In a follow-up session with map DLAA off, the owner reports
normal motion and the square pass is absent from the log. The Performance to
Quality switch successfully recreates all three tiles and changes evaluated
input from 5760 x 1080 to 7680 x 1440, retaining 11520 x 2160 output and
`LowResMV: true`. Actual MV scales/texture sampling remain unverified; map DLAA
as the blur's cause still needs an on/off comparison. Proposed
future handling: decide tiling per feature and keep outputs within the reported
dimension bound on the ordinary single-feature path. Preserve three tiles for
the main Surround output. No secondary-pass fix or GPU validation is done yet.

### Cyberpunk 2077 Ray Reconstruction investigation

See [the Cyberpunk RR note](docs/investigations/cyberpunk-ray-reconstruction.md).
On 2026-09-19 the owner reported heavy noise with RR enabled, an apparent fallback
to FSR, and continued noise after manually selecting DLSS. The first diagnostic
log confirms full-frame RR creation at 3840 x 720 -> 11520 x 2160 failed with
`0xBAD0000D` (`OutOfGPUMemory`), triggering FSR 2.1.2. The manual DLSS switch
created three ordinary SR tiles successfully, not RR. Actual VRAM exhaustion
versus an internal allocation problem remains unresolved. The initial diagnostic
backend used one full-frame RR feature. Added bounded Debug-level RR
parameter/resource snapshots and explicit backend-unavailable/creation-failure/
first-evaluation-success diagnostics. DLSSD is stored as DLSS in configuration;
the label alone does not confirm RR. A follow-up owner run successfully created
and evaluated RR v310.9.1 at 1280 x 720 -> 3840 x 2160, twice. RR performs
denoising/upscaling through the DLUnified feature; no separate SR creation appears
in that log. Captured render-sized guides include albedos, packed normals/roughness,
and specular hit distance; camera matrices are supplied, reflection MVs are null.
MV scales are 1280/720 with LowResMV=true. See the note for the AutoExposure flag
difference between the two successful instances. The width-limit hypothesis is
supported but the internal check is not proven.

Experimental DX12 RR tiling is now implemented in `DLSSDFeature_Dx12.*` and
`DLSSDTiling.h`: three independent RR handles, shared-output subrects for divisible
inputs, and the existing expanded-output/integer-crop geometry for uneven inputs.
RR guides get render-space offsets. The first build used an off-center copy of
the view-to-clip matrix; after the owner reported repeated/squeezed ghost images,
the current controlled test preserves the game's projection instead, with input
offsets unchanged. Neither projection interpretation is validated for tiled RR.
World-to-view and MV displacement scales remain unchanged. Parameters and
matrix pointers are restored even on failure. Histories reset after failed frames,
path switches, and input-size changes. Cropped handles/output caches persist until
feature destruction. Output widths below 8192 remain single-feature.

The prototype requires low-resolution MVs and camera matrices; it rejects supplied
legacy inputs without audited subrect handling (including reflection MVs) and
separate output alpha. Cyberpunk's captured inputs match this initial scope.
NVIDIA backend reselection in DX12 uses the original game's NGX feature ID so
selecting DLSS after fallback retries RR for an RR handle, while SR handles stay
SR. Initial creation routing is preserved. Release x64 builds; CPU geometry, RR
guide-offset, projection-correspondence and failure-restoration checks pass.
The owner ran tiled RR: all three handles created/evaluated successfully, but
reported severe ghost images (a center-monitor chimney repeated/squeezed into
the other monitors). Latest log: `OptiScaler_1797016139873.log`; direct tile
origins 0/1280/2560 at Ultra Performance, distinct native IDs 2/3/4. The controlled
projection test adds first-frame parameter readbacks immediately before each
tile's NGX call; previous resource snapshots were before tiling.
The owner reports the projection-control build looks somewhat better, but still
has stationary repeated images, heavy motion ghosting, and grain in motion.
`OptiScaler_1801525470509.log` confirms effective core/guide X bases 0/1280/2560
immediately before evaluation, unchanged MV scales 3840/720, and successful RR
calls. This proves parameter submission, not the DLL's actual texture sampling.
Research found ZachHembree's `ffx-denoise-experimental` conversion fork (full-frame
input conversion, not a verified tiling reference) and NVIDIA Omniverse 109.0.4
release note OMPE-78022 mentioning per-tile RR dimensions. Links and concrete
input findings are in the investigation note. The next diagnostic is implemented:
isolated tile-sized core/guide textures and private per-tile outputs, tile-local
projection, and zero input/output subrect origins. `DebugIsolatedTextures` in
`DLSSDTiling.h` marks this temporary path. Exact pixel-load shader copies handle
Cyberpunk's depth without partial depth-stencil copies. MV scales/jitter remain
unchanged. Resources and immutable descriptors are retained until destruction.
Release x64 and CPU checks pass. A standalone D3D12 WARP readback test passes
for captured formats 10/28/41/39 with no debug-layer warnings/errors; this tests
the copy shader, not NGX/game rendering. Initial deployment was blocked by an
approval-review usage limit. The owner's next run (`OptiScaler_1817339972967.log`)
still used the old game-projection build: confirmed by hash and log markers.
Do not count that run as a failed isolated-copy experiment. The isolated-copy
DLL was subsequently installed and hash-verified on 2026-09-19 (SHA256 starts
`88DB9C7C`). The owner's actual run (`OptiScaler_2159216991425.log`) produced no
upscaled image: the debug format whitelist rejected Color format 26
(`R11G11B10_FLOAT`) before any copy dispatch/native tile evaluation. All three RR
handles created successfully. Added format 26 support and an exact-byte WARP
readback regression test; all five captured formats pass with no debug-layer
warnings/errors. This fixes an identified debug-path rejection, not the original
ghosting. See the note for installed build hashes/backups and owner test status.
The corrected isolated-copy reference (`68165B43...`) now has owner-reported
success: no major ghosting, grain or repeated ghost textures. Occasional surfaces
flicker/turn black/look corrupted and smoke opacity fluctuates; the owner deferred
those investigations. Log `OptiScaler_2167964786672.log` confirms independent RR
handles, local inputs, successful evaluation and packed HDR output. It additionally
captures bias mask (R8), color-before-particles and subsurface-scattering guides.
This supports the complete isolated-tile setup, not a proven individual root cause.

Reference DLL, modified/untracked source snapshot and success log are preserved in
`x64/rr-reference-68165B43/`. The first optimization sets `DebugPrivateOutput=false`
while retaining isolated inputs, local dimensions/projection and original MV scales.
Divisible frames use output subrects directly, removing three output textures and
copies; uneven frames retain their private integer-crop outputs. At packed HDR
11520x2160, expected eliminated texture payload is about 95 MiB (not a measured
VRAM/FPS result). Owner comparison is required before removing any input copies.
No agent NVIDIA/NGX GPU capture has been performed. Do not backport to SR.

On the next owner run, direct-output build `CCFB5540...` preserved the improvement.
`OptiScaler_2176104318828.log` confirms `privateOutput=false`, tile-local projection,
and successful RR evaluation. Reference DLL/source/log are saved in
`x64/rr-reference-CCFB5540/`. The next experiment sets `DebugCopyCoreInputs=false`:
original Color/Depth/MotionVectors resources with per-tile subrects, while all
auxiliary/RR guides remain copied. Local dimensions/projection and direct output
stay unchanged. Set that flag true to restore the direct-output reference.
This removes nine input-copy dispatches per frame; quality validation is pending.
CPU mixed direct/copied input addressing and failure restoration checks pass.
The owner reports core-direct build `5B5ACC4A...` brought motion ghosting back
on center/right tiles while left stayed good. Log `OptiScaler_2182657339962.log`
confirms direct Color/Depth/MV bases 0/1280/2560, unchanged MV.Scale.X=3840,
and successful RR calls. Nonzero-origin addressing is suspected, not proven;
this test changed three inputs. Next isolation sets `DebugCopyMotionVectors=true`
with `DebugCopyCoreInputs=false`: only MV copies return; color/depth stay direct,
guides stay copied, direct output/local dimensions/projection remain. Do not
divide MV displacement scale by three.
Owner reports the MV-copy restoration build `B0973BAD...` looks good again.
`OptiScaler_2186987933154.log` confirms copied MVs at zero local bases, original
color/depth at 0/1280/2560, direct output and successful RR evaluation. This
implicates direct MV delivery in the tested setup; it does not prove an ignored
NGX key or that a copy is permanently required. This is the newest successful
reference, saved with source/log in `x64/rr-reference-B0973BAD/`. Keep MV copies
for subsequent guide-copy removal experiments; the goal still excludes debug
copies from the final implementation. Surface/smoke issues remain deferred.

Next optimization experiment sets `DebugCopyMaterialGuides=false`: original
diffuse/specular albedos and normals/roughness with tile subrects; MV copies remain
enabled, and hit-distance/optional effect guides remain copied. Set material flag
true to restore B0973BAD. Owner raised a pre/post-upscale MV-space hypothesis;
public RR helpers use the same MV subrect keys and pixel-space scale conversion
as SR, so no documented reversal is established. Internal allocation/extent
interpretation remains unresolved; MV scales are unchanged. This guide-direct
experiment requires owner image-quality comparison.

## Collaboration preferences

- The owner builds/debugs with Visual Studio 2022 and may use VS Code's Codex
  extension for editing and visual diff review. Both use this same repository.
- State whether you are editing source directly or creating an unapplied patch.
  Summarize which files and behaviors changed, and what was actually tested.
- Preserve existing uncommitted work. Keep changes scoped to the requested task.
- Do not introduce extra approval checkpoints for routine authorized edits.
- Distinguish owner-reported game results, code inspection, mock testing, and
  GPU validation. Avoid vague qualifications that imply a working mode is broken.
- Keep this context current when implementation decisions or validated results
  materially change; it should explain this fork rather than repeat upstream docs.
