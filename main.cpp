#include <windows.h>
#include <shellapi.h>
#include <powrprof.h>
#include <stdio.h>
#include <stdarg.h>
#include <string>
#include <fstream>
#include <stdexcept>
#include <algorithm>
#include <cmath>
#include <atomic>

#pragma comment(linker, "/SUBSYSTEM:windows /ENTRY:mainCRTStartup")
#pragma comment(linker, "/MANIFESTUAC:\"level='requireAdministrator' uiAccess='false'\"")
#pragma comment(lib, "PowrProf.lib")

// ==========================================
// --- Global Utilities & State ---
// ==========================================

bool g_EnableLogging = true;
char g_AppDirectory[MAX_PATH];
char g_LogPath[MAX_PATH];
FILE* g_LogFile = nullptr;

// Thread-safe flag to tell our main loop to re-apply MSRs.
std::atomic<bool> g_HardwareStateNeedsReset = true;

// ==========================================
// --- Elevation Check ---
// ==========================================

bool IsElevated() {
    BOOL fRet = FALSE;
    HANDLE hToken = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        TOKEN_ELEVATION Elevation;
        DWORD cbSize = sizeof(TOKEN_ELEVATION);
        if (GetTokenInformation(hToken, TokenElevation, &Elevation, sizeof(Elevation), &cbSize)) {
            fRet = Elevation.TokenIsElevated;
        }
    }
    if (hToken) {
        CloseHandle(hToken);
    }
    return fRet;
}

void RelaunchAsAdmin() {
    char szPath[MAX_PATH];
    if (GetModuleFileNameA(NULL, szPath, ARRAYSIZE(szPath))) {
        SHELLEXECUTEINFOA sei = { sizeof(sei) };
        sei.lpVerb = "runas";
        sei.lpFile = szPath;
        sei.hwnd = NULL;
        sei.nShow = SW_NORMAL;

        if (!ShellExecuteExA(&sei)) {
            ExitProcess(1);
        }
    }
}

// ==========================================
// --- OS Event Callbacks ---
// ==========================================

ULONG CALLBACK PowerStateCallback(PVOID Context, ULONG Type, PVOID Setting) {
    if (Type == PBT_APMRESUMESUSPEND) {
        g_HardwareStateNeedsReset = true;
    }
    return 0;
}

// ==========================================
// --- Logging & Mutex ---
// ==========================================

void InitAppDirectory() {
    char buffer[MAX_PATH];
    GetModuleFileNameA(NULL, buffer, MAX_PATH);
    std::string path(buffer);
    std::string dir = path.substr(0, path.find_last_of("\\/") + 1);
    strcpy_s(g_AppDirectory, sizeof(g_AppDirectory), dir.c_str());

    std::string logP = dir + "balancer.log";
    strcpy_s(g_LogPath, sizeof(g_LogPath), logP.c_str());
}

void InitLogging() {
    if (g_EnableLogging) {
        fopen_s(&g_LogFile, g_LogPath, "a");
    }
}

void CloseLogging() {
    if (g_LogFile) {
        fclose(g_LogFile);
        g_LogFile = nullptr;
    }
}

void LogFast(const char* format, ...) {
    if (!g_EnableLogging || !g_LogFile) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_LogFile, "[%04d-%02d-%02d %02d:%02d:%02d] ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    va_list args;
    va_start(args, format);
    vfprintf(g_LogFile, format, args);
    va_end(args);

    fprintf(g_LogFile, "\n");
    fflush(g_LogFile);
}

class SingleInstanceMutex {
private:
    HANDLE hMutex;
public:
    SingleInstanceMutex(const char* mutexName) {
        hMutex = CreateMutexA(NULL, TRUE, mutexName);
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            if (hMutex) { CloseHandle(hMutex); hMutex = NULL; }
            throw std::runtime_error("ALREADY_RUNNING");
        }
    }
    ~SingleInstanceMutex() {
        if (hMutex) { ReleaseMutex(hMutex); CloseHandle(hMutex); }
    }
};

