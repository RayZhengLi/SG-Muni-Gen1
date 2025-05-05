// File: sg-gpio-monitor.h
#ifndef SG_GPIO_MONITOR_H
#define SG_GPIO_MONITOR_H

#include <stdint.h>
#include <stdbool.h>

#define MAX_INPUTS 7

typedef struct {
    bool changed;
    bool rising_edge;
} InputChange;

typedef struct {
    InputChange inputs[MAX_INPUTS];
    uint32_t current_state;
} GPIOChangeInfo;

/**
 * @brief   Start the GPIO monitor thread
 * 
 * @param   [void]:  NULL
 * 
 * @return  NULL
 */
bool start_gpio_monitor();

/**
 * @brief   Stop the GPIO monitor thread
 * 
 * @param   [void]:  NULL
 * 
 * @return  NULL
 */
void stop_gpio_monitor();

/**
 * @brief   Register the GPIO monitor callback
 * 
 * @param   [void (*callback)]:            The callback funciton name
 * @param   [const GPIOChangeInfo *info]:  The GPIO change states are the input to callback functions
 * 
 * @return  NULL
 */
void set_gpio_change_callback(void (*callback)(const GPIOChangeInfo *info));

// Read config json from file


#endif // SG_GPIO_MONITOR_H
