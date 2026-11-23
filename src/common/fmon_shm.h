/*
 * fmon_shm.h - fmon 守护进程 / 图形界面之间的共享内存契约
 *
 * 设计要点（v24）：
 *   1. 头部带 magic / abi_version / struct_size，任何一端结构体不一致时
 *      可以立即拒绝，而不是越界读到垃圾数据（旧版会直接 SIGBUS/SIGSEGV）。
 *   2. 使用 seqlock：写者把 seq 加一（奇数）后写数据，写完再加一（偶数）。
 *      读者在拷贝前后各读一次 seq，相等才认为这次快照有效。这样 UI 永远
 *      不会看到“一半旧一半新”的数据，也不需要把整个结构锁住。
 *   3. 所有历史曲线数组只由守护进程的发布线程推进一次，避免旧版
 *      history_index 被推进两次导致曲线只有一半数据的问题。
 *   4. 字段全部使用定长整数类型，便于跨模块 / 跨版本检查。
 *
 * 修改本文件中的结构体布局时，必须同时递增 FMON_ABI_VERSION，
 * 并重新编译守护进程与界面（旧 GUI 会被新守护进程拒绝，反之亦然）。
 */

#ifndef FMON_SHM_H
#define FMON_SHM_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 共享内存对象名。结构体布局变更时必须改名，避免新旧进程混用。 */
#define FMON_SHM_NAME        "/fmon_daemon_shm_v24"

/* 安装前缀：meson 会以 -DFMON_LOCALBASE="..." 传入；
 * 单独编译时回退到 FreeBSD 默认的 /usr/local。
 * 所有“可能变化”的路径都从这里派生，禁止写死 /usr/local。 */
#ifndef FMON_LOCALBASE
#define FMON_LOCALBASE       "/usr/local"
#endif

/* 基础系统路径（FreeBSD 15+ 固定）：/bin /sbin /usr/bin /usr/sbin 可直接使用。 */

/* 头部校验值："FMON" 的大端 ASCII。 */
/* 版本号：安装脚本与 --version 使用 */
#define FMON_VERSION_STRING  "1.3.7"

#define FMON_SHM_MAGIC       0x464D4F4DU
#define FMON_ABI_VERSION     24U  /* 24: NIC model 扩大，完整保留长型号 */

#define FMON_MAX_PROCS       32   /* 上报给界面的进程条数（按 CPU 排序） */
#define FMON_MAX_NAME_LEN    32   /* 进程名长度 */
#define FMON_MAX_USER_LEN    16   /* 用户名长度 */
#define FMON_MAX_PROCS_TOTAL 0    /* 保留：进程总数在 proc_total 字段 */
#define FMON_HISTORY_POINTS  1200 /* 历史曲线点数（1Hz 采样 = 20 分钟） */
#define FMON_MAX_GPUS        4
#define FMON_MAX_IFACES      8
#define FMON_MAX_DISKS       8
#define FMON_MAX_MOUNTS      16
#define FMON_MAX_CPUS        64
#define FMON_MAX_POOLS       4    /* 同时展示的 zpool 数量 */

/* 采样周期（毫秒），界面据此判断数据是否过期 */
#define FMON_SAMPLE_INTERVAL_MS 1000
#define FMON_STALE_AFTER_MS     5000

/* 守护进程状态 */
typedef enum {
    FMON_STATUS_INIT    = 0,  /* 正在初始化，数据不可用 */
    FMON_STATUS_RUNNING = 1,  /* 正常运行 */
    FMON_STATUS_STOPPED = 2   /* 正常退出（界面应显示“已停止”） */
} FmonStatus;

/* 电池状态位（与 <dev/acpica/acpiio.h> 中的定义一致） */
#define FMON_BATT_DISCHARGING 0x01u
#define FMON_BATT_CHARGING    0x02u
#define FMON_BATT_CRITICAL    0x04u

typedef struct {
    uint32_t pid;
    uint32_t uid;
    uint32_t threads;
    char     state;                 /* ps 风格状态字符：R/S/D/Z/I/T ... */
    char     name[FMON_MAX_NAME_LEN];
    char     user[FMON_MAX_USER_LEN];
    double   cpu_usage_pct;         /* 距上次采样的瞬时 CPU 占用 */
    double   mem_rss_kb;
    double   mem_pct;               /* 物理内存占比 */
} FmonProcInfo;

