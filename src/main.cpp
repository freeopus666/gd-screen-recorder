/*
 * GD Screen Recorder v7.0.0 — Final Stable Edition
 * Developer: JamStickGD
 *
 * Features:
 *  - F5  = Start/Stop recording
 *  - F8  = Cycle microphone device (shows current + selects next)
 *  - F9  = Cycle game audio device
 *  - F12 = Screenshot (BMP)
 *  - F11 = Toggle indicator
 *  - Microphone recorded as Track 1 (via DirectShow)
 *  - Game audio recorded as Track 2 (via WASAPI loopback — works on any speaker/headphone)
 *  - Both tracks in ONE .mp4 file — mute/unmute per track in any video player
 *  - PBO double-buffer async GPU readback (zero GL stall)
 *  - Hardware encoder auto-detection (NVENC/AMF/QSV) with libx264 fallback
 *
 * Performance:
 *  - Frame duplication: auto-fills timing gaps for perfectly smooth CFR output
 *  - Async stop: GL thread returns INSTANTLY on F5-stop (no freeze)
 *  - Pool: 60 frames default, 120 for software encoding — zero frame drops
 *  - Writer thread at ABOVE_NORMAL priority — drains queue fast, no stutter
 *  - Software encoder: NORMAL FFmpeg priority + 64MB pipe buffer
 *  - FFmpeg thread_queue_size 2048 — absorbs audio muxing overhead
 *  - -shortest flag: FFmpeg exits immediately when video pipe closes (no mic hang)
 *  - Inline -movflags +faststart (no separate remux step)
 *  - Fast color conversion: -sws_flags fast_bilinear
 *  - FFmpeg wait timeout: 15s — safe margin for large recordings
 */

#include <Geode/Geode.hpp>
#include <Geode/modify/CCEGLView.hpp>

#include <windows.h>
#include <GL/gl.h>
#include <mmdeviceapi.h>
#include <propsys.h>
#include <comdef.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>
#include <unordered_map>

using namespace geode::prelude;
namespace fs = std::filesystem;

// ==================================================================
// OpenGL PBO extensions (GL 1.5+, loaded at runtime on Windows)
// ==================================================================

#ifndef GL_PIXEL_PACK_BUFFER
#define GL_PIXEL_PACK_BUFFER 0x88EB
#endif
#ifndef GL_STREAM_READ
#define GL_STREAM_READ 0x88E1
#endif
#ifndef GL_READ_ONLY
#define GL_READ_ONLY 0x88B8
#endif
#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_FRONT
#define GL_FRONT 0x0404
#endif
#ifndef GL_BACK
#define GL_BACK 0x0405
#endif

typedef ptrdiff_t GLsizeiptr_t;

typedef void      (APIENTRY* PFN_glGenBuffers)(GLsizei, GLuint*);
typedef void      (APIENTRY* PFN_glDeleteBuffers)(GLsizei, const GLuint*);
typedef void      (APIENTRY* PFN_glBindBuffer)(GLenum, GLuint);
typedef void      (APIENTRY* PFN_glBufferData)(GLenum, GLsizeiptr_t, const void*, GLenum);
typedef void*     (APIENTRY* PFN_glMapBuffer)(GLenum, GLenum);
typedef GLboolean (APIENTRY* PFN_glUnmapBuffer)(GLenum);

static PFN_glGenBuffers    fnGenBuffers    = nullptr;
static PFN_glDeleteBuffers fnDeleteBuffers = nullptr;
static PFN_glBindBuffer    fnBindBuffer    = nullptr;
static PFN_glBufferData    fnBufferData    = nullptr;
static PFN_glMapBuffer     fnMapBuffer     = nullptr;
static PFN_glUnmapBuffer   fnUnmapBuffer   = nullptr;

static bool loadPBOFunctions() {
    static int cached = -1;
    if (cached >= 0) return cached == 1;

    fnGenBuffers    = (PFN_glGenBuffers)   wglGetProcAddress("glGenBuffers");
    fnDeleteBuffers = (PFN_glDeleteBuffers)wglGetProcAddress("glDeleteBuffers");
    fnBindBuffer    = (PFN_glBindBuffer)   wglGetProcAddress("glBindBuffer");
    fnBufferData    = (PFN_glBufferData)   wglGetProcAddress("glBufferData");
    fnMapBuffer     = (PFN_glMapBuffer)    wglGetProcAddress("glMapBuffer");
    fnUnmapBuffer   = (PFN_glUnmapBuffer)  wglGetProcAddress("glUnmapBuffer");

    bool ok = fnGenBuffers && fnDeleteBuffers && fnBindBuffer &&
              fnBufferData && fnMapBuffer && fnUnmapBuffer;
    cached = ok ? 1 : 0;
    return ok;
}

// ==================================================================
// Utility functions
// ==================================================================

static std::string gdDir() {
    char buf[MAX_PATH]{};
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    return fs::path(buf).parent_path().string();
}

static std::string locateFFmpeg() {
    std::string local = gdDir() + "\\ffmpeg.exe";
    if (fs::exists(local)) return local;
    char buf[MAX_PATH]{};
    if (SearchPathA(nullptr, "ffmpeg", ".exe", MAX_PATH, buf, nullptr))
        return std::string(buf);
    return {};
}

static std::string getOutputDir() {
    std::string s = Mod::get()->getSettingValue<std::string>("output-dir");
    if (!s.empty()) return s;
    return gdDir() + "\\recordings";
}

static void createOutputDir() {
    std::error_code ec;
    fs::create_directories(getOutputDir(), ec);
}

static std::string buildOutputPath(const std::string& prefix = "GD") {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &t);
    std::ostringstream oss;
    oss << getOutputDir() << "\\" << prefix << "_"
        << std::put_time(&tm, "%Y%m%d_%H%M%S") << ".mp4";
    return oss.str();
}

static int qualityToCRF(int q) {
    switch (q) {
        case 1:  return 28;
        case 3:  return 18;
        case 4:  return 15;
        default: return 23;
    }
}

// ==================================================================
// GPU detection + encoder selection
// ==================================================================

static bool        g_gpuDetected = false;
static std::string g_gpuVendor;

static void detectGPU() {
    if (g_gpuDetected) return;
    const char* v = (const char*)glGetString(GL_VENDOR);
    const char* r = (const char*)glGetString(GL_RENDERER);
    if (v) g_gpuVendor = v;
    g_gpuDetected = true;
    log::info("[Rec] GPU: {} ({})", v ? v : "?", r ? r : "?");
}

static std::string autoDetectEncoder() {
    std::string v = g_gpuVendor;
    for (auto& c : v) c = static_cast<char>(toupper(c));
    if (v.find("NVIDIA") != std::string::npos) return "h264_nvenc";
    if (v.find("AMD")    != std::string::npos) return "h264_amf";
    if (v.find("ATI")    != std::string::npos) return "h264_amf";
    if (v.find("INTEL")  != std::string::npos) return "h264_qsv";
    return "libx264";
}

static std::string pickEncoder(int setting) {
    switch (setting) {
        case 2: return "h264_nvenc";
        case 3: return "h264_amf";
        case 4: return "h264_qsv";
        case 5: return "libx264";
        default: return autoDetectEncoder();
    }
}

static std::string buildEncoderArgs(const std::string& encoder, int crf,
                                    bool lowLatency = false) {
    if (encoder == "h264_nvenc") {
        if (lowLatency) {
            return " -c:v h264_nvenc -preset p1 -tune ll -rc constqp -qp " +
                   std::to_string(crf) + " -pix_fmt yuv420p";
        }
        return " -c:v h264_nvenc -preset p4 -rc constqp -qp " +
               std::to_string(crf) + " -pix_fmt yuv420p";
    }
    if (encoder == "h264_amf") {
        if (lowLatency) {
            return " -c:v h264_amf -usage lowlatency -quality speed -rc cqp -qp_i " +
                   std::to_string(crf) + " -qp_p " +
                   std::to_string(crf) + " -pix_fmt yuv420p";
        }
        return " -c:v h264_amf -quality speed -rc cqp -qp_i " +
               std::to_string(crf) + " -qp_p " +
               std::to_string(crf) + " -pix_fmt yuv420p";
    }
    if (encoder == "h264_qsv") {
        if (lowLatency) {
            return " -c:v h264_qsv -preset veryfast -low_power 1 -global_quality " +
                   std::to_string(crf) + " -pix_fmt yuv420p";
        }
        return " -c:v h264_qsv -preset veryfast -global_quality " +
               std::to_string(crf) + " -pix_fmt yuv420p";
    }
    // -threads 0 = use all available CPU cores.
    // Previously limited to 2, which capped encode throughput and caused
    // pipe backpressure on weak CPUs (e.g. GT 630M laptops with 4 logical cores).
    if (lowLatency) {
        return " -c:v libx264 -preset ultrafast -tune zerolatency -crf " +
               std::to_string(crf) + " -pix_fmt yuv420p -threads 0";
    }
    return " -c:v libx264 -preset ultrafast -tune zerolatency -crf " +
           std::to_string(crf) + " -pix_fmt yuv420p -threads 0";
}

