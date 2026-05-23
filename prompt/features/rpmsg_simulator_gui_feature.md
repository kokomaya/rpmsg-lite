# Feature: RPMsg-Lite PC Simulator with GUI Visualization

## Overview

Extract rpmsg-lite core protocol stack from hardware-specific platform, build a PC-runnable 
dual-thread simulator, and provide real-time GUI visualization of all internal states.

---

## Feature Breakdown

### F1: Core Code Extraction

**Goal**: Isolate pure RPMsg protocol logic from hardware dependencies.

**Files to extract** (unchanged logic):
- `rpmsg_lite.c` — core lifecycle + messaging
- `rpmsg_ns.c` — name service
- `rpmsg_queue.c` — blocking queue
- `virtqueue.c` — virtio ring operations
- `llist.c` — linked list utility

**Modifications needed in extracted code**:
1. Add `#include "rpmsg_trace.h"` at top
2. Insert `RPMSG_TRACE()` macro calls at state transitions (non-functional change)
3. Ensure `#include "rpmsg_platform.h"` resolves to simulation header

**Hardware code to disable**:
```c
// In CMakeLists.txt, simply don't compile:
// - lib/rpmsg_lite/porting/environment/rpmsg_env_*.c  (all)
// - lib/rpmsg_lite/porting/platform/*                 (all)
// Instead compile: sim/rpmsg_env_sim.c, sim/rpmsg_platform_sim.c
```

---

### F2: Environment Simulation Layer

**File**: `sim/rpmsg_env_sim.c`

**Functions to implement**:

| Function | PC Implementation |
|----------|-------------------|
| `env_init()` | Initialize mutex, alloc ISR table |
| `env_deinit()` | Cleanup |
| `env_allocate_memory(size, mem)` | `*mem = malloc(size)` |
| `env_free_memory(ptr)` | `free(ptr)` |
| `env_memset(ptr, val, n)` | `memset()` |
| `env_memcpy(dst, src, n)` | `memcpy()` |
| `env_strcmp(a, b)` | `strcmp()` |
| `env_strncpy(dst, src, n)` | `strncpy()` |
| `env_create_mutex(lock)` | `CreateMutex()` / `pthread_mutex_init()` |
| `env_delete_mutex(lock)` | `CloseHandle()` / `pthread_mutex_destroy()` |
| `env_lock_mutex(lock)` | `WaitForSingleObject()` / `pthread_mutex_lock()` |
| `env_unlock_mutex(lock)` | `ReleaseMutex()` / `pthread_mutex_unlock()` |
| `env_sleep_msec(msec)` | `Sleep(msec)` / `usleep(msec*1000)` |
| `env_register_isr(vector, data)` | Store callback in table |
| `env_enable_interrupt(vector)` | Set enable flag |
| `env_disable_interrupt(vector)` | Clear enable flag |
| `env_map_vatopa(addr)` | `return (uintptr_t)addr` |
| `env_map_patova(addr)` | `return (void*)addr` |
| `env_create_queue(queue, len)` | Thread-safe queue impl |
| `env_delete_queue(queue)` | Free queue |
| `env_put_queue(queue, msg, timeout)` | Enqueue + signal |
| `env_get_queue(queue, msg, timeout)` | Dequeue + wait |

---

### F3: Platform Simulation

**File**: `sim/rpmsg_platform_sim.c`

**Key mechanism**: 两个线程共享一块 malloc 的内存，通过 condition_variable 模拟中断通知。

```c
// 核心数据结构
typedef struct {
    pthread_mutex_t     mutex;
    pthread_cond_t      cond;
    volatile uint32_t   pending_vectors;  // bitmask of pending interrupts
    isr_entry_t         isr_table[16];    // registered ISR callbacks
} platform_sim_context_t;

// platform_notify() 实现逻辑：
// 1. 设置对端的 pending_vectors bit
// 2. signal 对端的 condition variable
// 3. 发送 RPMSG_EVT_VRING_KICK 到 instrumentation
```

---

### F4: IPC Simulation (Dual-Thread)

**File**: `sim/ipc_sim.c`

**Architecture**:
- `sim_master_thread()`: 调用 `rpmsg_lite_master_init()`, 创建 endpoint, 发送消息
- `sim_remote_thread()`: 调用 `rpmsg_lite_remote_init()`, 等待 link up, 创建 endpoint, 接收消息
- 共享内存: `static uint8_t g_shmem[SHMEM_SIZE]` 全局 buffer
- 中断模拟: 每个线程有一个 "interrupt dispatcher" 循环，等待 cond_var

