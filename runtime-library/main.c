/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bh_platform.h"
#include "bh_read_file.h"
#include "wasm_export.h"
#include "libdyntype_export.h"

extern uint32_t
get_libdyntype_symbols(char **p_module_name, NativeSymbol **p_native_symbols);

extern uint32_t
get_lib_console_symbols(char **p_module_name, NativeSymbol **p_native_symbols);

extern uint32_t
get_lib_array_symbols(char **p_module_name, NativeSymbol **p_native_symbols);

extern uint32_t
get_lib_timer_symbols(char **p_module_name, NativeSymbol **p_native_symbols);

extern uint32_t
get_struct_indirect_symbols(char **p_module_name, NativeSymbol **p_native_symbols);

extern dyn_value_t
dyntype_callback_wasm_dispatcher(void *exec_env_v, dyn_ctx_t ctx, void *vfunc,
                                 dyn_value_t this_obj, int argc,
                                 dyn_value_t *args);

#if BH_HAS_DLFCN
#include <dlfcn.h>
#endif

static int app_argc;
static char **app_argv;

/* clang-format off */
static int
print_help()
{
    printf("Usage: iwasm [-options] wasm_file [args...]\n");
    printf("options:\n");
    printf("  -f|--function name       Specify a function name of the module to run rather\n"
           "                           than main\n");
#if WASM_ENABLE_LOG != 0
    printf("  -v=n                     Set log verbose level (0 to 5, default is 2) larger\n"
           "                           level with more log\n");
#endif
#if WASM_ENABLE_INTERP != 0
    printf("  --interp                 Run the wasm app with interpreter mode\n");
#endif
#if WASM_ENABLE_FAST_JIT != 0
    printf("  --fast-jit               Run the wasm app with fast jit mode\n");
#endif
#if WASM_ENABLE_JIT != 0
    printf("  --llvm-jit               Run the wasm app with llvm jit mode\n");
#endif
#if WASM_ENABLE_JIT != 0 && WASM_ENABLE_FAST_JIT != 0 && WASM_ENABLE_LAZY_JIT != 0
    printf("  --multi-tier-jit         Run the wasm app with multi-tier jit mode\n");
#endif
    printf("  --stack-size=n           Set maximum stack size in bytes, default is 64 KB\n");
    printf("  --heap-size=n            Set maximum heap size in bytes, default is 16 KB\n");
#if WASM_ENABLE_FAST_JIT != 0
    printf("  --jit-codecache-size=n   Set fast jit maximum code cache size in bytes,\n");
    printf("                           default is %u KB\n", FAST_JIT_DEFAULT_CODE_CACHE_SIZE / 1024);
#endif
#if WASM_ENABLE_GC != 0
    printf("  --gc-heap-size=n         Set maximum gc heap size in bytes,\n");
    printf("                           default is %u KB\n", GC_HEAP_SIZE_DEFAULT / 1024);
#endif
#if WASM_ENABLE_JIT != 0
    printf("  --llvm-jit-size-level=n  Set LLVM JIT size level, default is 3\n");
    printf("  --llvm-jit-opt-level=n   Set LLVM JIT optimization level, default is 3\n");
#endif
    printf("  --repl                   Start a very simple REPL (read-eval-print-loop) mode\n"
           "                           that runs commands in the form of \"FUNC ARG...\"\n");
#if WASM_ENABLE_LIBC_WASI != 0
    printf("  --env=<env>              Pass wasi environment variables with \"key=value\"\n");
    printf("                           to the program, for example:\n");
    printf("                             --env=\"key1=value1\" --env=\"key2=value2\"\n");
    printf("  --dir=<dir>              Grant wasi access to the given host directories\n");
    printf("                           to the program, for example:\n");
    printf("                             --dir=<dir1> --dir=<dir2>\n");
    printf("  --addr-pool=<addrs>      Grant wasi access to the given network addresses in\n");
    printf("                           CIRD notation to the program, seperated with ',',\n");
    printf("                           for example:\n");
    printf("                             --addr-pool=1.2.3.4/15,2.3.4.5/16\n");
    printf("  --allow-resolve=<domain> Allow the lookup of the specific domain name or domain\n");
    printf("                           name suffixes using a wildcard, for example:\n");
    printf("                           --allow-resolve=example.com # allow the lookup of the specific domain\n");
    printf("                           --allow-resolve=*.example.com # allow the lookup of all subdomains\n");
    printf("                           --allow-resolve=* # allow any lookup\n");
#endif
#if BH_HAS_DLFCN
    printf("  --native-lib=<lib>       Register native libraries to the WASM module, which\n");
    printf("                           are shared object (.so) files, for example:\n");
    printf("                             --native-lib=test1.so --native-lib=test2.so\n");
#endif
#if WASM_ENABLE_MULTI_MODULE != 0
    printf("  --module-path=<path>     Indicate a module search path. default is current\n"
           "                           directory('./')\n");
#endif
#if WASM_ENABLE_LIB_PTHREAD != 0 || WASM_ENABLE_LIB_WASI_THREADS != 0
    printf("  --max-threads=n          Set maximum thread number per cluster, default is 4\n");
#endif
#if WASM_ENABLE_DEBUG_INTERP != 0
    printf("  -g=ip:port               Set the debug sever address, default is debug disabled\n");
    printf("                             if port is 0, then a random port will be used\n");
#endif
    printf("  --version                Show version information\n");
    return 1;
}
/* clang-format on */

