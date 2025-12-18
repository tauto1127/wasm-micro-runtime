#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../common/wasm_exec_env.h"
#include "../common/wasm_memory.h"
#include "../interpreter/wasm_runtime.h"
#include "platform_common.h"
#include "thread_manager.h"
#include "wasm_export.h"
#include "wasm_migration.h"
#include "wasm_restore.h"
#include "lib_wasi_threads_wrapper.h"
#include "wasm_dump.h"

static bool restore_flag;
void set_restore_flag(bool f)
{
    printf("restore flag set \n");
    restore_flag = f;
}
bool get_restore_flag()
{
    return restore_flag;
}


#if WASM_ENABLE_FAST_INTERP == 0
static inline WASMInterpFrame *
wasm_alloc_frame(WASMExecEnv *exec_env, uint32 size, WASMInterpFrame *prev_frame)
{
    WASMInterpFrame *frame = wasm_exec_env_alloc_wasm_frame(exec_env, size);

    if (frame) {
        frame->prev_frame = prev_frame;
#if WASM_ENABLE_PERF_PROFILING != 0
        frame->time_started = os_time_get_boot_microsecond();
#endif
    }
    else {
        wasm_set_exception((WASMModuleInstance *)exec_env->module_inst,
                           "wasm operand stack overflow");
    }

    return frame;
}


static void
_restore_stack(WASMExecEnv *exec_env, WASMInterpFrame *frame, FILE *fp)
{
    WASMModuleInstance *module_inst = exec_env->module_inst;
    WASMFunctionInstance *func = frame->function;
    int read_size = 0;

    // 初期化
    frame->sp_bottom = frame->lp + func->param_cell_num + func->local_cell_num;
    frame->sp_boundary = frame->sp_bottom + func->u.func->max_stack_cell_num;
    frame->csp_bottom = frame->sp_boundary;
    frame->csp_boundary = frame->csp_bottom + func->u.func->max_block_num;
    // frame->tsp_bottom = frame->csp_boundary;
    // frame->tsp_boundary = frame->tsp_bottom + func->u.func->max_stack_cell_num;

    // リターンアドレス
    WASMInterpFrame* prev_frame = frame->prev_frame;
    uint32 fidx, offset;
    read_size = fread(&fidx, sizeof(uint32), 1, fp);
    fread(&offset, sizeof(uint32), 1, fp);
    if (prev_frame->function != NULL)
        prev_frame->ip = wasm_get_func_code(prev_frame->function) + offset;

    // 型スタックのサイズ
    uint32 locals = func->param_count + func->local_count;
    uint32 full_type_stack_size, type_stack_size;
    fread(&full_type_stack_size, sizeof(uint32), 1, fp);
    type_stack_size = full_type_stack_size - locals;                                      // 統一フォーマットでは、ローカルも型/値スタックに入れているが、WAMRの型/値スタックのサイズはローカル抜き
    // frame->tsp = frame->tsp_bottom + type_stack_size;

    // 型スタックの中身
    fseek(fp, sizeof(uint8)*locals, SEEK_CUR);                      // localのやつはWAMRでは必要ないので飛ばす

    uint8 type_stack[type_stack_size];
    // uint32* tsp_bottom = frame->tsp_bottom;
    for (uint32 i = 0; i < type_stack_size; ++i) {
        fread(&type_stack[i], sizeof(uint8), 1, fp);
    }

    /*
     * 値スタックのサイズ
     *
     * 旧フォーマット: type stack を積算して value_stack_size を決める
     * 新フォーマット: dump 側が実測 value_stack_size を 'VSTK' マーカー付きで保存する
     *
     * stackmap がズレると旧フォーマットは ctrl stack の読み出し位置がずれて
     * CSP が破壊され、restore 後に br_if/br 等で out-of-range へ飛ぶ原因になる。
     */
    uint32 value_stack_size = 0;
    const uint32 value_stack_marker = 0x5653544B; /* 'VSTK' */
    uint32 marker_or_first_local = 0;
    long marker_pos = ftell(fp);
    if (marker_pos >= 0
        && fread(&marker_or_first_local, sizeof(uint32), 1, fp) == 1
        && marker_or_first_local == value_stack_marker) {
        fread(&value_stack_size, sizeof(uint32), 1, fp);
    }
    else {
        /* old format: rewind and compute from type stack */
        if (marker_pos >= 0) {
            fseek(fp, marker_pos, SEEK_SET);
        }
        for (uint32 i = 0; i < type_stack_size; ++i) {
            value_stack_size += type_stack[i];
        }
    }
    frame->sp = frame->sp_bottom + value_stack_size;

    // 値スタックの中身
    uint32 local_cell_num = func->param_cell_num + func->local_cell_num;
    fread(frame->lp, sizeof(uint32), local_cell_num, fp);
    fread(frame->sp_bottom, sizeof(uint32), value_stack_size, fp);

    // ラベルスタックのサイズ
    uint32 ctrl_stack_size;
    fread(&ctrl_stack_size, sizeof(uint32), 1, fp);
    frame->csp = frame->csp_bottom + ctrl_stack_size;


    // ラベルスタックの中身
    WASMBranchBlock *csp = frame->csp_bottom;
    uint8 *code = wasm_get_func_code(frame->function);
    uint8 *code_end = wasm_get_func_code_end(frame->function);
    bool csplog = getenv("WAMR_CSPLOG") != NULL;
    for (int i = 0; i < ctrl_stack_size; ++i, ++csp) {
        uint64 offset;

        // uint8 *begin_addr;
        fread(&offset, sizeof(uint32), 1, fp);
        csp->begin_addr = set_addr_offset(wasm_get_func_code(frame->function), offset);
        if (csplog && csp->begin_addr && code && code_end
            && !(csp->begin_addr >= code && csp->begin_addr < code_end)) {
            fprintf(stderr,
                    "[csplog restore] func_idx=%u idx=%d begin_addr=%p out_of_range code=%p code_end=%p off=%u\n",
                    (uint32)(frame->function - module_inst->e->functions), i,
                    csp->begin_addr, code, code_end, (uint32)offset);
        }

        // uint8 *target_addr;
        fread(&offset, sizeof(uint32), 1, fp);
        csp->target_addr = set_addr_offset(wasm_get_func_code(frame->function), offset);
        if (csplog && csp->target_addr && code && code_end
            && !(csp->target_addr >= code && csp->target_addr < code_end)) {
            fprintf(stderr,
                    "[csplog restore] func_idx=%u idx=%d target_addr=%p out_of_range code=%p code_end=%p off=%u\n",
                    (uint32)(frame->function - module_inst->e->functions), i,
                    csp->target_addr, code, code_end, (uint32)offset);
        }

        // uint32 *frame_sp;
        fread(&offset, sizeof(uint32), 1, fp);
        csp->frame_sp = set_addr_offset(frame->sp_bottom, offset);

        // uint32 *frame_tsp
        // fread(&offset, sizeof(uint32), 1, fp);
        // csp->frame_tsp = set_addr_offset(frame->tsp_bottom, offset);

        // uint32 cell_num;
        fread(&csp->cell_num, sizeof(uint32), 1, fp);

        // uint32 count;
        // fread(&csp->count, sizeof(uint32), 1, fp);
    }
}

