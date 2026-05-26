# 标准 remoteproc/rpmsg 与 rpmsg-lite 对比

## 1. 概念定义

### 1.1 标准 remoteproc + rpmsg（Linux 内核实现）

Linux 内核中，多核通信是一个完整的三层协议栈：

```
Linux Kernel
└── remoteproc 框架
      ├── 固件加载（ELF / 二进制）
      ├── resource table 解析（协商共享内存、vring 地址）
      ├── 生命周期管理（boot / stop / crash recovery）
      └── virtio 总线
            └── virtio_rpmsg_bus 驱动
                  ├── rpmsg 设备抽象
                  ├── name service 发现
                  └── endpoint channel 管理
```

每一层职责明确：

| 层级 | 职责 |
|------|------|
| **remoteproc** | 加载固件、解析 resource table、管理远端核生命周期 |
| **resource table** | 远端固件中的静态数据段，声明所需资源（vring 地址/大小、内存 carveout、trace buffer 等） |
| **virtio** | 基于 resource table 协商出来的 virtio 设备，提供标准的 vring 传输机制 |
| **rpmsg** | 运行在 virtio 传输层之上，提供端点（endpoint）、通道（channel）、名称服务（name service）等通信抽象 |

---

### 1.2 rpmsg-lite（本工程实现）

rpmsg-lite 是 NXP 面向嵌入式 MCU/DSP 场景设计的**极简化**实现，仅保留通信核心，剥离了 Linux 内核依赖：

```
rpmsg-lite
├── 平台移植层（porting/）     ← 替代 remoteproc：由用户/BSP 完成固件加载和地址配置
├── 环境抽象层（rpmsg_env.h）  ← 适配 RTOS / bare-metal OS 原语
├── virtqueue（virtqueue.c）   ← 保留 vring 数据结构，静态初始化，无 virtio 协商
└── rpmsg_lite（rpmsg_lite.c） ← endpoint / send / recv / name service
```

---

## 2. 协议层对比

| 特性 | 标准 remoteproc + rpmsg | rpmsg-lite |
|------|------------------------|------------|
| **固件加载** | remoteproc 框架负责，支持 ELF 解析 | ❌ 不包含，由 BSP/调试器完成 |
| **resource table** | ✅ 固件内嵌，动态解析 vring 地址 | ❌ 不包含，地址硬编码在 `rpmsg_config.h` / platform 层 |
| **virtio 设备协商** | ✅ 完整的 virtio 特性协商流程 | ❌ 跳过协商，直接静态初始化 vring |
| **vring 数据结构** | ✅ 标准 `vring_desc` / `vring_avail` / `vring_used` | ✅ 完全兼容（复用同一套结构体） |
| **rpmsg endpoint** | ✅ 动态注册，内核 rpmsg_create_ept | ✅ `rpmsg_lite_create_ept()` |
| **name service** | ✅ 内核 rpmsg_ns 自动广播 | ✅ 可选，`rpmsg_ns.c` |
| **零拷贝 API** | ❌ 标准 API 不直接支持 | ✅ `rpmsg_lite_alloc_tx_buffer()` |
| **多实例** | 受内核 virtio bus 约束 | ✅ `RL_ALLOW_CUSTOM_SHMEM_CONFIG` 支持多实例不同布局 |
| **生命周期管理** | ✅ remoteproc boot/stop/crash recovery | ❌ 不包含 |
| **运行环境** | Linux 内核驱动 | RTOS / bare-metal，可移植 |

---

## 3. resource table 详解

### 3.1 标准实现

resource table 是远端固件 ELF 中名为 `.resource_table` 的特殊数据段，结构如下：

```c
/* Linux 内核 include/linux/remoteproc.h (简化) */
struct resource_table {
    uint32_t ver;        /* 版本号，固定为 1 */
    uint32_t num;        /* 资源条目数量 */
    uint32_t reserved[2];
    uint32_t offset[];   /* 各条目相对于 resource_table 首地址的偏移 */
    /* 后续紧跟各 resource entry */
};

/* vring 资源条目 */
struct fw_rsc_vdev_vring {
    uint32_t da;    /* 设备地址（Linux 映射后写入，远端读取） */
    uint32_t align; /* vring 对齐要求 */
    uint32_t num;   /* 描述符数量 */
    uint32_t notifyid;
    uint32_t pa;
};
```

**协商流程：**