static const void *
app_instance_main(wasm_module_inst_t module_inst)
{
    const char *exception;

    wasm_application_execute_main(module_inst, app_argc, app_argv);
    exception = wasm_runtime_get_exception(module_inst);
    return exception;
}

static const void *
app_instance_func(wasm_module_inst_t module_inst, const char *func_name)
{
    wasm_application_execute_func(module_inst, func_name, app_argc - 1,
                                  app_argv + 1);
    /* The result of wasm function or exception info was output inside
       wasm_application_execute_func(), here we don't output them again. */
    return wasm_runtime_get_exception(module_inst);
}

/**
 * Split a space separated strings into an array of strings
 * Returns NULL on failure
 * Memory must be freed by caller
 * Based on: http://stackoverflow.com/a/11198630/471795
 */
static char **
split_string(char *str, int *count)
{
    char **res = NULL, **res1;
    char *p;
    int idx = 0;

    /* split string and append tokens to 'res' */
    do {
        p = strtok(str, " ");
        str = NULL;
        res1 = res;
        res = (char **)realloc(res1, sizeof(char *) * (uint32)(idx + 1));
        if (res == NULL) {
            free(res1);
            return NULL;
        }
        res[idx++] = p;
    } while (p);

    /**
     * Due to the function name,
     * res[0] might contain a '\' to indicate a space
     * func\name -> func name
     */
    p = strchr(res[0], '\\');
    while (p) {
        *p = ' ';
        p = strchr(p, '\\');
    }

    if (count) {
        *count = idx - 1;
    }
    return res;
}

static void *
app_instance_repl(wasm_module_inst_t module_inst)
{
    char *cmd = NULL;
    size_t len = 0;
    ssize_t n;

    while ((printf("webassembly> "), fflush(stdout),
            n = getline(&cmd, &len, stdin))
           != -1) {
        bh_assert(n > 0);
        if (cmd[n - 1] == '\n') {
            if (n == 1)
                continue;
            else
                cmd[n - 1] = '\0';
        }
        if (!strcmp(cmd, "__exit__")) {
            printf("exit repl mode\n");
            break;
        }
        app_argv = split_string(cmd, &app_argc);
        if (app_argv == NULL) {
            LOG_ERROR("Wasm prepare param failed: split string failed.\n");
            break;
        }
        if (app_argc != 0) {
            wasm_application_execute_func(module_inst, app_argv[0],
                                          app_argc - 1, app_argv + 1);
        }
        free(app_argv);
    }
    free(cmd);
    return NULL;
}