// ==========================================
// --- Hardware Structures ---
// ==========================================

struct PrecalcMSR {
    DWORD eax;
    DWORD edx;
};

struct HardwareState {
    PrecalcMSR voltageOffsets[5];
    PrecalcMSR iccMaxLimits[5];
    PrecalcMSR turboRatios;
};

struct AppConfig {
    double totalSystemBudget = 220.0;
    DWORD cpuMaxWatts = 65;
    DWORD cpuMinWatts = 15;

    DWORD pollingRateMs = 1000;
    DWORD idlePollingRateMs = 5000;     // NEW: Slower polling when GPU is idle
    double gpuIdleWatts = 30.0;         // NEW: Threshold to trigger idle polling
    DWORD timerToleranceMs = 50;

    DWORD_PTR affinityMask = 0;         // NEW: Core pinning mask (0 = OS Default)

    double offsets[5] = { 0.0, 0.0, 0.0, 0.0, 0.0 };
    double iccMax[5] = { 105.0, 105.0, 105.0, 105.0, 105.0 };
    DWORD coreRatios[8] = { 0 };
};

// ==========================================
// --- WinRing0 MSR Manager ---
// ==========================================

const DWORD MSR_RAPL_POWER_UNIT_MULTIPLIER = 8;
typedef BOOL(WINAPI* InitializeOlsFunc)();
typedef void (WINAPI* DeinitializeOlsFunc)();
typedef BOOL(WINAPI* ReadMsrFunc)(DWORD index, PDWORD eax, PDWORD edx);
typedef BOOL(WINAPI* WriteMsrFunc)(DWORD index, DWORD eax, DWORD edx);

class MSRWriter {
private:
    HMODULE hWinRing0;
    InitializeOlsFunc InitializeOls;
    DeinitializeOlsFunc DeinitializeOls;
    ReadMsrFunc Rdmsr;
    WriteMsrFunc Wrmsr;

public:
    MSRWriter() : hWinRing0(nullptr) {
        std::string dllPath = std::string(g_AppDirectory) + "WinRing0x64.dll";
        hWinRing0 = LoadLibraryA(dllPath.c_str());
        if (!hWinRing0) throw std::runtime_error("Failed to load WinRing0x64.dll");

        InitializeOls = (InitializeOlsFunc)GetProcAddress(hWinRing0, "InitializeOls");
        DeinitializeOls = (DeinitializeOlsFunc)GetProcAddress(hWinRing0, "DeinitializeOls");
        Rdmsr = (ReadMsrFunc)GetProcAddress(hWinRing0, "Rdmsr");
        Wrmsr = (WriteMsrFunc)GetProcAddress(hWinRing0, "Wrmsr");

        if (!InitializeOls || !InitializeOls()) throw std::runtime_error("WinRing0 Init Failed.");
    }
    ~MSRWriter() {
        if (DeinitializeOls) DeinitializeOls();
        if (hWinRing0) FreeLibrary(hWinRing0);
    }

    bool SetPackagePowerLimits(DWORD targetWatts) {
        DWORD eax, edx;
        if (!Rdmsr(0x610, &eax, &edx)) return false;
        DWORD targetHex = targetWatts * MSR_RAPL_POWER_UNIT_MULTIPLIER;
        eax = (eax & ~0x7FFF) | (targetHex & 0x7FFF);
        edx = (edx & ~0x7FFF) | (targetHex & 0x7FFF);
        return Wrmsr(0x610, eax, edx);
    }

    void WritePrecalc(DWORD msr, const PrecalcMSR& precalc) {
        Wrmsr(msr, precalc.eax, precalc.edx);
    }

    void GetTurboRatios(DWORD ratios[8]) {
        DWORD eax, edx;
        if (!Rdmsr(0x1AD, &eax, &edx)) return;
        for (int i = 0; i < 4; i++) ratios[i] = (eax >> (i * 8)) & 0xFF;
        for (int i = 0; i < 4; i++) ratios[i + 4] = (edx >> (i * 8)) & 0xFF;
    }
};