// Convert UTF-8 string to wide (UTF-16) for CreateProcessW
static std::wstring utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (len <= 0) return std::wstring(utf8.begin(), utf8.end());
    std::wstring wide(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wide.data(), len);
    return wide;
}

// Spawn a process with UTF-8 command line (handles Cyrillic/CJK device names)
static BOOL spawnProcessW(const std::string& cmdUtf8,
                           HANDLE hIn, HANDLE hOut, HANDLE hErr,
                           DWORD flags, PROCESS_INFORMATION* pi) {
    std::wstring wCmd = utf8ToWide(cmdUtf8);
    std::vector<wchar_t> buf(wCmd.begin(), wCmd.end());
    buf.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb        = sizeof(si);
    si.hStdInput = hIn;
    si.hStdOutput = hOut;
    si.hStdError = hErr;
    si.dwFlags   = STARTF_USESTDHANDLES;

    return CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE,
                          flags | CREATE_NO_WINDOW, nullptr, nullptr, &si, pi);
}

static bool testEncoderWorks(const std::string& ffmpegExe,
                              const std::string& encoder,
                              const std::string& logDir) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    std::string logPath = logDir + "\\~ffmpeg_log.txt";
    HANDLE stderrLog = CreateFileA(
        logPath.c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr
    );
    if (stderrLog != INVALID_HANDLE_VALUE) {
        std::string sep = "\r\n=== Encoder test [" + encoder + "] ===\r\n";
        DWORD bw = 0;
        WriteFile(stderrLog, sep.c_str(), (DWORD)sep.size(), &bw, nullptr);
    }

    HANDLE nul = CreateFileA(
        "NUL", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
        OPEN_EXISTING, 0, nullptr
    );

    std::string cmd = "\"" + ffmpegExe + "\""
        " -hide_banner -loglevel warning"
        " -f lavfi -i color=c=black:s=64x64:d=0.04:r=25"
        " -frames:v 1 -c:v " + encoder +
        " -pix_fmt yuv420p -f null -";

    HANDLE stderrH = (stderrLog != INVALID_HANDLE_VALUE) ? stderrLog : nul;

    PROCESS_INFORMATION pi{};
    BOOL ok = spawnProcessW(cmd, nul, nul, stderrH, 0, &pi);

    if (stderrLog != INVALID_HANDLE_VALUE) CloseHandle(stderrLog);
    CloseHandle(nul);

    if (!ok) return false;

    CloseHandle(pi.hThread);
    DWORD waitResult = WaitForSingleObject(pi.hProcess, 10000);
    if (waitResult == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 2000);
    }

    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    log::info("[Rec] Encoder test [{}] exit code: {}", encoder, exitCode);
    return exitCode == 0;
}

// Test if this FFmpeg build recognizes the WASAPI -loopback option.
// Returns true if supported (even if the probe device isn't found).
static bool testWasapiLoopback(const std::string& ffmpegExe) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE pipe_r = INVALID_HANDLE_VALUE, pipe_w = INVALID_HANDLE_VALUE;
    if (!CreatePipe(&pipe_r, &pipe_w, &sa, 0)) return false;
    SetHandleInformation(pipe_r, HANDLE_FLAG_INHERIT, 0);  // child must not inherit read end

    HANDLE nul = CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    if (nul == INVALID_HANDLE_VALUE) {
        CloseHandle(pipe_r); CloseHandle(pipe_w); return false;
    }

    // Use a bogus device — we only care whether FFmpeg recognises the -loopback option.
    // "Unrecognized option 'loopback'" = old build, no WASAPI loopback.
    // Any other error (device not found, etc.) = option IS present.
    std::string cmd = "\"" + ffmpegExe +
        "\" -f wasapi -loopback 1 -i \"__wasapi_probe__\" -t 0 -f null -";

    PROCESS_INFORMATION pi{};
    BOOL ok = spawnProcessW(cmd, nul, nul, pipe_w, 0, &pi);
    CloseHandle(pipe_w);
    CloseHandle(nul);
    if (!ok) { CloseHandle(pipe_r); return false; }
    CloseHandle(pi.hThread);

    // Drain stderr; ReadFile blocks until child closes its end (= child exits)
    std::string output;
    char buf[512]; DWORD br = 0;
    while (ReadFile(pipe_r, buf, sizeof(buf), &br, nullptr) && br > 0)
        output.append(buf, br);
    CloseHandle(pipe_r);

    WaitForSingleObject(pi.hProcess, 3000);
    CloseHandle(pi.hProcess);

    bool supported = output.find("Unrecognized option") == std::string::npos;
    if (supported)
        log::info("[Rec] WASAPI loopback: SUPPORTED — game audio captures speakers directly.");
    else
        log::warn("[Rec] WASAPI loopback: NOT supported by this FFmpeg build. "
                  "Falling back to dshow/Stereo Mix. Update FFmpeg for full support.");
    return supported;
}

// ==================================================================
// Audio device detection (Windows COM API + FFmpeg dshow)
// ==================================================================

// PKEY_Device_FriendlyName {a45c254e-df1c-4efd-8020-67d146a850e0}, 14
static const PROPERTYKEY PKEY_DeviceFriendlyName =
    {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

static std::string wcharToUtf8(const wchar_t* wstr) {
    if (!wstr) return "";
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return "";
    std::string result(size - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, result.data(), size, nullptr, nullptr);
    return result;
}

// Get default mic or speaker name via Windows Audio API
static std::string getDefaultAudioDeviceName(bool isMicrophone) {
    std::string result;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool needUninit = (SUCCEEDED(hr) || hr == S_FALSE);
    if (FAILED(hr) && hr != S_FALSE && hr != RPC_E_CHANGED_MODE) return "";

    IMMDeviceEnumerator* enumerator = nullptr;
    hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (void**)&enumerator
    );
    if (FAILED(hr) || !enumerator) {
        if (needUninit) CoUninitialize();
        return "";
    }

    IMMDevice* device = nullptr;
    hr = enumerator->GetDefaultAudioEndpoint(
        isMicrophone ? eCapture : eRender, eConsole, &device
    );
    if (FAILED(hr) || !device) {
        enumerator->Release();
        if (needUninit) CoUninitialize();
        return "";
    }

    IPropertyStore* props = nullptr;
    hr = device->OpenPropertyStore(STGM_READ, &props);
    if (SUCCEEDED(hr) && props) {
        PROPVARIANT varName;
        PropVariantInit(&varName);
        hr = props->GetValue(PKEY_DeviceFriendlyName, &varName);
        if (SUCCEEDED(hr) && varName.vt == VT_LPWSTR) {
            result = wcharToUtf8(varName.pwszVal);
        }
        PropVariantClear(&varName);
        props->Release();
    }

    device->Release();
    enumerator->Release();
    if (needUninit) CoUninitialize();
    return result;
}

// ==================================================================
// Audio device enumeration (Windows COM API — no FFmpeg subprocess!)
// ==================================================================

#ifndef DEVICE_STATE_ACTIVE
#define DEVICE_STATE_ACTIVE 0x00000001
#endif

static std::vector<std::string> g_captureDevices;  // microphones (UTF-8 names)
static std::vector<std::string> g_renderDevices;    // speakers/outputs (UTF-8 names)
static bool g_devicesEnumerated = false;
static bool g_wasapiLoopbackSupported = false;  // probed at mod load

static std::vector<std::string> enumerateEndpoints(EDataFlow flow) {
    std::vector<std::string> result;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool needUninit = (SUCCEEDED(hr) || hr == S_FALSE);
    if (FAILED(hr) && hr != S_FALSE && hr != RPC_E_CHANGED_MODE) return result;

    IMMDeviceEnumerator* enumerator = nullptr;
    hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (void**)&enumerator
    );
    if (FAILED(hr) || !enumerator) {
        if (needUninit) CoUninitialize();
        return result;
    }

    IMMDeviceCollection* collection = nullptr;
    hr = enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr) || !collection) {
        enumerator->Release();
        if (needUninit) CoUninitialize();
        return result;
    }

    UINT count = 0;
    collection->GetCount(&count);
    for (UINT i = 0; i < count; i++) {
        IMMDevice* device = nullptr;
        if (FAILED(collection->Item(i, &device)) || !device) continue;

        IPropertyStore* props = nullptr;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props)) && props) {
            PROPVARIANT varName;
            PropVariantInit(&varName);
            if (SUCCEEDED(props->GetValue(PKEY_DeviceFriendlyName, &varName)) &&
                varName.vt == VT_LPWSTR) {
                result.push_back(wcharToUtf8(varName.pwszVal));
            }
            PropVariantClear(&varName);
            props->Release();
        }
        device->Release();
    }

    collection->Release();
    enumerator->Release();
    if (needUninit) CoUninitialize();
    return result;
}