#if WASM_ENABLE_LIBC_WASI != 0
static bool
validate_env_str(char *env)
{
    char *p = env;
    int key_len = 0;

    while (*p != '\0' && *p != '=') {
        key_len++;
        p++;
    }

    if (*p != '=' || key_len == 0)
        return false;

    return true;
}
#endif

#if BH_HAS_DLFCN
typedef uint32_t (*get_native_lib_func)(char **p_module_name,
                                      NativeSymbol **p_native_symbols);

static uint32
load_and_register_native_libs(const char **native_lib_list,
                              uint32_t native_lib_count,
                              void **native_handle_list)
{
    uint32_t i, native_handle_count = 0, n_native_symbols;
    NativeSymbol *native_symbols;
    char *module_name;
    void *handle;

    for (i = 0; i < native_lib_count; i++) {
        /* open the native library */
        if (!(handle = dlopen(native_lib_list[i], RTLD_NOW | RTLD_GLOBAL))
            && !(handle = dlopen(native_lib_list[i], RTLD_LAZY))) {
            LOG_WARNING("warning: failed to load native library %s",
                        native_lib_list[i]);
            continue;
        }

        /* lookup get_native_lib func */
        get_native_lib_func get_native_lib = dlsym(handle, "get_native_lib");
        if (!get_native_lib) {
            LOG_WARNING("warning: failed to lookup `get_native_lib` function "
                        "from native lib %s",
                        native_lib_list[i]);
            dlclose(handle);
            continue;
        }

        n_native_symbols = get_native_lib(&module_name, &native_symbols);

        /* register native symbols */
        if (!(n_native_symbols > 0 && module_name && native_symbols
              && wasm_runtime_register_natives(module_name, native_symbols,
                                               n_native_symbols))) {
            LOG_WARNING("warning: failed to register native lib %s",
                        native_lib_list[i]);
            dlclose(handle);
            continue;
        }

        native_handle_list[native_handle_count++] = handle;
    }

    return native_handle_count;
}

static void
unregister_and_unload_native_libs(uint32_t native_lib_count,
                                  void **native_handle_list)
{
    uint32_t i, n_native_symbols;
    NativeSymbol *native_symbols;
    char *module_name;
    void *handle;

    for (i = 0; i < native_lib_count; i++) {
        handle = native_handle_list[i];

        /* lookup get_native_lib func */
        get_native_lib_func get_native_lib = dlsym(handle, "get_native_lib");
        if (!get_native_lib) {
            LOG_WARNING("warning: failed to lookup `get_native_lib` function "
                        "from native lib %p",
                        handle);
            continue;
        }

        n_native_symbols = get_native_lib(&module_name, &native_symbols);
        if (n_native_symbols == 0 || module_name == NULL
            || native_symbols == NULL) {
            LOG_WARNING("warning: get_native_lib returned different values for "
                        "native lib %p",
                        handle);
            continue;
        }

        /* unregister native symbols */
        if (!wasm_runtime_unregister_natives(module_name, native_symbols)) {
            LOG_WARNING("warning: failed to unregister native lib %p", handle);
            continue;
        }

        dlclose(handle);
    }
}
#endif /* BH_HAS_DLFCN */

#if WASM_ENABLE_MULTI_MODULE != 0
static char *
handle_module_path(const char *module_path)
{
    /* next character after = */
    return (strchr(module_path, '=')) + 1;
}

static char *module_search_path = ".";

static bool
module_reader_callback(const char *module_name, uint8 **p_buffer,
                       uint32_t *p_size)
{
    const char *format = "%s/%s.wasm";
    int sz = strlen(module_search_path) + strlen("/") + strlen(module_name)
             + strlen(".wasm") + 1;
    char *wasm_file_name = BH_MALLOC(sz);
    if (!wasm_file_name) {
        return false;
    }

    snprintf(wasm_file_name, sz, format, module_search_path, module_name);

    *p_buffer = (uint8_t *)bh_read_file_to_buffer(wasm_file_name, p_size);

    wasm_runtime_free(wasm_file_name);
    return *p_buffer != NULL;
}

