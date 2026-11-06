// Windows/Proton integration probe for Hover Junkers' OpenVR ABI (1.0.3).
// Compile with the Valve openvr-1.0.3.h include directory, link d3d11/dxgi.
// Put the game's openvr_api.dll beside the resulting executable. Never changes a game/prefix.
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <openvr-1.0.3.h>
#include <cstdio>
#include <cstdlib>

int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    HMODULE library = LoadLibraryA("openvr_api.dll");
    if (!library) { printf("FAIL LoadLibrary %lu\n", GetLastError()); return 1; }
    auto init = reinterpret_cast<uint32_t(*)(vr::EVRInitError*, vr::EVRApplicationType)>(GetProcAddress(library, "VR_InitInternal"));
    auto get = reinterpret_cast<void*(*)(const char*, vr::EVRInitError*)>(GetProcAddress(library, "VR_GetGenericInterface"));
    auto shutdown = reinterpret_cast<void(*)()>(GetProcAddress(library, "VR_ShutdownInternal"));
    if (!init || !get || !shutdown) return 1;
    vr::EVRInitError error{};
    init(&error, vr::VRApplication_Scene);
    if (error) { printf("FAIL VR_Init %d\n", error); return 1; }
    auto * system = static_cast<vr::IVRSystem*>(get(vr::IVRSystem_Version, &error));
    auto * compositor = static_cast<vr::IVRCompositor*>(get(vr::IVRCompositor_Version, &error));
    if (!system || !compositor) { printf("FAIL legacy interfaces %d\n", error); return 1; }
    int32_t adapter_index = 0; system->GetDXGIOutputInfo(&adapter_index);
    IDXGIFactory1 *factory = nullptr; IDXGIAdapter *adapter = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) ||
        FAILED(factory->EnumAdapters(adapter_index, &adapter))) return 1;
    ID3D11Device *device = nullptr; ID3D11DeviceContext *context = nullptr;
    if (FAILED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
                                D3D11_SDK_VERSION, &device, nullptr, &context))) return 1;
    D3D11_TEXTURE2D_DESC desc{}; desc.Width = desc.Height = 256; desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D *image = nullptr; ID3D11RenderTargetView *rtv = nullptr;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &image)) ||
        FAILED(device->CreateRenderTargetView(image, nullptr, &rtv))) return 1;
    const float green[]{0.1f, 0.7f, 0.3f, 1.f}; context->ClearRenderTargetView(rtv, green); context->Flush();
    vr::Texture_t texture{image, vr::API_DirectX, vr::ColorSpace_Gamma};
    ULONGLONG end = GetTickCount64() + 1000 * (argc > 1 ? atoi(argv[1]) : 12);
    int frames = 0; vr::TrackedDevicePose_t poses[16]{};
    while (GetTickCount64() < end) {
        auto result = compositor->WaitGetPoses(poses, 16, nullptr, 0);
        if (result) { printf("FAIL WaitGetPoses %d\n", result); return 1; }
        for (int eye = 0; eye < 2; ++eye) {
            result = compositor->Submit(vr::EVREye(eye), &texture);
            if (result) { printf("FAIL Submit %d\n", result); return 1; }
        }
        compositor->PostPresentHandoff(); ++frames;
    }
    shutdown(); rtv->Release(); image->Release(); context->Release(); device->Release(); adapter->Release(); factory->Release();
    printf("%s legacy OpenVR D3D11 rendered %d stereo frames\n", frames > 20 ? "ALL PASS" : "FAIL", frames);
    return frames > 20 ? 0 : 1;
}
