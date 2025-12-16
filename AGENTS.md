# AGENTS.md

## Overview

This document summarizes the current investigation state for **WAMR (WebAssembly Micro Runtime) with wasi-threads**, focusing on an issue where child threads stall after restore when running the `threads-echo` sample.

The information here is intended to help future contributors or agents quickly understand:
- the execution environment
- what has already been tested
- where the investigation is heading next

---

## Environment

### Sandbox States Observed

1. **Initial (read-only sandbox)**
   - `sandbox_mode`: read-only
   - `network_access`: restricted
   - `shell`: zsh

2. **Workspace-write sandbox**
   - `sandbox_mode`: workspace-write
   - `network_access`: restricted
   - `shell`: zsh

3. **Current**
   - `sandbox_mode`: danger-full-access
   - `network_access`: enabled
   - `approval_policy`: never
   - `shell`: zsh

---

## Repository & Working Directories

- **Repository root**
  ```
  /home/takuto1127/wamr-2.1.0-with-wasi-threads
  ```

- **Build directory**
  ```
  /home/takuto1127/wamr-2.1.0-with-wasi-threads/product-mini/platforms/linux/build
  ```

- **Sample application**
  ```
  /home/takuto1127/wamr-2.1.0-with-wasi-threads/wasm-sample/threads-echo
  ```

---

## Investigation Notes

- Rebuilt **iwasm** multiple times with additional instrumentation.
- Added logging for:
  - `wait` / `notify` behavior
  - thread start and exit
- Observed behavior:
  - After `--restore`, **child threads print once and then stop**
  - The **main thread continues printing normally**
- The stall consistently reproduces in `threads-echo` after restore.

---

## Instrumentation Added

Debug logs were inserted into the following components:

- **Shared memory wait/notify**
  ```
  core/iwasm/common/wasm_shared_memory.c
  ```
  - Logs wait/notify offsets and behavior

- **WASI threads wrapper**
  ```
  core/iwasm/libraries/lib-wasi-threads/lib_wasi_threads_wrapper.c
  ```
  - Logs thread start entry and exit

- **Thread manager**
  ```
  core/iwasm/libraries/thread-mgr/thread_manager.c
  ```
  - Logs native thread routine return paths

---

## Current Hypothesis

- Child threads likely block **after restore** inside:
  - WASI sleep
  - WASI timer–related syscall paths
- Thread creation and initial execution appear correct.
- The issue likely occurs **after the first successful print** in child threads.

---

## Pending Work

- Identify the exact blocking point of child threads after restore.
- Add detailed logging around:
  - WASI timer syscalls
  - sleep / clock / poll related paths
- Confirm whether restored thread state correctly reinitializes timer-related resources.

---

## Build Instructions

### Build WAMR (product-mini / linux)

```sh
cd /home/takuto1127/wamr-2.1.0-with-wasi-threads/product-mini/platforms/linux/build
make -j8
```

---

## Execution Commands

### Normal Execution

```sh
cd /home/takuto1127/wamr-2.1.0-with-wasi-threads/wasm-sample/threads-echo
~/wamr-2.1.0-with-wasi-threads/product-mini/platforms/linux/build/iwasm \
  --max-threads=32 \
  threads-echo.wasm
```

### Execution with Restore

```sh
cd /home/takuto1127/wamr-2.1.0-with-wasi-threads/wasm-sample/threads-echo
~/wamr-2.1.0-with-wasi-threads/product-mini/platforms/linux/build/iwasm \
  --max-threads=32 \
  --restore \
  threads-echo.wasm
```

---

## Log Capture Example

```sh
~/wamr-2.1.0-with-wasi-threads/product-mini/platforms/linux/build/iwasm \
  --max-threads=32 \
  --restore \
  /home/takuto1127/wamr-2.1.0-with-wasi-threads/wasm-sample/threads-echo/threads-echo.wasm \
  > /home/takuto1127/wamr-2.1.0-with-wasi-threads/product-mini/platforms/linux/build/threads-echo.log 2>&1
```

---

## Status

- **Reproducible**: Yes
- **Root cause identified**: Not yet
- **Next focus**: WASI timer / sleep path after restore

## Implementation for Two-Phase Thread Startup (WIP)

### Goal
Implement a mechanism where, during restore, waiting threads are started first, 
allowed to reach their wait state, and only then are normal threads started.

