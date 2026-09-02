DLSS 5 Neural Rendering for OptiScaler — a detail-synthesis pass that runs on the upscaler's output, before the interface is drawn.

The D3D12 core is **Dagherbou's**, from [OptiScaler_DLSSNR](https://github.com/Dagherbou/OptiScaler_DLSSNR) (`dlssnr-pr`), and it is unchanged here. Everything else is built on top of it.

This build was worked against NVIDIA's **DLSS Programming Guide** (310.6.0) and the **DLSS-RR Integration Guide** (December 2025). Where something below cites a section, the citation is real and the behaviour was changed to match it.

## Install

Extract into the game folder, run `setup_windows.bat`, then **supply `nvngx_dlssnr.dll` yourself** — NVIDIA's model, ~165 MB, from a driver package. It is not ours to redistribute.

Two files, one character apart:

| | |
|---|---|
| `nvngx_dlssnr.dll` | NVIDIA's model. **You supply this.** |
| `nvngx.dll_dlssnr.dll` | The forwarder, 110 KB. Included. **Game folder root**, beside `OptiScaler.dll`. |

## Fixed

**The game's scene-transition reset never left the first pass.** A game raises `InReset` on a camera cut or a level load precisely so that every temporal accumulator in the chain drops its history at that instant. It was read by the first pass and by nothing else — so on a cut, the model and the enlargement both kept blending a scene that no longer existed, and the frame smeared straight through the transition. Both receive it now. A partial reset is worse than none: the stages then disagree about which frame they are on.

**Multi-pass Custom handed its reduced first pass the game's unscaled jitter.** Section 3.7.3(2) puts jitter offsets in pixel space at the render target size and explicitly rules out supplying them at any other — unlike motion vectors, which may optionally be given at display resolution. That pass rasterises at the reduced size, so the game's offsets overstated the real displacement by the inverse of the reduction: at a 66% hold, a half-pixel offset was declared as 0.76 of a pixel, outside the ±0.5 that section 3.7.3(1) says a jitter value can even take. They now scale with the buffers, and anything still out of range is clamped and reported rather than passed on quietly.

Section 3.7.4 is the list of what that looks like, and it is worth writing down because it is easy to blame on the reduction itself: screen shaking, distant objects not resolving, a screen-door pattern over the output, static thin features and fine texture detail going fuzzy.

**The transfer no longer divides by a number that goes to zero.** A ratio diverges as its denominator does, and the two frames disagree about which pixels are near zero — which is the entire reason there are two of them. A pixel the first pass resolved to almost nothing produced an enormous ratio, and that ratio then multiplied a *different* almost-nothing in the jittered frame. The old floor at 1e-4 did not fix that; it moved where the blow-up started. Below `TransferLo` the edit is now carried as an absolute delta, which is well defined at zero and is also what the model is saying at that brightness; above `TransferHi` it stays a ratio, which is what keeps a bright surface's shape instead of flattening it.

**The second Super Resolution pass was on no preset at all.** That is not the same as the driver's default — the driver then picks one from that pass's own ratio, independently of the first pass, and the two can land on presets that disagree about the frame. Section 3.9 attaches behaviour to the choice and not only quality: exposure input is supported by Presets J and K alone, and Preset L *always* uses AutoExposure. This pass deliberately clears AutoExposure and binds an identity exposure texture, so a driver landing it on L would have ignored that texture and auto-exposed an already-normalised picture. It now takes the first pass's preset (`MatchPreset`, on by default).

## New

**The edit alignment is derived rather than guessed.** Section 3.7.3(4) states that jitter offsets use the same coordinate and direction system as motion vectors, with (0, 0) meaning no jitter — which makes the direction a consequence of the convention the game already declares, not a coin flip between three values. The offset displaces the rasterised scene, so content sitting at pixel *p* in the jittered frame sits at *p* minus the offset in the resolved one, along whichever axis directions the motion vector scales imply.

This matters more than a sign usually would. The wrong one *doubles* the misalignment rather than removing it, and the enlargement's temporal accumulation then averages the misplaced edit across jitter offsets — which cancels the model's work rather than merely blurring it. If 1.5's multi-pass transfer read as "NR is barely doing anything", this is the first thing to re-check.

**Cost by stage.** Encode, downsample, inference, resolve, transfer and enlarge each carry their own timestamp pair, and the overlay shows the split with each stage's share of the measured total. One number said the pass was expensive and nothing about why, and the candidate answers point at completely different work: if inference dominates, running the model smaller or less often pays; if the codec's own dispatches or the enlargement do, both are effort spent in the wrong place. Neural Rendering, Super Resolution and frame generation also compete for the same tensor units, so on a power-limited part some of this is contention rather than work.

