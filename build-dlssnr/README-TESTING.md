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

## Worth testing first

**The colour guard, in HDR.** Set Colour strength back to **1.00** and the guard to **1.00**.

Jedi Survivor's droid is the clean test: its backpack lights should be neon green, and were arriving
white. The guard should bring the green back *while the rest of the frame keeps the model's colour
work* — which is what turning Colour strength to 0 gave up.

Guard at **0.00** is exactly the old behaviour, so it is a clean A/B.

If the lights are still white with the guard on, switch Debug view to **the model's answer**. White
there means the diagnosis holds and the guard's threshold needs widening; green there means the
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
