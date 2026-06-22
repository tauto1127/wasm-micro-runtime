#include "platform_common.h"
#include "wasm_export.h"
void *
signal_control_routine(void *arg);
char *
get_file_prefix(int32 thread_id);
void
str_add_prefix(char *file_name, char *file_prefix);

int
wasm_restore_thread(wasm_exec_env_t parent_exec_env, char *file_prefix);

#define MAIN_THREAD_PREFIX "main-"
#define MAX_FILE_NAME_LENGTH 100
