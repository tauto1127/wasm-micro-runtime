#include "platform_api_extension.h"
#include "platform_api_vmcore.h"
#include "platform_common.h"
#include "thread_manager.h"
#include "wasm_checkpoint.h"

// checkpoint for thread routine
void *
signal_control_routine(void *arg)
{
    // チェックポイント用スレッドの処理
    WASMCluster *cluster = (WASMCluster *)arg;
    sigset_t set;
    int sig;

    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    sigaddset(&set, SIGUSR2);

    /* Loop forever waiting for SIGUSR1/2 and coordinate stop/resume */
    while (true) {
        if (sigwait(&set, &sig) != 0) {
            continue;
        }
        // チェックポイントシグナルが届いた時
        if (sig == SIGUSR2) {
            int waits = wasm_cluster_get_waiting_thread_count(cluster);
            printf("signal_control_routine: received SIGUSR2, 待機中スレッド: %d\n", waits);
            int counts = wasm_cluster_get_thread_count(cluster);
            printf("signal_control_routine: total threads: %d\n", counts);

            int not_waiting = counts - waits;

            struct AtomicCounter* counter = wasm_cluster_init_checkpointing_counter(cluster,not_waiting);
            printf("checkpointing_counter init : %d\n", not_waiting);
            // 停止シグナル
            wasm_cluster_send_signal_all(cluster, WAMR_SIG_CHECKPOINT);
            // wasm_cluster_send_signal_all(cluster, WAMR_SIG_CHECKPOINT);
            while(1) {
                os_cond_wait(&counter->cond, &counter->lock);
                os_mutex_lock(&counter->lock);
                printf("checkpointing_counter check: %d", counter->checkpointing_count);
                if (counter->checkpointing_count == 0) {
                    os_mutex_unlock(&counter->lock);
                    printf("all normal threads wake up!!");
                    break;
                }
                os_mutex_unlock(&counter->lock);
            };

            wasm_cluster_wake_up_threads(cluster);
            printf("waiting threads wake up!!");
        }
        else if (sig == SIGUSR1) {
            wasm_cluster_thread_continue_all(cluster);
        }
    }

    return NULL;
}
