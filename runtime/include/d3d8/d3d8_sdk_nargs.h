// Direct3D 8 SDK stdcall argument counts (excluding `this`) for HLE regression checks.
#pragma once

namespace d3d8::sdk_nargs {

struct Method {
  const char* name;
  int nargs;
};

inline constexpr Method kIDirect3D8[] = {
    {"QueryInterface", 2}, {"AddRef", 0}, {"Release", 0}, {"RegisterSoftwareDevice", 1}, {"GetAdapterCount", 0},
    {"GetAdapterIdentifier", 3}, {"GetAdapterModeCount", 2}, {"EnumAdapterModes", 4}, {"GetAdapterDisplayMode", 2},
    {"CheckDeviceType", 5}, {"CheckDeviceFormat", 6}, {"CheckDeviceMultiSampleType", 6}, {"CheckDepthStencilMatch", 5},
    {"GetDeviceCaps", 3}, {"GetAdapterMonitor", 1}, {"CreateDevice", 6},
};

inline constexpr Method kIDirect3DDevice8[] = {
    {"QueryInterface", 2},
    {"AddRef", 0},
    {"Release", 0},
    {"TestCooperativeLevel", 0},
    {"GetAvailableTextureMem", 0},
    {"ResourceManagerDiscardBytes", 1},
    {"GetDirect3D", 1},
    {"GetDeviceCaps", 1},
    {"GetDisplayMode", 1},
    {"GetCreationParameters", 1},
    {"SetCursorProperties", 3},
    {"SetCursorPosition", 3},
    {"ShowCursor", 1},
    {"CreateAdditionalSwapChain", 2},
    {"Reset", 1},
    {"Present", 4},
    {"GetBackBuffer", 3},
    {"GetRasterStatus", 1},
    {"SetGammaRamp", 2},
    {"GetGammaRamp", 1},
    {"CreateTexture", 7},
    {"CreateVolumeTexture", 8},
    {"CreateCubeTexture", 6},
    {"CreateVertexBuffer", 5},
    {"CreateIndexBuffer", 5},
    {"CreateRenderTarget", 6},
    {"CreateDepthStencilSurface", 5},
    {"CreateImageSurface", 4},
    {"CopyRects", 5},
    {"UpdateTexture", 2},
    {"GetFrontBuffer", 1},
    {"SetRenderTarget", 2},
    {"GetRenderTarget", 1},
    {"GetDepthStencilSurface", 1},
    {"BeginScene", 0},
    {"EndScene", 0},
    {"Clear", 6},
    {"SetTransform", 2},
    {"GetTransform", 2},
    {"MultiplyTransform", 2},
    {"SetViewport", 1},
    {"GetViewport", 1},
    {"SetMaterial", 1},
    {"GetMaterial", 1},
    {"SetLight", 2},
    {"GetLight", 2},
    {"LightEnable", 2},
    {"GetLightEnable", 2},
    {"SetClipPlane", 2},
    {"GetClipPlane", 2},
    {"SetRenderState", 2},
    {"GetRenderState", 2},
    {"BeginStateBlock", 0},
    {"EndStateBlock", 1},
    {"ApplyStateBlock", 1},
    {"CaptureStateBlock", 1},
    {"DeleteStateBlock", 1},
    {"CreateStateBlock", 2},
    {"SetClipStatus", 1},
    {"GetClipStatus", 1},
    {"GetTexture", 2},
    {"SetTexture", 2},
    {"GetTextureStageState", 3},
    {"SetTextureStageState", 3},
    {"ValidateDevice", 1},
    {"GetInfo", 3},
    {"SetPaletteEntries", 2},
    {"GetPaletteEntries", 2},
    {"SetCurrentTexturePalette", 1},
    {"GetCurrentTexturePalette", 1},
    {"DrawPrimitive", 3},
    {"DrawIndexedPrimitive", 5},
    {"DrawPrimitiveUP", 4},
    {"DrawIndexedPrimitiveUP", 8},
    {"ProcessVertices", 5},
    {"CreateVertexShader", 4},
    {"SetVertexShader", 1},
    {"GetVertexShader", 1},
    {"DeleteVertexShader", 1},
    {"SetVertexShaderConstant", 3},
    {"GetVertexShaderConstant", 3},
    {"GetVertexShaderFunction", 3},
    {"SetStreamSource", 3},
    {"GetStreamSource", 3},
    {"SetIndices", 2},
    {"GetIndices", 2},
    {"CreatePixelShader", 2},
    {"SetPixelShader", 1},
    {"GetPixelShader", 1},
    {"DeletePixelShader", 1},
    {"SetPixelShaderConstant", 3},
    {"GetPixelShaderConstant", 3},
    {"GetPixelShaderFunction", 3},
    {"DrawRectPatch", 3},
    {"DrawTriPatch", 3},
    {"DeletePatch", 1},
};

inline constexpr Method kIDirect3DSurface8[] = {
    {"QueryInterface", 2}, {"AddRef", 0}, {"Release", 0}, {"GetDevice", 1}, {"SetPrivateData", 3},
    {"GetPrivateData", 3}, {"FreePrivateData", 1}, {"GetContainer", 2}, {"GetDesc", 1}, {"LockRect", 3},
    {"UnlockRect", 1},
};

inline constexpr Method kIDirect3DTexture8[] = {
    {"QueryInterface", 2}, {"AddRef", 0}, {"Release", 0}, {"GetDevice", 1}, {"SetPrivateData", 3},
    {"GetPrivateData", 3}, {"FreePrivateData", 1}, {"SetPriority", 1}, {"GetPriority", 0}, {"PreLoad", 0},
    {"GetType", 0}, {"SetLOD", 1}, {"GetLOD", 0}, {"GetLevelCount", 0}, {"GetLevelDesc", 2},
    {"GetSurfaceLevel", 2}, {"LockRect", 4}, {"UnlockRect", 1}, {"AddDirtyRect", 1},
};

inline constexpr Method kIDirect3DVertexBuffer8[] = {
    {"QueryInterface", 2}, {"AddRef", 0}, {"Release", 0}, {"GetDevice", 1}, {"SetPrivateData", 3},
    {"GetPrivateData", 3}, {"FreePrivateData", 1}, {"SetPriority", 1}, {"GetPriority", 0}, {"PreLoad", 0},
    {"GetType", 0}, {"Lock", 4}, {"Unlock", 0}, {"GetDesc", 1},
};

inline constexpr Method kIDirect3DIndexBuffer8[] = {
    {"QueryInterface", 2}, {"AddRef", 0}, {"Release", 0}, {"GetDevice", 1}, {"SetPrivateData", 3},
    {"GetPrivateData", 3}, {"FreePrivateData", 1}, {"SetPriority", 1}, {"GetPriority", 0}, {"PreLoad", 0},
    {"GetType", 0}, {"Lock", 4}, {"Unlock", 0}, {"GetDesc", 1},
};

inline constexpr Method kIDirect3DStateBlock8[] = {
    {"QueryInterface", 2}, {"AddRef", 0}, {"Release", 0}, {"GetDevice", 1}, {"Apply", 0}, {"Capture", 0},
};

}  // namespace d3d8::sdk_nargs
