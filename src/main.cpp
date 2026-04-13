/*
 * GD Screen Recorder v1.0.0 - Release Edition
 * Developer: FreeOpus666
 *
 * Features:
 *  - F5  = Start/Stop recording
 *  - F8  = Cycle microphone device (shows current + selects next)
 *  - F12 = Screenshot (BMP)
 *  - F11 = Toggle indicator
 *  - Track 1 (default): Game + Mic mix, Track 2: Game Audio only, Track 3: Microphone only
 *  - Microphone captured separately via WASAPI and muxed as its own audio track
 *  - Both tracks in ONE .mp4 file - mute/unmute per track in any video player
 *  - PBO quad-buffer async GPU readback with fence sync (zero GL stall)
 *  - Hardware encoder auto-detection (NVENC/AMF/QSV) with libx264 fallback
 *  - Pre-warmed encoder cache at mod load (F5 starts instantly)
 *  - Unicode-safe FFmpeg paths (Cyrillic/CJK device names work)
 *  - Audio device auto-detection for microphones (Russian/CJK names supported)
 *
 * Performance:
 *  - Frame duplication: auto-fills timing gaps for perfectly smooth CFR output
 *  - Async stop: GL thread returns INSTANTLY on F5-stop (pipe closed before join)
 *  - Pool: 60 frames default, 40 for software encoding (reduced memory pressure)
 *  - Writer thread at ABOVE_NORMAL priority (NORMAL for SW) - drains queue fast
 *  - Software encoder: NORMAL FFmpeg priority + 32MB pipe buffer
 *  - Auto FPS reduction for libx264: 30fps at >720p, 45fps at <=720p
 *  - Max 4 dupes for software encoder (prevents CPU overload feedback loop)
 *  - Audio captured outside FFmpeg and muxed after stop (reduces live encode pressure)
 *  - Inline -movflags +faststart (no separate remux step)
 *  - Fast color conversion: -sws_flags fast_bilinear
 *  - FFmpeg wait timeout: 15s - safe margin for large recordings
 */

#include <Geode/Geode.hpp>
#include <Geode/modify/CCEGLView.hpp>
#include <Geode/binding/FMODAudioEngine.hpp>

#include <windows.h>
#include <winternl.h>
#include <GL/gl.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propsys.h>
#include <comdef.h>
#include <audioclient.h>
#include <audiopolicy.h>
#if __has_include(<audioclientactivationparams.h>)
#include <audioclientactivationparams.h>
#define GDSR_HAS_PROCESS_LOOPBACK 1
#else
#define GDSR_HAS_PROCESS_LOOPBACK 0
#endif

#pragma comment(lib, "mmdevapi.lib")
#pragma comment(lib, "Propsys.lib")

#include <atomic>
#include <chrono>
#include <cmath>
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
#include <new>
#include <cstdint>
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
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#endif
#ifndef GL_RENDERBUFFER
#define GL_RENDERBUFFER 0x8D41
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0 0x8CE0
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif
#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER 0x8CA8
#endif
#ifndef GL_DRAW_FRAMEBUFFER
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#endif
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif
#ifndef GL_MAP_READ_BIT
#define GL_MAP_READ_BIT 0x0001
#endif
#ifndef GL_MAP_PERSISTENT_BIT
#define GL_MAP_PERSISTENT_BIT 0x0040
#endif
#ifndef GL_MAP_COHERENT_BIT
#define GL_MAP_COHERENT_BIT 0x0080
#endif
#ifndef GL_CLIENT_STORAGE_BIT
#define GL_CLIENT_STORAGE_BIT 0x0200
#endif

// Native pixel format for glReadPixels: BGRA matches the BMP/FFmpeg rawvideo pipeline
#define CAPTURE_FORMAT GL_BGRA

// GL sync object constants (OpenGL 3.2 / ARB_sync)
#ifndef GL_SYNC_GPU_COMMANDS_COMPLETE
#define GL_SYNC_GPU_COMMANDS_COMPLETE  0x9117
#endif
#ifndef GL_SYNC_FLUSH_COMMANDS_BIT
#define GL_SYNC_FLUSH_COMMANDS_BIT     0x00000001
#endif
#ifndef GL_ALREADY_SIGNALED
#define GL_ALREADY_SIGNALED            0x911A
#endif
#ifndef GL_CONDITION_SATISFIED
#define GL_CONDITION_SATISFIED         0x911C
#endif
#ifndef GL_TIMEOUT_EXPIRED
#define GL_TIMEOUT_EXPIRED             0x911B
#endif
#ifndef GL_WAIT_FAILED
#define GL_WAIT_FAILED                 0x911D
#endif

typedef ptrdiff_t GLsizeiptr_t;
typedef ptrdiff_t GLintptr_t;

typedef void      (APIENTRY* PFN_glGenBuffers)(GLsizei, GLuint*);
typedef void      (APIENTRY* PFN_glDeleteBuffers)(GLsizei, const GLuint*);
typedef void      (APIENTRY* PFN_glBindBuffer)(GLenum, GLuint);
typedef void      (APIENTRY* PFN_glBufferData)(GLenum, GLsizeiptr_t, const void*, GLenum);
typedef void*     (APIENTRY* PFN_glMapBuffer)(GLenum, GLenum);
typedef GLboolean (APIENTRY* PFN_glUnmapBuffer)(GLenum);
typedef void      (APIENTRY* PFN_glBufferStorage)(GLenum, GLsizeiptr_t, const void*, GLbitfield);
typedef void*     (APIENTRY* PFN_glMapBufferRange)(GLenum, GLintptr_t, GLsizeiptr_t, GLbitfield);
typedef void      (APIENTRY* PFN_glGenFramebuffers)(GLsizei, GLuint*);
typedef void      (APIENTRY* PFN_glDeleteFramebuffers)(GLsizei, const GLuint*);
typedef void      (APIENTRY* PFN_glBindFramebuffer)(GLenum, GLuint);
typedef GLenum    (APIENTRY* PFN_glCheckFramebufferStatus)(GLenum);
typedef void      (APIENTRY* PFN_glGenRenderbuffers)(GLsizei, GLuint*);
typedef void      (APIENTRY* PFN_glDeleteRenderbuffers)(GLsizei, const GLuint*);
typedef void      (APIENTRY* PFN_glBindRenderbuffer)(GLenum, GLuint);
typedef void      (APIENTRY* PFN_glRenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
typedef void      (APIENTRY* PFN_glFramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
typedef void      (APIENTRY* PFN_glBlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);

static PFN_glGenBuffers    fnGenBuffers    = nullptr;
static PFN_glDeleteBuffers fnDeleteBuffers = nullptr;
static PFN_glBindBuffer    fnBindBuffer    = nullptr;
static PFN_glBufferData    fnBufferData    = nullptr;
static PFN_glMapBuffer     fnMapBuffer     = nullptr;
static PFN_glUnmapBuffer   fnUnmapBuffer   = nullptr;
static PFN_glBufferStorage fnBufferStorage = nullptr;
static PFN_glMapBufferRange fnMapBufferRange = nullptr;
static PFN_glGenFramebuffers        fnGenFramebuffers        = nullptr;
static PFN_glDeleteFramebuffers     fnDeleteFramebuffers     = nullptr;
static PFN_glBindFramebuffer        fnBindFramebuffer        = nullptr;
static PFN_glCheckFramebufferStatus fnCheckFramebufferStatus = nullptr;
static PFN_glGenRenderbuffers       fnGenRenderbuffers       = nullptr;
static PFN_glDeleteRenderbuffers    fnDeleteRenderbuffers    = nullptr;
static PFN_glBindRenderbuffer       fnBindRenderbuffer       = nullptr;
static PFN_glRenderbufferStorage    fnRenderbufferStorage    = nullptr;
static PFN_glFramebufferRenderbuffer fnFramebufferRenderbuffer = nullptr;
static PFN_glBlitFramebuffer        fnBlitFramebuffer        = nullptr;

// GL 3.2 sync objects - used for non-blocking PBO readback (prevents GL thread stalls).
// Use void* instead of GLsync to avoid dependency on glext.h / GLEW headers.
typedef void* (APIENTRY* PFN_glFenceSync)(GLenum condition, GLbitfield flags);
typedef GLenum (APIENTRY* PFN_glClientWaitSync)(void* sync, GLbitfield flags, GLuint64 timeout);
typedef void   (APIENTRY* PFN_glDeleteSync)(void* sync);

static PFN_glFenceSync       fnFenceSync       = nullptr;
static PFN_glClientWaitSync  fnClientWaitSync  = nullptr;
static PFN_glDeleteSync      fnDeleteSync      = nullptr;

static void* loadGLProc(const char* coreName, const char* extName = nullptr) {
    void* proc = reinterpret_cast<void*>(wglGetProcAddress(coreName));
    if (!proc && extName) proc = reinterpret_cast<void*>(wglGetProcAddress(extName));
    return proc;
}

static bool loadPBOFunctions() {
    static int cached = -1;
    if (cached >= 0) return cached == 1;

    fnGenBuffers    = (PFN_glGenBuffers)   wglGetProcAddress("glGenBuffers");
    fnDeleteBuffers = (PFN_glDeleteBuffers)wglGetProcAddress("glDeleteBuffers");
    fnBindBuffer    = (PFN_glBindBuffer)   wglGetProcAddress("glBindBuffer");
    fnBufferData    = (PFN_glBufferData)   wglGetProcAddress("glBufferData");
    fnMapBuffer     = (PFN_glMapBuffer)    wglGetProcAddress("glMapBuffer");
    fnUnmapBuffer   = (PFN_glUnmapBuffer)  wglGetProcAddress("glUnmapBuffer");
    fnBufferStorage = (PFN_glBufferStorage)loadGLProc("glBufferStorage", "glBufferStorageARB");
    fnMapBufferRange = (PFN_glMapBufferRange)loadGLProc("glMapBufferRange", "glMapBufferRangeARB");

    // Sync objects are optional: if unavailable, PBO map is used without fence check
    fnFenceSync      = (PFN_glFenceSync)      wglGetProcAddress("glFenceSync");
    fnClientWaitSync = (PFN_glClientWaitSync) wglGetProcAddress("glClientWaitSync");
    fnDeleteSync     = (PFN_glDeleteSync)     wglGetProcAddress("glDeleteSync");

    bool ok = fnGenBuffers && fnDeleteBuffers && fnBindBuffer &&
              fnBufferData && fnMapBuffer && fnUnmapBuffer;
    cached = ok ? 1 : 0;
    return ok;
}

static bool supportsPersistentPBO() {
    return fnBufferStorage && fnMapBufferRange;
}

static bool loadFramebufferFunctions() {
    static int cached = -1;
    if (cached >= 0) return cached == 1;

    fnGenFramebuffers = (PFN_glGenFramebuffers)loadGLProc("glGenFramebuffers", "glGenFramebuffersEXT");
    fnDeleteFramebuffers = (PFN_glDeleteFramebuffers)loadGLProc("glDeleteFramebuffers", "glDeleteFramebuffersEXT");
    fnBindFramebuffer = (PFN_glBindFramebuffer)loadGLProc("glBindFramebuffer", "glBindFramebufferEXT");
    fnCheckFramebufferStatus = (PFN_glCheckFramebufferStatus)loadGLProc("glCheckFramebufferStatus", "glCheckFramebufferStatusEXT");
    fnGenRenderbuffers = (PFN_glGenRenderbuffers)loadGLProc("glGenRenderbuffers", "glGenRenderbuffersEXT");
    fnDeleteRenderbuffers = (PFN_glDeleteRenderbuffers)loadGLProc("glDeleteRenderbuffers", "glDeleteRenderbuffersEXT");
    fnBindRenderbuffer = (PFN_glBindRenderbuffer)loadGLProc("glBindRenderbuffer", "glBindRenderbufferEXT");
    fnRenderbufferStorage = (PFN_glRenderbufferStorage)loadGLProc("glRenderbufferStorage", "glRenderbufferStorageEXT");
    fnFramebufferRenderbuffer = (PFN_glFramebufferRenderbuffer)loadGLProc("glFramebufferRenderbuffer", "glFramebufferRenderbufferEXT");
    fnBlitFramebuffer = (PFN_glBlitFramebuffer)loadGLProc("glBlitFramebuffer", "glBlitFramebufferEXT");

    bool ok = fnGenFramebuffers && fnDeleteFramebuffers && fnBindFramebuffer &&
              fnCheckFramebufferStatus && fnGenRenderbuffers && fnDeleteRenderbuffers &&
              fnBindRenderbuffer && fnRenderbufferStorage && fnFramebufferRenderbuffer &&
              fnBlitFramebuffer;
    cached = ok ? 1 : 0;
    return ok;
}

// ==================================================================
// Utility functions
// ==================================================================

static std::string utf16ToUtf8(const wchar_t* wstr) {
    if (!wstr) return "";
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return "";
    std::string out(size - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, out.empty() ? nullptr : &out[0], size, nullptr, nullptr);
    return out;
}

static std::string utf16ToUtf8(const std::wstring& wstr) {
    return utf16ToUtf8(wstr.c_str());
}

static std::wstring utf8ToUtf16(const std::string& utf8) {
    if (utf8.empty()) return L"";
    UINT cp = CP_UTF8;
    int len = MultiByteToWideChar(cp, 0, utf8.c_str(), -1, nullptr, 0);
    if (len <= 1) {
        // Fallback for old settings that might be in system ANSI codepage.
        cp = CP_ACP;
        len = MultiByteToWideChar(cp, 0, utf8.c_str(), -1, nullptr, 0);
        if (len <= 1) return L"";
    }
    std::wstring wide(len - 1, L'\0');
    MultiByteToWideChar(cp, 0, utf8.c_str(), -1, wide.empty() ? nullptr : &wide[0], len);
    return wide;
}

static fs::path utf8Path(const std::string& utf8) {
    return fs::path(utf8ToUtf16(utf8));
}

static int64_t steadyNowNanos() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

static int64_t steadyTimePointToNanos(std::chrono::steady_clock::time_point tp) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        tp.time_since_epoch()
    ).count();
}

static double computeLeadSeconds(int64_t videoStartNs, int64_t audioStartNs) {
    if (videoStartNs <= 0 || audioStartNs <= 0 || videoStartNs <= audioStartNs) return 0.0;
    double secs = static_cast<double>(videoStartNs - audioStartNs) / 1000000000.0;
    return std::clamp(secs, 0.0, 10.0);
}

static std::string gdDir() {
    wchar_t buf[MAX_PATH]{};
    DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return ".";
    return utf16ToUtf8(fs::path(buf).parent_path().wstring());
}

static std::string locateFFmpeg() {
    std::error_code ec;
    fs::path base = utf8Path(gdDir());

    std::vector<fs::path> candidates = {
        base / "ffmpeg.exe",
        base / "ffmpeg" / "bin" / "ffmpeg.exe",
        base / "geode" / "ffmpeg.exe",
        base / "geode" / "bin" / "ffmpeg.exe",
        base / "geode" / "tools" / "ffmpeg.exe"
    };

    for (const auto& p : candidates) {
        if (fs::exists(p, ec)) return utf16ToUtf8(p.wstring());
        ec.clear();
    }

    wchar_t buf[MAX_PATH]{};
    if (SearchPathW(nullptr, L"ffmpeg.exe", nullptr, MAX_PATH, buf, nullptr))
        return utf16ToUtf8(buf);
    return {};
}

static std::string getOutputDir() {
    std::string s = Mod::get()->getSettingValue<std::string>("output-dir");
    if (!s.empty()) return s;
    return gdDir() + "\\recordings";
}

static bool hasFmodGameAudioBackend();

static bool getBoolSetting(const char* key, bool fallback = false) {
    auto* mod = Mod::get();
    if (!mod) return fallback;
    return mod->getSettingValue<bool>(key);
}

static void createOutputDir() {
    std::error_code ec;
    fs::create_directories(utf8Path(getOutputDir()), ec);
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

static int qualityToMpeg4Q(int q) {
    switch (q) {
        case 1:  return 6;
        case 2:  return 5;
        case 3:  return 4;
        case 4:  return 3;
        default: return 5;
    }
}

static int normalizeGpuDownscaleMode(int mode) {
    if (mode < 0) return 0;
    if (mode > 5) return 5;
    return mode;
}

static void fitOutputToHeight(int srcW, int srcH, int targetH, int& outW, int& outH) {
    outW = srcW;
    outH = srcH;
    if (srcW <= 0 || srcH <= 0 || targetH <= 0 || srcH <= targetH) return;

    long long scaledW = (static_cast<long long>(srcW) * targetH) / std::max(1, srcH);
    outH = std::max(2, targetH & ~1);
    outW = std::max(2, static_cast<int>(scaledW) & ~1);
}

static void computeRequestedOutputSize(int srcW, int srcH, const std::string& encoder,
                                       int gpuDownscaleMode, int& outW, int& outH) {
    outW = srcW;
    outH = srcH;

    int mode = normalizeGpuDownscaleMode(gpuDownscaleMode);
    int targetH = 0;
    switch (mode) {
        case 1:
            targetH = (encoder == "libx264" || encoder == "libx265") ? 540 : 720;
            break;
        case 2: targetH = 720; break;
        case 3: targetH = 540; break;
        case 4: targetH = 480; break;
        case 5: targetH = 360; break;
        default: break;
    }

    fitOutputToHeight(srcW, srcH, targetH, outW, outH);
}

// ==================================================================
// GPU detection + encoder selection
// ==================================================================

static bool        g_gpuDetected = false;
static std::string g_gpuVendor;
static bool        g_gpuIsWeak = false;   // integrated/old GPU detected
static constexpr DWORD PROCESS_LOOPBACK_MIN_BUILD = 20348;

static DWORD getWindowsBuildNumber() {
    static DWORD cached = []() -> DWORD {
        auto* ntdll = GetModuleHandleW(L"ntdll.dll");
        if (!ntdll) return 0;

        using RtlGetVersionFn = LONG (WINAPI*)(PRTL_OSVERSIONINFOW);
        auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
        if (!fn) return 0;

        RTL_OSVERSIONINFOW ver{};
        ver.dwOSVersionInfoSize = sizeof(ver);
        if (fn(&ver) != 0) return 0;
        return ver.dwBuildNumber;
    }();
    return cached;
}

static bool isProcessLoopbackSupported() {
#if GDSR_HAS_PROCESS_LOOPBACK
    return getWindowsBuildNumber() >= PROCESS_LOOPBACK_MIN_BUILD;
#else
    return false;
#endif
}

static void tuneWorkerThread(int priority, int coreBiasFromEnd = 0) {
    SetThreadPriority(GetCurrentThread(), priority);
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    if (si.dwNumberOfProcessors > 4) {
        DWORD target = si.dwNumberOfProcessors - 1;
        if (coreBiasFromEnd > 0 && target >= static_cast<DWORD>(coreBiasFromEnd)) {
            target -= static_cast<DWORD>(coreBiasFromEnd);
        }
        SetThreadIdealProcessor(GetCurrentThread(), target);
    }
}

static void detectGPU() {
    if (g_gpuDetected) return;
    const char* v = (const char*)glGetString(GL_VENDOR);
    const char* r = (const char*)glGetString(GL_RENDERER);
    if (v) g_gpuVendor = v;
    g_gpuDetected = true;
    log::info("[Rec] GPU: {} ({})", v ? v : "?", r ? r : "?");

    // Detect weak/integrated GPUs for adaptive optimization
    std::string vendorLower = g_gpuVendor;
    std::string rendererLower = r ? r : "";
    for (auto& c : vendorLower) c = static_cast<char>(tolower(c));
    for (auto& c : rendererLower) c = static_cast<char>(tolower(c));

    // Intel integrated GPUs (HD/UHD/Iris Xe) - weak for encoding
    bool isIntel = vendorLower.find("intel") != std::string::npos;

    // Old NVIDIA mobile GPUs (GT/GS/M series, GeForce 100-900M)
    bool isOldNvidia = vendorLower.find("nvidia") != std::string::npos &&
        (rendererLower.find("gt ") != std::string::npos ||
         rendererLower.find("gs ") != std::string::npos ||
         rendererLower.find("m ") != std::string::npos ||
         rendererLower.find("microsoft") != std::string::npos);

    // AMD integrated (Radeon Vega Mobile, R3/R4/R5 integrated)
    bool isAmdIntegrated = vendorLower.find("amd") != std::string::npos &&
        (rendererLower.find("vega mobile") != std::string::npos ||
         rendererLower.find("radeon r3") != std::string::npos ||
         rendererLower.find("radeon r4") != std::string::npos ||
         rendererLower.find("radeon r5") != std::string::npos ||
         rendererLower.find("radeon r6") != std::string::npos ||
         rendererLower.find("radeon r7") != std::string::npos);

    // Microsoft Basic Render Adapter = no GPU at all
    bool isSoftwareRenderer = rendererLower.find("microsoft basic render") != std::string::npos ||
                              rendererLower.find("gdi") != std::string::npos;

    g_gpuIsWeak = isIntel || isOldNvidia || isAmdIntegrated || isSoftwareRenderer;
    if (g_gpuIsWeak) {
        log::info("[Rec] WEAK GPU detected - enabling adaptive optimizations");
    }
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
        case 6: return "mpeg4";
        case 7: return "libx265";
        default: return autoDetectEncoder();
    }
}

