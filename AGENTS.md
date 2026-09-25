# Context for this fork

This fork, `mystic3423/OptiScaler`, exists to make DLSS Super Resolution work
at **11520 x 2160**, using three 4K monitors in NVIDIA Surround. Read this
context before proposing changes to the tiling implementation. This is an
experimental workaround with working game builds, not a finished general solution.

## Branch separation (owner instruction, 2026-09-19; updated 2026-09-21)

- `dlss-tiling` is the separate DLSS Super Resolution work.
- `dlss-rr-tiling` is the experimental Ray Reconstruction work split from it.
  The owner also refers to this as the Ray Regeneration work; this branch's
  current implementation targets NVIDIA RR, not AMD Ray Regeneration.
- `dlss-fg-tiling` is the current branch, split from `dlss-rr-tiling` so its
  git history and this file's RR narrative stayed available as reference. Its
  actual source was then reset to match `dlss-tiling` exactly (no RR code is
  present); only this AGENTS.md and `docs/investigations/cyberpunk-ray-reconstruction.md`
  retain the RR content. This branch's own focus is Frame Generation (FG), not RR.
  Unlike RR, which fully replaces the SR path (its DLUnified backend does its own
  upscaling), FG runs downstream of and in tandem with SR/RR: it takes the already
  upscaled/composited frame plus depth, motion vectors and HUD-less color, and
  synthesizes extra interpolated frames to raise displayed FPS without changing
  output resolution. The SR tiling problem (working around DLSS's output-width
  limit) and the FG problem (whether FG itself needs tiling, and whether tiling a
  temporal algorithm is even sound) are different problems; do not assume findings
  from SR/RR tiling transfer to FG without checking the reasoning in each case.
- Keep these efforts separate until each works correctly. Always inspect the
  current branch and working tree; do not assume RR or FG changes or test results
  apply to `dlss-tiling`, and do not merge/backport experiments automatically.
- The isolated input/output copy path (RR-specific) is a temporary diagnostic, explicitly
  not intended for the final implementation. Source edits are direct, not patches.
- Branch-independent reminder: `../AGENTS.md` (OptiScaler-only section).

## Current FG focus (this branch, 2026-09-21)

No FG source changes have been made yet; this is a feasibility assessment only,
requested by the owner before any implementation. Findings below are from reading
this fork's existing `OptiScaler/framegen/` code and bundled FidelityFX-SDK source,
not from GPU testing or an external reference implementation of tiled FG.

- OptiScaler already supports multiple FG backends: native NVIDIA DLSS-G (routed
  through Streamline, `framegen/dlssg/DLSSG_Dx12.*`), AMD FSR3 frame interpolation
  (open-source FidelityFX-SDK, `framegen/ffx/FSRFG_Dx12.*`), Intel XeFG
  (`framegen/xefg/XeFG_Dx12.*`), and several NVNGX-facing proxy/compat shims under
  `framegen/nvngx/`. They share a common `IFGFeature`/`IFGFeature_Dx12` interface.
- DLSS-G's real work happens inside NVIDIA's closed Streamline plugin
  (`sl.dlss_g`), bound to a single swapchain viewport via `sl::ViewportHandle`, and
  is understood to use dedicated Optical Flow Accelerator (OFA) hardware on the
  full frame. This is architecturally unlike SR/RR, where OptiScaler itself calls
  `NVSDK_NGX_D3D12_CreateFeature`/`EvaluateFeature` per handle, which is what made
  creating three independent per-tile handles possible. There is no equivalent
  per-region handle creation exposed for DLSS-G; tiling it the way SR/RR were
  tiled does not look possible at the application level. Whether native DLSS-G
  even hits a width-style limit at 11520 has not been tested.