**Jitter is watched rather than assumed.** No game states its phase count, so the offsets each pass is given are observed until the sequence is seen to repeat, then checked against section 3.7.1.1's own rule — 8 × the pixel area ratio, which reproduces the guide's table exactly (DLAA 8, Quality 18, Balanced 24, Performance 32, Ultra Performance 72), with Ray Reconstruction's floor of 32 on top. The warning waits for the count to settle, because until the pattern cycles a low number means "early", not "short".

**One line in the log saying what the frame is built out of** — topology, per-pass input and output resolution, preset, HDR, exposure, jitter and phase count. Latched on the structure, so a placement change re-emits it and a steady session costs one line. Grep for `DLSS-NR integration:`. Every question asked about this feature so far has been answerable from data the code already had at the time, and asking for it one round trip at a time is how a five-minute diagnosis becomes a week.

**`TransferBlur`** — a low-pass radius on the edit before it is carried across. A brightness ratio measured on one image and applied to another that sampled the scene half a pixel elsewhere can carry tone faithfully and structure not at all, and a misplaced structure band is worse than a missing one. Raising this trades the structure band for a clean tone transfer. Worth trying first on the Ray Reconstruction pipeline, where that same band is the part that fights the denoise. At 0 the taps collapse onto the centre, so it is bit-for-bit what 1.5 did.

**`MinRatio`** separates the transfer's floor from `MaxRatio`'s ceiling. The two ends fail differently.

## Two things the plan for this build got wrong

Said plainly, because they were checked against the guides rather than assumed.

**The phase count is not a flat 16.** Section 3.7.1.1 scales it with the pixel area ratio, and the guide's own table falls out of the formula. The implementation uses the formula, as an area product so anisotropic scaling does not break it.

**Ray Reconstruction's DLAA preset needs no gate in Multi-pass Custom.** The concern was that a pass held below render resolution cannot use it, since the guide notes it works only when input and output render sizes are equal. But that pass *is* 1:1 — it is held below the game's render resolution and its inputs are resampled down to match before it runs, so it goes from its own size to its own size. It does not upscale, and no gate is needed.

## Known limits

- **Native D3D11 does not work.** The snippet's own D3D11 init returns `FAIL_FeatureNotSupported` — it declining the API. On a D3D11 game, set `Dx11Upscaler` to a `_12` option and the D3D11-on-D3D12 bridge carries the pass.
- **NVIDIA RTX only.** The model is an NGX snippet.
- **The game must already have an upscaler.** OptiScaler works by intercepting DLSS, FSR2/3 or XeSS inputs the game already provides. A title with none of them has nothing to hook, and installing this there achieves nothing.
- Anything but post-process needs OptiScaler's own upscaler to be DLSS or Ray Reconstruction; otherwise it falls back and logs why.

## Off-spec, deliberately

- **NR runs after tone mapping in every placement.** Both guides require the DLSS evaluation to occur during post-processing *before* tone mapping. The multi-pass second SR pass runs after NR, which runs after the codec's display-referred work. Quality regressions should be attributed here first.
- **Ray Reconstruction and Super Resolution both execute in one frame** on the multi-pass RR topology. The RR guide states that RR overrides SR execution — an RR feature and a separate SR feature in the same frame is not a configuration NVIDIA describes.
- **Neural Rendering is undocumented** and driven directly through its snippet.
- **The forwarder defeats the model's caller-path check.**

## Unverified

This tranche has had **no time on hardware at all**. It builds — D3D12 and the Vulkan SPIR-V both — and every change above is traceable to a cited requirement or to a defect visible in the source, but that is a different claim from working.

The two worth looking at first: **Edit alignment**, which now defaults to Derived — if that reads *stronger* than Off, the derivation is right and 1.5's `+1` default was pointed the wrong way. And **Cost by stage**, which decides whether the resolution and amortisation levers planned for the next tranche are worth building at all.

## Still to come in 2.0

The ratio-field and jitter visualisers, guided ratio upsampling, temporal ratio amortisation, the Ray Reconstruction guide-buffer inventory, the OkLab-decomposed transfer, and pre-roll-off encoding. This release is the first tranche: measurement, and the correctness bugs affecting what already shipped.

## Attribution

The colour composition — the two-branch luminance ratio, the OkLab hue correction, and the blend between a luminance-only result and the model's own colour — is taken from **RenoDX's DLSS 5 addon by clshortfuse**. It is their design. See `Licenses/RenoDX_ATTRIBUTION.txt`.

The D3D12 module, the forwarder and the colour codec are **Dagherbou's**.

Neural Rendering is undocumented and driven directly through its snippet. None of it is supported by NVIDIA.