static bool isSoftwareEncoderName(const std::string& encoder) {
    return encoder == "libx264" || encoder == "libx265";
}

static bool isCpuRealtimeEncoderName(const std::string& encoder) {
    return encoder == "mpeg4" || isSoftwareEncoderName(encoder);
}

static std::string chooseSoftwareFallbackEncoder(const std::string& primary,
                                                 int requestedFps,
                                                 int width,
                                                 int height) {
    if (primary == "libx264" || primary == "mpeg4" || primary == "libx265") {
        return primary;
    }

    long long pixels = static_cast<long long>(std::max(width, 0)) *
                       static_cast<long long>(std::max(height, 0));
    if (g_gpuIsWeak || requestedFps >= 60 || pixels > 1280LL * 720LL) {
        return "mpeg4";
    }
    return "libx264";
}

// ==================================================================
// Global encoder cache - forward-declared here so buildEncoderArgs can use it.
// Populated by prewarmEncoderCache() on a background thread at mod load.
// ==================================================================

static std::mutex g_encoderCacheMtx;
static std::unordered_map<std::string, bool> g_encoderCache;
static std::atomic<bool> g_encoderCacheReady{false};

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
        // Check if VDBOX (fixed-function HW encoder) is available - 2-3x faster
        bool useLowPower = false;
        {
            std::lock_guard<std::mutex> lk(g_encoderCacheMtx);
            auto it = g_encoderCache.find("h264_qsv_lp");
            if (it != g_encoderCache.end()) useLowPower = it->second;
        }

        std::string args = " -c:v h264_qsv -preset veryfast";
        if (useLowPower) args += " -low_power 1";
        // -bf 0: disable B-frames (lower latency, faster encode, no reordering delay)
        // -refs 1: single reference frame (less memory, faster motion search)
        // -look_ahead 0: disable lookahead (instant frame-by-frame encode)
        // -async_depth 4: 4-frame pipeline depth (better GPU utilization)
        args += " -look_ahead 0 -bf 0 -refs 1 -async_depth 4";
        args += " -global_quality " + std::to_string(crf) + " -pix_fmt nv12";
        log::info("[Rec] QSV args: low_power={} preset=veryfast bf=0 refs=1 async=4",
                  useLowPower ? "ON (VDBOX)" : "OFF (ring)");
        return args;
    }
    // libx265: better compression but slower. Good for weak PCs that want smaller files.
    if (encoder == "libx265") {
        int x265threads = 1;
        {
            SYSTEM_INFO si; GetSystemInfo(&si);
            int cores = static_cast<int>(si.dwNumberOfProcessors);
            x265threads = std::max(1, cores / 2);
            if (x265threads > 4) x265threads = 4;
        }
        return " -c:v libx265 -preset ultrafast -tune zerolatency -crf " +
               std::to_string(crf + 5) + " -pix_fmt yuv420p -x265-params no-scenecut=1" +
               " -tag:v hvc1 -threads " + std::to_string(x265threads);
    }
    // mpeg4: MUCH faster than libx264 (no motion estimation overhead).
    // Best for weak PCs - records smoothly at higher FPS/resolution.
    // Downside: larger files, lower quality at same bitrate. Output in .avi or .mp4.
    if (encoder == "mpeg4") {
        int mpegthreads = 1;
        {
            SYSTEM_INFO si; GetSystemInfo(&si);
            int cores = static_cast<int>(si.dwNumberOfProcessors);
            mpegthreads = std::max(1, cores / 2);
            if (mpegthreads > 2) mpegthreads = 2;
        }
        int mpegQ = qualityToMpeg4Q((crf <= 16) ? 4 : (crf <= 19) ? 3 : (crf <= 24) ? 2 : 1);
        return " -c:v mpeg4 -q:v " + std::to_string(mpegQ) +
               " -qmin " + std::to_string(mpegQ) +
               " -qmax " + std::to_string(mpegQ) +
               " -pix_fmt yuv420p -threads " + std::to_string(mpegthreads);
    }
    // For libx264: limit threads to avoid starving the game of CPU.
    // On old hardware (GT 630M class with dual-core CPUs), -threads 0 uses
    // all cores for encoding, leaving nothing for GD -> massive lag.
    // We cap at max(1, logicalCores/2) so the game always has CPU headroom.
    int x264threads = 2;
    {
        SYSTEM_INFO si; GetSystemInfo(&si);
        int cores = static_cast<int>(si.dwNumberOfProcessors);
        if (g_gpuIsWeak) {
            x264threads = 1;
        } else {
            x264threads = std::max(1, cores / 2);
            if (x264threads > 4) x264threads = 4;
        }
    }
    std::string threadArg = " -threads " + std::to_string(x264threads);

    if (lowLatency) {
        return " -c:v libx264 -preset ultrafast -tune zerolatency -crf " +
               std::to_string(crf) + " -pix_fmt yuv420p -x264-params no-scenecut=1:sliced-threads=1:sync-lookahead=0" + threadArg;
    }
    return " -c:v libx264 -preset ultrafast -tune zerolatency -crf " +
           std::to_string(crf) + " -pix_fmt yuv420p -x264-params no-scenecut=1:sliced-threads=1:sync-lookahead=0" + threadArg;
}

// Convert UTF-8 string to wide (UTF-16) for CreateProcessW
static std::wstring utf8ToWide(const std::string& utf8) {
    return utf8ToUtf16(utf8);
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
                              const std::string& logDir,
                              const std::string& extraArgs = "") {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    std::string logPath = logDir + "\\~ffmpeg_log.txt";
    HANDLE stderrLog = CreateFileW(
        utf8ToWide(logPath).c_str(), FILE_APPEND_DATA,
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
        " -frames:v 1 -c:v " + encoder + extraArgs +
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

// ==================================================================
// Global encoder cache - pre-warmed at mod load on a background thread
// so that F5 never blocks the GL thread for encoder probing.
// (Variables declared above buildEncoderArgs to satisfy forward use.)
// ==================================================================

static void prewarmEncoderCache(std::string ffmpegExe, std::string logDir) {
    log::info("[Rec] === Background encoder pre-test starting ===");
    auto t0 = std::chrono::steady_clock::now();
    for (const char* enc : {"h264_nvenc", "h264_amf", "h264_qsv", "libx264", "mpeg4", "libx265"}) {
        auto t1 = std::chrono::steady_clock::now();
        bool works = testEncoderWorks(ffmpegExe, enc, logDir);
        double dt = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t1).count();
        {
            std::lock_guard<std::mutex> lk(g_encoderCacheMtx);
            g_encoderCache[enc] = works;
        }
        log::info("[Rec]   [{}] {} ({:.1f}s)", enc, works ? "OK" : "FAIL", dt);
    }

    // QSV low_power (VDBOX fixed-function) - 2-3x faster than ring/EU encode
    // Only available on Intel Skylake+ (6th gen). Test separately so we can
    // fall back to QSV without low_power on older Intel GPUs.
    bool qsvWorks = false;
    {
        std::lock_guard<std::mutex> lk(g_encoderCacheMtx);
        auto it = g_encoderCache.find("h264_qsv");
        if (it != g_encoderCache.end()) qsvWorks = it->second;
    }
    if (qsvWorks) {
        auto t1 = std::chrono::steady_clock::now();
        bool lpWorks = testEncoderWorks(ffmpegExe, "h264_qsv", logDir, " -low_power 1");
        double dt = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t1).count();
        {
            std::lock_guard<std::mutex> lk(g_encoderCacheMtx);
            g_encoderCache["h264_qsv_lp"] = lpWorks;
        }
        log::info("[Rec]   [h264_qsv low_power] {} ({:.1f}s)", lpWorks ? "OK" : "FAIL", dt);
    }
    double total = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();
    g_encoderCacheReady.store(true, std::memory_order_release);
    log::info("[Rec] === Encoder pre-test done in {:.1f}s (cache ready) ===", total);
}

enum class WasapiInputMode {
    None,
    LoopbackDevice,
    DefaultWithLoopbackFlag
};

static WasapiInputMode g_wasapiInputMode = WasapiInputMode::None;
static bool g_wasapiLoopbackSupported = false;

struct FFmpegProbeResult {
    bool spawned{false};
    DWORD exitCode{1};
    std::string output;
};

static std::string lowerAscii(std::string s) {
    for (auto& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    }
    return s;
}

static FFmpegProbeResult runFFmpegProbe(const std::string& ffmpegExe,
                                        const std::string& args,
                                        DWORD timeoutMs = 8000) {
    FFmpegProbeResult result;

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE pipeR = INVALID_HANDLE_VALUE;
    HANDLE pipeW = INVALID_HANDLE_VALUE;
    if (!CreatePipe(&pipeR, &pipeW, &sa, 0)) return result;
    SetHandleInformation(pipeR, HANDLE_FLAG_INHERIT, 0);

    HANDLE nul = CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    if (nul == INVALID_HANDLE_VALUE) {
        CloseHandle(pipeR);
        CloseHandle(pipeW);
        return result;
    }

    std::string cmd = "\"" + ffmpegExe + "\" " + args;
    PROCESS_INFORMATION pi{};
    BOOL ok = spawnProcessW(cmd, nul, pipeW, pipeW, 0, &pi);

    CloseHandle(pipeW);
    CloseHandle(nul);
    if (!ok) {
        CloseHandle(pipeR);
        return result;
    }

    result.spawned = true;
    CloseHandle(pi.hThread);

    DWORD waitRes = WaitForSingleObject(pi.hProcess, timeoutMs);
    if (waitRes == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 1000);
    }

    GetExitCodeProcess(pi.hProcess, &result.exitCode);
    CloseHandle(pi.hProcess);

    char buf[1024];
    DWORD br = 0;
    while (ReadFile(pipeR, buf, sizeof(buf), &br, nullptr) && br > 0) {
        result.output.append(buf, br);
    }
    CloseHandle(pipeR);

    return result;
}

static bool isLikelyWasapiRuntimeError(const std::string& outputLower) {
    return outputLower.find("cannot open") != std::string::npos ||
           outputLower.find("device not found") != std::string::npos ||
           outputLower.find("i/o error") != std::string::npos ||
           outputLower.find("no such") != std::string::npos ||
           outputLower.find("wasapi") != std::string::npos;
}

static bool isWasapiSyntaxError(const std::string& outputLower) {
    return outputLower.find("unknown input format") != std::string::npos ||
           outputLower.find("unrecognized option") != std::string::npos ||
           outputLower.find("option not found") != std::string::npos;
}

// Test if this FFmpeg build supports WASAPI loopback capture.
// Supports both known FFmpeg syntaxes:
//   1) -f wasapi -i loopback
//   2) -f wasapi -loopback 1 -i default
static bool testWasapiLoopback(const std::string& ffmpegExe) {
    g_wasapiInputMode = WasapiInputMode::None;

    auto devices = runFFmpegProbe(ffmpegExe, "-hide_banner -devices", 10000);
    if (!devices.spawned) {
        log::warn("[Rec] WASAPI probe failed: could not run ffmpeg -devices");
        return false;
    }

    std::string devicesLower = lowerAscii(devices.output);
    bool hasWasapiIndev = devicesLower.find("wasapi") != std::string::npos;
    if (!hasWasapiIndev) {
        log::error("[Rec] WASAPI input device is missing in this FFmpeg build");
        log::error("[Rec] Download full FFmpeg from: https://www.gyan.dev/ffmpeg/builds/");
        return false;
    }

    struct Probe {
        WasapiInputMode mode;
        const char* label;
        const char* args;
    };

    const Probe probes[] = {
        { WasapiInputMode::LoopbackDevice, "loopback-device",
          "-y -hide_banner -loglevel warning -f wasapi -thread_queue_size 64 -i loopback -t 1 -f null -" },
        { WasapiInputMode::DefaultWithLoopbackFlag, "default+loopback-flag",
          "-y -hide_banner -loglevel warning -f wasapi -thread_queue_size 64 -loopback 1 -i default -t 1 -f null -" }
    };

    for (const auto& p : probes) {
        auto probe = runFFmpegProbe(ffmpegExe, p.args, 9000);
        if (!probe.spawned) continue;

        std::string outLower = lowerAscii(probe.output);
        bool syntaxError = isWasapiSyntaxError(outLower);
        bool runtimeError = isLikelyWasapiRuntimeError(outLower);

        log::info("[Rec] WASAPI probe [{}]: exit={}, output ({}B): {}",
                  p.label, probe.exitCode, probe.output.size(),
                  probe.output.size() > 500 ? probe.output.substr(0, 500) + "..." : probe.output);

        // Accept WASAPI support if:
        // 1. exitCode == 0 (perfect - probe captured audio successfully)
        // 2. runtimeError && !syntaxError (FFmpeg understands WASAPI syntax,
        //    the device opened but the short test had issues - this is normal!
        //    The actual recording with longer duration and proper buffers works fine.)
        // REJECT only if syntaxError (FFmpeg doesn't have WASAPI support at all).
        if (probe.exitCode == 0 || (runtimeError && !syntaxError)) {
            g_wasapiInputMode = p.mode;
            log::info("[Rec] WASAPI loopback supported via mode: {} (exit={}, runtimeErr={})",
                      p.label, probe.exitCode, runtimeError);
            return true;
        }
        if (syntaxError) {
            log::warn("[Rec] WASAPI probe [{}]: SYNTAX error - this FFmpeg "
                      "doesn't support WASAPI at all.", p.label);
        }
    }

    log::warn("[Rec] WASAPI loopback probe failed for all syntaxes. "
              "This FFmpeg build doesn't support WASAPI loopback.");
    log::warn("[Rec] Download full FFmpeg from: https://www.gyan.dev/ffmpeg/builds/");
    log::warn("[Rec] The 'ffmpeg-full' build is required (essentials build lacks WASAPI).");
    return false;
}

// ==================================================================
// Audio device detection (Windows COM API + FFmpeg dshow)
// ==================================================================

