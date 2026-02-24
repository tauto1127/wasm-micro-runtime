#ifndef _WASM_DUMP_H
#define _WASM_DUMP_H

#include "../common/wasm_exec_env.h"
#include "../interpreter/wasm_interp.h"

void wasm_set_checkpoint(bool f);
bool wasm_get_checkpoint();
bool wasm_ckpt_profile_is_enabled(void);
void wasm_ckpt_profile_reset_events(void);
void wasm_ckpt_profile_flush_events(void);
void wasm_ckpt_profile_flush_restore_events(void);
void wasm_ckpt_record_phase(WASMExecEnv *exec_env, const char *phase,
                            const struct timespec *start_ts,
                            const struct timespec *end_ts);
void wasm_ckpt_record_phase_with_tid(int thread_id, const char *phase,
                                     const struct timespec *start_ts,
                                     const struct timespec *end_ts);
void wasm_ckpt_record_dispatch_wait(WASMExecEnv *exec_env,
                                    const struct timespec *start_ts,
                                    const struct timespec *end_ts);
// void checkpoint_routine(WASMCluster *cluster);
// nopからのチェックポイント用
void* checkpoint_thread_routine(void* arg);
extern struct timespec startAtNop, endAtNop;

int wasm_dump(WASMExecEnv *exec_env,
         struct WASMModuleInstance *module,
         struct WASMMemoryInstance *memory,
         struct WASMGlobalInstance *globals,
         uint8 *global_data,
         uint8 *global_addr,
         struct WASMFunctionInstance *cur_func,
         struct WASMInterpFrame *frame,
         register uint8 *frame_ip,
         register uint32 *frame_sp,
         WASMBranchBlock *frame_csp,
        //  uint32 *frame_tsp,
         uint8 *frame_ip_end,
         uint8 *else_addr,
         uint8 *end_addr,
         uint8 *maddr,
         bool done_flag,
         char *file_prefix);

void str_add_prefix(char* file_name, char* file_prefix);

#endif // _WASM_CHECKPOINT_H

void* signal_control_routine(void *arg);
char* get_file_prefix(int32 thread_id);

#if WASM_ENABLE_CR != 0
#define MAIN_THREAD_PREFIX "main-"
#define MAX_FILE_NAME_LENGTH 100
#endif