WASMInterpFrame*
wasm_restore_stack(WASMExecEnv **_exec_env, char* file_prefix)
{
    printf("restore stack\n");
    WASMExecEnv *exec_env = *_exec_env;
    WASMModuleInstance *module_inst =
        (WASMModuleInstance *)exec_env->module_inst;
    WASMInterpFrame *frame, *prev_frame = wasm_exec_env_get_cur_frame(exec_env);
    frame = prev_frame;
    WASMFunctionInstance *function;
    uint32 func_idx, frame_size, all_cell_num;
    FILE *fp;

    uint32 frame_stack_size;
    char file_name_frame[MAX_FILE_NAME_LENGTH] = "frame_count.img";
    str_add_prefix(file_name_frame, file_prefix);
    fp = open_image(file_name_frame, "rb");
    fread(&frame_stack_size, sizeof(uint32), 1, fp);
    fclose(fp);

    uint32 fidx = 0;
    char file_name_stack[MAX_FILE_NAME_LENGTH] = "";
    for (uint32 i = frame_stack_size; i > 0; --i) {
        // char* file_name_stack = wasm_runtime_malloc(sizeof(char) * MAX_FILE_NAME_LENGTH);
        sprintf(file_name_stack, "stack%d.img", i);
        str_add_prefix(file_name_stack, file_prefix);
        fp = open_image(file_name_stack, "rb");

        fread(&fidx, sizeof(uint32), 1, fp);
        // 関数からスタックサイズを計算し,ALLOC
        // 前のframe2のenter_func_idxが、このframe->functionに対応
        function = module_inst->e->functions + fidx;

        // TODO: uint64になってるけど、多分uint32
        all_cell_num = (uint32)function->param_cell_num
                        + (uint32)function->local_cell_num
                        + (uint32)function->u.func->max_stack_cell_num
                        + ((uint32)function->u.func->max_block_num)
                                * sizeof(WASMBranchBlock) / 4
                        + (uint32)function->u.func->max_stack_cell_num;
        frame_size = wasm_interp_interp_frame_size(all_cell_num);
        frame = wasm_alloc_frame(exec_env, frame_size,
                            (WASMInterpFrame *)prev_frame);

        // フレームをrestore
        frame->function = function;
        _restore_stack(exec_env, frame, fp);

        prev_frame = frame;
        fclose(fp);
    }

    wasm_exec_env_set_cur_frame(exec_env, frame);

    _exec_env = &exec_env;

    return frame;
}

