#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <dwmapi.h>
#include <bcrypt.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wincodec.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>
#include "core.h"
#pragma warning(push)
#pragma warning(disable: 4100)
#include "vendor/openvr.h"
#pragma warning(pop)

namespace fs = std::filesystem;
using namespace winrt;
using namespace winrt::Windows::Graphics;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

static fs::path folder(REFKNOWNFOLDERID id) {
    PWSTR text = nullptr;
    check_hresult(SHGetKnownFolderPath(id, 0, nullptr, &text));
    fs::path result(text);
    CoTaskMemFree(text);
    return result;
}

static std::string sha256(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot read installed Pimax calibration files.");
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("Cannot initialize SHA-256.");
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        throw std::runtime_error("Cannot create SHA-256 hash.");
    }
    std::array<unsigned char, 65536> buffer{};
    while (file.read(reinterpret_cast<char*>(buffer.data()), buffer.size()) || file.gcount()) {
        if (BCryptHashData(hash, buffer.data(), static_cast<ULONG>(file.gcount()), 0) < 0) {
            BCryptDestroyHash(hash);
            BCryptCloseAlgorithmProvider(algorithm, 0);
            throw std::runtime_error("Cannot hash Pimax calibration files.");
        }
    }
    std::array<unsigned char, 32> digest{};
    const auto status = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0 || file.bad()) throw std::runtime_error("Pimax file verification failed.");
    std::ostringstream result;
    for (const auto byte : digest) result << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    return result.str();
}

template<class T> static T procedure(HMODULE module, const char* name) {
    FARPROC address = GetProcAddress(module, name);
    if (!address) throw std::runtime_error(std::string("SteamVR loader export missing: ") + name);
    T result;
    static_assert(sizeof(result) == sizeof(address));
    std::memcpy(&result, &address, sizeof(result));
    return result;
}

class SteamVR {
    HMODULE loader_ = nullptr;
    decltype(&vr::VR_ShutdownInternal) shutdown_ = nullptr;
    bool initialized_ = false;
public:
    vr::IVRSystem* system = nullptr;
    vr::IVROverlay* overlays = nullptr;
    vr::VROverlayHandle_t handle = vr::k_ulOverlayHandleInvalid;

    void open() {
        wchar_t steamPath[32768]{};
        DWORD bytes = sizeof(steamPath);
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ,
                         nullptr, steamPath, &bytes) != ERROR_SUCCESS)
            throw std::runtime_error("Steam installation not found. Install and start SteamVR first.");
        const fs::path dll = fs::path(steamPath) / L"steamapps/common/SteamVR/bin/win64/openvr_api.dll";
        loader_ = LoadLibraryExW(dll.c_str(), nullptr,
                                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!loader_) throw std::runtime_error("SteamVR loader not found in Steam's default library.");
        const auto init = procedure<decltype(&vr::VR_InitInternal2)>(loader_, "VR_InitInternal2");
        shutdown_ = procedure<decltype(&vr::VR_ShutdownInternal)>(loader_, "VR_ShutdownInternal");
        const auto get = procedure<decltype(&vr::VR_GetGenericInterface)>(loader_, "VR_GetGenericInterface");
        vr::EVRInitError error = vr::VRInitError_None;
        init(&error, vr::VRApplication_Overlay, nullptr);
        if (error != vr::VRInitError_None)
            throw std::runtime_error("SteamVR initialization failed (" + std::to_string(error) + "). Start SteamVR and connect the headset.");
        initialized_ = true;
        system = static_cast<vr::IVRSystem*>(get(vr::IVRSystem_Version, &error));
        if (!system || error != vr::VRInitError_None)
            throw std::runtime_error("SteamVR system interface unavailable. Update SteamVR.");
        overlays = static_cast<vr::IVROverlay*>(get(vr::IVROverlay_Version, &error));
        if (!overlays || error != vr::VRInitError_None)
            throw std::runtime_error("SteamVR overlay interface unavailable. Update SteamVR.");
    }

    void check(vr::EVROverlayError error, const char* operation) const {
        if (error != vr::VROverlayError_None)
            throw std::runtime_error(std::string(operation) + ": " + overlays->GetOverlayErrorNameFromEnum(error));
    }

    void create() {
        check(overlays->CreateOverlay("org.projectforge.steamvr-eye-calibrator", "Pimax Eye Calibration", &handle), "Create overlay");
        check(overlays->SetOverlaySortOrder(handle, 100), "Set overlay order");
        check(overlays->SetOverlayInputMethod(handle, vr::VROverlayInputMethod_None), "Set overlay input");
    }

    void position(int width, int height) {
        const auto geometry = calibrator::geometry(width, height);
        vr::HmdMatrix34_t transform{};
        transform.m[0][0] = transform.m[1][1] = transform.m[2][2] = 1;
        // The vendor canvas is attached to PVRCamera, not to one eye.
        transform.m[2][3] = -static_cast<float>(geometry.distanceMeters);
        check(overlays->SetOverlayWidthInMeters(handle, static_cast<float>(geometry.widthMeters)), "Set target geometry");
        check(overlays->SetOverlayTransformTrackedDeviceRelative(handle, vr::k_unTrackedDeviceIndex_Hmd,
                                                                &transform), "Attach overlay to headset");
    }

    ~SteamVR() {
        if (overlays && handle != vr::k_ulOverlayHandleInvalid) {
            overlays->HideOverlay(handle);
            overlays->ClearOverlayTexture(handle);
            overlays->DestroyOverlay(handle);
        }
        if (initialized_ && shutdown_) shutdown_();
        if (loader_) FreeLibrary(loader_);
    }
};

