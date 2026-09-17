# Spider-Man Remastered: cutscene performance

Last updated: 2026-09-17. **Deferred; cause unconfirmed.** The owner suspects
the tiling workaround may have introduced this behavior. No controlled comparison
has established that yet. Resume this investigation when the owner is ready.

## Symptom and reported measurements

With tiled DLSS at 11520 x 2160, gameplay initially ranged from 70-120 FPS,
while cutscenes ran at 25-30 FPS. Performance returns immediately when a cutscene
transitions smoothly into gameplay, even without an abrupt scene change.

The latest comparison reported by the owner:

| Measurement | Gameplay | Cutscene |
| --- | --- | --- |
| FPS | About 80 | About 24 |
| GPU utilization | About 70% | About 70% |
| Reported DLSS/upscaler time | Well below 4 ms | Well below 4 ms |

Other settings and observations:

- Frame Generation is **off**; tiled FG has not been validated.
- Ultra Performance is currently selected, but other DLSS modes also exhibit
  the slowdown. Ultra Performance itself works with the tiling implementation.
- V-Sync is off. OptiScaler's FPS limiter is set to **116**.
- OptiScaler file logging is disabled for the current test.
- Disabling the game's Depth of Field setting did **not** eliminate the slowdown.
  The owner still sees blur/DOF on the sides during cutscenes. It is not established
  which pass produces this effect or how much GPU time it consumes.
- Whether the cutscene's active image spans all three monitors or uses a smaller
  region has not been confirmed.

These are owner-reported readings, not an agent-captured GPU trace. At 80 FPS
the frame interval is about 12.5 ms; at 24 FPS it is about 41.7 ms. The additional
interval is not explained by the reported DLSS GPU time alone. That does not
rule out indirect effects of tiling, CPU-side submission, synchronization, resource
pressure, or changes elsewhere in the game's rendering. GPU utilization alone
does not establish either a CPU or GPU bottleneck.

## Installation and existing evidence

Game directory:

```text
C:\Program Files (x86)\Steam\steamapps\common\Marvel's Spider-Man Remastered
```

The deployed `dxgi.dll` inspected during this investigation was dated
2026-09-17 15:38:07. The available `OptiScaler_*.log` files were dated September 16,
before the current build. No fresh log capturing the reported slowdown was found.

The longest older tiled log, `OptiScaler_4982093778263.log`, contains 44,164
evaluation-size records, all showing 5760 x 1080 input and 11520 x 2160 output.
It shows one tiled DLSS creation after switching to DLSS, without repeated tiled
creation or DLSS evaluation errors. Other Streamline messages exist; do not call
the log error-free. This log is **not** a confirmed capture of the current cutscene
issue and cannot rule out current resolution or lifecycle problems.

## Relevant code inspected

- `OptiScaler/upscalers/dlss/DLSSFeature_Dx12.cpp`: three tile evaluations;
  current code recalculates input regions each frame. No new cutscene-specific
  rendering fix was made during this investigation.
- `OptiScaler/upscalers/IFeature_Dx12.cpp`: GPU timestamps surround the complete
  `EvaluateInternal()` call, including all tiles. The displayed aggregate upscaler
  time also includes active RCAS/output scaling passes. This timer does not measure
  all game work or all CPU waits.
- `OptiScaler/misc/FrameLimit.cpp`: fallback limiter waits to enforce a minimum
  frame interval. A 116 FPS limit corresponds to about 8.6 ms under normal use.
- `OptiScaler/wrapped/wrapped_swapchain.cpp`: fallback limiting can run after
  `LocalPresent()` in `Present()`/`Present1()` when Reflex/XeLL are not limiting.
- `OptiScaler/hooks/Reflex_Hooks.cpp`: can implement the configured cap through
  Reflex sleep parameters. Which limiter method is active in this run is unknown.
- `OptiScaler/menu/menu_common.cpp`: limiter UI exposes **Current method**,
  **Apply Limit**, and **Reset Limit**. Reset sets the cap to zero.

Neither the limiter nor Reflex has been identified as the cause. Avoid claiming
the configured 116 FPS cap directly explains 24 FPS.

## Pending tests, in order

1. **Limiter isolation:** note the limiter's Current method, click Reset Limit
   (0 = disabled), and replay the same cutscene with all other settings unchanged.
   Record FPS and upscaler time. This test was proposed but has not been performed
   or reported. Restore the owner's preferred cap after the comparison.
2. **Establish whether this is a tiling regression:** compare a repeatable cutscene
   and gameplay transition with tiling enabled versus a valid single-feature path.
   Use the same output resolution and settings for both runs at a width accepted
   by single-feature DLSS. The current source intentionally forces three tiles;
   setting the leftover environment variable to 1 will not disable tiling.
   Any temporary test switch should preserve the owner's intended default and
   avoid changing the general tile-count policy without discussing scope.
   This comparison cannot by itself settle behavior unique to 11520-wide output.
   An alternate upscaler at full Surround width can provide additional context,
   but changes the upscaler and is not a pure tiling comparison.
3. **If unresolved, capture frame activity:** obtain CPU/GPU timing through the
   cutscene-to-gameplay transition. Look for expensive passes outside DLSS,
   long CPU waits, presentation/Reflex delays, memory pressure, or repeated
   resource/feature creation. Average utilization percentages are insufficient.
4. **Inspect per-frame inputs if evidence points there:** active render/output
   regions, subrect bases, reset frequency, MV mode and resource dimensions,
   evaluation count per presented frame, and errors. Use bounded diagnostics;
   verbose per-frame logging can perturb performance and must not become the
   basis of an uncontrolled FPS comparison.

Do not remove game reset signals, bypass DLSS size limits, or change sampling
alignment on the assumption that they cause this slowdown. Preserve working
tiling and distinguish each new hypothesis from a measured result.
