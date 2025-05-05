// log.h
#ifndef _LOG_H_
#define _LOG_H_

#include <stdio.h>
#include <stddef.h>

// 日志等级定义
typedef enum {
    LOG_LEVEL_DEBUG,
    LOG_LEVEL_INFO,
    LOG_LEVEL_ERROR
} LogLevel;

// 初始化日志系统，传入日志文件路径、最大文件大小（字节）
int log_init(const char *filename, size_t max_size);

// 结束日志系统
void log_close(void);

// 写入不同等级的日志
void log_debug(const char *format, ...);
void log_info(const char *format, ...);
void log_error(const char *format, ...);

#endif // _LOG_H_
