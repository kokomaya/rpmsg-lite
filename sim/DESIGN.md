# RPMsg-Lite Web Simulator Design Document

## 概述

一个基于 Web 的 RPMsg-Lite 协议仿真器，允许用户通过图形界面交互式地观察和操控 RPMsg 协议的完整生命周期。支持慢放模式，可以逐步观察每个操作对共享内存、VirtQueue 和消息缓冲区的影响。

## 架构

```
┌─────────────────────────────────────────────────────┐
│                   Web Browser (GUI)                   │
│  ┌────────────┐  ┌────────────┐  ┌───────────────┐  │
│  │ Master面板 │  │ 共享内存视图│  │  Remote面板   │  │
│  │ - Endpoints│  │ - VRing可视 │  │ - Endpoints   │  │
│  │ - 发送控制 │  │ - Buffer状态│  │ - 接收控制    │  │
│  │ - 状态显示 │  │ - 描述符链  │  │ - 状态显示    │  │
│  └────────────┘  └────────────┘  └───────────────┘  │
│  ┌─────────────────────────────────────────────────┐ │
│  │  Timeline / 事件日志 / 慢放控制                  │ │
│  └─────────────────────────────────────────────────┘ │
└───────────────────────┬─────────────────────────────┘
                        │ WebSocket (JSON)
┌───────────────────────┴─────────────────────────────┐
│              C Simulation Backend                     │
│  ┌──────────────────────────────────────────────┐   │
│  │  原版 rpmsg_lite.c + virtqueue.c (未修改)     │   │
│  └──────────────────────────────────────────────┘   │
│  ┌──────────────────────────────────────────────┐   │
│  │  sim_env.c (仿真环境层 - 替代硬件平台)        │   │
│  │  - 共享内存 = malloc'd buffer                 │   │
│  │  - platform_notify = 事件队列                 │   │
│  │  - 慢放控制: step模式 / 延迟模式              │   │
│  └──────────────────────────────────────────────┘   │
│  ┌──────────────────────────────────────────────┐   │
│  │  sim_bridge.c (WebSocket服务器 + JSON序列化)   │   │
│  │  - 状态快照导出                               │   │
│  │  - 命令接收和执行                             │   │
│  │  - 事件通知推送                               │   │
│  └──────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────┘
```

## 核心设计原则

1. **协议零修改**: 使用原版 rpmsg_lite.c, rpmsg_ns.c, rpmsg_queue.c, virtqueue.c
2. **环境层替换**: 仅替换 `rpmsg_env.h` 的实现(env_* 函数)和 platform 层
3. **双核仿真**: 在单进程中模拟 Master 和 Remote 两个核心，通过共享内存通信
4. **可观察性**: 每个操作都产生事件，推送到 Web 前端
5. **可控性**: 支持暂停、单步、慢放、手动注入数据

## 仿真后端 (C + WebSocket)

### 文件结构
```
sim/
├── DESIGN.md              # 本文档
├── CMakeLists.txt         # 构建系统
├── backend/
│   ├── main.c             # 入口, WebSocket服务器
│   ├── sim_env.c          # 环境层实现 (替代 RTOS/裸机)
│   ├── sim_env.h          # 环境层头文件
│   ├── sim_platform.c     # 平台层实现 (IPC 仿真)
│   ├── sim_platform.h     # 平台层头文件
│   ├── sim_core.c         # 仿真核心逻辑 (Master+Remote实例管理)
│   ├── sim_core.h         # 仿真核心头文件
│   ├── sim_events.c       # 事件记录和推送
│   ├── sim_events.h       # 事件系统头文件
│   ├── rpmsg_config.h     # 仿真用配置
│   └── rpmsg_platform.h   # 平台配置头文件
├── web/
│   ├── index.html         # 主页面
│   ├── style.css          # 样式
│   ├── app.js             # 主应用逻辑
│   ├── ws-client.js       # WebSocket客户端
│   ├── components/
│   │   ├── master-panel.js    # Master核心面板
│   │   ├── remote-panel.js    # Remote核心面板
│   │   ├── shmem-view.js     # 共享内存可视化
│   │   ├── vring-view.js     # VirtQueue环形可视化
│   │   ├── timeline.js       # 事件时间线
│   │   └── controls.js       # 慢放/暂停/步进控制
│   └── lib/
│       └── reconnecting-websocket.js
└── third_party/
    └── mongoose/          # 轻量HTTP+WebSocket C库
        ├── mongoose.c
        └── mongoose.h
```

