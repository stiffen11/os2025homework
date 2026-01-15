#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <ctype.h>
#include <getopt.h>

#define MAX_PROCESSES 1024
#define VERSION "pstree 1.0"

typedef struct Process {
    pid_t pid;
    pid_t ppid;
    char name[256];
    struct Process *children;
    struct Process *next;
    struct Process *parent;
} Process;

typedef struct {
    Process *processes[MAX_PROCESSES];
    int count;
} ProcessArray;

// 全局选项
static int show_pids = 0;
static int numeric_sort = 0;
static int show_version = 0;

// 从 /proc/[pid]/stat 读取进程信息
int read_process_info(pid_t pid, pid_t *ppid, char *name) {
    char filename[256];
    char line[1024];
    
    snprintf(filename, sizeof(filename), "/proc/%d/stat", pid);
    FILE *fp = fopen(filename, "r");
    if (!fp) return -1;
    
    if (fgets(line, sizeof(line), fp) == NULL) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    
    // 解析 stat 文件内容
    // 格式: pid (comm) state ppid ...
    char *p = strrchr(line, ')');
    if (!p) return -1;
    
    // 提取进程名（去掉括号）
    char *name_start = strchr(line, '(');
    if (name_start && name_start < p) {
        int len = p - name_start - 1;
        if (len > 255) len = 255;
        strncpy(name, name_start + 1, len);
        name[len] = '\0';
    } else {
        strcpy(name, "unknown");
    }
    
    // 跳过 ) 和后面的空格
    p += 2;
    // state 字符
    p = strchr(p, ' ');
    if (!p) return -1;
    p++;
    
    // 读取 ppid
    *ppid = atoi(p);
    
    return 0;
}

// 获取所有进程
int get_all_processes(ProcessArray *pa) {
    DIR *dir;
    struct dirent *entry;
    
    dir = opendir("/proc");
    if (!dir) {
        perror("opendir");
        return -1;
    }
    
    pa->count = 0;
    
    while ((entry = readdir(dir)) != NULL) {
        // 检查是否是进程目录（以数字命名）
        if (entry->d_type == DT_DIR && isdigit(entry->d_name[0])) {
            pid_t pid = atoi(entry->d_name);
            if (pid <= 0) continue;
            
            Process *proc = malloc(sizeof(Process));
            if (!proc) continue;
            
            proc->pid = pid;
            proc->children = NULL;
            proc->next = NULL;
            proc->parent = NULL;
            
            if (read_process_info(pid, &proc->ppid, proc->name) == 0) {
                pa->processes[pa->count++] = proc;
            } else {
                free(proc);
            }
            
            if (pa->count >= MAX_PROCESSES - 1) break;
        }
    }
    
    closedir(dir);
    return 0;
}

// 构建进程树
Process *build_process_tree(ProcessArray *pa) {
    Process *root = NULL;
    
    // 建立父子关系
    for (int i = 0; i < pa->count; i++) {
        Process *proc = pa->processes[i];
        
        if (proc->ppid == 0) {  // 可能是根进程
            if (!root) root = proc;
            continue;
        }
        
        // 查找父进程
        for (int j = 0; j < pa->count; j++) {
            if (pa->processes[j]->pid == proc->ppid) {
                Process *parent = pa->processes[j];
                // 插入到父进程的子进程链表
                proc->next = parent->children;
                parent->children = proc;
                proc->parent = parent;
                break;
            }
        }
    }
    
    // 如果没有找到根进程（pid 1），则查找 init/systemd
    if (!root) {
        for (int i = 0; i < pa->count; i++) {
            Process *proc = pa->processes[i];
            if (proc->pid == 1) {
                root = proc;
                break;
            }
        }
    }
    
    return root;
}

// 比较函数，用于排序
int compare_pids(const void *a, const void *b) {
    Process *pa = *(Process **)a;
    Process *pb = *(Process **)b;
    return (pa->pid - pb->pid);
}

