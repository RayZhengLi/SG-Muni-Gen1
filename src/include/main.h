#ifndef SG_MAIN_H
#define SG_MAIN_H

#include "sg-tcp.h"
#include <pthread.h>
#include <signal.h>

#define QUEUE_CAP            1024
#define SERVER_PORT          6001
#define TIMEOUT_SEC          5
#define MAX_RETRIES          3
#define MAX_ESN_LEN          24
#define BINLIFT_PORT_NUM     1
#define REVERSE_PORT_NUM     2
#define BINCOUNT_FILE_NAME   "binrecord.bin"
#define ESN_FILE_NAME        "/etc/config/calamp_sysparams"


typedef struct{
    uint8_t bias;
    uint8_t enable;
}GpioBias;

extern SgTcpServer *g_srv;
extern volatile sig_atomic_t running;
extern GpioBias gpio_bias[7];  
extern int bincount;
extern pthread_mutex_t bincount_mutex;
extern char esn_data[MAX_ESN_LEN];

/**
 * @brief                       : Write the bincount to the file
 *
 * @param {char *filename}      : The file we gonna write to
 * @param {int *value}          : The bincount number
 * @return                      : 1 for success, 0 for failure 
 */
int write_bincount_to_file(const char *filename, int value);

/**
 * @brief                       : Read the bincount from the file
 *
 * @param {char *filename}      : The file we gonna read from
 * @param {int *value}          : The bincount number
 * @return                      : 1 for success, 0 for failure 
 */
int read_bincount_from_file(const char *filename, int *value);

int read_json_from_file(const char *filename, char *buffer, size_t buffer_size);

int write_json_to_file(const char *filename, const char *json_str);

int read_esn_from_file(const char *filename, char *esn, size_t esn_size);

#endif