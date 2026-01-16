#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <ctype.h>
#include <fcntl.h>

#define MAX_SYSCALLS 1024
#define TOP_N 5

typedef struct {
    char name[64];
    double time;
} syscall_stat;

typedef struct {
    syscall_stat stats[MAX_SYSCALLS];
    int count;
    double total_time;
} syscall_stats;

// 解析strace输出的一行，提取系统调用名称和时间
int parse_strace_line(char *line, char *syscall_name, double *time) {
    // 跳过前面的空格
    char *p = line;
    while (*p && isspace(*p)) p++;
    
    // 提取系统调用名（到'('为止）
    char *start = p;
    while (*p && *p != '(' && !isspace(*p)) p++;
    if (p == start || *p == '\0') return 0;
    
    strncpy(syscall_name, start, p - start);
    syscall_name[p - start] = '\0';
    
    // 查找时间部分 <...>
    char *time_start = strrchr(line, '<');
    if (!time_start) return 0;
    
    char *time_end = strchr(time_start, '>');
    if (!time_end) return 0;
    
    // 提取时间字符串
    *time_end = '\0';
    char *time_str = time_start + 1;
    
    // 转换为double
    *time = atof(time_str);
    
    return 1;
}

// 添加系统调用到统计中
void add_syscall(syscall_stats *stats, const char *name, double time) {
    // 检查是否已存在
    for (int i = 0; i < stats->count; i++) {
        if (strcmp(stats->stats[i].name, name) == 0) {
            stats->stats[i].time += time;
            stats->total_time += time;
            return;
        }
    }
    
    // 添加到新位置
    if (stats->count < MAX_SYSCALLS) {
        strncpy(stats->stats[stats->count].name, name, sizeof(stats->stats[0].name) - 1);
        stats->stats[stats->count].name[sizeof(stats->stats[0].name) - 1] = '\0';
        stats->stats[stats->count].time = time;
        stats->count++;
        stats->total_time += time;
    }
}

// 比较函数用于排序
int compare_syscalls(const void *a, const void *b) {
    const syscall_stat *sa = (const syscall_stat *)a;
    const syscall_stat *sb = (const syscall_stat *)b;
    return (sa->time < sb->time) - (sa->time > sb->time);
}

// 打印top n系统调用
void print_top_syscalls(syscall_stats *stats, int n) {
    if (stats->count == 0 || stats->total_time == 0) return;
    
    // 排序
    qsort(stats->stats, stats->count, sizeof(syscall_stat), compare_syscalls);
    
    // 打印top n
    int print_count = stats->count < n ? stats->count : n;
    for (int i = 0; i < print_count; i++) {
        int ratio = (int)((stats->stats[i].time / stats->total_time) * 100 + 0.5);
        if (ratio > 0) {  // 只打印有时间的
            printf("%s (%d%%)\n", stats->stats[i].name, ratio);
        }
    }
    
    // 输出80个\0作为分隔符
    for (int i = 0; i < 80; i++) {
        putchar(0);
    }
    fflush(stdout);
}

