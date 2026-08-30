# DLSS 5 Neural Rendering — test build

Personal test build. Extract everything to the game folder, then run `setup_windows.bat` to pick
the proxy DLL name as usual.

Two files sit beside `OptiScaler.dll` and are one character apart in the middle:

| | |
|---|---|
| `nvngx_dlssnr.dll` | NVIDIA's model, ~165 MB. In `OptiScaler/`. |
| `nvngx.dll_dlssnr.dll` | The forwarder, ~110 KB. In the **game folder root**. |

The forwarder exists for one reason: the model's snippet resolves whichever module owns its caller's
return address and refuses any whose path does not contain `nvngx.dll`. Nothing named `OptiScaler`
gets through, so every call to the model is made from that file instead.

**It changed in this build.** A forwarder from an earlier build has no D3D11 or Vulkan exports; the
log says so explicitly rather than reporting a generic failure, but copy it across anyway.

## What is new since the fork

Everything below is built on Dagherbou's `dlssnr-pr`, whose D3D12 path is unchanged.

- **Native D3D11 and Vulkan.** The snippet exports a complete surface for both (verified against the
  shipped DLL), so neither is bridged through D3D12. A D3D11 game already on a native D3D11 upscaler
  never touches D3D12 at all.
- **Placement** — post-process (default, unchanged), Upscale with DLSS-SR, Multi-pass Rendering, and
  Multi-pass Rendering (Custom). Anything but post-process applies to OptiScaler's own upscalers, not
  to a game's native DLSS passing straight through.
- **Highlight colour guard**, in the Colour section. See below.

## Fixed since the last build

**Multi-pass Custom now works.** It held the first pass below render resolution and nothing acted on
it, so the game's full-size buffers went over unchanged while the feature was told they were smaller.
DLSS read the top-left corner of each — a crop, not a reduction — and the frame arrived as a magnified
corner of itself.

The first pass's inputs are now resampled down to match before it runs: colour and motion vectors
filtered, **depth point-sampled**. A bilinear depth tap straddling a silhouette returns a distance
where no surface is, and the upscaler then reprojects against geometry that does not exist. The motion
vector scale and the render subrect follow the reduction, and everything downstream works at the size
that pass actually produced, so the enlargement covers the larger gap.

It is a **performance** control, not a quality one: a cheaper first pass, at the cost of the
enlargement starting from less real detail. At 50% you are asking DLSS to go 634→1920. Judge it on
whether it produces a whole frame, not on how it compares to the others.

**Multi-pass with DLSS should now be sharp and steady.** The softness and the warping had one cause:
the first pass resolves the game's jitter, so handing its output straight to a temporal upscaler
leaves that upscaler nothing to reconstruct from — one sample position per pixel, identical every
frame.

The fix does not hand it that image. The model's edit is *measured* on the resolved frame — how much
brighter or darker it made each pixel — and that ratio is applied to the game's **original jittered
frame**, which still carries its full subpixel sampling. The enlargement then gets a properly jittered
image with the model's work riding on it, plus the real jitter offsets to interpret it by. It behaves
like a normal DLSS upscale that happens to be carrying NR's enhancement.

A brightness ratio, not a per-channel one: the edit is a brightness decision, and applying it per
channel would drag the jittered frame's hue toward the resolved frame's.

The first pass still does its job — the model works on a clean, antialiased frame — and the render-side
jitter is untouched throughout.

- **DLSS enlargement input** — Transfer (default) or the old resolved-frame-with-zero-jitter, for a
  direct A/B. Read per frame, so no restart.
- **Enlargement** — DLSS (default) or the spatial filter. Spatial asks for no jitter and keeps no
  history, so neither failure is available to it, but it is bounded by what the first pass produced
  and cannot reconstruct past it. It is the fallback if the DLSS path misbehaves.

**Enlargement** (Placement section, `MultiPassEnlarge` in the ini) selects between the spatial filter
and the second DLSS pass. Spatial is no sharper — both are limited to what the first pass produced —
but it asks for no jitter and keeps no history, so neither failure is available to it.

If you want DLSS performing the enlargement, **Upscale with DLSS-SR** is the arrangement for that:
one temporal pass, with the game's jitter intact.

