#include "wasm_exec_env.h"
#include "wasm_thread_migration.h"
#include "wasm_migration.h"
#include "thread_manager.h"
#include "lib_wasi_threads_wrapper.h"
#include "wasm_runtime_common.h"
#include "wasm_native.h"
#include "bh_log.h"
#include <errno.h>

int *wait_thread_ids = NULL;
int wait_thread_ids_count = 0;

// SIGUSR2を受け取った時のチェックポイントスレッドの処理
void
checkpoint_routine(WASMCluster *cluster)
{
    struct timespec startAt, endAt;
    // dump linear memory
    int waits = wasm_cluster_get_waiting_thread_count(cluster);
    int counts = wasm_cluster_get_thread_count(cluster);

    // # Phase 1
    struct AtomicCounter *counter =
        wasm_cluster_init_checkpointing_counter(cluster, 0);

    fprintf(stderr, "=======スレッドの同時停止開始=========\n");
    // 停止シグナル
    wasm_cluster_send_signal_all(cluster, WAMR_SIG_CHECKPOINT);
    // recv 等でブロック中のスレッドを（終了させずに）起こし、interp ループへ
    // 戻して WAMR_SIG_CHECKPOINT を観測させる。これをしないと下の Phase 2 の
    // 待ちが永久にブロックする。
    wasm_cluster_wakeup_blocking_threads_for_checkpoint(cluster);

    // for (;;) {
    //     os_mutex_unlock(&counter->lock);
    //     sleep(1);
    //     os_mutex_lock(&counter->lock);
    //     printf("waits:%d, counts:%d, counter:%d\n", waits, counts,
    //            counter->checkpoint_count);
    //     os_mutex_unlock(&counter->lock);
    // }
    // # Phase 2
    // NOTE: get_thread_count()/get_waiting_thread_count() take cluster->lock,
    // and increase_checkpointing_counter() takes cluster->lock then
    // counter->lock. To avoid an ABBA deadlock we must NOT hold counter->lock
    // while querying the cluster counts, so read them before locking counter.
    for (;;) {
        waits = wasm_cluster_get_waiting_thread_count(cluster);
        counts = wasm_cluster_get_thread_count(cluster);

        os_mutex_lock(&counter->lock);
        if (counter->checkpoint_count + waits == counts) {
            os_mutex_unlock(&counter->lock);
            break;
        }
        os_cond_wait(&counter->cond, &counter->lock);
        os_mutex_unlock(&counter->lock);
    };
    // printf("======waitしているスレッド一覧をダンプします========\n");
    wait_thread_ids = wasm_cluster_get_waiting_thread_ids(cluster);
    wait_thread_ids_count = waits;

    // Phase 3
    // =====スレッド同時停止======
    wasm_cluster_wake_up_threads(cluster);
    for (;;) {
        counts = wasm_cluster_get_thread_count(cluster);
        os_mutex_lock(&counter->lock);
        if (counter->checkpoint_count == counts) {
            os_mutex_unlock(&counter->lock);
            break;
        }
        os_cond_wait(&counter->cond, &counter->lock);
        os_mutex_unlock(&counter->lock);
    };

    // ========チェックポイント開始========
    counter = wasm_cluster_init_checkpointing_counter(cluster, 0);
    wasm_cluster_thread_continue_all(cluster);

    // やっぱちゃんとカウントしないと，全部終わったか分からんな
    for (;;) {
        counts = wasm_cluster_get_thread_count(cluster);
        os_mutex_lock(&counter->lock);
        if (counter->checkpoint_count == counts) {
            os_mutex_unlock(&counter->lock);
            break;
        }
        os_cond_wait(&counter->cond, &counter->lock);
        os_mutex_unlock(&counter->lock);
    };
    // clock_gettime(CLOCK_MONOTONIC, &endAtNop);
    // printf("checkpoint time(nopから): %lu ns\n",
    //        get_time(startAtNop, endAtNop));
    fprintf(stderr, "checkpoint done:%lu\n", get_time(startAt, endAt));

    exit(0);
}