// ==========================================
// --- Configuration & Compiler ---
// ==========================================

AppConfig LoadConfig(MSRWriter& cpu) {
    AppConfig cfg;
    cpu.GetTurboRatios(cfg.coreRatios);

    std::string configPath = std::string(g_AppDirectory) + "config.ini";
    std::ifstream file(configPath);

    if (!file.is_open()) {
        std::ofstream newConfig(configPath);

        newConfig << "# ==================================================================\n";
        newConfig << "# Dynamic Power Balancer - Configuration\n";
        newConfig << "# Note: You must restart the program for changes to take effect.\n";
        newConfig << "# ==================================================================\n\n";

        newConfig << "# --- Global Power Settings ---\n";
        newConfig << "# TOTAL_SYSTEM_BUDGET: The absolute maximum wattage your system (CPU + GPU) is allowed to pull.\n";
        newConfig << "TOTAL_SYSTEM_BUDGET=220.0\n\n";

        newConfig << "# CPU_MAX_WATTS: The highest wattage the CPU is allowed to use when the GPU is idle.\n";
        newConfig << "CPU_MAX_WATTS=65\n\n";

        newConfig << "# CPU_MIN_WATTS: The absolute minimum wattage the CPU is allowed to throttle down to under heavy GPU load.\n";
        newConfig << "CPU_MIN_WATTS=15\n\n";

        newConfig << "# --- Engine & Architecture Settings ---\n";
        newConfig << "# POLLING_RATE_MS: How often (in ms) the program checks GPU usage under load.\n";
        newConfig << "POLLING_RATE_MS=1000\n\n";

        newConfig << "# IDLE_POLLING_RATE_MS: How often to poll when the GPU is resting (saves CPU cycles).\n";
        newConfig << "IDLE_POLLING_RATE_MS=5000\n\n";

        newConfig << "# GPU_IDLE_WATTS_THRESHOLD: The GPU wattage below which the program switches to the IDLE_POLLING_RATE.\n";
        newConfig << "GPU_IDLE_WATTS_THRESHOLD=30.0\n\n";

        newConfig << "# TIMER_TOLERANCE_MS: The OS timer coalescing window. Allows the CPU to group wake-ups (50-100ms recommended).\n";
        newConfig << "TIMER_TOLERANCE_MS=50\n\n";

        newConfig << "# AFFINITY_MASK (Hex): Locks the program to specific CPU cores. 0x0 = OS Default (Any core).\n";
        newConfig << "# Example: 0x8000 pins to Core 15. 0x1 pins to Core 0.\n";
        newConfig << "AFFINITY_MASK=0x0\n\n";

        newConfig << "# ENABLE_LOGGING: Set to 1 to enable writing to balancer.log, 0 to disable.\n";
        newConfig << "ENABLE_LOGGING=1\n\n";

        newConfig << "# --- Undervolting (Millivolts) ---\n";
        newConfig << "# Enter values as negative numbers to undervolt (e.g., -50.0). Max offset is -500.0.\n";
        newConfig << "OFFSET_CPU_CORE_MV=0.0\n";
        newConfig << "OFFSET_GPU_MV=0.0\n";
        newConfig << "OFFSET_CACHE_MV=0.0\n";
        newConfig << "OFFSET_SYSTEM_AGENT_MV=0.0\n";
        newConfig << "OFFSET_ANALOG_IO_MV=0.0\n\n";

        newConfig << "# --- Current Limits (Amps) ---\n";
        newConfig << "# IccMax limits for each plane. Hardware default is usually around 105.0A, Max is 511.75A.\n";
        newConfig << "ICCMAX_CPU_CORE_A=105.0\n";
        newConfig << "ICCMAX_GPU_A=105.0\n";
        newConfig << "ICCMAX_CACHE_A=105.0\n";
        newConfig << "ICCMAX_SYSTEM_AGENT_A=105.0\n";
        newConfig << "ICCMAX_ANALOG_IO_A=105.0\n\n";

        newConfig << "# --- Turbo Ratios ---\n";
        newConfig << "# Max CPU multiplier based on the number of active cores (e.g., 40 = 4.0 GHz).\n";
        for (int i = 0; i < 8; ++i) {
            newConfig << "RATIO_" << (i + 1) << "_CORE=" << cfg.coreRatios[i] << "\n";
        }

        return cfg;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        size_t delim = line.find('=');
        if (delim != std::string::npos) {
            std::string key = line.substr(0, delim);
            std::string val = line.substr(delim + 1);
            val.erase(std::remove_if(val.begin(), val.end(), ::isspace), val.end());

            try {
                if (key == "TOTAL_SYSTEM_BUDGET") cfg.totalSystemBudget = std::stod(val);
                else if (key == "CPU_MAX_WATTS") cfg.cpuMaxWatts = std::stoul(val);
                else if (key == "CPU_MIN_WATTS") cfg.cpuMinWatts = std::stoul(val);
                else if (key == "POLLING_RATE_MS") cfg.pollingRateMs = std::stoul(val);
                else if (key == "IDLE_POLLING_RATE_MS") cfg.idlePollingRateMs = std::stoul(val);
                else if (key == "GPU_IDLE_WATTS_THRESHOLD") cfg.gpuIdleWatts = std::stod(val);
                else if (key == "TIMER_TOLERANCE_MS") cfg.timerToleranceMs = std::stoul(val);
                else if (key == "AFFINITY_MASK") cfg.affinityMask = std::stoull(val, nullptr, 16);
                else if (key == "ENABLE_LOGGING") g_EnableLogging = (val == "1" || val == "true" || val == "TRUE");

                else if (key == "OFFSET_CPU_CORE_MV") cfg.offsets[0] = std::stod(val);
                else if (key == "OFFSET_GPU_MV") cfg.offsets[1] = std::stod(val);
                else if (key == "OFFSET_CACHE_MV") cfg.offsets[2] = std::stod(val);
                else if (key == "OFFSET_SYSTEM_AGENT_MV") cfg.offsets[3] = std::stod(val);
                else if (key == "OFFSET_ANALOG_IO_MV") cfg.offsets[4] = std::stod(val);

                else if (key == "ICCMAX_CPU_CORE_A") cfg.iccMax[0] = std::stod(val);
                else if (key == "ICCMAX_GPU_A") cfg.iccMax[1] = std::stod(val);
                else if (key == "ICCMAX_CACHE_A") cfg.iccMax[2] = std::stod(val);
                else if (key == "ICCMAX_SYSTEM_AGENT_A") cfg.iccMax[3] = std::stod(val);
                else if (key == "ICCMAX_ANALOG_IO_A") cfg.iccMax[4] = std::stod(val);

                else if (key.find("RATIO_") == 0 && key.find("_CORE") != std::string::npos) {
                    int coreIndex = std::stoi(key.substr(6, 1)) - 1;
                    if (coreIndex >= 0 && coreIndex < 8) cfg.coreRatios[coreIndex] = std::stoul(val);
                }
            }
            catch (...) {}
        }
    }
    return cfg;
}

