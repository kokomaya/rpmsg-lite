# RPMsg-Lite Web Simulator 使用指南

## 术语表

| 术语 | 英文全称 | 说明 |
|------|----------|------|
| **RPMsg** | Remote Processor Messaging | 处理器间通信协议，用于多核 SoC 中核间消息传递 |
| **VirtIO** | Virtual I/O | 一种标准化的虚拟设备 I/O 框架，RPMsg 基于此实现传输层 |
| **VirtQueue** | Virtual Queue | VirtIO 的传输队列，管理描述符和数据缓冲区的生产者-消费者队列 |
| **VRing** | Virtio Ring | VirtQueue 的底层环形缓冲区结构，包含 Descriptor Table、Available Ring 和 Used Ring |
| **Descriptor (描述符)** | Vring Descriptor | 描述一个数据缓冲区的元数据（地址、长度、标志、链接下一个描述符的索引） |
| **Available Ring** | — | 生产者（发送方）写入的环，告知消费者哪些描述符已准备好数据 |
| **Used Ring** | — | 消费者（接收方）写入的环，告知生产者哪些描述符已被处理完毕可以回收 |
| **Shared Memory (共享内存)** | — | Master 和 Remote 共同可见的物理内存区域，存放 VRing 结构和数据缓冲区 |
| **Master** | — | 负责初始化共享内存和 VRing 的核心（通常是主处理器，如 Cortex-A） |
| **Remote** | — | 使用 Master 已初始化的共享内存进行通信的核心（通常是协处理器，如 Cortex-M） |
| **Endpoint (端点)** | — | 通信的逻辑地址，类似于网络中的端口号。每个端点有唯一地址（1–65534） |
| **Channel (通道)** | — | 一对已绑定的端点之间的逻辑连接 |
| **Name Service (NS)** | — | 端点名称公告服务，Remote 通过广播端点名称让 Master 动态发现可用服务 |
| **Link State** | — | 通信链路状态。Master/Remote 初始化完成且 VRing 就绪后链路变为 UP |
| **Payload (有效载荷)** | — | 用户实际发送的数据内容（不含 RPMsg 协议头） |
| **RPMsg Header** | — | 每条消息前 16 字节的协议头，包含 src（源地址）、dst（目标地址）、len（长度）、flags |
| **Buffer** | — | 共享内存中的固定大小数据块，用于承载一条完整的 RPMsg 消息（Header + Payload） |
| **Kick / Notify** | — | 发送方填好数据后通过中断/信号通知对端有新消息可读 |
| **TX (发送)** | Transmit | 本端向对端发送消息的方向 |
| **RX (接收)** | Receive | 从对端收到消息的方向 |

### 数据流简述

```
Master:EPT30                                Remote:EPT40
     │                                          │
     │  1. 从空闲链取 buffer                       │
     │  2. 填写 RPMsg Header + Payload            │
     │  3. 将 desc_idx 写入 Available Ring         │
     │  4. Kick (通知 Remote)  ──────────────────→ │
     │                                          │  5. 从 Available Ring 读取 desc_idx
     │                                          │  6. 读取 buffer 内容 → RX callback
     │                                          │  7. 将 desc_idx 写入 Used Ring
     │  8. 回收 buffer (Used Ring)  ←──────────── │
     │                                          │
```

---

## 快速开始

### 构建

**Windows (PowerShell):**
```powershell
cd sim
$env:PATH = "D:\03_Tools\msys64\mingw64\bin;" + $env:PATH
.\build.bat
```

**Linux / MSYS2 Bash:**
```bash
cd sim
chmod +x build.sh
./build.sh
```

### 运行
```powershell
cd sim/build_sim
./rpmsg_sim.exe
```

启动后在浏览器中打开: **http://localhost:8088**

---

## 界面说明