static void
moudle_destroyer(uint8 *buffer, uint32_t size)
{
    if (!buffer) {
        return;
    }

    wasm_runtime_free(buffer);
    buffer = NULL;
}
#endif /* WASM_ENABLE_MULTI_MODULE */

#if WASM_ENABLE_GLOBAL_HEAP_POOL != 0
static char global_heap_buf[WASM_GLOBAL_HEAP_SIZE] = { 0 };
#endif

int
events_poll(wasm_exec_env_t exec_env)
{
    /* TODO: not detect macro tasks yet */
    return -1;
}

void
execute_micro_tasks(wasm_exec_env_t exec_env, dyn_ctx_t ctx)
{
    int err;

    for (;;) {
        /* execute the pending jobs */
        for (;;) {
            err = dyntype_execute_pending_jobs(ctx);
            if (err <= 0) {
                if (err < 0) {
                    dyntype_dump_error(ctx);
                }
                break;
            }
        }

        if (events_poll(exec_env))
            break;
    }
}

int
main(int argc, char *argv[])
{
    dyn_ctx_t dyn_ctx = NULL;
    int32 ret = -1;
    char *wasm_file = NULL;
    const char *func_name = NULL;
    uint8 *wasm_file_buf = NULL;
    uint32_t wasm_file_size;
    uint32_t stack_size = 8 * 1024, heap_size = 4 * 1024;
#if WASM_ENABLE_FAST_JIT != 0
    uint32_t jit_code_cache_size = FAST_JIT_DEFAULT_CODE_CACHE_SIZE;
#endif
#if WASM_ENABLE_GC != 0
    uint32_t gc_heap_size = GC_HEAP_SIZE_DEFAULT;
#endif
#if WASM_ENABLE_JIT != 0
    uint32_t llvm_jit_size_level = 3;
    uint32_t llvm_jit_opt_level = 3;
#endif
    wasm_module_t wasm_module = NULL;
    wasm_module_inst_t wasm_module_inst = NULL;
    wasm_exec_env_t exec_env = NULL;
    wasm_function_inst_t start_func = NULL;
    RunningMode running_mode = 0;
    RuntimeInitArgs init_args;
    char error_buf[128] = { 0 };
#if WASM_ENABLE_LOG != 0
    int log_verbose_level = 2;
#endif
    bool is_repl_mode = false;
    bool is_xip_file = false;
    const char *exception = NULL;
#if WASM_ENABLE_LIBC_WASI != 0
    const char *dir_list[8] = { NULL };
    uint32_t dir_list_size = 0;
    const char *env_list[8] = { NULL };
    uint32_t env_list_size = 0;
    const char *addr_pool[8] = { NULL };
    uint32_t addr_pool_size = 0;
    const char *ns_lookup_pool[8] = { NULL };
    uint32_t ns_lookup_pool_size = 0;
#endif
#if BH_HAS_DLFCN
    const char *native_lib_list[8] = { NULL };
    uint32_t native_lib_count = 0;
    void *native_handle_list[8] = { NULL };
    uint32_t native_handle_count = 0;
#endif
#if WASM_ENABLE_DEBUG_INTERP != 0
    char *ip_addr = NULL;
    int instance_port = 0;
#endif

    /* Process options. */
    for (argc--, argv++; argc > 0 && argv[0][0] == '-'; argc--, argv++) {
        if (!strcmp(argv[0], "-f") || !strcmp(argv[0], "--function")) {
            argc--, argv++;
            if (argc < 2) {
                return print_help();
            }
            func_name = argv[0];
        }
#if WASM_ENABLE_INTERP != 0
        else if (!strcmp(argv[0], "--interp")) {
            running_mode = Mode_Interp;
        }
#endif
#if WASM_ENABLE_FAST_JIT != 0
        else if (!strcmp(argv[0], "--fast-jit")) {
            running_mode = Mode_Fast_JIT;
        }
#endif
#if WASM_ENABLE_JIT != 0
        else if (!strcmp(argv[0], "--llvm-jit")) {
            running_mode = Mode_LLVM_JIT;
        }
#endif
#if WASM_ENABLE_JIT != 0 && WASM_ENABLE_FAST_JIT != 0 \
    && WASM_ENABLE_LAZY_JIT != 0
        else if (!strcmp(argv[0], "--multi-tier-jit")) {
            running_mode = Mode_Multi_Tier_JIT;
        }
#endif
#if WASM_ENABLE_LOG != 0
        else if (!strncmp(argv[0], "-v=", 3)) {
            log_verbose_level = atoi(argv[0] + 3);
            if (log_verbose_level < 0 || log_verbose_level > 5)
                return print_help();
        }
#endif
        else if (!strcmp(argv[0], "--repl")) {
            is_repl_mode = true;
        }
        else if (!strncmp(argv[0], "--stack-size=", 13)) {
            if (argv[0][13] == '\0')
                return print_help();
            stack_size = atoi(argv[0] + 13);
        }
        else if (!strncmp(argv[0], "--heap-size=", 12)) {
            if (argv[0][12] == '\0')
                return print_help();
            heap_size = atoi(argv[0] + 12);
        }
#if WASM_ENABLE_FAST_JIT != 0
        else if (!strncmp(argv[0], "--jit-codecache-size=", 21)) {
            if (argv[0][21] == '\0')
                return print_help();
            jit_code_cache_size = atoi(argv[0] + 21);
        }
#endif
#if WASM_ENABLE_GC != 0
        else if (!strncmp(argv[0], "--gc-heap-size=", 15)) {
            if (argv[0][21] == '\0')
                return print_help();
            gc_heap_size = atoi(argv[0] + 15);
        }
#endif
#if WASM_ENABLE_JIT != 0
        else if (!strncmp(argv[0], "--llvm-jit-size-level=", 22)) {
            if (argv[0][22] == '\0')
                return print_help();
            llvm_jit_size_level = atoi(argv[0] + 22);
            if (llvm_jit_size_level < 1) {
                printf("LLVM JIT size level shouldn't be smaller than 1, "
                       "setting it to 1\n");
                llvm_jit_size_level = 1;
            }
            else if (llvm_jit_size_level > 3) {
                printf("LLVM JIT size level shouldn't be greater than 3, "
                       "setting it to 3\n");
                llvm_jit_size_level = 3;
            }
        }
        else if (!strncmp(argv[0], "--llvm-jit-opt-level=", 21)) {
            if (argv[0][21] == '\0')
                return print_help();
            llvm_jit_opt_level = atoi(argv[0] + 21);
            if (llvm_jit_opt_level < 1) {
                printf("LLVM JIT opt level shouldn't be smaller than 1, "
                       "setting it to 1\n");
                llvm_jit_opt_level = 1;
            }
            else if (llvm_jit_opt_level > 3) {
                printf("LLVM JIT opt level shouldn't be greater than 3, "
                       "setting it to 3\n");
                llvm_jit_opt_level = 3;
            }
        }
#endif
#if WASM_ENABLE_LIBC_WASI != 0
        else if (!strncmp(argv[0], "--dir=", 6)) {
            if (argv[0][6] == '\0')
                return print_help();
            if (dir_list_size >= sizeof(dir_list) / sizeof(char *)) {
                printf("Only allow max dir number %d\n",
                       (int)(sizeof(dir_list) / sizeof(char *)));
                return 1;
            }
            dir_list[dir_list_size++] = argv[0] + 6;
        }
        else if (!strncmp(argv[0], "--env=", 6)) {
            char *tmp_env;

            if (argv[0][6] == '\0')
                return print_help();
            if (env_list_size >= sizeof(env_list) / sizeof(char *)) {
                printf("Only allow max env number %d\n",
                       (int)(sizeof(env_list) / sizeof(char *)));
                return 1;
            }
            tmp_env = argv[0] + 6;
            if (validate_env_str(tmp_env))
                env_list[env_list_size++] = tmp_env;
            else {
                printf("Wasm parse env string failed: expect \"key=value\", "
                       "got \"%s\"\n",
                       tmp_env);
                return print_help();
            }
        }
        /* TODO: parse the configuration file via --addr-pool-file */
        else if (!strncmp(argv[0], "--addr-pool=", strlen("--addr-pool="))) {
            /* like: --addr-pool=100.200.244.255/30 */
            char *token = NULL;

            if ('\0' == argv[0][12])
                return print_help();

            token = strtok(argv[0] + strlen("--addr-pool="), ",");
            while (token) {
                if (addr_pool_size >= sizeof(addr_pool) / sizeof(char *)) {
                    printf("Only allow max address number %d\n",
                           (int)(sizeof(addr_pool) / sizeof(char *)));
                    return 1;
                }

                addr_pool[addr_pool_size++] = token;
                token = strtok(NULL, ";");
            }
        }
        else if (!strncmp(argv[0], "--allow-resolve=", 16)) {
            if (argv[0][16] == '\0')
                return print_help();
            if (ns_lookup_pool_size
                >= sizeof(ns_lookup_pool) / sizeof(ns_lookup_pool[0])) {
                printf(
                    "Only allow max ns lookup number %d\n",
                    (int)(sizeof(ns_lookup_pool) / sizeof(ns_lookup_pool[0])));
                return 1;
            }
            ns_lookup_pool[ns_lookup_pool_size++] = argv[0] + 16;
        }
#endif /* WASM_ENABLE_LIBC_WASI */
#if BH_HAS_DLFCN
        else if (!strncmp(argv[0], "--native-lib=", 13)) {
            if (argv[0][13] == '\0')
                return print_help();
            if (native_lib_count >= sizeof(native_lib_list) / sizeof(char *)) {
                printf("Only allow max native lib number %d\n",
                       (int)(sizeof(native_lib_list) / sizeof(char *)));
                return 1;
            }
            native_lib_list[native_lib_count++] = argv[0] + 13;
        }
#endif
#if WASM_ENABLE_MULTI_MODULE != 0
        else if (!strncmp(argv[0],
                          "--module-path=", strlen("--module-path="))) {
            module_search_path = handle_module_path(argv[0]);
            if (!strlen(module_search_path)) {
                return print_help();
            }
        }
#endif
#if WASM_ENABLE_LIB_PTHREAD != 0 || WASM_ENABLE_LIB_WASI_THREADS != 0
        else if (!strncmp(argv[0], "--max-threads=", 14)) {
            if (argv[0][14] == '\0')
                return print_help();
            wasm_runtime_set_max_thread_num(atoi(argv[0] + 14));
        }
#endif
#if WASM_ENABLE_DEBUG_INTERP != 0
        else if (!strncmp(argv[0], "-g=", 3)) {
            char *port_str = strchr(argv[0] + 3, ':');
            char *port_end;
            if (port_str == NULL)
                return print_help();
            *port_str = '\0';
            instance_port = strtoul(port_str + 1, &port_end, 10);
            if (port_str[1] == '\0' || *port_end != '\0')
                return print_help();
            ip_addr = argv[0] + 3;
        }
#endif
        else if (!strncmp(argv[0], "--version", 9)) {
            uint32_t major, minor, patch;
            wasm_runtime_get_version(&major, &minor, &patch);
            printf("iwasm %" PRIu32 ".%" PRIu32 ".%" PRIu32 "\n", major, minor,
                   patch);
            return 0;
        }
        else
            return print_help();
    }

    if (argc == 0)
        return print_help();

    wasm_file = argv[0];
    app_argc = argc;
    app_argv = argv;

    memset(&init_args, 0, sizeof(RuntimeInitArgs));

    init_args.running_mode = running_mode;
#if WASM_ENABLE_GLOBAL_HEAP_POOL != 0
    init_args.mem_alloc_type = Alloc_With_Pool;
    init_args.mem_alloc_option.pool.heap_buf = global_heap_buf;
    init_args.mem_alloc_option.pool.heap_size = sizeof(global_heap_buf);
#else
    init_args.mem_alloc_type = Alloc_With_Allocator;
    init_args.mem_alloc_option.allocator.malloc_func = malloc;
    init_args.mem_alloc_option.allocator.realloc_func = realloc;
    init_args.mem_alloc_option.allocator.free_func = free;
#endif

#if WASM_ENABLE_FAST_JIT != 0
    init_args.fast_jit_code_cache_size = jit_code_cache_size;
#endif

#if WASM_ENABLE_GC != 0
    init_args.gc_heap_size = gc_heap_size;
#endif

#if WASM_ENABLE_JIT != 0
    init_args.llvm_jit_size_level = llvm_jit_size_level;
    init_args.llvm_jit_opt_level = llvm_jit_opt_level;
#endif

#if WASM_ENABLE_DEBUG_INTERP != 0
    init_args.instance_port = instance_port;
    if (ip_addr)
        strcpy(init_args.ip_addr, ip_addr);
#endif

    /* initialize runtime environment */
    if (!wasm_runtime_full_init(&init_args)) {
        printf("Init runtime environment failed.\n");
        return -1;
    }

    /* initialize dyntype context and set callback dispatcher */
    dyn_ctx = dyntype_context_init();
    dyntype_set_callback_dispatcher(dyntype_callback_wasm_dispatcher);

#if WASM_ENABLE_LOG != 0
    bh_log_set_verbose_level(log_verbose_level);
#endif

#if BH_HAS_DLFCN
    native_handle_count = load_and_register_native_libs(
        native_lib_list, native_lib_count, native_handle_list);
#endif

    /* Register APIs required by ts2wasm */
    NativeSymbol *native_symbols;
    char *module_name;
    uint32_t symbol_count;

    symbol_count = get_libdyntype_symbols(&module_name, &native_symbols);
    if (!wasm_runtime_register_natives(module_name, native_symbols,
                                       symbol_count)) {
        printf("Register libdyntype APIs failed.\n");
        goto fail1;
    }

    symbol_count = get_lib_console_symbols(&module_name, &native_symbols);
    if (!wasm_runtime_register_natives(module_name, native_symbols,
                                       symbol_count)) {
        printf("Register stdlib APIs failed.\n");
        goto fail1;
    }

    symbol_count = get_lib_array_symbols(&module_name, &native_symbols);
    if (!wasm_runtime_register_natives(module_name, native_symbols,
                                       symbol_count)) {
        printf("Register stdlib APIs failed.\n");
        goto fail1;
    }

    symbol_count = get_lib_timer_symbols(&module_name, &native_symbols);
    if (!wasm_runtime_register_natives(module_name, native_symbols,
                                       symbol_count)) {
        printf("Register stdlib APIs failed.\n");
        goto fail1;
    }

    symbol_count = get_struct_indirect_symbols(&module_name, &native_symbols);
    if (!wasm_runtime_register_natives(module_name, native_symbols,
                                       symbol_count)) {
        printf("Register struct-dyn APIs failed.\n");
        goto fail1;
    }

    /* load WASM byte buffer from WASM bin file */
    if (!(wasm_file_buf =
              (uint8 *)bh_read_file_to_buffer(wasm_file, &wasm_file_size)))
        goto fail1;

#if WASM_ENABLE_AOT != 0
    if (wasm_runtime_is_xip_file(wasm_file_buf, wasm_file_size)) {
        uint8 *wasm_file_mapped;
        int map_prot = MMAP_PROT_READ | MMAP_PROT_WRITE | MMAP_PROT_EXEC;
        int map_flags = MMAP_MAP_32BIT;

        if (!(wasm_file_mapped =
                  os_mmap(NULL, (uint32)wasm_file_size, map_prot, map_flags))) {
            printf("mmap memory failed\n");
            wasm_runtime_free(wasm_file_buf);
            goto fail1;
        }

        bh_memcpy_s(wasm_file_mapped, wasm_file_size, wasm_file_buf,
                    wasm_file_size);
        wasm_runtime_free(wasm_file_buf);
        wasm_file_buf = wasm_file_mapped;
        is_xip_file = true;
    }
#endif

#if WASM_ENABLE_MULTI_MODULE != 0
    wasm_runtime_set_module_reader(module_reader_callback, moudle_destroyer);
#endif

    /* load WASM module */
    if (!(wasm_module = wasm_runtime_load(wasm_file_buf, wasm_file_size,
                                          error_buf, sizeof(error_buf)))) {
        printf("%s\n", error_buf);
        goto fail2;
    }

#if WASM_ENABLE_LIBC_WASI != 0
    wasm_runtime_set_wasi_args(wasm_module, dir_list, dir_list_size, NULL, 0,
                               env_list, env_list_size, argv, argc);

    wasm_runtime_set_wasi_addr_pool(wasm_module, addr_pool, addr_pool_size);
    wasm_runtime_set_wasi_ns_lookup_pool(wasm_module, ns_lookup_pool,
                                         ns_lookup_pool_size);
#endif

    /* instantiate the module */
    if (!(wasm_module_inst =
              wasm_runtime_instantiate(wasm_module, stack_size, heap_size,
                                       error_buf, sizeof(error_buf)))) {
        printf("%s\n", error_buf);
        goto fail3;
    }

    exec_env = wasm_runtime_get_exec_env_singleton(wasm_module_inst);
    if (exec_env == NULL) {
        printf("%s\n", wasm_runtime_get_exception(wasm_module_inst));
    }

#if WASM_ENABLE_DEBUG_INTERP != 0
    if (ip_addr != NULL) {
        uint32_t debug_port;
        debug_port = wasm_runtime_start_debug_instance(exec_env);
        if (debug_port == 0) {
            printf("Failed to start debug instance\n");
            goto fail4;
        }
    }
#endif

    ret = 0;

    start_func = wasm_runtime_lookup_function(wasm_module_inst, "_entry", NULL);
    if (!start_func) {
        printf("%s\n", "Missing '_entry' function in wasm module\n");
        goto fail4;
    }
    if (!wasm_runtime_call_wasm(exec_env, start_func, 0, NULL)) {
        printf("%s\n", wasm_runtime_get_exception(wasm_module_inst));
        goto fail4;
    }

    if (is_repl_mode) {
        app_instance_repl(wasm_module_inst);
    }
    else if (func_name) {
        exception = app_instance_func(wasm_module_inst, func_name);
    }
    else {
        exception = app_instance_main(wasm_module_inst);
    }

    if (exception) {
        ret = 1;
        printf("%s\n", exception);
    }

#if WASM_ENABLE_LIBC_WASI != 0
    if (ret == 0) {
        /* propagate wasi exit code. */
        ret = wasm_runtime_get_wasi_exit_code(wasm_module_inst);
    }
#endif

    /* run micro tasks */
    execute_micro_tasks(exec_env, dyn_ctx);

fail4:
    /* destroy the module instance */
    wasm_runtime_deinstantiate(wasm_module_inst);

fail3:
    /* unload the module */
    wasm_runtime_unload(wasm_module);

fail2:
    /* free the file buffer */
    if (!is_xip_file) {
        wasm_runtime_free(wasm_file_buf);
    }
#if WASM_ENABLE_AOT != 0
    else {
        os_munmap(wasm_file_buf, wasm_file_size);
    }
#endif

fail1:
#if BH_HAS_DLFCN
    /* unload the native libraries */
    unregister_and_unload_native_libs(native_handle_count, native_handle_list);
#endif

    /* destroy dynamic ctx */
    dyntype_context_destroy(dyn_ctx);

    /* destroy runtime environment */
    wasm_runtime_destroy();

    return ret;
}
