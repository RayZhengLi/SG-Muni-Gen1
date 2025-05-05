#ifndef SG_GPIO_H
#define SG_GPIO_H

#include <stdint.h>
#include <stdbool.h>

#define ALL_INPUT 0xFF
#define ALL_OUTPUT 0x7F
#define ALL_ADC 0xFF

// 定义GPIO输入/输出/ADC状态结构体
typedef struct {
    uint32_t input_state;     // 当前输入状态
    uint32_t output_state;    // 当前输出状态
    int32_t adc_values[8];    // 当前ADC值
} GPIOStatus;

// 初始化共享内存和消息队列
bool gpio_init();

// 读取单个或多个GPIO输入状态
bool gpio_read_inputs(uint32_t mask, uint32_t *state);

// 设置单个或多个GPIO输出状态
bool gpio_set_outputs(uint32_t mask, uint32_t value);

// Set the input bias
bool gpio_set_inputbias(uint32_t mask, uint32_t value);

// 读取单个或多个ADC值
bool gpio_read_adc(uint32_t mask, int32_t *adc_values);

// 获取当前GPIO状态
const GPIOStatus *gpio_get_status();

// 释放共享内存和消息队列
void gpio_deinit();

#endif // GPIO_CONTROL_H
