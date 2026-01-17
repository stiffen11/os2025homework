#include <mymalloc.h>
#include <stddef.h>
#include <stdint.h>

spinlock_t big_lock;
long malloc_count;

// 对齐宏
#define ALIGNMENT 8
#define ALIGN(size) (((size) + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1))

// 内存块头部信息
typedef struct block_header {
    size_t size;                // 块大小（包括头部）
    struct block_header* next;  // 空闲链表指针
    int is_free;                // 是否空闲
} block_header_t;

#define HEADER_SIZE ALIGN(sizeof(block_header_t))
#define MIN_BLOCK_SIZE (HEADER_SIZE + ALIGNMENT)

// 从vmalloc获取的大内存块
typedef struct heap_segment {
    struct heap_segment* next;
    size_t size;
    // 数据从segments+1开始
} heap_segment_t;

static heap_segment_t* heap_segments = NULL;
static block_header_t* free_list = NULL;
static spinlock_t heap_lock;
static spinlock_t free_list_lock;

// 初始化堆
static void init_heap() {
    static int initialized = 0;
    if (!initialized) {
        heap_segments = NULL;
        free_list = NULL;
        initialized = 1;
    }
}

// 从堆中分配一个新的大块
static void* get_new_heap_segment(size_t size) {
    // 计算需要分配的总大小（包括segment头部）
    size_t total_size = size + sizeof(heap_segment_t);
    
    // 确保按页对齐（4096字节）
    if (total_size % 4096 != 0) {
        total_size = ((total_size + 4095) / 4096) * 4096;
    }
    
    // 使用框架提供的vmalloc分配内存
    void* mem = vmalloc(NULL, total_size);
    if (!mem) return NULL;
    
    // 设置segment头部
    heap_segment_t* seg = (heap_segment_t*)mem;
    seg->size = total_size - sizeof(heap_segment_t);
    
    // 获取堆锁并更新链表
    spin_lock(&heap_lock);
    seg->next = heap_segments;
    heap_segments = seg;
    spin_unlock(&heap_lock);
    
    // 返回可用于分配的内存区域
    return (void*)((char*)mem + sizeof(heap_segment_t));
}

// 分割内存块
static void split_block(block_header_t* block, size_t needed_size) {
    // 确保分割后有足够空间存放新的头部和最小块
    if (block->size >= needed_size + MIN_BLOCK_SIZE) {
        block_header_t* new_block = (block_header_t*)((char*)block + needed_size);
        new_block->size = block->size - needed_size;
        new_block->is_free = 1;
        new_block->next = block->next;
        
        block->size = needed_size;
        block->next = new_block;
    }
}

// 合并相邻的空闲块
static void merge_free_blocks() {
    block_header_t* curr = free_list;
    block_header_t* prev = NULL;
    
    while (curr && curr->next) {
        if ((char*)curr + curr->size == (char*)curr->next) {
            // 合并相邻块
            curr->size += curr->next->size;
            curr->next = curr->next->next;
            // 继续检查，可能可以继续合并
        } else {
            prev = curr;
            curr = curr->next;
        }
    }
}

// 在空闲链表中查找合适大小的块（最佳适应）
static block_header_t* find_free_block(size_t size) {
    block_header_t* curr = free_list;
    block_header_t* best_fit = NULL;
    block_header_t* best_fit_prev = NULL;
    block_header_t* prev = NULL;
    
    while (curr) {
        if (curr->is_free && curr->size >= size) {
            // 最佳适应算法
            if (!best_fit || curr->size < best_fit->size) {
                best_fit = curr;
                best_fit_prev = prev;
            }
            // 如果找到完全匹配的块，直接返回
            if (curr->size == size) {
                break;
            }
        }
        prev = curr;
        curr = curr->next;
    }
    
    // 如果需要从链表中移除最佳匹配块
    if (best_fit && best_fit_prev) {
        best_fit_prev->next = best_fit->next;
    } else if (best_fit && best_fit == free_list) {
        free_list = best_fit->next;
    }
    
    return best_fit;
}

void *mymalloc(size_t size) {
    if (size == 0) {
        return NULL;
    }
    
    // 对齐大小
    size_t aligned_size = ALIGN(size);
    size_t total_size = HEADER_SIZE + aligned_size;
    
    // 更新malloc计数（使用大锁保护）
    spin_lock(&big_lock);
    malloc_count++;
    spin_unlock(&big_lock);
    
    // 初始化堆（如果未初始化）
    init_heap();
    
    // 尝试从空闲链表中分配
    spin_lock(&free_list_lock);
    
    block_header_t* block = find_free_block(total_size);
    if (block) {
        // 标记为已使用
        block->is_free = 0;
        // 分割块（如果需要）
        split_block(block, total_size);
        
        spin_unlock(&free_list_lock);
        
        // 返回数据区域的指针
        return (void*)((char*)block + HEADER_SIZE);
    }
    
    spin_unlock(&free_list_lock);
    
    // 没有找到合适的空闲块，分配新的大块内存
    // 分配的大小至少为total_size，但为了减少碎片，分配更大的块
    size_t segment_size = total_size * 4;  // 一次分配多个块
    if (segment_size < 4096) {
        segment_size = 4096;  // 最小分配一页
    }
    
    void* new_mem = get_new_heap_segment(segment_size);
    if (!new_mem) {
        return NULL;  // 内存分配失败
    }
    
    // 初始化新块
    block_header_t* new_block = (block_header_t*)new_mem;
    new_block->size = segment_size;
    new_block->is_free = 0;
    new_block->next = NULL;
    
    // 分割新块
    split_block(new_block, total_size);
    
    // 将剩余部分添加到空闲链表
    if (new_block->next) {
        spin_lock(&free_list_lock);
        new_block->next->next = free_list;
        free_list = new_block->next;
        spin_unlock(&free_list_lock);
    }
    
    // 返回分配的内存
    return (void*)((char*)new_block + HEADER_SIZE);
}

void myfree(void *ptr) {
    if (!ptr) {
        return;
    }
    
    // 获取块头部
    block_header_t* block = (block_header_t*)((char*)ptr - HEADER_SIZE);
    
    // 检查块是否有效
    if (block->is_free) {
        // 双重释放，未定义行为，简单返回
        return;
    }
    
    // 将块标记为空闲并添加到空闲链表
    spin_lock(&free_list_lock);
    block->is_free = 1;
    block->next = free_list;
    free_list = block;
    
    // 合并空闲块
    merge_free_blocks();
    spin_unlock(&free_list_lock);
}
