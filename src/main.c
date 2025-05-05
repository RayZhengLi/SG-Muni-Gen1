// File: main.c
#include "sg-tcp.h"
#include "sg-gpio.h"
#include "sg-comm.h"
#include "sg-ringbuffer.h"
#include "sg-log.h"
#include "cJSON.h"
#include "main.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <syslog.h>

#include <sys/types.h>
#include <stddef.h>
#include <unistd.h>
#include <stdbool.h>
#include <getopt.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <sys/stat.h>

#include <calamp/stm_shm_defs.h>
#include <calamp/stm_msg_defs.h>


/*=========================== Type Defines ===================================*/
typedef struct {
    int pos;
    int b_value;
}BitDiff;

/*=========================== Global Variables ===================================*/

volatile sig_atomic_t running = 1;  // The global flag for app exit

GpioBias gpio_bias[7];              // The GPIO strcut to store the digital input bias              
pthread_mutex_t bincount_mutex;     // The global bincount mutex to avoid the conflict between gpio thread and server response
int bincount = 0;                   // This is the bincount number

SgTcpServer *g_srv;                 // The global server struct

pthread_t input_thread;

char esn_data[MAX_ESN_LEN] = "";
char channel_name[7][12] = {"ignition","binlift","reverse","waterjet","sweeper","reserve1","reserve2"};

/*======================== End of Global Variables ===============================*/

/*=========================== Private Functions Defines ===================================*/

/**
 * @brief   This is the stop signal handler
 * 
 * @param   [int sig]:  this is currretly not in use
 * 
 * @return  [NULL]
 */
void handle_sigint(int sig) {
    (void)sig;
    printf("[signal] SIGINT received, stopping server...\n");
    running = 0;
    SgTcpServer_stop(g_srv);
}

/*======================== End of Private Functions Defines ===============================*/

int main(){
    // Start the log thread
    if (log_init("/tmp/sg-tcp-muni-1.log", 100 * 1024) != 0) {
        return EXIT_FAILURE;
    }
    // Read esn no. from system file
    if(read_esn_from_file(ESN_FILE_NAME, esn_data, MAX_ESN_LEN)){
        log_error("Cannot read esn number from the file!");
        return EXIT_FAILURE;
    }

    // Read the bincount number from the file
    if(!read_bincount_from_file(BINCOUNT_FILE_NAME, &bincount)){
        write_bincount_to_file(BINCOUNT_FILE_NAME, 0);
    }

    // Init GPIO first before server and gpio thread started
    if(!gpio_init()){
        log_error("Failed to initialise the GPIOs");
        return EXIT_FAILURE;
    }

    // Set the input bias to high when it is under lab test
    char config_json[512]="";
    uint32_t gpio_bias_mask = 0xFE;
    uint32_t gpio_bias_state = 0x00;
    if(read_json_from_file("config.json",config_json,sizeof(config_json))){
        cJSON *root = cJSON_Parse(config_json);
        if (!root) {
            log_error("Filed to parse config JSON %s");
            gpio_bias_mask = 0xFE;
            gpio_bias_state = 0x00;
        }else{
            for(int i = 0; i < 7; i++){
                cJSON *obj = cJSON_GetObjectItem(root, channel_name[i]);
                if (cJSON_IsObject(obj)) {
                    cJSON *bias   = cJSON_GetObjectItem(obj, "bias");
                    cJSON *enable = cJSON_GetObjectItem(obj, "enable");
                    if (cJSON_IsString(bias) && cJSON_IsString(enable)) {
                        if(strcmp(bias->valuestring,"high") == 0){
                            gpio_bias[i].bias = 1;
                            gpio_bias_state |= (1 << i);
                        }else{
                            gpio_bias[i].bias = 0;
                            gpio_bias_state &= ~(1 << i);
                        }
                        if(strcmp(enable->valuestring,"high") == 0){
                            gpio_bias[i].enable = 1;
                        }else{
                            gpio_bias[i].enable = 0;
                        }
                    }
                }
            }
            gpio_set_inputbias(gpio_bias_mask,gpio_bias_state);
        }
        cJSON_Delete(root);
    }else{
        log_error("Failed to read config.json");
        cJSON *root = cJSON_CreateObject();
        for (int i = 0; i < 7; i++) {
            // 创建设备对象
            cJSON *device = cJSON_CreateObject();
            cJSON_AddStringToObject(device, "bias", "low");
            cJSON_AddStringToObject(device, "enable", "high");

            // 将设备对象添加到根 JSON
            cJSON_AddItemToObject(root, channel_name[i], device);
        }
        char *json_str = cJSON_Print(root);
        if (write_json_to_file("config.json", json_str) != 0) {
            log_error("Can not write to JSON File");
        }
        free(json_str);
        cJSON_Delete(root);
    }

    if(start_sg_server() == EXIT_FAILURE){
        log_error("Failed: start_sg_server()");
        return EXIT_FAILURE;
    }

    while(running){
        log_debug("Bincount number is %d", bincount);
        sleep(1);
    }

    stop_sg_server();
    gpio_deinit();

    log_info("Application exited gracefully!\n");
    log_close();

    return 0;
}


