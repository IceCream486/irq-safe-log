#include "ringbuf.h"
#include <string.h>

int ringbuf_init(struct ringbuf *rb, uint8_t *buffer, uint32_t size) {
    if (!rb || !buffer || size == 0 || (size & (size - 1)) != 0) return -1;
    
    rb->buffer = buffer;
    rb->size = size;
    rb->mask = size - 1;
    atomic_init(&rb->head, 0);
    atomic_init(&rb->tail, 0);
    return 0;
}

/**
 * @brief 申请指定长度的环形缓冲区空间
 * @param rb 环形缓冲区结构体指针
 * @param len 申请的长度
 * @param claimed 环形缓冲区申请结构体指针
 * @note 可能在任务和中断上下文
 */
bool ringbuf_claim(struct ringbuf *rb, uint32_t len, struct ringbuf_claimed *claimed) {
    uint32_t current_head, current_tail, next_head;
    uint32_t used, available;

    // 空间足够 但是被抢占了 会重新申请 如果不够会直接退出 不会阻塞中断
    do {
        current_head = atomic_load_explicit(&rb->head, memory_order_relaxed);
        current_tail = atomic_load_explicit(&rb->tail, memory_order_acquire);
        
        // ─── 修复：绝对索引直接相减，天然支持无符号溢出 ───
        used = current_head - current_tail; 
        available = rb->size - used;
        
        // 保留 1 字节防止 head==tail 无法区分空满
        if (available < len + 1) {
            return false; 
        }
        
        next_head = current_head + len;
    } while (!atomic_compare_exchange_weak_explicit(
                &rb->head, &current_head, next_head,
                memory_order_release, memory_order_relaxed));

    claimed->start_idx = current_head & rb->mask; 
    claimed->len = len;
    atomic_init(&claimed->ready, false);
    return true;
}

/**
 * @brief 写入申请的数据到环形缓冲区
 * @param rb 环形缓冲区结构体指针
 * @param claimed 环形缓冲区申请结构体指针
 * @param data 要写入的数据
 * @note 可能在任务上下文也可能在中断上下文 安全的
 */
void ringbuf_write_claimed(struct ringbuf *rb, struct ringbuf_claimed *claimed, const uint8_t *data) {
    uint32_t start = claimed->start_idx;
    uint32_t len = claimed->len;
    
    // 分段拷贝（处理环形换行）
    uint32_t first_chunk = rb->size - start;
    if (first_chunk >= len) {
        memcpy(rb->buffer + start, data, len);
    } else {
        memcpy(rb->buffer + start, data, first_chunk);
        memcpy(rb->buffer, data + first_chunk, len - first_chunk);
    }
    
    // 确保数据写入完成后再标记 ready
    atomic_store_explicit(&claimed->ready, true, memory_order_release);
}

/**
 * @brief 更新环形缓冲区的尾部指针
 * @param rb 环形缓冲区结构体指针
 * @param len 要更新的长度
 * @note 只可能在任务上下文
 */
void ringbuf_advance_tail(struct ringbuf *rb, uint32_t len) {
    uint32_t old_tail = atomic_load_explicit(&rb->tail, memory_order_relaxed);
    atomic_store_explicit(&rb->tail, old_tail + len, memory_order_release);
}
