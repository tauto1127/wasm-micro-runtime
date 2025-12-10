void* signal_control_routine(void *arg);

#if WASM_ENABLE_CR != 0
#define MAIN_THREAD_PREFIX "main-"
#define MAX_FILE_NAME_LENGTH 100
#endif
