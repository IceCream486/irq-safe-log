#include "log.h"
#include <string.h>
#include <stdio.h>
#include "FreeRTOS.h"
#include "task.h"

#include "main.h"
#include "usart.h"

extern UART_HandleTypeDef huart1; 
static struct log_handle g_log = {0};

/**
 * @brief 底层日志物理输出函数
 * @note  100% 在任务上下文中运行，通过信号量控制 DMA 流水 只在任务上下文
 */
static void low_level_log_output(const uint8_t *buf, uint32_t len) {
    // 原子检查initialized
    if (!atomic_load_explicit(&g_log.initialized, memory_order_relaxed) || len == 0 || !buf) {
        return; // 还没有初始化或参数错误
    }

    if (xSemaphoreTake(g_log.complete_semaphore, pdMS_TO_TICKS(200)) == pdTRUE) {
        while (huart1.gState != HAL_UART_STATE_READY) {
            taskYIELD(); // 如果是抢占式内核，可以安全让步
        }
        HAL_StatusTypeDef status = HAL_UART_Transmit_DMA(&huart1, (uint8_t *)buf, len);
        if (status != HAL_OK) {
            xSemaphoreGive(g_log.complete_semaphore);
        }
    } else {
        HAL_UART_DMAStop(&huart1);
        huart1.gState = HAL_UART_STATE_READY;
        xSemaphoreGive(g_log.complete_semaphore);
    }
}

/**
 * @brief UART DMA 发送完成中断回调函数
 * @note  由 STM32 HAL 库在串口 DMA 发送完成中断中自动调用
 */
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART1) { // 确保是负责日志输出的串口
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        
        // 释放二值信号量，通知日志任务可以发送下一段数据了
        xSemaphoreGiveFromISR(g_log.complete_semaphore, &xHigherPriorityTaskWoken);
        
        // 如果有更高优先级的任务被唤醒，触发上下文切换
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}

static void log_output_task(void *pvParameters);