static void enumerateAudioDevices() {
    if (g_devicesEnumerated) return;
    g_devicesEnumerated = true;
    g_captureDevices = enumerateEndpoints(eCapture);
    g_renderDevices  = enumerateEndpoints(eRender);
}

static void logAudioDevices() {
    enumerateAudioDevices();

    log::info("[Rec] === Microphone Devices (for mic-device setting) ===");
    for (size_t i = 0; i < g_captureDevices.size(); i++) {
        log::info("[Rec]   {}: \"{}\"", i + 1, g_captureDevices[i]);
    }
    if (g_captureDevices.empty()) log::info("[Rec]   (none found)");

    log::info("[Rec] === Audio Output Devices (for game-audio-device setting) ===");
    for (size_t i = 0; i < g_renderDevices.size(); i++) {
        log::info("[Rec]   {}: \"{}\"", i + 1, g_renderDevices[i]);
    }
    if (g_renderDevices.empty()) log::info("[Rec]   (none found)");

    std::string defMic = getDefaultAudioDeviceName(true);
    if (!defMic.empty()) log::info("[Rec] Default mic (auto): \"{}\"", defMic);

    log::info("[Rec] Tip: set mic-device to \"1\", \"2\" etc. or exact device name. Empty = auto.");
}

// Resolve device name from settings: "" = auto, "1"/"2" = by index, else = exact name
static std::string resolveDeviceName(const std::string& setting, bool isMicrophone) {
    if (setting.empty()) {
        return getDefaultAudioDeviceName(isMicrophone);
    }
    // Numeric index: "1", "2", etc.
    bool isNumeric = !setting.empty() &&
        std::all_of(setting.begin(), setting.end(), [](char c){ return std::isdigit((unsigned char)c); });
    if (isNumeric) {
        enumerateAudioDevices();
        int idx = std::stoi(setting) - 1;
        const auto& devices = isMicrophone ? g_captureDevices : g_renderDevices;
        if (idx >= 0 && idx < static_cast<int>(devices.size())) {
            log::info("[Rec] Resolved device #{} -> \"{}\"", setting, devices[idx]);
            return devices[idx];
        }
        log::warn("[Rec] Device index {} out of range (have {} devices)",
                  setting, devices.size());
    }
    return setting;  // exact name
}

// ==================================================================
// Shared Frame Capture (PBO + fallback, single capture per frame)
// ==================================================================

class SharedFrameCapture {
public:
    bool isInitialized() const { return m_initialized; }
    int  width()     const { return m_w; }
    int  height()    const { return m_h; }
    int  frameSize() const { return m_frameSize; }

    void init() {
        GLint vp[4];
        glGetIntegerv(GL_VIEWPORT, vp);
        m_vpX = vp[0]; m_vpY = vp[1];
        m_w   = vp[2]; m_h   = vp[3];
        if (m_w % 2) m_w--;
        if (m_h % 2) m_h--;
        if (m_w <= 0 || m_h <= 0) return;

        m_frameSize = m_w * m_h * 4;

        if (loadPBOFunctions()) {
            fnGenBuffers(3, m_pbo);
            for (int i = 0; i < 3; i++) {
                fnBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[i]);
                fnBufferData(GL_PIXEL_PACK_BUFFER, m_frameSize, nullptr, GL_STREAM_READ);
            }
            fnBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            if (glGetError() == GL_NO_ERROR) {
                m_usePBO = true;
                log::info("[Rec] SharedCapture: PBO OK 3x ({}x{})", m_w, m_h);
            } else {
                fnDeleteBuffers(3, m_pbo);
                m_pbo[0] = m_pbo[1] = m_pbo[2] = 0;
            }
        }

        if (!m_usePBO) {
            m_fallbackBuf.resize(m_frameSize);
            log::info("[Rec] SharedCapture: fallback mode ({}x{})", m_w, m_h);
        }

        m_initialized = true;
    }

    // Zero-copy: PBO maps directly into caller's buffer, skipping intermediate copy
    bool captureInto(uint8_t* dst, int dstSize) {
        if (!m_initialized) return false;

        if (!m_usePBO) {
            glReadPixels(m_vpX, m_vpY, m_w, m_h, GL_BGRA, GL_UNSIGNED_BYTE, dst);
            return true;
        }

        bool gotFrame = false;
        // Triple-buffered PBO: read the buffer written 2 frames ago.
        // This gives the GPU DMA transfer 2 full frame intervals (~33ms at 60fps)
        // to complete before we map it, eliminating glMapBuffer stalls on complex scenes
        // that caused progressive FPS degradation as the level loaded more objects.
        if (m_pboFrame >= 2) {
            int readIdx = (m_pboFrame - 2) % 3;
            fnBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[readIdx]);
            void* mapped = fnMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
            if (mapped) {
                std::memcpy(dst, mapped, dstSize);
                gotFrame = true;
                fnUnmapBuffer(GL_PIXEL_PACK_BUFFER);
            }
            fnBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        }

        int writeIdx = m_pboFrame % 3;
        fnBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[writeIdx]);
        glReadPixels(m_vpX, m_vpY, m_w, m_h, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
        fnBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

        m_pboFrame++;

        return gotFrame;
    }

    void reinitIfNeeded() {
        GLint vp[4];
        glGetIntegerv(GL_VIEWPORT, vp);
        int newW = vp[2], newH = vp[3];
        if (newW % 2) newW--;
        if (newH % 2) newH--;
        if (newW != m_w || newH != m_h) {
            log::info("[Rec] Viewport changed {}x{} -> {}x{}, reinit", m_w, m_h, newW, newH);
            destroy();
            init();
        }
    }

    void destroy() {
        if (m_usePBO && fnDeleteBuffers) {
            fnDeleteBuffers(3, m_pbo);
            m_pbo[0] = m_pbo[1] = m_pbo[2] = 0;
        }
        m_usePBO = false;
        m_fallbackBuf.clear();
        m_initialized = false;
        m_pboFrame = 0;
    }

private:
    bool   m_initialized{false};
    bool   m_usePBO{false};
    GLuint m_pbo[3]{};
    int    m_pboFrame{0};
    int    m_vpX{}, m_vpY{}, m_w{}, m_h{}, m_frameSize{};
    std::vector<uint8_t> m_fallbackBuf;
};

static SharedFrameCapture g_capture;

// ==================================================================
// Base recording pipeline
// ==================================================================

class RecordingPipeline {
public:
    enum class State { Idle, Active, Saving };

    State getState() const { return m_state.load(); }
    bool  isActive() const { return m_state == State::Active; }
    bool  isSaving() const { return m_state == State::Saving; }
    bool  hasPipeError() const { return m_pipeError.load(std::memory_order_relaxed); }
    int   getFrames()  const { return m_frames; }
    int   getDropped() const { return m_dropped; }

    // Zero-copy capture: grabs free pool slot, PBO maps directly into it
    void captureDirectly(SharedFrameCapture& cap) {
        if (m_state != State::Active) return;
        if (m_pipeError.load(std::memory_order_relaxed)) return;
        if (m_frameSize != cap.frameSize()) return;

        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - m_lastCapture).count();
        if (elapsed < m_captureInterval * 0.95) return;
        m_lastCapture = now;

        // Timestamp relative to recording start (used for frame duplication in writer)
        double captureTime = std::chrono::duration<double>(now - m_startTime).count();

        int idx = -1;
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            if (m_freeSlots.empty()) { ++m_dropped; return; }
            idx = m_freeSlots.front();
            m_freeSlots.pop();
        }

        // Read from GL_BACK (before swapBuffers) — avoids GL_FRONT sync stall
        bool ok = cap.captureInto(m_pool[idx].data(), m_frameSize);

        if (ok) {
            {
                std::lock_guard<std::mutex> lk(m_mtx);
                m_frameTimes[idx] = captureTime;
                m_writeQueue.push(idx);
                ++m_frames;
            }
            m_cv.notify_one();
        } else {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_freeSlots.push(idx);
        }
    }

    double getElapsedSeconds() const {
        if (m_state != State::Active && m_state != State::Saving) return 0.0;
        return std::chrono::duration<double>(
            std::chrono::steady_clock::now() - m_startTime).count();
    }

