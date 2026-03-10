#include "platform_common.h"
#include "wasm_export.h"
typedef struct {
    /* app's entry function */
    wasm_function_inst_t start_func;
    /* arg of the app's entry function */
    uint32_t arg;
    /* thread id passed to the app */
    int32 thread_id;
} ThreadStartArg;