/* GPU 负载数据来源。FreeBSD 只对部分显卡暴露占用率，界面需要据此
 * 区分“真的 0%”和“没有传感器”，不能把两者显示成一个样子。 */
typedef enum {
    FMON_GPU_SENSOR_NONE   = 0,  /* 只探测到设备，没有可用的负载接口 */
    FMON_GPU_SENSOR_NVML   = 1,  /* NVIDIA：nvidia-smi（NVML）*/
    FMON_GPU_SENSOR_DRM    = 2,  /* AMD：libdrm_amdgpu 的 AMDGPU_INFO_SENSOR */
    FMON_GPU_SENSOR_SYSCTL = 3   /* 内核 sysctl（预留给未来驱动）*/
} FmonGpuSensor;

/* GPU 类型 */
typedef enum {
    FMON_GPU_DISCRETE   = 0,
    FMON_GPU_INTEGRATED = 1
} FmonGpuKind;

typedef struct {
    char     name[64];              /* 可读名称，如 "AMD Radeon 780M Graphics" */
    char     vendor[16];            /* "NVIDIA" / "AMD" / "Intel" / "Unknown" */
    char     pci_id[16];            /* "1002:15bf"；未知时为空 */
    char     slot[24];              /* PCI 槽位，如 pci0:1:0:0（来自 pciconf）*/
    uint32_t present;               /* 1 = 设备存在 */
    uint32_t kind;                  /* FmonGpuKind */
    uint32_t sensor;                /* FmonGpuSensor */
    uint32_t driver_loaded;         /* 1 = 有 DRM 节点或厂商驱动已加载 */
    uint32_t load_available;        /* 1 = 下面的 usage/mem/temp 有效 */
    uint32_t clock_mhz;             /* 当前核心频率（0 = 未知）*/
    double   usage_pct;             /* 负载百分比；不可用时为 -1 */
    double   mem_used_mb;           /* 显存（核显为 GTT 动态显存）已用 */
    double   mem_total_mb;
    double   temp_c;                /* 摄氏度；不可用时为 -1 */
    double   gtt_used_mb;           /* 共享内存（GTT）用量，供详情提示用 */
    double   gtt_total_mb;

    /* NVIDIA NVML 明细（AMD 卡保持 0 值） */
    char     driver_version[32];
    char     vbios_version[32];
    char     serial[64];
    char     uuid[64];
    uint32_t sm_count;          /* CUDA Cores（0=未知） */
    double   power_w;
    double   power_limit_w;
    double   util_mem_pct;      /* 显存控制器利用率 */
    double   util_enc_pct;      /* 编码器 */
    double   util_dec_pct;      /* 解码器 */
    double   util_jpeg_pct;
    double   util_ofa_pct;
    double   clock_sm_mhz;
    double   clock_mem_mhz;
    double   clock_video_mhz;
    double   fan_pct;
    double   bar1_used_mb;
    double   bar1_total_mb;
    double   pcie_rx_mbps;
    double   pcie_tx_mbps;
    uint32_t pcie_gen;
    uint32_t pcie_width;
    uint32_t ecc_mode;          /* 0=未知 1=关闭 2=开启 */
    uint64_t ecc_sbe;
    uint64_t ecc_dbe;
    uint32_t pstate;
} FmonGpuInfo;

typedef struct {
    char     name[16];              /* 例如 em0 / wlan0 */
    char     addr[48];              /* 首选 IPv4 / IPv6 地址（文本） */
    char     mac[20];               /* MAC 地址 */
    char     ipv4[48];              /* 首选 IPv4 */
    char     ipv6[64];              /* 首选 IPv6 */
    char     ssid[36];              /* Wi-Fi 网络名称 */
    uint32_t mtu;
    uint32_t flags;                 /* IFF_UP / IFF_RUNNING ... */
    uint32_t link_type;             /* FMON_LINK_* */
    uint32_t connected;             /* 1 = 已连接 */
    uint32_t channel;               /* Wi-Fi 信道，0 = 未知 */
    int32_t  signal_dbm;            /* Wi-Fi 信号强度，0 = 未知 */
    uint32_t speed_mbps;            /* 链路速率，0 = 未知 */
    uint64_t rx_bytes;              /* 累计字节数 */
    uint64_t tx_bytes;
    uint64_t rx_errors;
    uint64_t tx_errors;
    double   rx_kbps;               /* 本次采样区间内的速率 */
    double   tx_kbps;
    uint32_t detail_valid;          /* 1 = ssid/channel/signal/speed 探测成功 */
} FmonIfaceInfo;

