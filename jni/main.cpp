// stepspoof_joyrun v1.3.4 — 纯 native Zygisk 模块
// 目标：com.huachenjie.shandong_school（闪动校园）
// 功能：
//   1) 加速度波形仿真（FuckRun 设计：X=0, Y=1.5·cos, Z=9.81+3.5·sin）
//      驱动 App 自研带通滤波计步算法（h9.d）产生步数
//   2) 陀螺仪 / 旋转矢量同频同相注入（v1.3.0）
//      只有加速度在动、角速度和姿态恒为 0，是明显的传感器伪造特征；
//      现在三路信号用同一个 2πft 相位合成，多传感器融合校验看到的是一段
//      完整的人体跑步动作。
//   3) STEP_COUNTER / STEP_DETECTOR 事件改写（App 注册硬件计步器时生效）
//      v1.3.0 起计步器走「基线锚定 + 单调钳制」：仿真开启瞬间锁住当前硬件
//      读数，之后只输出平滑递增的曲线，不会跳变或倒退触发防作弊清零。
//   4) 步频仿真线程：在 [min, max] 区间随机锁定步频（默认 135–160，跑步区间），
//      整段稳定不抖动；仿真步数按 pid 分片写入共享目录并在各进程间求和，
//      保证多进程读到的步数完全一致。
//   5) 运行日志：同时写 logcat 与 /sdcard/Download/stepspoof_joyrun.log
//      （2MB 自动轮转到 .old；/sdcard 不可写时降级到 App 私有目录）
//      每行带 pid；每 10 秒一行 heartbeat 快照；每种传感器类型首次出现报一次。
//      v1.3.4 起启动横幅带日期与设备名称/指纹，多台设备的日志可区分。
//   6) 本地 HTTP API（127.0.0.1:8888）供 KSU WebUI 通过 root shell nc 访问
//   7) conf_watch_loop：轮询 App 私有目录 stepspoof.conf，断连后持久化生效
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
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <malloc.h>
#include <netinet/in.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/system_properties.h>
#include <unistd.h>

#define LOG_TAG "stepspoof"

// 目标包名（日志降级路径、控制通道路径都依赖它，必须在最前定义）
#define TARGET_PKG_STR "com.huachenjie.shandong_school"

// ---------------------------------------------------------------------------
// 设备标识（v1.3.4）
// 日志 banner 里带上「设备名称 + 系统指纹」，多台设备导出的日志才分得清。
// 只读公开系统属性：品牌、型号、Android 版本、SDK、ABI、ro.build.fingerprint。
// 不含序列号 / IMEI / ANDROID_ID / MAC，不引入新的个人标识。
// fp8 是 ro.build.fingerprint 的 8 位哈希，标识「ROM 身份」而非单机——同型号
// 同版本的两台机器哈希相同，这是刻意的；完整 fingerprint 同时写一行，便于核对。
// ---------------------------------------------------------------------------
struct DeviceInfo {
    char name[128];      // "Xiaomi 23054RA19C"
    char android[64];    // "Android 15 (SDK 35)"
    char abi[64];        // "arm64-v8a"
    char fp8[16];        // "8f3a21c7"
    char fp_full[PROP_VALUE_MAX + 8];
};

static void prop_get(const char *key, char *buf, size_t n) {
    if (!buf || n == 0) return;
    buf[0] = 0;
    if (!key || !key[0]) return;
    char tmp[PROP_VALUE_MAX + 8] = {0};
    int len = __system_property_get(key, tmp);
    if (len <= 0) return;
    if ((size_t)len >= n) len = (int)n - 1;
    memcpy(buf, tmp, (size_t)len);
    buf[len] = 0;
}

// 一次性读取并缓存，之后所有线程共用只读快照
static const DeviceInfo &device_info() {
    static DeviceInfo d {};
    static std::atomic<int> once{0};
    if (!once.exchange(1)) {
        char brand[64] = {0}, model[64] = {0};
        char rel[32] = {0}, sdk[32] = {0}, abi[64] = {0};

        prop_get("ro.product.brand", brand, sizeof(brand));
        prop_get("ro.product.model", model, sizeof(model));
        if (!model[0]) prop_get("ro.product.name", model, sizeof(model));
        if (brand[0] && model[0]) snprintf(d.name, sizeof(d.name), "%s %s", brand, model);
        else if (model[0]) snprintf(d.name, sizeof(d.name), "%s", model);
        else snprintf(d.name, sizeof(d.name), "unknown");

        prop_get("ro.build.version.release", rel, sizeof(rel));
        prop_get("ro.build.version.sdk", sdk, sizeof(sdk));
        if (rel[0] && sdk[0]) snprintf(d.android, sizeof(d.android), "Android %s (SDK %s)", rel, sdk);
        else if (rel[0]) snprintf(d.android, sizeof(d.android), "Android %s", rel);
        else snprintf(d.android, sizeof(d.android), "unknown");

        prop_get("ro.product.cpu.abi", abi, sizeof(abi));
        snprintf(d.abi, sizeof(d.abi), "%s", abi[0] ? abi : "?");

        prop_get("ro.build.fingerprint", d.fp_full, sizeof(d.fp_full));
        unsigned int h = 2166136261u;   // FNV-1a 32
        const char *src = d.fp_full[0] ? d.fp_full : (rel[0] ? rel : "unknown");
        for (const char *p = src; *p; ++p) {
            h ^= (unsigned char)*p;
            h *= 16777619u;
        }
        if (h == 0) h = 1;              // 全 0 留作「未知」占位
        snprintf(d.fp8, sizeof(d.fp8), "%08x", h);
    }
    return d;
}

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

    int head = snprintf(line, sizeof(line), "%s pid=%d %c/stepspoof: ",
                        stamp, getpid(), prio_ch[ci]);
    int body = vsnprintf(line + head, sizeof(line) - (size_t)head - 2, fmt, ap);
    if (body < 0) return;
    int total = head + body;
    if (total > (int)sizeof(line) - 2) total = (int)sizeof(line) - 2;
    line[total++] = '\n';
    ssize_t wr = write(g_log_fd, line, (size_t)total);
    if (wr > 0) g_log_bytes += wr;
    log_close_and_rotate_if_needed();
}

// format 属性要写在声明最前面。原先写成 "void log_d(...) LOGFMT(1) {"
// 是 clang 才认的位置，GCC 前端会报 -Wgcc-compat 说这里不允许。
#if defined(__clang__) || defined(__GNUC__)
#define LOGFMT __attribute__((format(printf, 1, 2)))
#else
#define LOGFMT
#endif
// LOGD 目前没有调用点，留着方便以后临时加调试输出，显式标记避免 unused 告警
#define LOGD_UNUSED __attribute__((unused))