protected:
    static constexpr int DEFAULT_MAX_QUEUE = 120;      // ~2s buffer at 60fps — large enough for NVENC on GT 630M
    static constexpr int SW_ENCODER_MAX_QUEUE = 360;   // ~6s buffer for libx264 (GT 630M always falls back here)
    static constexpr DWORD PIPE_BUF_SIZE = 64 * 1024 * 1024;    // 64MB pipe (~8 frames at 1080p, prevents write stalls)
    static constexpr DWORD SW_PIPE_BUF_SIZE = 256 * 1024 * 1024; // 256MB for libx264 — absorbs BELOW_NORMAL scheduling gaps
    static constexpr DWORD FFMPEG_WAIT_TIMEOUT = 15000; // 15s wait for FFmpeg finalization

    int m_maxQueue{DEFAULT_MAX_QUEUE};
    int m_poolSize{DEFAULT_MAX_QUEUE + 4};
    DWORD m_ffmpegPriority{0};
    DWORD m_pipeBufSize{PIPE_BUF_SIZE};

    std::atomic<State> m_state{State::Idle};
    int m_frameSize{};
    int m_width{};   // frame width  (set in start() — used for writer-thread row flip)
    int m_height{};  // frame height
    int m_fps{60};
    double m_captureInterval{1.0 / 60.0};

    // Pre-allocated frame pool + queue
    std::vector<std::vector<uint8_t>> m_pool;
    std::vector<double> m_frameTimes;  // capture timestamp per slot (relative to start)
    std::queue<int> m_freeSlots;
    std::queue<int> m_writeQueue;
    std::mutex m_mtx;
    std::condition_variable m_cv;

    // FFmpeg process handles
    HANDLE m_pipe{INVALID_HANDLE_VALUE};
    HANDLE m_proc{INVALID_HANDLE_VALUE};
    HANDLE m_nulHandle{INVALID_HANDLE_VALUE};

    // Writer thread
    std::thread m_writerThread;

    // Stats + error tracking
    std::atomic<int> m_frames{0};
    std::atomic<int> m_dropped{0};
    std::atomic<int> m_duped{0};
    std::atomic<bool> m_pipeError{false};
    bool m_ffmpegOk{false};
    std::string m_stderrLogPath;

    // Timing
    std::chrono::steady_clock::time_point m_startTime;
    std::chrono::steady_clock::time_point m_lastCapture;

    void allocatePool() {
        m_pool.resize(m_poolSize);
        m_frameTimes.resize(m_poolSize, 0.0);
        for (int i = 0; i < m_poolSize; i++) {
            m_pool[i].resize(m_frameSize);
            m_freeSlots.push(i);
        }
    }

    void freePool() {
        m_pool.clear();
        m_pool.shrink_to_fit();
        m_frameTimes.clear();
        m_frameTimes.shrink_to_fit();
        while (!m_freeSlots.empty())  m_freeSlots.pop();
        while (!m_writeQueue.empty()) m_writeQueue.pop();
    }

    void writerLoop() {
        // NORMAL priority — do NOT use ABOVE_NORMAL, it starves the game's GL
        // thread and causes FPS drops in the game itself.
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
        std::vector<uint8_t> frameBuf;
        double nextPts = 0.0;
        bool firstFrame = true;

        for (;;) {
            int idx = -1;
            double captureTime = 0.0;
            {
                std::unique_lock<std::mutex> lk(m_mtx);
                m_cv.wait(lk, [this] {
                    return !m_writeQueue.empty() || m_state != State::Active;
                });
                if (m_writeQueue.empty()) break;
                idx = m_writeQueue.front();
                m_writeQueue.pop();
                captureTime = m_frameTimes[idx];
            }

            // Copy frame data to local buffer and IMMEDIATELY release the pool
            // slot back to the GL thread. Previously the slot was held for the
            // entire duration of WriteFile calls (including all dup writes),
            // which could block for seconds under pipe backpressure, starving
            // the capture pool and causing cascading FPS drops.
            if ((int)frameBuf.size() != m_frameSize)
                frameBuf.resize(m_frameSize);
            std::memcpy(frameBuf.data(), m_pool[idx].data(), m_frameSize);
            {
                std::lock_guard<std::mutex> lk(m_mtx);
                m_freeSlots.push(idx);
            }

            // Flip rows in-place (OpenGL reads bottom-up; video encoders expect top-down).
            // Doing this here eliminates the -vf vflip CPU filter inside FFmpeg,
            // cutting ~15% encode overhead on old/weak GPUs like GT 630M.
            if (m_width > 0 && m_height > 1) {
                const int stride = m_width * 4;
                for (int y = 0; y < m_height / 2; y++) {
                    uint8_t* r1 = frameBuf.data() + y * stride;
                    uint8_t* r2 = frameBuf.data() + (m_height - 1 - y) * stride;
                    std::swap_ranges(r1, r1 + stride, r2);
                }
            }

            if (firstFrame) {
                firstFrame = false;
                nextPts = captureTime;
            }

            // Fill timing gaps with duplicate frames (prevents video speed-up
            // when game FPS drops below recording FPS). Cap at 0.5 seconds —
            // larger gaps are skipped to avoid pipe flooding from a long game
            // pause, which was causing cascading backpressure that killed FPS.
            int dupCount = 0;
            int maxDups = m_fps / 2; // max 0.5 seconds of gap filling
            while (nextPts + m_captureInterval * 0.5 < captureTime && dupCount < maxDups) {
                writeFrameToPipe(frameBuf.data(), m_frameSize);
                nextPts += m_captureInterval;
                ++m_duped;
                ++dupCount;
            }
            // If gap is still too large after max dups, skip ahead in time
            if (nextPts + m_captureInterval * 0.5 < captureTime) {
                nextPts = captureTime;
            }

            // Write actual captured frame
            writeFrameToPipe(frameBuf.data(), m_frameSize);
            nextPts += m_captureInterval;
        }
    }

    void writeFrameToPipe(const uint8_t* data, int size) {
        if (m_pipe == INVALID_HANDLE_VALUE || m_pipeError.load(std::memory_order_relaxed))
            return;
        DWORD off = 0;
        while (static_cast<int>(off) < size) {
            DWORD written = 0;
            BOOL ok = WriteFile(m_pipe, data + off, size - off, &written, nullptr);
            if (!ok || written == 0) {
                DWORD err = GetLastError();
                log::error("[Rec] WriteFile failed: {} (pipe broken)", err);
                m_pipeError.store(true, std::memory_order_relaxed);
                break;
            }
            off += written;
        }
    }

    void stopPipeline(bool fast = false) {
        State expected = State::Active;
        m_state.compare_exchange_strong(expected, State::Idle);
        if (expected == State::Saving) m_state = State::Saving;
        m_cv.notify_all();

        // In fast mode, kill FFmpeg FIRST to break the pipe and unblock writer thread
        if (fast && m_proc != INVALID_HANDLE_VALUE) {
            TerminateProcess(m_proc, 0);
        }

        if (m_writerThread.joinable()) m_writerThread.join();

        if (m_pipe != INVALID_HANDLE_VALUE) {
            CloseHandle(m_pipe);
            m_pipe = INVALID_HANDLE_VALUE;
        }

        m_ffmpegOk = false;
        if (m_proc != INVALID_HANDLE_VALUE) {
            DWORD timeout = fast ? 1000 : FFMPEG_WAIT_TIMEOUT;
            DWORD result = WaitForSingleObject(m_proc, timeout);
            if (result == WAIT_TIMEOUT) {
                log::warn("[Rec] FFmpeg timeout, terminating");
                TerminateProcess(m_proc, 1);
                WaitForSingleObject(m_proc, 500);
            }
            DWORD exitCode = 0;
            GetExitCodeProcess(m_proc, &exitCode);
            m_ffmpegOk = (exitCode == 0 && !m_pipeError.load());
            if (!m_ffmpegOk && !fast) {
                log::error("[Rec] FFmpeg exited with code: {} (pipe_error={})",
                           exitCode, m_pipeError.load());
            }
            CloseHandle(m_proc);
            m_proc = INVALID_HANDLE_VALUE;
        }

        if (m_nulHandle != INVALID_HANDLE_VALUE) {
            CloseHandle(m_nulHandle);
            m_nulHandle = INVALID_HANDLE_VALUE;
        }

        freePool();
        m_pipeError.store(false);
    }

    // Build FFmpeg command with optional audio dshow inputs
    std::string buildFFmpegCommand(
        const std::string& ffmpegExe,
        int w, int h, int fps,
        const std::string& encoder, int crf,
        const std::string& outPath,
        bool addKeyframes, bool lowLatency,
        bool micEnabled, const std::string& micDevice,
        bool gameAudioEnabled, const std::string& gameAudioDevice
    ) {
        std::ostringstream cmd;
        cmd << "\"" << ffmpegExe << "\""
            << " -y -hide_banner -loglevel warning"
            << " -f rawvideo -pixel_format bgra"
            << " -video_size " << w << "x" << h
            << " -framerate " << fps
            << " -thread_queue_size 2048"
            << " -i pipe:0"
            << " -sws_flags fast_bilinear";  // faster BGRA→YUV420P conversion

        int inputIdx = 1;
        int micInputIdx = -1;
        int gameInputIdx = -1;

        // Microphone input via dshow
        if (micEnabled && !micDevice.empty()) {
            cmd << " -f dshow -thread_queue_size 2048 -i audio=\"" << micDevice << "\"";
            micInputIdx = inputIdx++;
        }

        // Game audio: WASAPI loopback (modern FFmpeg) or dshow/StereoMix (older FFmpeg)
        if (gameAudioEnabled && !gameAudioDevice.empty()) {
            if (g_wasapiLoopbackSupported)
                cmd << " -f wasapi -loopback 1 -thread_queue_size 2048 -i \"" << gameAudioDevice << "\"";
            else
                cmd << " -f dshow -thread_queue_size 2048 -i audio=\"" << gameAudioDevice << "\"";
            gameInputIdx = inputIdx++;
        }

        // Map streams: video from pipe, audio tracks from dshow
        cmd << " -map 0:v";
        if (micInputIdx >= 0)  cmd << " -map " << micInputIdx << ":a";
        if (gameInputIdx >= 0) cmd << " -map " << gameInputIdx << ":a";

        // Vertical flip is handled in the writer thread (std::swap_ranges per row),
        // so no -vf vflip is needed here — saves a CPU filter pass in FFmpeg.
        // For libx264 on slow/old GPUs (GT 630M): auto-scale to 720p when source
        // is larger. 720p libx264 ultrafast encodes ~60fps on any dual-core CPU;
        // 1080p only manages ~25-30fps, causing constant pipe backpressure.
        bool isSoftware = (encoder == "libx264");
        if (isSoftware && h > 720) {
            cmd << " -vf \"scale=-2:720\"";
        }
        cmd << buildEncoderArgs(encoder, crf, lowLatency);

        // Keyframes every 2 seconds (wider = less encode overhead, ±2s trim accuracy is fine)
        if (addKeyframes) {
            cmd << " -g " << (fps * 2);
        }

        // Audio encoding for each track
        int audioTrack = 0;
        if (micInputIdx >= 0) {
            cmd << " -c:a:" << audioTrack << " aac -b:a:" << audioTrack << " 192k"
                << " -metadata:s:a:" << audioTrack << " title=\"Microphone\"";
            audioTrack++;
        }
        if (gameInputIdx >= 0) {
            cmd << " -c:a:" << audioTrack << " aac -b:a:" << audioTrack << " 192k"
                << " -metadata:s:a:" << audioTrack << " title=\"Game Audio\"";
            audioTrack++;
        }

        // If any audio input exists, -shortest makes FFmpeg stop when video pipe closes
        // (prevents hang: dshow audio capture is real-time and never sends EOF)
        bool hasAudio = (micInputIdx >= 0 || gameInputIdx >= 0);
        if (hasAudio) {
            cmd << " -shortest"
                << " -max_muxing_queue_size 4096";  // prevent muxing stalls with audio
        }

        // Faststart: move moov atom to beginning for instant playback (no separate remux needed)
        cmd << " -movflags +faststart";

        cmd << " \"" << outPath << "\"";
        return cmd.str();
    }

    bool spawnFFmpegProcess(const std::string& cmdStr) {
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE rEnd = INVALID_HANDLE_VALUE;
        HANDLE wEnd = INVALID_HANDLE_VALUE;

        if (!CreatePipe(&rEnd, &wEnd, &sa, m_pipeBufSize)) {
            log::error("[Rec] CreatePipe failed: {}", GetLastError());
            return false;
        }
        SetHandleInformation(wEnd, HANDLE_FLAG_INHERIT, 0);

        m_stderrLogPath = getOutputDir() + "\\~ffmpeg_log.txt";
        HANDLE stderrHandle = CreateFileA(
            m_stderrLogPath.c_str(), FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr
        );
        if (stderrHandle == INVALID_HANDLE_VALUE) {
            stderrHandle = CreateFileA(
                "NUL", GENERIC_WRITE, FILE_SHARE_WRITE | FILE_SHARE_READ,
                &sa, OPEN_EXISTING, 0, nullptr
            );
        } else {
            std::string sep = "\r\n=== Recording session ===\r\n";
            DWORD bw = 0;
            WriteFile(stderrHandle, sep.c_str(), (DWORD)sep.size(), &bw, nullptr);
        }

        HANDLE nul = CreateFileA(
            "NUL", GENERIC_WRITE, FILE_SHARE_WRITE | FILE_SHARE_READ,
            &sa, OPEN_EXISTING, 0, nullptr
        );

        // Use CreateProcessW for correct handling of Cyrillic/CJK device names
        PROCESS_INFORMATION pi{};
        BOOL ok = spawnProcessW(cmdStr, rEnd, nul, stderrHandle,
                                m_ffmpegPriority, &pi);

        CloseHandle(rEnd);

        if (!ok) {
            log::error("[Rec] CreateProcess failed: {}", GetLastError());
            CloseHandle(wEnd);
            CloseHandle(nul);
            CloseHandle(stderrHandle);
            return false;
        }

        CloseHandle(pi.hThread);
        m_pipe      = wEnd;
        m_proc      = pi.hProcess;
        m_nulHandle = nul;
        CloseHandle(stderrHandle);
        return true;
    }

    // Encoder fallback chain: try each HW encoder in order, then libx264.
    // QSV (Intel Quick Sync) is tried right after the primary encoder because
    // it uses D3D11 (not CUDA), so it works even when NVENC fails with
    // "Cannot load cuMemAllocAsync" on older CUDA drivers.
    bool startFFmpeg(const std::string& ffmpegExe,
                     int w, int h, int fps,
                     int encoderSetting, int crf,
                     const std::string& outPath,
                     bool addKeyframes, bool lowLatency,
                     bool micEnabled, const std::string& micDevice,
                     bool gameAudioEnabled, const std::string& gameAudioDevice,
                     std::string& outEncoderName,
                     int& actualFps) {
        actualFps = fps;
        std::string primary = pickEncoder(encoderSetting);

        // Build HW encoder list: primary first, then the others
        std::vector<std::string> hwChain;
        if (primary != "libx264") hwChain.push_back(primary);
        for (const char* fb : {"h264_qsv", "h264_nvenc", "h264_amf"}) {
            if (std::find(hwChain.begin(), hwChain.end(), fb) == hwChain.end())
                hwChain.push_back(fb);
        }

        // Cache encoder test results across recordings.
        // On GT 630M, each testEncoderWorks() call spawns FFmpeg and waits up to 10s,
        // causing a visible game freeze on every F5 press. Test once, cache forever.
        static std::unordered_map<std::string, bool> s_encoderCache;

        for (const auto& enc : hwChain) {
            bool works;
            auto it = s_encoderCache.find(enc);
            if (it != s_encoderCache.end()) {
                works = it->second;
                log::info("[Rec] Encoder {} (cached): {}", enc, works ? "OK" : "FAIL");
            } else {
                log::info("[Rec] Testing encoder: {}", enc);
                works = testEncoderWorks(ffmpegExe, enc, getOutputDir());
                s_encoderCache[enc] = works;
            }
            if (works) {
                std::string cmd = buildFFmpegCommand(
                    ffmpegExe, w, h, fps, enc, crf, outPath, addKeyframes, lowLatency,
                    micEnabled, micDevice, gameAudioEnabled, gameAudioDevice
                );
                log::info("[Rec] CMD: {}", cmd);
                if (spawnFFmpegProcess(cmd)) {
                    outEncoderName = enc;
                    log::info("[Rec] Using encoder: {}", enc);
                    return true;
                }
            } else {
                log::warn("[Rec] {} test failed, trying next encoder...", enc);
            }
        }

        // All HW encoders failed — fall back to libx264 (CPU encoding)
        log::warn("[Rec] All HW encoders failed, falling back to libx264. "
                  "CPU encoding will be used — may impact game performance.");
        // BELOW_NORMAL: game's GL thread keeps priority over FFmpeg/libx264.
        // The huge SW_PIPE_BUF_SIZE absorbs any scheduling gaps from lower priority.
        m_ffmpegPriority = BELOW_NORMAL_PRIORITY_CLASS;
        m_pipeBufSize = SW_PIPE_BUF_SIZE;
        m_maxQueue = SW_ENCODER_MAX_QUEUE;
        m_poolSize = m_maxQueue + 4;

        std::string cmd = buildFFmpegCommand(
            ffmpegExe, w, h, fps, "libx264", crf, outPath, addKeyframes, lowLatency,
            micEnabled, micDevice, gameAudioEnabled, gameAudioDevice
        );
        log::info("[Rec] CMD (fallback): {}", cmd);
        if (spawnFFmpegProcess(cmd)) {
            outEncoderName = "libx264";
            log::info("[Rec] Using encoder: libx264 (fallback, queue={}, pipe={}MB)",
                       m_maxQueue, m_pipeBufSize / (1024 * 1024));
            return true;
        }
        return false;
    }
};