- FSR3's frame interpolation is open source and shader-based (already vendored
  under `external/FidelityFX-SDK` and `OptiScaler/include/fsr3*`), so it is at
  least inspectable/modifiable, unlike DLSS-G. But its optical-flow and
  interpolation/inpainting passes (`ffx_frameinterpolation_optical_flow_vector_field.h`,
  `ffx_frameinterpolation_game_motion_vector_field.h`,
  `ffx_frameinterpolation_compute_inpainting_pyramid.h`) already use a single
  `InterpolationRectBase()`/`InterpolationRectSize()` concept, but that exists for
  letterboxing/UI-safe-area purposes (see `IFGFeature::SetInterpolationRect`
  usages), not for splitting one frame into independently processed regions.
  Motion estimation performs mip-pyramid block matching across the whole frame;
  naively running it three times on independent horizontal tiles would very likely
  reproduce or worsen the cross-boundary motion problems already seen in the RR
  tiling investigation (an object's flow vector needs source pixels outside its
  own tile once it approaches a tile edge), and frame interpolation is a temporal,
  whole-image synthesis rather than SR/RR's local per-pixel spatial resampling, so
  seam artifacts would likely be more visible, not less.
- Importantly, by the time FG would run, SR tiling has already produced one
  seamless composited 11520x2160 frame (the real swapchain back buffer). FG
  backends are not known to have the same internal per-feature width-limit check
  that motivated SR/RR tiling in the first place; that check appears specific to
  `NVSDK_NGX_D3D12_CreateFeature` for DLSS/DLSSD. So the open question is not only
  "can FG be tiled" but "does FG even need tiling at 11520 wide," which is untested.
- Recommended first experiment (not yet done): try running FSR3 FG and/or DLSS-G
  untiled against the already-tiled SR composited output at 11520x2160, and see
  whether creation/evaluation succeeds or fails the way full-frame RR did before
  tiling was needed. If it succeeds untiled, no FG tiling work is needed at all.
  If it fails with something width-shaped, a full-frame-once-optical-flow +
  tiled-reconstruction hybrid (compute flow globally, only tile the final
  reconstruction/output write) is a more promising direction than three fully
  independent per-tile FG passes, but would still be a substantially larger
  undertaking than SR/RR tiling given FG's temporal, whole-frame nature and its
  tie-in to Present-time frame pacing/Reflex signaling.

### DLSS-G viewport/subrect finding (2026-09-21, from the real Streamline guide)

