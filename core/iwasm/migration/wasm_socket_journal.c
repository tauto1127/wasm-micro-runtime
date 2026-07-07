#include "wasm_socket_journal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wasm_migration.h"
#include "wasm_thread_migration.h"
#include "wasm_runtime_common.h"

#define MAX_SOCK_OPS 4096

static sock_op g_journal[MAX_SOCK_OPS];
static uint32_t g_journal_count = 0;
static bool g_journal_overflowed = false;
static korp_mutex journal_lock;
static bool journal_lock_initialized = false;

/* Initialize the journal lock once, before any wasm code (hence any socket op)
 * can run. Using a constructor avoids the data race of lazily initializing the
 * mutex from socket_journal_record(), which is called concurrently from
 * embeddedRTPS worker threads. */
__attribute__((constructor)) static void
socket_journal_ctor(void)
{
    if (os_mutex_init(&journal_lock) == BHT_OK) {
        journal_lock_initialized = true;
    }
}

static void
lock_journal(void)
{
    /* The constructor above initializes the lock before main(); this branch is
     * only a defensive fallback and is not reached concurrently. */
    if (journal_lock_initialized) {
        os_mutex_lock(&journal_lock);
    }
}

static void
unlock_journal(void)
{
    if (journal_lock_initialized) {
        os_mutex_unlock(&journal_lock);
    }
}

void
socket_journal_record(const sock_op *op)
{
    lock_journal();
    if (g_journal_count < MAX_SOCK_OPS) {
        g_journal[g_journal_count++] = *op;
    } else {
        g_journal_overflowed = true;
        fprintf(stderr,
                "Warning: socket journal is full (%d ops), operation dropped; "
                "restore will be incomplete.\n",
                MAX_SOCK_OPS);
    }
    unlock_journal();
}

bool
socket_journal_has_open(int32_t fd)
{
    uint32_t i;
    bool found = false;
    lock_journal();
    for (i = 0; i < g_journal_count; i++) {
        if (g_journal[i].kind == SOCK_OP_OPEN && g_journal[i].fd == fd) {
            found = true;
        }
        else if ((g_journal[i].kind == SOCK_OP_CLOSE) && g_journal[i].fd == fd) {
            /* a later close cancels a previous open of the same fd number */
            found = false;
        }
    }
    unlock_journal();
    return found;
}

int
socket_journal_dump(const char *file_prefix)
{
    char file_name[128] = "socket.img";
    FILE *fp;
    int32_t count;

    if (file_prefix) {
        str_add_prefix(file_name, (char *)file_prefix);
    }

    fp = wamr_open_image(file_name, "wb");
    if (!fp) {
        return -1;
    }

    lock_journal();
    if (g_journal_overflowed) {
        fprintf(stderr,
                "Error: socket journal overflowed before checkpoint; the dump "
                "is incomplete and restore will not fully rebuild sockets.\n");
    }
    count = (int32_t)g_journal_count;
    if (fwrite(&count, sizeof(int32_t), 1, fp) != 1) {
        fprintf(stderr, "Failed to write journal count to %s\n", file_name);
        unlock_journal();
        fclose(fp);
        return -1;
    }

    if (count > 0) {
        if (fwrite(g_journal, sizeof(sock_op), count, fp) != (size_t)count) {
            fprintf(stderr, "Failed to write journal ops to %s\n", file_name);
            unlock_journal();
            fclose(fp);
            return -1;
        }
    }
    unlock_journal();
    fclose(fp);
    return 0;
}

static bool
is_fd_opened_in_journal(int32_t fd, int current_index)
{
    int i;
    for (i = 0; i < current_index; i++) {
        if (g_journal[i].kind == SOCK_OP_OPEN && g_journal[i].fd == fd) {
            return true;
        }
    }
    return false;
}