// 对子进程进行排序
void sort_children(Process *parent) {
    if (!parent || !parent->children) return;
    
    // 收集子进程到数组
    Process *children[256];
    int count = 0;
    Process *child = parent->children;
    
    while (child && count < 255) {
        children[count++] = child;
        child = child->next;
    }
    
    // 按PID排序
    qsort(children, count, sizeof(Process *), compare_pids);
    
    // 重新链接
    parent->children = NULL;
    for (int i = count - 1; i >= 0; i--) {
        children[i]->next = parent->children;
        parent->children = children[i];
    }
}

// 递归排序整个树
void sort_process_tree(Process *node) {
    if (!node) return;
    
    sort_children(node);
    
    Process *child = node->children;
    while (child) {
        sort_process_tree(child);
        child = child->next;
    }
}

// 打印进程树
void print_tree(Process *node, int depth, int *has_more) {
    if (!node) return;
    
    // 打印缩进
    for (int i = 0; i < depth - 1; i++) {
        if (has_more[i]) {
            printf("│   ");
        } else {
            printf("    ");
        }
    }
    
    if (depth > 0) {
        if (node->next) {
            printf("├── ");
        } else {
            printf("└── ");
        }
    }
    
    // 打印进程信息
    if (show_pids) {
        printf("%s(%d)", node->name, node->pid);
    } else {
        printf("%s", node->name);
    }
    
    // 如果有多线程，可以显示线程数（简化版不实现）
    printf("\n");
    
    // 递归打印子进程
    Process *child = node->children;
    int child_count = 0;
    while (child) {
        child_count++;
        child = child->next;
    }
    
    child = node->children;
    int i = 0;
    while (child) {
        if (depth > 0) {
            has_more[depth - 1] = (i < child_count - 1);
        }
        print_tree(child, depth + 1, has_more);
        child = child->next;
        i++;
    }
}

// 清理内存
void free_process_tree(Process *node) {
    if (!node) return;
    
    Process *child = node->children;
    while (child) {
        Process *next = child->next;
        free_process_tree(child);
        child = next;
    }
    
    free(node);
}

// 解析命令行参数
void parse_args(int argc, char *argv[]) {
    struct option long_options[] = {
        {"show-pids", no_argument, NULL, 'p'},
        {"numeric-sort", no_argument, NULL, 'n'},
        {"version", no_argument, NULL, 'V'},
        {0, 0, 0, 0}
    };
    
    int opt;
    while ((opt = getopt_long(argc, argv, "pnV", long_options, NULL)) != -1) {
        switch (opt) {
            case 'p':
                show_pids = 1;
                break;
            case 'n':
                numeric_sort = 1;
                break;
            case 'V':
                show_version = 1;
                break;
            default:
                fprintf(stderr, "Usage: %s [-p] [-n] [-V]\n", argv[0]);
                fprintf(stderr, "Options:\n");
                fprintf(stderr, "  -p, --show-pids     show PIDs\n");
                fprintf(stderr, "  -n, --numeric-sort  sort output by PID\n");
                fprintf(stderr, "  -V, --version       display version information\n");
                exit(EXIT_FAILURE);
        }
    }
}

int main(int argc, char *argv[]) {
    parse_args(argc, argv);
    
    if (show_version) {
        printf("%s\n", VERSION);
        return 0;
    }
    
    ProcessArray pa;
    if (get_all_processes(&pa) < 0) {
        fprintf(stderr, "Failed to get process list\n");
        return 1;
    }
    
    if (pa.count == 0) {
        fprintf(stderr, "No processes found\n");
        return 1;
    }
    
    Process *root = build_process_tree(&pa);
    
    if (!root) {
        fprintf(stderr, "Failed to build process tree\n");
        // 清理内存
        for (int i = 0; i < pa.count; i++) {
            free(pa.processes[i]);
        }
        return 1;
    }
    
    if (numeric_sort) {
        sort_process_tree(root);
    }
    
    // 打印进程树
    int has_more[64] = {0};
    print_tree(root, 0, has_more);
    
    // 清理内存
    // 注意：由于进程间已经建立父子关系，只需要释放根节点
    // 子节点会被递归释放
    free_process_tree(root);
    
    // 清理未链接到树中的进程
    for (int i = 0; i < pa.count; i++) {
        if (pa.processes[i]->parent == NULL && pa.processes[i] != root) {
            free(pa.processes[i]);
        }
    }
    
    return 0;
}