**Multi-pass no longer smears the frame when the camera moves.** The second upscaler was created with
`NVSDK_NGX_DLSS_Feature_Flags_MVLowRes` hardcoded on. Borderlands 4 declares `LowResMV: false` — its
motion vectors are at display resolution — so DLSS read a 1920×1080 vector field as though it were
1268×713 and reprojected history by a factor growing with distance from the origin. The first pass had
the game's flags untouched, which is why only the enlargement smeared.

The flag is now forwarded from what the game declares, like every other flag on that feature. The
creation log states the vector resolution, depth convention and jitter the pass was given.

**The broken sun in Upscale with DLSS-SR is fixed.** Post-process works on the upscaler's output
(RGBA16F); the reordered modes work on the game's own scene colour, which here is R11G11B10_FLOAT —
and a sun arrives in it as infinity. The encode computed `rolled / displayLuma` as 1/Inf, which is
zero, then multiplied that back into an infinite channel. `Inf * 0` is NaN, the model was handed the
NaN, and what came back was a block of garbage exactly where the sun was. The codec now bounds
non-finite values on the way in, on all three backends.

**`ResetEveryFrame` — a diagnostic, not a setting.** The model takes motion vectors and a `Reset`,
neither of which means anything to a pass without internal history, so it almost certainly
accumulates. Setting `DlssNr/ResetEveryFrame=true` denies it any history at all. If stability changes,
it had some — which settles by observation what can otherwise only be inferred. Leave it off
normally.

**Upscale with DLSS-SR no longer shows flickering coloured blocks.** Borderlands 4 allocates its
colour buffer at display size and renders into the top-left 1268×713 of it. In post-process the model
works on the finished output, where the whole allocation is valid; in the reordered arrangements it
works on the upscaler's *input*, and the pass was sized from the resource extent — so it encoded
about 1.3 megapixels of memory nobody had written and composed the result onto the frame. The caller
now names the render rect.

**The final Super Resolution pass in a multi-pass chain no longer gets the first pass's jitter.**
After the first pass resolves, its output is grid-aligned, so a per-frame Halton offset describes a
subpixel displacement the image no longer has; reprojecting against it shimmers at the period of the
jitter sequence — the thing the first pass was there to remove.

One code path serves both pipelines: from the final pass's point of view the only upstream difference
is which feature produced the resolved 1:1 image.

- **Final pass jitter** (Placement section, and `MultiPassJitter` in the ini) — zero by default,
  or forward the game's real offsets. Read every frame, so it can be A/B'd without a restart.
- The game's render-side jitter is never touched. The first pass, DLAA or Ray Reconstruction, always
  receives the game's sequence intact along with its create flags and motion vector scales.
- If a game declares `MVJittered`, the setting is ignored and the real offsets are forwarded: DLSS
  cancels the baked offset using those values, and zeroing them leaves a full offset uncancelled
  every frame. The log says when that happened. Borderlands 4 does *not* set it (create flags `0x49`),
  so the dropdown will do something there.

The final pass also gets one `Reset` when newly created, so its first frame is not blended against
whatever its allocation contained.

**The crash on rebuilding a feature is fixed.** "Pure virtual function being called" on the RHI
thread, inside OptiScaler under its proxy name.

`SetInitParameters` runs from `DLSSFeature`'s *constructor*, with `DLSSFeatureDx12` not yet built, so
`Api()` and `GetUpscalerType()` are still pure virtual there. Deciding the arrangement in that
function called both and aborted the process. The flag override and the 1:1 hold now happen in
`NRPrepareForCreate`, called from `ProcessInitParams` with the object complete.

It had been latent since the placements landed — nothing constructed a feature while multi-pass was
selected until the rebuild fix made that happen.

The arrangements are now restricted to DLSS and Ray Reconstruction as well. Only those record a built
mode, so with FSR or XeSS active a configured multi-pass would have asked for a rebuild every frame,
for ever.

**Changing the placement no longer breaks the upscaler.** Multi-pass with Super Resolution as the
first pass failed every frame with `FAIL_InvalidParameter`. The target resolution is latched when the
feature is created and nothing rebuilt it — the game's resolutions had not moved, which is the only
thing engines watch. The pipeline meanwhile read config live, so it routed the upscaler's output into
a render-resolution buffer while its feature was still built to write display resolution.

A placement change now rebuilds the feature, and the pipeline branches on what the feature was
**built** for rather than on what is configured, so the two cannot disagree. Expect one frame of
hitch when you switch placement; that is the rebuild, not an error.

Ray Reconstruction only appeared to work before: this game has no RR, so it fell back to post-process
and never built the stage at all.

The reordered placements are now also gated to D3D12, where they are implemented. On D3D11 and Vulkan
they fall back to post-process rather than clearing IsHDR on a feature nothing runs ahead of.

## What the model itself consumes

A property of `nvngx_dlssnr.dll`, so one answer covers every title. Taken from the full set of
parameters the forwarder writes to it:

| | |
|---|---|
| Jitter offsets | **Not consumed.** There is no jitter key in its parameter surface at all. |
| Motion vectors | Consumed — `MVec`, `MVecScaleX/Y`, and a full subrect. |
| `Reset` | Present. |

Motion vectors plus a Reset only mean something to a pass that keeps history: a stateless filter has
nothing to reproject and nothing to reset. So the model is almost certainly a temporal accumulator.
That is inference from the parameter surface, not proof — the decisive test is forcing `Reset` every
frame and seeing whether stability degrades.

It matters most on the RT path. Ray Reconstruction is already a temporal accumulator doing denoising
history on top of upscaling, so RR → NR → SR puts three independent history-rejection stages in
series. On a single disocclusion those rejections compound, and the third stage cannot distinguish a
frame the second already invalidated from real motion. That is a larger effect than the jitter
parameter ever was.

## Worth testing first

Test in this order — it isolates each change, so a failure tells you which one did not hold.

1. **Multi-pass with Enlargement = Spatial** (the new default), at your usual upscale ratio. The
   warping should be gone. It will still be softer than post-process, and that part is real: the
   first pass produced render-resolution detail and no filter can invent past it.
2. **Flip Enlargement to DLSS Super Resolution** for the direct comparison.
3. **`ResetEveryFrame=true`, against post-process specifically.** This chases the baseline ghosting
   that post-process and 1:1 multi-pass share — if it changes, the model keeps history. Diagnostic
   only; turn it back off.
4. **The colour guard** — still no result on it at all. Colour strength **1.00**, guard **1.00**,
   then guard **0.00** for the A/B. No HDR monitor needed; see below.
5. **The sun**, in Upscale with DLSS-SR. Should be a sun rather than a coloured block.

If something crashes rather than looks wrong, the log is more use than the crash dump: the last line
before the abort says how far it got.



**The colour guard.** Set Colour strength back to **1.00** and the guard to **1.00**.

**An HDR monitor is not needed.** The guard keys off the game's own DLSS create flag `IsHDR`, which
describes the *frame buffer* — linear and open-ended — not the display. Plenty of games render that
way and tone-map to SDR at the very end. Borderlands 4 is one of them; the log says so:

```
DLSS-NR: the game's DLSS buffer is linear HDR (create flags 0x49), so the colour transform is on
```

`0x49` is AutoExposure + DepthInverted + IsHDR. The encode is running, so the guard is live and
testable on an SDR screen. What genuinely cannot be tested without an HDR display is how the result
*looks* at high nits — but whether a saturated highlight comes back coloured instead of white is
visible either way.

Look for any small, very bright, strongly coloured thing: neon signage, a muzzle flash, an emissive
panel, a coloured light source. Those are what leave the model's range and arrive white.

Guard at **0.00** is exactly the old behaviour, so it is a clean A/B.

If it still looks white with the guard on, switch Debug view to **the model's answer**. White there
means the diagnosis holds and the guard's threshold needs widening; coloured there means the
whitening happens later and the wrong stage has been blamed.

**Multi-pass Custom.** The scale slider does nothing until **Apply Scale** is pressed — deliberately.
It rebuilds both features, and acting on every value a drag passes through exhausts the driver's
create-time latches, after which the model stops responding until the game is restarted. The button
greys out when nothing has changed, and the applied value is shown while it differs.

Set **First pass** to match the upscaler actually running. OptiScaler cannot substitute Ray
Reconstruction for Super Resolution — RR needs G-buffer inputs an SR integration never supplies — so
a mismatch falls back to post-process and says so in the log.

## Known-unverified

None of the D3D11, Vulkan, placement or colour-guard code has run on hardware. It builds, and it is
ported from a D3D12 path that works, but that is a different claim from working.

The RDNA4 track is cancelled and carries no code here. Its source is preserved on the `ours-dlssnr`
branch.