### 仿真环境层 (sim_env.c)

替代硬件相关的 env_* 函数:

| 原始函数 | 仿真实现 |
|---------|---------|
| `env_init()` | 初始化仿真内存和事件队列 |
| `env_deinit()` | 清理 |
| `env_allocate_memory()` | `malloc()` |
| `env_free_memory()` | `free()` |
| `env_memset/memcpy` | 标准库 |
| `env_lock_mutex()` | pthread_mutex 或 Windows CriticalSection |
| `env_unlock_mutex()` | 对应解锁 |
| `env_sleep_msec()` | Sleep/usleep + 慢放倍率 |
| `env_mb/wmb/rmb()` | 编译器barrier (仿真中为空) |
| `env_cache_flush/invalidate` | 空操作 + 事件记录 |
| `platform_notify()` | 触发对端回调 + 事件推送 |
| `platform_init()` | 设置仿真共享内存 |

### 事件系统

每个有意义的操作都生成事件 (JSON 格式):

```json
{
  "seq": 42,
  "timestamp_us": 123456,
  "type": "vring_add_buffer",
  "core": "master",
  "direction": "tx",
  "details": {
    "desc_idx": 3,
    "avail_idx": 5,
    "buffer_addr": "0x1000",
    "buffer_len": 512
  }
}
```

事件类型:
- `init_master` / `init_remote` - 初始化
- `deinit` - 反初始化
- `link_up` / `link_down` - 链路状态变化
- `ept_create` / `ept_destroy` - 端点创建/销毁
- `tx_alloc` - 分配发送缓冲区
- `tx_send` - 发送消息 (含header详情)
- `rx_callback` - 接收回调触发
- `rx_release` / `rx_hold` - 接收缓冲区释放/保持
- `vring_add_buffer` - 描述符加入可用环
- `vring_get_buffer` - 从已用环取缓冲区
- `vring_kick` - 通知对端
- `ns_announce` - 名字服务公告
- `ns_response` - 名字服务响应

### 慢放模式

三种运行模式:
1. **正常模式**: 全速运行，事件按批次推送
2. **慢放模式**: 每个操作之间插入可配置延迟(100ms-2000ms)
3. **单步模式**: 暂停在每个操作，等待用户点击"下一步"

## Web 前端

### 主界面布局

```
┌─────────────────────────────────────────────────────────┐
│  [▶ Play] [⏸ Pause] [⏭ Step] [🐌 Speed: ___ms]  │ 控制栏
├──────────┬───────────────────────────┬──────────────────┤
│ MASTER   │    Shared Memory View     │      REMOTE      │
│          │                           │                  │
│ State:   │  ┌─VRing0 (M→R)────────┐ │  State:          │
│  [INIT]  │  │ Desc[0] ■ Desc[1] □ │ │   [WAIT_LINK]   │
│          │  │ Avail: [0,1,_,_]     │ │                  │
│ Endpts:  │  │ Used:  [_,_,_,_]     │ │  Endpts:         │
│  ┌─ 30 ─┐│  └─────────────────────┘ │   ┌─ 40 ─┐      │
│  │ ns   ││                           │   │ echo │      │
│  └──────┘│  ┌─VRing1 (R→M)────────┐ │   └──────┘      │
│          │  │ Desc[0] □ Desc[1] ■ │ │                  │
│ [Send]   │  │ Avail: [_,_,_,_]     │ │  [Send]         │
│ dst:___  │  │ Used:  [0,_,_,_]     │ │  dst:___        │
│ data:___ │  └─────────────────────┘ │  data:___        │
│          │                           │                  │
│          │  ┌─ Buffers ───────────┐  │                  │
│          │  │ [0] HDR|PAYLOAD     │  │                  │
│          │  │ [1] HDR|PAYLOAD     │  │                  │
│          │  │ [2] (free)          │  │                  │
│          │  │ [3] (free)          │  │                  │
│          │  └─────────────────────┘  │                  │
├──────────┴───────────────────────────┴──────────────────┤
│  Timeline / Event Log                                    │
│  [42] master tx_send → dst:40 len:12 "Hello!"           │
│  [43] vring0 add_buffer idx:2 avail_idx:3               │
│  [44] vring0 kick → remote                              │
│  [45] remote rx_callback dst:40 src:30                   │
└─────────────────────────────────────────────────────────┘
```