class WindowCapture {
    com_ptr<ID3D11Device> device_;
    com_ptr<ID3D11DeviceContext> context_;
    IDirect3DDevice winrtDevice_{nullptr};
    GraphicsCaptureItem item_{nullptr};
    Direct3D11CaptureFramePool pool_{nullptr};
    GraphicsCaptureSession session_{nullptr};
    com_ptr<ID3D11Texture2D> shared_;
    SizeInt32 size_{};
public:
    unsigned frames = 0;
    int width = 0, height = 0;

    void start(HWND window, vr::IVRSystem* system) {
        if (!GraphicsCaptureSession::IsSupported())
            throw std::runtime_error("Windows Graphics Capture is unavailable. Windows 10 2004 or newer is required.");
        int32_t adapterIndex = -1;
        system->GetDXGIOutputInfo(&adapterIndex);
        com_ptr<IDXGIFactory1> factory;
        check_hresult(CreateDXGIFactory1(__uuidof(IDXGIFactory1), factory.put_void()));
        com_ptr<IDXGIAdapter1> adapter;
        if (adapterIndex >= 0) check_hresult(factory->EnumAdapters1(static_cast<UINT>(adapterIndex), adapter.put()));
        check_hresult(D3D11CreateDevice(adapter.get(), adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
            nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            device_.put(), nullptr, context_.put()));
        const auto dxgiDevice = device_.as<IDXGIDevice>();
        com_ptr<IInspectable> inspectable;
        check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), inspectable.put()));
        winrtDevice_ = inspectable.as<IDirect3DDevice>();
        const auto interop = get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        check_hresult(interop->CreateForWindow(window, guid_of<GraphicsCaptureItem>(), put_abi(item_)));
        size_ = item_.Size();
        pool_ = Direct3D11CaptureFramePool::CreateFreeThreaded(winrtDevice_,
            DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size_);
        session_ = pool_.CreateCaptureSession(item_);
        session_.IsCursorCaptureEnabled(false);
        session_.StartCapture();
    }

    bool update(HWND window, SteamVR& vr) {
        auto frame = pool_.TryGetNextFrame();
        if (!frame) return false;
        const auto content = frame.ContentSize();
        if (content.Width <= 0 || content.Height <= 0) return false;
        if (content.Width != size_.Width || content.Height != size_.Height) {
            frame.Close();
            size_ = content;
            pool_.Recreate(winrtDevice_, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size_);
            return false;
        }
        const auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        com_ptr<ID3D11Texture2D> surface;
        check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D), surface.put_void()));
        D3D11_TEXTURE2D_DESC description{};
        surface->GetDesc(&description);
        if (!shared_ || width != static_cast<int>(description.Width) || height != static_cast<int>(description.Height)) {
            if (shared_) vr.check(vr.overlays->ClearOverlayTexture(vr.handle), "Release resized texture");
            shared_ = nullptr;
            description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            description.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
            description.Usage = D3D11_USAGE_DEFAULT;
            description.CPUAccessFlags = 0;
            check_hresult(device_->CreateTexture2D(&description, nullptr, shared_.put()));
            width = static_cast<int>(description.Width);
            height = static_cast<int>(description.Height);
        }
        RECT client{}, bounds{};
        if (!GetClientRect(window, &client)) throw std::runtime_error("Calibrator window closed.");
        POINT origin{};
        if (!ClientToScreen(window, &origin)) throw std::runtime_error("Calibrator window closed.");
        check_hresult(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds)));
        const auto crop = calibrator::cropBounds(content.Width, content.Height,
            origin.x - bounds.left, origin.y - bounds.top, client.right, client.bottom);
        const vr::VRTextureBounds_t textureBounds{crop.uMin, crop.vMin, crop.uMax, crop.vMax};
        vr.check(vr.overlays->SetOverlayTextureBounds(vr.handle, &textureBounds), "Crop window chrome");
        vr.position(client.right, client.bottom);
        context_->CopyResource(shared_.get(), surface.get());
        context_->Flush();
        const vr::Texture_t texture{shared_.get(), vr::TextureType_DirectX, vr::ColorSpace_Auto};
        vr.check(vr.overlays->SetOverlayTexture(vr.handle, &texture), "Submit calibration frame");
        vr.check(vr.overlays->ShowOverlay(vr.handle), "Show calibration in SteamVR");
        ++frames;
        return true;
    }

    void save(const fs::path& path) {
        if (!shared_) throw std::runtime_error("No calibration frame captured.");
        D3D11_TEXTURE2D_DESC desc{};
        shared_->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = desc.MiscFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        com_ptr<ID3D11Texture2D> staging;
        check_hresult(device_->CreateTexture2D(&desc, nullptr, staging.put()));
        context_->CopyResource(staging.get(), shared_.get());
        D3D11_MAPPED_SUBRESOURCE pixels{};
        check_hresult(context_->Map(staging.get(), 0, D3D11_MAP_READ, 0, &pixels));
        try {
            const auto factory = create_instance<IWICImagingFactory>(CLSID_WICImagingFactory);
            com_ptr<IWICStream> stream;
            check_hresult(factory->CreateStream(stream.put()));
            check_hresult(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE));
            com_ptr<IWICBitmapEncoder> encoder;
            check_hresult(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put()));
            check_hresult(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache));
            com_ptr<IWICBitmapFrameEncode> frame;
            check_hresult(encoder->CreateNewFrame(frame.put(), nullptr));
            check_hresult(frame->Initialize(nullptr));
            check_hresult(frame->SetSize(desc.Width, desc.Height));
            WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
            check_hresult(frame->SetPixelFormat(&format));
            check_hresult(frame->WritePixels(desc.Height, pixels.RowPitch, pixels.RowPitch * desc.Height,
                                             static_cast<BYTE*>(pixels.pData)));
            check_hresult(frame->Commit());
            check_hresult(encoder->Commit());
        } catch (...) { context_->Unmap(staging.get(), 0); throw; }
        context_->Unmap(staging.get(), 0);
    }

    ~WindowCapture() {
        if (session_) session_.Close();
        if (pool_) pool_.Close();
    }
};

