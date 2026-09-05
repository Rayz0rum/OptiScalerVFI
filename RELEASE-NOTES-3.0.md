Everything below is new since **Test Build 2.0**. For install, requirements and the arrangement each placement produces, see `READ ME FIRST - OptiNR.txt` in the archive.

Upgrading: replace `OptiScaler.dll` and `OptiScaler.ini`. An old ini still works — delete any `TransferBlur=` line from `[DlssNr]`, which is replaced by `ToneStrength`, `DetailStrength`, `DetailBand` and `DetailCompensation`.

## Fixed

**Toggling a setting could crash the game.** Rebuilding the enlargement freed textures the command list being recorded had already named — the rebuild ran from inside the dispatch, between the edit transfer writing the enlargement's source texture and the enlargement reading it, so `Release()` freed that texture in the gap and the freed pointer went to NGX as the colour input. The first pass's own output buffer went with it, while earlier commands in the same list were still writing into it.

The toggle was not really the cause. Any create-time latch changing mid-session does this, and a quality preset change has been able to since the feature existed. So the feature is now built before the pipeline records anything, and retired resources are parked for 32 frames rather than freed — the margin the rest of the module has always used and this object never did.

**The enlargement was given the wrong depth and motion vectors in Multi-pass Custom.** Both were captured before `ResampleFeature1Inputs` rewrites the parameter block, and rewriting it is the entire point of that mode: colour, depth and motion vectors are replaced with copies at the reduced size and the vector scale is adjusted to match. The second feature is created at that reduced size, so it received full-render-resolution guides for a pass expecting smaller ones — DLSS reads the top-left corner of each, which is a crop rather than a view. The motion vector scale read alongside them had already been scaled for the resampled buffers it was not being given.

**The enlargement was created for a colour space it never receives.** It cleared `IsHDR` and `AutoExposure` and bound an identity exposure, on the premise that Neural Rendering hands it a tone-mapped picture. It does not: the codec's resolve ends by multiplying back out of the normalised space it worked in, so the pass returns the frame in whatever space it was given — at zero strength, bit for bit what went in. The edit transfer goes further and applies a near-unity ratio to the game's own jittered buffer. Declaring that display-referred selects the programming guide's LDR path, which quantises to 8 bits and expects a perceptually linear encoding; handing it linear colour is that guide's own account of banding and colour shifting. Flags now follow what the game declared, with `MatchGameColourSpace` to go back.

**Preset matching silently skipped the titles it was for.** Presets were read into an `int` and tested `>= 0` to mean "the game named one", so a valid preset with the high bit set — Mortal Shell 2 reports `0x8000000B` — tested as negative and the enlargement was left on the driver's own independent choice.

**Highlight flickering in multi-pass.** The transfer took a ratio of averages where it wanted an average of ratios. A quotient of two interpolated quantities is not the interpolation of their quotient, and the gap grows with the gradient being interpolated: nothing on a flat surface, large on a specular highlight where a subpixel shift moves the sample a long way up the slope. Since the alignment offset follows the jitter, that error was redrawn every frame, so high-contrast pixels carried a multiplier changing frame to frame for no reason in the scene. Each tap's quotient is now taken while numerator and denominator still belong to the same texel. The hard ratio clamp — itself a discontinuity a wobbling pixel could cross — is now a compressive curve, exact at 1.0.

## New

**Ratio history — the ghosting control.** The enlargement is a temporal reconstructor: it accumulates samples of what it believes is one surface and resolves disagreement between them by smearing. A ratio that changes frame to frame on a static surface is exactly that disagreement, because the model re-decides part of its answer each frame and the alignment offset moves with the jitter.

So the ratio field is averaged along the surface before it ever reaches that pass — reprojected by the game's motion vectors, blended in log space, and clamped into the range the current neighbourhood spans, which throws the history away across disocclusions and moving objects without needing a depth test. What survives the average is the model's real decision about the material; what cancels is the per-frame churn.

Use this rather than lowering Tone strength. Tone is the model's low-frequency verdict on light and colour and a large part of what it is for; lowering it hides ghosting by removing the effect. Default 0.75.

**Tone strength and Detail strength, separately.** The model's edit is two different claims about the picture. Tone is the low-frequency verdict — light interaction, colour, how a surface should sit. Detail is the high-frequency remainder, the material and shading structure it synthesises. **Tone 0 with Detail 1 restores the game's own tone and colour while keeping the neural detail.** Split in log space, because the edit is a ratio and a ratio decomposes into a sum of logarithms — which also makes it exact: at 1 and 1 the bands recombine into precisely the edit that went in, whatever the band radius, so the controls cost nothing until one moves. `Band radius` sets where tone stops and detail begins.

**Hold strength across resolution.** Detail is synthesised at whatever resolution the model ran at, and anything magnifying its work afterwards — a reduced model resolution enlarged back, or a Super Resolution pass taking render to display — spreads that detail over more pixels and thins it. Nothing about the model changed between post-process and an upscaled placement; only how many pixels its work ended up covering. The detail band is now scaled by the ratio of those two resolutions so apparent strength holds. Tone is left alone: it survives magnification largely intact, so lifting it too would overshoot. Bounded at 4×.

**Highlight damping.** Fades the transferred edit out above the white point, where the model was shown an already-clipped picture and where a multiplicative edit amplifies most. A trade rather than a free fix — it costs the model its highlight work — so 0 restores the unfaded behaviour.

**The transfer is additive near black.** A ratio diverges as its denominator does, and the two frames disagree about which pixels are near zero, which is the entire reason there are two of them. Below `TransferLo` the edit is carried as an absolute delta, well defined at zero and also what the model is saying at that brightness; above `TransferHi` it stays a ratio, which keeps a bright surface's shape.

**Input quality is now stated rather than left to be discovered.** The model reads high-frequency texture detail to decide what material a surface is, so its input wants to be as detailed as possible *and* clean — and those pull against each other. Only Multi-pass Rendering satisfies both, because a 1:1 first pass resolves the jitter into antialiased detail at render resolution. **Upscale with DLSS-SR** shows the model the game's raw unresolved buffer, where the high frequencies are aliasing rather than material; **model resolution below full** low-passes away the very evidence the material decision rests on. Neither merely weakens the answer — both change what the model concludes the surface is. The overlay says so where each is selected, and the log says it once.

## Removed

**Feeding the enlargement the resolved frame with zero jitter.** Kept for comparison, and the comparison is settled: the first pass resolves the game's jitter, so that path handed a temporal upscaler one sample position per pixel, identical every frame. There is no title in which it is the better answer. The final pass's jitter now follows whether the transfer actually happened rather than a setting, which is also correct when the edit buffers fail to allocate.

## Unverified

The ratio-history filter, the tone/detail split and the resolution compensation have not been on hardware. The crash and the two guide-buffer defects are reasoned from a log and from the source; the rest is design.
