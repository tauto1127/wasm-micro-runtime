#include "thread_manager.h"
#include "wasm_checkpoint.h"

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
        if (sig == SIGUSR2) {
            int waits = wasm_cluster_get_waiting_thread_count(cluster);
            printf("signal_control_routine: received SIGUSR2, 待機中スレッド: %d\n", waits);
            int counts = wasm_cluster_get_thread_count(cluster);
            printf("signal_control_routine: total threads: %d\n", counts);

            int not_waiting = counts - waits;

            // 停止シグナル
            wasm_cluster_send_signal_all(cluster, WAMR_SIG_STOP);
            // wasm_cluster_send_signal_all(cluster, WAMR_SIG_CHECKPOINT);

            wasm_cluster_wake_up_threads(cluster);
            printf("waiting threads wake up!!");
        }
        else if (sig == SIGUSR1) {
            wasm_cluster_thread_continue_all(cluster);
        }
    }

    return NULL;
}