int
socket_journal_restore(wasm_exec_env_t exec_env, const char *file_prefix)
{
    char file_name[128] = "socket.img";
    FILE *fp;
    int32_t count = 0;
    int i;
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    WASIContext *wasi_ctx = wasm_runtime_get_wasi_ctx(inst);
    struct fd_table *curfds = wasi_ctx ? wasi_ctx->curfds : NULL;
    struct addr_pool *addr_pool = wasi_ctx ? wasi_ctx->addr_pool : NULL;
    struct fd_prestats *prestats = wasi_ctx ? wasi_ctx->prestats : NULL;

    if (file_prefix) {
        str_add_prefix(file_name, (char *)file_prefix);
    }

    fp = fopen(file_name, "rb");
    if (!fp) {
        // If file doesn't exist, we don't have any socket journal to restore, which is fine
        return 0;
    }

    lock_journal();
    if (fread(&count, sizeof(int32_t), 1, fp) != 1) {
        fprintf(stderr, "Failed to read journal count from %s\n", file_name);
        unlock_journal();
        fclose(fp);
        return -1;
    }

    if (count < 0 || count > MAX_SOCK_OPS) {
        fprintf(stderr, "Invalid journal count: %d\n", count);
        unlock_journal();
        fclose(fp);
        return -1;
    }

    if (count > 0) {
        if (fread(g_journal, sizeof(sock_op), count, fp) != (size_t)count) {
            fprintf(stderr, "Failed to read journal ops from %s\n", file_name);
            unlock_journal();
            fclose(fp);
            return -1;
        }
    }
    g_journal_count = count;
    unlock_journal();
    fclose(fp);

    /* Replay runs on the main thread during restore, before any worker thread
     * is re-created and before the guest resumes, so no other thread records
     * into g_journal concurrently here; the loop reads it without the lock.
     * Limitation: the journal only tracks sockets. If a socket fd number was
     * closed and later reused by a non-socket fd (e.g. a file) that is still
     * open at checkpoint time, replaying OPEN for that number would wrongly
     * recreate a socket. Not an issue for the socket-only mROS2 nodes. */
    for (i = 0; i < count; i++) {
        const sock_op *op = &g_journal[i];
        __wasi_errno_t err = __WASI_ESUCCESS;

        switch (op->kind) {
            case SOCK_OP_OPEN: {
                err = wasi_ssp_sock_restore_open(exec_env, curfds, op->fd, op->af, op->socktype, op->protocol);
                if (err != __WASI_ESUCCESS) {
                    fprintf(stderr, "Restore: failed to restore sock_open for fd %d, err %d\n", op->fd, err);
                }
                break;
            }
            case SOCK_OP_BIND: {
                err = wasi_ssp_sock_bind(exec_env, curfds, addr_pool, op->fd, (__wasi_addr_t *)&op->addr);
                if (err != __WASI_ESUCCESS) {
                    fprintf(stderr, "Restore: failed to restore sock_bind for fd %d, err %d\n", op->fd, err);
                }
                break;
            }
            case SOCK_OP_SET_REUSE_ADDR: {
                err = wasmtime_ssp_sock_set_reuse_addr(exec_env, curfds, op->fd, op->val != 0);
                if (err != __WASI_ESUCCESS) {
                    fprintf(stderr, "Restore: failed to restore set_reuse_addr for fd %d, err %d\n", op->fd, err);
                }
                break;
            }
            case SOCK_OP_SET_BROADCAST: {
                err = wasmtime_ssp_sock_set_broadcast(exec_env, curfds, op->fd, op->val != 0);
                if (err != __WASI_ESUCCESS) {
                    fprintf(stderr, "Restore: failed to restore set_broadcast for fd %d, err %d\n", op->fd, err);
                }
                break;
            }
            case SOCK_OP_SET_IP_MULTICAST_LOOP: {
                bool ipv6 = (op->val & 2) != 0;
                bool is_enabled = (op->val & 1) != 0;
                err = wasmtime_ssp_sock_set_ip_multicast_loop(exec_env, curfds, op->fd, ipv6, is_enabled);
                if (err != __WASI_ESUCCESS) {
                    fprintf(stderr, "Restore: failed to restore set_ip_multicast_loop for fd %d, err %d\n", op->fd, err);
                }
                break;
            }
            case SOCK_OP_SET_IP_MULTICAST_TTL: {
                err = wasmtime_ssp_sock_set_ip_multicast_ttl(exec_env, curfds, op->fd, (uint8_t)op->val);
                if (err != __WASI_ESUCCESS) {
                    fprintf(stderr, "Restore: failed to restore set_ip_multicast_ttl for fd %d, err %d\n", op->fd, err);
                }
                break;
            }
            case SOCK_OP_SET_IP_TTL: {
                err = wasmtime_ssp_sock_set_ip_ttl(exec_env, curfds, op->fd, (uint8_t)op->val);
                if (err != __WASI_ESUCCESS) {
                    fprintf(stderr, "Restore: failed to restore set_ip_ttl for fd %d, err %d\n", op->fd, err);
                }
                break;
            }
            case SOCK_OP_ADD_MEMBERSHIP: {
                err = wasmtime_ssp_sock_set_ip_add_membership(exec_env, curfds, op->fd, (__wasi_addr_ip_t *)&op->multiaddr, op->interface);
                if (err != __WASI_ESUCCESS) {
                    fprintf(stderr, "Restore: failed to restore add_membership for fd %d, err %d\n", op->fd, err);
                }
                break;
            }
            case SOCK_OP_DROP_MEMBERSHIP: {
                err = wasmtime_ssp_sock_set_ip_drop_membership(exec_env, curfds, op->fd, (__wasi_addr_ip_t *)&op->multiaddr, op->interface);
                if (err != __WASI_ESUCCESS) {
                    fprintf(stderr, "Restore: failed to restore drop_membership for fd %d, err %d\n", op->fd, err);
                }
                break;
            }
            case SOCK_OP_CLOSE: {
                if (is_fd_opened_in_journal(op->fd, i)) {
                    err = wasmtime_ssp_fd_close(exec_env, curfds, prestats, op->fd);
                    if (err != __WASI_ESUCCESS) {
                        fprintf(stderr, "Restore: failed to restore fd_close for fd %d, err %d\n", op->fd, err);
                    }
                }
                break;
            }
            default:
                fprintf(stderr, "Restore: unknown op kind %d\n", op->kind);
                break;
        }
    }
    return 0;
}