/* 接口类型 */
#define FMON_LINK_UNKNOWN 0U
#define FMON_LINK_ETHER   1U
#define FMON_LINK_WIFI    2U
#define FMON_LINK_LOOP    3U
#define FMON_LINK_VIRTUAL 4U

/* 单个磁盘设备的 I/O 明细（devstat） */
typedef struct {
    char     name[16];              /* ada0 / da0 / nvme0 ... */
    char     model[64];             /* 设备型号 */
    char     serial[32];            /* 序列号 */
    uint64_t size_bytes;            /* 容量 */
    uint32_t kind;                  /* FMON_DISK_* */
    uint32_t rotation_rpm;          /* 0 = SSD / 未知 */
    uint64_t read_bytes;            /* 累计 */
    uint64_t write_bytes;
    uint64_t read_ops;              /* 累计操作数 */
    uint64_t write_ops;
    double   read_kbps;             /* 区间速率 */
    double   write_kbps;
    double   busy_pct;              /* 区间内设备繁忙百分比 */
    double   temp_c;                /* SMART 温度（°C）；<0 表示不可用 */
    int32_t  spare_pct;             /* Available spare；-1 不可用 */
    int32_t  wear_pct;              /* Percentage used；-1 不可用 */
    uint32_t smart_valid;           /* 1 = 上面的 SMART 字段有效 */
    uint32_t smart_source;          /* FMON_SMART_SRC_*，0 = 无 provider */
    uint32_t meta_valid;            /* 1 = model/serial/size/kind 至少部分由内核探测到 */
} FmonDiskInfo;

/* 磁盘类型 */
#define FMON_DISK_UNKNOWN 0U
#define FMON_DISK_SSD     1U
#define FMON_DISK_HDD     2U
#define FMON_DISK_FLASH   3U   /* 移动闪存盘/小容量 USB SSD */

/* kind 的高 8 位携带接口类型（不改变结构体大小 / ABI） */
#define FMON_DISK_TYPE_MASK   0x000000FFU
#define FMON_DISK_IFACE_MASK  0x0000FF00U
#define FMON_DISK_IFACE_NVME  0x00000100U
#define FMON_DISK_IFACE_SATA  0x00000200U
#define FMON_DISK_IFACE_USB   0x00000400U
#define FMON_DISK_IFACE_SCSI  0x00000800U

/* USB 协商速率（kind 的 16-19 位） */
#define FMON_DISK_USB_SPEED_MASK 0x000F0000U
#define FMON_DISK_USB2           0x00010000U   /* 480Mbps 及以下 */
#define FMON_DISK_USB3           0x00020000U   /* 5Gbps 及以上 */

/* 磁盘原生接口（kind 的 20-23 位） */
#define FMON_DISK_TRANSPORT_MASK 0x00F00000U
#define FMON_DISK_TS_SATA1       0x00100000U   /* 1.5 Gb/s */
#define FMON_DISK_TS_SATA2       0x00200000U   /* 3 Gb/s */
#define FMON_DISK_TS_SATA3       0x00300000U   /* 6 Gb/s */
#define FMON_DISK_TS_NVME        0x00400000U   /* PCIe/NVMe */

/* 内存条（dmidecode 探测，需要 root） */
#define FMON_MAX_MEM_MODULES 8

typedef struct {
    char     locator[32];       /* DIMM 0 / ChannelA-DIMM0 */
    char     bank[32];
    char     type[16];          /* DDR4 / DDR5 / LPDDR5 */
    char     manufacturer[32];
    uint32_t size_mb;
    uint32_t speed_mts;         /* 标称速率 */
    uint32_t configured_mts;    /* 实际运行速率 */
    uint32_t used;              /* 1 = 已安装 */
} FmonMemModule;