### WebSocket 协议

**Client → Server (命令)**:
```json
{"cmd": "init_master", "shmem_size": 8192}
{"cmd": "init_remote"}
{"cmd": "create_ept", "core": "master", "addr": 30}
{"cmd": "create_ept", "core": "remote", "addr": 40}
{"cmd": "send", "core": "master", "src": 30, "dst": 40, "data": "SGVsbG8="}
{"cmd": "send", "core": "remote", "src": 40, "dst": 30, "data": "V29ybGQ="}
{"cmd": "set_mode", "mode": "slow", "delay_ms": 500}
{"cmd": "set_mode", "mode": "step"}
{"cmd": "step"}
{"cmd": "set_mode", "mode": "normal"}
{"cmd": "deinit", "core": "master"}
{"cmd": "reset"}
{"cmd": "get_state"}
```

**Server → Client (事件/状态)**:
```json
{"type": "event", "event": {...}}
{"type": "state", "state": {...全量状态快照...}}
{"type": "waiting_step"}
{"type": "error", "msg": "..."}
```

### 全量状态快照结构

```json
{
  "master": {
    "initialized": true,
    "link_state": 1,
    "endpoints": [{"addr": 30, "name": "ns_client"}],
    "tvq": {
      "free_cnt": 2,
      "desc_head_idx": 2,
      "avail_idx": 2,
      "used_cons_idx": 0
    },
    "rvq": { ... }
  },
  "remote": {
    "initialized": true,
    "link_state": 1,
    "endpoints": [{"addr": 40, "name": "echo_svc"}],
    "tvq": { ... },
    "rvq": { ... }
  },
  "shmem": {
    "base": "0x0",
    "size": 8192,
    "vring0": {
      "desc": [
        {"addr": "0x400", "len": 512, "flags": 0, "next": 1},
        ...
      ],
      "avail": {"flags": 0, "idx": 2, "ring": [0, 1, 0, 0]},
      "used": {"flags": 0, "idx": 0, "ring": []}
    },
    "vring1": { ... },
    "buffers": [
      {"index": 0, "state": "used_by_master_tx", "hdr": {"src":30,"dst":40,"len":5,"flags":0}, "payload_hex": "48656c6c6f"},
      {"index": 1, "state": "free", "hdr": null, "payload_hex": ""},
      ...
    ]
  },
  "mode": "slow",
  "delay_ms": 500
}
```

## 构建和运行

```bash
cd sim
mkdir build && cd build
cmake ..
make
./rpmsg_sim    # 启动后端，监听 http://localhost:8080
```

浏览器打开 `http://localhost:8080` 即可使用。

## 依赖

- **Mongoose** (MIT): 嵌入式 HTTP/WebSocket 服务器 (单文件 C 库)
- 原版 rpmsg-lite 源码 (lib/ 目录)
- C11 编译器 (GCC/Clang/MSVC)
- CMake 3.14+

## 游戏化交互

用户可以:
1. 手动初始化 Master/Remote（观察共享内存如何被划分）
2. 创建端点（观察端点链表变化）
3. 从任一侧发送消息（观察 buffer 分配 → header 填充 → vring 更新 → kick → 回调）
4. 在慢放模式下逐步观察每个内部操作
5. 编辑待发送的 payload 数据
6. 观察 VirtQueue 环形缓冲区的索引变化
7. 手动释放/保持接收缓冲区
8. 使用名字服务进行服务发现
9. 反初始化并观察清理过程
