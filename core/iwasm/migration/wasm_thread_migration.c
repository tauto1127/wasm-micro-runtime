#include "wasm_exec_env.h"
#include "wasm_thread_migration.h"
#include "wasm_migration.h"
#include "thread_manager.h"
#include "lib_wasi_threads_wrapper.h"

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

    printf("=======スレッドの同時停止開始=========\n");
    // 停止シグナル
    wasm_cluster_send_signal_all(cluster, WAMR_SIG_CHECKPOINT);

    // for (;;) {
    //     os_mutex_unlock(&counter->lock);
    //     sleep(1);
    //     os_mutex_lock(&counter->lock);
    //     printf("waits:%d, counts:%d, counter:%d\n", waits, counts,
    //            counter->checkpoint_count);
    //     os_mutex_unlock(&counter->lock);
    // }
    // # Phase 2
    os_mutex_lock(&counter->lock);
    for (;;) {
        waits = wasm_cluster_get_waiting_thread_count(cluster);
        counts = wasm_cluster_get_thread_count(cluster);

        printf("waits:%d, counts:%d, counter:%d\n", waits, counts,
               counter->checkpoint_count);
        if (counter->checkpoint_count + waits == counts) {
            os_mutex_unlock(&counter->lock);
            break;
        }

        os_cond_wait(&counter->cond, &counter->lock);
    };
    // printf("======waitしているスレッド一覧をダンプします========\n");
    wait_thread_ids = wasm_cluster_get_waiting_thread_ids(cluster);
    wait_thread_ids_count = waits;

    // Phase 3
    // =====スレッド同時停止======
    wasm_cluster_wake_up_threads(cluster);
    os_mutex_lock(&counter->lock);
    for (;;) {
        counts = wasm_cluster_get_thread_count(cluster);
        if (counter->checkpoint_count == counts) {
            os_mutex_unlock(&counter->lock);
            break;
        }
        os_cond_wait(&counter->cond, &counter->lock);
    };

    // ========チェックポイント開始========
    counter = wasm_cluster_init_checkpointing_counter(cluster, 0);
    wasm_cluster_thread_continue_all(cluster);

    // やっぱちゃんとカウントしないと，全部終わったか分からんな
    os_mutex_lock(&counter->lock);
    for (;;) {
        counts = wasm_cluster_get_thread_count(cluster);
        if (counter->checkpoint_count == counts) {
            os_mutex_unlock(&counter->lock);
            break;
        }
        os_cond_wait(&counter->cond, &counter->lock);
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
    printf("3\n");
    // チェックポイント用スレッドの処理
    WASMCluster *cluster = (WASMCluster *)arg;
    sigset_t set;
    int sig;

    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    sigaddset(&set, SIGUSR2);

    while (true) {
        if (sigwait(&set, &sig) != 0) {
            continue;
        }
        // チェックポイントシグナルが届いた時
        if (sig == SIGUSR2) {
            // 戻す
            printf("SIGUSR2 called, %ld\n", pthread_self());
            checkpoint_routine(cluster);
        }
        else if (sig == SIGUSR1) {
            printf("SIGUSR1 called, %ld", pthread_self());
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
