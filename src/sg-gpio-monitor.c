// File: sg-gpio-monitor.c
// File: sg-gpio-monitor.c
#include "sg-gpio-monitor.h"
#include "sg-gpio.h"
#include "sg-log.h"
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>

static pthread_t gpio_monitor_thread;
static bool stop_thread = false;
static void (*gpio_change_callback)(const GPIOChangeInfo *info) = NULL;

void set_gpio_change_callback(void (*callback)(const GPIOChangeInfo *info)) {
    gpio_change_callback = callback;
}

static void *gpio_monitor_task(void *arg) {
    (void) arg;
    uint32_t last_input_state = 0;
    uint32_t current_state = 0;

    printf("GPIO Monitor Thread Started.\n");

    if (!gpio_read_inputs(ALL_INPUT, &last_input_state)) {
        printf("Failed to read initial GPIO inputs\n");
        return NULL;
    }

    while (!stop_thread) {
        if (gpio_read_inputs(ALL_INPUT, &current_state)) {
            uint32_t changed = (last_input_state ^ current_state) & 0x7F; // 只检测7个输入位
            if (changed) {
                GPIOChangeInfo info;
                info.current_state = current_state;

                for (int i = 0; i < MAX_INPUTS; ++i) {
                    if (changed & (1 << i)) {
                        info.inputs[i].changed = true;
                        info.inputs[i].rising_edge = (current_state & (1 << i)) != 0;
                        log_debug("Input %d state is changed",i);
                    }
                }

                if (gpio_change_callback) {
                    gpio_change_callback(&info);
                }

                for(int i =0; i < MAX_INPUTS; ++i){
                    info.inputs[i].changed = false;
                }

                last_input_state = current_state;
            }
        }
        usleep(5000); // 5ms
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
