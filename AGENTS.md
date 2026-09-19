# Context for this fork

This fork, `mystic3423/OptiScaler`, exists to make DLSS Super Resolution work
at **11520 x 2160**, using three 4K monitors in NVIDIA Surround. Read this
context before proposing changes to the tiling implementation. This is an
experimental workaround with working game builds, not a finished general solution.

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
are not the only intended sizes. The present scope is SR on DX11/DX12; this
does not establish tiled support for Ray Reconstruction or Frame Generation.

## Current implementation and decisions

Snapshot: 2026-09-17. Inspect the working tree and Git history before acting;
some changes may still be uncommitted or may have evolved since this note.

- Main files: `OptiScaler/upscalers/dlss/DLSSTiling.h`, `DLSSFeature.h`,
  `DLSSFeature_Dx11.cpp`, and `DLSSFeature_Dx12.cpp` in the same directory.
- `TileCountFromEnv()` intentionally returns **3** immediately. The environment
  variable implementation is leftover scaffolding. The owner explicitly wants
  the hardcoded debug hotfix preserved for now. A future OptiScaler option is
  preferred over an environment variable; do not implement that prematurely.
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