```
┌─────────────────── 控制栏 ───────────────────────────┐
│ [Reset] [Init Master] [Init Remote]                   │
│ Mode: [Normal/Slow/Step]  Delay: [滑块]  [Step按钮]   │
└──────────────────────────────────────────────────────┘
┌─────────┬──────────────────────────┬─────────────────┐
│ MASTER  │      Shared Memory       │     REMOTE      │
│         │  - VRing 0 (M→R)        │                 │
│ 端点列表│  - VRing 1 (R→M)        │  端点列表       │
│ 发送控制│  - Buffer 可视化         │  发送控制       │
└─────────┴──────────────────────────┴─────────────────┘
┌──────────────── Event Timeline ──────────────────────┐
│ 实时事件日志                                          │
└──────────────────────────────────────────────────────┘
```

---

## 操作流程

### 1. 初始化

按照真实硬件的初始化顺序操作：

1. 点击 **Init Master** — 初始化 Master 侧，分配共享内存，创建 VirtQueue
2. 点击 **Init Remote** — 初始化 Remote 侧，映射已有共享内存

> ⚠️ 必须先初始化 Master，再初始化 Remote（与真实硬件一致）

初始化完成后，可在共享内存视图中看到 VRing 描述符链和可用环的初始状态。

### 2. 创建端点 (Endpoint)

在 Master 或 Remote 面板中：

1. 输入端点地址（1-65534 之间的整数）
2. 点击 **+ Create**

示例：
- Master 侧创建端点地址 `30`
- Remote 侧创建端点地址 `40`

### 3. 发送消息

在发送控制区：

1. **From**: 选择本侧的源端点
2. **To**: 输入目标端点地址（对端的端点地址）
3. **Data**: 输入文本消息内容
4. 点击 **Send**

示例：从 Master:30 发送 "Hello" 到 Remote:40

### 4. 观察数据流

发送消息后，可以在界面中观察到：

- **VRing 描述符**: 颜色变化（绿色=空闲，红色=占用）
- **Available Ring**: 索引递增，新条目高亮
- **Used Ring**: 接收方消费后更新
- **Buffer 内容**: 显示 Header (src, dst, len, flags) 和 Payload 的十六进制/ASCII
- **Event Timeline**: 按时间顺序显示每个内部操作

### 5. 接收消息

消息发送后，接收方的 **Received Messages** 面板会自动显示收到的消息：

- 每条接收记录显示：时间戳、源地址 → 目标地址、数据长度
- Payload 内容以 ASCII 形式显示（不可打印字符显示为 `.`）
- 最多保留最近 50 条接收记录

> 完整的 发送→接收 流程：Master:30 发送 "Hello" → Remote:40 的 **Received Messages** 中自动出现该消息

### 6. 反初始化

- 点击 **Reset** 完全重置仿真
- 或通过 WebSocket 发送 `{"cmd":"deinit","core":"master"}` 反初始化单侧

---

## 慢放模式

### Normal (正常模式)
所有操作全速执行，事件立即在 Timeline 中显示。

### Slow Motion (慢放模式)
每个内部操作之间插入延迟：
- 拖动 **Delay 滑块** 设置延迟时间（100ms - 2000ms）
- 可以清楚看到 buffer 分配 → header 填充 → vring 更新 → kick → 回调 的完整过程

### Step (单步模式)
操作在每一步暂停，等待用户点击 **⏭ Step** 按钮继续：
- 适合仔细观察每个 VirtQueue 操作的效果
- Step 按钮脉冲高亮表示正在等待用户输入

---

## 可视化元素说明

### VRing 描述符 (Desc)
```
[0] [1] [2] [3]   ← 每个方块代表一个描述符
 绿   绿   红  绿   ← 绿色=空闲链中, 红色=正在被使用
```
鼠标悬停显示: addr, len, flags, next

### Available Ring
```
idx=2  [0] [1] [_] [_]   ← idx 表示生产者写入位置
        ↑高亮表示已填入
```

### Used Ring
```
idx=1  [0:512] [_] [_] [_]   ← id:len 格式
        ↑高亮表示已消费
```

### Buffer 卡片
```
┌─ Buffer[0] ─── 5B ──┐
│ src:30 → dst:40 flags:0 │
│ Hello                     │
└──────────────────────────┘
```

---

## WebSocket API（高级用法）

可以直接通过 WebSocket 发送 JSON 命令：

