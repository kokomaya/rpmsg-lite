# RPMsg-Lite Web Simulator 使用指南

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

### 5. 反初始化

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

## 故障排除

| 问题 | 解决方案 |
|------|---------|
| 端口 8088 被占用 | 修改 `main.c` 中的 `s_listen_url` |
| Send 返回 "not ready" | 确保 link 已 UP（需先从一侧发送消息触发） |
| Remote init 失败 | 确保 Master 已先初始化 |
| 浏览器无法连接 | 检查防火墙是否阻止了 8088 端口 |