struct WindowSearch { fs::path executable; HWND window = nullptr; DWORD pid = 0; };
static BOOL CALLBACK findWindow(HWND window, LPARAM parameter) {
    auto& search = *reinterpret_cast<WindowSearch*>(parameter);
    wchar_t className[128]{};
    GetClassNameW(window, className, 128);
    if (!IsWindowVisible(window) || std::wcscmp(className, L"UnityWndClass") != 0) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return TRUE;
    wchar_t path[32768]{};
    DWORD length = 32768;
    const bool matches = QueryFullProcessImageNameW(process, 0, path, &length) &&
                         calibrator::sameExecutablePath(search.executable.wstring(), path);
    CloseHandle(process);
    if (matches && (!search.pid || search.pid == pid)) {
        search.window = window;
        search.pid = pid;
        return FALSE;
    }
    return TRUE;
}

class Application {
    calibrator::Mode mode_;
    fs::path dataDirectory_;
    fs::path executable_;
    std::ofstream log_;
    SteamVR vr_;
    // Capture must be released before the overlay/runtime.
    std::unique_ptr<WindowCapture> capture_;
    WindowSearch source_;
    bool ownsProcess_ = false;
    std::array<calibrator::ButtonEdges, vr::k_unMaxTrackedDeviceCount> buttons_{};
    HWND control_ = nullptr, status_ = nullptr;
    std::chrono::steady_clock::time_point started_{};
    std::chrono::steady_clock::time_point lastFrame_{};
    bool displayLive_ = false;
public:
    bool quit = false;
    bool sendEnter = false;