### Progress
1. ✅ Added function to collect waiting thread exec_envs: `wasm_shared_memory_get_waiting_exec_envs()`
2. ✅ Implemented waiting thread ID dump at checkpoint time to `waiting_threads.img`
3. ✅ Added `waiting_thread_ids` field to `WASMCluster` structure
4. ✅ Implemented `wasm_cluster_load_waiting_thread_ids()` to load waiting thread IDs from file
5. ✅ Implemented `wasm_cluster_is_waiting_thread()` to check if a thread was waiting
6. ✅ Implemented two-phase thread startup control logic during restore:
   - Load waiting thread IDs via `wasm_cluster_load_waiting_thread_ids()`
   - Phase 1: Start only waiting threads
   - Wait for all waiting threads to enter wait state (check via `get_wait_node_count()`)
   - Phase 2: Start normal threads

### Files Modified
- `core/iwasm/common/wasm_shared_memory.c` - Added `wasm_shared_memory_get_waiting_exec_envs()`
- `core/iwasm/common/wasm_shared_memory.h` - Added function declaration
- `core/iwasm/migration/wasm_dump.c` - Added waiting thread ID dump at checkpoint time
- `core/iwasm/libraries/thread-mgr/thread_manager.h` - Added waiting thread fields to WASMCluster
- `core/iwasm/libraries/thread-mgr/thread_manager.c` - Added load/check functions and cleanup for waiting threads
- `core/iwasm/interpreter/wasm_interp_classic.c` - Implemented two-phase thread startup during restore

### Implementation Details

#### Checkpoint Phase
- When checkpoint is triggered, `wasm_shared_memory_get_waiting_exec_envs()` collects all threads currently in wait state
- Thread IDs of waiting threads are dumped to `waiting_threads.img`

#### Restore Phase
1. Load waiting thread IDs from `waiting_threads.img` into cluster structure
2. **Phase 1**: Start only threads that were waiting at checkpoint time
3. Wait for all Phase 1 threads to enter wait state (polling `get_wait_node_count()`)
4. **Phase 2**: Start remaining normal threads
5. This ensures waiting threads reach their wait state before normal threads start executing

---

## Investigation: Bank Account Simulator Deadlock

### Goal
Investigate a deadlock that occurs when restoring the `bank.c` multi-threaded application from a checkpoint.

### Application Source
- **Working Directory:** `/home/takuto1127/wamr-2.1.0-with-wasi-threads/wasm-sample/bank/`
- **Source File:** `bank.c`

### Build Instructions

1.  **Compile to WASM:**
    ```sh
    cd /home/takuto1127/wamr-2.1.0-with-wasi-threads/wasm-sample/bank/
    /opt/wasi-sdk-21/bin/clang --sysroot /home/takuto1127/wasi-libc/sysroot \
        --target=wasm32-wasi-threads -pthread \
        -Wl,--import-memory,--export-memory,--max-memory=67108864 \
        bank.c -o bank.wasm
    ```

2.  **Generate Stack Tables (Required for Checkpoint):**
    ```sh
    cd /home/takuto1127/wamr-2.1.0-with-wasi-threads/wasm-sample/bank/
    /home/takuto1127/Code/Wacret/target/release/wacret create bank.wasm
    ```
    *(Note: This command creates `tablemap_func`, `tablemap_offset`, and `type_table` files in the current directory.)*

### Execution Commands

**Normal Execution:**
```sh
cd /home/takuto1127/wamr-2.1.0-with-wasi-threads/wasm-sample/bank/
~/wamr-2.1.0-with-wasi-threads/product-mini/platforms/linux/build/iwasm \
  --max-threads=10 \
  bank.wasm
```

**Execution with Restore:**
```sh
cd /home/takuto1127/wamr-2.1.0-with-wasi-threads/wasm-sample/bank/
~/wamr-2.1.0-with-wasi-threads/product-mini/platforms/linux/build/iwasm \
  --max-threads=10 \
  --restore \
  bank.wasm
```

### Current Hypothesis
The deadlock is caused by the C/R mechanism not saving/restoring the state of native `pthread_mutex_t` locks. This leads to undefined behavior when a restored thread attempts to unlock a mutex it no longer owns, corrupting the mutex and causing deadlocks.

### Current Status
- **Reproducible**: Yes
- **Root cause identified**: Highly likely (Mutex state C/R)
- **Next focus**: Confirm hypothesis by adding diagnostics to `bank.c` to log errors from `pthread_mutex_unlock`.