// PKEY_Device_FriendlyName {a45c254e-df1c-4efd-8020-67d146a850e0}, 14
static const PROPERTYKEY PKEY_DeviceFriendlyName =
    {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

static std::string wcharToUtf8(const wchar_t* wstr) {
    return utf16ToUtf8(wstr);
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
// Audio device enumeration (Windows COM API - no FFmpeg subprocess!)
// ==================================================================

#ifndef DEVICE_STATE_ACTIVE
#define DEVICE_STATE_ACTIVE 0x00000001
#endif

static std::vector<std::string> g_captureDevices;  // microphones (UTF-8 names)
static std::vector<std::string> g_renderDevices;    // speakers/outputs (UTF-8 names)
static bool g_devicesEnumerated = false;

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

    log::info("[Rec] === Audio Output Devices (debug only; game audio no longer uses device loopback) ===");
    for (size_t i = 0; i < g_renderDevices.size(); i++) {
        log::info("[Rec]   {}: \"{}\"", i + 1, g_renderDevices[i]);
    }
    if (g_renderDevices.empty()) log::info("[Rec]   (none found)");

    std::string defMic = getDefaultAudioDeviceName(true);
    if (!defMic.empty()) log::info("[Rec] Default mic (auto): \"{}\"", defMic);

    if (hasFmodGameAudioBackend()) {
        log::info("[Rec] Game audio backend: FMOD master tap from GeometryDash.exe");
    } else if (isProcessLoopbackSupported()) {
        log::info("[Rec] Game audio fallback backend: process-only loopback from GeometryDash.exe");
    } else {
        log::warn("[Rec] Game audio backend unavailable: FMOD tap not ready and process loopback requires Windows build {}+ (current build={})",
                  PROCESS_LOOPBACK_MIN_BUILD, getWindowsBuildNumber());
    }

    log::info("[Rec] Tip: set mic-device to \"1\", \"2\" etc. or exact device name. Empty = auto.");
}

// Resolve device name from settings: "" = auto, "1"/"2" = by index, else = exact name
static std::string resolveDeviceName(const std::string& setting, bool isMicrophone) {
    if (setting.empty()) {
        std::string defName = getDefaultAudioDeviceName(isMicrophone);
        if (!defName.empty()) return defName;
        // Default device not found (rare) - fall back to first enumerated device
        enumerateAudioDevices();
        const auto& devices = isMicrophone ? g_captureDevices : g_renderDevices;
        if (!devices.empty()) {
            log::warn("[Rec] Default {} device not found via COM, using first available: \"{}\"",
                      isMicrophone ? "capture" : "render", devices[0]);
            return devices[0];
        }
        log::error("[Rec] No {} devices found at all!", isMicrophone ? "capture" : "render");
        return "";
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

static bool looksLikeStereoMixDevice(const std::string& name) {
    std::string lo = lowerAscii(name);
    if (lo.find("stereo mix") != std::string::npos) return true;
    if (lo.find("what u hear") != std::string::npos) return true;
    if (lo.find("wave out") != std::string::npos) return true;
    if (lo.find("loopback") != std::string::npos) return true;
    if (lo.find("stereo") != std::string::npos && lo.find("mix") != std::string::npos) return true;

    // Russian: "mixer", "stereo", "mixed" (in Russian: \xD0\xBC\xD0\xB8\xD0\xBA\xD1\x88\xD0\xB5\xD1\x80 etc.)
    if (name.find("\xD0\xBC\xD0\xB8\xD0\xBA\xD1\x88\xD0\xB5\xD1\x80") != std::string::npos) return true;
    if (name.find("\xD1\x81\xD1\x82\xD0\xB5\xD1\x80\xD0\xB5\xD0\xBE") != std::string::npos) return true;
    if (name.find("\xD1\x81\xD0\xBC\xD0\xB5\xD1\x88") != std::string::npos) return true;
    return false;
}

// ==================================================================
// Game Audio Capture via WASAPI process loopback (GeometryDash.exe only)
// Captures audio rendered by the current process tree, writes to a temp WAV,
// then FFmpeg muxes it into the final MP4 as a separate audio track.
// ==================================================================

#if GDSR_HAS_PROCESS_LOOPBACK
class ProcessLoopbackActivationHandler final : public IActivateAudioInterfaceCompletionHandler {
public:
    ProcessLoopbackActivationHandler()
        : m_done(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {}

    ~ProcessLoopbackActivationHandler() {
        if (m_audioClient) m_audioClient->Release();
        if (m_done) CloseHandle(m_done);
    }

    HRESULT wait(IAudioClient** outClient, DWORD timeoutMs) {
        if (!outClient) return E_POINTER;
        *outClient = nullptr;
        if (!m_done) return E_FAIL;

        DWORD waitRes = WaitForSingleObject(m_done, timeoutMs);
        if (waitRes != WAIT_OBJECT_0) {
            return HRESULT_FROM_WIN32(waitRes == WAIT_TIMEOUT ? WAIT_TIMEOUT : GetLastError());
        }
        if (FAILED(m_result)) return m_result;
        if (!m_audioClient) return E_FAIL;

        *outClient = m_audioClient;
        (*outClient)->AddRef();
        return S_OK;
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        *ppvObject = nullptr;
        if (riid == __uuidof(IUnknown) ||
            riid == __uuidof(IActivateAudioInterfaceCompletionHandler) ||
            riid == __uuidof(IAgileObject)) {
            *ppvObject = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        return static_cast<ULONG>(++m_refs);
    }

    STDMETHODIMP_(ULONG) Release() override {
        ULONG refs = static_cast<ULONG>(--m_refs);
        if (refs == 0) delete this;
        return refs;
    }

    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override {
        IUnknown* punkAudioInterface = nullptr;
        HRESULT hrActivateResult = E_UNEXPECTED;
        HRESULT hr = operation->GetActivateResult(&hrActivateResult, &punkAudioInterface);
        if (FAILED(hr)) {
            m_result = hr;
        } else if (FAILED(hrActivateResult)) {
            m_result = hrActivateResult;
        } else if (!punkAudioInterface) {
            m_result = E_POINTER;
        } else {
            m_result = punkAudioInterface->QueryInterface(__uuidof(IAudioClient), (void**)&m_audioClient);
        }

        if (punkAudioInterface) punkAudioInterface->Release();
        if (m_done) SetEvent(m_done);
        return S_OK;
    }

private:
    std::atomic<long> m_refs{1};
    HANDLE m_done{nullptr};
    HRESULT m_result{E_FAIL};
    IAudioClient* m_audioClient{nullptr};
};

static HRESULT activateProcessLoopbackAudioClient(DWORD targetProcessId, IAudioClient** outClient) {
    if (!outClient) return E_POINTER;
    *outClient = nullptr;

    AUDIOCLIENT_ACTIVATION_PARAMS activationParams{};
    activationParams.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    activationParams.ProcessLoopbackParams.TargetProcessId = targetProcessId;
    activationParams.ProcessLoopbackParams.ProcessLoopbackMode =
        PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

    PROPVARIANT activateParams{};
    activateParams.vt = VT_BLOB;
    activateParams.blob.cbSize = sizeof(activationParams);
    activateParams.blob.pBlobData = reinterpret_cast<BYTE*>(&activationParams);

    auto* handler = new (std::nothrow) ProcessLoopbackActivationHandler();
    if (!handler) return E_OUTOFMEMORY;

    IActivateAudioInterfaceAsyncOperation* asyncOp = nullptr;
    HRESULT hr = ActivateAudioInterfaceAsync(
        VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
        __uuidof(IAudioClient),
        &activateParams,
        handler,
        &asyncOp
    );
    if (asyncOp) asyncOp->Release();
    if (SUCCEEDED(hr)) hr = handler->wait(outClient, 5000);
    handler->Release();
    return hr;
}
#endif

static std::string getFriendlyDeviceName(IMMDevice* device) {
    if (!device) return {};

    IPropertyStore* props = nullptr;
    std::string result;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props)) && props) {
        PROPVARIANT name;
        PropVariantInit(&name);
        if (SUCCEEDED(props->GetValue(PKEY_DeviceFriendlyName, &name)) && name.vt == VT_LPWSTR && name.pwszVal) {
            result = utf16ToUtf8(name.pwszVal);
        }
        PropVariantClear(&name);
        props->Release();
    }
    return result;
}

static HRESULT activateCaptureAudioClientByName(const std::string& requestedName,
                                                IAudioClient** outClient,
                                                std::string* outResolvedName) {
    if (!outClient) return E_POINTER;
    *outClient = nullptr;
    if (outResolvedName) outResolvedName->clear();

    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (void**)&enumerator
    );
    if (FAILED(hr) || !enumerator) return FAILED(hr) ? hr : E_FAIL;

    IMMDevice* device = nullptr;
    if (requestedName.empty()) {
        hr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device);
    } else {
        IMMDeviceCollection* coll = nullptr;
        hr = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &coll);
        if (SUCCEEDED(hr) && coll) {
            UINT count = 0;
            coll->GetCount(&count);
            for (UINT i = 0; i < count; i++) {
                IMMDevice* candidate = nullptr;
                if (FAILED(coll->Item(i, &candidate)) || !candidate) continue;

                std::string candidateName = getFriendlyDeviceName(candidate);
                bool matches = (candidateName == requestedName);
                if (!matches && !candidateName.empty() && !requestedName.empty()) {
                    matches = (lowerAscii(candidateName) == lowerAscii(requestedName));
                }

                if (matches) {
                    device = candidate;
                    if (outResolvedName) *outResolvedName = candidateName;
                    break;
                }
                candidate->Release();
            }
            coll->Release();
        }
    }

    if (!device) {
        enumerator->Release();
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }

    if (outResolvedName && outResolvedName->empty()) {
        *outResolvedName = getFriendlyDeviceName(device);
    }

    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)outClient);
    device->Release();
    enumerator->Release();
    return hr;
}

static void writeWavHeaderForFormat(std::ofstream& f, const WAVEFORMATEX* format, uint32_t dataSize) {
    bool isExtensible = format && format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= 22;
    bool hasExtra = format && !isExtensible && format->cbSize > 0 &&
                    format->wFormatTag != WAVE_FORMAT_PCM &&
                    format->wFormatTag != WAVE_FORMAT_IEEE_FLOAT;
    uint32_t fmtSize = isExtensible ? static_cast<uint32_t>(sizeof(WAVEFORMATEXTENSIBLE))
                                    : (hasExtra ? static_cast<uint32_t>(18 + format->cbSize) : 16u);
    uint32_t fileSize = 4 + (8 + fmtSize) + (8 + dataSize);

    f.write("RIFF", 4);
    f.write(reinterpret_cast<const char*>(&fileSize), 4);
    f.write("WAVE", 4);
    f.write("fmt ", 4);
    f.write(reinterpret_cast<const char*>(&fmtSize), 4);

    if (isExtensible) {
        auto const* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE const*>(format);
        f.write(reinterpret_cast<const char*>(&ext->Format.wFormatTag), 2);
        f.write(reinterpret_cast<const char*>(&ext->Format.nChannels), 2);
        f.write(reinterpret_cast<const char*>(&ext->Format.nSamplesPerSec), 4);
        f.write(reinterpret_cast<const char*>(&ext->Format.nAvgBytesPerSec), 4);
        f.write(reinterpret_cast<const char*>(&ext->Format.nBlockAlign), 2);
        f.write(reinterpret_cast<const char*>(&ext->Format.wBitsPerSample), 2);
        f.write(reinterpret_cast<const char*>(&ext->Format.cbSize), 2);
        uint16_t validBits = ext->Samples.wValidBitsPerSample;
        f.write(reinterpret_cast<const char*>(&validBits), 2);
        f.write(reinterpret_cast<const char*>(&ext->dwChannelMask), 4);
        f.write(reinterpret_cast<const char*>(&ext->SubFormat), sizeof(GUID));
    } else {
        f.write(reinterpret_cast<const char*>(&format->wFormatTag), 2);
        f.write(reinterpret_cast<const char*>(&format->nChannels), 2);
        f.write(reinterpret_cast<const char*>(&format->nSamplesPerSec), 4);
        f.write(reinterpret_cast<const char*>(&format->nAvgBytesPerSec), 4);
        f.write(reinterpret_cast<const char*>(&format->nBlockAlign), 2);
        f.write(reinterpret_cast<const char*>(&format->wBitsPerSample), 2);
        if (hasExtra) {
            f.write(reinterpret_cast<const char*>(&format->cbSize), 2);
            f.write(reinterpret_cast<const char*>(format) + sizeof(WAVEFORMATEX), format->cbSize);
        }
    }

    f.write("data", 4);
    f.write(reinterpret_cast<const char*>(&dataSize), 4);
}

static bool wavHasAudioData(const std::string& wavPath) {
    std::error_code ec;
    return fs::exists(utf8Path(wavPath), ec) && fs::file_size(utf8Path(wavPath), ec) > 44;
}

static bool hasFmodGameAudioBackend() {
    auto* engine = FMODAudioEngine::sharedEngine();
    if (!engine) {
        log::warn("[Rec] FMOD: sharedEngine() returned null");
        return false;
    }
    if (!engine->m_system) {
        log::warn("[Rec] FMOD: m_system is null (engine not initialized yet?)");
        return false;
    }
    log::info("[Rec] FMOD: backend available (sampleRate={})", engine->m_sampleRate);
    return true;
}

class GameAudioCapture {
public:
    static GameAudioCapture& get() {
        static GameAudioCapture s;
        return s;
    }

    bool start(const std::string& wavPath) {
        if (m_running) return true;
        m_wavPath = wavPath;
        m_firstPacketNs.store(0, std::memory_order_release);
        m_running = true;
        m_thread = std::thread([this] { captureLoop(); });
        return true;
    }

    void stop() {
        m_running = false;
        signalWakeEvent();
        if (m_thread.joinable()) m_thread.join();
    }

    bool isRunning() const { return m_running; }
    int64_t firstPacketNs() const { return m_firstPacketNs.load(std::memory_order_acquire); }
    std::string wavPath() const { return m_wavPath; }

private:
    std::atomic<bool> m_running{false};
    std::atomic<int64_t> m_firstPacketNs{0};
    std::string m_wavPath;
    std::thread m_thread;
    std::mutex m_eventMtx;
    HANDLE m_sampleReadyEvent{nullptr};

    void markFirstPacketTimestamp() {
        int64_t expected = 0;
        m_firstPacketNs.compare_exchange_strong(
            expected, steadyNowNanos(), std::memory_order_release, std::memory_order_relaxed
        );
    }

    void signalWakeEvent() {
        std::lock_guard<std::mutex> lk(m_eventMtx);
        if (m_sampleReadyEvent) SetEvent(m_sampleReadyEvent);
    }

    void setWakeEvent(HANDLE eventHandle) {
        std::lock_guard<std::mutex> lk(m_eventMtx);
        m_sampleReadyEvent = eventHandle;
    }

    void clearWakeEvent(HANDLE eventHandle) {
        std::lock_guard<std::mutex> lk(m_eventMtx);
        if (m_sampleReadyEvent == eventHandle) m_sampleReadyEvent = nullptr;
    }

    static void writeWavHeader(std::ofstream& f, int channels, int sampleRate,
                               int bitsPerSample, int blockAlign,
                               int avgBytesPerSec, uint32_t dataSize) {
        uint32_t fileSize = 36 + dataSize;
        uint16_t formatTag = WAVE_FORMAT_PCM;
        uint32_t fmtSize = 16;
        uint16_t ch = static_cast<uint16_t>(channels);
        uint16_t bps = static_cast<uint16_t>(bitsPerSample);

        f.write("RIFF", 4);
        f.write(reinterpret_cast<const char*>(&fileSize), 4);
        f.write("WAVE", 4);
        f.write("fmt ", 4);
        f.write(reinterpret_cast<const char*>(&fmtSize), 4);
        f.write(reinterpret_cast<const char*>(&formatTag), 2);
        f.write(reinterpret_cast<const char*>(&ch), 2);
        f.write(reinterpret_cast<const char*>(&sampleRate), 4);
        f.write(reinterpret_cast<const char*>(&avgBytesPerSec), 4);
        f.write(reinterpret_cast<const char*>(&blockAlign), 2);
        f.write(reinterpret_cast<const char*>(&bps), 2);
        f.write("data", 4);
        f.write(reinterpret_cast<const char*>(&dataSize), 4);
    }

    void captureLoop() {
        tuneWorkerThread(THREAD_PRIORITY_NORMAL, 1);
        log::info("[Rec] GameAudioCapture: starting process-only loopback");

        if (!isProcessLoopbackSupported()) {
            log::error("[Rec] GameAudioCapture: process loopback unsupported on this system");
            m_running = false;
            return;
        }

        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool needUninit = SUCCEEDED(hr) || hr == S_FALSE;
        if (hr == RPC_E_CHANGED_MODE) needUninit = false;
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
            log::error("[Rec] GameAudioCapture: COM init failed: 0x{:X}", (unsigned)hr);
            m_running = false;
            return;
        }

        IAudioClient* audioClient = nullptr;
        IAudioCaptureClient* captureClient = nullptr;
        HANDLE sampleReadyEvent = nullptr;
        std::ofstream wavFile;
        uint32_t dataSize = 0;
        UINT64 totalFrames = 0;
        UINT64 silentPackets = 0;
        std::vector<char> zeroBuffer;

        auto cleanup = [&]() {
            clearWakeEvent(sampleReadyEvent);
            if (sampleReadyEvent) CloseHandle(sampleReadyEvent);
            if (captureClient) captureClient->Release();
            if (audioClient) audioClient->Release();
            if (needUninit) CoUninitialize();
            m_running = false;
            log::info("[Rec] GameAudioCapture: stopped");
        };

#if !GDSR_HAS_PROCESS_LOOPBACK
        log::error("[Rec] GameAudioCapture: build lacks process loopback headers");
        cleanup();
        return;
#else
        hr = activateProcessLoopbackAudioClient(GetCurrentProcessId(), &audioClient);
        if (FAILED(hr) || !audioClient) {
            log::error("[Rec] GameAudioCapture: ActivateAudioInterfaceAsync failed: 0x{:X}", (unsigned)hr);
            cleanup();
            return;
        }

        WAVEFORMATEX captureFormat{};
        captureFormat.wFormatTag = WAVE_FORMAT_PCM;
        captureFormat.nChannels = 2;
        captureFormat.nSamplesPerSec = 48000;
        captureFormat.wBitsPerSample = 16;
        captureFormat.nBlockAlign = captureFormat.nChannels * captureFormat.wBitsPerSample / 8;
        captureFormat.nAvgBytesPerSec = captureFormat.nSamplesPerSec * captureFormat.nBlockAlign;

        DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK |
                      AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                      AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                      AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 0, 0, &captureFormat, nullptr);
        if (FAILED(hr)) {
            flags &= ~AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
            hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 0, 0, &captureFormat, nullptr);
        }
        if (FAILED(hr)) {
            log::error("[Rec] GameAudioCapture: Initialize failed: 0x{:X}", (unsigned)hr);
            cleanup();
            return;
        }

        hr = audioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&captureClient);
        if (FAILED(hr) || !captureClient) {
            log::error("[Rec] GameAudioCapture: GetService(IAudioCaptureClient) failed: 0x{:X}", (unsigned)hr);
            cleanup();
            return;
        }

        sampleReadyEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!sampleReadyEvent) {
            log::error("[Rec] GameAudioCapture: CreateEvent failed: {}", GetLastError());
            cleanup();
            return;
        }
        setWakeEvent(sampleReadyEvent);

        hr = audioClient->SetEventHandle(sampleReadyEvent);
        if (FAILED(hr)) {
            log::error("[Rec] GameAudioCapture: SetEventHandle failed: 0x{:X}", (unsigned)hr);
            cleanup();
            return;
        }

        wavFile.open(utf8Path(m_wavPath), std::ios::binary);
        if (!wavFile) {
            log::error("[Rec] GameAudioCapture: cannot create WAV: {}", m_wavPath);
            cleanup();
            return;
        }
        writeWavHeader(
            wavFile,
            captureFormat.nChannels,
            captureFormat.nSamplesPerSec,
            captureFormat.wBitsPerSample,
            captureFormat.nBlockAlign,
            captureFormat.nAvgBytesPerSec,
            0
        );

        hr = audioClient->Start();
        if (FAILED(hr)) {
            log::error("[Rec] GameAudioCapture: Start failed: 0x{:X}", (unsigned)hr);
            wavFile.close();
            cleanup();
            return;
        }

        log::info("[Rec] GameAudioCapture: capturing GeometryDash.exe -> {}ch {}Hz {}bit PCM",
                  captureFormat.nChannels,
                  captureFormat.nSamplesPerSec,
                  captureFormat.wBitsPerSample);

        while (m_running) {
            DWORD waitRes = WaitForSingleObject(sampleReadyEvent, 250);
            if (!m_running) break;
            if (waitRes == WAIT_TIMEOUT) continue;
            if (waitRes != WAIT_OBJECT_0) {
                log::warn("[Rec] GameAudioCapture: wait failed: {}", GetLastError());
                break;
            }

            UINT32 packetLength = 0;
            hr = captureClient->GetNextPacketSize(&packetLength);
            if (FAILED(hr)) break;

            while (packetLength > 0 && m_running) {
                BYTE* pData = nullptr;
                UINT32 numFrames = 0;
                DWORD bufferFlags = 0;
                hr = captureClient->GetBuffer(&pData, &numFrames, &bufferFlags, nullptr, nullptr);
                if (FAILED(hr)) break;

                UINT32 bytesToWrite = numFrames * captureFormat.nBlockAlign;
                if (bytesToWrite > 0) {
                    markFirstPacketTimestamp();
                    if ((bufferFlags & AUDCLNT_BUFFERFLAGS_SILENT) || !pData) {
                        if (zeroBuffer.size() < bytesToWrite) zeroBuffer.resize(bytesToWrite, 0);
                        std::fill(zeroBuffer.begin(), zeroBuffer.begin() + bytesToWrite, 0);
                        wavFile.write(zeroBuffer.data(), bytesToWrite);
                        ++silentPackets;
                    } else {
                        wavFile.write(reinterpret_cast<const char*>(pData), bytesToWrite);
                    }
                    dataSize += bytesToWrite;
                    totalFrames += numFrames;
                }

                captureClient->ReleaseBuffer(numFrames);
                hr = captureClient->GetNextPacketSize(&packetLength);
                if (FAILED(hr)) break;
            }

            if (FAILED(hr)) break;
        }

        audioClient->Stop();
        wavFile.seekp(0);
        writeWavHeader(
            wavFile,
            captureFormat.nChannels,
            captureFormat.nSamplesPerSec,
            captureFormat.wBitsPerSample,
            captureFormat.nBlockAlign,
            captureFormat.nAvgBytesPerSec,
            dataSize
        );
        wavFile.close();

        double duration = captureFormat.nAvgBytesPerSec > 0
            ? static_cast<double>(dataSize) / captureFormat.nAvgBytesPerSec
            : 0.0;
        log::info("[Rec] GameAudioCapture: wrote {:.1f}s ({} bytes, {} silent packets, {} frames)",
                  duration, dataSize, silentPackets, totalFrames);
        cleanup();