typedef struct {
    char     mountpoint[64];
    char     device[64];
    char     fstype[16];
    uint64_t total_bytes;
    uint64_t free_bytes;            /* 普通用户可用的空闲空间 */
    double   used_pct;
} FmonMountInfo;

/* 一个 zpool 的容量与 I/O 指标（支持多池） */
typedef struct {
    char     name[32];
    char     health[32];
    uint32_t available;             /* 1 = 本池数据有效 */
    uint64_t total_bytes;
    uint64_t alloc_bytes;
    uint64_t free_bytes;
    double   util_pct;
    uint64_t read_ops;              /* 累计 */
    uint64_t write_ops;
    uint64_t read_bytes;
    uint64_t write_bytes;
    double   read_kbps;             /* 区间速率 */
    double   write_kbps;
} FmonPoolInfo;

typedef struct {
    /* ------------------------------------------------------------------
     * 头部：任何裁剪/校验都必须从这几个字段开始
     * ------------------------------------------------------------------ */
    uint32_t magic;
    uint32_t abi_version;
    uint32_t struct_size;           /* sizeof(FmonShmData) */
    _Atomic uint32_t seq;           /* seqlock 序号，奇数表示正在写入 */
    int32_t  daemon_pid;
    int32_t  status;                /* FmonStatus */
    uint64_t last_update_us;        /* 最近一次发布的时间（CLOCK_MONOTONIC，微秒） */
    uint64_t daemon_start_us;       /* 守护进程启动时间（同上） */

    /* ------------------------------------------------------------------
     * 静态系统信息（只在启动时写入一次）
     * ------------------------------------------------------------------ */
    char     hostname[64];
    char     sys_version[64];
    char     cpu_model[128];
    uint64_t uptime_sec;
    uint16_t cpu_count;             /* 逻辑 CPU 数 */
    uint16_t cpu_core_count;        /* 实际采集到的核心数（<= FMON_MAX_CPUS） */
    uint32_t proc_total;            /* 系统进程总数 */

    /* 探测有效位：1 = 对应采集成功；0 = 不可用。
     * 0 不能同时表示“真实为 0”和“探测失败”，所以单独用位图。 */
    uint64_t probe_valid;
#define FMON_PROBE_SWAP        (1ull << 0)
#define FMON_PROBE_POOLS       (1ull << 1)
#define FMON_PROBE_MEM_MODULES (1ull << 2)
#define FMON_PROBE_MOUNTS      (1ull << 3)
#define FMON_PROBE_PROCS       (1ull << 4)
#define FMON_PROBE_ZFS_ARC     (1ull << 5)
#define FMON_PROBE_BATTERY     (1ull << 6)

    /* ------------------------------------------------------------------
     * CPU / 内存 / 交换区
     * ------------------------------------------------------------------ */
    double   cpu_usage_pct;
    double   cpu_temp_c;
    double   load_avg[3];
    double   cpu_core_pct[FMON_MAX_CPUS];

    uint64_t mem_total_bytes;
    uint64_t mem_free_bytes;
    uint64_t mem_avail_bytes;        /* free + inactive + cache（真正可用） */
    double   mem_used_pct;

    uint64_t swap_total_kb;
    uint64_t swap_used_kb;
    double   swap_used_pct;

    /* ------------------------------------------------------------------
     * 硬件与电源
     * ------------------------------------------------------------------ */
    double   fan_rpm;
    double   battery_pct;           /* <0 表示无电池 */
    double   battery_discharge_w;
    uint32_t battery_present;
    uint32_t battery_flags;         /* FMON_BATT_* */
    uint32_t acline;                /* 1 = 已接通外部电源 */

    /* ------------------------------------------------------------------
     * 网络 / 磁盘
     * ------------------------------------------------------------------ */
    double   net_rx_kbps;
    double   net_tx_kbps;
    double   disk_read_kbps;
    double   disk_write_kbps;
    double   disk_busy_pct;
    uint64_t disk_read_ops;         /* 累计 */
    uint64_t disk_write_ops;        /* 累计 */

    uint32_t iface_count;
    FmonIfaceInfo ifaces[FMON_MAX_IFACES];

    /* 网卡硬件型号（pciconf 探测，最多 4 条） */
    uint32_t     nic_count;
    char         nic_models[4][192];

    uint32_t     disk_count;
    FmonDiskInfo disks[FMON_MAX_DISKS];

    uint32_t     mem_slot_total;
    uint32_t     mem_module_count;
    FmonMemModule mem_modules[FMON_MAX_MEM_MODULES];

    uint32_t mount_count;
    FmonMountInfo mounts[FMON_MAX_MOUNTS];

    /* ------------------------------------------------------------------
     * GPU
     * ------------------------------------------------------------------ */
    int32_t  gpu_count;
    FmonGpuInfo gpus[FMON_MAX_GPUS];

    /* ------------------------------------------------------------------
     * ZFS
     * ------------------------------------------------------------------ */
    uint64_t zfs_arc_size;
    uint64_t zfs_arc_c_max;
    uint64_t zfs_arc_mru_size;
    uint64_t zfs_arc_mfu_size;
    uint64_t zfs_arc_hits;
    uint64_t zfs_arc_misses;
    uint64_t zfs_l2_hits;
    uint64_t zfs_l2_misses;
    uint64_t zfs_l2_size;           /* L2ARC 缓存大小（0 = 未配置） */
    uint64_t zfs_l2_asize;          /* L2ARC 实际分配（含压缩） */
    uint64_t zfs_mru_ghost_hits;
    uint64_t zfs_mfu_ghost_hits;
    uint64_t zfs_arc_prune;
    uint64_t zfs_vdev_cache_hits;
    uint64_t zfs_vdev_cache_misses;
    double   zfs_arc_pct;           /* ARC 命中率 */

    /* 多 zpool：按 zpool list 的顺序排列，pool_count 为有效个数 */
    uint32_t     pool_count;
    FmonPoolInfo pools[FMON_MAX_POOLS];

    /* ------------------------------------------------------------------
     * 历史曲线（环形缓冲，1Hz）
     *
     * 约定：NaN = 该采样点没有有效数据（传感器不存在/读取失败）。
     * 绘图端遇到 NaN 必须断开折线，不能补 0；CSV 导出留空。
     * 这样 “0%” 和 “没有传感器” 不会被混为一谈。
     * ------------------------------------------------------------------ */
    double   cpu_history[FMON_HISTORY_POINTS];
    double   mem_history[FMON_HISTORY_POINTS];
    double   swap_history[FMON_HISTORY_POINTS];
    double   net_rx_history[FMON_HISTORY_POINTS];
    double   net_tx_history[FMON_HISTORY_POINTS];
    double   disk_read_history[FMON_HISTORY_POINTS];
    double   disk_write_history[FMON_HISTORY_POINTS];
    double   temp_history[FMON_HISTORY_POINTS];
    double   battery_history[FMON_HISTORY_POINTS];
    double   zpool_util_history[FMON_HISTORY_POINTS];  /* 第一个池的使用率 */
    double   zfs_arc_history[FMON_HISTORY_POINTS];
    double   zfs_hit_history[FMON_HISTORY_POINTS];
    double   zfs_io_read_history[FMON_HISTORY_POINTS];
    double   zfs_io_write_history[FMON_HISTORY_POINTS];
    double   zfs_arc_prune_history[FMON_HISTORY_POINTS];
    double   zfs_l2_size_history[FMON_HISTORY_POINTS];  /* L2ARC 大小（GB） */
    double   zfs_l2_hit_history[FMON_HISTORY_POINTS];   /* L2ARC 命中率（%） */
    double   gpu_history[FMON_MAX_GPUS][FMON_HISTORY_POINTS];

    /* 每个历史采样点的 CLOCK_MONOTONIC 时间戳（微秒），与上面的
     * history_* 数组使用同一个环形成员索引。0 表示该槽位没有时间戳
     * （旧 ABI 或尚未写入），绘图端退回按 history_interval_us 等间距。 */
    uint64_t history_time_us[FMON_HISTORY_POINTS];

    uint32_t history_index;         /* 下一个写入位置 */
    uint32_t history_count;
    uint32_t _pad_hist;             /* 对齐保留 */
    uint64_t history_interval_us;   /* 标称/相邻采样间隔（微秒），无时间戳时回退 */

    /* ------------------------------------------------------------------
     * 进程快照（按 CPU 降序，最多 FMON_MAX_PROCS 条）
     * ------------------------------------------------------------------ */
    uint32_t proc_count;
    FmonProcInfo procs[FMON_MAX_PROCS];
} FmonShmData;