**Sequence**:
```
1. main() 分配 shared memory
2. 启动 master_thread → master_init(shmem)
3. 启动 remote_thread → remote_init(shmem)
4. remote: wait_for_link_up()
5. master: create_ept(50), remote: create_ept(51)
6. master: send(ept, 51, "hello", 5)
7. remote: rx_callback triggered → respond
8. Loop or user-driven interaction
```

---

### F5: Instrumentation / Trace System

**Files**: `instrumentation/rpmsg_trace.h`, `rpmsg_trace.c`

**Design**: Lock-free SPSC ring buffer

```c
#define RPMSG_TRACE_RING_SIZE 4096

typedef struct {
    rpmsg_event_t events[RPMSG_TRACE_RING_SIZE];
    volatile uint32_t write_idx;  // only written by producer
    volatile uint32_t read_idx;   // only written by consumer
} rpmsg_trace_ring_t;

// Producer (sim threads):
void rpmsg_trace_emit(const rpmsg_event_t *evt);

// Consumer (GUI thread):
bool rpmsg_trace_poll(rpmsg_event_t *out_evt);
```

---

### F6: GUI Visualization

**Technology**: Dear ImGui + GLFW + OpenGL3

**Panels**:

#### 6.1 State Panel
- Real-time state indicators for Master and Remote
- Color-coded: Uninit=Grey, Init=Blue, Ready=Green, Error=Red
- Endpoint list with addresses
- Buffer usage bar chart

#### 6.2 VRing Visualization
- Grid of buffer slots (colored by state)
- Animated avail/used index pointers
- Click to inspect buffer content
- Shows descriptor chain links

#### 6.3 Message Inspector
- Triggered by clicking a message event or vring buffer
- Shows: src, dst, len, flags, reserved fields
- Hex dump + ASCII decode of payload
- Highlights differences between consecutive messages

#### 6.4 Event Timeline
- Scrollable list with timestamp, type, details
- Color-coded by category
- Filterable by event type
- Supports pause/resume/clear
- Click event → highlights related vring buffer

#### 6.5 Control Panel
- Start/Pause/Step simulation
- Send custom message (input src, dst, payload)
- Create/Destroy endpoint
- Inject fault (link down, buffer full)
- Simulation speed slider

---

## Build System

```cmake
cmake_minimum_required(VERSION 3.16)
project(rpmsg_lite_sim C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Core RPMsg sources
set(RPMSG_CORE_SOURCES
    core/rpmsg_lite.c
    core/rpmsg_ns.c
    core/rpmsg_queue.c
    core/virtqueue.c
    core/llist.c
)

# Simulation layer
set(SIM_SOURCES
    sim/rpmsg_env_sim.c
    sim/rpmsg_platform_sim.c
    sim/shared_memory.c
    sim/ipc_sim.c
)

# Instrumentation
set(TRACE_SOURCES
    instrumentation/rpmsg_trace.c
)

# GUI
set(GUI_SOURCES
    gui/main.cpp
    gui/app.cpp
    gui/panels/state_panel.cpp
    gui/panels/vring_panel.cpp
    gui/panels/message_panel.cpp
    gui/panels/timeline_panel.cpp
    gui/sim_controller.cpp
)

# Include paths (sim/ must come first to override platform headers)
include_directories(
    sim               # rpmsg_platform.h, rpmsg_config.h (override)
    core/include      # core headers
    instrumentation   # trace headers
    third_party/imgui
    third_party/glfw/include
)

add_executable(rpmsg_sim
    ${RPMSG_CORE_SOURCES}
    ${SIM_SOURCES}
    ${TRACE_SOURCES}
    ${GUI_SOURCES}
)

target_compile_definitions(rpmsg_sim PRIVATE
    RPMSG_INSTRUMENTATION_ENABLED=1
)

# Link libraries
if(WIN32)
    target_link_libraries(rpmsg_sim opengl32 glfw)
else()
    target_link_libraries(rpmsg_sim GL glfw pthread)
endif()
```

---

## Acceptance Criteria

1. ✅ `rpmsg_lite_sim` 可执行文件能在 Windows PC 上编译运行
2. ✅ Master init + Remote init → Link UP 完整流程可视化
3. ✅ 发送消息后 GUI 实时显示 header/payload 内容
4. ✅ VRing 描述符状态实时更新
5. ✅ 事件时间线记录所有状态转换
6. ✅ 用户可通过 GUI 交互发送消息
7. ✅ Deinit 流程正确释放资源并在 GUI 中体现