#endif
    }
};

class FmodGameAudioCapture {
public:
    static FmodGameAudioCapture& get() {
        static FmodGameAudioCapture s;
        return s;
    }

    bool start(const std::string& wavPath) {
        if (m_running) return true;

        auto* engine = FMODAudioEngine::sharedEngine();
        if (!engine || !engine->m_system) {
            log::warn("[Rec] FmodGameAudioCapture: FMOD engine not ready");
            return false;
        }

        m_system = engine->m_system;
        m_wavPath = wavPath;
        m_dataSize = 0;
        m_droppedBlocks = 0;
        m_pendingBytes = 0;
        m_firstPacketNs.store(0, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            while (!m_chunks.empty()) m_chunks.pop();
            m_freeChunks.clear();
            m_freeChunks.reserve(CHUNK_POOL_LIMIT);
            for (size_t i = 0; i < CHUNK_POOL_LIMIT / 2; ++i) {
                std::vector<char> chunk;
                chunk.resize(INITIAL_CHUNK_BYTES);
                m_freeChunks.push_back(std::move(chunk));
            }
        }

        int sampleRate = engine->m_sampleRate;
        FMOD_SPEAKERMODE speakerMode = FMOD_SPEAKERMODE_STEREO;
        int rawSpeakers = 0;
        if (m_system->getSoftwareFormat(&sampleRate, &speakerMode, &rawSpeakers) != FMOD_OK || sampleRate <= 0) {
            sampleRate = 48000;
        }
        m_sampleRate = sampleRate;

        FMOD::ChannelGroup* masterGroup = nullptr;
        if (m_system->getMasterChannelGroup(&masterGroup) != FMOD_OK || !masterGroup) {
            log::error("[Rec] FmodGameAudioCapture: getMasterChannelGroup failed");
            m_system = nullptr;
            return false;
        }
        m_masterGroup = masterGroup;

        m_running = true;
        m_writerThread = std::thread([this] { writerLoop(); });

        FMOD_DSP_DESCRIPTION desc{};
        desc.pluginsdkversion = FMOD_PLUGIN_SDK_VERSION;
        std::strncpy(desc.name, "GDSR FMOD Tap", sizeof(desc.name) - 1);
        desc.version = 0x00010000;
        desc.numinputbuffers = 1;
        desc.numoutputbuffers = 1;
        desc.read = &FmodGameAudioCapture::dspRead;

        FMOD::DSP* dsp = nullptr;
        FMOD_RESULT res = m_system->createDSP(&desc, &dsp);
        if (res != FMOD_OK || !dsp) {
            log::error("[Rec] FmodGameAudioCapture: createDSP failed ({})", static_cast<int>(res));
            stopWriterOnly();
            m_system = nullptr;
            m_masterGroup = nullptr;
            return false;
        }

        dsp->setUserData(this);
        dsp->setMeteringEnabled(false, false);

        m_system->lockDSP();
        res = m_masterGroup->addDSP(0, dsp);
        m_system->unlockDSP();
        if (res != FMOD_OK) {
            log::error("[Rec] FmodGameAudioCapture: addDSP failed ({})", static_cast<int>(res));
            dsp->release();
            stopWriterOnly();
            m_system = nullptr;
            m_masterGroup = nullptr;
            return false;
        }

        m_dsp = dsp;
        log::info("[Rec] FmodGameAudioCapture: attached FMOD master tap at {} Hz", m_sampleRate);
        return true;
    }

    void stop() {
        if (!m_running && !m_dsp) return;

        m_running = false;

        if (m_system && m_masterGroup && m_dsp) {
            m_system->lockDSP();
            m_masterGroup->removeDSP(m_dsp);
            m_system->unlockDSP();
        }
        if (m_dsp) {
            m_dsp->setUserData(nullptr);
            m_dsp->release();
            m_dsp = nullptr;
        }

        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_cv.notify_all();
        }
        if (m_writerThread.joinable()) m_writerThread.join();

        m_masterGroup = nullptr;
        m_system = nullptr;
        log::info("[Rec] FmodGameAudioCapture: stopped");
    }

    bool isRunning() const { return m_running; }
    int64_t firstPacketNs() const { return m_firstPacketNs.load(std::memory_order_acquire); }

private:
    static constexpr size_t MAX_PENDING_BYTES = 8 * 1024 * 1024;
    static constexpr size_t CHUNK_POOL_LIMIT = 24;
    static constexpr size_t INITIAL_CHUNK_BYTES = 4096 * 2 * sizeof(float);

    void markFirstPacketTimestamp() {
        int64_t expected = 0;
        m_firstPacketNs.compare_exchange_strong(
            expected, steadyNowNanos(), std::memory_order_release, std::memory_order_relaxed
        );
    }

    static FMOD_RESULT F_CALL dspRead(FMOD_DSP_STATE* dspState, float* inbuffer, float* outbuffer,
                                      unsigned int length, int inchannels, int* outchannels) {
        if (outchannels) *outchannels = inchannels;
        if (outbuffer && inbuffer && length > 0 && inchannels > 0) {
            std::memcpy(outbuffer, inbuffer, static_cast<size_t>(length) * inchannels * sizeof(float));
        }

        if (!dspState || !dspState->functions || !dspState->functions->getuserdata ||
            !inbuffer || length == 0 || inchannels <= 0) {
            return FMOD_OK;
        }

        void* userData = nullptr;
        if (dspState->functions->getuserdata(dspState, &userData) != FMOD_OK || !userData) {
            return FMOD_OK;
        }

        static_cast<FmodGameAudioCapture*>(userData)->pushSamples(inbuffer, length, inchannels);
        return FMOD_OK;
    }

    void pushSamples(const float* input, unsigned int frames, int inchannels) {
        if (!m_running || !input || frames == 0 || inchannels <= 0) return;
        markFirstPacketTimestamp();

        std::vector<char> chunk;
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            if (!m_freeChunks.empty()) {
                chunk = std::move(m_freeChunks.back());
                m_freeChunks.pop_back();
            }
        }
        chunk.resize(static_cast<size_t>(frames) * 2 * sizeof(float));
        float* dst = reinterpret_cast<float*>(chunk.data());

        for (unsigned int i = 0; i < frames; ++i) {
            const float* src = input + static_cast<size_t>(i) * inchannels;
            float left = src[0];
            float right = (inchannels >= 2) ? src[1] : src[0];
            dst[i * 2 + 0] = left;
            dst[i * 2 + 1] = right;
        }

        std::lock_guard<std::mutex> lk(m_mtx);
        if (m_pendingBytes + chunk.size() > MAX_PENDING_BYTES) {
            if (m_freeChunks.size() < CHUNK_POOL_LIMIT) m_freeChunks.push_back(std::move(chunk));
            ++m_droppedBlocks;
            return;
        }
        m_pendingBytes += chunk.size();
        m_chunks.push(std::move(chunk));
        m_cv.notify_one();
    }

    void writerLoop() {
        tuneWorkerThread(THREAD_PRIORITY_BELOW_NORMAL, 1);
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        format.nChannels = 2;
        format.nSamplesPerSec = m_sampleRate > 0 ? m_sampleRate : 48000;
        format.wBitsPerSample = 32;
        format.nBlockAlign = format.nChannels * (format.wBitsPerSample / 8);
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

        std::ofstream wavFile(utf8Path(m_wavPath), std::ios::binary);
        if (!wavFile) {
            log::error("[Rec] FmodGameAudioCapture: cannot create WAV: {}", m_wavPath);
            return;
        }

        writeWavHeaderForFormat(wavFile, &format, 0);

        while (true) {
            std::vector<char> chunk;
            {
                std::unique_lock<std::mutex> lk(m_mtx);
                m_cv.wait(lk, [this] { return !m_chunks.empty() || !m_running; });
                if (m_chunks.empty()) {
                    if (!m_running) break;
                    continue;
                }
                chunk = std::move(m_chunks.front());
                m_chunks.pop();
                if (m_pendingBytes >= chunk.size()) m_pendingBytes -= chunk.size();
            }

            if (!chunk.empty()) {
                wavFile.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
                m_dataSize += static_cast<uint32_t>(chunk.size());
            }

            {
                std::lock_guard<std::mutex> lk(m_mtx);
                if (m_freeChunks.size() < CHUNK_POOL_LIMIT) {
                    m_freeChunks.push_back(std::move(chunk));
                }
            }
        }

        wavFile.seekp(0);
        writeWavHeaderForFormat(wavFile, &format, m_dataSize);
        wavFile.close();

        double duration = format.nAvgBytesPerSec > 0
            ? static_cast<double>(m_dataSize) / format.nAvgBytesPerSec
            : 0.0;
        log::info("[Rec] FmodGameAudioCapture: wrote {:.1f}s ({} bytes, dropped blocks={})",
                  duration, m_dataSize, m_droppedBlocks.load());
    }

    void stopWriterOnly() {
        m_running = false;
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_cv.notify_all();
        }
        if (m_writerThread.joinable()) m_writerThread.join();
    }

    std::atomic<bool> m_running{false};
    std::atomic<int> m_droppedBlocks{0};
    std::string m_wavPath;
    std::thread m_writerThread;
    std::mutex m_mtx;
    std::condition_variable m_cv;
    std::queue<std::vector<char>> m_chunks;
    std::vector<std::vector<char>> m_freeChunks;
    size_t m_pendingBytes{0};
    uint32_t m_dataSize{0};
    std::atomic<int64_t> m_firstPacketNs{0};
    int m_sampleRate{48000};
    FMOD::System* m_system{nullptr};
    FMOD::ChannelGroup* m_masterGroup{nullptr};
    FMOD::DSP* m_dsp{nullptr};
};

class MicrophoneCapture {
public:
    static MicrophoneCapture& get() {
        static MicrophoneCapture s;
        return s;
    }

    bool start(const std::string& wavPath, const std::string& deviceName) {
        if (m_running) return true;
        m_wavPath = wavPath;
        m_deviceName = deviceName;
        m_firstPacketNs.store(0, std::memory_order_release);
        m_running = true;
        m_thread = std::thread([this] { captureLoop(); });
        Sleep(80);
        return m_running.load();
    }

    void stop() {
        m_running = false;
        signalWakeEvent();
        if (m_thread.joinable()) m_thread.join();
    }

    bool isRunning() const { return m_running; }
    int64_t firstPacketNs() const { return m_firstPacketNs.load(std::memory_order_acquire); }

private:
    std::atomic<bool> m_running{false};
    std::atomic<int64_t> m_firstPacketNs{0};
    std::string m_wavPath;
    std::string m_deviceName;
    std::thread m_thread;
    std::mutex m_eventMtx;
    HANDLE m_sampleReadyEvent{nullptr};

    void markFirstPacketTimestamp() {
        int64_t expected = 0;
        m_firstPacketNs.compare_exchange_strong(
            expected, steadyNowNanos(), std::memory_order_release, std::memory_order_relaxed
        );
    }

    void signalWakeEvent() {
        std::lock_guard<std::mutex> lk(m_eventMtx);
        if (m_sampleReadyEvent) SetEvent(m_sampleReadyEvent);
    }

    void setWakeEvent(HANDLE eventHandle) {
        std::lock_guard<std::mutex> lk(m_eventMtx);
        m_sampleReadyEvent = eventHandle;
    }

    void clearWakeEvent(HANDLE eventHandle) {
        std::lock_guard<std::mutex> lk(m_eventMtx);
        if (m_sampleReadyEvent == eventHandle) m_sampleReadyEvent = nullptr;
    }

    void captureLoop() {
        tuneWorkerThread(THREAD_PRIORITY_NORMAL, 1);
        log::info("[Rec] MicrophoneCapture: starting WASAPI capture");

        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool needUninit = SUCCEEDED(hr) || hr == S_FALSE;
        if (hr == RPC_E_CHANGED_MODE) needUninit = false;
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
            log::error("[Rec] MicrophoneCapture: COM init failed: 0x{:X}", (unsigned)hr);
            m_running = false;
            return;
        }

        IAudioClient* audioClient = nullptr;
        IAudioCaptureClient* captureClient = nullptr;
        WAVEFORMATEX* mixFormat = nullptr;
        HANDLE sampleReadyEvent = nullptr;
        std::ofstream wavFile;
        uint32_t dataSize = 0;
        UINT64 totalFrames = 0;
        UINT64 silentPackets = 0;
        std::vector<char> zeroBuffer;
        std::string resolvedDeviceName;

        auto cleanup = [&]() {
            clearWakeEvent(sampleReadyEvent);
            if (sampleReadyEvent) CloseHandle(sampleReadyEvent);
            if (mixFormat) CoTaskMemFree(mixFormat);
            if (captureClient) captureClient->Release();
            if (audioClient) audioClient->Release();
            if (needUninit) CoUninitialize();
            m_running = false;
            log::info("[Rec] MicrophoneCapture: stopped");
        };

        hr = activateCaptureAudioClientByName(m_deviceName, &audioClient, &resolvedDeviceName);
        if (FAILED(hr) || !audioClient) {
            log::error("[Rec] MicrophoneCapture: failed to open microphone \"{}\": 0x{:X}",
                       m_deviceName, (unsigned)hr);
            cleanup();
            return;
        }

        hr = audioClient->GetMixFormat(&mixFormat);
        if (FAILED(hr) || !mixFormat) {
            log::error("[Rec] MicrophoneCapture: GetMixFormat failed: 0x{:X}", (unsigned)hr);
            cleanup();
            return;
        }

        DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
        hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 0, 0, mixFormat, nullptr);
        if (FAILED(hr)) {
            log::error("[Rec] MicrophoneCapture: Initialize failed: 0x{:X}", (unsigned)hr);
            cleanup();
            return;
        }

        hr = audioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&captureClient);
        if (FAILED(hr) || !captureClient) {
            log::error("[Rec] MicrophoneCapture: GetService(IAudioCaptureClient) failed: 0x{:X}", (unsigned)hr);
            cleanup();
            return;
        }

        sampleReadyEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!sampleReadyEvent) {
            log::error("[Rec] MicrophoneCapture: CreateEvent failed: {}", GetLastError());
            cleanup();
            return;
        }
        setWakeEvent(sampleReadyEvent);

        hr = audioClient->SetEventHandle(sampleReadyEvent);
        if (FAILED(hr)) {
            log::error("[Rec] MicrophoneCapture: SetEventHandle failed: 0x{:X}", (unsigned)hr);
            cleanup();
            return;
        }

        wavFile.open(utf8Path(m_wavPath), std::ios::binary);
        if (!wavFile) {
            log::error("[Rec] MicrophoneCapture: cannot create WAV: {}", m_wavPath);
            cleanup();
            return;
        }
        writeWavHeaderForFormat(wavFile, mixFormat, 0);

        hr = audioClient->Start();
        if (FAILED(hr)) {
            log::error("[Rec] MicrophoneCapture: Start failed: 0x{:X}", (unsigned)hr);
            wavFile.close();
            cleanup();
            return;
        }

        log::info("[Rec] MicrophoneCapture: capturing \"{}\" -> {}ch {}Hz {}bit tag={} cbSize={}",
                  resolvedDeviceName.empty() ? m_deviceName : resolvedDeviceName,
                  mixFormat->nChannels,
                  mixFormat->nSamplesPerSec,
                  mixFormat->wBitsPerSample,
                  mixFormat->wFormatTag,
                  mixFormat->cbSize);

        while (m_running) {
            DWORD waitRes = WaitForSingleObject(sampleReadyEvent, 250);
            if (!m_running) break;
            if (waitRes == WAIT_TIMEOUT) continue;
            if (waitRes != WAIT_OBJECT_0) {
                log::warn("[Rec] MicrophoneCapture: wait failed: {}", GetLastError());
                break;
            }

            UINT32 packetLength = 0;
            hr = captureClient->GetNextPacketSize(&packetLength);
            if (FAILED(hr)) break;

            while (packetLength > 0 && m_running) {
                BYTE* pData = nullptr;
                UINT32 numFrames = 0;
                DWORD bufferFlags = 0;
                hr = captureClient->GetBuffer(&pData, &numFrames, &bufferFlags, nullptr, nullptr);
                if (FAILED(hr)) break;

                UINT32 bytesToWrite = numFrames * mixFormat->nBlockAlign;
                if (bytesToWrite > 0) {
                    markFirstPacketTimestamp();
                    if ((bufferFlags & AUDCLNT_BUFFERFLAGS_SILENT) || !pData) {
                        if (zeroBuffer.size() < bytesToWrite) zeroBuffer.resize(bytesToWrite, 0);
                        std::fill(zeroBuffer.begin(), zeroBuffer.begin() + bytesToWrite, 0);
                        wavFile.write(zeroBuffer.data(), bytesToWrite);
                        ++silentPackets;
                    } else {
                        wavFile.write(reinterpret_cast<const char*>(pData), bytesToWrite);
                    }
                    dataSize += bytesToWrite;
                    totalFrames += numFrames;
                }

                captureClient->ReleaseBuffer(numFrames);
                hr = captureClient->GetNextPacketSize(&packetLength);
                if (FAILED(hr)) break;
            }

            if (FAILED(hr)) break;
        }

        audioClient->Stop();
        wavFile.seekp(0);
        writeWavHeaderForFormat(wavFile, mixFormat, dataSize);
        wavFile.close();

        double duration = mixFormat->nAvgBytesPerSec > 0
            ? static_cast<double>(dataSize) / mixFormat->nAvgBytesPerSec
            : 0.0;
        log::info("[Rec] MicrophoneCapture: wrote {:.1f}s ({} bytes, {} silent packets, {} frames)",
                  duration, dataSize, silentPackets, totalFrames);
        cleanup();
    }
};

// ==================================================================
// Shared Frame Capture (PBO + fallback, single capture per frame)
// ==================================================================

class SharedFrameCapture {
public:
    bool isInitialized() const { return m_initialized; }
    int  width()     const { return m_srcW; }
    int  height()    const { return m_srcH; }
    int  frameSize() const { return m_frameSize; }

    static bool supportsGpuDownscale() {
        return loadFramebufferFunctions();
    }

    void init() {
        GLint vp[4];
        glGetIntegerv(GL_VIEWPORT, vp);
        m_srcVpX = vp[0];
        m_srcVpY = vp[1];
        m_srcW   = vp[2];
        m_srcH   = vp[3];
        if (m_srcW % 2) m_srcW--;
        if (m_srcH % 2) m_srcH--;
        if (m_srcW <= 0 || m_srcH <= 0) return;

        // Drain any stale GL errors (e.g. from conflicting mods)
        while (glGetError() != GL_NO_ERROR) {}

        initBuffers();
    }

    bool configureCaptureSize(int requestedW, int requestedH) {
        int normalizedW = 0;
        int normalizedH = 0;
        if (requestedW <= 0 || requestedH <= 0) {
            normalizedW = 0;
            normalizedH = 0;
        } else {
            normalizedW = requestedW & ~1;
            normalizedH = requestedH & ~1;
        }

        if (m_requestedW == normalizedW && m_requestedH == normalizedH && m_initialized) {
            return (m_captureW == (normalizedW > 0 ? normalizedW : m_srcW) &&
                    m_captureH == (normalizedH > 0 ? normalizedH : m_srcH));
        }

        m_requestedW = normalizedW;
        m_requestedH = normalizedH;

        if (!m_initialized) return true;

        destroyBuffers();
        initBuffers();
        bool usingRequested = (m_requestedW > 0 && m_requestedH > 0);
        return !usingRequested || (m_captureW == m_requestedW && m_captureH == m_requestedH);
    }

