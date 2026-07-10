#ifndef _WASM_SOCKET_JOURNAL_H
#define _WASM_SOCKET_JOURNAL_H

#include "wasmtime_ssp.h"

typedef enum {
    SOCK_OP_OPEN = 1,
    SOCK_OP_BIND,
    SOCK_OP_SET_REUSE_ADDR,
    SOCK_OP_SET_BROADCAST,
    SOCK_OP_SET_IP_MULTICAST_LOOP,
    SOCK_OP_SET_IP_MULTICAST_TTL,
    SOCK_OP_SET_IP_TTL,
    SOCK_OP_ADD_MEMBERSHIP,
    SOCK_OP_DROP_MEMBERSHIP,
    SOCK_OP_CLOSE,
    SOCK_OP_OPEN_PACKET
} sock_op_kind;

/* Keep in sync with WASI_IFNAME_SIZE in wasi_socket_ext.h */
#ifndef SOCK_JOURNAL_IFNAME_SIZE
#define SOCK_JOURNAL_IFNAME_SIZE 16
#endif

typedef struct {
    uint8_t   kind;
    int32_t   fd;                       /* OPEN=結果fd, 他=対象fd */
    int32_t   af;
    int32_t   socktype;                 /* OPEN */
    int32_t   protocol;                 /* OPEN / OPEN_PACKET */
    __wasi_addr_t   addr;                 /* BIND */
    __wasi_addr_ip_t multiaddr;
    uint32_t  interface;                 /* ADD/DROP */
    uint64_t  val;                      /* SET_* の bool/uint8, loop の場合は (ipv6 << 1) | is_enabled */
    int32_t   flags;                    /* OPEN_PACKET */
    char      ifname[SOCK_JOURNAL_IFNAME_SIZE]; /* OPEN_PACKET */
} sock_op;

void socket_journal_record(const sock_op *op);
/* Returns true if the given fd is currently an open socket in the journal
 * (an OPEN not yet cancelled by a later CLOSE of the same fd number). */
bool socket_journal_has_open(int32_t fd);
int  socket_journal_dump(const char *file_prefix);
int  socket_journal_restore(wasm_exec_env_t exec_env, const char *file_prefix);

#endif /* _WASM_SOCKET_JOURNAL_H */