void restore_dirty_memory(WASMMemoryInstance **memory, FILE* memory_fp) {
    const int PAGE_SIZE = 4096;
    while (!feof(memory_fp)) {
        if (feof(memory_fp)) break;
        uint32 offset;
        uint32 len;
        len = fread(&offset, sizeof(uint32), 1, memory_fp);
        if (len == 0) break;
        // printf("len: %d\n", len);
        // printf("i: %d\n", offset);

        uint8* addr = (*memory)->memory_data + offset;
        len = fread(addr, PAGE_SIZE, 1, memory_fp);
        // printf("PAGESIZE: %d\n", len);
    }
}

int wasm_restore_memory(WASMModuleInstance *module, WASMMemoryInstance **memory, uint8** maddr, char* file_prefix) {
    char file_name_mem[MAX_FILE_NAME_LENGTH] = "memory.img";
    str_add_prefix(file_name_mem, file_prefix);

    char file_name_mem_size[MAX_FILE_NAME_LENGTH] = "mem_page_count.img";
    str_add_prefix(file_name_mem_size, file_prefix);

    FILE* memory_fp = open_image(file_name_mem, "rb");
    FILE* mem_size_fp = open_image(file_name_mem_size, "rb");

    // restore page_count
    uint32 page_count;
    fread(&page_count, sizeof(uint32), 1, mem_size_fp);
    wasm_enlarge_memory(module, page_count- (*memory)->cur_page_count);
    *maddr = page_count * (*memory)->num_bytes_per_page;

    restore_dirty_memory(memory, memory_fp);
    // restore memory_data
    // fread((*memory)->memory_data, sizeof(uint8),
    //         (*memory)->num_bytes_per_page * (*memory)->cur_page_count, memory_fp);

    fclose(memory_fp);
    fclose(mem_size_fp);
    return 0;
}

int wasm_restore_global(const WASMModuleInstance *module, const WASMGlobalInstance *globals, uint8 **global_data, uint8 **global_addr, char* file_prefix) {
    char file_name_global[MAX_FILE_NAME_LENGTH] = "global.img";
    str_add_prefix(file_name_global, file_prefix);
    FILE* fp = open_image(file_name_global, "rb");

    for (int i = 0; i < module->e->global_count; i++) {
        switch (globals[i].type) {
            case VALUE_TYPE_I32:
            case VALUE_TYPE_F32:
                *global_addr = get_global_addr_for_migration(*global_data, globals + i);
                fread(*global_addr, sizeof(uint32), 1, fp);
                break;
            case VALUE_TYPE_I64:
            case VALUE_TYPE_F64:
                *global_addr = get_global_addr_for_migration(*global_data, globals + i);
                fread(*global_addr, sizeof(uint64), 1, fp);
                break;
            default:
                perror("wasm_restore_global:type error:A\n");
                break;
        }
    }

    fclose(fp);
    return 0;
}


int wasm_restore_program_counter(
    WASMModuleInstance *module,
    uint8 **frame_ip,
    char* file_prefix)
{
    char file_name_pc[MAX_FILE_NAME_LENGTH] = "program_counter.img";
    str_add_prefix(file_name_pc, file_prefix);
    FILE* fp = open_image(file_name_pc, "rb");

    uint32 fidx, offset;
    fread(&fidx, sizeof(uint32), 1, fp);
    fread(&offset, sizeof(uint32), 1, fp);

    *frame_ip = wasm_get_func_code(module->e->functions + fidx) + offset;
    fclose(fp);

    return 0;
}

int wasm_restore(WASMModuleInstance **module,
            WASMExecEnv **exec_env,
            WASMFunctionInstance **cur_func,
            WASMInterpFrame **prev_frame,
            WASMMemoryInstance **memory,
            WASMGlobalInstance **globals,
            uint8 **global_data,
            uint8 **global_addr,
            WASMInterpFrame **frame,
            uint8 **frame_ip,
            uint32 **frame_lp,
            uint32 **frame_sp,
            WASMBranchBlock **frame_csp,
            uint8 **frame_ip_end,
            uint8 **else_addr,
            uint8 **end_addr,
            uint8 **maddr,
            bool *done_flag,
            char* file_prefix)
{
    struct timespec ts1, ts2;
    // メインスレッドのみ復元
    if (strcmp(file_prefix, MAIN_THREAD_PREFIX) == 0) {
        // restore memory
        clock_gettime(CLOCK_MONOTONIC, &ts1);
        wasm_restore_memory(*module, memory, maddr, file_prefix);
        clock_gettime(CLOCK_MONOTONIC, &ts2);
        fprintf(stderr, "memory, %lu\n", get_time(ts1, ts2));
        // printf("Success to restore linear memory\n");
    }

    // restore globals
    clock_gettime(CLOCK_MONOTONIC, &ts1);
    wasm_restore_global(*module, *globals, global_data, global_addr, file_prefix);
    clock_gettime(CLOCK_MONOTONIC, &ts2);
    fprintf(stderr, "global, %lu\n", get_time(ts1, ts2));
    // printf("Success to restore globals\n");

    // restore program counter
    clock_gettime(CLOCK_MONOTONIC, &ts1);
    wasm_restore_program_counter(*module, frame_ip, file_prefix);
    clock_gettime(CLOCK_MONOTONIC, &ts2);
    fprintf(stderr, "program counter, %lu\n", get_time(ts1, ts2));
    // printf("Success to program counter\n");

    return 0;
}