// ==================================================================
// Remux + trim utilities
// ==================================================================

static bool remuxWithFaststart(const std::string& filePath) {
    std::string ffmpegExe = locateFFmpeg();
    if (ffmpegExe.empty()) return false;

    std::string tmpPath = filePath + ".faststart.tmp";
    std::ostringstream cmd;
    cmd << "\"" << ffmpegExe << "\""
        << " -y -hide_banner -loglevel error"
        << " -i \"" << filePath << "\""
        << " -c copy -movflags +faststart"
        << " \"" << tmpPath << "\"";

    std::string cmdStr = cmd.str();

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE nul = CreateFileA(
        "NUL", GENERIC_WRITE, FILE_SHARE_WRITE | FILE_SHARE_READ,
        &sa, OPEN_EXISTING, 0, nullptr
    );

    PROCESS_INFORMATION pi{};
    BOOL ok = spawnProcessW(cmdStr, nul, nul, nul, 0, &pi);
    if (!ok) { CloseHandle(nul); return false; }
    CloseHandle(pi.hThread);

    DWORD result = WaitForSingleObject(pi.hProcess, 120000);
    if (result == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 1);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(nul);

    std::error_code ec;
    if (exitCode == 0 && fs::exists(tmpPath, ec) && fs::file_size(tmpPath, ec) > 0) {
        fs::remove(filePath, ec);
        fs::rename(tmpPath, filePath, ec);
        if (!ec) return true;
    }
    fs::remove(tmpPath, ec);
    return false;
}

