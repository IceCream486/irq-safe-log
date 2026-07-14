#ifndef LOG_H
#define LOG_H

#include "ringbuf.h"
#include <stdint.h>
#include <stdatomic.h>
#include "FreeRTOS.h"
#include "semphr.h"
#include <stdio.h>

// 这两个参数都必须2^n才可以
#define RINGBUF_SIZE         1024
#define LOG_INFO_QUEUE_SIZE  16

enum log_level {
    LOG_LEVEL_DEBUG,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARNING,
    LOG_LEVEL_ERROR,
    LOG_LEVEL_CRITICAL
};

struct log_info {
    enum log_level level;
    uint32_t timestamp;
    struct ringbuf_claimed claimed;
    atomic_uint state;
};

struct log_handle {
    struct ringbuf rb;  // 环形缓冲区实例
    uint8_t buffer[RINGBUF_SIZE];   // 环形缓冲区的内存
    struct log_info info_queue[LOG_INFO_QUEUE_SIZE];    // 日志信息队列 使用RTOS的队列需要进行两次复制 太浪费了
    atomic_uint info_head;  // 日志信息队列的头部
    atomic_uint info_tail;  // 日志信息队列的尾部
    SemaphoreHandle_t info_queue_sem;   // 日志信息队列的信号量 计数信号量
    SemaphoreHandle_t complete_semaphore;   // 完成信号量
    atomic_bool initialized;   // 初始化标志 如果不是原子操作 写的中途
};

int log_init(void);
int log_write(enum log_level level, const char *msg, uint32_t len);

// 内部核心宏保持纯粹：它只管把fmt和后面的变参格式化出来
#define LOG_INTERNAL(level, fmt, ...) do { \
    char _log_local_buf[128]; \
    int _log_local_len = snprintf(_log_local_buf, sizeof(_log_local_buf), fmt, ##__VA_ARGS__); \
    if (_log_local_len > 0) { \
        if (_log_local_len >= (int)sizeof(_log_local_buf)) { \
            _log_local_len = sizeof(_log_local_buf) - 1; \
        } \
        log_write(level, _log_local_buf, (uint32_t)_log_local_len); \
    } \
} while(0)

// 基础精简版保持不变
#define LOG_DBG(fmt, ...)   LOG_INTERNAL(LOG_LEVEL_DEBUG,    fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  LOG_INTERNAL(LOG_LEVEL_INFO,     fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  LOG_INTERNAL(LOG_LEVEL_WARNING,  fmt, ##__VA_ARGS__)
#define LOG_ERR(fmt, ...)   LOG_INTERNAL(LOG_LEVEL_ERROR,    fmt, ##__VA_ARGS__)
#define LOG_CRIT(fmt, ...)  LOG_INTERNAL(LOG_LEVEL_CRITICAL, fmt, ##__VA_ARGS__)

#endif
