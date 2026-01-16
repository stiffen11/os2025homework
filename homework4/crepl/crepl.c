#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <dlfcn.h>
#include <errno.h>

#define MAX_FUNCTIONS 100
#define MAX_CODE_LEN 4096
#define TEMP_DIR "/tmp/"

// 函数信息结构
typedef struct {
    char name[64];
    void *handle;  // dlopen返回的句柄
    char library_path[256];  // 库文件路径
} function_info;

function_info functions[MAX_FUNCTIONS];
int function_count = 0;
int expression_counter = 0;  // 表达式计数器，用于生成唯一函数名

// 编译代码为共享库
bool compile_code(const char* code, const char* output_path) {
    // 创建临时源文件
    char source_path[256];
    snprintf(source_path, sizeof(source_path), "%screpl_%d.c", TEMP_DIR, getpid());
    
    FILE *fp = fopen(source_path, "w");
    if (!fp) {
        perror("Failed to create source file");
        return false;
    }
    
    // 写入代码
    fprintf(fp, "%s\n", code);
    fclose(fp);
    
    // 编译为共享库
    char compile_cmd[512];
    snprintf(compile_cmd, sizeof(compile_cmd),
             "gcc -fPIC -shared -Wno-implicit-function-declaration -o %s %s 2>&1",
             output_path, source_path);
    
    // 执行编译命令
    FILE *pipe = popen(compile_cmd, "r");
    if (!pipe) {
        remove(source_path);
        return false;
    }
    
    // 读取编译输出（如果有错误）
    char buffer[256];
    bool has_error = false;
    while (fgets(buffer, sizeof(buffer), pipe)) {
        if (strstr(buffer, "error:") || strstr(buffer, "Error:")) {
            has_error = true;
        }
    }
    
    int status = pclose(pipe);
    remove(source_path);
    
    return (status == 0 && !has_error);
}

// 查找函数定义
function_info* find_function(const char* name) {
    for (int i = 0; i < function_count; i++) {
        if (strcmp(functions[i].name, name) == 0) {
            return &functions[i];
        }
    }
    return NULL;
}

// 从函数定义中提取函数名
bool extract_function_name(const char* function_def, char* name, size_t name_len) {
    // 跳过 "int "
    const char *p = function_def + 4;  // 跳过 "int "
    
    // 查找函数名开始位置
    while (*p && (*p == ' ' || *p == '\t')) p++;
    
    // 提取函数名（到 '(' 为止）
    const char *start = p;
    while (*p && *p != '(') p++;
    
    if (p == start) return false;
    
    size_t len = p - start;
    if (len >= name_len) len = name_len - 1;
    
    strncpy(name, start, len);
    name[len] = '\0';
    
    return true;
}

// Compile a function definition and load it
bool compile_and_load_function(const char* function_def) {
    // 提取函数名
    char func_name[64];
    if (!extract_function_name(function_def, func_name, sizeof(func_name))) {
        fprintf(stderr, "Failed to extract function name\n");
        return false;
    }
    
    // 检查是否已存在同名函数
    if (find_function(func_name) != NULL) {
        // 可以覆盖，先卸载旧的
        for (int i = 0; i < function_count; i++) {
            if (strcmp(functions[i].name, func_name) == 0) {
                if (functions[i].handle) {
                    dlclose(functions[i].handle);
                }
                remove(functions[i].library_path);
                
                // 移动后面的函数向前
                for (int j = i; j < function_count - 1; j++) {
                    functions[j] = functions[j + 1];
                }
                function_count--;
                break;
            }
        }
    }
    
    // 生成库文件路径
    char lib_path[256];
    snprintf(lib_path, sizeof(lib_path), "%screpl_func_%d_%d.so", 
             TEMP_DIR, getpid(), function_count);
    
    // 编译代码
    if (!compile_code(function_def, lib_path)) {
        fprintf(stderr, "Compilation failed for function: %s\n", func_name);
        return false;
    }
    
    // 加载共享库
    void *handle = dlopen(lib_path, RTLD_LAZY | RTLD_GLOBAL);
    if (!handle) {
        fprintf(stderr, "Failed to load library: %s\n", dlerror());
        remove(lib_path);
        return false;
    }
    
    // 保存函数信息
    strncpy(functions[function_count].name, func_name, sizeof(functions[function_count].name) - 1);
    functions[function_count].name[sizeof(functions[function_count].name) - 1] = '\0';
    functions[function_count].handle = handle;
    strncpy(functions[function_count].library_path, lib_path, sizeof(functions[function_count].library_path) - 1);
    functions[function_count].library_path[sizeof(functions[function_count].library_path) - 1] = '\0';
    function_count++;
    
    return true;
}

// Evaluate an expression
bool evaluate_expression(const char* expression, int* result) {
    // 为表达式生成包装函数
    expression_counter++;
    
    // 生成包装函数代码
    char wrapper_code[MAX_CODE_LEN];
    snprintf(wrapper_code, sizeof(wrapper_code),
             "int __expr_wrapper_%d() { return (%s); }",
             expression_counter, expression);
    
    // 生成库文件路径
    char lib_path[256];
    snprintf(lib_path, sizeof(lib_path), "%screpl_expr_%d_%d.so", 
             TEMP_DIR, getpid(), expression_counter);
    
    // 编译包装函数
    if (!compile_code(wrapper_code, lib_path)) {
        fprintf(stderr, "Compilation failed for expression\n");
        return false;
    }
    
    // 加载共享库
    void *handle = dlopen(lib_path, RTLD_LAZY);
    if (!handle) {
        fprintf(stderr, "Failed to load expression library: %s\n", dlerror());
        remove(lib_path);
        return false;
    }
    
    // 查找包装函数
    char func_name[64];
    snprintf(func_name, sizeof(func_name), "__expr_wrapper_%d", expression_counter);
    
    int (*wrapper_func)(void) = dlsym(handle, func_name);
    if (!wrapper_func) {
        fprintf(stderr, "Failed to find wrapper function: %s\n", dlerror());
        dlclose(handle);
        remove(lib_path);
        return false;
    }
    
    // 执行包装函数获取结果
    *result = wrapper_func();
    
    // 清理
    dlclose(handle);
    remove(lib_path);
    
    return true;
}

// 清理函数
void cleanup() {
    for (int i = 0; i < function_count; i++) {
        if (functions[i].handle) {
            dlclose(functions[i].handle);
        }
        remove(functions[i].library_path);
    }
}

int main() {
    char line[1024];
    printf(">> ");
    fflush(stdout);
    
    while (fgets(line, sizeof(line), stdin)) {
        // 移除换行符
        line[strcspn(line, "\n")] = '\0';
        
        // 跳过空行
        if (strlen(line) == 0) {
            printf(">> ");
            fflush(stdout);
            continue;
        }
        
        // 检查是否是函数定义（以 "int " 开头）
        if (strncmp(line, "int ", 4) == 0) {
            // 函数定义
            if (compile_and_load_function(line)) {
                printf("OK.\n");
            } else {
                printf("Compilation failed.\n");
            }
        } else {
            // 表达式求值
            int result;
            if (evaluate_expression(line, &result)) {
                printf("= %d.\n", result);
            } else {
                printf("Evaluation failed.\n");
            }
        }
        
        printf(">> ");
        fflush(stdout);
    }
    
    cleanup();
    return 0;
}