void *
signal_control_routine(void *arg)
{
    fprintf(stderr, "3 stderr\n");
    // チェックポイント用スレッドの処理
    WASMCluster *cluster = (WASMCluster *)arg;
    sigset_t set;
    int sig;

    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    sigaddset(&set, SIGUSR2);

    while (true) {
        if (sigwait(&set, &sig) != 0) {
            fprintf(stderr, "sigwait failed: %d\n", errno);
            continue;
        }
        // チェックポイントシグナルが届いた時
        if (sig == SIGUSR2) {
            // 戻す
            fprintf(stderr, "SIGUSR2 called, %ld\n", pthread_self());
            checkpoint_routine(cluster);
        }
        else if (sig == SIGUSR1) {
            fprintf(stderr, "SIGUSR1 called, %ld\n", pthread_self());
            // wasm_cluster_thread_continue_all(cluster);
        }
    }

    return NULL;
}

// thread_id, スレッドの開始関数の引数をdump
int
wasm_dump_thread_states(WASMExecEnv *exec_env, char *file_prefix)
{
    ThreadStartArg *thread_arg = (ThreadStartArg *)exec_env->thread_arg;

    // main thread
    if (thread_arg == NULL) {
        printf("main thread dump_thread_states\n");
        FILE *fp;
        char file_name[MAX_FILE_NAME_LENGTH] = "thread_state.img";
        str_add_prefix(file_name, file_prefix);
        fp = wamr_open_image(file_name, "wb");
        if (fp == NULL) {
            fprintf(stderr, "failed to open %s\n", file_name);
            return -1;
        }

        int16 thread_count = wasm_cluster_get_thread_count(exec_env->cluster);
        // スレッドの総数を保存
        dump_value(&thread_count, sizeof(int16), 1, fp);
        // // ここでfile_prefix一覧を保存する
        // WASMExecEnv *exec_env_iter =
        // wasm_cluster_get_first_exec_env(exec_env->cluster);
        // wasm_cluster_traverse_lock(exec_env);
        // char* str_to_dump = wasm_runtime_malloc(sizeof(char) *
        // MAX_FILE_NAME_LENGTH); while (exec_env_iter) {
        //     ThreadStartArg *arg = (ThreadStartArg
        //     *)exec_env_iter->thread_arg; int32 tid = (arg == NULL) ? -1 :
        //     arg->thread_id; char* prefix = get_file_prefix(tid);

        //     if(count == 0) sprintf(str_to_dump, "%s", prefix);
        //     else sprintf(str_to_dump, ",%s", prefix);
        //     wasm_runtime_free(prefix);

        //     exec_env_iter = exec_env_iter->next;
        //     count++;
        // }
        int *ids = wasm_cluster_get_thread_ids(exec_env->cluster);
        int count = 0;
        for (int *p = ids; *p != -1; ++p) {
            printf("thread id: %d\n", *p);
            count++;
        }

        dump_value(ids, sizeof(int), count, fp);
        wasm_runtime_free(ids);

        // 待機中スレッド一覧の保存
        printf("待機中スレッド一覧の保存をdump_valueで行います．数；%d\n",
               wait_thread_ids_count);
        dump_value(&wait_thread_ids_count, sizeof(int), 1, fp);
        dump_value(wait_thread_ids, sizeof(int), wait_thread_ids_count, fp);
        wasm_runtime_free(wait_thread_ids);

        return 0;
    }

    // 子スレッド
    FILE *fp;
    char file_name[MAX_FILE_NAME_LENGTH] = "thread_state.img";
    str_add_prefix(file_name, file_prefix);
    fp = wamr_open_image(file_name, "wb");
    if (fp == NULL) {
        fprintf(stderr, "failed to open %s\n", file_name);
        return -1;
    }

    int32 thread_id = thread_arg->thread_id;
    uint32 arg = thread_arg->arg;
    dump_value(&thread_id, sizeof(int32), 1, fp);
    dump_value(&arg, sizeof(uint32), 1, fp);

    return 0;
}