HardwareState CompileHardwareState(const AppConfig& cfg) {
    HardwareState state;
    for (int i = 0; i < 5; i++) {
        double off = cfg.offsets[i];
        if (off > 0.0) off = 0.0;
        if (off < -500.0) off = -500.0;
        state.voltageOffsets[i].eax = static_cast<DWORD>(std::round(off * 1.024)) << 21;
        state.voltageOffsets[i].edx = 0x80000011 | (i << 8);

        double amps = cfg.iccMax[i];
        if (amps < 0.0) amps = 0.0;
        if (amps > 511.75) amps = 511.75;
        state.iccMaxLimits[i].eax = static_cast<DWORD>(std::round(amps * 4.0));
        state.iccMaxLimits[i].edx = 0x80000017 | (i << 8);
    }
    DWORD r[8];
    for (int i = 0; i < 8; i++) r[i] = (cfg.coreRatios[i] > 255) ? 255 : cfg.coreRatios[i];
    state.turboRatios.eax = (r[3] << 24) | (r[2] << 16) | (r[1] << 8) | r[0];
    state.turboRatios.edx = (r[7] << 24) | (r[6] << 16) | (r[5] << 8) | r[4];

    return state;
}

// ==========================================
// --- NVML Manager ---
// ==========================================

#define NVML_SUCCESS 0
typedef int nvmlReturn_t;
typedef struct nvmlDevice_st* nvmlDevice_t;
typedef nvmlReturn_t(*nvmlInit_t)();
typedef nvmlReturn_t(*nvmlDeviceGetHandleByIndex_t)(unsigned int, nvmlDevice_t*);
typedef nvmlReturn_t(*nvmlDeviceGetPowerUsage_t)(nvmlDevice_t, unsigned int*);
typedef nvmlReturn_t(*nvmlShutdown_t)();

