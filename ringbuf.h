#ifndef __RINGBUF_H__
#define __RINGBUF_H__

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif


struct ringbuf {
    uint8_t *buffer;    // 环形缓冲区内存真实地址
    uint32_t size;  // 环形缓冲区大小
    uint32_t mask;  // 环形缓冲区大小的掩码 这就要求环形缓冲区大小要是2^n
    atomic_uint head;  // 环形缓冲区头部指针
    atomic_uint tail;   // 环形缓冲区尾部指针
};

struct ringbuf_claimed {
    uint32_t start_idx; // 环形缓冲区头部指针
    uint32_t len;   // 申请的长度
    atomic_bool ready;    // 数据是否复制成功
};

// 初始化一个环形缓冲区
int ringbuf_init(struct ringbuf *rb, uint8_t *buffer, uint32_t size);
// 从环形缓冲区中原子申请一块空间 但是内容没写入
bool ringbuf_claim(struct ringbuf *rb, uint32_t len, struct ringbuf_claimed *claimed);
// 向申请的空间写入数据 非常值得注意的是 申请多少写多少 因为打印是逐字节打印的 如果没有写 不保证内容是什么
void ringbuf_write_claimed(struct ringbuf *rb, struct ringbuf_claimed *claimed, const uint8_t *data);
// 打印完了之后 需要向前移动尾指针 非常值得注意 用户不需要调用
void ringbuf_advance_tail(struct ringbuf *rb, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif
