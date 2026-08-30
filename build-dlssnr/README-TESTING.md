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

## Worth testing first

Test in this order — it isolates each fix, so a failure tells you which one did not hold.

1. **Post-process only**, Neural Rendering enabled. This is the path that crashed on feature
   construction, regardless of what you did afterwards. If it survives, that fix holds.
2. **Switch to Multi-pass + Super Resolution** in the menu. One frame of hitch, then it should run;
   that hitch is the feature rebuilding. Set **First pass** to Super Resolution — this game has no
   Ray Reconstruction, so RR will keep falling back to post-process and logging why.
3. **The colour guard** — still the one with no result at all yet. It does not need an HDR monitor;
   see below.

If step 1 still dies, the log is more use than the crash dump: the last line before the abort says
how far construction got.

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