// ==================================================================
// ScreenRecorder (F5 — Normal Recording with dual audio tracks)
// ==================================================================

class ScreenRecorder : public RecordingPipeline {
public:
    static ScreenRecorder& get() {
        static ScreenRecorder s;
        return s;
    }

    bool start(int w, int h) {
        if (m_state != State::Idle) return false;

        readSettings();

        if (w <= 0 || h <= 0) return false;
        m_w = w; m_h = h;
        m_width = w; m_height = h;
        m_frameSize = w * h * 4;

        // Use NORMAL priority for FFmpeg. BELOW_NORMAL caused pipe backpressure
        // because FFmpeg couldn't drain the pipe fast enough, which blocked the
        // writer thread and starved the capture pool → game FPS drops.
        // HW encoders (NVENC/AMF/QSV) barely use CPU anyway, so NORMAL is safe.
        m_ffmpegPriority = NORMAL_PRIORITY_CLASS;
        m_pipeBufSize = PIPE_BUF_SIZE;
        m_maxQueue = DEFAULT_MAX_QUEUE;
        m_poolSize = m_maxQueue + 4;

        std::string ffmpeg = locateFFmpeg();
        if (ffmpeg.empty()) {
            Notification::create(
                "FFmpeg not found! Put ffmpeg.exe in the GD folder.",
                NotificationIcon::Error, 5.f
            )->show();
            return false;
        }

        createOutputDir();
        m_outPath = buildOutputPath("GD");

        // Resolve microphone device: supports "1"/"2" by index, exact name, or auto
        std::string micDev;
        if (m_micEnabled) {
            micDev = resolveDeviceName(m_micDevice, true);
            if (micDev.empty()) {
                log::warn("[Rec] Mic enabled but no device found!");
            } else {
                log::info("[Rec] Mic device: \"{}\"", micDev);
            }
        }

        std::string gameAudioDev;
        if (m_gameAudioEnabled) {
            if (g_wasapiLoopbackSupported) {
                // Render device (speakers/headphones) → WASAPI loopback
                gameAudioDev = resolveDeviceName(m_gameAudioDevice, false);
            } else {
                // Capture device (Stereo Mix) → dshow fallback
                if (m_gameAudioDevice.empty()) {
                    // Auto-detect: search capture devices for Stereo Mix
                    enumerateAudioDevices();
                    for (const auto& dev : g_captureDevices) {
                        std::string lo = dev;
                        for (auto& c : lo) if (c >= 'A' && c <= 'Z') c += 32;
                        if (lo.find("mix") != std::string::npos ||
                            dev.find("\xD0\xBC\xD0\xB8\xD0\xBA\xD1\x88\xD0\xB5\xD1\x80") != std::string::npos) {
                            gameAudioDev = dev;
                            break;
                        }
                    }
                } else {
                    gameAudioDev = resolveDeviceName(m_gameAudioDevice, true);
                }
            }
            if (gameAudioDev.empty()) {
                if (g_wasapiLoopbackSupported)
                    log::warn("[Rec] Game audio: speaker device not found. "
                              "Set game-audio-device to '1' or exact speaker name.");
                else
                    log::warn("[Rec] Game audio: Stereo Mix not found. "
                              "Enable it: Sound Settings → Recording tab → right-click → "
                              "Show Disabled Devices → Enable Stereo Mix. "
                              "Or update FFmpeg from gyan.dev for WASAPI loopback.");
            } else {
                log::info("[Rec] Game audio ({}) : \"{}\"",
                    g_wasapiLoopbackSupported ? "WASAPI loopback" : "dshow/StereoMix",
                    gameAudioDev);
            }
        }

        std::string encoderName;
        int actualFps = m_fps;
        if (!startFFmpeg(ffmpeg, w, h, m_fps, m_encoderSetting, qualityToCRF(m_quality),
                         m_outPath, false, false,
                         m_micEnabled, micDev,
                         m_gameAudioEnabled, gameAudioDev,
                         encoderName, actualFps)) {
            Notification::create(
                "Failed to start FFmpeg!",
                NotificationIcon::Error, 3.f
            )->show();
            return false;
        }

        // Update FPS if software encoder reduced it
        if (actualFps != m_fps) {
            log::info("[Rec] FPS adapted: {} -> {} (software encoder)", m_fps, actualFps);
            m_fps = actualFps;
            m_captureInterval = 1.0 / static_cast<double>(m_fps);
        }

        // Allocate pool AFTER encoder selection (software encoder increases queue size)
        allocatePool();

        m_encoderName = encoderName;

        // Warn user if using CPU encoder (major performance impact)
        if (encoderName == "libx264") {
            log::warn("[Rec] Using SOFTWARE encoder (libx264) — update GPU drivers for h264_nvenc!");
            Notification::create(
                "GPU encoder failed! Using CPU (libx264). "
                "Update NVIDIA drivers + FFmpeg for best performance!",
                NotificationIcon::Warning, 6.f
            )->show();
        }

        m_state       = State::Active;
        m_frames      = 0;
        m_dropped     = 0;
        m_duped       = 0;
        m_pipeError   = false;
        m_ffmpegOk    = false;
        m_startTime   = std::chrono::steady_clock::now();
        m_lastCapture = m_startTime;

        m_writerThread = std::thread([this] { writerLoop(); });

        std::string audioInfo;
        if (m_micEnabled && !micDev.empty()) audioInfo += " +MIC";
        if (m_gameAudioEnabled && !gameAudioDev.empty()) audioInfo += " +GAME";

        log::info("[Rec] Recording started ({}x{} @{}fps, encoder={}{})",
                  w, h, m_fps, encoderName, audioInfo);
        return true;
    }