/* ---------------------------------------------------------------------------
 * 运行时一致性检查
 * ------------------------------------------------------------------------- */
_Static_assert(sizeof(FmonShmData) < (1u << 20),
               "FmonShmData 过大，共享内存不应超过 1 MiB");
_Static_assert(offsetof(FmonShmData, magic) == 0, "magic 必须位于结构体开头");
_Static_assert(sizeof(((FmonShmData *)0)->seq) == sizeof(uint32_t),
               "seq 必须是 32 位，seqlock 依赖这一点");
_Static_assert(offsetof(FmonShmData, seq) % 4 == 0,
               "seq 需 4 字节对齐，跨进程原子访问才安全");
_Static_assert(offsetof(FmonShmData, history_time_us) % 8 == 0,
               "history_time_us 需 8 字节对齐");

/* ---------------------------------------------------------------------------
 * 读写 API
 *
 * 守护进程（唯一写者）：
 *     FmonShmData *shm = fmon_shm_create(&fd, false, err, sizeof err);
 *     fmon_shm_writer_init(shm);
 *     fmon_shm_wr_begin();  ...写字段...  fmon_shm_wr_end();
 *
 * 图形界面（只读，任意多个读者）：
 *     FmonShmData *live = fmon_shm_attach(false, err, sizeof err);
 *     if (fmon_shm_snapshot(live, &local_copy) == 0) { 使用 local_copy }
 * ------------------------------------------------------------------------- */

