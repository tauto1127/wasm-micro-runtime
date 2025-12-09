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

            struct AtomicCounter* counter = wasm_cluster_init_checkpointing_counter(cluster,0);
            printf("checkpointing_counter init : %d\n", 0);
            // 停止シグナル
            wasm_cluster_send_signal_all(cluster, WAMR_SIG_CHECKPOINT);
            // wasm_cluster_send_signal_all(cluster, WAMR_SIG_CHECKPOINT);
            os_mutex_lock(&counter->lock);
            for(;;) {
                waits = wasm_cluster_get_waiting_thread_count(cluster);
                counts = wasm_cluster_get_thread_count(cluster);
                printf("総スレッド数：%d, 総waitingスレッド：%d, 総チェックポイント中スレッド：%d\n", counts, waits, counter->checkpointing_count);
                if (counter->checkpointing_count + waits == counts) {
                    os_mutex_unlock(&counter->lock);
                    printf("all threads stopped!!\n");
                    break;
                }

                os_cond_wait(&counter->cond, &counter->lock);
                // if (counter->checkpointing_count == 0) {
                //     os_mutex_unlock(&counter->lock);
                //     printf("all normal threads wake up!!\n");
                //     break;
                // }
            };
            os_mutex_unlock(&counter->lock);
            printf("waiting threadを起こします");

            // =====スレッド同時停止======
            wasm_cluster_wake_up_threads(cluster);
            os_mutex_lock(&counter->lock);
            for(;;) {
                printf("checkpointing_counter check: %d\n", counter->checkpointing_count);
                counts = wasm_cluster_get_thread_count(cluster);
                if (counter->checkpointing_count == counts) {
                    os_mutex_unlock(&counter->lock);
                    printf("all waiting threads wake up!! checkpointing count: %d\n", counter->checkpointing_count);
                    break;
                }
                os_cond_wait(&counter->cond, &counter->lock);
            };

            // ========チェックポイント開始========
            printf("start checkpoint \n");
            counter = wasm_cluster_init_checkpointing_counter(cluster, 0);
            wasm_cluster_thread_continue_all(cluster);

            // やっぱちゃんとカウントしないと，全部終わったか分からんな
            os_mutex_lock(&counter->lock);
            for(;;) {
                printf("checkpointed_counter check: %d\n", counter->checkpointing_count);
                counts = wasm_cluster_get_thread_count(cluster);
                if (counter->checkpointing_count == counts) {
                    printf("all waiting threads check pointed!! count: %d\n", counter->checkpointing_count);
                    os_mutex_unlock(&counter->lock);
                    break;
                }
                os_cond_wait(&counter->cond, &counter->lock);
            };

            exit(0);
            // wasm_cluster_reset_checkpointing_counter(cluster);
        }
        else if (sig == SIGUSR1) {
            printf("SIGUSR1 called, %ld", pthread_self());
            wasm_cluster_thread_continue_all(cluster);
        }
    }

    return NULL;
}
