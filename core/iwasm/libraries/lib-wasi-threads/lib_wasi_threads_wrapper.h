#include "platform_common.h"
#include "wasm_export.h"
typedef struct {
    /* app's entry function */
    wasm_function_inst_t start_func;
    /* arg of the app's entry function */
    uint32 arg;
    /* thread id passed to the app */
    int32 thread_id;
} ThreadStartArg;

#define THREAD_START_FUNCTION "wasi_thread_start"

void deallocate_thread_id(int32 thread_id);

void *thread_start(void *arg);
