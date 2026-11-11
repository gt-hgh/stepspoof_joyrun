// stepspoof_joyrun v1.2.0 — 纯 native Zygisk 模块
// 目标：com.huachenjie.shandong_school（闪动校园）
// 功能：
//   1) 加速度波形仿真（FuckRun 设计：X=0, Y=1.5·cos, Z=9.81+3.5·sin）
//      驱动 App 自研带通滤波计步算法（h9.d）产生步数
//   2) STEP_COUNTER / STEP_DETECTOR 事件改写（App 注册硬件计步器时生效）
//   3) 步频仿真线程：在 [min, max] 区间随机锁定步频，整段稳定不抖动
//   4) 运行日志：同时写 logcat 与 /sdcard/Download/stepspoof_joyrun.log
//      （2MB 自动轮转到 .old；/sdcard 不可写时降级到 App 私有目录）
//   5) 本地 HTTP API（127.0.0.1:8888）供 KSU WebUI 通过 root shell nc 访问
//   6) conf_watch_loop：轮询 App 私有目录 stepspoof.conf，断连后持久化生效
// 不依赖 LSPosed / 不建 ClassLoader / 不加载内存 dex。
//
// Hook 路径：
//   Android ≤13：PLT hook ASensorEventQueue_getEvents（libandroid_runtime.so）
//   Android 14+：inline hook libsensor.so::SensorEventQueue::read（mprotect 路线）

#include <sys/types.h>   // dev_t / ino_t —— zygisk.hpp 依赖，必须在它之前

#include "zygisk.hpp"

#include <android/log.h>
#include <android/sensor.h>

#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <malloc.h>
#include <netinet/in.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#define LOG_TAG "stepspoof"

// 目标包名（日志降级路径、控制通道路径都依赖它，必须在最前定义）
#define TARGET_PKG_STR "com.huachenjie.shandong_school"

// ---------------------------------------------------------------------------
// 日志：同时输出到 logcat 与文件
// 文件路径 /sdcard/Download/stepspoof_joyrun.log（v1.2.0 起）
// 这样用户不用 adb 也能直接拿到日志。写失败时自动降级到 App 私有目录。
// ---------------------------------------------------------------------------
static int g_log_fd = -1;
static long long g_log_bytes = 0;
static constexpr long long kLogMaxBytes = 2 * 1024 * 1024;   // 2MB 自动轮转

// 由 log_open() 在首次写入时确定实际可用路径
static char g_log_path[256] = {0};

static void log_open() {
    if (g_log_fd >= 0) return;

    // App 进程的 uid 对 /sdcard 需要经 MediaProvider 授权，直接用 open 常失败；
    // 先试 /sdcard/Download（多数 ROM 对 app 域已放宽），失败再试私有目录。
    const char *cands[] = {
        "/sdcard/Download/stepspoof_joyrun.log",
        "/storage/emulated/0/Download/stepspoof_joyrun.log",
        "/storage/self/primary/Download/stepspoof_joyrun.log",
    };
    for (const char *p : cands) {
        int fd = open(p, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666);
        if (fd >= 0) {
            g_log_fd = fd;
            strncpy(g_log_path, p, sizeof(g_log_path) - 1);
            g_log_path[sizeof(g_log_path) - 1] = 0;
            struct stat st {};
            if (fstat(fd, &st) == 0) g_log_bytes = (long long)st.st_size;
            return;
        }
    }

    // 兜底：App 私有目录（root 与 App 自身都可读，用户可用文件管理器 root 模式取）
    char fallback[256];
    snprintf(fallback, sizeof(fallback),
             "/data/data/%s/files/stepspoof.log", TARGET_PKG_STR);
    int fd = open(fallback, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666);
    if (fd >= 0) {
        g_log_fd = fd;
        strncpy(g_log_path, fallback, sizeof(g_log_path) - 1);
        g_log_path[sizeof(g_log_path) - 1] = 0;
        struct stat st {};
        if (fstat(fd, &st) == 0) g_log_bytes = (long long)st.st_size;
    }
}

static void log_close_and_rotate_if_needed() {
    if (g_log_fd < 0) return;
    if (g_log_bytes < kLogMaxBytes) return;

    close(g_log_fd);
    g_log_fd = -1;
    g_log_bytes = 0;

    // 轮转：把当前文件改名成 .old，再重新打开新建
    char old_path[300];
    snprintf(old_path, sizeof(old_path), "%s.old", g_log_path);
    rename(g_log_path, old_path);
    log_open();
}

// 统一写入：logcat + 文件
static void log_v(int prio, const char *fmt, va_list ap) {
    // 1) logcat
    va_list ap2;
    va_copy(ap2, ap);
    __android_log_vprint(prio, LOG_TAG, fmt, ap2);
    va_end(ap2);

    // 2) 文件
    log_open();
    if (g_log_fd < 0) return;

    char line[1024];
    char stamp[32];
    struct timespec ts {};
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tmv {};
    time_t sec = ts.tv_sec;
    localtime_r(&sec, &tmv);
    snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d.%03ld",
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ts.tv_nsec / 1000000);
    static const char prio_ch[] = {'V', 'D', 'I', 'W', 'E', 'F'};
    int ci = (prio >= ANDROID_LOG_VERBOSE && prio <= ANDROID_LOG_FATAL)
             ? (prio - ANDROID_LOG_VERBOSE) : 2;

    int head = snprintf(line, sizeof(line), "%s %c/stepspoof: ", stamp, prio_ch[ci]);
    int body = vsnprintf(line + head, sizeof(line) - (size_t)head - 2, fmt, ap);
    if (body < 0) return;
    int total = head + body;
    if (total > (int)sizeof(line) - 2) total = (int)sizeof(line) - 2;
    line[total++] = '\n';
    ssize_t wr = write(g_log_fd, line, (size_t)total);
    if (wr > 0) g_log_bytes += wr;
    log_close_and_rotate_if_needed();
}