// スレッドidから，ファイルprefixを生成．メインスレッドの場合は-1を入れる．
char *
get_file_prefix(int32 thread_id)
{
    if (thread_id == -1) {
        return MAIN_THREAD_PREFIX;
    }
    else {
        char *prefix = wasm_runtime_malloc(sizeof(char) * MAX_FILE_NAME_LENGTH);
        sprintf(prefix, "%d-", thread_id);
        return prefix;
    }
}

str_add_prefix(char *file_name, char *file_prefix)
{
    if (file_prefix == NULL)
        return;
    size_t len = strlen(file_name) + strlen(file_prefix) + 1;
    char *buf = malloc(len);
    if (!buf)
        return;

    strcpy(buf, file_prefix);
    strcat(buf, file_name);
    strcpy(file_name, buf);
}

// thread_id とスレッド開始関数の引数を <prefix>thread_state.img から復元し、
// 開始関数を解決して thread_start_arg を組み立てる
int
wasm_restore_thread_start_arg(ThreadStartArg *thread_start_arg,
                              wasm_module_inst_t new_module_inst,
                              char *file_prefix)
{
    char file_name[MAX_FILE_NAME_LENGTH] = "thread_state.img";
    str_add_prefix(file_name, file_prefix);
    FILE *fp = wamr_open_image(file_name, "rb");
    if (fp == NULL) {
        fprintf(stderr, "failed to open %s\n", file_name);
        goto thread_preparation_fail;
    }
    fread(&thread_start_arg->thread_id, sizeof(int32), 1, fp);
    printf("thread %d restore\n", thread_start_arg->thread_id);
    fread(&thread_start_arg->arg, sizeof(uint32), 1, fp);

    wasm_function_inst_t start_func =
        wasm_runtime_lookup_function(new_module_inst, THREAD_START_FUNCTION,
                                     NULL);
    if (!start_func) {
        LOG_ERROR("Failed to find thread start function %s",
                  THREAD_START_FUNCTION);
        fclose(fp);
        goto thread_preparation_fail;
    }

    thread_start_arg->start_func = start_func;
    fclose(fp);

    return 0;

thread_preparation_fail:
    if (new_module_inst)
        wasm_runtime_deinstantiate_internal(new_module_inst, true);
    if (thread_start_arg)
        wasm_runtime_free(thread_start_arg);

    return -1;
}

// restore wasm thread and start execution
// 親のフラグを継承する
int
wasm_restore_thread(wasm_exec_env_t parent_exec_env, char *file_prefix)
{
    // モジュールインスタンスを作成する．
    wasm_module_t module = wasm_exec_env_get_module(parent_exec_env);
    wasm_module_inst_t module_inst = get_module_inst(parent_exec_env);
    wasm_module_inst_t new_module_inst = NULL;
    ThreadStartArg *thread_start_arg = NULL;
    uint32 stack_size = 8192;

    bh_assert(module);
    bh_assert(module_inst);

    // モジュールインスタンスの生成
    stack_size = ((WASMModuleInstance *)module_inst)->default_wasm_stack_size;

    if (!(new_module_inst = wasm_runtime_instantiate_internal(
              module, module_inst, parent_exec_env, stack_size, 0, NULL, 0,
              true))) {
        printf("Failed to create new module inst\n");
        return -1;
    }

    // 親からカスタムデータなどを引き継ぐ
    wasm_runtime_set_custom_data_internal(
        new_module_inst, wasm_runtime_get_custom_data(module_inst));

    wasm_native_inherit_contexts(new_module_inst, module_inst);

    // スレッド開始に必要な引数を復元
    if (!(thread_start_arg = wasm_runtime_malloc(sizeof(ThreadStartArg)))) {
        LOG_ERROR("Runtime args allocation failed");
        wasm_runtime_deinstantiate_internal(new_module_inst, true);
        return -1;
    }

    if (wasm_restore_thread_start_arg(thread_start_arg, new_module_inst,
                                      file_prefix)
        != 0) {
        return -1;
    }

    // is_aux_stack_allocated は threads_spawn_wrapper に合わせて false にする
    // ここで exec_env を作っている
    wasm_cluster_create_thread(parent_exec_env, new_module_inst, false, 0, 0,
                               thread_start, thread_start_arg);

    return 0;
}