class NVMLReader {
private:
    HMODULE hNvml;
    nvmlDevice_t gpuHandle;
    nvmlDeviceGetPowerUsage_t nvmlGetPower;
    nvmlShutdown_t nvmlShutdown;

public:
    NVMLReader() : hNvml(nullptr), gpuHandle(nullptr) {
        hNvml = LoadLibraryA("C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll");
        if (!hNvml) hNvml = LoadLibraryA("nvml.dll");
        if (!hNvml) throw std::runtime_error("Failed to load nvml.dll");

        auto nvmlInit = (nvmlInit_t)GetProcAddress(hNvml, "nvmlInit_v2");
        auto nvmlGetHandle = (nvmlDeviceGetHandleByIndex_t)GetProcAddress(hNvml, "nvmlDeviceGetHandleByIndex_v2");
        nvmlGetPower = (nvmlDeviceGetPowerUsage_t)GetProcAddress(hNvml, "nvmlDeviceGetPowerUsage");
        nvmlShutdown = (nvmlShutdown_t)GetProcAddress(hNvml, "nvmlShutdown");

        if (!nvmlInit || nvmlInit() != NVML_SUCCESS || nvmlGetHandle(0, &gpuHandle) != NVML_SUCCESS) {
            throw std::runtime_error("Failed to initialize NVML");
        }
    }
    ~NVMLReader() {
        if (nvmlShutdown) nvmlShutdown();
        if (hNvml) FreeLibrary(hNvml);
    }

    unsigned int GetPowerMilliWatts() {
        unsigned int powerMw = 0;
        if (nvmlGetPower(gpuHandle, &powerMw) == NVML_SUCCESS) return powerMw;
        return 0;
    }
};

// ==========================================
// --- Core Application Entry ---
// ==========================================

