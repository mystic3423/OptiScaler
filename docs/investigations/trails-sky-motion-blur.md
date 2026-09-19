# Trails in the Sky 2nd Chapter: motion blur investigation

## Owner report

- Slow camera pans produce a motion-blur-like effect; motion compensation feels wrong.
- Game folder: `C:\Program Files (x86)\Steam\steamapps\common\Trails in the Sky 2nd Chapter`.
- The owner reports an in-game map DLAA option and suspects it accounts for the
  secondary square DLSS pass. They may leave this option off while investigating.
  This association and any effect on the blur are not yet confirmed.

## Existing log evidence

Session on 2026-09-17, approximately 17:20–17:29 local time:

- File logging enabled at trace level.
- DX11 main feature: 5760 x 1080 input to 11520 x 2160 output,
  `LowResMV: true`, three 3840-pixel output tiles.
- A distinct feature is created and evaluated at 1536 x 1536 input/output,
  `LowResMV: false`, split into three 512-pixel output tiles.
- Brief main-feature evaluations at 11520 x 2160 input/output also appear.
- An attempted 6776-pixel input falls back to a full-width feature, whose
  creation fails with BAD00005. This identifies behavior from before the
  experimental integer-crop implementation; it does not test that new path.
- Motion-vector scales and texture descriptions are absent from this log.

## Follow-up: map DLAA off, Performance to Quality

Session on 2026-09-17, 21:55-21:58 local time. The owner reports normal-looking
motion with map DLAA left off, including after switching Performance to Quality.

- At 21:56:05, three tiles are successfully created for 5760 x 1080 input.
  4,214 subsequent evaluation records report that input and 11520 x 2160 output.
- At 21:56:49, three replacement tiles are successfully created for
  7680 x 1440 input. Starting at 21:56:50, 6,154 evaluation records report
  that new input and the same 11520 x 2160 output.
- Both initializations report `LowResMV: true`. The equal-split input geometry
  implied by these dimensions changes from 1920 x 1080 per tile to
  2560 x 1440; individual sampled resources/offsets are not logged.
- No 1536 x 1536 feature or evaluation appears. This supports the suspected
  association with map DLAA but does not establish that it caused the blur.
- Menu queries include 6776 x 1270 and 11520 x 2160, but these are optimal-size
  queries, not actual evaluations at those sizes in this session.
- No tiled create/evaluate failure is logged. Three FSR2 DX12 device-not-found
  errors remain, while the DX11 DLSS path initializes and evaluates afterward.
- The logged build stamp remains `20260917_153748`; this does not validate the
  newer integer-crop build. Both actually evaluated widths divide by three.

Next discriminating test, if desired: toggle map DLAA on and back off with the
same main-view settings/scene and compare both blur and the secondary feature.
No source changes or build were made for this follow-up.

## Secondary-pass handling proposal

Choose whether to tile separately for each feature, based on its output
dimensions. Passes that fit within the reported DLSS dimension bound should
use one ordinary DLSS/DLAA feature. The 11520 x 2160 main output still needs
three tiles; the 1536 x 1536 pass does not need tiling for this workaround.

Preserve the hardcoded tile count of three for features requiring tiling.
Do not decide from the first feature created, the overall display resolution,
or DLAA mode alone: full-Surround DLAA can still require tiling. A pass with
excessive height needs separate handling; horizontal splitting cannot fix it.

This is a proposal, not an implemented change or confirmed blur fix. A capture
or controlled map-DLAA on/off comparison is needed to identify the square pass.

## Pending diagnostics

- Compare map DLAA off/on and verify which feature disappears from the log.
- Record whether blur affects all monitors equally or mainly middle/right.
- Capture effective MV flags, scales, resource dimensions, original subrect
  bases and per-tile offsets, correlated by feature handle.
- Keep main-view blur and secondary-pass identification as separate questions
  until a controlled comparison establishes a connection.