```javascript
// 连接
const ws = new WebSocket('ws://localhost:8088/ws');

// 命令示例
ws.send(JSON.stringify({cmd: "init_master"}));
ws.send(JSON.stringify({cmd: "init_remote"}));
ws.send(JSON.stringify({cmd: "create_ept", core: "master", addr: 30}));
ws.send(JSON.stringify({cmd: "create_ept", core: "remote", addr: 40}));
ws.send(JSON.stringify({cmd: "send", core: "master", src: 30, dst: 40, data: btoa("Hello")}));
ws.send(JSON.stringify({cmd: "set_mode", mode: "slow", delay_ms: 500}));
ws.send(JSON.stringify({cmd: "step"}));
ws.send(JSON.stringify({cmd: "get_state"}));
ws.send(JSON.stringify({cmd: "destroy_ept", core: "master", addr: 30}));
ws.send(JSON.stringify({cmd: "deinit", core: "master"}));
ws.send(JSON.stringify({cmd: "reset"}));
```

### 服务器推送事件格式
```json
{"type": "event", "event": {"seq":1, "timestamp_us":1234, "type":"tx_send", "core":"master", ...}}
{"type": "state", "state": {...完整状态快照...}}
{"type": "waiting_step"}
{"type": "ok", "cmd": "init_master"}
{"type": "error", "msg": "..."}
```

---

## 协议对照

| 仿真操作 | 对应原始 API |
|---------|-------------|
| Init Master | `rpmsg_lite_master_init()` |
| Init Remote | `rpmsg_lite_remote_init()` |
| Create Endpoint | `rpmsg_lite_create_ept()` |
| Send Message | `rpmsg_lite_send()` |
| Destroy Endpoint | `rpmsg_lite_destroy_ept()` |
| Deinit | `rpmsg_lite_deinit()` |

所有 VirtQueue 操作（`virtqueue_add_buffer`, `virtqueue_get_buffer`, `virtqueue_kick` 等）在内部自动执行并产生可观察事件。

---

## 配置参数

当前仿真使用的参数（定义在 `sim/backend/rpmsg_config.h`）：

| 参数 | 值 | 说明 |
|------|-----|------|
| `RL_BUFFER_PAYLOAD_SIZE` | 496 | 单个消息最大负载 |
| `RL_BUFFER_COUNT` | 4 | 每个方向的缓冲区数量 |
| `VRING_ALIGN` | 16 | VRing 对齐字节数 |
| 共享内存总大小 | 4608 | 自动计算 |

修改 `rpmsg_config.h` 后重新编译即可改变配置。

---

## 操作录制与回放

支持录制用户的操作序列，方便后续回放复现操作流程或用于演示/教学。

### 录制操作

1. 点击 Event Timeline 区域的 **⏺ Record** 按钮开始录制
2. 正常执行操作（初始化、创建端点、发送消息等）
3. 点击 **⏹ Stop** 结束录制

录制期间，所有用户发起的命令都会被记录（包含相对时间戳）。

### 回放操作

1. 点击 **▶ Replay** 按钮
2. 系统会自动 Reset 仿真，然后按照录制的时间间隔依次重放每个操作
3. Event Timeline 中显示回放进度

> 回放时每个操作间的等待时间上限为 3 秒，避免因长时间空闲等待

### 导出/导入录制

- **💾 Export**: 将录制导出为 JSON 文件，可保存到本地
- **📂 Import**: 从 JSON 文件导入之前保存的录制

导出的 JSON 格式：
```json
{
  "version": 1,
  "timestamp": "2026-05-25T10:00:00.000Z",
  "actions": [
    { "timestamp": 0, "cmd": { "cmd": "init_master" } },
    { "timestamp": 1200, "cmd": { "cmd": "init_remote" } },
    { "timestamp": 2500, "cmd": { "cmd": "create_ept", "core": "master", "addr": 30 } },
    ...
  ]
}
```

---

## 故障排除

| 问题 | 解决方案 |
|------|---------|
| 端口 8088 被占用 | 修改 `main.c` 中的 `s_listen_url` |
| Send 返回 "not ready" | 确保 link 已 UP（需先从一侧发送消息触发） |
| Remote init 失败 | 确保 Master 已先初始化 |
| 浏览器无法连接 | 检查防火墙是否阻止了 8088 端口 |
