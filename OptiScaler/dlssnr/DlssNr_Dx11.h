#pragma once

#include <d3d11.h>
#include <nvsdk_ngx.h>

#include <optional>

// DLSS 5 Neural Rendering on native Direct3D 11.
//
// The same pass as the D3D12 module, on a D3D11 game's own device. OptiScaler can already bridge a
// D3D11 game onto D3D12 and reach the model there, but that route exists to give D3D11 games a D3D12
// upscaler and costs a shared-resource round trip per frame; the snippet exports a complete D3D11
// surface, so a game already running a native D3D11 upscaler need not involve D3D12 at all.
//
// Separate state from the D3D12 path on purpose. A process uses one or the other, and sharing the
// feature handle across two devices is not a thing the model supports.
namespace DlssNr
{
// Runs the model over Output on the immediate context, straight after the upscaler has written it.
// Safe to call every frame; it builds what it needs on first use and disables itself for the session
// if anything fails, rather than retrying into a crash.
void EvaluateAfterUpscaleDx11(ID3D11DeviceContext* ctx, NVSDK_NGX_Parameter* params);

bool IsRunningDx11();
const char* FailureReasonDx11();
std::optional<double> LastGpuTimeDx11();

// Clears the session failure latch, so a failure caused by transient thrash does not cost a restart.
void RetryAfterFailureDx11();

void ShutdownDx11();
} // namespace DlssNr