int log_init(void) {
    if (ringbuf_init(&g_log.rb, g_log.buffer, RINGBUF_SIZE) != 0) {
        return -1;
    }
    
    // 创建计数信号量，最大计数值为队列大小，初始值为 0
    g_log.info_queue_sem = xSemaphoreCreateCounting(LOG_INFO_QUEUE_SIZE, 0);
    
    // 创建二值信号量用于同步 DMA，并立即 Give 给予初始发送许可
    g_log.complete_semaphore = xSemaphoreCreateBinary();
    if (g_log.complete_semaphore != NULL) {
        xSemaphoreGive(g_log.complete_semaphore);
    }
    // 创建任务
    if(xTaskCreate(log_output_task, "log_output_task", 1024, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
        return -1;
    }
    
    // 初始化日志队列
    atomic_init(&g_log.info_head, 0);
    atomic_init(&g_log.info_tail, 0);
    memset(g_log.info_queue, 0, sizeof(g_log.info_queue));
    
    // 原子操作initialized
    atomic_store_explicit(&g_log.initialized, true, memory_order_release);
    return 0;
}

/**
 * @brief 日志输出任务
 * @note  可能在中断中运行也可能在任务上下文中运行
 */
int log_write(enum log_level level, const char *msg, uint32_t len) {
    // 参数检查
    if(level < LOG_LEVEL_DEBUG || level > LOG_LEVEL_CRITICAL || !msg || len == 0)
       return -1; // 参数检查

    // 原子检查initialized
    if (!atomic_load_explicit(&g_log.initialized, memory_order_relaxed)) {
        return -1; // 还没有初始化
    } 
        
    if (len > RINGBUF_SIZE / 2) {
        len = RINGBUF_SIZE / 2;
    }

    uint32_t curr_i_head, next_i_head, curr_i_tail;
    int retry_count = 0;
    // 原子获取一个槽位 然后这个槽位不会被多个线程使用只会在一个线程中使用
    do {
        curr_i_head = atomic_load_explicit(&g_log.info_head, memory_order_relaxed);
        curr_i_tail = atomic_load_explicit(&g_log.info_tail, memory_order_acquire);
        
        // 绝对索引直接相减，天然支持无符号数溢出回绕
        if ((curr_i_head - curr_i_tail) >= (LOG_INFO_QUEUE_SIZE - 1)) {
            if (xPortIsInsideInterrupt()) {
                return -2; // 中断内如果满队列，直接放弃，严禁阻塞或让步
            }
            if (retry_count++ > 100) {
                return -2; // 任务上下文中尝试 100 次仍满，抛弃该条日志
            }
            taskYIELD();
            continue;
        }
        
        next_i_head = curr_i_head + 1;
    } while (!atomic_compare_exchange_weak_explicit(
                &g_log.info_head, &curr_i_head, next_i_head,
                memory_order_release, memory_order_relaxed));

    struct log_info *info = &g_log.info_queue[curr_i_head & (LOG_INFO_QUEUE_SIZE - 1)];
    info->claimed.ready = false; // 尽早重置状态，防止上一轮数据污染！

    // 在环形缓冲区中预留空间
    if (!ringbuf_claim(&g_log.rb, len, &info->claimed)) {
        info->claimed.len = 0;
        info->claimed.ready = true;
        // 即使预留失败，也要释放信号量让消费者推进 tail 指针 因为前面的info已经申请成功了
        if (xPortIsInsideInterrupt()) {
            BaseType_t xHigherPriorityTaskWoken = pdFALSE;
            xSemaphoreGiveFromISR(g_log.info_queue_sem, &xHigherPriorityTaskWoken);
            portYIELD_FROM_ISR(xHigherPriorityTaskWoken);   // 这个其实不调用也是可以的 主要是理论上的实时需要
        } else {
            xSemaphoreGive(g_log.info_queue_sem);
        }
        return -3;
    }

    info->level = level;
    info->timestamp = (xPortIsInsideInterrupt()) ? xTaskGetTickCountFromISR() : xTaskGetTickCount();
    
    // 写入数据并将 claimed.ready 标记为 true
    ringbuf_write_claimed(&g_log.rb, &info->claimed, (const uint8_t *)msg);
    
    // 释放计数信号量，唤醒全自动输出任务
    if (xPortIsInsideInterrupt()) {
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        xSemaphoreGiveFromISR(g_log.info_queue_sem, &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    } else {
        xSemaphoreGive(g_log.info_queue_sem);
    }
    
    return 0;
}

// 等级字符串数组
const char *log_level_str[] = {
    "DEBUG",
    "INFO",
    "WARNING",
    "ERROR",
    "CRITICAL"
};


static void log_output_task(void *pvParameters) {
    uint8_t format_buf[32];
    (void)pvParameters;
    
    for (;;) {
        // ─── 核心：无数据时绝对挂起 ───
        xSemaphoreTake(g_log.info_queue_sem, portMAX_DELAY);
    
        uint32_t curr_tail = atomic_load_explicit(&g_log.info_tail, memory_order_relaxed);
        uint32_t curr_head = atomic_load_explicit(&g_log.info_head, memory_order_acquire);
        
        if (curr_tail == curr_head) {   // 无数据
            continue; 
        }
        
        struct log_info *info = &g_log.info_queue[curr_tail & (LOG_INFO_QUEUE_SIZE - 1)];
        
        // 等待数据就绪
        while (!info->claimed.ready) {
            vTaskDelay(1);
        }
        
        if (info->claimed.len > 0) {
            // 1. 格式化并输出日志头
            int head_len = snprintf((char*)format_buf, sizeof(format_buf),
                                    "[%lu][%s] ", info->timestamp, log_level_str[info->level]);
            if (head_len > 0) {
                low_level_log_output(format_buf, head_len);
            }
            
            // 2. 直接从 ringbuf 发送日志正文（零拷贝）
            uint32_t start = info->claimed.start_idx;
            uint32_t remaining = info->claimed.len;
            
            while (remaining > 0) {
                // 计算当前片段能连续发送的长度（到 ringbuf 末尾）
                uint32_t chunk = g_log.rb.size - start;
                if (chunk > remaining) {
                    chunk = remaining;
                }
                
                // 直接从 ringbuf 发送，无需 memcpy
                low_level_log_output(g_log.rb.buffer + start, chunk);
                
                // 更新位置
                start = (start + chunk) & g_log.rb.mask;
                remaining -= chunk;
            }
            // 释放 ringbuf 占用的空间
            ringbuf_advance_tail(&g_log.rb, info->claimed.len);
        }
        
        // 推进 info_queue 队列的尾指针
        atomic_store_explicit(&g_log.info_tail, curr_tail + 1, memory_order_release);
    }
}