/**
 * @brief                       : Write the bincount to the file
 *
 * @param {char *filename}      : The file we gonna write to
 * @param {int *value}          : The bincount number
 * @return                      : 1 for success, 0 for failure 
 */
int write_bincount_to_file(const char *filename, int value){
        FILE *fp = fopen(filename, "rb+");  // 尝试以读写方式打开文件
    if (fp == NULL) {
        // if the file doesnot exist, than make a new one
        fp = fopen(filename, "wb+");
        if (fp == NULL) {
            perror("Cannot open the file");
            return 0;
        }
    }
    // Locat the front of the file
    if (fseek(fp, 0, SEEK_SET) != 0) {
        perror("Cannot locate the start of file");
        fclose(fp);
        return 0;
    }
    // write the bincount number of the file
    if (fwrite(&value, sizeof(int), 1, fp) != 1) {
        perror("Failed at writing");
        fclose(fp);
        return 0;
    }
    fclose(fp);
    return 1;
}

/**
 * @brief                       : Read the bincount from the file
 *
 * @param {char *filename}      : The file we gonna read from
 * @param {int *value}          : The bincount number
 * @return                      : 1 for success, 0 for failure 
 */
int read_bincount_from_file(const char *filename, int *value) {
    FILE *fp = fopen(filename, "rb");
    if (fp == NULL) {
        perror("File doesn't existed");
        return 0;
    }
    // 从文件中读取 int 数值
    if (fread(value, sizeof(int), 1, fp) != 1) {
        perror("Failed when reading");
        fclose(fp);
        return 0;
    }
    fclose(fp);
    return 1;
}

/**
 * @brief                       : Read the esn of LMU from the file
 *
 * @param {char *filename}      : The file we gonna read from
 * @param {int *value}          : The bincount number
 * @return                      : 1 for success, 0 for failure 
 */
int read_esn_from_file(const char *filename, char *esn, size_t esn_size) {
    FILE *fp = fopen(filename, "r");
    if (fp == NULL) {
        perror("Error opening file");
        return -1;
    }

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        // Check if the current line contains "option esn"
        if (strstr(line, "option esn") != NULL) {
            // Find the first single quote
            char *start = strchr(line, '\'');
            if (start != NULL) {
                start++;  // Move past the first single quote
                // Find the closing single quote
                char *end = strchr(start, '\'');
                if (end != NULL) {
                    size_t len = end - start;
                    // Ensure we do not exceed the provided buffer size
                    if (len >= esn_size) {
                        len = esn_size - 1;
                    }
                    strncpy(esn, start, len);
                    esn[len] = '\0';  // Null-terminate the string
                    fclose(fp);
                    return 0;
                }
            }
        }
    }

    fclose(fp);
    return -2;  // ESN not found or improperly formatted
}

/**
 * @brief                       : Read the config json from the file
 *
 * @param {char *filename}      : The file we gonna read from
 * @param {char *buffer}        : The buffer to be filled in
 * @param {size_t buffer_size}  : The buffer size
 * @return                      : 1 for success, 0 for failure 
 */
int read_json_from_file(const char *filename, char *buffer, size_t buffer_size)
{
    if (filename == NULL || buffer == NULL || buffer_size == 0) {
        fprintf(stderr, "invalid file parameter\n");
        return 0;
    }

    // 先通过stat获取文件大小
    struct stat st;
    if (stat(filename, &st) != 0) {
        perror("failed to get the size of the file");
        return 0;
    }
    // st.st_size是文件的字节数
    // +1 是为了存放字符串结束符 '\0'
    if ((off_t)buffer_size <= st.st_size) {
        fprintf(stderr, "buffer too small\n");
        return 0;
    }

    // 打开文件
    // JSON通常是文本文件，但也可以用"rb"保险地读取所有字节
    // 以确保保留任何换行符、特殊字符
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        perror("failed to open the file");
        return 0;
    }

    // 读取文件内容到buffer中
    size_t bytes_read = fread(buffer, 1, st.st_size, fp);
    if (bytes_read < (size_t)st.st_size) {
        if (ferror(fp)) {
            perror("failed when reading");
            fclose(fp);
            return 0;
        }
    }
    fclose(fp);

    // 在读取数据的末尾补上'\0'以形成C字符串
    buffer[bytes_read] = '\0';

    return 1;
}

/**
 * @brief                       : Write the config json from the file
 *
 * @param {char *filename}      : The file we gonna read from
 * @param {char *buffer}        : The buffer to be filled in
 * @param {size_t buffer_size}  : The buffer size
 * @return                      : 1 for success, 0 for failure 
 */
int write_json_to_file(const char *filename, const char *json_str)
{
    if (filename == NULL || json_str == NULL) {
        fprintf(stderr, "invalid parameters\n");
        return -1;
    }

    FILE *fp = fopen(filename, "wb");
    if (fp == NULL) {
        perror("failed to open the file");
        return -1;
    }

    // 直接写入字符串的全部长度
    // 这里用"wb"以便原样写入换行等字符
    size_t length = 0;
    while (json_str[length] != '\0') {
        length++;
    }

    size_t bytes_written = fwrite(json_str, 1, length, fp);
    if (bytes_written < length) {
        perror("failed to write into the file");
        fclose(fp);
        return -1;
    }

    fclose(fp);
    return 0;
}