LOGFMT LOGD_UNUSED static void log_d(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); log_v(ANDROID_LOG_DEBUG, fmt, ap); va_end(ap);
}
LOGFMT static void log_i(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); log_v(ANDROID_LOG_INFO, fmt, ap); va_end(ap);
}
LOGFMT static void log_w(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); log_v(ANDROID_LOG_WARN, fmt, ap); va_end(ap);
}
LOGFMT static void log_e(const char *fmt, ...) {
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
    // 默认步频对齐跑步区间：135–160 步/分。
    // 旧默认 60–120 换算配速约 9'30"/km，是走路速度，过不了闪动校园的跑步判定。
    std::atomic<int> sim_rate_min{135};   // 步/分钟
    std::atomic<int> sim_rate_max{160};   // 步/分钟
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
// 拆成三份：本进程 local + 其它进程分片 other = 对外暴露的 total
static std::atomic<long long> g_sim_steps{0};
static std::atomic<long long> g_local_steps{0};
static std::atomic<long long> g_sim_other{0};
static std::atomic<int> g_sim_peers{0};
// 主进程才有资格写状态文件、决定共享步频、累加全局步数
static bool g_is_main_proc = false;
// WebUI 实际绑定成功的端口。双开时后启动的实例会顺延到 8889 及以后，
// 前端靠 /api/status 的 port 字段自动发现该连哪个口。
static constexpr int kWebuiPortBase = 8888;
static constexpr int kWebuiPortSpan = 10;
static std::atomic<int> g_webui_port{0};
// 当前瞬时目标步频（步/分钟），由 sim_loop 刷新，供加速度波形合成使用
static std::atomic<int> g_cur_rate{0};
// 事件类型直方图（诊断：看清 App 到底收哪些传感器）
// 传感器类型号最大到 35（ACCELEROMETER_UNCALIBRATED），留到 64 兜住厂商扩展值
static constexpr int kTypeSlots = 64;
static std::atomic<long long> g_type_cnt[kTypeSlots];
// 每个类型在本进程内是否已报过首次出现
static std::atomic<bool> g_type_seen[kTypeSlots];

static int64_t mono_ms() {
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// 本进程的 cmdline basename，比如 "com.huachenjie.shandong_school" 或
// "com.huachenjie.shandong_school:push"。WebUI 的实例下拉框靠它区分
// 同一个空间里被注入的多个进程——它们 uid 相同，只有进程名不同。
static const char *proc_name() {
    static char name[128] = {};
    static std::atomic<int> once{0};
    if (once.exchange(1)) return name;
    int fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        char b[256] = {};
        ssize_t r = read(fd, b, sizeof(b) - 1);
        close(fd);
        if (r > 0) {
            b[r] = 0;
            // cmdline 以 \0 分隔，第一个字段就是主 cmdline
            const char *slash = strrchr(b, '/');
            snprintf(name, sizeof(name), "%s", slash ? slash + 1 : b);
        }
    }
    if (!name[0]) snprintf(name, sizeof(name), "%s", TARGET_PKG);
    return name;
}

// 本进程已经收到过多少种传感器类型的事件。0 表示这个进程根本没在读传感器，
// 实例列表里优先选非 0 的那个。
static int active_sensor_types() {
    int n = 0;
    for (int i = 0; i < kTypeSlots; i++) {
        if (g_type_seen[i].load(std::memory_order_relaxed)) n++;
    }
    return n;
}

static const char *sensor_type_name(int t) {
    switch (t) {
        case 1:  return "ACCELEROMETER";
        case 2:  return "MAGNETIC_FIELD";
        case 3:  return "ORIENTATION";
        case 4:  return "GYROSCOPE";
        case 5:  return "LIGHT";
        case 6:  return "PRESSURE";
        case 7:  return "TEMPERATURE";
        case 8:  return "PROXIMITY";
        case 9:  return "GRAVITY";
        case 10: return "LINEAR_ACCELERATION";
        case 11: return "ROTATION_VECTOR";
        case 12: return "RELATIVE_HUMIDITY";
        case 13: return "AMBIENT_TEMPERATURE";
        case 14: return "MAGNETIC_FIELD_UNCALIBRATED";
        case 15: return "GAME_ROTATION_VECTOR";
        case 16: return "GYROSCOPE_UNCALIBRATED";
        case 17: return "SIGNIFICANT_MOTION";
        case 18: return "STEP_DETECTOR";
        case 19: return "STEP_COUNTER";
        case 20: return "GEOMAGNETIC_ROTATION_VECTOR";
        case 21: return "HEART_RATE";
        case 28: return "POSE_6DOF";
        case 29: return "STATIONARY_DETECT";
        case 30: return "MOTION_DETECT";
        case 31: return "HEART_BEAT";
        case 32: return "DYNAMIC_SENSOR_META";
        case 33: return "ADDITIONAL_INFO";
        case 34: return "LOW_LATENCY_OFFBODY_DETECT";
        case 35: return "ACCELEROMETER_UNCALIBRATED";
        default: return "?";
    }
}

// 是不是「能拿来算步」的加速度类事件：
//   1  ACCELEROMETER（含重力）
//   10 LINEAR_ACCELERATION（已去重力）
//   35 ACCELEROMETER_UNCALIBRATED（含重力，data[3..5] 是 bias）
static inline bool is_accel_type(int t) {
    return t == ASENSOR_TYPE_ACCELEROMETER ||
           t == ASENSOR_TYPE_LINEAR_ACCELERATION ||
           t == ASENSOR_TYPE_ACCELEROMETER_UNCALIBRATED;
}

// 每个类型第一次出现时报一行，带人类可读名，避免再看 type0=33 猜是哪个
static void bump_type(int t) {
    if (t < 0 || t >= kTypeSlots) return;
    g_type_cnt[t].fetch_add(1, std::memory_order_relaxed);
    if (!g_type_seen[t].exchange(true, std::memory_order_relaxed)) {
        LOGI("sensor type %d (%s) first seen", t, sensor_type_name(t));
    }
}

// 加速度改写计数（独立于 STEP_COUNTER，两者互不影响诊断）
static std::atomic<long long> g_accel_patched{0};
// 收到加速度事件但仿真没开——这是「App 在读、但模块没注」的直接证据
static std::atomic<bool> g_accel_seen_simoff{false};
// 陀螺仪 / 旋转矢量改写计数。
// 这两路传感器单独看没有「步数」意义，但它们和加速度是同一段跑步动作的不同投影：
// 加速度在周期震荡而陀螺仪/旋转矢量恒为 0，等于告诉 App「人没在动」，会被多传感器
// 融合一致性校验判掉。所以仿真开启时同步注入同频率的微幅角速度/姿态扰动。
static std::atomic<long long> g_gyro_patched{0};
static std::atomic<long long> g_rv_patched{0};
static std::atomic<bool> g_gyro_seen_simoff{false};
static std::atomic<bool> g_rv_seen_simoff{false};

// ---------------------------------------------------------------------------
// STEP_COUNTER 基线锚定
// 闪动校园启动时会先读一次硬件计步器作为基准，之后只关心「相对基准的增量」。
// 旧逻辑直接把 offset（含 sim_steps）加到硬件读数上：仿真一启动 sim_steps 从 0
// 开始累加，但硬件读数在两次读取之间可能已经自己往前走了一截，于是输出值出现
// 跳变甚至倒退，触发 App 的防作弊清零。
// 新逻辑：仿真开启的瞬间抓住当前硬件读数，把「以后应该输出的值」锁成一条平滑
// 递增曲线（cur + sim_steps），永不倒退、永不自发跳变。
// ---------------------------------------------------------------------------
static std::atomic<bool>   g_base_valid{false};   // 是否已经锚定
static std::atomic<long long> g_last_out{0};      // 上一次输出的计步器值（单调不减）
static std::atomic<long long> g_hw_at_anchor{0};  // 锚定时刻的硬件原始读数（诊断用）
static std::atomic<long long> g_cnt_patched{0};   // STEP_COUNTER 改写次数
static std::atomic<bool>   g_cnt_seen_simoff{false};
// 重入深度：防止 read→filterEvents 这种嵌套调用把同一批事件打两遍
static thread_local int g_depth = 0;

// ---------------------------------------------------------------------------
// 传感器事件处理
// ---------------------------------------------------------------------------
static inline void patch_event(ASensorEvent *ev) {
    if (ev->type == ASENSOR_TYPE_STEP_COUNTER) {
        bool sim_on = g_cfg.sim_active.load(std::memory_order_relaxed);
        long long sim = g_sim_steps.load(std::memory_order_relaxed);
        long long offset = g_cfg.step_offset.load(std::memory_order_relaxed);

        uint64_t hw = ev->u64.step_counter;
        long long out;

        if (g_cfg.absolute_mode.load(std::memory_order_relaxed)) {
            out = g_cfg.absolute_value.load(std::memory_order_relaxed) + sim;
        } else if (sim_on) {
            // 仿真开启：以「锚定时刻硬件读数 + 已仿真步数」为输出曲线。
            // 首次进入（或上次锚定已被停止仿真清掉）时锚定。
            if (!g_base_valid.exchange(true, std::memory_order_relaxed)) {
                g_hw_at_anchor.store((long long)hw, std::memory_order_relaxed);
                g_last_out.store((long long)hw, std::memory_order_relaxed);
                LOGI("step_counter baseline anchored: hw=%llu (out starts here)",
                     (unsigned long long)hw);
            }
            out = (long long)hw + (sim - g_hw_at_anchor.load(std::memory_order_relaxed));
            if (out < 0) out = 0;
            // 单调钳制：输出只增不减，杜绝倒退触发的防作弊清零
            long long prev = g_last_out.load(std::memory_order_relaxed);
            if (out < prev) out = prev;
            g_last_out.store(out, std::memory_order_relaxed);
        } else {
            // 仿真没开：只做用户手动偏移，并且同样单调，避免回绕跳变
            if (offset != 0) {
                long long nv = (long long)hw + offset;
                if (nv < 0) nv = 0;
                out = nv;
            } else {
                out = (long long)hw;
            }
            long long prev = g_last_out.load(std::memory_order_relaxed);
            if (g_base_valid.load(std::memory_order_relaxed) && out < prev) out = prev;
            g_last_out.store(out, std::memory_order_relaxed);
            if (!g_cnt_seen_simoff.exchange(true, std::memory_order_relaxed)) {
                LOGI("step_counter seen but sim off (hw=%llu offset=%lld)",
                     (unsigned long long)hw, offset);
            }
        }

        uint64_t outv = (uint64_t)out;
        memcpy(&ev->u64.step_counter, &outv, sizeof(outv));
        g_patched_events.fetch_add(1, std::memory_order_relaxed);
        long long c = g_cnt_patched.fetch_add(1, std::memory_order_relaxed) + 1;
        if (c == 1 || c % 200 == 0) {
            long long sim_delta = g_base_valid.load(std::memory_order_relaxed)
                ? (long long)hw - g_hw_at_anchor.load(std::memory_order_relaxed) : 0;
            LOGI("step_counter patch #%lld: hw=%llu out=%llu sim_steps=%lld "
                 "hw_delta=%lld sim=%d offset=%lld",
                 c, (unsigned long long)hw, (unsigned long long)outv, sim,
                 sim_delta, sim_on ? 1 : 0, offset);
        }
    } else if (ev->type == ASENSOR_TYPE_STEP_DETECTOR) {
        if (g_cfg.sim_active.load(std::memory_order_relaxed)) {
            ev->data[0] = 1.0f;   // 原设计：每个检测事件强制为 1 步
            g_patched_events.fetch_add(1, std::memory_order_relaxed);
            static std::atomic<long long> n_det{0};
            long long c = n_det.fetch_add(1, std::memory_order_relaxed) + 1;
            if (c == 1 || c % 200 == 0) {
                LOGI("step_detector patch #%lld: forced 1.0", c);
            }
        } else if (g_cfg.absolute_mode.load(std::memory_order_relaxed)) {
            ev->data[0] = 0.0f;
        }
    } else if (is_accel_type(ev->type)) {
        // 波形严格对齐 FuckRun / RunHook 原设计（对闪动校园实测兼容）：
        //     X = 0
        //     Y = 1.5·cos(2πft) + jitter(±0.15)
        //     Z = 9.81 + 3.5·sin(2πft) + jitter(±0.15)
        // 必须是完整正负正弦。此前用的单向尖峰 s³ 只有正向冲量，400Hz 采样下
        // 绝大多数样本贴着 9.81 重力基线，自研计步器做带通滤波后能量不足，
        // 判定为静止 → 结算步数为 0。
        //
        // 覆盖三种「能算步」的加速度事件：
        //   1  ACCELEROMETER               含重力，Z 基线 9.81
        //   10 LINEAR_ACCELERATION         已去重力，Z 基线 0
        //   35 ACCELEROMETER_UNCALIBRATED  含重力，data[0..2]=实测值 data[3..5]=bias
        bool sim_on = g_cfg.sim_active.load(std::memory_order_relaxed) &&
                      g_cfg.sim_accel.load(std::memory_order_relaxed);
        if (sim_on) {
            int rate = g_cur_rate.load(std::memory_order_relaxed);
            if (rate <= 0) rate = g_cfg.sim_rate_min.load(std::memory_order_relaxed);
            if (rate > 0) {
                double f = (double)rate / 60.0;                 // 步频 Hz
                double t = (double)ev->timestamp * 1e-9;        // 秒
                double ph = 2.0 * M_PI * f * t;
                double base = (ev->type == ASENSOR_TYPE_LINEAR_ACCELERATION) ? 0.0 : 9.81;
                double z = base + 3.5 * sin(ph);
                double y = 1.5 * cos(ph);
                double jy = ((double)rand() / (double)RAND_MAX - 0.5) * 0.3;
                double jz = ((double)rand() / (double)RAND_MAX - 0.5) * 0.3;
                ev->data[0] = 0.0f;
                ev->data[1] = (float)(y + jy);
                ev->data[2] = (float)(z + jz);
                if (ev->type == ASENSOR_TYPE_ACCELEROMETER_UNCALIBRATED) {
                    // bias 保持不动，让实测值 - bias 后仍是同一条波形
                    ev->data[3] = ev->data[0];
                    ev->data[4] = ev->data[1];
                    ev->data[5] = ev->data[2];
                }
                g_patched_events.fetch_add(1, std::memory_order_relaxed);
                g_accel_patched.fetch_add(1, std::memory_order_relaxed);
                static std::atomic<long long> n_acc{0};
                long long c = n_acc.fetch_add(1, std::memory_order_relaxed) + 1;
                if (c == 1) {
                    LOGI("first accel patch: type=%d (%s) rate=%d pid=%d",
                         ev->type, sensor_type_name(ev->type), rate, getpid());
                }
            }
        } else if (!g_accel_seen_simoff.exchange(true, std::memory_order_relaxed)) {
            // 关键诊断：App 确实在读加速度，但仿真没开（或 sim_accel 关着）
            LOGI("accel event %d (%s) seen but sim off "
                 "(sim_active=%d sim_accel=%d)",
                 ev->type, sensor_type_name(ev->type),
                 g_cfg.sim_active.load() ? 1 : 0,
                 g_cfg.sim_accel.load() ? 1 : 0);
        }
    } else if (ev->type == ASENSOR_TYPE_GYROSCOPE ||
               ev->type == ASENSOR_TYPE_GYROSCOPE_UNCALIBRATED) {
        // 陀螺仪：与加速度同频率、同相位的微幅角速度。
        // 跑步时躯干绕 Y 轴（俯仰方向）每秒轻微摆动 1~2 次，峰值幅度约 ±30°/s。
        // 与加速度用同一个 2πft 相位，保证多传感器融合时「动作是同一段」。
        // data[0..2] = 角速度 rad/s（X/Y/Z），GYROSCOPE_UNCALIBRATED 时 data[3..5] 是 bias。
        bool sim_on = g_cfg.sim_active.load(std::memory_order_relaxed) &&
                      g_cfg.sim_accel.load(std::memory_order_relaxed);
        if (sim_on) {
            int rate = g_cur_rate.load(std::memory_order_relaxed);
            if (rate <= 0) rate = g_cfg.sim_rate_min.load(std::memory_order_relaxed);
            if (rate > 0) {
                double f = (double)rate / 60.0;
                double t = (double)ev->timestamp * 1e-9;
                double ph = 2.0 * M_PI * f * t;
                const double A = 0.52;                    // ≈30°/s 峰值
                double gx = 0.15 * A * cos(ph);           // 侧向微摆
                double gy = A * cos(ph);                  // 主摆动轴
                double gz = 0.12 * A * sin(ph);           // 偏航微抖
                double jy = ((double)rand() / (double)RAND_MAX - 0.5) * 0.06;
                ev->data[0] = (float)gx;
                ev->data[1] = (float)(gy + jy);
                ev->data[2] = (float)gz;
                if (ev->type == ASENSOR_TYPE_GYROSCOPE_UNCALIBRATED) {
                    ev->data[3] = 0.0f;   // bias 归零，让实测值 - bias 仍是同一条曲线
                    ev->data[4] = 0.0f;
                    ev->data[5] = 0.0f;
                }
                g_patched_events.fetch_add(1, std::memory_order_relaxed);
                g_gyro_patched.fetch_add(1, std::memory_order_relaxed);
                static std::atomic<long long> n_gy{0};
                long long c = n_gy.fetch_add(1, std::memory_order_relaxed) + 1;
                if (c == 1) {
                    LOGI("first gyro patch: rate=%d A=%.2f rad/s pid=%d", rate, A, getpid());
                }
            }
        } else if (!g_gyro_seen_simoff.exchange(true, std::memory_order_relaxed)) {
            LOGI("gyroscope event seen but sim off (sim_active=%d)",
                 g_cfg.sim_active.load() ? 1 : 0);
        }
    } else if (ev->type == ASENSOR_TYPE_ROTATION_VECTOR ||
               ev->type == ASENSOR_TYPE_GAME_ROTATION_VECTOR) {
        // 旋转矢量：四元数 x,y,z,w（w 在 data[3]）。
        // App 若用旋转矢量做姿态解算再反推步态，恒定的四元数同样会被判静止。
        // 这里给一个绕 X 轴的小角度往复摆动（±2.5°），与步频同相。
        bool sim_on = g_cfg.sim_active.load(std::memory_order_relaxed) &&
                      g_cfg.sim_accel.load(std::memory_order_relaxed);
        if (sim_on) {
            int rate = g_cur_rate.load(std::memory_order_relaxed);
            if (rate <= 0) rate = g_cfg.sim_rate_min.load(std::memory_order_relaxed);
            if (rate > 0) {
                double f = (double)rate / 60.0;
                double t = (double)ev->timestamp * 1e-9;
                double ph = 2.0 * M_PI * f * t;
                double half = 0.0436;                       // ±2.5° 的一半
                double qx = sin(half * sin(ph));
                double qw = cos(half * sin(ph));
                ev->data[0] = (float)qx;
                ev->data[1] = 0.0f;
                ev->data[2] = 0.0f;
                ev->data[3] = (float)qw;
                if (ev->type == ASENSOR_TYPE_ROTATION_VECTOR) {
                    // data[4] 是预估航向精度（rad），保持小值表示姿态可信
                    ev->data[4] = 0.05f;
                }
                g_patched_events.fetch_add(1, std::memory_order_relaxed);
                g_rv_patched.fetch_add(1, std::memory_order_relaxed);
                static std::atomic<long long> n_rv{0};
                long long c = n_rv.fetch_add(1, std::memory_order_relaxed) + 1;
                if (c == 1) {
                    LOGI("first rotation_vector patch: type=%d rate=%d pid=%d",
                         ev->type, rate, getpid());
                }
            }
        } else if (!g_rv_seen_simoff.exchange(true, std::memory_order_relaxed)) {
            LOGI("rotation_vector event %d (%s) seen but sim off",
                 ev->type, sensor_type_name(ev->type));
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
            LOGI("slot[%d] first read: n=%zd type0=%d (%s)",
                 I, n, events[0].type, sensor_type_name(events[0].type));
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
// 多进程仿真步数同步（前置声明，sim_loop 用）
// 闪动校园同时开好几个进程（主进程 + :push 之类），每个进程都有自己的一份
// 模块副本、各自跑 sim_loop。若各自独立累加，同一时刻读同一个硬件计步器会得到
// 互不相同的步数，跨进程一致性校验直接暴露。
// 做法：每个进程按 pid 写自己的分片，读取时把所有分片求和，得到全局唯一的
// 仿真步数序列——这样任何进程算出来的计步器曲线都完全一致。
// ---------------------------------------------------------------------------
static const char *CTL_SIMDIR = "/data/data/" TARGET_PKG_STR "/files/stepspoof_sim";
static void sim_write_shard(long long steps, int rate);
static long long sim_read_peers();
static void sim_prune_shards();

// 共享步频：由主进程决定一次，所有进程照抄。
// 之前每个进程在自己 tick 里随机锁一个步频，四个进程锁出 107/105/100/118，
// 而 App 实际吃到的波形频率取决于哪个进程真正在 patch 加速度；控制台设的区间
// 和 App 显示的步频因此永远对不上。现在只有 main=1 的进程随机，子进程读它写下的值。
static const char *CTL_CADENCE = "/data/data/" TARGET_PKG_STR "/files/stepspoof_sim/cadence";
static void sim_write_cadence(int rate, int lo, int hi);
static bool sim_read_cadence(int *rate, int *lo, int *hi);

// ---------------------------------------------------------------------------
// 步频仿真线程
// 每 500ms tick：在 [min, max] 范围内随机取一个步频（步/分钟），
// 换算为该 tick 应增加的步数（带小数累加）。
// 仿真步数分为「本进程 local」+「其它进程分片之和 other」，对外暴露的
// g_sim_steps 是两者之和，保证所有进程看到同一条步数曲线。
// ---------------------------------------------------------------------------
static void sim_loop() {
    LOGI("sim_loop started");
    unsigned int seed = (unsigned int)time(nullptr) ^ (unsigned int)getpid();
    srandom(seed);

    constexpr int64_t TICK_MS = 500;
    bool prev_active = false;
    int picked = 0, picked_lo = -1, picked_hi = -1;
    long long ticks = 0;
    long long prune_tick = 0;
    long long hb_last_steps = 0;
    int64_t hb_last_ms = 0;

    for (;;) {
        usleep((useconds_t)(TICK_MS * 1000));

        bool active = g_cfg.sim_active.load(std::memory_order_relaxed)
                   && g_cfg.enabled.load(std::memory_order_relaxed);

        if (active != prev_active) {
            // 仿真状态翻转时复位「仿真未开启时已上报」标志，
            // 否则一次 sim off → on → off 之后就再也看不到这类诊断行了
            g_accel_seen_simoff.store(false, std::memory_order_relaxed);
            g_cnt_seen_simoff.store(false, std::memory_order_relaxed);
            g_gyro_seen_simoff.store(false, std::memory_order_relaxed);
            g_rv_seen_simoff.store(false, std::memory_order_relaxed);
            // 心跳基线必须跟着复位：g_local_steps 在 IDLE 时会清零，而
            // hb_last_steps 还留着上一段的终值，两者相减就是负数（日志3 里
            // rate=138 meas=-75 就是这么来的）。
            hb_last_steps = 0;
            hb_last_ms = 0;
            LOGI("sim state -> %s", active ? "ACTIVE" : "IDLE");
        }

        int lo = g_cfg.sim_rate_min.load(std::memory_order_relaxed);
        int hi = g_cfg.sim_rate_max.load(std::memory_order_relaxed);
        if (lo < 0) lo = 0;
        if (hi < lo) hi = lo;

        if (!active) {
            if (prev_active) {
                // 仿真由开转关：清掉锚定状态，下次开启重新锚定到当前硬件读数。
                // 不清的话下次开启会沿用上一段跑步的基线，输出值凭空跳一大截。
                if (g_base_valid.exchange(false, std::memory_order_relaxed)) {
                    LOGI("sim off: baseline re-arm (last_out=%lld)",
                         g_last_out.load(std::memory_order_relaxed));
                }
                g_cur_rate.store(0, std::memory_order_relaxed);
                if (g_is_main_proc) unlink(CTL_CADENCE);
                LOGI("sim loop idle: sim_steps total=%lld",
                     g_sim_steps.load(std::memory_order_relaxed));
            }
            prev_active = false;
            // 停止仿真时只清本进程的局部累加器和自己的分片（各进程独立，
            // 非主进程不碰公共状态），保证下次开启从 0 重新累加，
            // 上一段的步数不会被带进新一轮跑步。
            g_local_steps.store(0, std::memory_order_relaxed);
            g_sim_other.store(0, std::memory_order_relaxed);
            g_sim_steps.store(0, std::memory_order_relaxed);
            sim_accum = 0.0;
            sim_write_shard(0, 0);
            continue;
        }

        // 步频稳定性（原设计关键）：只在「开始仿真」或「区间被改动」时取一次
        // 随机值并锁定。每 500ms 重随机会让步态周期抖动，自研计步器直接判成
        // 非人步态；真实跑步的步频在整段里也是近似恒定的。
        // 随机只由主进程做：多进程时子进程读主进程写下的值，否则各锁各的，
        // App 吃到的波形频率和界面上设的对不上。
        if (!prev_active || lo != picked_lo || hi != picked_hi) {
            picked_lo = lo;
            picked_hi = hi;
            prev_active = true;
            if (g_is_main_proc) {
                picked = lo;
                if (hi > lo) picked = lo + (int)(random() % (hi - lo + 1));
                sim_write_cadence(picked, lo, hi);
                g_cur_rate.store(picked, std::memory_order_relaxed);
                LOGI("sim cadence locked: %d spm (range %d-%d) [main]", picked, lo, hi);
            } else {
                int r = 0, rlo = -1, rhi = -1;
                if (sim_read_cadence(&r, &rlo, &rhi) && rlo == lo && rhi == hi
                    && r >= lo && r <= hi) {
                    picked = r;
                    g_cur_rate.store(picked, std::memory_order_relaxed);
                    LOGI("sim cadence followed: %d spm (range %d-%d) [child]", picked, lo, hi);
                } else {
                    // 主进程还没来得及写：先按下限跑，下一 tick 就同步过去了
                    picked = lo;
                    g_cur_rate.store(picked, std::memory_order_relaxed);
                    LOGW("sim cadence pending, fallback %d spm (range %d-%d) [child]",
                         picked, lo, hi);
                }
            }
        } else {
            if (!g_is_main_proc) {
                // 子进程每 tick 跟一次：主进程重锁后立刻同步，不用等区间变化
                int r = 0, rlo = -1, rhi = -1;
                if (sim_read_cadence(&r, &rlo, &rhi) && rlo == lo && rhi == hi
                    && r >= lo && r <= hi && r != picked) {
                    picked = r;
                    LOGI("sim cadence resync: %d spm (range %d-%d) [child]", picked, lo, hi);
                }
            }
            g_cur_rate.store(picked, std::memory_order_relaxed);
        }

        // 每 tick 应增加的步数 = picked * TICK_MS / 60000
        // 只有主进程真正累加：四个进程都跑 sim_loop，如果各自累加再互相求和，
        // 全局步数就是真实值的 N 倍（日志里 procs=3 时 local=67、peers=135、
        // total=202 就是三个进程各自累加的产物）。App 读到的步频因此被放大
        // 到设定值的 N 倍，和界面上设的对不上。子进程只读不写。
        if (g_is_main_proc) {
            double steps_this_tick = (double)picked * (double)TICK_MS / 60000.0;
            sim_accum += steps_this_tick;

            long long add = (long long)floor(sim_accum);
            if (add > 0) {
                sim_accum -= (double)add;
                g_local_steps.fetch_add(add, std::memory_order_relaxed);
            }
        }
        // 每 tick 落一次分片，并汇总其它进程的仿真步数。
        // 汇总放在 tick 内做（而不是只在心跳里做）是为了让步数曲线跟得上：
        // 心跳 10 秒一次，中间 10 秒的跨进程差异足够被一致性校验抓到。
        // 子进程写 0：它的分片只为让 procs 计数反映真实进程数，不参与累加。
        sim_write_shard(g_is_main_proc ? g_local_steps.load(std::memory_order_relaxed) : 0, picked);
        long long other = sim_read_peers();
        g_sim_other.store(other, std::memory_order_relaxed);
        g_sim_steps.store(g_local_steps.load(std::memory_order_relaxed) + other,
                          std::memory_order_relaxed);
        if (++prune_tick % 40 == 0) sim_prune_shards();

        // 心跳：每 10 秒（20 tick）打一行运行快照。
        // 之前没有周期日志，设备 C 那 12 分钟的空白没法判断是「没在跑」还是
        // 「在跑但没读传感器」，现在这一行直接给出答案。
        if (++ticks % 20 == 0) {
            // meas = 本进程这一窗口内实测出的步频。accel 改动量和步数是两套独立
            // 计时（一个是采样事件数、一个是仿真线程累加），光看两个数放一起会
            // 以为「改了 8 万个事件只走 166 步」是异常，meas 直接给出真实节拍。
            int64_t now = mono_ms();
            long long cur = g_local_steps.load(std::memory_order_relaxed);
            int meas = 0;
            if (hb_last_ms > 0) {
                double span = (double)(now - hb_last_ms) / 1000.0;
                long long delta = cur - hb_last_steps;
                if (span > 0.5) meas = (int)((double)delta * 60.0 / span + 0.5);
            }
            // 首条心跳或刚复位时 hb_last_ms=0，meas 无意义，只打实测值不打 0
            if (hb_last_ms == 0 || !active) meas = -1;
            hb_last_ms = now;
            hb_last_steps = cur;
            LOGI("heartbeat: rate=%d meas=%d this_steps=%lld all_steps=%lld procs=%d "
                 "hits=%lld patched=%lld "
                 "accel=%lld/lp=%lld gyro=%lld/gp=%lld rv=%lld/rp=%lld "
                 "stepdet=%lld stepcnt=%lld/cp=%lld anchored=%d "
                 "base_hw=%lld last_out=%lld",
                 picked, meas, cur, g_sim_steps.load(std::memory_order_relaxed),
                 g_sim_peers.load(),
                 g_hook_hits.load(), g_patched_events.load(),
                 g_type_cnt[ASENSOR_TYPE_ACCELEROMETER].load(), g_accel_patched.load(),
                 g_type_cnt[ASENSOR_TYPE_GYROSCOPE].load(), g_gyro_patched.load(),
                 g_type_cnt[ASENSOR_TYPE_ROTATION_VECTOR].load(), g_rv_patched.load(),
                 g_type_cnt[ASENSOR_TYPE_STEP_DETECTOR].load(),
                 g_type_cnt[ASENSOR_TYPE_STEP_COUNTER].load(), g_cnt_patched.load(),
                 g_base_valid.load() ? 1 : 0,
                 g_hw_at_anchor.load(), g_last_out.load());
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

static int64_t mono_ms();

// 读进程启动时刻（/proc/<pid>/stat 第 22 字段，单位 jiffies）。
// 光看 /proc/<pid> 是否存在不够：pid 会被回收复用，上一个被杀的进程留下的分片
// 会被当成「还活着的兄弟进程」读进去（日志里 procs 从 7 掉到 3、peers 从 1492
// 掉到 135 就是这么来的）。带上 starttime 才能判定分片是不是同一个进程写的。
static unsigned long long proc_starttime(int pid) {
    char p[64];
    snprintf(p, sizeof(p), "/proc/%d/stat", pid);
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    char b[512] = {};
    ssize_t r = read(fd, b, sizeof(b) - 1);
    close(fd);
    if (r <= 0) return 0;
    // comm 字段可能含空格和括号，从最后一个 ')' 之后开始数
    char *rp = strrchr(b, ')');
    if (rp == nullptr) return 0;
    char *s = rp + 1;
    // ')' 之后是第 3 字段（state），starttime 是第 22 字段，再往后跳过 19 个
    for (int f = 3; f < 22; ++f) {
        while (*s == ' ') ++s;
        while (*s && *s != ' ') ++s;
    }
    while (*s == ' ') ++s;
    return strtoull(s, nullptr, 10);
}

// 分片是否还由活着的同一进程持有
static bool shard_alive(int pid, unsigned long long starttime) {
    if (pid <= 0) return false;
    char proc[64];
    snprintf(proc, sizeof(proc), "/proc/%d", pid);
    if (access(proc, F_OK) != 0) return false;
    if (starttime == 0) return true;      // 读不到 starttime 时退化成只管存在性
    unsigned long long cur = proc_starttime(pid);
    if (cur == 0) return true;            // 同上
    return cur == starttime;
}

// 把自己的分片写出去。文件名用 pid 区分，内容是「步数 步频 启动时刻」。
static void sim_write_shard(long long steps, int rate) {
    char path[256];
    snprintf(path, sizeof(path), "%s/%d", CTL_SIMDIR, getpid());
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (fd < 0) return;
    char b[96];
    int n = snprintf(b, sizeof(b), "%lld %d %llu\n", steps, rate, proc_starttime(getpid()));
    (void)!write(fd, b, (size_t)n);
    close(fd);
}

// 汇总所有分片。返回别的进程的步数之和，并统计活着的分片数。
// 死进程的分片在这一步就删掉，不再等 40 tick 的定期清理。
static long long sim_read_peers() {
    DIR *d = opendir(CTL_SIMDIR);
    if (d == nullptr) {
        if (mkdir(CTL_SIMDIR, 0777) != 0 && errno != EEXIST) return 0;
        d = opendir(CTL_SIMDIR);
        if (d == nullptr) return 0;
    }
    int self = getpid();
    long long sum = 0;
    int peers = 0;
    struct dirent *de;
    while ((de = readdir(d)) != nullptr) {
        if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
        int pid = atoi(de->d_name);
        if (pid <= 0) continue;
        char path[256];
        snprintf(path, sizeof(path), "%s/%s", CTL_SIMDIR, de->d_name);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        char b[96] = {};
        ssize_t r = read(fd, b, sizeof(b) - 1);
        close(fd);
        long long steps = 0, rate = 0;
        unsigned long long st = 0;
        int got = (r > 0) ? sscanf(b, "%lld %lld %llu", &steps, &rate, &st) : 0;
        if (got < 2) { unlink(path); continue; }
        if (pid != self && !shard_alive(pid, st)) {
            unlink(path);
            continue;
        }
        ++peers;
        if (pid == self) continue;
        if (steps > 0) sum += steps;
    }
    closedir(d);
    g_sim_peers.store(peers, std::memory_order_relaxed);
    return sum;
}

// 清理已经不存在的进程留下的分片（兜底：sim_read_peers 已经顺手删过了）
static void sim_prune_shards() {
    DIR *d = opendir(CTL_SIMDIR);
    if (d == nullptr) return;
    int self = getpid();
    struct dirent *de;
    while ((de = readdir(d)) != nullptr) {
        if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
        int pid = atoi(de->d_name);
        if (pid <= 0 || pid == self) continue;
        if (!shard_alive(pid, 0)) {
            char path[256];
            snprintf(path, sizeof(path), "%s/%s", CTL_SIMDIR, de->d_name);
            unlink(path);
        }
    }
    closedir(d);
}

// 主进程写共享步频，格式「步频 下限 上限」。
static void sim_write_cadence(int rate, int lo, int hi) {
    if (mkdir(CTL_SIMDIR, 0777) != 0 && errno != EEXIST) return;
    int fd = open(CTL_CADENCE, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (fd < 0) return;
    char b[64];
    int n = snprintf(b, sizeof(b), "%d %d %d\n", rate, lo, hi);
    (void)!write(fd, b, (size_t)n);
    close(fd);
}

// 子进程读共享步频。区间对不上说明这不是本次录音写下的值，按无效处理。
static bool sim_read_cadence(int *rate, int *lo, int *hi) {
    int fd = open(CTL_CADENCE, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char b[64] = {};
    ssize_t r = read(fd, b, sizeof(b) - 1);
    close(fd);
    if (r <= 0) return false;
    int a = 0, c = 0, d = 0;
    if (sscanf(b, "%d %d %d", &a, &c, &d) != 3) return false;
    if (rate) *rate = a;
    if (lo) *lo = c;
    if (hi) *hi = d;
    return true;
}

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
    // 兜底：区间反了就交换。写端（WebUI 拖动、conf 手改、多进程并发写）都可能
    // 出现 min > max 的瞬间，不修的话 sim_loop 里的 rand 区间为负，步频会算出
    // 离谱值或直接卡住。
    int lo = g_cfg.sim_rate_min.load(), hi = g_cfg.sim_rate_max.load();
    if (lo > hi) { g_cfg.sim_rate_min.store(hi); g_cfg.sim_rate_max.store(lo); }
}

// 把当前配置写回 CTL_CONF。
// 关键：这段代码跑在目标 App 进程里，"/data/data/<pkg>" 会按进程自己的 user
// 解析（user 0 → /data/user/0，999 空间 → /data/user/999），天然写对沙盒。
// WebUI 的 persist() 走 ksu.exec 起的是全局 root shell，那里的 /data/data
// 恒等于 /data/user/0，所以从 999 空间打开 WebUI 也只会写进 user 0，
// 999 空间的主进程永远读不到自己的 conf。写端必须落到进程内部。
static void save_conf_native() {
    std::string txt;
    txt += "enabled=" + std::to_string(g_cfg.enabled.load() ? 1 : 0) + "\n";
    txt += "offset=" + std::to_string(g_cfg.step_offset.load()) + "\n";
    txt += "absolute=" + std::to_string(g_cfg.absolute_mode.load() ? 1 : 0) + "\n";
    txt += "absolute_value=" + std::to_string(g_cfg.absolute_value.load()) + "\n";
    txt += "sim_active=" + std::to_string(g_cfg.sim_active.load() ? 1 : 0) + "\n";
    txt += "sim_min=" + std::to_string(g_cfg.sim_rate_min.load()) + "\n";
    txt += "sim_max=" + std::to_string(g_cfg.sim_rate_max.load()) + "\n";
    txt += "sim_accel=" + std::to_string(g_cfg.sim_accel.load() ? 1 : 0) + "\n";
    txt += "stride=" + std::to_string(g_cfg.stride_cm.load()) + "\n";

    // 原子替换：先写同目录临时文件，fsync 落盘后再 rename 覆盖正式文件。
    // 同一空间里可能有多个进程（主进程 + :push 等）都在写 conf，也可能和
    // service.sh 的拷贝撞上。直接 O_TRUNC 覆写会让 conf_watch_loop 在
    // 截断与写完之间读到 0 字节或半截内容，配置被当成"无效"跳过甚至重置。
    // 同文件系统内 rename 是原子的，读端要么看到旧文件、要么看到新文件。
    std::string tmp = std::string(CTL_CONF) + ".tmp." + std::to_string(getpid());
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (fd < 0) {
        LOGW("save_conf_native open failed: %s", strerror(errno));
        return;
    }
    ssize_t wr = write(fd, txt.data(), txt.size());
    if (wr < 0 || (size_t)wr != txt.size()) {
        LOGW("save_conf_native write failed: %s", strerror(errno));
        close(fd);
        unlink(tmp.c_str());
        return;
    }
    fsync(fd);
    close(fd);
    chmod(tmp.c_str(), 0666);
    if (rename(tmp.c_str(), CTL_CONF) != 0) {
        LOGW("save_conf_native rename failed: %s", strerror(errno));
        unlink(tmp.c_str());
        return;
    }
    LOGI("conf written from native (%zu bytes)", txt.size());
}

static void write_state_file() {
    char js[896];
    int n = snprintf(js, sizeof(js),
        "enabled=%d\noffset=%lld\nabsolute=%d\nabsolute_value=%lld\n"
        "sim_active=%d\nsim_min=%d\nsim_max=%d\nsim_accel=%d\n"
        "hook_hits=%lld\npatched=%lld\nsim_steps=%lld\naccel_patched=%lld\n"
        "gyro_patched=%lld\nrv_patched=%lld\ncnt_patched=%lld\n"
        "local_steps=%lld\nother_steps=%lld\npeers=%d\n"
        "anchored=%d\nbase_hw=%lld\nlast_out=%lld\n"
        "acc=%lld\nlinacc=%lld\naccu=%lld\ngyro=%lld\nrv=%lld\n"
        "stepdet=%lld\nstepcnt=%lld\n"
        "stride=%d\ncur_rate=%d\n",
        g_cfg.enabled.load() ? 1 : 0, g_cfg.step_offset.load(),
        g_cfg.absolute_mode.load() ? 1 : 0, g_cfg.absolute_value.load(),
        g_cfg.sim_active.load() ? 1 : 0, g_cfg.sim_rate_min.load(),
        g_cfg.sim_rate_max.load(), g_cfg.sim_accel.load() ? 1 : 0,
        g_hook_hits.load(), g_patched_events.load(), g_sim_steps.load(),
        g_accel_patched.load(), g_gyro_patched.load(), g_rv_patched.load(),
        g_cnt_patched.load(),
        g_local_steps.load(), g_sim_other.load(), g_sim_peers.load(),
        g_base_valid.load() ? 1 : 0, g_hw_at_anchor.load(), g_last_out.load(),
        g_type_cnt[ASENSOR_TYPE_ACCELEROMETER].load(),
        g_type_cnt[ASENSOR_TYPE_LINEAR_ACCELERATION].load(),
        g_type_cnt[ASENSOR_TYPE_ACCELEROMETER_UNCALIBRATED].load(),
        g_type_cnt[ASENSOR_TYPE_GYROSCOPE].load(),
        g_type_cnt[ASENSOR_TYPE_ROTATION_VECTOR].load(),
        g_type_cnt[ASENSOR_TYPE_STEP_DETECTOR].load(),
        g_type_cnt[ASENSOR_TYPE_STEP_COUNTER].load(),
        g_cfg.stride_cm.load(), g_cur_rate.load());
    int fd = open(CTL_STATE, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (fd < 0) return;
    (void)!write(fd, js, (size_t)n);
    close(fd);
}

// 主进程才有资格写状态文件，避免子进程互相覆盖（g_is_main_proc 定义在全局区）

static void conf_watch_loop() {
    LOGI("conf_watch started: %s", CTL_CONF);
    struct stat st {};
    off_t last_sz = -1;
    time_t last_mt = 0;
    bool first = true;

    for (;;) {
        usleep(700 * 1000);

        if (stat(CTL_CONF, &st) == 0) {
            if (first || st.st_size != last_sz || st.st_mtime != last_mt) {
                first = false;
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

        if (g_is_main_proc) write_state_file();
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
        char js[2048];
        snprintf(js, sizeof(js),
                 "{\"enabled\":%d,\"step_offset\":%lld,\"absolute_mode\":%d,"
                 "\"absolute_value\":%lld,\"sim_active\":%d,\"sim_rate_min\":%d,"
                 "\"sim_rate_max\":%d,\"hook_hits\":%lld,\"patched_events\":%lld,"
                 "\"sim_steps\":%lld,\"hooks\":%d,\"sim_accel\":%d,\"cur_rate\":%d,"
                 "\"accel_patched\":%lld,\"gyro_patched\":%lld,\"rv_patched\":%lld,"
                 "\"cnt_patched\":%lld,\"local_steps\":%lld,\"other_steps\":%lld,"
                 "\"peers\":%d,\"anchored\":%d,\"base_hw\":%lld,\"last_out\":%lld,"
                 "\"acc\":%lld,\"linacc\":%lld,\"accu\":%lld,"
                 "\"gyro\":%lld,\"rv\":%lld,\"stepdet\":%lld,\"stepcnt\":%lld,"
                 "\"stride\":%d,\"speed_cmps\":%d,\"pkg\":\"%s\","
                 "\"port\":%d,\"uid\":%d,\"proc\":\"%s\",\"sensors\":%d}",
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
                 g_accel_patched.load(), g_gyro_patched.load(), g_rv_patched.load(),
                 g_cnt_patched.load(),
                 g_local_steps.load(), g_sim_other.load(), g_sim_peers.load(),
                 g_base_valid.load() ? 1 : 0, g_hw_at_anchor.load(), g_last_out.load(),
                 g_type_cnt[ASENSOR_TYPE_ACCELEROMETER].load(),
                 g_type_cnt[ASENSOR_TYPE_LINEAR_ACCELERATION].load(),
                 g_type_cnt[ASENSOR_TYPE_ACCELEROMETER_UNCALIBRATED].load(),
                 g_type_cnt[ASENSOR_TYPE_GYROSCOPE].load(),
                 g_type_cnt[ASENSOR_TYPE_ROTATION_VECTOR].load(),
                 g_type_cnt[ASENSOR_TYPE_STEP_DETECTOR].load(),
                 g_type_cnt[ASENSOR_TYPE_STEP_COUNTER].load(),
                 g_cfg.stride_cm.load(),
                 (int)((long long)g_cur_rate.load() * g_cfg.stride_cm.load() / 60),
                 TARGET_PKG,
                 g_webui_port.load(), (int)getuid(),
                 proc_name(), active_sensor_types());
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
        {
            int lo = g_cfg.sim_rate_min.load(), hi = g_cfg.sim_rate_max.load();
            if (lo > hi) { g_cfg.sim_rate_min.store(hi); g_cfg.sim_rate_max.store(lo); }
        }
        g_cfg.sim_accel.store(json_num(body, "sim_accel", g_cfg.sim_accel.load() ? 1 : 0) != 0);
        g_cfg.stride_cm.store((int)json_num(body, "stride", g_cfg.stride_cm.load()));
        save_conf_native();
        LOGI("cfg updated: enabled=%d offset=%lld absolute=%d abs_value=%lld "
             "sim_active=%d min=%d max=%d sim_accel=%d stride=%dcm pace=%d cm/s",
             g_cfg.enabled.load() ? 1 : 0, g_cfg.step_offset.load(),
             g_cfg.absolute_mode.load() ? 1 : 0, g_cfg.absolute_value.load(),
             g_cfg.sim_active.load() ? 1 : 0, g_cfg.sim_rate_min.load(),
             g_cfg.sim_rate_max.load(), g_cfg.sim_accel.load() ? 1 : 0,
             g_cfg.stride_cm.load(),
             g_cfg.sim_rate_max.load() * g_cfg.stride_cm.load() / 60);
        http_reply(fd, 200, "application/json", "{\"ok\":true}");
    } else if (method == "POST" && path == "/api/start") {
        g_cfg.enabled.store(true);
        g_cfg.sim_active.store(true);
        save_conf_native();
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
        save_conf_native();
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

    // 端口不能写死 8888：双开时 user 0 和 999 空间各有一个主进程都想绑它，
    // 后启动的那个 bind 失败就直接放弃了（日志里 18:22:57.504 与 11:12:34.077
    // 两条 "bind 8888: Address already in use"）。它绑不上就没有 WebUI，
    // 也就永远收不到 /api/start，整个 999 空间实例等于死的。
    // 改成从 8888 往后顺延，绑上哪个用哪个，由 /api/status 的 port 字段告知前端。
    int bound_port = 0;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    for (int p = kWebuiPortBase; p < kWebuiPortBase + kWebuiPortSpan; ++p) {
        addr.sin_port = htons((unsigned short)p);
        if (bind(srv, (sockaddr *)&addr, sizeof(addr)) == 0) { bound_port = p; break; }
        if (errno != EADDRINUSE) {
            LOGE("bind %d: %s", p, strerror(errno));
            close(srv);
            return;
        }
    }
    if (bound_port == 0) {
        LOGE("bind %d-%d: %s", kWebuiPortBase, kWebuiPortBase + kWebuiPortSpan - 1,
             strerror(errno));
        close(srv);
        return;
    }
    g_webui_port.store(bound_port, std::memory_order_relaxed);

    if (listen(srv, 8) != 0) { LOGE("listen: %s", strerror(errno)); close(srv); return; }

    LOGI("webui listening on 127.0.0.1:%d (uid=%d)", bound_port, getuid());

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

        // 首个日志落盘，附带一次环境快照，便于他人排障。
        // 时间戳带日期（行内时间戳只有时分秒，跨天无法定位是哪一天）；
        // 再跟设备名称与指纹，多台设备的日志才不会互相混淆。
        char when[32];
        struct timespec ts {};
        clock_gettime(CLOCK_REALTIME, &ts);
        struct tm tmv {};
        time_t secs = ts.tv_sec;
        localtime_r(&secs, &tmv);
        snprintf(when, sizeof(when), "%04d-%02d-%02d %02d:%02d:%02d",
                 tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                 tmv.tm_hour, tmv.tm_min, tmv.tm_sec);

        const DeviceInfo &dev = device_info();
        LOGI("===== stepspoof_joyrun v1.3.4 start %s pid=%d main=%d =====",
             when, getpid(), is_main ? 1 : 0);
        LOGI("device: %s | %s | %s", dev.name, dev.android, dev.abi);
        LOGI("device fingerprint: %s", dev.fp8);
        LOGI("device fingerprint full: %s",
             dev.fp_full[0] ? dev.fp_full : "(unavailable)");
        LOGI("log file: %s", g_log_path[0] ? g_log_path : "(not opened yet)");

        // PLT hook 没打上时，尝试 inline hook（Android 14+ 路径）
        if (g_hook_cnt.load() == 0) {
            try_install_sensor_hook();
        }

        // 仿真、控制通道在所有目标进程都起：真正读传感器的可能是子进程，
        // 只给主进程起会导致子进程 sim_active 恒为 false，波形永远不注入。
        // WebUI（占用 8888 端口）只在主进程起。
        g_is_main_proc = is_main;
        if (is_main) {
            std::thread(webui_loop).detach();
        }
        std::thread(sim_loop).detach();
        std::thread(conf_watch_loop).detach();
    }

private:
    zygisk::Api *api = nullptr;
    JNIEnv *env = nullptr;
    bool is_target = false;
    bool is_main = false;
};

REGISTER_ZYGISK_MODULE(StepSpoofModule)