    void stop() {
        if (m_state != State::Active) return;

        // Immediately set Saving — blocks new captures, returns GL thread fast
        m_state = State::Saving;
        m_cv.notify_all();

        // All heavy work on a background thread (pipe close, FFmpeg wait, notifications)
        std::thread([this] {
            // 1. Join writer thread (drains remaining frames)
            if (m_writerThread.joinable()) m_writerThread.join();

            // 2. Close pipe → FFmpeg gets EOF on video input
            if (m_pipe != INVALID_HANDLE_VALUE) {
                CloseHandle(m_pipe);
                m_pipe = INVALID_HANDLE_VALUE;
            }

            // 3. Wait for FFmpeg to finish (with -shortest, should exit in ~2s)
            bool ffmpegOk = false;
            if (m_proc != INVALID_HANDLE_VALUE) {
                DWORD result = WaitForSingleObject(m_proc, FFMPEG_WAIT_TIMEOUT);
                if (result == WAIT_TIMEOUT) {
                    log::warn("[Rec] FFmpeg timeout on stop, terminating");
                    TerminateProcess(m_proc, 1);
                    WaitForSingleObject(m_proc, 500);
                }
                DWORD exitCode = 0;
                GetExitCodeProcess(m_proc, &exitCode);
                ffmpegOk = (exitCode == 0 && !m_pipeError.load());
                if (!ffmpegOk) {
                    log::error("[Rec] FFmpeg exited with code: {} (pipe_error={})",
                               exitCode, m_pipeError.load());
                }
                CloseHandle(m_proc);
                m_proc = INVALID_HANDLE_VALUE;
            }

            if (m_nulHandle != INVALID_HANDLE_VALUE) {
                CloseHandle(m_nulHandle);
                m_nulHandle = INVALID_HANDLE_VALUE;
            }

            freePool();
            m_pipeError.store(false);

            int frames = m_frames.load();
            int dropped = m_dropped.load();
            int duped = m_duped.load();
            std::string outPath = m_outPath;
            std::string logPath = m_stderrLogPath;
            bool micOn = m_micEnabled;
            bool gameOn = m_gameAudioEnabled;

            // 4. Build notification and finalize on main thread
            Loader::get()->queueInMainThread([=] {
                if (!ffmpegOk) {
                    std::string fname = fs::path(outPath).filename().string();
                    std::error_code ec;
                    auto fsize = fs::file_size(outPath, ec);
                    if (ec || fsize == 0) {
                        fs::remove(outPath, ec);
                        Notification::create(
                            "Recording FAILED! Check ffmpeg_log.txt",
                            NotificationIcon::Error, 5.f
                        )->show();
                    } else {
                        Notification::create(
                            "Recording may be damaged: " + fname,
                            NotificationIcon::Warning, 5.f
                        )->show();
                    }
                } else {
                    std::string fname = fs::path(outPath).filename().string();
                    std::string desc = "Saved: " + fname;
                    bool hasAudio = micOn || gameOn;
                    if (hasAudio) {
                        desc += " [";
                        if (micOn) desc += "MIC";
                        if (micOn && gameOn) desc += "+";
                        if (gameOn) desc += "GAME";
                        desc += " audio]";
                    }
                    Notification::create(desc, NotificationIcon::Success, 5.f)->show();
                    std::error_code ec;
                    fs::remove(logPath, ec);
                }

                // Transition to Idle — recording system fully available again
                ScreenRecorder::get().m_state = State::Idle;
                log::info("[Rec] Recording stopped. Frames: {}, Dropped: {}, Duped: {}",
                          frames, dropped, duped);
            });
        }).detach();
    }

private:
    int m_w{}, m_h{};
    int m_quality{2};
    int m_encoderSetting{1};
    bool m_micEnabled{false};
    std::string m_micDevice;
    bool m_gameAudioEnabled{false};
    std::string m_gameAudioDevice;
    std::string m_encoderName;
    std::string m_outPath;

    void readSettings() {
        m_quality         = static_cast<int>(Mod::get()->getSettingValue<int64_t>("quality"));
        m_fps             = static_cast<int>(Mod::get()->getSettingValue<int64_t>("fps"));
        m_encoderSetting  = static_cast<int>(Mod::get()->getSettingValue<int64_t>("encoder"));
        m_captureInterval = 1.0 / static_cast<double>(m_fps);
        m_micEnabled      = Mod::get()->getSettingValue<bool>("mic-enabled");
        m_micDevice       = Mod::get()->getSettingValue<std::string>("mic-device");
        m_gameAudioEnabled = Mod::get()->getSettingValue<bool>("game-audio-enabled");
        m_gameAudioDevice  = Mod::get()->getSettingValue<std::string>("game-audio-device");
    }
};

// ==================================================================
// UI: Recording indicator (REC with timer)
// ==================================================================

class RecorderIndicator : public cocos2d::CCNode {
public:
    static RecorderIndicator* create() {
        auto* node = new RecorderIndicator();
        if (node && node->setup()) {
            node->autorelease();
            return node;
        }
        delete node;
        return nullptr;
    }

    bool setup() {
        if (!cocos2d::CCNode::init()) return false;

        constexpr float bgW = 90.f;
        auto* bg = cocos2d::CCLayerColor::create({0, 0, 0, 150}, bgW, 24.f);
        bg->setPosition({-bgW / 2.f, -12.f});
        addChild(bg, 0);

        m_label = cocos2d::CCLabelBMFont::create("REC", "bigFont.fnt");
        m_label->setScale(0.38f);
        m_label->setColor({255, 55, 55});
        m_label->setAnchorPoint({0.f, 0.5f});
        m_label->setPosition({-bgW / 2.f + 18.f, 0.f});
        addChild(m_label, 1);

        auto* dot = cocos2d::CCDrawNode::create();
        dot->drawDot({-bgW / 2.f + 8.f, 0.f}, 5.f, {1.f, 0.2f, 0.2f, 1.f});
        addChild(dot, 1);

        m_timerLabel = cocos2d::CCLabelBMFont::create("00:00", "bigFont.fnt");
        m_timerLabel->setScale(0.3f);
        m_timerLabel->setColor({200, 200, 200});
        m_timerLabel->setAnchorPoint({1.f, 0.5f});
        m_timerLabel->setPosition({bgW / 2.f - 4.f, 0.f});
        addChild(m_timerLabel, 1);

        scheduleUpdate();
        return true;
    }

    void update(float dt) override {
        m_time += dt;
        m_label->setVisible(fmodf(m_time, 1.0f) < 0.6f);

        double secs = ScreenRecorder::get().getElapsedSeconds();
        int m = static_cast<int>(secs) / 60;
        int s = static_cast<int>(secs) % 60;
        char buf[16];
        snprintf(buf, sizeof(buf), "%02d:%02d", m, s);
        m_timerLabel->setString(buf);
    }

private:
    cocos2d::CCLabelBMFont* m_label{};
    cocos2d::CCLabelBMFont* m_timerLabel{};
    float m_time{};
};
// ==================================================================
// Global UI state + hotkey handling
// ==================================================================

static RecorderIndicator* g_recIndicator = nullptr;
static bool g_f5Held  = false;
static bool g_f8Held  = false;
static bool g_f9Held  = false;
static bool g_f11Held = false;
static bool g_f12Held = false;
static bool g_indicatorHidden = false;
static int  g_frameCounter = 0;

static void attachRecIndicator() {
    if (g_indicatorHidden) return;
    if (!Mod::get()->getSettingValue<bool>("show-indicator")) return;
    auto* scene = cocos2d::CCDirector::sharedDirector()->getRunningScene();
    if (!scene) return;

    if (g_recIndicator) {
        if (g_recIndicator->getParent()) g_recIndicator->removeFromParent();
        g_recIndicator->release();
        g_recIndicator = nullptr;
    }

    g_recIndicator = RecorderIndicator::create();
    if (!g_recIndicator) return;
    g_recIndicator->retain();
    auto win = cocos2d::CCDirector::sharedDirector()->getWinSize();
    g_recIndicator->setPosition({win.width - 60.f, win.height - 18.f});
    scene->addChild(g_recIndicator, 9999);
}

static void detachRecIndicator() {
    if (g_recIndicator) {
        if (g_recIndicator->getParent()) g_recIndicator->removeFromParent();
        g_recIndicator->release();
        g_recIndicator = nullptr;
    }
}

// F8 = Cycle through microphone devices
static void onF8Pressed() {
    enumerateAudioDevices();
    auto& devices = g_captureDevices;
    if (devices.empty()) {
        Notification::create("No microphones found", NotificationIcon::Warning, 2.f)->show();
        return;
    }

    std::string current = Mod::get()->getSettingValue<std::string>("mic-device");
    int curIdx = -1;
    if (!current.empty() && std::all_of(current.begin(), current.end(),
        [](char c){ return std::isdigit((unsigned char)c); })) {
        curIdx = std::stoi(current) - 1;
    }

    int nextIdx = (curIdx + 1) % static_cast<int>(devices.size());
    std::string nextVal = std::to_string(nextIdx + 1);
    Mod::get()->setSettingValue<std::string>("mic-device", nextVal);

    std::string msg = "MIC [" + std::to_string(nextIdx + 1) + "/" +
                      std::to_string(devices.size()) + "]: " + devices[nextIdx];
    Notification::create(msg, NotificationIcon::Success, 3.f)->show();
    log::info("[Rec] Mic device set to {}: \"{}\"", nextIdx + 1, devices[nextIdx]);
}