The owner asked whether NVIDIA's own developer docs had been checked; the
assessment above was code-only. Fetched and byte-verified (via `curl`, not just
an AI summary) NVIDIA-RTX/Streamline's `docs/ProgrammingGuideDLSS_G.md`
(`main` branch, matches the vendored `external/streamline` headers' API shapes).
This meaningfully updates the earlier "no per-region handle exists" conclusion:

- Section 5.3 "MULTIPLE VIEWPORTS": "DLSS-G supports multiple viewports.
  Resources for each viewport must be tagged independently... Input resources
  may differ between viewports, but all viewports write to the same back
  buffer." Each viewport is tagged via its own `slSetTagForFrame()` calls, using
  `sl::ViewportHandle`, matching the header found locally at
  `external/streamline/sl_dlss_g.h` (`slDLSSGSetOptions`/`slDLSSGGetState` both
  take a `viewport` parameter).
- Section 5.0/5.2 documents a `sl::Extent` subrect explicitly for the backbuffer
  tag: `sl::Extent backBufferSubrectInfo {128, 128, 512, 512}; // backbuffer
  subrect info to run FG on.` `sl::ResourceTag` (in
  `external/streamline/sl_core_types.h`) carries this same `Extent` for any
  tagged resource. This is conceptually the same per-region subrect idea that
  made SR/RR tiling possible via NGX, but expressed through Streamline's tagging
  API instead of NGX's per-handle Width/Height/subrect-base parameters.
- Section 4.0 "HANDLE MULTIPLE SWAP-CHAINS" is unrelated to this idea: it is
  about DLSS-G attaching to only one of several swap chains an app/editor may
  create (e.g. editor viewports), not about spanning one swap chain across
  multiple outputs.
- No maximum-resolution limit is documented anywhere in the guide (checked for
  8192/16384-style width caps and found none); only a documented minimum
  (`sl::DLSSGSettings::minWidthOrHeight`, undocumented numeric value, exposed as
  `DLSSGStatus::eFailResolutionTooLow`). This further supports trying DLSS-G
  untiled at 11520 wide before assuming any width-driven failure.
- What the guide does NOT discuss anywhere: split-screen use, cross-viewport
  motion continuity, or how multiple viewports' independently-interpolated
  regions are expected to compose seamlessly at their shared boundary within one
  back buffer. The absence of guidance here is the same shape of risk already
  raised for FSR3 above (each viewport's optical flow only sees its own tagged
  extent), just now with a documented, sanctioned API surface for attempting it
  rather than no surface at all.

Owner plan (2026-09-21): test FG directly in a game against the existing SR
tiling fix before any FG-specific source work. Untested; nothing implemented.

Build/install for this test (2026-09-21): Release x64 built successfully via
MSBuild (existing C4250 dominance and LNK4098/LNK4744 warnings only, zero
errors; the documented missing-packaging-copy-path message still appears and
was not investigated further). No source changes were made; this is the plain
`dlss-tiling`-equivalent code already on this branch, with no FG-specific
changes. Installed to
`C:\Program Files (x86)\Steam\steamapps\common\Marvel's Spider-Man
Remastered\` (game was closed; no `Spider-Man.exe`/`crs-*` processes running
at install time). Built and installed `dxgi.dll`/`OptiScaler.dll` hashes match:
`DACB80CBB0D295AC922F9B713168BCCA72F07E1243C6DA3D121A1A404A6F839B`. Previous
installed DLL backed up beside the game as
`dxgi.dll.before-fg-tiling-build-20260921-211604.bak`.

Set the game's existing `OptiScaler.ini` `LogToFile = true` and `LogLevel = 0`
(Trace); both were previously `auto` (`LogToFile` auto resolves to false, so
logging was effectively off despite `LogLevel` auto already meaning Trace).
No other ini keys were touched, so existing tuned settings (FG backend choice,
tiling options, etc.) are preserved. Previous ini backed up as
`OptiScaler.ini.before-fg-tiling-trace-logging-20260921-211604.bak`. Owner can
now launch and test FG; each session will write a new timestamped
`OptiScaler_<n>.log` (SingleFile was already false) at Trace level beside the
game exe.

Owner report: FG "looks like it does run" in Spider-Man Remastered with the
existing SR tiling fix. This confirms basic activation/execution, not image
quality; seam/ghosting behavior at the tile boundaries and any Trace-log detail
have not yet been reviewed by the agent. Owner is moving on to test a second
game next; which game was not yet specified at this note's time.

Second game (2026-09-21): Avatar: Frontiers of Pandora ("AFOP"), installed at
`C:\Program Files (x86)\Ubisoft\Ubisoft Game Launcher\games\AFOP`. Owner had
already copied the same build's `dxgi.dll` there themselves (hash-verified by
the agent to match the Spider-Man build,
`DACB80CB...A6F839B`); this game also ships `nvngx_dlssd.dll` (RR), unlike
Spider-Man. Owner explicitly requested no backup this time. Set only
`LogToFile = true` and `LogLevel = 0` (Trace) in the existing tuned
`OptiScaler.ini`, same as the Spider-Man change; no other keys touched, no
backup taken. Game was not running at the time of the edit. Owner test pending.

AFOP FSR3-FG crash (2026-09-21): owner enabled FSR3 frame generation by
accident (meant to test native DLSS-G) while using DLSS SR (not RR) for
upscaling, and the game crashed. `OptiScaler.log` (this game uses a single
fixed filename, not the timestamped-per-session pattern; SingleFile is
presumably true here) shows normal-looking operation up to the crash: DLSS SR
evaluate calls at Render 5875x1102 -> Target/Display 11520x2160 (OptiHandle
1000000; the log's evaluate line does not distinguish per-tile native handles,
so this does not by itself confirm or rule out 3-tile SR routing), then FSR3 FG
context creation (`CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12`
then `CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION`, both `result: 0` success), then
two full successful `DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE`/
`DISPATCH_DESC_TYPE_FRAMEGENERATION` cycles. The log then ends immediately
after a third `DISPATCH_DESC_TYPE_FRAMEGENERATION` dispatch call, with no
further lines, no logged error, and no graceful-shutdown message: consistent
with a hard crash during/after that GPU dispatch, not an OptiScaler-detected
failure path.

Windows Application event log (`Get-WinEvent` "Application Error" provider)
confirms two separate crash reports, both: faulting module `afop.exe` itself
(the game's own executable, not `dxgi.dll`/`OptiScaler.dll`, not
`amd_fidelityfx_framegeneration_dx12.dll`, not an NVIDIA driver module),
exception code `0xc0000005` (access violation), and the *same* fault offset
`0x000000000162b730` in both occurrences. Two independent crashes at the exact
same unsymbolized address suggest a reproducible fault, not random corruption,
but the agent has no symbols for `afop.exe` to resolve that offset to source;
whether the game is faulting on a resource that FSR3 FG/tiling produced in a
shape it didn't expect, or something unrelated to this fork's changes, is not
established. No FSR3/dispatch-level error was logged before the crash, so if
this is tiling-related the failure mode is silent corruption rather than a
loud rejection (unlike NGX's `0xBAD0000D`-style creation failures).

Since this used FSR3 FG (accidental) rather than the native DLSS-G the owner
actually intends to evaluate for this branch, this result should not be read
as a verdict on DLSS-G's viewport/subrect approach discussed above. It is a
real data point that untiled FSR3 FG did not survive multiple frames against
this fork's 11520-wide tiled SR output in this game, which is still relevant
background if FSR3 FG tiling is ever revisited. Owner intends to retry with
DLSS-G explicitly selected instead.

Recovery (2026-09-21): owner reported the game now crashes before the title
screen whenever FG is enabled, so the in-game overlay is unreachable to turn it
off. The unchanged `OptiScaler.log` mtime/content confirms newer launch
attempts crashed too early to log anything new (or before Logger init).
`[FrameGen]` `FGInput`/`FGOutput` were still `auto` in the ini (the earlier
crash was likely the *game's own native* FSR3 FG request, which OptiScaler's
FFX API hooks intercept regardless of `[FrameGen] Enabled`, so that key alone
may not have been the actual gate). Force-disabled via direct source-confirmed
kill switch (`Config.cpp`/`dllmain.cpp` honor `FGOutput=NoFG`/`FGInput=NoFG` as
an explicit override, not just "no OptiScaler-injected FG"): set
`[FrameGen] Enabled = false`, `FGInput = nofg`, `FGOutput = nofg` (previously
all `auto`). No backup taken, matching the owner's earlier no-backup request
for this game/session. Owner should retry launching now; if it still crashes,
the trigger is likely upstream of OptiScaler (e.g. the game's own native
FSR3 FG toggle persisted in its own settings, outside OptiScaler.ini) and the
game's own graphics settings file would need to be located and reset instead.

Correction (2026-09-21): owner clarified the crash was always the game itself
crashing (matches the earlier Windows Application-error finding: faulting
module `afop.exe`, not `dxgi.dll`/`OptiScaler.dll`/the FFX DLL), not something
OptiScaler's own `[FrameGen]` gate controlled. Found the real switch:
`C:\Users\brand\Documents\My Games\AFOP\graphic settings.cfg`, a small Lua-like
table the game itself writes, `Video.frameGeneration = 1` with
`frameGenerationMode = 0` (FSR, matching the owner's "the fsr one by
accident"), `rayReconstruction = false` (matches "using DLSS SR not RR").
Changed only `frameGeneration` from `1` to `0`; nothing else in the file was
touched. The file also carries a `hash = -320276597` field (likely a
content-integrity check the game computes over the rest of the table); its
algorithm is unknown to the agent, so this edit was not re-hashed. If the game
rejects the file as corrupted it will likely reset to a default preset
(playable again, but the owner's other tuned graphics settings would be lost)
rather than fail to launch; this was not verified before reporting. Owner
should relaunch to confirm; if a hash mismatch resets other settings, those
would need retuning separately from this fork's code.

Spider-Man DLSS-G not loading (2026-09-21/23): with OptiScaler FGInput/FGOutput
= DLSSG, trace logs show Streamline 2.4.0 `Ignoring plugin 'sl.dlss_g' since it
is was not requested by the host`, then `Can't init StreamlineProxy, disabling
FGOutput`. The game decides at Streamline init which features to request, so an
in-game toggle after startup cannot fix that launch. Owner also reports a 4K
no-OptiScaler control did not work. As of 2026-09-23 the folder holds the stock
pairing again: `nvngx_dlssg.dll` 3.7.0 with all `sl.*` at 2.4.0 (the swapped
DLL is no longer present); `nvngx_dlss.dll` is 310.9.1. HAGS is on; driver
reports 32.0.16.1656. Community reports (Steam threads) say DLSS FG only appears
after selecting DLSS Super Resolution and restarting the game. Swapping only
`nvngx_dlssg.dll` in Streamline games risks a version mismatch with the old
`sl.*` runtime. The NVIDIA App's Enhanced FG preset list does not include
Spider-Man Remastered. Not verified by the agent in-game.

Width gate ported (2026-09-23, owner request): applied only the SR
`TargetWidth() < 8192 ? 1 : DLSSTiling::TileCountFromEnv()` change from
`dlss-rr-tiling` commit a83312ab to `DLSSFeature_Dx11.cpp` and
`DLSSFeature_Dx12.cpp` (the commit also holds RR code, so it was not
cherry-picked whole). Both files now match `dlss-rr-tiling` byte-for-byte.
Outputs narrower than 8192 use ordinary single-feature DLSS for A/B comparison.
Release x64 built with exit code 0; packaged `dxgi.dll` SHA256
`3D5E823162274EA139F34E1CB9BEBB1E9B446F174DED095572641A2473873C6B`.
Installed to Spider-Man Remastered on 2026-09-23 as `dxgi.dll` and
`OptiScaler.dll`; both hashes match the build. No `dxgi.dll` was present
beforehand because the owner had renamed the previous build to `dxgi.dll.bak`
for the no-OptiScaler 4K test, so no new backup was taken. Not yet tested in-game.

OptiScaler DLSSG output setup (2026-09-23): the owner's Spider-Man ini has
`FGInput = DLSSG` and `FGOutput = DLSSG`. That output makes OptiScaler load its
own Streamline from `<game dir>\streamline\` (`Streamline_Proxy.h`), which did
not exist, so it showed "Can't init DLSSG Output / Are you missing the
streamline folder?" and disabled FG. This fork's Release packaging does not
ship Streamline files. Created that folder from NVIDIA's official
`streamline-sdk-v2.14.1.zip` GitHub release, production `bin/x64` files only:
`sl.interposer`, `sl.common`, `sl.dlss_g`, `sl.reflex`, `sl.pcl`, `sl.dlss`
(all 2.14.1.0) and `nvngx_dlssg.dll` 310.9.1.0. All are Authenticode-valid,
signed by NVIDIA Corporation. The SDK's `nvngx_dlssg.dll` is byte-identical to
the one DLSS Swapper put in the game folder. The game's own `sl.*` files stay
at 2.4.0 and were not touched. The repo headers are Streamline 2.11.1.

First run (2026-09-23, `OptiScaler_5949955483943.log`, 3840x2160): the width
gate works; DLSS SR ran single-feature at 1280x720 -> 3840x2160 with no
"Tiled DLSS" line. OptiScaler's own DLSS-G (SL 2.14.1 from `streamline\`)
activated at 22:37:18 and dispatched about 20k times with `Result: Ok`, 2x.
Owner says the NVIDIA overlay changed to look like Preset B in another game,
and reports a lot of tearing in the middle of the screen. Log facts: NVCP
VSync "Force ON" is overriding the app and SL's RSYNC reports VSync enabled;
the game presents with SyncInterval 0 plus ALLOW_TEARING. The game's own SL
2.4.0 logged 9,822 times that it supports one swap chain and is skipping
Present hooks on the second one, so two Streamline runtimes are in the process.
Reflex frame IDs jumped from ~2982 to ~5.93e18 right as FG activated. SL
warned once that camera matrices (cameraViewToClip, clipToPrevClip, etc.) are
invalid and once that no backbuffer extent was given. Cause of the tearing is
not established. Suggested A/B: FGInput/FGOutput off so the game's native
DLSS-G runs through OptiScaler, same scene.

Streamline 1 ruled out (2026-09-23): the owner asked whether Spider-Man is an
unsupported SL1 game. It is not. The upstream wiki's SL1 list (Witcher 3,
Dying Light 2, Returnal, A Plague Tale Requiem, etc.) does not include it, and
the log shows the game's `sl.interposer.dll` is Streamline 2.4.0, hooked as v2.
The wiki recommends the DLSSG-via-SL input for games with native DLSS-FG.
Stronger proof than the list: every Spider-Man log shows the game's slInit
reporting `host SDK v2.4.0`, meaning the game itself was built against the
Streamline 2.4.0 SDK. The game's own wiki page lists "DLSSG via SL ->
FSR-FG/XeFG" as working, tested only on AMD GPUs with OptiScaler 0.9; it does
not list the DLSSG output this setup uses.

Wider search (2026-09-23): no public report was found of DLSSG input -> DLSSG
output on NVIDIA, and nothing on tearing with the DLSSG output. Closest leads:
upstream issue #1094 (Starfield, AMD): with DLSSG input + FSR-FG output, the
game's native DLSS-G also ran, causing latency, swapchain recreation and
crashes; a local patch blocked native `slSetFeatureLoaded(kFeatureDLSS_G)`.
Spider-Man does not show that: in the last three logs the game's SL 2.4.0 skips
`sl.dlss_g` and only OptiScaler's SL 2.14.1 loads it. The dlss-unlocked
project ships SL 2.14.1 in `OptiScaler/streamline/` for the DLSSG output on
NVIDIA; its issue #35 (RTX 4070) reports FG frames not improving smoothness and
was closed with no documented fix. Upstream #1087 notes DLSSG-via-SL -> FSR-FG
hitting FPS limits, suspected to be the FSR-FG swapchain.
Overlay "Current DLSSG state: OFF" (2026-09-23): display quirk, not FG off.
The label reads `State::dlssgDetectedInterpolationCount`, set only in
`NVNGX_DLSS_Dx12.cpp` when an NGX `NVSDK_NGX_Feature_FrameGeneration` evaluate
passes through OptiScaler's NGX hook (reads `DLSSG.MultiFrameCount`). With the
DLSSG output, OptiScaler's own SL 2.14.1 calls the driver's NGX directly, so
the hook never sees it. `OptiScaler_5966058486324.log` shows 0 such evals but
14,758 `DLSSG_Dx12::Dispatch Result: Ok`, and "Streamline FG state: ON".
HUD hypothesis (2026-09-23): FSR-FG input showed the same artifact; owner
reverted to DLSSG input/output. Owner reports "Show Detected UI" tints scene
elements, not just HUD, and suspects FG is misreading HUD inputs rather than
bad pacing. That toggle (`shaders/hudless_compare`) tints pixels where the
game's hudless texture and the final back buffer differ by more than 0.003 per
channel (`HC_Dx12.cpp`), below one 8-bit step, so post effects after the
hudless capture can also tint. `OptiScaler_5977347793960.log`: one hudless and
one UI texture per frame; the second hudless SetResource each frame is inside
OptiScaler's own Dispatch with the same texture. The game reports hudless and
UIColorAndAlpha with frameId 0 (SL 2.4 style). Unverified which is the cause.
Owner follow-up: lowering OptiScaler's Reflex FPS limit from the VRR
calculator's 116 to 114 seemed better, with "Disable UI texture" still
unchecked. Early impression only. Owner paused this investigation on
2026-09-23 to play for longer sessions and separate pacing tearing from
bad-input artifacts.
Planned next test (owner suggestion, mimic FSR-only games): in-game frame
generation set to FSR 3.1, OptiScaler `FGInput = fsrfg`, `FGOutput = DLSSG`.
The ini was not edited because the game was running. Applied later the same
day with the game closed: `FGInput` changed from `DLSSG` to `fsrfg`,
`FGOutput = DLSSG` unchanged. Owner must pick FSR frame generation in game.

Net effect: multi-viewport DLSS-G tiling is not ruled out at the API level the
way it looked before this check, and Streamline explicitly supports the
mechanics (multiple viewports, per-resource Extent, shared backbuffer). Whether
it avoids the cross-tile ghosting the RR investigation fought for weeks remains
untested and undocumented either way. This is still meaningfully cheaper to
prototype than reworking FSR3's open-source shaders, since it only requires
driving an already-supported public API three times with different extents,
rather than modifying FidelityFX's optical-flow/inpainting passes. It should be
tried after (not before) the plain "does untiled FG work at 11520 wide at all"
experiment above, since if untiled DLSS-G already works, none of this is needed.

### FG tiling implementation (2026-09-24, owner request)

Owner asked to implement FG tiling now. Direct source edits, no patch. Untiled
DLSS-G at 11520 wide has still not been tested, so tiling is not yet proven
necessary; `DLSSGTiling::EnableTiling = false` restores the single-viewport
path at every width for an A/B comparison (rebuild required).

Scope: only OptiScaler's own DLSS-G output (`FGOutput = DLSSG`,
`framegen/dlssg/DLSSG_Dx12.*`). FSR-FG and XeFG outputs, and the game's own
native DLSS-G, are unchanged.

- New `OptiScaler/framegen/dlssg/DLSSGTiling.h` (added to the project/filters):
  `TileCountForDisplay` (3 tiles when display width >= 8192, same rule as the
  SR gate), `SplitSpan` (contiguous, gap-free proportional split; uneven widths
  differ by at most one pixel), `ApplyTileToProjectionRowVector` (same
  off-center tile projection formula as the RR work), and debug constants
  `EnableTiling` and `MVScaleUsesTileWidth`.
- `DLSSG_Dx12::Dispatch`: at >= 8192, turns on Streamline viewports 0..2 with
  identical DLSS-G options; per viewport it tags the backbuffer with only that
  monitor's subrect (null resource, `kBufferTypeBackbuffer`, extent
  x = 0/3840/7680, w 3840 at 11520) and sets its own constants: tile aspect
  ratio, tile-local off-center `cameraViewToClip`/`clipToCameraView` when
  OptiScaler synthesises the projection, and `mvecScale.x` normalised to the
  tile's MV extent width (pixel displacement unchanged). Reset is forced on
  single <-> tiled transitions; unused viewports are turned off. Below 8192
  the code makes the same calls as before on viewport 0.
- `DLSSG_Dx12::SetResource`: each depth/MV/hudless/UI tag is sent once per
  viewport with that tile's slice of the resource's own extent (render-space
  for depth/low-res MVs, display-space for hudless/UI).
- `Deactivate` turns off every viewport that Dispatch enabled.

Assumptions and risks (not validated): Streamline normalises MVs relative to
the tagged extent (flip `MVScaleUsesTileWidth` if motion is 3x off on tiles);
DLSS-G honours nonzero extent origins (the RR work found NGX RR did not for
direct subrect inputs; tile 0's zero origin could hide such a bug again); a
null-resource backbuffer tag with no command list is accepted; hudless/UI
extents match the backbuffer tile only when the game's hudless covers the
full frame from x = 0. Each tile has independent optical flow and history, so
seams or ghosting at monitor boundaries are expected risks. The SL guide does
not document cross-viewport continuity.

Validation: `tests/dlssg_tiling_tests.cpp` (build command in its header)
passes 59,447 CPU checks: tile-count gate, split coverage for widths 0-12000,
known layouts (11520, 3840, AFOP's 5875), render/display boundary agreement
within half a pixel, projection correspondence for three tiles (x shifts by the
tile origin, y/z/w unchanged). Existing SR suite still passes 81,910 cases.
Release x64 built with zero errors and no warnings in the DLSSG files; packaged
`dxgi.dll` SHA256 `51FB0E94AB96A01D57DD90B2E070DF962D90751AA914E1E329FE3620E9A68217`.
Installed to Spider-Man Remastered on 2026-09-24 (game closed); installed
`dxgi.dll`/`OptiScaler.dll` hashes match the build. Previous width-gate build
(`3D5E8231...`) backed up beside the game as
`dxgi.dll.before-fg-tiling-20260924-164652.bak`. Game ini has FGInput/FGOutput
= DLSSG, LogToFile true, LogLevel auto (resolves to Trace); the `streamline`
folder is present. No in-game or GPU testing by the agent yet. Useful log
markers: `DLSSG tiling: 0 -> 3 viewport(s)`, per-tile `Tile N: backbuffer x`
lines, and `SetTagForFrame ... viewport: N ... extent:`.

First in-game run (2026-09-24, `OptiScaler_6674985597610.log`, 11520x2160,
Ultra Performance 3840x720): owner reports tiled FG "seemed to work fine",
apart from the tearing already seen before tiling. Log confirms SR tiling (3
tiles) and `DLSSG tiling: 0 -> 3 viewport(s)` twice; per-tile lines show
backbuffer x 0/3840/7680 w 3840, MV x 0/1280/2560 w 1280, tile aspect 1.778.
The game crashed after the owner changed several settings. Streamline 2.14.1
caught an exception at 18:50:57.522 and wrote
`C:\ProgramData\NVIDIA\Streamline\Spider-Man\1790290257521211\sl-sha-98614dad6.dmp`.
cdb on that dump: access violation (read 0xCAF8) in `nvwgf2umx.dll` (NVIDIA D3D
driver) inside `ID3D12GraphicsCommandList::ResourceBarrier`, called from
OptiScaler's SL `sl.common` via `slSetTagForFrame`, from OptiScaler's
`dxgi.dll`, on the game's render thread. The log shows it was the HUD-less tag
for viewport 0 (tile 0), frame 9983, sent from the game's own tag call with the
game's command list; it later returned `eErrorExceptionHandler`. OptiScaler
then deactivated and reactivated FG, and rendering froze. No Windows
Application Error or driver-reset event was logged. The same viewport-0 tag
call exists without tiling, so tiling is not established as the cause; the
settings changes (missing depth/MV tags at 18:50:38-39) are another candidate.
Logging volume: 72 MB in ~6.5 min at Trace. Tiling adds ~9% (per-tile Dispatch
lines plus three SetTagForFrame lines per tag, all Debug level).

Tearing solved (2026-09-24): it was a VSync ownership conflict, not tiling and
not the HUD inputs. `OptiScaler_6681961633265.log` showed OptiScaler's
`[V-Sync] ForceVsync = true` setting SyncInterval 1 and stripping
DXGI_PRESENT_ALLOW_TEARING on all 6,755 presents, while Streamline logged
`NVCPL 'Force OFF' overriding app VSync request - VSync disabled` and
`DRS VSync mode is Force OFF`, with `fullscreen iFlip` flipping 0/1. Owner set
the NVIDIA app to "Use the 3D application setting" and kept VSync on in
OptiScaler; tearing is gone and the owner reports tiled FG "looks so good now".
Record this pairing: driver on app-controlled, VSync forced in OptiScaler. The
earlier suspects (Reflex frame-ID jumps, hudless/UI inputs, the "Show Detected
UI" pink tint, the 114 vs 116 cap) did not fix it and were not the cause,
though the 114 cap helping is consistent with a VRR-range effect. Tiled DLSS-G
at 11520x2160 is therefore owner-confirmed working for image quality; seam
behaviour over long sessions and the 18:50 crash remain open.

Outer-edge band (2026-09-24, owner report, assessment only): during fast yaw a
band along the left edge of the left monitor and right edge of the right
monitor is not interpolated cleanly, ending in a visible vertical line; same
width on both sides, thinner bands at top/bottom; something similar but milder
at 3840x2160. Leading hypothesis: normal FG screen-edge disocclusion, amplified
by the 48:9 perspective. Under yaw, screen-space speed in tan units is
omega * (1 + x^2); the outer Surround edge sits at x = tan(vfov/2) * 48/9
versus 16/9 for a 4K edge, so the outer edges move roughly 4-6x faster for
vertical FOVs of 50-70 degrees, and yaw also creates vertical motion near the
outer corners. An MV-scale error would affect whole tiles, not just outer
edges. Monitor seams sit at the same x as a 4K edge, so a seam band like the
4K edge band would indicate tiles are not sharing context; not yet reported.
Also found: OptiScaler's DLSSG output never forwards the game's own camera
matrices (`clipToPrevClip`, `cameraViewToClip`, etc.); `Sl_Inputs_Dx12` reads
the projection only for near/far, and `DLSSG_Dx12::Dispatch` leaves matrices
zero when the game supplies camera vectors. The game's native DLSS-G receives
them. Whether that affects the edge band is unknown. Suggested checks: seams,
corner-weighted top/bottom band, and native DLSS-G vs OptiScaler DLSSG output
at 3840x2160 with the same spin. One-off SL warning this session: MV extent
1280x720 exceeded a 4x4 MV resource (likely a loading-screen placeholder);
extents are not clamped to resource size.

## Current RR handoff

Note (2026-09-21): everything from here through the end of this file describes
`dlss-rr-tiling`'s history and is retained on `dlss-fg-tiling` as background
context only. None of this RR source exists in this branch's working tree
(it was reset to `dlss-tiling`); do not assume any RR file path, flag or
handoff state mentioned below is present here.

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
remaining. Direct-output optimization is under test. DX11/Vulkan RR tiling is
not implemented. On `dlss-fg-tiling`, experimental DLSS-G tiling exists for
OptiScaler's own DLSSG output only (see "FG tiling implementation" above).

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