#if defined(__clang__)
#define LOGFMT(fmt_idx) __attribute__((format(printf, fmt_idx, fmt_idx + 1)))
#else
#define LOGFMT(fmt_idx)
#endif

static void log_d(const char *fmt, ...) LOGFMT(1) {
    va_list ap; va_start(ap, fmt); log_v(ANDROID_LOG_DEBUG, fmt, ap); va_end(ap);
}
static void log_i(const char *fmt, ...) LOGFMT(1) {
    va_list ap; va_start(ap, fmt); log_v(ANDROID_LOG_INFO, fmt, ap); va_end(ap);
}
static void log_w(const char *fmt, ...) LOGFMT(1) {
    va_list ap; va_start(ap, fmt); log_v(ANDROID_LOG_WARN, fmt, ap); va_end(ap);
}
static void log_e(const char *fmt, ...) LOGFMT(1) {
    va_list ap; va_start(ap, fmt); log_v(ANDROID_LOG_ERROR, fmt, ap); va_end(ap);
}

#define LOGD(...) log_d(__VA_ARGS__)
#define LOGI(...) log_i(__VA_ARGS__)
#define LOGW(...) log_w(__VA_ARGS__)
#define LOGE(...) log_e(__VA_ARGS__)

static const char *TARGET_PKG = TARGET_PKG_STR;
static constexpr size_t PAGE_SZ = 4096;

// ---------------------------------------------------------------------------
// 运行配置（进程内）
// ---------------------------------------------------------------------------
struct Config {
    std::atomic<bool> enabled{true};
    std::atomic<long long> step_offset{0};
    std::atomic<bool> absolute_mode{false};
    std::atomic<long long> absolute_value{0};
    // 步频仿真
    std::atomic<bool> sim_active{false};
    std::atomic<int> sim_rate_min{60};    // 步/分钟
    std::atomic<int> sim_rate_max{120};   // 步/分钟
    std::atomic<bool> sim_accel{true};    // 是否合成加速度波形（自研计步器的 App 需要）
    // GPS 联动：闪动校园结算时拿步数/距离/GPS 轨迹做一致性校验。
    // 步长决定「同一速度下应有的步频」，也决定「同一距离下应有的步数」。
    std::atomic<int> stride_cm{110};      // 步长（厘米），跑步常见 100–130
};

static Config g_cfg;

// 仿真小数累加器（只在 sim_loop 线程访问）
static double sim_accum = 0.0;

// ---------------------------------------------------------------------------
// 原始函数指针
// ---------------------------------------------------------------------------
typedef int (*get_events_fn)(ASensorEventQueue *, ASensorEvent *, size_t);
static get_events_fn g_orig_get_events = nullptr;
static std::atomic<long long> g_hook_hits{0};
static std::atomic<long long> g_patched_events{0};
// 仿真产生的步数（与用户设置的 step_offset 分离，避免被配置回写覆盖）
static std::atomic<long long> g_sim_steps{0};
// 当前瞬时目标步频（步/分钟），由 sim_loop 刷新，供加速度波形合成使用
static std::atomic<int> g_cur_rate{0};
// 事件类型直方图（诊断：看清 App 到底收哪些传感器）
static std::atomic<long long> g_type_cnt[32];
static void bump_type(int t) {
    if (t >= 0 && t < 32) g_type_cnt[t].fetch_add(1, std::memory_order_relaxed);
}
// 重入深度：防止 read→filterEvents 这种嵌套调用把同一批事件打两遍
static thread_local int g_depth = 0;