    void resetCaptureSize() {
        configureCaptureSize(0, 0);
    }

    int captureWidth() const { return m_captureW; }
    int captureHeight() const { return m_captureH; }
    bool isGpuFlipped() const { return m_useScaleFBO; }

    // Zero-copy: PBO maps directly into caller's buffer, skipping intermediate copy
    bool captureInto(uint8_t* dst, int dstSize) {
        if (!m_initialized || !dst || dstSize <= 0) return false;
        if (m_captureW <= 0 || m_captureH <= 0 || m_frameSize <= 0) return false;

        if (!prepareReadSource()) return false;

        if (!m_usePBO) {
            if (dstSize < m_frameSize) {
                restoreReadSource();
                return false;
            }
            glReadPixels(0, 0, m_captureW, m_captureH, CAPTURE_FORMAT, GL_UNSIGNED_BYTE, dst);
            restoreReadSource();
            GLenum err = glGetError();
            if (err != GL_NO_ERROR) {
                log::error("[Rec] glReadPixels error: 0x{:X}", (unsigned)err);
                return false;
            }
            return true;
        }

        bool gotFrame = false;
        if (m_pboFrame >= (PBO_COUNT - 1)) {
            int readIdx = (m_pboFrame - (PBO_COUNT - 1)) % PBO_COUNT;

            bool ready = true;
            if (fnClientWaitSync && m_pboSync[readIdx]) {
                GLenum syncRes = fnClientWaitSync(
                    m_pboSync[readIdx], GL_SYNC_FLUSH_COMMANDS_BIT, 0);
                ready = (syncRes == GL_ALREADY_SIGNALED ||
                         syncRes == GL_CONDITION_SATISFIED);
                if (!ready) {
                    ++m_syncMisses;
                    if ((m_syncMisses == 1 || m_syncMisses % 300 == 0) &&
                        getBoolSetting("log-performance-degradation", true)) {
                        log::warn("[Rec] PBO DMA not ready (miss #{}) at {}x{}, dropping frame",
                                  m_syncMisses, m_captureW, m_captureH);
                    }
                }
            }

            if (ready) {
                const void* mapped = nullptr;
                if (m_usePersistentPBO) {
                    mapped = m_pboMapped[readIdx];
                } else {
                    fnBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[readIdx]);
                    mapped = fnMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
                }

                if (mapped) {
                    int copySize = std::min(dstSize, m_frameSize);
                    std::memcpy(dst, mapped, copySize);
                    gotFrame = true;
                    if (!m_usePersistentPBO) {
                        fnUnmapBuffer(GL_PIXEL_PACK_BUFFER);
                    }
                } else if (!m_usePersistentPBO) {
                    GLenum err = glGetError();
                    if (err != GL_NO_ERROR)
                        log::error("[Rec] PBO map failed, GL error: 0x{:X}", (unsigned)err);
                }

                if (!m_usePersistentPBO) {
                    fnBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
                }
            }
        }

        int writeIdx = m_pboFrame % PBO_COUNT;
        if (fnDeleteSync && m_pboSync[writeIdx]) {
            fnDeleteSync(m_pboSync[writeIdx]);
            m_pboSync[writeIdx] = nullptr;
        }
        fnBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[writeIdx]);
        glReadPixels(0, 0, m_captureW, m_captureH, CAPTURE_FORMAT, GL_UNSIGNED_BYTE, nullptr);
        fnBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        restoreReadSource();
        if (fnFenceSync)
            m_pboSync[writeIdx] = fnFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

        m_pboFrame++;
        return gotFrame;
    }

    // Returns true if viewport changed (recording must be stopped!)
    bool reinitIfNeeded() {
        GLint vp[4];
        glGetIntegerv(GL_VIEWPORT, vp);
        int newW = vp[2], newH = vp[3];
        if (newW % 2) newW--;
        if (newH % 2) newH--;
        if (newW != m_srcW || newH != m_srcH) {
            log::info("[Rec] Viewport changed {}x{} -> {}x{}, reinit", m_srcW, m_srcH, newW, newH);
            bool wasValid = (m_srcW > 0 && m_srcH > 0);
            destroy();
            init();
            return wasValid;  // true = was capturing, viewport changed -> stop recording
        }
        return false;
    }

    // Check if viewport is zero (window minimized or hidden)
    bool isViewportZero() const {
        GLint vp[4];
        glGetIntegerv(GL_VIEWPORT, vp);
        return vp[2] <= 0 || vp[3] <= 0;
    }

    void destroy() {
        destroyBuffers();
        m_srcVpX = m_srcVpY = 0;
        m_srcW = m_srcH = 0;
        m_captureW = m_captureH = 0;
        m_frameSize = 0;
        m_initialized = false;
    }

private:
    void initBuffers() {
        m_usePBO = false;
        m_useScaleFBO = false;
        m_frameSize = 0;

        if (m_srcW <= 0 || m_srcH <= 0) return;

        int desiredW = (m_requestedW > 0) ? std::min(m_requestedW, m_srcW) : m_srcW;
        int desiredH = (m_requestedH > 0) ? std::min(m_requestedH, m_srcH) : m_srcH;
        if (desiredW % 2) desiredW--;
        if (desiredH % 2) desiredH--;
        if (desiredW <= 0 || desiredH <= 0) {
            desiredW = m_srcW;
            desiredH = m_srcH;
        }

        m_captureW = desiredW;
        m_captureH = desiredH;

        bool wantsResize = (m_captureW != m_srcW || m_captureH != m_srcH);
        bool wantsGpuFlipOnly = !wantsResize &&
                                (static_cast<long long>(m_captureW) * m_captureH >=
                                 640LL * 360LL);
        bool wantsGpuBlit = wantsResize || wantsGpuFlipOnly;
        if (wantsGpuBlit && loadFramebufferFunctions()) {
            if (createScaleFBO(m_captureW, m_captureH)) {
                m_useScaleFBO = true;
                if (wantsResize) {
                    log::info("[Rec] SharedCapture: GPU downscale {}x{} -> {}x{}",
                              m_srcW, m_srcH, m_captureW, m_captureH);
                } else {
                    log::info("[Rec] SharedCapture: GPU flip/blit enabled at {}x{}",
                              m_captureW, m_captureH);
                }
            } else {
                if (wantsResize) {
                    log::warn("[Rec] SharedCapture: GPU downscale unavailable, falling back to full-size readback");
                    m_captureW = m_srcW;
                    m_captureH = m_srcH;
                } else {
                    log::warn("[Rec] SharedCapture: GPU flip/blit unavailable, falling back to CPU flip");
                }
            }
        } else if (wantsResize) {
            log::warn("[Rec] SharedCapture: FBO/blit unavailable, using FFmpeg-side scale fallback");
            m_captureW = m_srcW;
            m_captureH = m_srcH;
        }

        m_frameSize = m_captureW * m_captureH * 4;

        if (m_usePBO && fnDeleteBuffers) {
            destroyBuffers();
        }

        if (loadPBOFunctions()) {
            if (!initPersistentPBOs()) {
                initStreamingPBOs();
            }
        }

        if (!m_usePBO) {
            m_fallbackBuf.resize(m_frameSize);
            log::info("[Rec] SharedCapture: fallback mode (capture {}x{}, source {}x{})",
                      m_captureW, m_captureH, m_srcW, m_srcH);
        }

        m_initialized = true;
        m_pboFrame = 0;
        m_syncMisses = 0;
    }

    bool createScaleFBO(int w, int h) {
        if (!fnGenFramebuffers || !fnBindFramebuffer || !fnGenRenderbuffers ||
            !fnBindRenderbuffer || !fnRenderbufferStorage || !fnFramebufferRenderbuffer ||
            !fnCheckFramebufferStatus) return false;

        fnGenFramebuffers(1, &m_scaleFbo);
        fnGenRenderbuffers(1, &m_scaleColorRb);
        if (!m_scaleFbo || !m_scaleColorRb) return false;

        fnBindFramebuffer(GL_FRAMEBUFFER, m_scaleFbo);
        fnBindRenderbuffer(GL_RENDERBUFFER, m_scaleColorRb);
        fnRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);
        fnFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, m_scaleColorRb);
        GLenum status = fnCheckFramebufferStatus(GL_FRAMEBUFFER);
        fnBindRenderbuffer(GL_RENDERBUFFER, 0);
        fnBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            destroyScaleFBO();
            return false;
        }
        return true;
    }

    void destroyScaleFBO() {
        if (fnDeleteRenderbuffers && m_scaleColorRb) {
            fnDeleteRenderbuffers(1, &m_scaleColorRb);
            m_scaleColorRb = 0;
        }
        if (fnDeleteFramebuffers && m_scaleFbo) {
            fnDeleteFramebuffers(1, &m_scaleFbo);
            m_scaleFbo = 0;
        }
        m_useScaleFBO = false;
    }

    void destroyBuffers() {
        destroyPBOBuffers();
        destroyScaleFBO();
        m_usePBO = false;
        m_usePersistentPBO = false;
        m_fallbackBuf.clear();
        m_pboFrame = 0;
        m_syncMisses = 0;
    }

    void destroyPBOBuffers() {
        if (!fnDeleteBuffers) {
            for (int i = 0; i < PBO_COUNT; ++i) {
                m_pboMapped[i] = nullptr;
                m_pbo[i] = 0;
            }
            return;
        }

        if (fnDeleteSync) {
            for (int i = 0; i < PBO_COUNT; i++) {
                if (m_pboSync[i]) {
                    fnDeleteSync(m_pboSync[i]);
                    m_pboSync[i] = nullptr;
                }
            }
        }

        if (fnUnmapBuffer) {
            for (int i = 0; i < PBO_COUNT; ++i) {
                if (m_pbo[i] && m_pboMapped[i]) {
                    fnBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[i]);
                    fnUnmapBuffer(GL_PIXEL_PACK_BUFFER);
                }
                m_pboMapped[i] = nullptr;
            }
            fnBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        }

        bool hasBuffers = false;
        for (int i = 0; i < PBO_COUNT; ++i) {
            if (m_pbo[i]) {
                hasBuffers = true;
                break;
            }
        }
        if (hasBuffers) {
            fnDeleteBuffers(PBO_COUNT, m_pbo);
        }
        for (int i = 0; i < PBO_COUNT; ++i) {
            m_pbo[i] = 0;
            m_pboMapped[i] = nullptr;
        }
    }

    bool initPersistentPBOs() {
        if (!supportsPersistentPBO()) return false;

        const GLbitfield storageFlags = GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT |
                                        GL_MAP_COHERENT_BIT | GL_CLIENT_STORAGE_BIT;
        const GLbitfield mapFlags = GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT |
                                    GL_MAP_COHERENT_BIT;

        fnGenBuffers(PBO_COUNT, m_pbo);
        bool ok = true;
        for (int i = 0; i < PBO_COUNT; ++i) {
            if (!m_pbo[i]) {
                ok = false;
                break;
            }

            fnBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[i]);
            fnBufferStorage(GL_PIXEL_PACK_BUFFER, m_frameSize, nullptr, storageFlags);
            if (glGetError() != GL_NO_ERROR) {
                ok = false;
                break;
            }

            m_pboMapped[i] = fnMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, m_frameSize, mapFlags);
            if (!m_pboMapped[i]) {
                ok = false;
                break;
            }
        }
        fnBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

        if (!ok || glGetError() != GL_NO_ERROR) {
            destroyPBOBuffers();
            m_usePersistentPBO = false;
            return false;
        }

        m_usePBO = true;
        m_usePersistentPBO = true;
        bool hasSyncFns = (fnFenceSync && fnClientWaitSync && fnDeleteSync);
        log::info("[Rec] SharedCapture: persistent PBO OK {}x (capture {}x{}, source {}x{}) sync={}",
                  PBO_COUNT, m_captureW, m_captureH, m_srcW, m_srcH, hasSyncFns ? "fence" : "none");
        return true;
    }

    bool initStreamingPBOs() {
        fnGenBuffers(PBO_COUNT, m_pbo);
        for (int i = 0; i < PBO_COUNT; i++) {
            fnBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[i]);
            fnBufferData(GL_PIXEL_PACK_BUFFER, m_frameSize, nullptr, GL_STREAM_READ);
        }
        fnBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        if (glGetError() == GL_NO_ERROR) {
            m_usePBO = true;
            m_usePersistentPBO = false;
            bool hasSyncFns = (fnFenceSync && fnClientWaitSync && fnDeleteSync);
            log::info("[Rec] SharedCapture: PBO OK {}x (capture {}x{}, source {}x{}) sync={}",
                      PBO_COUNT,
                      m_captureW, m_captureH, m_srcW, m_srcH, hasSyncFns ? "fence" : "none");
            return true;
        }

        fnDeleteBuffers(PBO_COUNT, m_pbo);
        for (int i = 0; i < PBO_COUNT; ++i) m_pbo[i] = 0;
        return false;
    }

    bool prepareReadSource() {
        if (m_useScaleFBO) {
            if (!fnBindFramebuffer || !fnBlitFramebuffer) return false;
            fnBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
            glReadBuffer(GL_BACK);
            fnBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_scaleFbo);
            GLenum blitFilter = (m_captureW == m_srcW && m_captureH == m_srcH) ? GL_NEAREST : GL_LINEAR;
            fnBlitFramebuffer(
                m_srcVpX, m_srcVpY, m_srcVpX + m_srcW, m_srcVpY + m_srcH,
                0, m_captureH, m_captureW, 0,
                GL_COLOR_BUFFER_BIT, blitFilter
            );
            fnBindFramebuffer(GL_FRAMEBUFFER, m_scaleFbo);
            glReadBuffer(GL_COLOR_ATTACHMENT0);
            return glGetError() == GL_NO_ERROR;
        }

        glReadBuffer(GL_BACK);
        return true;
    }

    void restoreReadSource() {
        if (m_useScaleFBO && fnBindFramebuffer) {
            fnBindFramebuffer(GL_FRAMEBUFFER, 0);
        }
        glReadBuffer(GL_BACK);
    }

    static constexpr int PBO_COUNT = 8;
    bool   m_initialized{false};
    bool   m_usePBO{false};
    bool   m_usePersistentPBO{false};
    bool   m_useScaleFBO{false};
    GLuint m_pbo[PBO_COUNT]{};
    GLuint m_scaleFbo{};
    GLuint m_scaleColorRb{};
    void*  m_pboMapped[PBO_COUNT]{};
    void*  m_pboSync[PBO_COUNT]{};   // GL fence sync handles (void* = GLsync without glext.h)
    int    m_pboFrame{0};
    int    m_syncMisses{0};
    int    m_srcVpX{}, m_srcVpY{}, m_srcW{}, m_srcH{};
    int    m_captureW{}, m_captureH{}, m_frameSize{};
    int    m_requestedW{}, m_requestedH{};
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
        int64_t captureNowNs = steadyTimePointToNanos(now);
        auto tolerance = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(m_captureInterval * 0.05)
        );
        if (now + tolerance < m_nextCaptureDue) return;
        auto step = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(m_captureInterval)
        );
        if (step.count() <= 0) step = std::chrono::steady_clock::duration(1);
        do {
            m_nextCaptureDue += step;
        } while (m_nextCaptureDue + tolerance < now);

        // Timestamp relative to recording start (used for frame duplication in writer)
        double captureTime = std::chrono::duration<double>(now - m_startTime).count();

        int queueSize = static_cast<int>(m_writeQueue.size());
        int skipThreshold = std::max(1, (m_maxQueue * 4) / 5);
        if (m_isSoftwareEncoder) {
            skipThreshold = std::max(1, m_maxQueue / 2);
        } else if (m_isMpeg4Encoder || g_gpuIsWeak) {
            skipThreshold = std::max(1, (m_maxQueue * 2) / 3);
        }
        if (queueSize >= skipThreshold) {
            // Encoder is behind: skip this capture tick to reduce stutter on game thread.
            int skipped = ++m_backpressureSkips;
            if (m_logPerformanceDegradation && (skipped == 1 || skipped % 120 == 0)) {
                log::warn("[Rec] Encoder backpressure: queue={} / {} (threshold={}, skip #{})",
                          queueSize, m_maxQueue, skipThreshold, skipped);
            }
            return;
        }

        int idx = -1;
        if (m_freeSlots.empty()) {
            int d = ++m_dropped;
            if (m_logPerformanceDegradation && (d == 1 || (d % 60 == 0)))
                log::warn("[Rec] Frame dropped (pool full): total dropped={}", d);
            return;
        }
        if (!m_freeSlots.pop(idx)) {
            int d = ++m_dropped;
            if (m_logPerformanceDegradation && (d == 1 || (d % 60 == 0)))
                log::warn("[Rec] Frame dropped (free queue underflow): total dropped={}", d);
            return;
        }

        // Read from GL_BACK (before swapBuffers) - avoids GL_FRONT sync stall
        uint8_t* slot = slotData(idx);
        bool ok = slot && cap.captureInto(slot, m_frameSize);

        if (ok) {
            m_frameTimes[idx] = captureTime;
            if (!m_writeQueue.push(idx)) {
                ++m_dropped;
                m_freeSlots.push(idx);
                return;
            }
            markFirstVideoFrameTimestamp(captureNowNs);
            ++m_frames;
            m_writerCv.notify_one();
        } else {
            m_freeSlots.push(idx);
        }
    }

    double getElapsedSeconds() const {
        if (m_state != State::Active && m_state != State::Saving) return 0.0;
        return std::chrono::duration<double>(
            std::chrono::steady_clock::now() - m_startTime).count();
    }