int wasm_restore_thread_start_arg(ThreadStartArg* thread_start_arg, wasm_module_inst_t new_module_inst, char* file_prefix) {
    char file_name[MAX_FILE_NAME_LENGTH] = "thread_state.img";
    str_add_prefix(file_name, file_prefix);
    FILE *fp = open_image(file_name, "rb");
    printf("-==============================restore\n");
    fread(&thread_start_arg->thread_id, sizeof(int32), 1, fp);
    printf("thread %d restore\n", thread_start_arg->thread_id);
    fread(&thread_start_arg->arg, sizeof(uint32), 1, fp);

    WASMFunctionInstanceCommon* start_func =
        wasm_runtime_lookup_function(new_module_inst, THREAD_START_FUNCTION);
    if (!start_func) {
        LOG_ERROR("Failed to find thread start function %s",
                  THREAD_START_FUNCTION);
        goto thread_preparation_fail;
    }

    thread_start_arg->start_func = (wasm_function_inst_t *)start_func;
    fclose(fp);

    return 0;

    thread_preparation_fail:
        if (new_module_inst)
            wasm_runtime_deinstantiate_internal(new_module_inst, true);
        if (thread_start_arg)
            wasm_runtime_free(thread_start_arg);

        return -1;
    // wasm_runtime_lookup_function(new_module_inst, THREAD_START_FUNCTION);
    /*wasm_function_inst_t* func = thread_arg->func;*/\
    /*os_mutex_unlock(&exec_env->wait_lock);\*/\
}

// restore wasm thread and start execution
// 親のフラグを継承する
int wasm_restore_thread(wasm_exec_env_t parent_exec_env, char* file_prefix) {
    // モジュールインスタンスを作成する．
    wasm_module_t module = wasm_exec_env_get_module(parent_exec_env);
    wasm_module_inst_t module_inst = get_module_inst(parent_exec_env);
    wasm_module_inst_t new_module_inst = NULL;
    ThreadStartArg *thread_start_arg = NULL;
    wasm_function_inst_t start_func;
    int32 thread_id;
    uint32 stack_size = 8192;
    int32 ret = -1;

    bh_assert(module);
    bh_assert(module_inst);

    // モジュールインスタンスの生成
    stack_size = ((WASMModuleInstance *)module_inst)->default_wasm_stack_size;

    printf("start init module inst\n");
    if (!(new_module_inst = wasm_runtime_instantiate_internal(
              module, module_inst, parent_exec_env, stack_size, 0, 0, true, NULL, 0))){
                  printf("Failed to create new module inst\n");
                  return -1;
              }

    printf("done init module inst\n");

    // 親からカスタムデータなどを引き継ぐ
    wasm_runtime_set_custom_data_internal(
        new_module_inst, wasm_runtime_get_custom_data(module_inst));
    printf("done init custom\n");

    // if (!(wasm_cluster_dup_c_api_imports(new_module_inst, module_inst)))
    //     goto thread_preparation_fail;

    wasm_native_inherit_contexts(new_module_inst, module_inst);
    printf("done init contexts\n");

    // スレッド開始に必要な引数を復元
    if (!(thread_start_arg = wasm_runtime_malloc(sizeof(ThreadStartArg)))) {
        LOG_ERROR("Runtime args allocation failed");
    }
    printf("%sStart constructing Wasm VM\n", file_prefix);
    wasm_restore_thread_start_arg(thread_start_arg, new_module_inst, file_prefix);
    printf("%sStart constructing Wasm VM\n", file_prefix);

    // is_aux_stack_allocatedはthreads_spawn_wrapperに合わせてfalse にする
    // aux_stack_start, sizeも
    // ここでexec_envを作っている
    wasm_cluster_create_thread(parent_exec_env, new_module_inst, false, 0, 0, thread_start, thread_start_arg);

    return 0;
}
#endif // WASM_ENABLE_FAST_INTERP != 0