```
远端固件 ELF
    │  .resource_table 段
    │  ├── RSC_VDEV（virtio device）
    │  │     └── vring[0].da = 0   ← 远端填 0，请求 Linux 分配
    │  │     └── vring[1].da = 0
    │  └── RSC_CARVEOUT（内存映射请求）
    ▼
remoteproc 加载固件
    │  解析 resource table
    │  为 vring 分配/映射物理内存
    │  将地址回填到 vring[x].da
    ▼
远端核启动
    │  从 .resource_table 读取已填好的 vring 地址
    │  初始化 vring
    ▼
通信开始
```

### 3.2 rpmsg-lite 实现

**没有 resource table**。vring 地址由开发者在 `rpmsg_config.h` 或 platform 层中**静态指定**：

```c
/* rpmsg_config.h 中配置缓冲区大小和数量 */
#define RL_BUFFER_PAYLOAD_SIZE  (496U)   /* payload 大小（不含 16 字节 header） */
#define RL_BUFFER_COUNT         (2U)     /* 单方向 buffer 数量（必须是 2 的幂） */

/* platform 层（由用户实现）提供物理地址 */
/* 例：platform_get_base_addr() 返回共享内存起始地址 */
```

初始化时直接传入地址：

```c
/* Master 侧 */
rpmsg_lite_master_init(
    (void *)SHMEM_BASE_ADDR,   /* 共享内存起始地址，由链接脚本/BSP固定 */
    SHMEM_SIZE,
    VRING_LINK_ID,
    RL_NO_FLAGS,
    &rpmsg_instance
);
```

---

## 4. 共享内存布局对比

### 4.1 标准 remoteproc 内存布局

Linux 通过 CMA（连续内存分配器）或 reserved-memory 节点分配，地址在运行时确定：

```
物理内存（由 dts reserved-memory 或 CMA 分配）
┌─────────────────────────────────────┐
│  vring0（Master→Remote TX）          │
│  ├── Descriptor Table (N × 16 B)    │
│  ├── Available Ring (6 + N×2 B)     │
│  └── Used Ring (6 + N×8 B)          │
├─────────────────────────────────────┤
│  vring1（Remote→Master TX）          │
│  ├── Descriptor Table               │
│  ├── Available Ring                 │
│  └── Used Ring                      │
├─────────────────────────────────────┤
│  Buffer Pool（N×512 B，按 vring 引用）│
└─────────────────────────────────────┘
```

地址来源：`resource_table → remoteproc 回填 → 远端读取`

### 4.2 rpmsg-lite 内存布局

地址静态确定，布局由初始化代码计算，Master 负责初始化整块共享内存：

```
SHMEM_BASE_ADDR（由链接脚本/宏定义固定）
┌────────────────────────────────────────────────────┐  ← SHMEM_BASE_ADDR
│  vring0（Master TX → Remote RX）                    │
│  ├── Descriptor Table  [RL_BUFFER_COUNT × 16 B]    │
│  ├── Available Ring                                 │
│  └── Used Ring                                      │
├────────────────────────────────────────────────────┤  ← 按 vring_align 对齐
│  vring1（Remote TX → Master RX）                    │
│  ├── Descriptor Table  [RL_BUFFER_COUNT × 16 B]    │
│  ├── Available Ring                                 │
│  └── Used Ring                                      │
├────────────────────────────────────────────────────┤
│  Buffer Pool                                        │
│  ├── buf[0]  [RL_BUFFER_PAYLOAD_SIZE + 16 B]       │  ← 每块 = header(16B) + payload
│  ├── buf[1]  [RL_BUFFER_PAYLOAD_SIZE + 16 B]       │
│  ├── ...（vring0 对应的 RL_BUFFER_COUNT 块）         │
│  ├── buf[N]  [RL_BUFFER_PAYLOAD_SIZE + 16 B]       │
│  ├── ...（vring1 对应的 RL_BUFFER_COUNT 块）         │
│  └── buf[2N-1]                                      │
└────────────────────────────────────────────────────┘

总大小估算（默认配置）：
  每块 buffer = 496 + 16 = 512 B
  buffer 总数 = RL_BUFFER_COUNT × 2 = 4 块 = 2048 B
  vring 结构  ≈ 少量（取决于对齐，通常 < 1 KB）
  合计        ≈ ~3 KB（默认最小配置）
```

关键代码（`rpmsg_lite.c`）：

```c
/* Master 初始化时计算并写入 buffer 地址到 Descriptor Table */
for (i = 0; i < (int32_t)num_desc; i++) {
    vq_ring_add_buffer(vq, buff, RL_BUFFER_COUNT);
    buff += (RL_BUFFER_PAYLOAD_SIZE + 16U);  /* 每块 512 B（默认） */
}
```

