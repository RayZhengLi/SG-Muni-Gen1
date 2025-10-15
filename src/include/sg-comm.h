#ifndef SG_COMM_H
#define SG_COMM_H

#include "cJSON.h"
#include "sg-tcp.h"
#include "sg-ringbuffer.h"
#include <pthread.h>
#include <stdbool.h>

#define ADC_REQUEST 1
#define OUTPUT_REQUEST 2
#define SYNC_REQUEST 3

#define CMD_QUES "{BIN?;}"
#define CMD_CLR  "{CLR?;}"
#define CMD_EQL  "{BIN="

typedef enum {
    MSG_UNKNOWN,
    MSG_BIN_QUERY,
    MSG_CLR_QUERY,
    MSG_BIN_ACK,
    MSG_WEB_GPIO,
    MSG_WEB_GPS,
} MsgType;

typedef struct{
    int client_fd;
    int bincount;
    char message[128];
}ClientRequest;

typedef struct{
    MsgType type;
    bool retry_msg;  // If true, this is a re-sent message
    char url[256];
    char body[1024];
}WebRequest;

typedef struct{
    RingBuffer *bin_query_tasks;
    RingBuffer *bin_clear_tasks;
    RingBuffer *bin_ack_tasks;
    RingBuffer *web_client_tasks;
}TaskQueueSet;

// 待确认消息链表
typedef struct PendingAck {
    int                 client_fd;
    char               *msg;
    size_t              len;
    time_t              last_sent;
    int                 retries;
    int                 acked;
    int        bincount;
    struct PendingAck  *next;
} PendingAck;


/**
 * @brief   Creat the task lists for 3 client request handlers
 * 
 * @param   [TaskQueueSet]:   The taks lists
 * 
 * @return  [int 1]: Successful
 *          [int 0]: Failed
 */
int create_task_pipeline(TaskQueueSet *pipeline);

/*=========================================================*/
/**
 * @brief   This is the recall function for TCP server. It parse
 *          the message and pusb task in to the ringbuf [task_q]
 * 
 * @param   [SgTcpServer *srv]:  The server strcut
 * @param   [const SgTcpMessage *msg]:  The client orignial msg
 * @param   [void *ud]:  We can use this to update the task queue
 * 
 * @return  NULL
 */
void on_message(SgTcpServer *srv, const SgTcpMessage *msg, void *ud);

/**
 * @brief   Add a tcp job to the pending linked list
 * 
 * @param   [int fd]:   The Client fd num
 * @param   [const char *str]:   The message to re-send
 * #param   [unsigned int bincount]:  The bincount to re-send
 * 
 * @return  NULL
 */
void add_pending(int fd, const char *str, unsigned int bincount);
/**
 * @brief   This is the thread function to manage the ACK from client
 * 
 * @param   [void *arg]:  The param import from thread creation
 * 
 * @return  NULL
 */
void *ack_manager(void *arg);



int start_sg_server();

void stop_sg_server();

/*=========================================================*/

#endif