    explicit Application(calibrator::Mode mode) : mode_(mode) {
        dataDirectory_ = folder(FOLDERID_LocalAppData) / L"SteamVREyeCalibrator";
        fs::create_directories(dataDirectory_);
        log_.open(dataDirectory_ / (mode == calibrator::Mode::Diagnose ? L"diagnostic.log" :
                                  mode == calibrator::Mode::Smoke ? L"smoke.log" : L"session.log"));
        if (!log_) throw std::runtime_error("Cannot write the utility's log.");
        executable_ = folder(FOLDERID_ProgramFiles) / L"Pimax/EyeTrackingGuide/EyeTrackingGuide.exe";
    }

    void record(const std::string& text) { log_ << text << std::endl; }
    const fs::path& dataDirectory() const { return dataDirectory_; }
    void initialize() {
        record("SteamVR Eye Calibrator 1.0.0; original Pimax calibration engine.");
        const auto data = executable_.parent_path() / L"EyeTrackingGuide_Data";
        if (sha256(data / L"level0") != "97f35379366b32c797542d0b8344435679dcc7652298f22642bd9c8917d0a7a0" ||
            sha256(data / L"Managed/Assembly-CSharp.dll") != "ade48c18c95660706df78b70b56e25777b948289491b8426edebd81c13b0e0e0")
            throw std::runtime_error("This Pimax calibrator version has not been checked for target geometry. Do not calibrate through a resized desktop mirror. This utility needs a geometry update for your installed version.");
        record("PASS: installed Pimax scene and calibration-code identities match inspected geometry.");
        vr_.open();
        char model[256]{};
        vr_.system->GetStringTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd,
            vr::Prop_ModelNumber_String, model, sizeof(model));
        record(std::string("PASS: native SteamVR overlay application initialized. HMD: ") + model);
        if (mode_ == calibrator::Mode::Diagnose) return;
        vr_.create();
        source_.executable = executable_;
        EnumWindows(findWindow, reinterpret_cast<LPARAM>(&source_));
        if (!source_.window) {
            std::wstring command = L"\"" + executable_.wstring() +
                L"\" -screen-fullscreen 0 -screen-width 1280 -screen-height 720 -popupwindow -force-d3d11";
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            PROCESS_INFORMATION process{};
            if (!CreateProcessW(executable_.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                                executable_.parent_path().c_str(), &startup, &process))
                throw std::runtime_error("Cannot start the installed Pimax calibrator.");
            source_.pid = process.dwProcessId;
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            ownsProcess_ = true;
            record("Started original Pimax calibrator without modifying its files or licenses.");
        } else {
            record("Attached to existing verified Pimax window; it will be left running on exit.");
            ShowWindow(source_.window, SW_RESTORE);
        }
        started_ = std::chrono::steady_clock::now();
    }

    void controls(HINSTANCE instance) {
        WNDCLASSW type{};
        type.lpfnWndProc = windowProcedure;
        type.hInstance = instance;
        type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        type.lpszClassName = L"SteamVREyeCalibratorControl";
        if (!RegisterClassW(&type)) throw std::runtime_error("Cannot register control window.");
        control_ = CreateWindowW(type.lpszClassName, L"SteamVR Eye Calibrator", WS_OVERLAPPED | WS_CAPTION |
            WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT, 550, 300,
            nullptr, nullptr, instance, this);
        if (!control_) throw std::runtime_error("Cannot create control window.");
        CreateWindowW(L"STATIC", L"Pimax eye calibration in SteamVR", WS_CHILD | WS_VISIBLE,
            22, 20, 490, 30, control_, nullptr, instance, nullptr);
        status_ = CreateWindowW(L"STATIC", L"Waiting for Pimax's calibration window...", WS_CHILD | WS_VISIBLE,
            22, 55, 490, 40, control_, nullptr, instance, nullptr);
        CreateWindowW(L"STATIC", L"Put on the headset and close the SteamVR dashboard.\r\nPress A / trigger or Enter to calibrate. Follow Pimax's dots.\r\nKeep the Pimax window open and unminimized.",
            WS_CHILD | WS_VISIBLE, 22, 105, 490, 65, control_, nullptr, instance, nullptr);
        CreateWindowW(L"BUTTON", L"Calibrate / Enter", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            22, 190, 210, 36, control_, reinterpret_cast<HMENU>(1), instance, nullptr);
        CreateWindowW(L"BUTTON", L"Exit", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            250, 190, 110, 36, control_, reinterpret_cast<HMENU>(2), instance, nullptr);
        ShowWindow(control_, SW_SHOW);
    }

    static LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        auto* app = reinterpret_cast<Application*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            app = static_cast<Application*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        if (app) {
            if (message == WM_CLOSE || (message == WM_COMMAND && LOWORD(wParam) == 2)) {
                app->quit = true; return 0;
            }
            if ((message == WM_COMMAND && LOWORD(wParam) == 1) || (message == WM_KEYDOWN && wParam == VK_RETURN)) {
                app->sendEnter = true; return 0;
            }
            if (message == WM_KEYDOWN && wParam == VK_ESCAPE) { app->quit = true; return 0; }
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    void enter() {
        if (!source_.window || !IsWindow(source_.window) || !capture_) return;
        const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - lastFrame_).count();
        if (!calibrator::canForwardInput(capture_->frames, age, IsIconic(source_.window) != FALSE,
                                        vr_.overlays->IsDashboardVisible())) {
            record("Input suppressed: no recent visible calibration frame in SteamVR.");
            return;
        }
        DWORD pid = 0;
        GetWindowThreadProcessId(source_.window, &pid);
        if (pid != source_.pid) throw std::runtime_error("Calibrator window identity changed; input was not sent.");
        ShowWindow(source_.window, SW_RESTORE);
        SetForegroundWindow(source_.window);
        DWORD foregroundPid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &foregroundPid);
        if (foregroundPid != source_.pid) {
            if (status_) SetWindowTextW(status_, L"Click Pimax's window once to allow input, then press Enter.");
            record("Input suppressed: Pimax window did not receive foreground focus.");
            return;
        }
        INPUT input[2]{};
        input[0].type = input[1].type = INPUT_KEYBOARD;
        input[0].ki.wVk = input[1].ki.wVk = VK_RETURN;
        input[1].ki.dwFlags = KEYEVENTF_KEYUP;
        if (SendInput(2, input, sizeof(INPUT)) != 2)
            throw std::runtime_error("Windows blocked input to Pimax's calibrator.");
        record("Forwarded explicit Enter action to verified original calibrator.");
    }

    void tick() {
        vr::VREvent_t event{};
        while (vr_.system->PollNextEvent(&event, sizeof(event))) {
            if (event.eventType == vr::VREvent_Quit || event.eventType == vr::VREvent_DriverRequestedQuit) quit = true;
        }
        if (quit) return;
        const auto elapsed = std::chrono::steady_clock::now() - started_;
        if (!source_.window) {
            EnumWindows(findWindow, reinterpret_cast<LPARAM>(&source_));
            if (!source_.window) {
                if (elapsed > std::chrono::seconds(30)) throw std::runtime_error("Pimax's calibration window did not open within 30 seconds.");
                return;
            }
        }
        if (!IsWindow(source_.window)) { record("Pimax calibrator closed."); quit = true; return; }
        if (IsIconic(source_.window)) {
            vr_.check(vr_.overlays->HideOverlay(vr_.handle), "Hide minimized calibration display");
            displayLive_ = false;
            sendEnter = false;
            if (status_) SetWindowTextW(status_, L"Restore the Pimax calibration window to continue.");
            return;
        }
        if (!capture_) {
            if (!source_.pid) throw std::runtime_error("Calibrator process identity was not established.");
            capture_ = std::make_unique<WindowCapture>();
            capture_->start(source_.window, vr_.system);
            record("PASS: Windows Graphics Capture bound to verified Pimax window.");
        }
        if (capture_->update(source_.window, vr_)) {
            lastFrame_ = std::chrono::steady_clock::now();
            if (capture_->frames == 1)
                record("PASS: captured Direct3D frame submitted to native SteamVR overlay at OEM geometry.");
            if (!displayLive_) {
                displayLive_ = true;
                if (status_) SetWindowTextW(status_, L"Live in the headset. Close the SteamVR dashboard to see it.");
            }
        } else if (displayLive_ && std::chrono::steady_clock::now() - lastFrame_ > std::chrono::milliseconds(1500)) {
            vr_.check(vr_.overlays->HideOverlay(vr_.handle), "Hide stale calibration display");
            displayLive_ = false;
            if (status_) SetWindowTextW(status_, L"Capture paused. Restore Pimax; restart calibration if interrupted.");
            record("Hidden stale calibration display. Restart vendor calibration if it was interrupted.");
        }
        if (mode_ == calibrator::Mode::Smoke) {
            if (elapsed > std::chrono::seconds(15) && capture_->frames >= 30) {
                capture_->save(dataDirectory_ / L"smoke-capture.png");
                record("PASS: native overlay smoke test; frames=" + std::to_string(capture_->frames) +
                       ", resolution=" + std::to_string(capture_->width) + "x" + std::to_string(capture_->height));
                record("No Enter action sent. Eye calibration was not started.");
                quit = true;
            } else if (elapsed > std::chrono::seconds(35)) {
                throw std::runtime_error("No usable calibration frames arrived. Check that Pimax's window is visible and unminimized.");
            }
            return;
        }
        const bool dashboard = vr_.overlays->IsDashboardVisible();
        for (uint32_t index = 0; index < vr::k_unMaxTrackedDeviceCount; ++index) {
            vr::VRControllerState_t state{};
            const bool connected = vr_.system->GetControllerState(index, &state, sizeof(state));
            const uint64_t mask = vr::ButtonMaskFromId(vr::k_EButton_A) | vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Trigger);
            if (buttons_[index].update(connected && (state.ulButtonPressed & mask) != 0, dashboard)) sendEnter = true;
        }
        if (sendEnter) { sendEnter = false; if (!dashboard) enter(); }
        if (capture_->frames == 0 && elapsed > std::chrono::seconds(35))
            throw std::runtime_error("No calibration frames arrived from Windows Graphics Capture.");
    }

    ~Application() {
        capture_.reset();
        if (ownsProcess_) {
            if (!source_.window) EnumWindows(findWindow, reinterpret_cast<LPARAM>(&source_));
            if (source_.window && IsWindow(source_.window)) PostMessageW(source_.window, WM_CLOSE, 0, 0);
        }
        if (control_) DestroyWindow(control_);
        record("Released capture and overlay; requested normal exit only for a calibrator started by this utility.");
    }
};

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    std::unique_ptr<Application> app;
    calibrator::Mode mode = calibrator::Mode::Normal;
    HANDLE mutex = nullptr;
    try {
        init_apartment(apartment_type::multi_threaded);
        int count = 0;
        LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!args) throw std::runtime_error("Cannot read command-line arguments.");
        std::vector<std::wstring> arguments;
        for (int i = 1; i < count; ++i) arguments.emplace_back(args[i]);
        LocalFree(args);
        mode = calibrator::parseMode(arguments);
        if (mode != calibrator::Mode::Diagnose) {
            mutex = CreateMutexW(nullptr, FALSE, L"Local\\SteamVREyeCalibrator");
            if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS)
                throw std::runtime_error("SteamVR Eye Calibrator is already running.");
        }
        app = std::make_unique<Application>(mode);
        app->initialize();
        if (mode == calibrator::Mode::Diagnose) app->record("PASS: diagnostics complete. No overlay or calibration started.");
        else {
            if (mode == calibrator::Mode::Normal) app->controls(instance);
            while (!app->quit) {
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
                app->tick();
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
            }
        }
        app.reset();
        if (mutex) CloseHandle(mutex);
        return 0;
    } catch (const hresult_error& error) {
        const auto text = to_string(error.message());
        if (app) app->record("ERROR: " + text);
        if (mode == calibrator::Mode::Normal) MessageBoxW(nullptr, error.message().c_str(), L"SteamVR Eye Calibrator", MB_OK | MB_ICONERROR);
    } catch (const std::exception& error) {
        if (app) app->record(std::string("ERROR: ") + error.what());
        if (mode == calibrator::Mode::Normal) MessageBoxW(nullptr, to_hstring(error.what()).c_str(), L"SteamVR Eye Calibrator", MB_OK | MB_ICONERROR);
    }
    app.reset();
    if (mutex) CloseHandle(mutex);
    return 1;
}
