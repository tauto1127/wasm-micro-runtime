#include "wasm_exec_env.h"
#include "wasm_thread_migration.h"
#include "thread_manager.h"

int *wait_thread_ids = NULL;
int wait_thread_ids_count = 0;

// SIGUSR2を受け取った時の処理
void
checkpoint_routine(WASMCluster *cluster)
{
    struct timespec startAt, endAt;
    // dump linear memory
    int waits = wasm_cluster_get_waiting_thread_count(cluster);
    int counts = wasm_cluster_get_thread_count(cluster);

    struct AtomicCounter *counter =
        wasm_cluster_init_checkpointing_counter(cluster, 0);

    printf("=======スレッドの同時停止開始=========\n");
    // 停止シグナル
    wasm_cluster_send_signal_all(cluster, WAMR_SIG_CHECKPOINT);

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
    }
    // printf("======waitしているスレッド一覧をダンプします========\n");
    wait_thread_ids = wasm_cluster_get_waiting_thread_ids(cluster);
    wait_thread_ids_count = waits;

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
