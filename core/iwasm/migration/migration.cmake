set (MIGRATION_DIR ${CMAKE_CURRENT_LIST_DIR})

add_definitions (-DWASM_ENABLE_MIGRATION=1)

include_directories(${MIGRATION_DIR})

# Keep threaded legacy migration implementation as the default build path.
# Funera-synced split sources are staged and enabled only with opt-in.
if (NOT DEFINED WAMR_BUILD_FUNERA_MIGRATION_SYNC)
    set (WAMR_BUILD_FUNERA_MIGRATION_SYNC 0)
endif ()

if (NOT DEFINED WAMR_BUILD_FUNERA_MIGRATION_BRIDGE)
    set (WAMR_BUILD_FUNERA_MIGRATION_BRIDGE 0)
endif ()

if (NOT DEFINED WAMR_FUNERA_WASMIG_API_AVAILABLE)
    set (WAMR_FUNERA_WASMIG_API_AVAILABLE 0)
endif ()

if (NOT DEFINED WAMR_BUILD_FUNERA_WASMIG_LIB)
    set (WAMR_BUILD_FUNERA_WASMIG_LIB 0)
endif ()

add_definitions(-DWAMR_BUILD_FUNERA_MIGRATION_SYNC=${WAMR_BUILD_FUNERA_MIGRATION_SYNC})
add_definitions(-DWAMR_BUILD_FUNERA_MIGRATION_BRIDGE=${WAMR_BUILD_FUNERA_MIGRATION_BRIDGE})

set (MIGRATION_SOURCE
    ${MIGRATION_DIR}/wasm_dispatch.c
    ${MIGRATION_DIR}/wasm_dump.c
    ${MIGRATION_DIR}/wasm_restore.c
)

if (WAMR_BUILD_FUNERA_MIGRATION_SYNC EQUAL 1)
    include(FetchContent)
    FetchContent_Declare(
        wasmig
        GIT_REPOSITORY https://github.com/funera1/wasmig.git
        GIT_TAG main
        GIT_SHALLOW TRUE
        GIT_SUBMODULES ""
        GIT_SUBMODULES_RECURSE FALSE
    )
    FetchContent_GetProperties(wasmig)
    if (NOT wasmig_POPULATED)
        message ("-- Fetching wasmig ..")
        FetchContent_Populate(wasmig)
        include_directories("${wasmig_SOURCE_DIR}/include")
        file (GLOB_RECURSE WASMIG_SOURCE ${wasmig_SOURCE_DIR}/src/*.c)
    endif ()

    if (WAMR_BUILD_FUNERA_WASMIG_LIB EQUAL 1)
        add_subdirectory(${wasmig_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/wasmig-build)
        set (WAMR_FUNERA_WASMIG_API_AVAILABLE 1)
    endif ()

    # Stage 2 sync: compile funera split sources under prefixed symbols.
    # Threaded public APIs are still provided by legacy wasm_dump/restore.
    list (APPEND MIGRATION_SOURCE
        ${MIGRATION_DIR}/wasm_migration_helper.c
        ${MIGRATION_DIR}/wasm_dump_classic.c
        ${MIGRATION_DIR}/wasm_dump_fast.c
        ${MIGRATION_DIR}/wasm_restore_classic.c
        ${MIGRATION_DIR}/wasm_restore_fast.c
        ${WASMIG_SOURCE}
    )
endif ()

if (WAMR_BUILD_FUNERA_MIGRATION_BRIDGE EQUAL 1
    AND WAMR_FUNERA_WASMIG_API_AVAILABLE EQUAL 0)
    message ("-- Funera migration bridge runtime hooks are disabled "
             "(set WAMR_BUILD_FUNERA_WASMIG_LIB=1 with a full wasmig checkout "
             "to enable them)")
endif ()

add_definitions(-DWAMR_FUNERA_WASMIG_API_AVAILABLE=${WAMR_FUNERA_WASMIG_API_AVAILABLE})
