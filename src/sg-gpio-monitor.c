// File: sg-gpio-monitor.c
#include "sg-gpio-monitor.h"
#include "sg-gpio.h"
#include "sg-log.h"
#include "main.h"

#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <limits.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>

static pthread_t gpio_monitor_thread;
static bool stop_thread = false;
static void (*gpio_change_callback)(const GPIOChangeInfo *info) = NULL;

// —— 新增：全局持久化信息与互斥量 ——
static GPIOChangeInfo g_info;
static pthread_mutex_t g_info_mtx = PTHREAD_MUTEX_INITIALIZER;

// 由环境变量覆盖，或用默认路径
static const char *default_counter_path = "gpio_counters.dat";
static char g_counter_path[256] = {0};

extern GpioBias gpio_bias[7];

// —— 工具函数：mkdir -p —— //
static int mkdir_p(const char *path, mode_t mode) {
    if (!path || !*path) return -1;
    char tmp[512];
    size_t len = strnlen(path, sizeof(tmp) - 1);
    if (len == 0 || len >= sizeof(tmp)) return -1;
    memcpy(tmp, path, len);
    tmp[len] = '\0';

    // 去掉末尾的 '/'（如果有）
    if (len > 1 && tmp[len - 1] == '/') tmp[len - 1] = '\0';

    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
                *p = '/';
                return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) != 0 && errno != EEXIST) return -1;
    return 0;
}

// —— 原子写：写临时文件 -> fflush -> fsync -> rename —— //
static int atomic_write_lines(const char *filename, const char *const *lines, int nlines) {
    if (!filename || !lines || nlines <= 0) return -1;

    // 构造父目录
    char dirbuf[512];
    const char *slash = strrchr(filename, '/');
    if (slash) {
        size_t dlen = (size_t)(slash - filename);
        if (dlen >= sizeof(dirbuf)) return -1;
        memcpy(dirbuf, filename, dlen);
        dirbuf[dlen] = '\0';
        if (mkdir_p(dirbuf, 0755) != 0) {
            // 目录创建失败也许是权限问题
            // 继续尝试写文件（若目录本已存在但不可写会失败）
        }
    }

    char tmpname[512];
    snprintf(tmpname, sizeof(tmpname), "%s.tmp.%d", filename, (int)getpid());

    FILE *fp = fopen(tmpname, "w");
    if (!fp) return -2;

    for (int i = 0; i < nlines; ++i) {
        if (fputs(lines[i], fp) == EOF) {
            fclose(fp);
            unlink(tmpname);
            return -3;
        }
    }

    if (fflush(fp) != 0) {
        fclose(fp);
        unlink(tmpname);
        return -4;
    }

    int fd = fileno(fp);
    if (fd < 0) {
        fclose(fp);
        unlink(tmpname);
        return -5;
    }

    if (fsync(fd) != 0) {
        fclose(fp);
        unlink(tmpname);
        return -6;
    }

    if (fclose(fp) != 0) {
        unlink(tmpname);
        return -7;
    }

    // 原子替换
    if (rename(tmpname, filename) != 0) {
        unlink(tmpname);
        return -8;
    }

    return 0;
}

// —— 对外：保存每个 counter 到文件（每行一个数），内部走原子写 —— //
int save_counters_to_file(const char *filename, const GPIOChangeInfo *info) {
    if (!filename || !info) return -1;

    // 组织行
    char *lines[MAX_INPUTS];
    char buf[MAX_INPUTS][32];
    for (int i = 0; i < MAX_INPUTS; i++) {
        snprintf(buf[i], sizeof(buf[i]), "%u\n", info->inputs[i].counter);
        lines[i] = buf[i];
    }
    return atomic_write_lines(filename, (const char *const *)lines, MAX_INPUTS);
}

// —— 对外：从文件读取各个 counter 值（文本格式，每行一个 unsigned） —— //
int load_counters_from_file(const char *filename, GPIOChangeInfo *info) {
    if (!filename || !info) return -1;

    FILE *fp = fopen(filename, "r");
    if (!fp) return -2;

    for (int i = 0; i < MAX_INPUTS; i++) {
        unsigned int value;
        if (fscanf(fp, "%u", &value) != 1) {
            fclose(fp);
            return -3;  // 文件格式不正确或数据不足
        }
        info->inputs[i].counter = value;
    }

    fclose(fp);
    return 0;  // 成功
}

void set_gpio_change_callback(void (*callback)(const GPIOChangeInfo *info)) {
    gpio_change_callback = callback;
}