---

## 5. 消息结构对比

两者 rpmsg 消息头**完全一致**，均为 16 字节：

```c
/* 标准 Linux kernel include/uapi/linux/rpmsg.h */
/* rpmsg-lite lib/include/rpmsg_lite.h          */
/* —— 结构体定义完全相同 —— */
struct rpmsg_std_hdr {
    uint32_t src;       /* 源端点地址 */
    uint32_t dst;       /* 目的端点地址 */
    uint32_t reserved;  /* 保留（rpmsg-lite 用于存 vring idx） */
    uint16_t len;       /* payload 长度 */
    uint16_t flags;     /* 标志位 */
};
```

这保证了 rpmsg-lite 与 Linux rpmsg 内核驱动在**消息格式层面完全互通**。

---

## 6. 适用性对比

| 场景 | 标准 remoteproc + rpmsg | rpmsg-lite |
|------|------------------------|------------|
| **Linux 主核 + MCU 从核** | ✅ 最佳选择，Linux 侧天然支持 | ✅ MCU 侧使用 rpmsg-lite，与 Linux 兼容 |
| **MCU + MCU 双核** | ❌ 需要 Linux 内核，不适用 | ✅ 最佳选择，两侧均用 rpmsg-lite |
| **RTOS 环境** | ❌ 内核驱动，不可直接移植 | ✅ FreeRTOS / Zephyr / ThreadX 均有移植层 |
| **Bare-metal 环境** | ❌ 不支持 | ✅ 支持（env 层提供轻量原语） |
| **固件热重载/OTA** | ✅ remoteproc 原生支持 | ❌ 不支持，需自行实现 |
| **多核 crash 恢复** | ✅ remoteproc recovery 机制 | ❌ 不支持 |
| **内存占用** | 大（内核驱动 + virtio 框架） | 极小（< 10 KB Flash，< 3 KB RAM） |
| **移植复杂度** | 低（Linux 驱动现成） | 中（需实现 platform / env 移植层） |
| **标准化程度** | 高（Linux 主线，规范完整） | 中（NXP 主导，消息格式兼容标准） |
| **与 Linux 互通** | ✅ 原生 | ✅ 消息格式兼容，可与 Linux rpmsg 驱动通信 |
| **调试可视化** | 通过 `/sys/kernel/debug/remoteproc/` | 本工程提供 Web 模拟器（`sim/`） |

---

## 7. 与 Linux 对接时的注意事项

当 rpmsg-lite（MCU 侧）需要与 Linux remoteproc/rpmsg（主核侧）通信时：

1. **resource table 必须由 MCU 固件自己提供**：Linux remoteproc 会在启动 MCU 前解析固件中的 `.resource_table` 段，获取 vring 地址。MCU 固件需要在链接脚本中声明该段，并填写正确的 vring 配置。rpmsg-lite 本身不生成 resource table，需用户自行定义。

2. **vring 数量和参数需对齐**：Linux 侧默认每个 virtio 设备有 2 个 vring（TX/RX 各一个），`num` 通常为 16 或 32，`align` 为 4096。rpmsg-lite 的 `RL_BUFFER_COUNT` 需与 Linux 侧一致。

3. **notify id / 中断需对应**：Linux 通过 mailbox 或 SGI 中断 kick 远端核，platform 层需正确实现 `platform_notify()` 对接中断控制器。

4. **地址空间映射**：Linux 侧看到的是 iova（I/O 虚拟地址），远端核看到的是物理地址或本地地址，需通过 `da_to_va` 正确转换。

---

## 8. 总结

```
标准 remoteproc 栈                    rpmsg-lite
─────────────────────────────────     ──────────────────────────────
remoteproc（固件加载 + 生命周期）      ❌ 由 BSP/调试器替代
  └── resource table（动态协商）       ❌ 替换为静态配置（rpmsg_config.h）
        └── virtio 总线（特性协商）     ❌ 跳过，直接静态初始化 vring
              └── vring（环形队列）     ✅ 完全保留，结构兼容
                    └── rpmsg 消息     ✅ 完全保留，头格式兼容
                          └── endpoint ✅ 完全保留，API 略有不同
```

**rpmsg-lite 的本质**：保留 vring + rpmsg 消息层的完全兼容性，剥除需要 OS 支持的上层协商机制，以静态配置换取极低的资源占用和可移植性。