/* 打开（并可创建）共享内存，同时完成大小 / magic / ABI 校验。
 * writable=false 时以只读方式映射，供界面使用。
 * 失败返回 NULL，并把人类可读的原因写入 err。 */
FmonShmData *fmon_shm_create(int *out_fd, bool force, char *err,
                             size_t err_len);
FmonShmData *fmon_shm_attach(bool writable, char *err, size_t err_len);

/* 仅供单元测试：用自定义名字创建/附加，跳过“已有实例”检查。 */
FmonShmData *fmon_shm_create_for_test(const char *name, int *out_fd, char *err,
                                      size_t err_len);
FmonShmData *fmon_shm_attach_for_test(const char *name, bool writable,
                                      char *err, size_t err_len);

/* 解除映射。传 NULL 安全。 */
void fmon_shm_detach(FmonShmData *shm);

/* 守护进程侧：注册写者、初始化写锁与头部字段。 */
void fmon_shm_writer_init(FmonShmData *shm);

/* 写事务：二者必须成对调用，期间由进程内互斥锁串行化。 */
void fmon_shm_wr_begin(void);
void fmon_shm_wr_end(void);

/* 原子地拷贝一份一致快照。成功返回 0；重试多次仍失败返回 -1。 */
int fmon_shm_snapshot(const FmonShmData *src, FmonShmData *dst);

/* 判断数据是否新鲜：状态为 RUNNING 且 last_update_us 距 now_us 不超过 max_age_us。 */
static inline bool fmon_shm_is_online(const FmonShmData *shm, uint64_t now_us,
                                      uint64_t max_age_us)
{
    if (shm == NULL)
        return false;
    if (shm->magic != FMON_SHM_MAGIC || shm->abi_version != FMON_ABI_VERSION)
        return false;
    if (shm->status != (int32_t)FMON_STATUS_RUNNING)
        return false;
    if (shm->last_update_us == 0 || now_us < shm->last_update_us)
        return false;
    return (now_us - shm->last_update_us) <= max_age_us;
}

#ifdef __cplusplus
}
#endif

#endif /* FMON_SHM_H */