// 查找可执行文件的完整路径
char* find_executable(const char *command) {
    // 如果是绝对路径或相对路径，直接检查
    if (command[0] == '/' || strchr(command, '/')) {
        if (access(command, X_OK) == 0) {
            return strdup(command);
        }
        return NULL;
    }
    
    // 从PATH环境变量中查找
    char *path_env = getenv("PATH");
    if (!path_env) return NULL;
    
    char *path_copy = strdup(path_env);
    char *dir = strtok(path_copy, ":");
    
    while (dir) {
        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir, command);
        
        if (access(full_path, X_OK) == 0) {
            free(path_copy);
            return strdup(full_path);
        }
        
        dir = strtok(NULL, ":");
    }
    
    free(path_copy);
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s COMMAND [ARG]...\n", argv[0]);
        return 1;
    }
    
    // 查找可执行文件
    char *full_path = find_executable(argv[1]);
    if (!full_path) {
        fprintf(stderr, "Command not found: %s\n", argv[1]);
        return 1;
    }
    
    // 创建管道
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        perror("pipe");
        free(full_path);
        return 1;
    }
    
    // 设置管道为非阻塞
    fcntl(pipefd[0], F_SETFL, fcntl(pipefd[0], F_GETFL) | O_NONBLOCK);
    
    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        free(full_path);
        return 1;
    }
    
    if (pid == 0) {  // 子进程
        // 关闭管道的读端
        close(pipefd[0]);
        
        // 将管道的写端复制到stderr
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        
        // 构建strace参数
        char **strace_args = malloc((argc + 3) * sizeof(char *));
        strace_args[0] = "strace";
        strace_args[1] = "-T";  // 显示系统调用时间
        strace_args[2] = full_path;
        
        for (int i = 2; i < argc; i++) {
            strace_args[i + 1] = argv[i];
        }
        strace_args[argc + 1] = NULL;
        
        // 执行strace
        execve("/usr/bin/strace", strace_args, environ);
        
        // 如果上面失败，尝试其他路径
        execve("/bin/strace", strace_args, environ);
        
        // 如果都失败
        perror("execve");
        free(strace_args);
        free(full_path);
        exit(EXIT_FAILURE);
    } else {  // 父进程
        free(full_path);
        
        // 关闭管道的写端
        close(pipefd[1]);
        
        syscall_stats stats = {0};
        struct timeval last_print, now;
        gettimeofday(&last_print, NULL);
        
        char buffer[4096];
        char line[4096];
        int line_pos = 0;
        
        int child_exited = 0;
        int last_pid_status = -1;
        
        while (!child_exited) {
            // 检查子进程状态（非阻塞）
            int status;
            pid_t result = waitpid(pid, &status, WNOHANG);
            
            if (result == pid) {
                child_exited = 1;
                if (WIFEXITED(status)) {
                    last_pid_status = WEXITSTATUS(status);
                } else if (WIFSIGNALED(status)) {
                    last_pid_status = WTERMSIG(status) + 128;
                }
            } else if (result == -1) {
                // 错误
                child_exited = 1;
            }
            
            // 读取管道数据
            ssize_t bytes_read;
            while ((bytes_read = read(pipefd[0], buffer, sizeof(buffer) - 1)) > 0) {
                buffer[bytes_read] = '\0';
                
                // 逐字符处理，构建行
                for (ssize_t i = 0; i < bytes_read; i++) {
                    if (buffer[i] == '\n') {
                        line[line_pos] = '\0';
                        
                        // 解析这一行
                        char syscall_name[64];
                        double time;
                        if (parse_strace_line(line, syscall_name, &time)) {
                            add_syscall(&stats, syscall_name, time);
                        }
                        
                        line_pos = 0;
                    } else if (line_pos < sizeof(line) - 1) {
                        line[line_pos++] = buffer[i];
                    }
                }
            }
            
            // 检查是否需要打印
            gettimeofday(&now, NULL);
            double elapsed = (now.tv_sec - last_print.tv_sec) + 
                            (now.tv_usec - last_print.tv_usec) / 1000000.0;
            
            if (elapsed >= 0.1) {  // 大约每秒10次
                print_top_syscalls(&stats, TOP_N);
                gettimeofday(&last_print, NULL);
            }
            
            // 如果没有数据可读且子进程已退出，退出循环
            if (bytes_read == 0 && child_exited) {
                break;
            }
            
            // 避免忙等待
            usleep(10000);  // 10ms
        }
        
        // 读取剩余数据
        ssize_t bytes_read;
        while ((bytes_read = read(pipefd[0], buffer, sizeof(buffer) - 1)) > 0) {
            buffer[bytes_read] = '\0';
            
            for (ssize_t i = 0; i < bytes_read; i++) {
                if (buffer[i] == '\n') {
                    line[line_pos] = '\0';
                    
                    char syscall_name[64];
                    double time;
                    if (parse_strace_line(line, syscall_name, &time)) {
                        add_syscall(&stats, syscall_name, time);
                    }
                    
                    line_pos = 0;
                } else if (line_pos < sizeof(line) - 1) {
                    line[line_pos++] = buffer[i];
                }
            }
        }
        
        // 最终打印一次
        if (stats.total_time > 0) {
            print_top_syscalls(&stats, TOP_N);
        }
        
        close(pipefd[0]);
        return last_pid_status;
    }
    
    return 0;
}
