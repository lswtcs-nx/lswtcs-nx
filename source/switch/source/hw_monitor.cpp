// [CLOCK] lines (LSWTCS_CLOCKLOG, seconds between samples, default 5, 0 = off): the clocks the console really runs at,
// GPU load, temperature and power. Read-only: nothing here sets a clock. Every number so far was taken under an unknown
// clock, and whether a heavy frame is limited by the GPU depends on it. Services that do not answer are logged once and
// their fields stay empty.
#include <switch.h>
#include <cstdio>
#include <cstdlib>

namespace {
struct ClockSource {
    const char* name;
    PcvModule pcv;
    PcvModuleId id;
    ClkrstSession session;
    bool open;
};
ClockSource g_clocks[3] = {{"cpu", PcvModule_CpuBus, PcvModuleId_CpuBus, {}, false},
                           {"gpu", PcvModule_GPU, PcvModuleId_GPU, {}, false},
                           {"emc", PcvModule_EMC, PcvModuleId_EMC, {}, false}};
int g_period_s = 5;
bool g_use_clkrst, g_have_clocks, g_have_ts, g_have_psm, g_have_apm;
u32 g_gpu_fd;
bool g_have_gpu_load;
TsSession g_ts_soc, g_ts_pcb;
bool g_ts_sessions;

void init_services() {
    g_use_clkrst = hosversionAtLeast(8, 0, 0);
    const Result clocks = g_use_clkrst ? clkrstInitialize() : pcvInitialize();
    g_have_clocks = R_SUCCEEDED(clocks);
    if (g_have_clocks && g_use_clkrst)
        for (ClockSource& c : g_clocks) c.open = R_SUCCEEDED(clkrstOpenSession(&c.session, c.id, 3));
    const Result ts = tsInitialize();
    g_have_ts = R_SUCCEEDED(ts);
    // Session API from 10.0.0; the older call stops at 13.2.1. TMP451: external = SoC, internal = PCB (ts.h).
    if (g_have_ts && hosversionAtLeast(10, 0, 0))
        g_ts_sessions = R_SUCCEEDED(tsOpenSession(&g_ts_soc, TsDeviceCode_LocationExternal)) &&
                        R_SUCCEEDED(tsOpenSession(&g_ts_pcb, TsDeviceCode_LocationInternal));
    const Result psm = psmInitialize();
    g_have_psm = R_SUCCEEDED(psm);
    const Result apm = apmInitialize();
    g_have_apm = R_SUCCEEDED(apm);
    Result gpu = nvInitialize();
    if (R_SUCCEEDED(gpu)) gpu = nvOpen(&g_gpu_fd, "/dev/nvhost-ctrl-gpu");
    g_have_gpu_load = R_SUCCEEDED(gpu);
    printf("[CLOCK] services: clocks 0x%X (%s), temperature 0x%X, power 0x%X, apm 0x%X, gpu load 0x%X, firmware %u.%u.%u\n", clocks,
           g_use_clkrst ? "clkrst" : "pcv", ts, psm, apm, gpu, HOSVER_MAJOR(hosversionGet()), HOSVER_MINOR(hosversionGet()),
           HOSVER_MICRO(hosversionGet()));
    fflush(stdout);
}

double clock_mhz(ClockSource& c) {
    u32 hz = 0;
    if (!g_have_clocks) return -1;
    const Result rc = g_use_clkrst ? (c.open ? clkrstGetClockRate(&c.session, &hz) : 1) : pcvGetClockRate(c.pcv, &hz);
    return R_SUCCEEDED(rc) ? hz / 1e6 : -1;
}

void sample() {
    const double cpu = clock_mhz(g_clocks[0]), gpu = clock_mhz(g_clocks[1]), emc = clock_mhz(g_clocks[2]);
    u32 load = 0;   // NVGPU_GPU_IOCTL_PMU_GET_GPU_LOAD: per mille, as read by sys-clk's and Status Monitor's overlays
    const bool have_load = g_have_gpu_load && R_SUCCEEDED(nvIoctl(g_gpu_fd, 0x80044715, &load));
    float soc = -1, pcb = -1;
    if (g_ts_sessions) {
        if (R_FAILED(tsSessionGetTemperature(&g_ts_soc, &soc))) soc = -1;
        if (R_FAILED(tsSessionGetTemperature(&g_ts_pcb, &pcb))) pcb = -1;
    } else if (g_have_ts) {
        s32 v = 0;
        if (R_SUCCEEDED(tsGetTemperatureMilliC(TsLocation_External, &v))) soc = v / 1000.0f;
        if (R_SUCCEEDED(tsGetTemperatureMilliC(TsLocation_Internal, &v))) pcb = v / 1000.0f;
    }
    u32 battery = 0;
    PsmChargerType charger = PsmChargerType_Unconnected;
    if (g_have_psm) { psmGetBatteryChargePercentage(&battery); psmGetChargerType(&charger); }
    ApmPerformanceMode mode = ApmPerformanceMode_Invalid;
    u32 config = 0;
    if (g_have_apm) {
        apmGetPerformanceMode(&mode);
        apmGetPerformanceConfiguration(mode, &config);
    }
    printf("[CLOCK] t=%llums cpu=%.1f gpu=%.1f emc=%.1f MHz gpu_load=%.1f%% soc=%.1fC pcb=%.1fC battery=%u%% charger=%d "
           "op=%s apm_mode=%d apm_config=0x%08X\n", (unsigned long long)(armTicksToNs(armGetSystemTick()) / 1000000ull), cpu, gpu, emc,
           have_load ? load / 10.0 : -1.0, soc, pcb, battery, int(charger),
           appletGetOperationMode() == AppletOperationMode_Handheld ? "handheld" : "docked", int(mode), config);
    fflush(stdout);
}

void monitor_main(void*) {
    init_services();
    for (;;) {
        sample();
        svcSleepThread(int64_t(g_period_s) * 1000000000ll);
    }
}
}  // namespace

extern "C" void lsw_hw_monitor_start() {
    if (const char* e = getenv("LSWTCS_CLOCKLOG")) g_period_s = atoi(e);
    if (g_period_s <= 0) return;
    static Thread t;
    // Lowest priority in normal use, on the core with the most room.
    if (R_SUCCEEDED(threadCreate(&t, monitor_main, nullptr, nullptr, 0x10000, 0x3A, 2))) threadStart(&t);
}