protected:
    struct IntRingQueue {
        std::vector<int> m_data;
        std::atomic<size_t> m_head{0};
        std::atomic<size_t> m_tail{0};

        void reset(size_t capacity) {
            m_data.assign(capacity + 1, 0);
            m_head.store(0, std::memory_order_relaxed);
            m_tail.store(0, std::memory_order_relaxed);
        }

        void clear() {
            m_data.clear();
            m_head.store(0, std::memory_order_relaxed);
            m_tail.store(0, std::memory_order_relaxed);
        }

        bool empty() const {
            return m_head.load(std::memory_order_acquire) ==
                   m_tail.load(std::memory_order_acquire);
        }

        size_t size() const {
            if (m_data.empty()) return 0;
            size_t head = m_head.load(std::memory_order_acquire);
            size_t tail = m_tail.load(std::memory_order_acquire);
            return (tail >= head) ? (tail - head) : (m_data.size() - head + tail);
        }

        bool push(int value) {
            if (m_data.size() < 2) return false;
            size_t tail = m_tail.load(std::memory_order_relaxed);
            size_t next = (tail + 1) % m_data.size();
            if (next == m_head.load(std::memory_order_acquire)) return false;
            m_data[tail] = value;
            m_tail.store(next, std::memory_order_release);
            return true;
        }

        bool pop(int& value) {
            if (m_data.size() < 2) return false;
            size_t head = m_head.load(std::memory_order_relaxed);
            if (head == m_tail.load(std::memory_order_acquire)) return false;
            value = m_data[head];
            m_head.store((head + 1) % m_data.size(), std::memory_order_release);
            return true;
        }
    };

    // Strong GPU: more queue = smoother under load
    // Weak GPU: less queue = less RAM, less latency, fewer stutters
    static constexpr int DEFAULT_MAX_QUEUE      = 24;
    static constexpr int WEAK_GPU_MAX_QUEUE     = 6;
    static constexpr int MPEG4_MAX_QUEUE        = 20;
    static constexpr int WEAK_MPEG4_MAX_QUEUE   = 8;
    static constexpr int SW_ENCODER_MAX_QUEUE   = 16;
    static constexpr int WEAK_SW_MAX_QUEUE      = 4;
    static constexpr DWORD PIPE_BUF_SIZE        = 64 * 1024 * 1024;
    static constexpr DWORD WEAK_PIPE_BUF_SIZE   = 8  * 1024 * 1024;
    static constexpr DWORD MPEG4_PIPE_BUF_SIZE  = 48 * 1024 * 1024;
    static constexpr DWORD WEAK_MPEG4_PIPE_BUF  = 12 * 1024 * 1024;
    static constexpr DWORD SW_PIPE_BUF_SIZE     = 32 * 1024 * 1024;
    static constexpr DWORD WEAK_SW_PIPE_BUF     = 4  * 1024 * 1024;
    static constexpr DWORD FFMPEG_WAIT_TIMEOUT  = 15000;

    int m_maxQueue{DEFAULT_MAX_QUEUE};
    int m_poolSize{DEFAULT_MAX_QUEUE + 4};
    DWORD m_ffmpegPriority{0};
    DWORD m_pipeBufSize{PIPE_BUF_SIZE};

    std::atomic<State> m_state{State::Idle};
    int m_frameSize{};
    int m_width{};   // frame width  (set in start() - used for writer-thread row flip)
    int m_height{};  // frame height
    int m_fps{60};
    double m_captureInterval{1.0 / 60.0};

    // Pre-allocated frame pool + queue
    std::vector<uint8_t> m_poolStorage;
    std::vector<double> m_frameTimes;  // capture timestamp per slot (relative to start)
    IntRingQueue m_freeSlots;
    IntRingQueue m_writeQueue;
    std::mutex m_writerWaitMtx;
    std::condition_variable m_writerCv;

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
    std::atomic<int> m_backpressureSkips{0};
    std::atomic<bool> m_pipeError{false};
    bool m_ffmpegOk{false};
    bool m_isSoftwareEncoder{false};
    bool m_isMpeg4Encoder{false};
    bool m_gpuReadbackFlipped{false};
    bool m_logPerformanceDegradation{true};
    std::string m_stderrLogPath;

    uint8_t* slotData(int idx) {
        if (idx < 0 || m_frameSize <= 0) return nullptr;
        size_t offset = static_cast<size_t>(idx) * static_cast<size_t>(m_frameSize);
        return (offset < m_poolStorage.size()) ? (m_poolStorage.data() + offset) : nullptr;
    }

    const uint8_t* slotData(int idx) const {
        if (idx < 0 || m_frameSize <= 0) return nullptr;
        size_t offset = static_cast<size_t>(idx) * static_cast<size_t>(m_frameSize);
        return (offset < m_poolStorage.size()) ? (m_poolStorage.data() + offset) : nullptr;
    }

    // Quick check if FFmpeg process is still running
    bool checkFFmpegAlive() const {
        if (m_proc == INVALID_HANDLE_VALUE) return false;
        DWORD exitCode = 0;
        if (!GetExitCodeProcess(m_proc, &exitCode)) return false;
        return exitCode == STILL_ACTIVE;
    }

    // Clean up FFmpeg handles without full stopPipeline (no writer/pool)
    void cleanupFFmpegHandles() {
        if (m_pipe != INVALID_HANDLE_VALUE) { CloseHandle(m_pipe); m_pipe = INVALID_HANDLE_VALUE; }
        if (m_proc != INVALID_HANDLE_VALUE) { CloseHandle(m_proc); m_proc = INVALID_HANDLE_VALUE; }
        if (m_nulHandle != INVALID_HANDLE_VALUE) { CloseHandle(m_nulHandle); m_nulHandle = INVALID_HANDLE_VALUE; }
    }

    // Timing
    std::chrono::steady_clock::time_point m_startTime;
    std::chrono::steady_clock::time_point m_lastCapture;
    std::chrono::steady_clock::time_point m_nextCaptureDue;
    std::atomic<int64_t> m_firstVideoFrameNs{0};
    std::atomic<int64_t> m_stopRequestNs{0};

    void markFirstVideoFrameTimestamp(int64_t frameNs) {
        if (frameNs <= 0) return;
        int64_t expected = 0;
        m_firstVideoFrameNs.compare_exchange_strong(
            expected, frameNs, std::memory_order_release, std::memory_order_relaxed
        );
    }

    void allocatePool() {
        size_t totalBytes = static_cast<size_t>(m_poolSize) * static_cast<size_t>(m_frameSize);
        m_poolStorage.resize(totalBytes);
        m_frameTimes.assign(m_poolSize, 0.0);
        m_freeSlots.reset(static_cast<size_t>(m_poolSize));
        m_writeQueue.reset(static_cast<size_t>(m_poolSize));
        for (int i = 0; i < m_poolSize; i++) {
            m_freeSlots.push(i);
        }
    }

    void freePool() {
        m_poolStorage.clear();
        m_frameTimes.clear();
        m_freeSlots.clear();
        m_writeQueue.clear();
    }

    void writerLoop() {
        // Keep hardware paths aggressive, but let CPU-based encoders yield more time to the game.
        tuneWorkerThread(
            m_isSoftwareEncoder ? THREAD_PRIORITY_NORMAL : THREAD_PRIORITY_ABOVE_NORMAL,
            0
        );
        log::info("[Rec] Writer thread started (tid={})", GetCurrentThreadId());
        std::vector<uint8_t> frameBuf;
        frameBuf.resize(m_frameSize);
        std::vector<uint8_t> lastFrameBuf;
        lastFrameBuf.resize(m_frameSize);
        bool haveTimelineBase = false;
        bool haveLastFrame = false;
        double firstCaptureTime = 0.0;
        int64_t firstFrameNsAbs = 0;
        int64_t nextFrameIndex = 0;
        int statsCounter = 0;

        auto writeDuplicateFrame = [&]() {
            if (!haveLastFrame || m_frameSize <= 0) return;
            writeFrameToPipe(lastFrameBuf.data(), m_frameSize);
            ++m_duped;
            ++nextFrameIndex;
        };

        auto writeActualFrame = [&](const uint8_t* src) {
            if (!src || m_frameSize <= 0) return;
            writeFrameToPipe(src, m_frameSize);
            std::memcpy(lastFrameBuf.data(), src, m_frameSize);
            haveLastFrame = true;
            ++nextFrameIndex;
        };

        auto fillGapWithDuplicates = [&](int64_t targetFrameIndex) {
            if (!haveLastFrame || targetFrameIndex <= nextFrameIndex) return;

            int64_t missingFrames = targetFrameIndex - nextFrameIndex;
            if (m_logPerformanceDegradation && missingFrames >= std::max<int64_t>(2, m_fps / 4)) {
                log::warn("[Rec] Timing gap detected: duplicating {} frame(s) to preserve A/V sync",
                          missingFrames);
            }

            while (nextFrameIndex < targetFrameIndex) {
                writeDuplicateFrame();
            }
        };

        for (;;) {
            int idx = -1;
            double captureTime = 0.0;
            while (m_writeQueue.empty()) {
                if (m_state != State::Active) break;
                std::unique_lock<std::mutex> lk(m_writerWaitMtx);
                m_writerCv.wait_for(lk, std::chrono::milliseconds(2), [this] {
                    return !m_writeQueue.empty() || m_state != State::Active;
                });
            }
            if (m_writeQueue.empty()) {
                if (m_state != State::Active) {
                    int64_t stopRequestNs = m_stopRequestNs.load(std::memory_order_acquire);
                    if (haveLastFrame && haveTimelineBase && stopRequestNs > firstFrameNsAbs && m_captureInterval > 0.0) {
                        double stopRelativeTime = static_cast<double>(stopRequestNs - firstFrameNsAbs) / 1000000000.0;
                        int64_t targetTotalFrames = std::max<int64_t>(
                            1,
                            static_cast<int64_t>(std::llround(stopRelativeTime / m_captureInterval))
                        );
                        fillGapWithDuplicates(targetTotalFrames);
                    }
                }
                break;
            }
            if (!m_writeQueue.pop(idx)) continue;
            captureTime = m_frameTimes[idx];

            const uint8_t* frameData = slotData(idx);
            bool keepSlotUntilWrite = m_gpuReadbackFlipped;
            if (!keepSlotUntilWrite) {
                // Fallback path: flip during copy only when GPU-side flip is unavailable.
                const int stride = m_width * 4;
                if (frameData && m_width > 0 && m_height > 0) {
                    for (int y = 0; y < m_height; y++) {
                        std::memcpy(
                            frameBuf.data() + y * stride,
                            frameData + static_cast<size_t>(m_height - 1 - y) * stride,
                            stride
                        );
                    }
                } else if (frameData) {
                    std::memcpy(frameBuf.data(), frameData, m_frameSize);
                }
                frameData = frameBuf.data();
            }

            if (!haveTimelineBase) {
                haveTimelineBase = true;
                firstCaptureTime = captureTime;
                firstFrameNsAbs = steadyTimePointToNanos(m_startTime) +
                    static_cast<int64_t>(std::llround(captureTime * 1000000000.0));
            }

            double relativeTime = captureTime - firstCaptureTime;
            if (relativeTime < 0.0) relativeTime = 0.0;

            int64_t targetFrameIndex = 0;
            if (m_captureInterval > 0.0) {
                targetFrameIndex = static_cast<int64_t>(std::llround(relativeTime / m_captureInterval));
            }
            if (targetFrameIndex < nextFrameIndex) {
                targetFrameIndex = nextFrameIndex;
            }
            fillGapWithDuplicates(targetFrameIndex);

            // Write actual captured frame
            if (frameData) writeActualFrame(frameData);

            m_freeSlots.push(idx);

            // Periodic stats log (~every 5 seconds at 60fps)
            if (++statsCounter % 600 == 0) {
                int queueSize = static_cast<int>(m_writeQueue.size());
                int freeSize = static_cast<int>(m_freeSlots.size());
                log::info("[Rec] Stats: written={} dropped={} duped={} queue={} free={}",
                          m_frames.load(), m_dropped.load(), m_duped.load(),
                          queueSize, freeSize);
            }
        }
        log::info("[Rec] Writer thread exiting");
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
        // Save pipeError BEFORE closing pipe (closing causes writer's WriteFile to fail)
        bool hadPipeError = m_pipeError.load(std::memory_order_relaxed);

        State expected = State::Active;
        m_state.compare_exchange_strong(expected, State::Idle);
        if (expected == State::Saving) m_state = State::Saving;
        m_writerCv.notify_all();

        // In fast mode, kill FFmpeg FIRST to break the pipe and unblock writer thread
        if (fast && m_proc != INVALID_HANDLE_VALUE) {
            TerminateProcess(m_proc, 0);
        }

        // Close pipe BEFORE joining writer thread - prevents infinite hang
        // when writer is blocked on WriteFile with full pipe buffer
        // (FFmpeg backpressure -> writer stuck on WriteFile -> join never returns)
        if (m_pipe != INVALID_HANDLE_VALUE) {
            CloseHandle(m_pipe);
            m_pipe = INVALID_HANDLE_VALUE;
        }

        if (m_writerThread.joinable()) m_writerThread.join();

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
            // With -shortest + live audio inputs, FFmpeg often exits non-zero
            // even though the file is perfectly valid. Don't rely on exit code alone.
            m_ffmpegOk = !hadPipeError;
            if (!m_ffmpegOk && !fast) {
                log::error("[Rec] FFmpeg exited with code: {} (pipe_error_during_rec={})",
                           exitCode, hadPipeError);
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
        m_isSoftwareEncoder = false;
        m_isMpeg4Encoder = false;
        m_gpuReadbackFlipped = false;
        m_backpressureSkips.store(0);
    }

    static int encoderMaxFps(const std::string& encoder, int width, int height) {
        if (encoder == "h264_nvenc") return 120;
        if (encoder == "h264_amf")   return 120;
        if (encoder == "h264_qsv")   return 120;
        if (encoder == "mpeg4") {
            if (height > 1080 || width > 1920) return 30;
            if (height > 720 || width > 1280)  return 45;
            return 60;
        }
        if (encoder == "libx264") {
            if (height > 720 || width > 1280) return 30;
            if (height > 480 || width > 854)  return 45;
            return 60;
        }
        if (encoder == "libx265") {
            if (height > 720 || width > 1280) return 24;
            return 30;
        }
        return 120;
    }

    void configurePipelineForEncoder(const std::string& encoder) {
        if (encoder == "mpeg4") {
            m_ffmpegPriority = BELOW_NORMAL_PRIORITY_CLASS;
        } else if (encoder == "libx264" || encoder == "libx265") {
            m_ffmpegPriority = BELOW_NORMAL_PRIORITY_CLASS;
        } else {
            m_ffmpegPriority = NORMAL_PRIORITY_CLASS;
        }

        if (encoder == "libx264" || encoder == "libx265") {
            if (g_gpuIsWeak) {
                m_pipeBufSize = WEAK_SW_PIPE_BUF;
                m_maxQueue = WEAK_SW_MAX_QUEUE;
            } else {
                m_pipeBufSize = SW_PIPE_BUF_SIZE;
                m_maxQueue = SW_ENCODER_MAX_QUEUE;
            }
        } else if (encoder == "mpeg4") {
            if (g_gpuIsWeak) {
                m_pipeBufSize = WEAK_MPEG4_PIPE_BUF;
                m_maxQueue = WEAK_MPEG4_MAX_QUEUE;
            } else {
                m_pipeBufSize = MPEG4_PIPE_BUF_SIZE;
                m_maxQueue = MPEG4_MAX_QUEUE;
            }
        } else {
            if (g_gpuIsWeak) {
                m_pipeBufSize = WEAK_PIPE_BUF_SIZE;
                m_maxQueue = WEAK_GPU_MAX_QUEUE;
            } else {
                m_pipeBufSize = PIPE_BUF_SIZE;
                m_maxQueue = DEFAULT_MAX_QUEUE;
            }
        }
        m_poolSize = m_maxQueue + 4;
    }

    void tunePipelineForFrameSize(const std::string& encoder, int frameBytes, int fps) {
        if (frameBytes <= 0) return;

        bool softwareEncoder = isSoftwareEncoderName(encoder);
        bool cpuRealtime = isCpuRealtimeEncoderName(encoder);

        int targetQueue = m_maxQueue;
        if (softwareEncoder) {
            targetQueue = std::max(targetQueue, g_gpuIsWeak ? 8 : 12);
            if (fps >= 60) {
                targetQueue = std::max(targetQueue, g_gpuIsWeak ? 10 : 14);
            }
        } else if (encoder == "mpeg4") {
            targetQueue = std::max(targetQueue, g_gpuIsWeak ? 10 : 14);
        } else {
            targetQueue = std::max(targetQueue, g_gpuIsWeak ? 6 : 10);
        }

        uint64_t minPipeBytes = cpuRealtime
            ? static_cast<uint64_t>(g_gpuIsWeak ? 12 : 16) * 1024ULL * 1024ULL
            : 8ULL * 1024ULL * 1024ULL;
        int pipeFrames = softwareEncoder ? 3 : (encoder == "mpeg4" ? 4 : 2);
        uint64_t desiredPipeBytes = static_cast<uint64_t>(frameBytes) *
                                    static_cast<uint64_t>(pipeFrames);
        desiredPipeBytes = std::max(desiredPipeBytes, minPipeBytes);
        desiredPipeBytes = std::min<uint64_t>(desiredPipeBytes, 64ULL * 1024ULL * 1024ULL);

        if (desiredPipeBytes > static_cast<uint64_t>(m_pipeBufSize)) {
            m_pipeBufSize = static_cast<DWORD>(desiredPipeBytes);
        }
        if (targetQueue > m_maxQueue) {
            m_maxQueue = targetQueue;
        }
        m_poolSize = m_maxQueue + 4;

        log::info("[Rec] Pipeline tuned for {}: frame={}KB queue={} pipe={}MB",
                  encoder, frameBytes / 1024, m_maxQueue,
                  m_pipeBufSize / (1024 * 1024));
    }

    // Build FFmpeg command for video-only encode.
    // Audio is captured separately via WASAPI and muxed in after stop.
    std::string buildFFmpegCommand(
        const std::string& ffmpegExe,
        int inputW, int inputH, int fps,
        const std::string& encoder, int crf,
        const std::string& outPath,
        bool addKeyframes, bool lowLatency,
        int outputW, int outputH
    ) {
        bool isSoftware = (encoder == "libx264" || encoder == "libx265");
        int videoQ = g_gpuIsWeak ? 512 : 2048;
        if (g_gpuIsWeak && isSoftware) {
            videoQ = 256;
        }

        std::ostringstream cmd;
        cmd << "\"" << ffmpegExe << "\""
            << " -y -hide_banner -loglevel warning"
            << " -f rawvideo -pixel_format bgra"
            << " -video_size " << inputW << "x" << inputH
            << " -framerate " << fps
            << " -thread_queue_size " << videoQ
            << " -i pipe:0"
            << " -sws_flags fast_bilinear";  // faster BGRA->YUV420P conversion
        cmd << " -map 0:v";

        // Vertical flip is handled in the writer thread (flip-during-copy),
        // so no -vf vflip is needed here - saves a CPU filter pass in FFmpeg.
        // If GPU-side downscale is unavailable, keep the old FFmpeg-side scale fallback.
        if (inputW != outputW || inputH != outputH) {
            cmd << " -vf \"scale=" << outputW << ":" << outputH << "\"";
        }
        cmd << buildEncoderArgs(encoder, crf, lowLatency);

        // Keyframes every 2 seconds (wider = less encode overhead, ?2s trim accuracy is fine)
        if (addKeyframes) {
            cmd << " -g " << (fps * 2);
        }

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
        HANDLE stderrHandle = CreateFileW(
            utf8ToWide(m_stderrLogPath).c_str(), FILE_APPEND_DATA,
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
                     int encoderSetting, int gpuDownscaleMode, int crf,
                     const std::string& outPath,
                     bool addKeyframes, bool lowLatency,
                     bool micEnabled, const std::string& micDevice,
                     bool gameAudioEnabled, const std::string& gameAudioDevice,
                     std::string& outEncoderName,
                     int& actualFps,
                     int& actualCaptureW,
                     int& actualCaptureH) {
        actualFps = fps;
        actualCaptureW = w;
        actualCaptureH = h;
        std::string primary = pickEncoder(encoderSetting);

        // Auto mode tries the preferred HW encoder first, then the other HW encoders.
        // Manual mode respects the selected encoder and only falls back after it fails.
        std::vector<std::string> hwChain;
        if (encoderSetting == 1) {
            if (primary != "libx264") hwChain.push_back(primary);
            for (const char* fb : {"h264_qsv", "h264_nvenc", "h264_amf"}) {
                if (std::find(hwChain.begin(), hwChain.end(), fb) == hwChain.end())
                    hwChain.push_back(fb);
            }
        } else {
            hwChain.push_back(primary);
        }

        // Use global encoder cache (pre-warmed at mod load on background thread).
        // If cache isn't ready yet (F5 pressed very early), test on demand.

        for (const auto& enc : hwChain) {
            bool works = false;
            bool cached = false;
            {
                std::lock_guard<std::mutex> lk(g_encoderCacheMtx);
                auto it = g_encoderCache.find(enc);
                if (it != g_encoderCache.end()) {
                    works = it->second;
                    cached = true;
                }
            }
            if (cached) {
                log::info("[Rec] Encoder {} (cached): {}", enc, works ? "OK" : "FAIL");
            } else {
                log::warn("[Rec] Encoder {} not in cache - testing on main thread "
                          "(press F5 later to avoid freeze)", enc);
                auto t0 = std::chrono::steady_clock::now();
                works = testEncoderWorks(ffmpegExe, enc, getOutputDir());
                double dt = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - t0).count();
                log::info("[Rec] Encoder {} test: {} ({:.1f}s)", enc, works ? "OK" : "FAIL", dt);
                {
                    std::lock_guard<std::mutex> lk(g_encoderCacheMtx);
                    g_encoderCache[enc] = works;
                }
            }
            if (works) {
                configurePipelineForEncoder(enc);

                int desiredW = w;
                int desiredH = h;
                computeRequestedOutputSize(w, h, enc, gpuDownscaleMode, desiredW, desiredH);

                int maxFps = encoderMaxFps(enc, desiredW, desiredH);
                int useFps = std::min(fps, maxFps);
                if (useFps != fps) {
                    log::warn("[Rec] FPS capped for {}: {} -> {} (workload {}x{}, max={})",
                              enc, fps, useFps, desiredW, desiredH, maxFps);
                    Notification::create(
                        "FPS capped to " + std::to_string(useFps) + " for " + enc +
                        " (" + std::to_string(desiredW) + "x" + std::to_string(desiredH) + ")",
                        NotificationIcon::Info, 3.f
                    )->show();
                }
                actualFps = useFps;

                int inputW = w;
                int inputH = h;
                bool wantsDownscale = (desiredW != w || desiredH != h);
                if (wantsDownscale) {
                    bool usingRequested = g_capture.configureCaptureSize(desiredW, desiredH);
                    if (usingRequested && g_capture.captureWidth() == desiredW && g_capture.captureHeight() == desiredH) {
                        inputW = desiredW;
                        inputH = desiredH;
                        log::info("[Rec] {}: using GPU downscale {}x{} -> {}x{}",
                                  enc, w, h, inputW, inputH);
                    } else {
                        inputW = w;
                        inputH = h;
                        log::info("[Rec] {}: GPU downscale unavailable, using FFmpeg scale fallback {}x{} -> {}x{}",
                                  enc, w, h, desiredW, desiredH);
                    }
                } else {
                    g_capture.resetCaptureSize();
                }

                tunePipelineForFrameSize(enc, inputW * inputH * 4, useFps);

                std::string cmd = buildFFmpegCommand(
                    ffmpegExe, inputW, inputH, useFps, enc, crf, outPath, addKeyframes, lowLatency,
                    desiredW, desiredH
                );
                log::info("[Rec] CMD: {}", cmd);
                if (spawnFFmpegProcess(cmd)) {
                    m_isSoftwareEncoder = (enc == "libx264" || enc == "libx265");
                    m_isMpeg4Encoder = (enc == "mpeg4");
                    outEncoderName = enc;
                    actualCaptureW = inputW;
                    actualCaptureH = inputH;
                    log::info("[Rec] Using encoder: {}", enc);
                    return true;
                }
            } else {
                log::warn("[Rec] {} test failed, trying next encoder...", enc);
            }
        }

        // All HW encoders failed - fall back to CPU encoding
        std::string swEncoder = chooseSoftwareFallbackEncoder(primary, fps, w, h);
        log::warn("[Rec] All HW encoders failed, falling back to {}. "
                  "CPU encoding will be used - may impact game performance.", swEncoder);
        configurePipelineForEncoder(swEncoder);

        int desiredW = w;
        int desiredH = h;
        computeRequestedOutputSize(w, h, swEncoder, gpuDownscaleMode, desiredW, desiredH);

        int maxFps = encoderMaxFps(swEncoder, desiredW, desiredH);
        int useFps = std::min(fps, maxFps);
        if (useFps != fps) {
            log::warn("[Rec] FPS capped for {}: {} -> {} (workload {}x{}, max={})",
                      swEncoder, fps, useFps, desiredW, desiredH, maxFps);
            Notification::create(
                "FPS capped to " + std::to_string(useFps) + " for " + swEncoder +
                " (" + std::to_string(desiredW) + "x" + std::to_string(desiredH) + ")",
                NotificationIcon::Info, 3.f
            )->show();
        }
        actualFps = useFps;

        int inputW = w;
        int inputH = h;
        bool wantsDownscale = (desiredW != w || desiredH != h);
        if (wantsDownscale) {
            bool usingRequested = g_capture.configureCaptureSize(desiredW, desiredH);
            if (usingRequested && g_capture.captureWidth() == desiredW && g_capture.captureHeight() == desiredH) {
                inputW = desiredW;
                inputH = desiredH;
                log::info("[Rec] {} fallback: using GPU downscale {}x{} -> {}x{}",
                          swEncoder, w, h, inputW, inputH);
            } else {
                log::info("[Rec] {} fallback: GPU downscale unavailable, using FFmpeg scale fallback {}x{} -> {}x{}",
                          swEncoder, w, h, desiredW, desiredH);
            }
        } else {
            g_capture.resetCaptureSize();
        }

        tunePipelineForFrameSize(swEncoder, inputW * inputH * 4, actualFps);

        std::string cmd = buildFFmpegCommand(
            ffmpegExe, inputW, inputH, actualFps, swEncoder, crf, outPath, addKeyframes, lowLatency,
            desiredW, desiredH
        );
        log::info("[Rec] CMD (fallback): {}", cmd);
        if (spawnFFmpegProcess(cmd)) {
            m_isSoftwareEncoder = (swEncoder == "libx264" || swEncoder == "libx265");
            m_isMpeg4Encoder = (swEncoder == "mpeg4");
            outEncoderName = swEncoder;
            actualCaptureW = inputW;
            actualCaptureH = inputH;
            log::info("[Rec] Using encoder: {} (fallback, queue={}, pipe={}MB)",
                       swEncoder, m_maxQueue, m_pipeBufSize / (1024 * 1024));
            return true;
        }
        g_capture.resetCaptureSize();
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
    if (exitCode == 0 && fs::exists(utf8Path(tmpPath), ec) &&
        fs::file_size(utf8Path(tmpPath), ec) > 0) {
        fs::remove(utf8Path(filePath), ec);
        fs::rename(utf8Path(tmpPath), utf8Path(filePath), ec);
        if (!ec) return true;
    }
    fs::remove(utf8Path(tmpPath), ec);
    return false;
}

// ==================================================================
// ScreenRecorder (F5 - normal recording with multi-track audio)
// ==================================================================

class ScreenRecorder : public RecordingPipeline {
public:
    static ScreenRecorder& get() {
        static ScreenRecorder s;
        return s;
    }

    bool start(int w, int h) {
        if (m_state != State::Idle) return false;
        auto startupT0 = std::chrono::steady_clock::now();
        log::info("[Rec] ?? start() begin ({}x{}) ??", w, h);

        readSettings();
        log::info("[Rec] Settings: quality={} fps={} encoder={} gpuDownscale={} mic={} gameAudio={} micVolume={}%",
                  m_quality, m_fps, m_encoderSetting, m_gpuDownscaleMode, m_micEnabled,
                  m_gameAudioEnabled, Mod::get()->getSettingValue<int>("mic-volume"));

        if (w <= 0 || h <= 0) return false;
        m_w = w; m_h = h;
        m_width = w; m_height = h;
        m_frameSize = w * h * 4;

        m_ffmpegPriority = NORMAL_PRIORITY_CLASS;
        m_pipeBufSize  = g_gpuIsWeak ? WEAK_PIPE_BUF_SIZE : PIPE_BUF_SIZE;
        m_maxQueue     = g_gpuIsWeak ? WEAK_GPU_MAX_QUEUE : DEFAULT_MAX_QUEUE;
        m_poolSize     = m_maxQueue + 4;

        std::string ffmpeg = locateFFmpeg();
        if (ffmpeg.empty()) {
            Notification::create(
                "FFmpeg not found! Put ffmpeg.exe in GD folder, ffmpeg\\bin, or PATH.",
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

        std::string micWavPath;
        std::string gameAudioWavPath;
        bool recordMic = m_micEnabled && !micDev.empty();
        bool recordGame = false;
        bool usingDirectGameAudio = false;
        bool usingFmodGameAudio = false;

        if (m_gameAudioEnabled) {
            if (hasFmodGameAudioBackend()) {
                gameAudioWavPath = m_outPath + ".gameaudio.wav";
                usingDirectGameAudio = true;
                usingFmodGameAudio = true;
                recordGame = true;
                log::info("[Rec] Game audio: FMOD master tap from GeometryDash.exe");
                log::info("[Rec] WAV temp file: {}", gameAudioWavPath);
            } else if (isProcessLoopbackSupported()) {
                gameAudioWavPath = m_outPath + ".gameaudio.wav";
                usingDirectGameAudio = true;
                recordGame = true;
                log::info("[Rec] Game audio: process-only loopback fallback from GeometryDash.exe");
                log::info("[Rec] WAV temp file: {}", gameAudioWavPath);
            } else {
                log::warn("[Rec] Game audio disabled for this recording: FMOD tap unavailable and process loopback requires Windows build {}+ (current build={})",
                          PROCESS_LOOPBACK_MIN_BUILD, getWindowsBuildNumber());
                if (m_showUnsupportedOsWarning) {
                    Notification::create(
                        "Game audio backend unavailable on this system right now. Recording without game audio.",
                        NotificationIcon::Warning, 5.f
                    )->show();
                }
            }
        } else {
            log::info("[Rec] Game audio: disabled in settings");
        }

        if (recordMic) {
            micWavPath = m_outPath + ".mic.wav";
            log::info("[Rec] Microphone: WASAPI endpoint capture -> {}", micWavPath);
            if (!MicrophoneCapture::get().start(micWavPath, micDev)) {
                log::error("[Rec] Failed to start microphone capture!");
                if (m_autoDisableMicOnError) {
                    Notification::create(
                        "Microphone capture failed. Continuing without mic.",
                        NotificationIcon::Warning, 5.f
                    )->show();
                    recordMic = false;
                    micWavPath.clear();
                } else {
                    if (usingDirectGameAudio) GameAudioCapture::get().stop();
                    Notification::create(
                        "Microphone capture failed! Recording cancelled.",
                        NotificationIcon::Error, 5.f
                    )->show();
                    return false;
                }
            }
        }

        std::string encoderName;
        int actualFps = m_fps;
        int captureW = w;
        int captureH = h;

        if (usingDirectGameAudio) {
            log::info("[Rec] Starting game audio capture...");
            bool audioStarted = usingFmodGameAudio
                ? FmodGameAudioCapture::get().start(gameAudioWavPath)
                : GameAudioCapture::get().start(gameAudioWavPath);
            if (!audioStarted) {
                log::error("[Rec] Failed to start game audio capture!");
                if (usingFmodGameAudio && isProcessLoopbackSupported()) {
                    log::warn("[Rec] FMOD game audio capture failed, retrying process loopback fallback...");
                    usingFmodGameAudio = false;
                    if (!GameAudioCapture::get().start(gameAudioWavPath)) {
                        Notification::create(
                            "Game audio capture failed! Recording video only.",
                            NotificationIcon::Warning, 5.f
                        )->show();
                        usingDirectGameAudio = false;
                        recordGame = false;
                        gameAudioWavPath.clear();
                    }
                } else {
                    Notification::create(
                        "Game audio capture failed! Recording video only.",
                        NotificationIcon::Warning, 5.f
                    )->show();
                    usingDirectGameAudio = false;
                    recordGame = false;
                    gameAudioWavPath.clear();
                }
            }
        }

        log::info("[Rec] Starting FFmpeg (encoder cache ready: {})...",
                  g_encoderCacheReady.load() ? "yes" : "NO - may freeze!");
        auto ffmpegT0 = std::chrono::steady_clock::now();
        if (!startFFmpeg(ffmpeg, w, h, m_fps, m_encoderSetting, m_gpuDownscaleMode, qualityToCRF(m_quality),
                         m_outPath, false, false,
                         false, "",
                         false, "",
                         encoderName, actualFps, captureW, captureH)) {
            if (recordMic) MicrophoneCapture::get().stop();
            if (usingDirectGameAudio) {
                if (usingFmodGameAudio) FmodGameAudioCapture::get().stop();
                else GameAudioCapture::get().stop();
            }
            Notification::create(
                "Failed to start FFmpeg!",
                NotificationIcon::Error, 3.f
            )->show();
            return false;
        }
        double ffmpegDt = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - ffmpegT0).count();
        log::info("[Rec] FFmpeg spawned in {:.2f}s", ffmpegDt);

        if (recordGame) {
            Notification::create(
                usingFmodGameAudio
                    ? "Game audio: capturing GeometryDash.exe via FMOD"
                    : "Game audio: capturing GeometryDash.exe only",
                NotificationIcon::Info, 3.f
            )->show();
        }

        if (actualFps != m_fps) {
            log::info("[Rec] FPS capped: {} -> {} (encoder max)", m_fps, actualFps);
            m_fps = actualFps;
            m_captureInterval = 1.0 / static_cast<double>(m_fps);
        }

        m_w = captureW;
        m_h = captureH;
        m_width = captureW;
        m_height = captureH;
        m_frameSize = captureW * captureH * 4;
        m_gpuReadbackFlipped = g_capture.isGpuFlipped();

        // Allocate pool AFTER encoder selection (software encoder increases queue size)
        {
            auto poolT0 = std::chrono::steady_clock::now();
            allocatePool();
            double poolDt = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - poolT0).count();
            int poolMB = (m_poolSize * m_frameSize) / (1024 * 1024);
            log::info("[Rec] Pool allocated: {} slots x {}KB = {}MB ({:.2f}s)",
                      m_poolSize, m_frameSize / 1024, poolMB, poolDt);
        }

        m_encoderName = encoderName;
        m_micWavPath = micWavPath;
        m_gameAudioWavPath = gameAudioWavPath;
        m_usingDirectGameAudio = usingDirectGameAudio;
        m_gameAudioUsesFmod = usingFmodGameAudio;
        m_micSessionEnabled = recordMic;
        m_gameAudioSessionEnabled = recordGame;

        // Warn user if using CPU encoder (major performance impact)
        if (encoderName == "libx264") {
            std::string warnMsg = "GPU encoder failed! Using CPU (libx264). "
                                  "Update NVIDIA drivers + FFmpeg for best performance!";
            if (recordMic) {
                warnMsg += " Disable mic in settings to reduce stutter!";
            }
            log::warn("[Rec] Using SOFTWARE encoder (libx264) - update GPU drivers for h264_nvenc!");
            Notification::create(warnMsg, NotificationIcon::Warning, 7.f)->show();
        } else if (encoderName == "mpeg4") {
            std::string warnMsg = "GPU encoder failed. Using fast CPU fallback (mpeg4) for smoother recording; files will be larger.";
            if (recordMic) {
                warnMsg += " Disable mic in settings if the game still lags.";
            }
            log::warn("[Rec] Using fast CPU fallback (mpeg4) because hardware encoders were unavailable");
            Notification::create(warnMsg, NotificationIcon::Warning, 7.f)->show();
        }

        m_state       = State::Active;
        m_frames      = 0;
        m_dropped     = 0;
        m_duped       = 0;
        m_backpressureSkips = 0;
        m_pipeError   = false;
        m_ffmpegOk    = false;
        m_startTime   = std::chrono::steady_clock::now();
        m_lastCapture = m_startTime;
        m_nextCaptureDue = m_startTime;
        m_firstVideoFrameNs.store(0, std::memory_order_release);
        m_stopRequestNs.store(0, std::memory_order_release);
        m_gameAudioLeadSecs = 0.0;
        m_micAudioLeadSecs = 0.0;

        m_writerThread = std::thread([this] { writerLoop(); });

        std::string audioInfo;
        if (recordMic) audioInfo += " +MIC";
        if (recordGame) audioInfo += " +GAME(process)";

        log::info("[Rec] Recording started ({}x{} @{}fps, encoder={}{})",
                  w, h, m_fps, encoderName, audioInfo);
        if (g_gpuIsWeak) {
            log::info("[Rec] Weak GPU optimizations active: queue={} pipe={}MB",
                      m_maxQueue, m_pipeBufSize / (1024 * 1024));
        }
        double totalDt = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - startupT0).count();
        log::info("[Rec] ?? start() completed in {:.2f}s ??", totalDt);
        return true;
    }

    void stop() {
        if (m_state != State::Active) return;
        log::info("[Rec] ?? stop() called (frames={}, dropped={}, duped={}) ??",
                  m_frames.load(), m_dropped.load(), m_duped.load());
        m_stopRequestNs.store(steadyNowNanos(), std::memory_order_release);

        if (m_micSessionEnabled) {
            MicrophoneCapture::get().stop();
            log::info("[Rec] Microphone capture stopped");
        }

        // Stop COM WASAPI game audio capture (if running)
        if (m_usingDirectGameAudio) {
            if (m_gameAudioUsesFmod) {
                FmodGameAudioCapture::get().stop();
                log::info("[Rec] FMOD game audio capture stopped");
            } else {
                GameAudioCapture::get().stop();
                log::info("[Rec] Process loopback game audio capture stopped");
            }
        }

        // Immediately set Saving - blocks new captures, returns GL thread fast
        m_state = State::Saving;
        m_writerCv.notify_all();

        // All heavy work on a background thread (pipe close, FFmpeg wait, notifications)
        std::thread([this] {
            try {
            auto stopT0 = std::chrono::steady_clock::now();

            // 1. Close pipe first so writer cannot block forever on WriteFile
            // when FFmpeg back-pressures near stop.
            // NOTE: Save pipeError BEFORE closing - closing the pipe causes the
            // writer's pending WriteFile to fail, which would falsely set pipeError.
            bool hadPipeErrorDuringRecording = m_pipeError.load(std::memory_order_relaxed);
            if (m_pipe != INVALID_HANDLE_VALUE) {
                CloseHandle(m_pipe);
                m_pipe = INVALID_HANDLE_VALUE;
                log::info("[Rec] stop: pipe closed");
            }

            if (m_writerThread.joinable()) m_writerThread.join();
            log::info("[Rec] stop: writer joined");

            // 3. Wait for FFmpeg to finish (with -shortest, should exit quickly)
            DWORD ffmpegExitCode = 1;
            if (m_proc != INVALID_HANDLE_VALUE) {
                DWORD result = WaitForSingleObject(m_proc, FFMPEG_WAIT_TIMEOUT);
                if (result == WAIT_TIMEOUT) {
                    log::warn("[Rec] FFmpeg timeout on stop, terminating");
                    TerminateProcess(m_proc, 1);
                    WaitForSingleObject(m_proc, 500);
                }
                GetExitCodeProcess(m_proc, &ffmpegExitCode);
                if (ffmpegExitCode != 0) {
                    log::error("[Rec] FFmpeg exited with code: {} (pipe_error_during_rec={})",
                               ffmpegExitCode, hadPipeErrorDuringRecording);
                }
                CloseHandle(m_proc);
                m_proc = INVALID_HANDLE_VALUE;
                log::info("[Rec] stop: ffmpeg done (exit={})", ffmpegExitCode);
            }

            if (m_nulHandle != INVALID_HANDLE_VALUE) {
                CloseHandle(m_nulHandle);
                m_nulHandle = INVALID_HANDLE_VALUE;
            }

            freePool();
            log::info("[Rec] stop: pool freed");
            m_pipeError.store(false);

            int frames = m_frames.load();
            int dropped = m_dropped.load();
            int duped = m_duped.load();
            std::string outPath = m_outPath;
            std::string logPath = m_stderrLogPath;
            bool micOn = m_micSessionEnabled;
            bool gameOn = m_gameAudioSessionEnabled;
            std::string micWavPath = m_micWavPath;
            std::string wavPath = m_gameAudioWavPath;
            int64_t firstVideoFrameNs = m_firstVideoFrameNs.load(std::memory_order_acquire);
            if (firstVideoFrameNs <= 0) firstVideoFrameNs = steadyTimePointToNanos(m_startTime);

            double gameAudioLeadSecs = 0.0;
            if (gameOn) {
                int64_t gameFirstPacketNs = m_gameAudioUsesFmod
                    ? FmodGameAudioCapture::get().firstPacketNs()
                    : GameAudioCapture::get().firstPacketNs();
                gameAudioLeadSecs = computeLeadSeconds(firstVideoFrameNs, gameFirstPacketNs);
            }

            double micAudioLeadSecs = 0.0;
            if (micOn) {
                micAudioLeadSecs = computeLeadSeconds(
                    firstVideoFrameNs,
                    MicrophoneCapture::get().firstPacketNs()
                );
            }

            m_gameAudioLeadSecs = gameAudioLeadSecs;
            m_micAudioLeadSecs = micAudioLeadSecs;
            bool haveGameWav = !wavPath.empty() && wavHasAudioData(wavPath);
            bool haveMicWav = !micWavPath.empty() && wavHasAudioData(micWavPath);

            if (haveGameWav || haveMicWav) {
                log::info("[Rec] Audio trim offsets: game={:.3f}s mic={:.3f}s",
                          gameAudioLeadSecs, micAudioLeadSecs);
            }

            // 4a. Mux optional WASAPI tracks into the final MP4.
            if (haveGameWav || haveMicWav) {
                log::info("[Rec] stop: starting mux...");
                std::error_code ec;
                if (fs::exists(utf8Path(outPath), ec)) {
                    std::string tmpOut = outPath + ".muxing.mp4";
                    std::string ffmpegExe = locateFFmpeg();

                    if (!ffmpegExe.empty() && (haveGameWav || haveMicWav)) {
                        int nextInputIdx = 1;
                        int gameInputIdx = -1;
                        int micInputIdx = -1;
                        std::ostringstream muxCmd;
                        muxCmd << "\"" << ffmpegExe << "\""
                               << " -y -hide_banner -loglevel error"
                               << " -i \"" << outPath << "\"";

                        if (haveGameWav) {
                            // -ss trims the audio lead (time captured before video frame 0)
                            if (gameAudioLeadSecs > 0.001) {
                                char ssBuf[32];
                                snprintf(ssBuf, sizeof(ssBuf), "%.3f", gameAudioLeadSecs);
                                muxCmd << " -ss " << ssBuf;
                            }
                            muxCmd << " -i \"" << wavPath << "\"";
                            gameInputIdx = nextInputIdx++;
                        }
                        if (haveMicWav) {
                            if (micAudioLeadSecs > 0.001) {
                                char ssBuf[32];
                                snprintf(ssBuf, sizeof(ssBuf), "%.3f", micAudioLeadSecs);
                                muxCmd << " -ss " << ssBuf;
                            }
                            muxCmd << " -i \"" << micWavPath << "\"";
                            micInputIdx = nextInputIdx++;
                        }

                        int micVol = Mod::get()->getSettingValue<int>("mic-volume");
                        if (micVol < 0) micVol = 0;
                        if (micVol > 300) micVol = 300;
                        double micVolFactor = micVol / 100.0;
                        bool needMix = (gameInputIdx >= 0 && micInputIdx >= 0);
                        bool needFilterComplex = needMix || (micInputIdx >= 0 && micVol != 100);

                        if (needFilterComplex) {
                            muxCmd << " -filter_complex \"";

                            if (gameInputIdx >= 0 && micInputIdx >= 0) {
                                muxCmd << "[" << micInputIdx << ":a]volume=" << micVolFactor
                                       << ",asplit=2[mic_mix][mic_track];"
                                       << "[" << gameInputIdx << ":a][mic_mix]"
                                       << "amix=inputs=2:duration=longest:dropout_transition=0:normalize=0[mix]";
                            } else if (micInputIdx >= 0 && micVol != 100) {
                                muxCmd << "[" << micInputIdx << ":a]volume=" << micVolFactor << "[mic_track]";
                            }

                            muxCmd << "\"";
                        }

                        muxCmd << " -map 0:v";
                        int audioTrack = 0;

                        if (needMix) {
                            muxCmd << " -map \"[mix]\""
                                   << " -c:a:" << audioTrack << " aac"
                                   << " -ar:a:" << audioTrack << " 48000"
                                   << " -ac:a:" << audioTrack << " 2"
                                   << " -b:a:" << audioTrack << " 192k"
                                   << " -metadata:s:a:" << audioTrack << " title=\"Game + Mic\"";
                            audioTrack++;
                        }

                        if (gameInputIdx >= 0) {
                            muxCmd << " -map " << gameInputIdx << ":a"
                                   << " -c:a:" << audioTrack << " aac"
                                   << " -ar:a:" << audioTrack << " 48000"
                                   << " -ac:a:" << audioTrack << " 2"
                                   << " -b:a:" << audioTrack << " 192k"
                                   << " -metadata:s:a:" << audioTrack << " title=\"Game Audio\"";
                            audioTrack++;
                        }
                        if (micInputIdx >= 0) {
                            if (needFilterComplex && (micVol != 100 || needMix)) {
                                muxCmd << " -map \"[mic_track]\"";
                            } else {
                                muxCmd << " -map " << micInputIdx << ":a";
                            }
                            muxCmd << " -c:a:" << audioTrack << " aac"
                                   << " -ar:a:" << audioTrack << " 48000"
                                   << " -ac:a:" << audioTrack << " 2"
                                   << " -b:a:" << audioTrack << " 160k"
                                   << " -metadata:s:a:" << audioTrack << " title=\"Microphone\"";
                        }

                        muxCmd << " -c:v copy -movflags +faststart -f mp4"
                               << " \"" << tmpOut << "\"";
                        log::info("[Rec] Muxing WASAPI audio WAVs into MP4...");
                        log::info("[Rec] Mux CMD: {}", muxCmd.str());

                        std::string stderrLogPath = outPath + ".mux.stderr.log";

                        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
                        HANDLE hStderr = CreateFileA(stderrLogPath.c_str(),
                            GENERIC_WRITE, FILE_SHARE_WRITE | FILE_SHARE_READ, &sa,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                        if (hStderr == INVALID_HANDLE_VALUE) {
                            hStderr = CreateFileA("NUL", GENERIC_WRITE,
                                FILE_SHARE_WRITE | FILE_SHARE_READ, &sa,
                                OPEN_EXISTING, 0, nullptr);
                        }
                        HANDLE hNul = CreateFileA("NUL", GENERIC_WRITE,
                            FILE_SHARE_WRITE | FILE_SHARE_READ, &sa,
                            OPEN_EXISTING, 0, nullptr);
                        PROCESS_INFORMATION pi{};
                        BOOL ok = spawnProcessW(muxCmd.str(), hNul, hNul, hStderr, 0, &pi);
                        CloseHandle(hNul);
                        CloseHandle(hStderr);
                        if (ok) {
                            CloseHandle(pi.hThread);
                            DWORD waitRes = WaitForSingleObject(pi.hProcess, 60000);
                            if (waitRes == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 1);
                            DWORD muxExit = 0;
                            GetExitCodeProcess(pi.hProcess, &muxExit);
                            CloseHandle(pi.hProcess);

                            if (muxExit == 0 && fs::exists(utf8Path(tmpOut), ec)) {
                                fs::remove(utf8Path(outPath), ec);
                                fs::rename(utf8Path(tmpOut), utf8Path(outPath), ec);
                                log::info("[Rec] stop: mux OK");
                                fs::remove(utf8Path(stderrLogPath), ec);
                            } else {
                                int32_t signedExit = static_cast<int32_t>(muxExit);
                                log::warn("[Rec] stop: mux failed (exit={})", signedExit);
                                std::ifstream errFile(utf8Path(stderrLogPath));
                                if (errFile) {
                                    std::string errContent((std::istreambuf_iterator<char>(errFile)),
                                                           std::istreambuf_iterator<char>());
                                    if (!errContent.empty()) {
                                        size_t maxLen = 500;
                                        if (errContent.size() > maxLen)
                                            errContent.resize(maxLen);
                                        log::warn("[Rec] mux stderr: {}", errContent);
                                    }
                                    errFile.close();
                                }
                                fs::remove(utf8Path(tmpOut), ec);
                                fs::remove(utf8Path(stderrLogPath), ec);
                            }
                        } else {
                            fs::remove(utf8Path(stderrLogPath), ec);
                        }
                    }
                }
                log::info("[Rec] stop: mux done");
            }

            {
                std::error_code ec;
                if (!micWavPath.empty()) fs::remove(utf8Path(micWavPath), ec);
                if (!wavPath.empty()) fs::remove(utf8Path(wavPath), ec);
            }

            micOn = micOn && haveMicWav;
            gameOn = gameOn && haveGameWav;

            // 4. Build notification and finalize on main thread
            Loader::get()->queueInMainThread([=] {
                // Determine recording success primarily by output file state.
                // FFmpeg exit code is not the main success signal here because
                // audio is muxed in a second step and the final file state matters more.
                std::string fname = utf16ToUtf8(utf8Path(outPath).filename().wstring());
                std::error_code ec;
                auto fsize = fs::file_size(utf8Path(outPath), ec);
                bool fileValid = (!ec && fsize > 0);

                if (fileValid) {
                    std::string desc = "Saved: " + fname;
                    bool hasAudio = micOn || gameOn;
                    if (hasAudio) {
                        desc += " [";
                        if (gameOn) desc += "GAME";
                        if (gameOn && micOn) desc += "+";
                        if (micOn) desc += "MIC";
                        desc += " audio]";
                    }
                    Notification::create(desc, NotificationIcon::Success, 5.f)->show();
                    fs::remove(utf8Path(logPath), ec);
                } else {
                    // No valid file - recording truly failed
                    fs::remove(utf8Path(outPath), ec);
                    Notification::create(
                        "Recording FAILED! Check ffmpeg_log.txt",
                        NotificationIcon::Error, 5.f
                    )->show();
                }

                // Transition to Idle - recording system fully available again
                 ScreenRecorder::get().m_state = State::Idle;
                 ScreenRecorder::get().m_usingDirectGameAudio = false;
                 ScreenRecorder::get().m_gameAudioUsesFmod = false;
                 log::info("[Rec] Recording stopped. Frames: {}, Dropped: {}, Duped: {}",
                           frames, dropped, duped);
             });
            } catch (const std::exception& e) {
                log::error("[Rec] CRASH in stop thread: {}", e.what());
                ScreenRecorder::get().m_state = State::Idle;
            } catch (...) {
                log::error("[Rec] CRASH in stop thread (unknown exception)");
                ScreenRecorder::get().m_state = State::Idle;
            }
        }).detach();
    }

private:
    int m_w{}, m_h{};
    int m_quality{2};
    int m_encoderSetting{1};
    int m_gpuDownscaleMode{0};
    bool m_micEnabled{false};
    std::string m_micDevice;
    bool m_gameAudioEnabled{false};
    bool m_showUnsupportedOsWarning{true};
    bool m_autoDisableMicOnError{true};
    bool m_micSessionEnabled{false};
    bool m_gameAudioSessionEnabled{false};
    std::string m_encoderName;
    std::string m_outPath;
    std::string m_micWavPath;
    std::string m_gameAudioWavPath;
    bool m_usingDirectGameAudio{false};
    bool m_gameAudioUsesFmod{false};
    double m_gameAudioLeadSecs{0.0};
    double m_micAudioLeadSecs{0.0};

    void readSettings() {
        m_quality         = static_cast<int>(Mod::get()->getSettingValue<int64_t>("quality"));
        m_fps             = static_cast<int>(Mod::get()->getSettingValue<int64_t>("fps"));
        m_encoderSetting  = static_cast<int>(Mod::get()->getSettingValue<int64_t>("encoder"));
        m_gpuDownscaleMode = normalizeGpuDownscaleMode(static_cast<int>(Mod::get()->getSettingValue<int64_t>("gpu-downscale")));
        m_captureInterval = 1.0 / static_cast<double>(m_fps);
        m_micEnabled      = Mod::get()->getSettingValue<bool>("mic-enabled");
        m_micDevice       = Mod::get()->getSettingValue<std::string>("mic-device");
        m_gameAudioEnabled = Mod::get()->getSettingValue<bool>("game-audio-enabled");
        m_showUnsupportedOsWarning = Mod::get()->getSettingValue<bool>("show-unsupported-os-warning");
        m_autoDisableMicOnError = Mod::get()->getSettingValue<bool>("auto-disable-mic-on-error");
        m_logPerformanceDegradation = Mod::get()->getSettingValue<bool>("log-performance-degradation");
        m_micSessionEnabled = false;
        m_gameAudioSessionEnabled = false;
        m_micWavPath.clear();
        m_gameAudioWavPath.clear();
        m_gameAudioUsesFmod = false;
        m_gameAudioLeadSecs = 0.0;
        m_micAudioLeadSecs = 0.0;
        m_stopRequestNs.store(0, std::memory_order_release);
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
        m_uiAccum += dt;
        if (m_uiAccum < 0.1f) return;
        m_uiAccum = 0.f;
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
    float m_uiAccum{};
};
// ==================================================================
// Global UI state + hotkey handling
// ==================================================================

RecorderIndicator* g_recIndicator = nullptr;
bool g_conflictDetected = false;
bool g_f5Held  = false;
bool g_f8Held  = false;
bool g_f11Held = false;
bool g_f12Held = false;
bool g_indicatorHidden = false;
int  g_frameCounter = 0;
bool g_cachedViewportZero = false;

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

// F12 = Take screenshot (BMP)
static void takeScreenshot() {
    if (g_conflictDetected) return;
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

    std::string fname = utf16ToUtf8(utf8Path(path).filename().wstring());
    Notification::create("Screenshot: " + fname, NotificationIcon::Success, 3.f)->show();
    log::info("[Rec] Screenshot saved: {}", path);
}

static void onF5Pressed() {
    if (g_conflictDetected) {
        Notification::create(
            "Recording disabled: another recorder mod is active! Remove it.",
            NotificationIcon::Error, 3.f
        )->show();
        return;
    }

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
                if (hasGame) msg += " GAME";
                if (hasMic)  msg += " +MIC";
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
        // If another recorder mod is loaded, skip ALL our GL work to prevent
        // ACCESS_VIOLATION crashes from dual-hook PBO/readback conflicts
        if (g_conflictDetected) {
            cocos2d::CCEGLView::swapBuffers();
            return;
        }

        auto& rec = ScreenRecorder::get();

        detectGPU();
        ++g_frameCounter;

        // Initialize shared frame capture on first frame
        if (!g_capture.isInitialized()) {
            g_capture.init();
        }

        // Periodic viewport re-check (avoid polling viewport every frame)
        // If viewport changes during recording -> auto-stop (FFmpeg has fixed resolution)
        if (g_capture.isInitialized() && (g_frameCounter % 240 == 0)) {
            bool viewportChanged = g_capture.reinitIfNeeded();
            if (viewportChanged && rec.isActive()) {
                log::warn("[Rec] Viewport changed during recording - auto-stopping! "
                          "Resolution changes (fullscreen/windowed/resize) require re-recording.");
                detachRecIndicator();
                rec.stop();
                Notification::create(
                    "Recording stopped: window resolution changed! "
                    "Press F5 to start a new recording.",
                    NotificationIcon::Warning, 5.f
                )->show();
            }
        }

        if (rec.isActive() && g_capture.isInitialized() && (g_frameCounter % 60 == 0)) {
            g_cachedViewportZero = g_capture.isViewportZero();
        } else if (!rec.isActive()) {
            g_cachedViewportZero = false;
        }

        // Skip capture if window is minimized (viewport = 0x0)
        bool windowMinimized = rec.isActive() && g_capture.isInitialized() && g_cachedViewportZero;

        // Capture BEFORE swap: reads from GL_BACK (complete frame, no sync stall)
        // Skip if window minimized - viewport is 0x0, GL readback would be garbage
        if (rec.isActive() && g_capture.isInitialized() && !windowMinimized) {
            rec.captureDirectly(g_capture);
        }

        // Swap buffers (after capture)
        cocos2d::CCEGLView::swapBuffers();

        // Pipe error detection - auto-stop
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

        // ?? Hotkeys ??
        bool f5Now = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
        if (f5Now && !g_f5Held) onF5Pressed();
        g_f5Held = f5Now;

        // F8 = Cycle microphone device
        bool f8Now = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
        if (f8Now && !g_f8Held) onF8Pressed();
        g_f8Held = f8Now;

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

        // F12 = Screenshot (reads GL_FRONT after swap - correct for displayed frame)
        bool f12Now = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
        if (f12Now && !g_f12Held) takeScreenshot();
        g_f12Held = f12Now;
    }
};

// ==================================================================
// Mod init
// ==================================================================

static int _gdsr_on_mod_loaded_init = []() {
    geode::ModStateEvent(geode::ModEventType::Loaded, geode::getMod())
        .listen([]() {
            for (auto* mod : Loader::get()->getAllMods()) {
                if (mod == Mod::get()) continue;
                std::string id = mod->getID();
                if (id.find("screenrecorder") != std::string::npos ||
                    id.find("screen-recorder") != std::string::npos ||
                    id.find("screen_recorder") != std::string::npos) {
                    g_conflictDetected = true;
                    log::error("[Rec] CONFLICT DETECTED: \"{}\" ({}) is also installed!",
                               mod->getName(), id);
                    log::error("[Rec] Two screen recorders hooking swapBuffers WILL crash.");
                    log::error("[Rec] Recording is DISABLED. Remove one mod to fix.");
                }
            }

            if (g_conflictDetected) {
                Loader::get()->queueInMainThread([] {
                    Notification::create(
                        "CONFLICT: Another screen recorder mod detected! "
                        "Disable one to prevent crashes. Recording disabled.",
                        NotificationIcon::Error, 10.f
                    )->show();
                });
                return;
            }

            log::info("=== GD Screen Recorder v1.0.0 Release (by FreeOpus666) ===");
            log::info("F5  = Start/Stop recording");
            log::info("F8  = Cycle microphone device");
            log::info("F11 = Toggle indicator");
            log::info("F12 = Screenshot");
            log::info("Game audio: prefers FMOD master tap from GeometryDash.exe; process loopback is fallback only.");

            std::error_code ec;
            std::string recDir = gdDir() + "\\recordings";
            fs::remove(utf8Path(recDir + "\\~ffmpeg_log.txt"), ec);

            logAudioDevices();

            std::string ffmpegPath = locateFFmpeg();
            if (!ffmpegPath.empty()) {
                std::string logDir = recDir;
                std::thread([ffmpegPath, logDir] {
                    prewarmEncoderCache(ffmpegPath, logDir);
                }).detach();
            } else {
                log::warn("[Rec] ffmpeg.exe not found (checked GD folder, ffmpeg\\bin, Geode paths, PATH)");
            }
        }).leak();
    return 0;
}();
