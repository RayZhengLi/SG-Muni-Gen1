#include "sg-gpio.h"
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

// 引入共享内存和消息定义
#include <calamp/stm_shm_defs.h>
#include <calamp/stm_msg_defs.h>

// 全局状态
static GPIOStatus gpio_status;
static struct stm_shm *shm_info = NULL;
static int msg_queue_id = -1;
static int32_t stm_cli_seq = 0;

// 初始化共享内存和消息队列
bool gpio_init() {
    key_t shm_key = ftok(STM_SHM_FTOK_FILE, STM_SHM_FTOKID);
    if (shm_key == -1) {
        perror("ftok");
        return false;
    }

    int shm_id = shmget(shm_key, 0, 0);
    if (shm_id == -1) {
        perror("shmget");
        return false;
    }

    shm_info = shmat(shm_id, NULL, SHM_RDONLY);
    if (shm_info == (void *)-1) {
        perror("shmat");
        return false;
    }

    key_t msg_key = ftok(STM_MSG_FTOK_FILE, STM_MSG_SEND_FTOKID);
    if (msg_key == -1) {
        perror("ftok");
        return false;
    }

    msg_queue_id = msgget(msg_key, S_IWUSR);
    if (msg_queue_id == -1) {
        perror("msgget");
        return false;
    }

    return true;
}

// 读取单个或多个GPIO输入状态
bool gpio_read_inputs(uint32_t mask, uint32_t *state) {
    if (!shm_info || !state) {
        return false;
    }

    *state = shm_info->curr_input_state & mask;
    gpio_status.input_state &= *state;
    return true;
}

// 设置单个或多个GPIO输出状态
bool gpio_set_outputs(uint32_t mask, uint32_t value) {
    if (msg_queue_id == -1) {
        return false;
    }

    struct stm_mgr_msg msg = {0};
    msg.mtype = MTYPE_STM_DST; // This is the STM manager app ID
    msg.mtype_src = MTYPE_APP_3_SRC;
    msg.seq = stm_cli_seq;
    msg.opcode = STM_MGR_OPCODE_SET_OUTPUT; // The function code of output in STM manager
    memcpy(&msg.payload[0], &mask, sizeof(mask));
    memcpy(&msg.payload[sizeof(mask)], &value, sizeof(value));
    printf("payload: data_0 0x%x data_1 0x%x, data_2 0x%x, data_3 0x%x \n",
        msg.payload[0], msg.payload[1], msg.payload[2], msg.payload[3]);
    printf("payload: data_0 0x%x data_1 0x%x, data_2 0x%x, data_3 0x%x \n",
    msg.payload[4], msg.payload[5], msg.payload[6], msg.payload[7]);
    if (msgsnd(msg_queue_id, &msg, sizeof(msg) - sizeof(msg.mtype), 0) == -1) {
        perror("msgsnd");
        return false;
    }

    gpio_status.output_state = value & mask;
    return true;
}

// Set the input bias
bool gpio_set_inputbias(uint32_t mask, uint32_t value){
        if (msg_queue_id == -1) {
        return false;
    }

    struct stm_mgr_msg msg = {0};
    msg.mtype = MTYPE_STM_DST; // This is the STM manager app ID
    msg.mtype_src = MTYPE_APP_3_SRC;
    msg.seq = stm_cli_seq;
    msg.opcode = STM_MGR_OPCODE_SET_INPUT_BIAS; // The function code of output in STM manager
    memcpy(&msg.payload[0], &mask, sizeof(mask));
    memcpy(&msg.payload[sizeof(mask)], &value, sizeof(value));
    printf("payload: data_0 0x%x data_1 0x%x, data_2 0x%x, data_3 0x%x \n",
        msg.payload[0], msg.payload[1], msg.payload[2], msg.payload[3]);
    printf("payload: data_0 0x%x data_1 0x%x, data_2 0x%x, data_3 0x%x \n",
    msg.payload[4], msg.payload[5], msg.payload[6], msg.payload[7]);
    if (msgsnd(msg_queue_id, &msg, sizeof(msg) - sizeof(msg.mtype), 0) == -1) {
        perror("msgsnd");
        return false;
    }

    gpio_status.output_state = value & mask;
    return true;
}

// 读取单个或多个ADC值
bool gpio_read_adc(uint32_t mask, int32_t *adc_values) {
    if (!shm_info || !adc_values) {
        return false;
    }

    for (int i = 0; i < 8; i++) {
        if (mask & (1 << i)) {
            adc_values[i] = shm_info->curr_adc[i];
            gpio_status.adc_values[i] = shm_info->curr_adc[i];
        }
    }
    return true;
}

// 获取当前GPIO状态
const GPIOStatus *gpio_get_status() {
    uint32_t input_state;
    gpio_read_inputs(ALL_INPUT,&input_state);
    gpio_status.input_state = input_state;

    int32_t adc_readings[8];
    gpio_read_adc(ALL_ADC,adc_readings);
    for(int i = 0; i < 8; i++){
        gpio_status.adc_values[i] = adc_readings[i];
    }

    return &gpio_status;
}

// 释放共享内存和消息队列
void gpio_deinit() {
    if (shm_info) {
        if (shmdt(shm_info) == -1) {
            perror("shmdt");
        }
        shm_info = NULL;
    }

    // 这里不删除消息队列，因为它可能是多个进程共享的
    msg_queue_id = -1;
}