// F9 = Cycle through game audio devices
static void onF9Pressed() {
    enumerateAudioDevices();
    auto& devices = g_wasapiLoopbackSupported ? g_renderDevices : g_captureDevices;
    std::string label = g_wasapiLoopbackSupported ? "AUDIO" : "STEREO MIX";
    if (devices.empty()) {
        Notification::create("No audio devices found", NotificationIcon::Warning, 2.f)->show();
        return;
    }

    std::string current = Mod::get()->getSettingValue<std::string>("game-audio-device");
    int curIdx = -1;
    if (!current.empty() && std::all_of(current.begin(), current.end(),
        [](char c){ return std::isdigit((unsigned char)c); })) {
        curIdx = std::stoi(current) - 1;
    }

    int nextIdx = (curIdx + 1) % static_cast<int>(devices.size());
    std::string nextVal = std::to_string(nextIdx + 1);
    Mod::get()->setSettingValue<std::string>("game-audio-device", nextVal);

    std::string msg = label + " [" + std::to_string(nextIdx + 1) + "/" +
                      std::to_string(devices.size()) + "]: " + devices[nextIdx];
    Notification::create(msg, NotificationIcon::Success, 3.f)->show();
    log::info("[Rec] Game audio device set to {}: \"{}\"", nextIdx + 1, devices[nextIdx]);
}

// F12 = Take screenshot (BMP)
static void takeScreenshot() {
    if (!g_capture.isInitialized()) {
        Notification::create("Screenshot failed: not initialized", NotificationIcon::Error, 2.f)->show();
        return;
    }

    int w = g_capture.width(), h = g_capture.height();
    int dataSize = w * h * 4;
    std::vector<uint8_t> pixels(dataSize);

    // Direct synchronous readback (no PBO delay for screenshots)
    glReadBuffer(GL_FRONT);
    glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, pixels.data());
    glReadBuffer(GL_BACK);

    // Build filename
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &t);
    char timeBuf[64];
    std::strftime(timeBuf, sizeof(timeBuf), "%Y%m%d_%H%M%S", &tm);
    createOutputDir();
    std::string path = getOutputDir() + "\\GD_" + timeBuf + ".bmp";

    // Write BMP (BGRA matches BMP native format, bottom-up matches OpenGL)
    int fileSize = 54 + dataSize;
    std::ofstream file(path, std::ios::binary);
    if (!file) return;

    // BMP file header (14 bytes)
    uint8_t bmpHdr[14] = {'B', 'M'};
    *reinterpret_cast<uint32_t*>(&bmpHdr[2])  = static_cast<uint32_t>(fileSize);
    *reinterpret_cast<uint32_t*>(&bmpHdr[10]) = 54;
    file.write(reinterpret_cast<char*>(bmpHdr), 14);

    // DIB header (40 bytes)
    uint8_t dibHdr[40] = {};
    *reinterpret_cast<uint32_t*>(&dibHdr[0])  = 40;
    *reinterpret_cast<int32_t*>(&dibHdr[4])   = w;
    *reinterpret_cast<int32_t*>(&dibHdr[8])   = h;
    *reinterpret_cast<uint16_t*>(&dibHdr[12]) = 1;
    *reinterpret_cast<uint16_t*>(&dibHdr[14]) = 32;
    *reinterpret_cast<uint32_t*>(&dibHdr[20]) = static_cast<uint32_t>(dataSize);
    file.write(reinterpret_cast<char*>(dibHdr), 40);

    file.write(reinterpret_cast<char*>(pixels.data()), dataSize);
    file.close();

    std::string fname = fs::path(path).filename().string();
    Notification::create("Screenshot: " + fname, NotificationIcon::Success, 3.f)->show();
    log::info("[Rec] Screenshot saved: {}", path);
}

static void onF5Pressed() {
    auto& rec = ScreenRecorder::get();

    if (rec.isSaving()) {
        Notification::create("Saving recording, please wait...", NotificationIcon::Loading, 2.f)->show();
        return;
    }

    if (rec.isActive()) {
        detachRecIndicator();
        rec.stop();
    } else if (rec.getState() == RecordingPipeline::State::Idle) {
        if (!g_capture.isInitialized()) return;
        if (rec.start(g_capture.width(), g_capture.height())) {
            attachRecIndicator();
            std::string msg = "Recording! F5=stop.";
            bool hasMic  = Mod::get()->getSettingValue<bool>("mic-enabled");
            bool hasGame = Mod::get()->getSettingValue<bool>("game-audio-enabled");
            if (hasMic || hasGame) {
                msg += " Audio:";
                if (hasMic)  msg += " MIC";
                if (hasGame) msg += " GAME";
            }
            Notification::create(msg, NotificationIcon::Loading, 3.f)->show();
        }
    }
}

// ==================================================================
// Hook: CCEGLView::swapBuffers
// ==================================================================

class $modify(RecorderEGLView, cocos2d::CCEGLView) {
    void swapBuffers() {
        auto& rec = ScreenRecorder::get();

        detectGPU();
        ++g_frameCounter;

        // Initialize shared frame capture on first frame
        if (!g_capture.isInitialized()) {
            g_capture.init();
        }

        // Periodic viewport re-check (every 120 frames)
        if (g_capture.isInitialized() && (g_frameCounter % 120 == 0)) {
            g_capture.reinitIfNeeded();
        }

        // Capture BEFORE swap: reads from GL_BACK (complete frame, no sync stall)
        if (rec.isActive() && g_capture.isInitialized()) {
            rec.captureDirectly(g_capture);
        }

        // Swap buffers (after capture)
        cocos2d::CCEGLView::swapBuffers();

        // Pipe error detection — auto-stop
        if (rec.isActive() && rec.hasPipeError()) {
            detachRecIndicator();
            rec.stop();
            log::error("[Rec] Auto-stopped due to pipe error");
        }

        // Re-attach indicator after scene changes (every 30 frames)
        if (!g_indicatorHidden && (g_frameCounter % 30 == 0)) {
            if (rec.isActive()) {
                if (g_recIndicator && !g_recIndicator->getParent()) {
                    g_recIndicator->release();
                    g_recIndicator = nullptr;
                }
                if (!g_recIndicator) attachRecIndicator();
            }
        }

        // ── Hotkeys ──
        bool f5Now = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
        if (f5Now && !g_f5Held) onF5Pressed();
        g_f5Held = f5Now;

        // F8 = Cycle microphone device
        bool f8Now = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
        if (f8Now && !g_f8Held) onF8Pressed();
        g_f8Held = f8Now;

        // F9 = Cycle game audio device
        bool f9Now = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        if (f9Now && !g_f9Held) onF9Pressed();
        g_f9Held = f9Now;

        // F11 = Toggle indicator visibility
        bool f11Now = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
        if (f11Now && !g_f11Held) {
            g_indicatorHidden = !g_indicatorHidden;
            if (g_indicatorHidden) {
                detachRecIndicator();
            } else {
                if (rec.isActive()) attachRecIndicator();
            }
        }
        g_f11Held = f11Now;

        // F12 = Screenshot (reads GL_FRONT after swap — correct for displayed frame)
        bool f12Now = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
        if (f12Now && !g_f12Held) takeScreenshot();
        g_f12Held = f12Now;
    }
};

// ==================================================================
// Mod init
// ==================================================================

$on_mod(Loaded) {
    log::info("=== GD Screen Recorder v7.0.0 (by JamStickGD) ===");
    log::info("F5  = Start/Stop recording");
    log::info("F8  = Cycle microphone device");
    log::info("F9  = Cycle game audio device");
    log::info("F11 = Toggle indicator");
    log::info("F12 = Screenshot");
    log::info("Devices can also be set by number (\"1\",\"2\") or name in mod settings.");

    std::error_code ec;
    std::string recDir = gdDir() + "\\recordings";
    fs::remove(recDir + "\\~ffmpeg_log.txt", ec);

    // Enumerate audio devices (fast COM API, no subprocess)
    logAudioDevices();

    // Probe WASAPI loopback support in current FFmpeg build
    std::string ffmpegPath = locateFFmpeg();
    if (!ffmpegPath.empty()) {
        g_wasapiLoopbackSupported = testWasapiLoopback(ffmpegPath);
        if (!g_wasapiLoopbackSupported) {
            log::warn("[Rec] To fix game audio: update FFmpeg → https://www.gyan.dev/ffmpeg/builds/");
            log::warn("[Rec] Or enable Stereo Mix: Sound Settings → Recording → right-click → Show Disabled Devices → Enable");
        }
    } else {
        log::warn("[Rec] ffmpeg.exe not found — place it in the GD folder.");
    }
}