// ---------------------------------------------------------------------------
// 传感器事件处理
// ---------------------------------------------------------------------------
static inline void patch_event(ASensorEvent *ev) {
    if (ev->type == ASENSOR_TYPE_STEP_COUNTER) {
        long long offset = g_cfg.step_offset.load(std::memory_order_relaxed)
                         + g_sim_steps.load(std::memory_order_relaxed);
        if (g_cfg.absolute_mode.load(std::memory_order_relaxed)) {
            uint64_t v = (uint64_t)g_cfg.absolute_value.load(std::memory_order_relaxed);
            memcpy(&ev->u64.step_counter, &v, sizeof(v));
        } else if (offset != 0) {
            uint64_t cur = ev->u64.step_counter;
            int64_t nv = (int64_t)cur + offset;
            if (nv < 0) nv = 0;
            uint64_t out = (uint64_t)nv;
            memcpy(&ev->u64.step_counter, &out, sizeof(out));
        }
        g_patched_events.fetch_add(1, std::memory_order_relaxed);
        static std::atomic<int> once{0};
        if (once.fetch_add(1) == 0) {
            LOGI("first patch: type=%d counter=%llu offset=%lld sim=%lld",
                 ev->type, (unsigned long long)ev->u64.step_counter,
                 (long long)g_cfg.step_offset.load(), (long long)g_sim_steps.load());
        }
    } else if (ev->type == ASENSOR_TYPE_STEP_DETECTOR) {
        if (g_cfg.sim_active.load(std::memory_order_relaxed)) {
            ev->data[0] = 1.0f;   // 原设计：每个检测事件强制为 1 步
            g_patched_events.fetch_add(1, std::memory_order_relaxed);
        } else if (g_cfg.absolute_mode.load(std::memory_order_relaxed)) {
            ev->data[0] = 0.0f;
        }
    } else if (ev->type == ASENSOR_TYPE_ACCELEROMETER) {
        // 波形严格对齐 FuckRun / RunHook 原设计（对闪动校园实测兼容）：
        //     X = 0
        //     Y = 1.5·cos(2πft) + jitter(±0.15)
        //     Z = 9.81 + 3.5·sin(2πft) + jitter(±0.15)
        // 必须是完整正负正弦。此前用的单向尖峰 s³ 只有正向冲量，400Hz 采样下
        // 绝大多数样本贴着 9.81 重力基线，自研计步器做带通滤波后能量不足，
        // 判定为静止 → 结算步数为 0。
        if (g_cfg.sim_active.load(std::memory_order_relaxed) &&
            g_cfg.sim_accel.load(std::memory_order_relaxed)) {
            int rate = g_cur_rate.load(std::memory_order_relaxed);
            if (rate <= 0) rate = g_cfg.sim_rate_min.load(std::memory_order_relaxed);
            if (rate > 0) {
                double f = (double)rate / 60.0;                 // 步频 Hz
                double t = (double)ev->timestamp * 1e-9;        // 秒
                double ph = 2.0 * M_PI * f * t;
                double z = 9.81 + 3.5 * sin(ph);
                double y = 1.5 * cos(ph);
                double jy = ((double)rand() / (double)RAND_MAX - 0.5) * 0.3;
                double jz = ((double)rand() / (double)RAND_MAX - 0.5) * 0.3;
                ev->data[0] = 0.0f;
                ev->data[1] = (float)(y + jy);
                ev->data[2] = (float)(z + jz);
                g_patched_events.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
}

// PLT 版本（旧机型兜底）
static int hook_get_events(ASensorEventQueue *queue, ASensorEvent *events, size_t count) {
    if (g_orig_get_events == nullptr) return 0;
    ++g_depth;
    int n = g_orig_get_events(queue, events, count);
    if (n > 0 && g_depth == 1) {
        g_hook_hits.fetch_add(1, std::memory_order_relaxed);
        bool en = g_cfg.enabled.load(std::memory_order_relaxed);
        for (int i = 0; i < n; ++i) {
            bump_type(events[i].type);
            if (en) patch_event(&events[i]);
        }
    }
    --g_depth;
    return n;
}

// ---------------------------------------------------------------------------
// 多候选 inline hook（Android 14+：libsensor.so 的 C++ SensorEventQueue）
// ABI：ssize_t (this*, ASensorEvent*, size_t)
// 每个槽位独立 trampoline；thread_local 深度守卫保证嵌套调用只打一遍
// ---------------------------------------------------------------------------
typedef ssize_t (*sensor_read_fn)(void *, ASensorEvent *, size_t);

static constexpr int kMaxHooks = 4;
static sensor_read_fn g_origs[kMaxHooks] = {nullptr};
static std::atomic<int> g_hook_cnt{0};

template <int I>
static ssize_t hook_slot(void *a, ASensorEvent *events, size_t count) {
    if (g_origs[I] == nullptr) return 0;
    ++g_depth;
    ssize_t n = g_origs[I](a, events, count);
    if (n > 0 && g_depth == 1) {
        g_hook_hits.fetch_add(1, std::memory_order_relaxed);
        static std::atomic<int> once{0};
        if (once.fetch_add(1) == 0) {
            LOGI("slot[%d] first read: n=%zd type0=%d", I, n, events[0].type);
        }
        bool en = g_cfg.enabled.load(std::memory_order_relaxed);
        for (ssize_t i = 0; i < n; ++i) {
            bump_type(events[i].type);
            if (en) patch_event(&events[i]);
        }
    }
    --g_depth;
    return n;
}

static void *kHookFns[kMaxHooks] = {
    (void *)&hook_slot<0>, (void *)&hook_slot<1>,
    (void *)&hook_slot<2>, (void *)&hook_slot<3>,
};

// ---------------------------------------------------------------------------
// AArch64 inline hook（通过 /proc/self/mem 绕过 RELRO 只读保护）
// 跳转桩 16 字节：LDR X16, #8；BR X16；<8 字节绝对地址>
// 蹦床布局：[0..15] 原始头 16 字节 + [16..31] 跳回 fn+16
// ---------------------------------------------------------------------------
static bool inline_hook_abs(void *target_fn, void *hook_fn, void **orig_out) {
    uintptr_t fn = (uintptr_t)target_fn;
    constexpr size_t STUB = 16;

    // 分配 RWX 蹦床
    uint8_t *tramp = (uint8_t *)mmap(nullptr, STUB * 2,
        PROT_READ | PROT_WRITE | PROT_EXEC,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (tramp == MAP_FAILED) {
        LOGE("inline_hook mmap: %s", strerror(errno));
        return false;
    }

    // 复制原始前 16 字节到蹦床
    memcpy(tramp, (const void *)fn, STUB);

    // 追加跳回存根：LDR X16, #8；BR X16；<fn+STUB>
    const uint32_t LDR = 0x58000050u;  // LDR X16, #8
    const uint32_t BR  = 0xD61F0200u;  // BR  X16
    memcpy(tramp + STUB,     &LDR, 4);
    memcpy(tramp + STUB + 4, &BR,  4);
    uintptr_t ret = fn + STUB;
    memcpy(tramp + STUB + 8, &ret, 8);
    __builtin___clear_cache((char *)tramp, (char *)(tramp + STUB * 2));

    // 构建跳转桩
    uint8_t patch[STUB];
    memcpy(patch,     &LDR, 4);
    memcpy(patch + 4, &BR,  4);
    uintptr_t hk = (uintptr_t)hook_fn;
    memcpy(patch + 8, &hk, 8);

    // 写入 .text：优先 /proc/self/mem（老内核可用），失败则 mprotect + memcpy
    // Android 14+ SELinux 禁止 app 域 open("/proc/self/mem")，必须走 mprotect 路线
    bool written = false;
    int memfd = open("/proc/self/mem", O_RDWR | O_CLOEXEC);
    if (memfd >= 0) {
        ssize_t wr = pwrite(memfd, patch, STUB, (off_t)fn);
        close(memfd);
        if (wr == (ssize_t)STUB) {
            written = true;
            LOGI("inline_hook via /proc/self/mem");
        } else {
            LOGW("inline_hook pwrite: %zd/%zu errno=%d, fallback mprotect", wr, STUB, errno);
        }
    } else {
        LOGI("inline_hook /proc/self/mem denied (%s), using mprotect", strerror(errno));
    }

    if (!written) {
        uintptr_t page = fn & ~(uintptr_t)(PAGE_SZ - 1);
        size_t span = (fn + STUB) - page;
        span = (span + PAGE_SZ - 1) & ~(size_t)(PAGE_SZ - 1);

        int mp = mprotect((void *)page, span, PROT_READ | PROT_WRITE | PROT_EXEC);
        if (mp != 0) {
            LOGW("inline_hook mprotect RWX failed (%s), trying RW then RX", strerror(errno));
            if (mprotect((void *)page, span, PROT_READ | PROT_WRITE) != 0) {
                LOGE("inline_hook mprotect RW failed: %s", strerror(errno));
                munmap(tramp, STUB * 2);
                return false;
            }
        }
        memcpy((void *)fn, patch, STUB);
        __builtin___clear_cache((char *)fn, (char *)(fn + STUB));
        // 恢复可执行（失败则保持 RWX，不影响执行）
        if (mprotect((void *)page, span, PROT_READ | PROT_EXEC) != 0) {
            LOGW("inline_hook restore RX failed (%s), page stays RWX", strerror(errno));
        }
        written = true;
        LOGI("inline_hook via mprotect fn=%p", (void *)fn);
    }

    *orig_out = tramp;
    LOGI("inline_hook OK fn=%p hook=%p tramp=%p", (void *)fn, hook_fn, tramp);
    return true;
}

// 安装传感器 hook：同时挂 libsensor.so 的 read + filterEvents（Android 14+ 实际路径），
// NDK C API 作兜底；同一函数地址只挂一次。
static bool try_install_sensor_hook() {
    struct Cand { const char *lib; const char *sym; };
    static const Cand cands[] = {
        // ── 首选：Android 14+ Java SensorManager 的真实路径（libsensor.so）──
        {"libsensor.so", "_ZN7android16SensorEventQueue4readEP12ASensorEventm"},
        {nullptr,        "_ZN7android16SensorEventQueue4readEP12ASensorEventm"},
        {"libsensor.so", "_ZNK7android16SensorEventQueue12filterEventsEP12ASensorEventm"},
        {nullptr,        "_ZNK7android16SensorEventQueue12filterEventsEP12ASensorEventm"},
        // ── 兜底：NDK C API ──
        {nullptr,                 "ASensorEventQueue_getEvents"},
        {"libandroid.so",         "ASensorEventQueue_getEvents"},
        {"libandroid_runtime.so", "ASensorEventQueue_getEvents"},
        // ── 旧 mangling 备选 ──
        {"libsensor.so",          "_ZN7android16SensorEventQueue4readEP13ASensorEventm"},
        {"libsensor.so",          "_ZN7android16SensorEventQueue4readEP13ASensorEventi"},
    };

    void *hooked[kMaxHooks] = {nullptr};
    int hooked_n = 0;
    int slot = g_hook_cnt.load();

    for (auto &c : cands) {
        if (slot >= kMaxHooks) break;
        void *handle = c.lib ? dlopen(c.lib, RTLD_NOLOAD | RTLD_LAZY) : RTLD_DEFAULT;
        if (!handle && c.lib) handle = dlopen(c.lib, RTLD_LAZY);
        if (!handle) continue;
        void *fn = dlsym(handle, c.sym);
        if (!fn) continue;

        bool dup = false;
        for (int i = 0; i < hooked_n; ++i) if (hooked[i] == fn) { dup = true; break; }
        if (dup) continue;

        LOGI("sensor sym: %s @ %p (lib=%s)", c.sym, fn, c.lib ? c.lib : "default");
        if (inline_hook_abs(fn, kHookFns[slot], (void **)&g_origs[slot])) {
            LOGI("sensor inline hook[%d] installed via %s", slot, c.sym);
            hooked[hooked_n++] = fn;
            ++slot;
            g_hook_cnt.store(slot);
        }
    }

    if (slot > 0) {
        LOGI("sensor hooks installed: %d", slot);
        return true;
    }
    LOGE("try_install_sensor_hook: all paths failed");
    return false;
}

// ---------------------------------------------------------------------------
// 从 /proc/self/maps 枚举所有已映射 .so 的唯一 (dev, ino)
// 用于对全部库广播 PLT hook 注册，让 Zygisk 自行找有该符号 PLT 条目的库
// ---------------------------------------------------------------------------
static std::vector<std::pair<dev_t, ino_t>> get_all_mapped_sos() {
    std::vector<std::pair<dev_t, ino_t>> result;
    std::set<std::pair<dev_t, ino_t>> seen;

    int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return result;

    std::string buf;
    char tmp[4096];
    ssize_t r;
    while ((r = read(fd, tmp, sizeof(tmp))) > 0) buf.append(tmp, (size_t)r);
    close(fd);

    size_t pos = 0;
    while (pos < buf.size()) {
        size_t eol = buf.find('\n', pos);
        if (eol == std::string::npos) eol = buf.size();
        std::string line = buf.substr(pos, eol - pos);
        pos = eol + 1;

        if (line.find(".so") == std::string::npos) continue;

        unsigned int ma = 0, mi = 0;
        unsigned long long ino = 0;
        if (sscanf(line.c_str(), "%*s %*s %*s %x:%x %llu", &ma, &mi, &ino) == 3 && ino != 0) {
            auto key = std::make_pair(makedev(ma, mi), (ino_t)ino);
            if (seen.insert(key).second) result.push_back(key);
        }
    }
    LOGI("maps: found %zu unique sos", result.size());
    return result;
}

// ---------------------------------------------------------------------------
// 配置读取（模块目录 — 仅 preAppSpecialize 阶段可访问）
// ---------------------------------------------------------------------------
static void load_config_from_dir(int dir_fd) {
    if (dir_fd < 0) return;
    int fd = openat(dir_fd, "spoof.conf", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    char buf[512] = {};
    ssize_t r = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (r <= 0) return;

    char *save = nullptr;
    for (char *tok = strtok_r(buf, " \n", &save); tok; tok = strtok_r(nullptr, " \n", &save)) {
        char *eq = strchr(tok, '=');
        if (!eq) continue;
        *eq = 0;
        const char *k = tok, *v = eq + 1;
        if (!strcmp(k, "enabled")) g_cfg.enabled.store(atoi(v) != 0);
        else if (!strcmp(k, "offset")) g_cfg.step_offset.store(atoll(v));
        else if (!strcmp(k, "absolute")) g_cfg.absolute_mode.store(atoi(v) != 0);
        else if (!strcmp(k, "absolute_value")) g_cfg.absolute_value.store(atoll(v));
        else if (!strcmp(k, "sim_active")) g_cfg.sim_active.store(atoi(v) != 0);
        else if (!strcmp(k, "sim_min")) g_cfg.sim_rate_min.store(atoi(v));
        else if (!strcmp(k, "sim_max")) g_cfg.sim_rate_max.store(atoi(v));
        else if (!strcmp(k, "sim_accel")) g_cfg.sim_accel.store(atoi(v) != 0);
        else if (!strcmp(k, "stride")) g_cfg.stride_cm.store(atoi(v) > 0 ? atoi(v) : 110);
    }
    LOGI("config loaded");
}

// ---------------------------------------------------------------------------
// 步频仿真线程
// 每 500ms tick：在 [min, max] 范围内随机取一个步频（步/分钟），
// 换算为该 tick 应增加的步数（带小数累加），累加到 step_offset。
// ---------------------------------------------------------------------------
static void sim_loop() {
    LOGI("sim_loop started");
    unsigned int seed = (unsigned int)time(nullptr) ^ (unsigned int)getpid();
    srandom(seed);

    constexpr int64_t TICK_MS = 500;
    bool prev_active = false;
    int picked = 0, picked_lo = -1, picked_hi = -1;

    for (;;) {
        usleep((useconds_t)(TICK_MS * 1000));

        bool active = g_cfg.sim_active.load(std::memory_order_relaxed)
                   && g_cfg.enabled.load(std::memory_order_relaxed);

        int lo = g_cfg.sim_rate_min.load(std::memory_order_relaxed);
        int hi = g_cfg.sim_rate_max.load(std::memory_order_relaxed);
        if (lo < 0) lo = 0;
        if (hi < lo) hi = lo;

        if (!active) { prev_active = false; continue; }

        // 步频稳定性（原设计关键）：只在「开始仿真」或「区间被改动」时取一次
        // 随机值并锁定。每 500ms 重随机会让步态周期抖动，自研计步器直接判成
        // 非人步态；真实跑步的步频在整段里也是近似恒定的。
        if (!prev_active || lo != picked_lo || hi != picked_hi) {
            picked = lo;
            if (hi > lo) picked = lo + (int)(random() % (hi - lo + 1));
            picked_lo = lo;
            picked_hi = hi;
            prev_active = true;
            g_cur_rate.store(picked, std::memory_order_relaxed);
            LOGI("sim cadence locked: %d spm (range %d-%d)", picked, lo, hi);
        } else {
            g_cur_rate.store(picked, std::memory_order_relaxed);
        }

        // 每 tick 应增加的步数 = picked * TICK_MS / 60000
        double steps_this_tick = (double)picked * (double)TICK_MS / 60000.0;
        sim_accum += steps_this_tick;

        long long add = (long long)floor(sim_accum);
        if (add > 0) {
            sim_accum -= (double)add;
            g_sim_steps.fetch_add(add, std::memory_order_relaxed);
        }
    }
}

// ---------------------------------------------------------------------------
// 运行时控制通道
// WebUI 无法 fetch http://127.0.0.1:8888（https 页面 ←→ http 的混合内容会被
// WebView 拦截），改由 ksu.exec 以 root 身份写控制文件，本进程轮询读取。
// 路径选在 App 私有目录：root 可写，App 自身必然可读（避开 /data/adb 的 SELinux 限制）。
// ---------------------------------------------------------------------------
static const char *CTL_CONF  = "/data/data/" TARGET_PKG_STR "/files/stepspoof.conf";
static const char *CTL_STATE = "/data/data/" TARGET_PKG_STR "/files/stepspoof.state";

static void apply_conf_text(char *buf) {
    char *save = nullptr;
    for (char *tok = strtok_r(buf, " \n\r\t", &save); tok; tok = strtok_r(nullptr, " \n\r\t", &save)) {
        char *eq = strchr(tok, '=');
        if (!eq) continue;
        *eq = 0;
        const char *k = tok, *v = eq + 1;
        if (!strcmp(k, "enabled")) g_cfg.enabled.store(atoi(v) != 0);
        else if (!strcmp(k, "offset")) g_cfg.step_offset.store(atoll(v));
        else if (!strcmp(k, "absolute")) g_cfg.absolute_mode.store(atoi(v) != 0);
        else if (!strcmp(k, "absolute_value")) g_cfg.absolute_value.store(atoll(v));
        else if (!strcmp(k, "sim_active")) g_cfg.sim_active.store(atoi(v) != 0);
        else if (!strcmp(k, "sim_min")) g_cfg.sim_rate_min.store(atoi(v));
        else if (!strcmp(k, "sim_max")) g_cfg.sim_rate_max.store(atoi(v));
        else if (!strcmp(k, "sim_accel")) g_cfg.sim_accel.store(atoi(v) != 0);
        else if (!strcmp(k, "stride")) g_cfg.stride_cm.store(atoi(v) > 0 ? atoi(v) : 110);
    }
}

static void write_state_file() {
    char js[384];
    int n = snprintf(js, sizeof(js),
        "enabled=%d\noffset=%lld\nabsolute=%d\nabsolute_value=%lld\n"
        "sim_active=%d\nsim_min=%d\nsim_max=%d\nsim_accel=%d\n"
        "hook_hits=%lld\npatched=%lld\nsim_steps=%lld\n"
        "acc=%lld\ngyro=%lld\nstepdet=%lld\nstepcnt=%lld\n"
        "stride=%d\ncur_rate=%d\n",
        g_cfg.enabled.load() ? 1 : 0, g_cfg.step_offset.load(),
        g_cfg.absolute_mode.load() ? 1 : 0, g_cfg.absolute_value.load(),
        g_cfg.sim_active.load() ? 1 : 0, g_cfg.sim_rate_min.load(),
        g_cfg.sim_rate_max.load(), g_cfg.sim_accel.load() ? 1 : 0,
        g_hook_hits.load(), g_patched_events.load(), g_sim_steps.load(),
        g_type_cnt[ASENSOR_TYPE_ACCELEROMETER].load(),
        g_type_cnt[ASENSOR_TYPE_GYROSCOPE].load(),
        g_type_cnt[ASENSOR_TYPE_STEP_DETECTOR].load(),
        g_type_cnt[ASENSOR_TYPE_STEP_COUNTER].load(),
        g_cfg.stride_cm.load(), g_cur_rate.load());
    int fd = open(CTL_STATE, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (fd < 0) return;
    (void)!write(fd, js, (size_t)n);
    close(fd);
}

static void conf_watch_loop() {
    LOGI("conf_watch started: %s", CTL_CONF);
    struct stat st {};
    off_t last_sz = -1;
    time_t last_mt = 0;

    for (;;) {
        usleep(700 * 1000);

        if (stat(CTL_CONF, &st) == 0) {
            if (st.st_size != last_sz || st.st_mtime != last_mt) {
                last_sz = st.st_size;
                last_mt = st.st_mtime;
                int fd = open(CTL_CONF, O_RDONLY | O_CLOEXEC);
                if (fd >= 0) {
                    char b[512] = {};
                    ssize_t r = read(fd, b, sizeof(b) - 1);
                    close(fd);
                    if (r > 0) {
                        apply_conf_text(b);
                        LOGI("conf channel applied (%zd bytes)", r);
                    }
                } else {
                    LOGW("conf open denied: %s", strerror(errno));
                }
            }
        } else if (errno != ENOENT && last_sz >= 0) {
            LOGW("conf stat failed: %s", strerror(errno));
        }

        write_state_file();
    }
}

// ---------------------------------------------------------------------------
// HTTP API（127.0.0.1:8888）— 供 KSU WebUI 调用
// ---------------------------------------------------------------------------
static void http_reply(int fd, int code, const char *ctype, const std::string &body) {
    char head[512];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                     "Cache-Control: no-store\r\n"
                     "Access-Control-Allow-Origin: *\r\n"
                     "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                     "Access-Control-Allow-Headers: Content-Type\r\n"
                     "Connection: close\r\n\r\n",
                     code, code == 200 ? "OK" : "Error", ctype, body.size());
    if (n > 0) (void)!write(fd, head, (size_t)n);
    if (!body.empty()) (void)!write(fd, body.data(), body.size());
}

static long long json_num(const std::string &js, const char *key, long long dflt) {
    std::string pat = std::string("\"") + key + "\"";
    size_t p = js.find(pat);
    if (p == std::string::npos) return dflt;
    p = js.find(':', p);
    if (p == std::string::npos) return dflt;
    return atoll(js.c_str() + p + 1);
}

static void handle_client(int fd) {
    struct timeval tv{};
    tv.tv_sec = 3;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    std::string req;
    char buf[2048];
    ssize_t r = read(fd, buf, sizeof(buf) - 1);
    if (r <= 0) { close(fd); return; }
    req.assign(buf, (size_t)r);

    std::string method, path;
    {
        size_t sp1 = req.find(' ');
        size_t sp2 = req.find(' ', sp1 + 1);
        if (sp1 != std::string::npos && sp2 != std::string::npos) {
            method = req.substr(0, sp1);
            path = req.substr(sp1 + 1, sp2 - sp1 - 1);
        }
    }

    // CORS preflight
    if (method == "OPTIONS") {
        const char *resp = "HTTP/1.1 204 No Content\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type\r\n"
            "Content-Length: 0\r\n\r\n";
        (void)!write(fd, resp, strlen(resp));
        close(fd);
        return;
    }

    if (method == "GET" && path == "/api/status") {
        char js[1024];
        snprintf(js, sizeof(js),
                 "{\"enabled\":%d,\"step_offset\":%lld,\"absolute_mode\":%d,"
                 "\"absolute_value\":%lld,\"sim_active\":%d,\"sim_rate_min\":%d,"
                 "\"sim_rate_max\":%d,\"hook_hits\":%lld,\"patched_events\":%lld,"
                 "\"sim_steps\":%lld,\"hooks\":%d,\"sim_accel\":%d,\"cur_rate\":%d,"
                 "\"acc\":%lld,\"gyro\":%lld,\"stepdet\":%lld,\"stepcnt\":%lld,"
                 "\"stride\":%d,\"speed_cmps\":%d,\"pkg\":\"%s\"}",
                 g_cfg.enabled.load() ? 1 : 0,
                 g_cfg.step_offset.load(),
                 g_cfg.absolute_mode.load() ? 1 : 0,
                 g_cfg.absolute_value.load(),
                 g_cfg.sim_active.load() ? 1 : 0,
                 g_cfg.sim_rate_min.load(),
                 g_cfg.sim_rate_max.load(),
                 g_hook_hits.load(), g_patched_events.load(),
                 g_sim_steps.load(), g_hook_cnt.load(),
                 g_cfg.sim_accel.load() ? 1 : 0, g_cur_rate.load(),
                 g_type_cnt[ASENSOR_TYPE_ACCELEROMETER].load(),
                 g_type_cnt[ASENSOR_TYPE_GYROSCOPE].load(),
                 g_type_cnt[ASENSOR_TYPE_STEP_DETECTOR].load(),
                 g_type_cnt[ASENSOR_TYPE_STEP_COUNTER].load(),
                 g_cfg.stride_cm.load(),
                 (int)((long long)g_cur_rate.load() * g_cfg.stride_cm.load() / 60),
                 TARGET_PKG);
        http_reply(fd, 200, "application/json", js);
    } else if (method == "POST" && path == "/api/config") {
        size_t body_at = req.find("\r\n\r\n");
        std::string body = body_at == std::string::npos ? std::string() : req.substr(body_at + 4);
        if (body.empty()) {
            ssize_t r2 = read(fd, buf, sizeof(buf) - 1);
            if (r2 > 0) body.append(buf, (size_t)r2);
        }
        g_cfg.enabled.store(json_num(body, "enabled", 1) != 0);
        g_cfg.step_offset.store(json_num(body, "step_offset", g_cfg.step_offset.load()));
        g_cfg.absolute_mode.store(json_num(body, "absolute_mode", 0) != 0);
        g_cfg.absolute_value.store(json_num(body, "absolute_value", g_cfg.absolute_value.load()));
        g_cfg.sim_active.store(json_num(body, "sim_active", g_cfg.sim_active.load() ? 1 : 0) != 0);
        g_cfg.sim_rate_min.store((int)json_num(body, "sim_rate_min", g_cfg.sim_rate_min.load()));
        g_cfg.sim_rate_max.store((int)json_num(body, "sim_rate_max", g_cfg.sim_rate_max.load()));
        g_cfg.sim_accel.store(json_num(body, "sim_accel", g_cfg.sim_accel.load() ? 1 : 0) != 0);
        g_cfg.stride_cm.store((int)json_num(body, "stride", g_cfg.stride_cm.load()));
        LOGI("cfg updated: en=%d off=%lld abs=%d val=%lld sim=%d min=%d max=%d",
             g_cfg.enabled.load() ? 1 : 0, g_cfg.step_offset.load(),
             g_cfg.absolute_mode.load() ? 1 : 0, g_cfg.absolute_value.load(),
             g_cfg.sim_active.load() ? 1 : 0, g_cfg.sim_rate_min.load(),
             g_cfg.sim_rate_max.load());
        http_reply(fd, 200, "application/json", "{\"ok\":true}");
    } else if (method == "POST" && path == "/api/start") {
        g_cfg.enabled.store(true);
        g_cfg.sim_active.store(true);
        LOGI("sim started: min=%d max=%d", g_cfg.sim_rate_min.load(), g_cfg.sim_rate_max.load());
        http_reply(fd, 200, "application/json", "{\"ok\":true,\"sim_active\":true}");
    } else if (method == "GET" && path == "/api/log") {
        // 返回日志文件路径与实际大小，WebUI 用它给用户提示去哪里取文件
        char js[512];
        struct stat st {};
        long long sz = 0;
        int exists = 0;
        if (g_log_path[0] && stat(g_log_path, &st) == 0) {
            sz = (long long)st.st_size;
            exists = 1;
        }
        snprintf(js, sizeof(js),
                 "{\"path\":\"%s\",\"exists\":%d,\"size\":%lld}",
                 g_log_path[0] ? g_log_path : "", exists, sz);
        http_reply(fd, 200, "application/json", js);
    } else if (method == "POST" && path == "/api/log/clear") {
        if (g_log_path[0]) {
            close(g_log_fd);
            g_log_fd = -1;
            g_log_bytes = 0;
            unlink(g_log_path);
            LOGI("log cleared by request");
        }
        http_reply(fd, 200, "application/json", "{\"ok\":true}");
    } else if (method == "POST" && path == "/api/log/mark") {
        LOGI("=== user marked this point (WebUI) ===");
        http_reply(fd, 200, "application/json", "{\"ok\":true}");
    } else if (method == "POST" && path == "/api/stop") {
        g_cfg.sim_active.store(false);
        LOGI("sim stopped");
        http_reply(fd, 200, "application/json", "{\"ok\":true,\"sim_active\":false}");
    } else {
        http_reply(fd, 404, "application/json", "{\"ok\":false}");
    }
    close(fd);
}

static void webui_loop() {
    int srv = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (srv < 0) { LOGE("socket: %s", strerror(errno)); return; }

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8888);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(srv, (sockaddr *)&addr, sizeof(addr)) != 0) {
        LOGE("bind 8888: %s", strerror(errno));
        close(srv);
        return;
    }
    if (listen(srv, 8) != 0) { LOGE("listen: %s", strerror(errno)); close(srv); return; }

    LOGI("webui listening on 127.0.0.1:8888");

    for (;;) {
        int c = accept4(srv, nullptr, nullptr, SOCK_CLOEXEC);
        if (c < 0) {
            if (errno == EINTR) continue;
            usleep(50 * 1000);
            continue;
        }
        handle_client(c);
    }
}

// ---------------------------------------------------------------------------
// Zygisk 模块
// ---------------------------------------------------------------------------
class StepSpoofModule : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api *api, JNIEnv *env) override {
        this->api = api;
        this->env = env;
    }

    void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {
        if (!env || !args || !args->nice_name) {
            if (api) api->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        const char *name = env->GetStringUTFChars(args->nice_name, nullptr);
        bool target = false;
        bool main_proc = false;
        if (name) {
            size_t plen = strlen(TARGET_PKG);
            main_proc = (strcmp(name, TARGET_PKG) == 0);
            // 子进程：包名 + ":" 前缀（如 :push / :daemon）
            bool child_proc = (strncmp(name, TARGET_PKG, plen) == 0 && name[plen] == ':');
            target = main_proc || child_proc;
            LOGI("proc name=%s main=%d target=%d", name, main_proc ? 1 : 0, target ? 1 : 0);
            env->ReleaseStringUTFChars(args->nice_name, name);
        }

        if (!target) {
            if (api) api->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        is_target = true;
        is_main = main_proc;

        int dir = api ? api->getModuleDir() : -1;
        load_config_from_dir(dir);
        if (dir >= 0) close(dir);

        if (api) {
            auto sos = get_all_mapped_sos();
            for (auto &kv : sos) {
                api->pltHookRegister(kv.first, kv.second,
                                     "ASensorEventQueue_getEvents",
                                     (void *)hook_get_events,
                                     (void **)&g_orig_get_events);
            }
            if (api->pltHookCommit()) {
                LOGI("sensor hook committed: %zu sos scanned orig=%p",
                     sos.size(), (void *)g_orig_get_events);
            } else {
                LOGE("pltHookCommit failed on all %zu sos", sos.size());
            }
        }
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs *) override {
        if (!is_target) return;

        // 防重复：Zygisk 在部分 ROM 上会二次回调，避免起两条 sim/webui
        static std::atomic<bool> installed{false};
        if (installed.exchange(true)) {
            LOGI("postAppSpecialize already installed, skip pid=%d", getpid());
            return;
        }

        // 首个日志落盘，附带一次环境快照，便于他人排障
        LOGI("===== stepspoof_joyrun v1.2.0 start pid=%d main=%d =====",
             getpid(), is_main ? 1 : 0);
        LOGI("log file: %s", g_log_path[0] ? g_log_path : "(not opened yet)");

        // PLT hook 没打上时，尝试 inline hook（Android 14+ 路径）
        if (g_hook_cnt.load() == 0) {
            try_install_sensor_hook();
        }

        // 仿真、WebUI、控制通道只在主进程起，子进程只挂 hook
        if (is_main) {
            std::thread(webui_loop).detach();
            std::thread(sim_loop).detach();
            std::thread(conf_watch_loop).detach();
        }
    }

private:
    zygisk::Api *api = nullptr;
    JNIEnv *env = nullptr;
    bool is_target = false;
    bool is_main = false;
};

REGISTER_ZYGISK_MODULE(StepSpoofModule)