int main() {
    if (!IsElevated()) {
        RelaunchAsAdmin();
        return 0;
    }

    HPOWERNOTIFY hPowerNotify = NULL;
    HANDLE hTimer = NULL;

    try {
        SingleInstanceMutex appMutex("Global\\DynamicPowerBalancer_SingleInstanceMutex");
        InitAppDirectory();

        MSRWriter cpu;
        AppConfig config = LoadConfig(cpu);
        HardwareState precompiledState = CompileHardwareState(config);
        NVMLReader gpu;

        InitLogging();
        LogFast("Service Started. Budget: %.1fW | Max CPU: %luW | Min CPU: %luW",
            config.totalSystemBudget, config.cpuMaxWatts, config.cpuMinWatts);

        // --- 1. Register Sleep/Wake Callbacks ---
        DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS powerParams = { 0 };
        powerParams.Callback = PowerStateCallback;
        PowerRegisterSuspendResumeNotification(DEVICE_NOTIFY_CALLBACK, &powerParams, &hPowerNotify);

        // Pre-calculate thresholds to avoid floating-point math in the main loop
        unsigned int totalBudgetMw = static_cast<unsigned int>(config.totalSystemBudget * 1000.0);
        unsigned int idleThresholdMw = static_cast<unsigned int>(config.gpuIdleWatts * 1000.0);
        DWORD currentAppliedCpuLimit = 0;

        // --- 2. OS Task Scheduling Optimizations ---
        SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);

        if (config.affinityMask != 0) {
            if (SetProcessAffinityMask(GetCurrentProcess(), config.affinityMask)) {
                LogFast("Core Affinity applied: Mask 0x%llX", config.affinityMask);
            }
            else {
                LogFast("Warning: Failed to set Core Affinity (Invalid mask?).");
            }
        }

        // --- 3. Memory Paging Optimizations ---
        // Purge unnecessary initialization data from physical RAM
        SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);

        // Lock the hot-path variables strictly into physical RAM so they never page to disk
        VirtualLock(&config, sizeof(AppConfig));
        VirtualLock(&precompiledState, sizeof(HardwareState));
        VirtualLock(&totalBudgetMw, sizeof(totalBudgetMw));
        VirtualLock(&idleThresholdMw, sizeof(idleThresholdMw));
        VirtualLock(&currentAppliedCpuLimit, sizeof(currentAppliedCpuLimit));

        // Create Timer for Coalescing
        hTimer = CreateWaitableTimerEx(NULL, NULL, 0, TIMER_ALL_ACCESS);
        if (!hTimer) {
            LogFast("Warning: Failed to create waitable timer. System will fall back to Sleep().");
        }

        // --- The Hot Loop ---
        while (true) {
            if (g_HardwareStateNeedsReset) {
                for (int i = 0; i < 5; i++) {
                    cpu.WritePrecalc(0x150, precompiledState.voltageOffsets[i]);
                    cpu.WritePrecalc(0x150, precompiledState.iccMaxLimits[i]);
                }
                cpu.WritePrecalc(0x1AD, precompiledState.turboRatios);
                g_HardwareStateNeedsReset = false;
                LogFast("Hardware states applied (Startup/Wake Event).");
            }

            unsigned int gpuMw = gpu.GetPowerMilliWatts();
            DWORD targetCpuWatts;

            if (totalBudgetMw > gpuMw) {
                targetCpuWatts = (totalBudgetMw - gpuMw) / 1000;
            }
            else {
                targetCpuWatts = config.cpuMinWatts;
            }

            if (targetCpuWatts < config.cpuMinWatts) targetCpuWatts = config.cpuMinWatts;
            if (targetCpuWatts > config.cpuMaxWatts) targetCpuWatts = config.cpuMaxWatts;

            if (targetCpuWatts != currentAppliedCpuLimit) {
                if (cpu.SetPackagePowerLimits(targetCpuWatts)) {
                    currentAppliedCpuLimit = targetCpuWatts;
                    LogFast("Shifted -> CPU Target: %luW | GPU Measured: %u.%03uW",
                        targetCpuWatts, gpuMw / 1000, gpuMw % 1000);
                }
            }

            // --- 4. Adaptive Polling & Timer Coalescing ---
            // If GPU wattage is below our threshold, drastically slow down the polling rate
            DWORD currentSleepTimeMs = (gpuMw < idleThresholdMw) ? config.idlePollingRateMs : config.pollingRateMs;

            if (hTimer) {
                LARGE_INTEGER dueTime;
                // Convert milliseconds to 100-nanosecond intervals (negative for relative time)
                dueTime.QuadPart = -(static_cast<LONGLONG>(currentSleepTimeMs) * 10000LL);

                SetWaitableTimerEx(hTimer, &dueTime, 0, NULL, NULL, NULL, config.timerToleranceMs);
                WaitForSingleObject(hTimer, INFINITE);
            }
            else {
                Sleep(currentSleepTimeMs);
            }
        }

    }
    catch (const std::exception& e) {
        if (std::string(e.what()) != "ALREADY_RUNNING") {
            InitLogging();
            LogFast("Fatal Error: %s", e.what());
        }
    }

    if (hPowerNotify) PowerUnregisterSuspendResumeNotification(hPowerNotify);
    if (hTimer) CloseHandle(hTimer);
    CloseLogging();

    return 0;
}