// File: sg-gpio-monitor.h
#ifndef SG_GPIO_MONITOR_H
#define SG_GPIO_MONITOR_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

// ======================= 配置与说明 =======================
//
// 计数持久化文件路径：默认使用
//   /var/lib/sg/gpio_counters.txt
// 也可通过环境变量覆盖：
//   export SG_GPIO_COUNTER_FILE=/path/to/file
//
// 文件格式：文本，每行一个无符号整数（共 MAX_INPUTS 行）。
// 掉电保护：写入采用 临时文件 + fflush + fsync + rename 的原子替换策略。
// =========================================================

// 仅检测 7 路输入
#ifndef MAX_INPUTS
#define MAX_INPUTS 7
#endif

// 每路输入的变化信息与计数
typedef struct {
    bool         changed;      // 本次扫描是否发生变化（回调视图字段）
    bool         rising_edge;  // 本次变化是否为上升沿（回调视图字段）
    unsigned int counter;      // 触发计数（持久化字段）
} InputChange;

// GPIO 汇总信息
typedef struct {
    InputChange inputs[MAX_INPUTS];
    uint32_t    current_state; // 最近一次读取到的原始输入位图（低 7 位有效）
} GPIOChangeInfo;

/**
 * @brief 设置 GPIO 变化回调
 * @param callback  回调函数指针；回调收到的指针在回调期间有效，勿跨线程/异步保存。
 *                  如需长期保存，请自行拷贝内容。
 */
void set_gpio_change_callback(void (*callback)(const GPIOChangeInfo *info));

/**
 * @brief 启动 GPIO 监控线程
 * @return true 启动成功；false 启动失败
 *
 * 线程启动后会：
 *  1) 从计数文件中恢复各输入的 counter（若文件无效则从 0 开始）；
 *  2) 以 ~5ms 轮询读取输入状态（仅低 7 位）；
 *  3) 在符合计数条件时更新 counter，并原子持久化到文件；
 *  4) 若设置了回调，会在每次检测到变化时触发回调。
 */
bool start_gpio_monitor(void);

/**
 * @brief 停止 GPIO 监控线程（阻塞直至退出）
 */
void stop_gpio_monitor(void);

/**
 * @brief 将 info 中各通道的 counter 写入文件（文本，每行一个无符号数）
 *        内部采用“临时文件 + fflush + fsync + rename”的原子替换策略。
 * @param filename 输出文件路径
 * @param info     含有各 counter 的结构
 * @return 0 成功；负值为错误码
 */
int save_counters_to_file(const char *filename, const GPIOChangeInfo *info);

/**
 * @brief 从文件读取各通道的 counter（文本，每行一个无符号数）
 * @param filename 输入文件路径
 * @param info     输出结构（仅写入 counter 字段；其他字段调用方可再初始化）
 * @return 0 成功；负值为错误码
 */
int load_counters_from_file(const char *filename, GPIOChangeInfo *info);

/**
 * @brief 线程安全地获取当前 GPIO 状态与各通道计数的“快照”
 * @param out  输出指针；成功时写入一份完整拷贝（包括 counters 与 current_state）
 * @return true 成功；false 失败（如 out 为 NULL）
 *
 * 说明：
 * - 本函数会在内部加锁，复制内部维护的 g_info，然后立即解锁。
 * - 由于“变化标志 changed / rising_edge”是瞬时视图，主线程在回调后会清零，
 *   因此快照通常显示为 false；如果你需要边沿事件，请使用回调机制。
 */
bool get_gpio_info_snapshot(GPIOChangeInfo *out);

/**
 * @brief 将所有输入的 counter 清零并立即保存到文件（线程安全）
 * @return true 成功；false 失败
 *
 * 说明：
 * - 内部会加锁保护 g_info。
 * - 清零完成后立即调用 save_counters_to_file() 原子写入。
 * - 若当前监控线程未启动，仍可调用该函数清零上次保存的文件。
 */
bool reset_gpio_counters(void);

/**
 * @brief 只清零指定通道的 counter 并立即保存到文件（线程安全）
 * @param index 通道索引 [0, MAX_INPUTS)
 * @return true 成功；false 失败（含 index 越界或保存失败）
 */
bool reset_gpio_counter(int index);


#ifdef __cplusplus
}
#endif

#endif // SG_GPIO_MONITOR_H