static void *gpio_monitor_task(void *arg) {
    (void) arg;

    // —— 设定计数文件路径 —— //
    const char *env_path = getenv("SG_GPIO_COUNTER_FILE");
    const char *counter_path = env_path && *env_path ? env_path : default_counter_path;
    snprintf(g_counter_path, sizeof(g_counter_path), "%s", counter_path);

    // —— 初始化 g_info，并从文件恢复 —— //
    pthread_mutex_lock(&g_info_mtx);
    memset(&g_info, 0, sizeof(g_info));
    pthread_mutex_unlock(&g_info_mtx);

    int lrc = load_counters_from_file(g_counter_path, &g_info);
    if (lrc != 0) {
        // 没有文件或格式错误，从 0 开始
        log_error("GPIO Monitor: counter file not valid (%d), start from zero\n", lrc);
    }

    uint32_t last_input_state = 0;
    uint32_t current_state = 0;

    printf("GPIO Monitor Thread Started.\n");

    if (!gpio_read_inputs(ALL_INPUT, &last_input_state)) {
        printf("Failed to read initial GPIO inputs\n");
        return NULL;
    }

    pthread_mutex_lock(&g_info_mtx);
    g_info.current_state = last_input_state;
    pthread_mutex_unlock(&g_info_mtx);

    while (!stop_thread) {
        if (gpio_read_inputs(ALL_INPUT, &current_state)) {
            uint32_t changed = (last_input_state ^ current_state) & 0x7F; // 只检测7个输入位
            if (changed) {
                pthread_mutex_lock(&g_info_mtx);

                g_info.current_state = current_state;

                // 先清本次 changed / edge 状态（仅为回调视图）
                for (int i = 0; i < MAX_INPUTS; ++i) {
                    g_info.inputs[i].changed = false;
                    g_info.inputs[i].rising_edge = false;
                }

                for (int i = 0; i < MAX_INPUTS; ++i) {
                    if (changed & (1u << i)) {
                        g_info.inputs[i].changed = true;
                        g_info.inputs[i].rising_edge = (current_state & (1u << i)) != 0;

                        // 仅当上升沿 == gpio_bias[i].enable 时计数
                        if (g_info.inputs[i].rising_edge == gpio_bias[i].enable) {
                            if (g_info.inputs[i].counter >= UINT_MAX) {
                                g_info.inputs[i].counter = 0;
                            }
                            g_info.inputs[i].counter++;
                        }
                        log_debug("Input %d state is changed", i);
                    }
                }

                // 计数已更新 —— 立即原子持久化
                int src = save_counters_to_file(g_counter_path, &g_info);
                if (src != 0) {
                    log_error(stderr, "GPIO Monitor: save counters failed = %d\n", src);
                }

                // 回调（若有）
                if (gpio_change_callback) {
                    // 回调内若访问 g_info，这里仍在锁内是安全的；
                    // 若担心回调阻塞，可复制一份再解锁。
                    gpio_change_callback(&g_info);
                }

                // 回调后把 changed 清回 false（便于上层只看到“瞬时变化”）
                for (int i = 0; i < MAX_INPUTS; ++i) {
                    g_info.inputs[i].changed = false;
                }

                pthread_mutex_unlock(&g_info_mtx);

                last_input_state = current_state;
            }
        }

        // 5ms 轮询
        usleep(5000);
    }

    // 退出前做一次保存
    pthread_mutex_lock(&g_info_mtx);
    int src = save_counters_to_file(g_counter_path, &g_info);
    pthread_mutex_unlock(&g_info_mtx);
    if (src != 0) {
        log_error("GPIO Monitor: final save failed = %d\n", src);
    }

    printf("GPIO Monitor Thread Exiting.\n");
    return NULL;
}

bool start_gpio_monitor() {
    stop_thread = false;
    if (pthread_create(&gpio_monitor_thread, NULL, gpio_monitor_task, NULL) != 0) {
        perror("pthread_create");
        return false;
    }
    return true;
}

void stop_gpio_monitor() {
    stop_thread = true;
    pthread_join(gpio_monitor_thread, NULL);
}

/*========= Externel functions for other thread to get counter ==========*/
// —— 确保 g_counter_path 已初始化（线程可能未启动也能用） —— //
static void ensure_counter_path_initialized(void) {
    if (g_counter_path[0] == '\0') {
        const char *env_path = getenv("SG_GPIO_COUNTER_FILE");
        const char *counter_path = (env_path && *env_path) ? env_path : default_counter_path;
        snprintf(g_counter_path, sizeof(g_counter_path), "%s", counter_path);
    }
}

// —— 确保 g_counter_path 已初始化（线程可能未启动也能用） —— //
bool get_gpio_info_snapshot(GPIOChangeInfo *out) {
    if (!out) return false;
    ensure_counter_path_initialized();
    pthread_mutex_lock(&g_info_mtx);
    // 拷贝当前内部状态（计数、位图等）
    *out = g_info;   // 结构体按值拷贝
    pthread_mutex_unlock(&g_info_mtx);
    return true;
}

bool reset_gpio_counters(void) {
    ensure_counter_path_initialized();
    
    pthread_mutex_lock(&g_info_mtx);

    // 清零内存中的所有计数器
    for (int i = 0; i < MAX_INPUTS; ++i) {
        g_info.inputs[i].counter = 0;
    }

    // 保存到文件（原子写）
    int rc = save_counters_to_file(g_counter_path, &g_info);

    pthread_mutex_unlock(&g_info_mtx);

    if (rc != 0) {
        log_debug("GPIO Monitor: reset counters save failed = %d\n", rc);
        return false;
    }

    log_debug("GPIO Monitor: all counters reset to zero");
    return true;
}

bool reset_gpio_counter(int index) {
    ensure_counter_path_initialized();

    if (index < 0 || index >= MAX_INPUTS) {
        fprintf(stderr, "GPIO Monitor: reset counter index out of range: %d\n", index);
        return false;
    }

    ensure_counter_path_initialized();

    pthread_mutex_lock(&g_info_mtx);

    g_info.inputs[index].counter = 0;

    // 持久化到文件（原子写）
    int rc = save_counters_to_file(g_counter_path, &g_info);

    pthread_mutex_unlock(&g_info_mtx);

    if (rc != 0) {
        fprintf(stderr, "GPIO Monitor: reset counter[%d] save failed = %d\n", index, rc);
        return false;
    }

    log_debug("GPIO Monitor: counter[%d] reset to zero", index);
    return true;
}