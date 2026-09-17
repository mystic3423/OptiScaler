# Experimental DLSS integer crop

Implemented directly in source on 2026-09-17, following the owner's choice of
integer crop rather than a fractional alignment/resampling pass.

## Behavior

For input widths divisible by three, the three existing features still write
directly to their output subrects. At 11520 x 2160 output, each writes 3840 x 2160.
For other input widths:

1. Calculate three equal-width expanded input regions from the current frame's
   active render dimensions. Clamp the outer regions to the full frame edges.
2. Evaluate each region to a temporary **3856 x 2160** output.
3. Copy an integer-aligned **3840 x 2160** rectangle from each result to its
   monitor's region of the destination texture, preserving destination X/Y bases.

There is no final spatial upscale or seam blending. Output widths must still
divide by three. The 16-pixel expansion is fixed in output space; input expansion
changes with the active render size. Only horizontal geometry changes.

Expanded features are created on the first uneven frame and retained. Normal
features are retained too, so subsequent path changes reuse the appropriate set.
The temporary output is reused between tiles and frames, with a cache for format
changes. At RGBA16F and triple 4K it occupies about 64 MiB, in addition to the
three expanded features' internal resources. First use can incur a creation hitch.

History resets on switching paths, following a failed tile frame, or changing
render dimensions while cropped. Frequent uneven dynamic-resolution changes
can therefore reduce temporal accumulation quality.

## Approximation and remaining checks

Integer input origins and integer output crops cannot exactly represent every
fractional boundary. Equal expanded tile scales avoid assigning a different
scale to each tile, but differ slightly from the ideal full-frame scale.
CPU calculations measured a maximum **2.996 output-pixel crop-edge error** across
the tested 1x-3x scaling range. Larger ratios can have larger errors.

Render-resolution motion vectors use expanded input origins. Display-resolution
motion vectors use rounded/clamped expanded output-space origins. Their value
scales remain unchanged. These display-resolution coordinates are approximate;
the existing optional-input resolution assumptions still need validation.

DX12 transitions scratch output UAV -> copy source -> UAV, and destination UAV
-> copy destination -> UAV. This follows the existing OptiScaler output-state
contract; NVIDIA's [Streamline DLSS integration](https://github.com/NVIDIA-RTX/Streamline/blob/main/source/plugins/sl.dlss/dlssEntry.cpp)
also transitions NGX color output to storage read/write before evaluation.
DX11 uses CopySubresourceRegion and runtime-managed resource synchronization.
Neither copy path has been tested here on a GPU.

Suggested game checks:

- Divisible Ultra Performance input (3840 x 720) as the baseline.
- Nearby uneven widths such as 3841 and 3842, including startup at those sizes.
- Transitions between divisible and uneven widths, and repeated changes in
  uneven sizes. Watch for history flashes, flicker, and creation hitches.
- Pan past thin geometry and text on all three monitors and across both seams.
- Repeat with games providing render-resolution and display-resolution MVs.
- Compare upscaler time and VRAM before and after the first uneven frame.

## Verification performed

- Release x64 built and linked successfully. Existing compiler/linker warnings
  remain. The prebuild metadata event initially failed with its recurring file
  access error and left the generated date/commit headers empty. Restored the
  headers, then built with `/p:PreBuildEventUseInBuild=false`. The project event
  itself is unchanged. Packaging reported an existing missing copy path; verify
  auxiliary DLL packaging separately if a complete distribution is needed.
- Standalone CPU test passed **81,910** layouts: every input width 3..16384
  against output widths 5760, 7680, 11520, 12288, and 16383. Checked input/crop/MV
  bounds, outer edges, identical scales, complete disjoint destination coverage,
  unchanged divisible geometry, and rejection of invalid dimensions.
- Mock parameters checked both MV modes, original input bases, nonzero output
  bases, reset preservation, fallback render dimensions, and scope restoration
  for both API resource types when returning after each possible tile.
- No NGX runtime or in-game image-quality/performance validation was performed.

Run the CPU test from a VS 2022 x64 Developer PowerShell at the repository root:

```powershell
cl /nologo /EHsc /std:c++20 /W4 /Iexternal\nvngx_dlss_sdk tests\dlss_tiling_tests.cpp /Fo:x64\dlss_tiling_tests.obj /Fe:x64\dlss_tiling_tests.exe
if ($LASTEXITCODE -eq 0) { & .\x64\dlss_tiling_tests.exe }
```
