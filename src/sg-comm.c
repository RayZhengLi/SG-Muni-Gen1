#include "sg-comm.h"
#include "sg-tcp.h"
#include "sg-gpio.h"
#include "sg-ringbuffer.h"
#include "sg-log.h"
#include "sg-gpio-monitor.h"
#include "main.h"
#include <time.h>
#include "cJSON.h"
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <uuid/uuid.h>
#include <syslog.h>
#include <ctype.h>
static PendingAck      *pending_head  = NULL;
static pthread_mutex_t  pending_mtx   = PTHREAD_MUTEX_INITIALIZER;
static TaskQueueSet    *task_queues   = NULL;   // CAUTION: the tasks in this queue must be freed in [task threads] and should not be freed in [on_message] function
                                                // CAUTION: this ringbuffer should not be freed before end of the whole programe
static pthread_t bin_query_thread;
static pthread_t bin_clr_thread;
static pthread_t bin_ack_thread;
static pthread_t bin_resend_thread;
static int       thread_stop          = 1;

static MsgType parse_client_request(ClientRequest *client_req, const char *msg);
static void gpio_change_callback(const GPIOChangeInfo *info);
static void assemble_reverse_message(char *str, bool state);
static char *assemble_binlift_message(int bin_count, const char *esn);

/**
 * @brief   Creat the task lists for 3 client request handlers
 * 
 * @param   [TaskQueueSet]:   The taks lists
 * 
 * @return  [int 1]: Successful
 *          [int 0]: Failed
 */
int create_task_pipeline(TaskQueueSet *pipeline){
    pipeline->bin_ack_tasks = rb_create(128);
    pipeline->bin_clear_tasks = rb_create(128);
    pipeline->bin_query_tasks = rb_create(128);
    if(pipeline->bin_ack_tasks && pipeline->bin_clear_tasks && pipeline->bin_query_tasks){
        return 1;
    }else{
        return 0;
    }
}

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
void on_message(SgTcpServer *srv, const SgTcpMessage *msg, void *ud) {
    (void) srv;
    TaskQueueSet *queues = (TaskQueueSet *)ud;
    ClientRequest *task = calloc(1, sizeof(ClientRequest));
    task->client_fd = msg->client_fd;
    MsgType task_type = parse_client_request(task, (const char*)msg->data);
    log_debug("Received type (%d) client request from client (%d)",task_type, task->client_fd);
    int pushed = 0;

    switch(task_type){
        case MSG_BIN_QUERY:
            log_debug("Pushing the bin query: (%s) from client (%d) request into queue", task->message, task->client_fd);
            pushed = rb_timed_push(queues->bin_query_tasks, task, 100);
            break;
        case MSG_CLR_QUERY:
            log_debug("Pushing the bin clear: (%s) from client (%d) request into queue", task->message, task->client_fd);
            pushed = rb_timed_push(queues->bin_clear_tasks, task, 100);
            break;
        case MSG_BIN_ACK:
            log_debug("Pushing the bin clear: (%s) from client (%d) request into queue", task->message, task->client_fd);
            pushed = rb_timed_push(queues->bin_ack_tasks, task, 100);
            break;
        default:
            break;
    }
    if(!pushed){
        log_error("Failed to push task (%s) from client (%d), dropping", task->message, task->client_fd);
        free(task);
    }else{
        log_debug("Task (%s) from client (%d) has been pushed successfully", task->message, task->client_fd);
    }
}

/**
 * @brief   Add a tcp job to the pending linked list
 * 
 * @param   [int fd]:   The Client fd num
 * @param   [const char *str]:   The message to re-send
 * 
 * @return  NULL
 */
void add_pending(int fd, const char *str, unsigned int bincount){
    PendingAck *p = calloc(1, sizeof(*p));
    p->client_fd = fd;
    p->bincount  = bincount;
    p->len       = strlen(str);
    p->msg       = strdup(str);
    p->last_sent = time(NULL);
    pthread_mutex_lock(&pending_mtx);
    p->next = pending_head;
    pending_head = p;
    pthread_mutex_unlock(&pending_mtx);
}

/**
 * @brief   This is the thread function to manage the ACK from client.
 *          It re-send the messages if 
 * 
 * @param   [void *arg]:  The param import from thread creation
 * 
 * @return  NULL
 */
void *ack_manager(void *arg) {
    (void)arg;
    printf("[ack_manager] thread started\n");
    while (!thread_stop) {
        sleep(1);
        time_t now = time(NULL);
        pthread_mutex_lock(&pending_mtx);
        PendingAck **pp = &pending_head;
        while (*pp) {
            PendingAck *p = *pp;
            if (p->acked || p->retries >= MAX_RETRIES) {
                *pp = p->next;
                free(p->msg);
                free(p);
                continue;
            }
            if (now - p->last_sent >= TIMEOUT_SEC) {
                SgTcpServer_send(g_srv, p->client_fd, (uint8_t*)p->msg, p->len);
                p->last_sent = now;
                p->retries++;
            }
            pp = &p->next;
        }
        pthread_mutex_unlock(&pending_mtx);
    }
    printf("[ack_manager] exiting\n");
    return NULL;
}

/*=========================================================*/
/* Client request processing functions */

/**
 * @brief   This is the parser to parse tasks from client
 * 
 * @param   [ClientRequest *client_req]:  The task struct to pop in
 * @param   [const char *msg]:  The char message get from tcp thread
 * 
 * @return  [MsgType]:  enum to indicate which task queue to pop in
 */
static MsgType parse_client_request(ClientRequest *client_req, const char *msg){

    if (strncmp(msg, CMD_QUES, 7) == 0) {
        return MSG_BIN_QUERY;
    } else if (strncmp(msg, CMD_CLR, 7) == 0) {
        return MSG_CLR_QUERY;
    } else if (strncmp(msg, CMD_EQL, 5) == 0) {
        const char* p = msg + 5;
        int value = 0;
        if (!isdigit(*p)) return MSG_UNKNOWN;

        // 逐位解析整数
        while (isdigit(*p)) {
            value = value * 10 + (*p - '0');
            p++;
        }

        // 检查是否后续为 `;}`
        if (*p == ';' && *(p + 1) == '}') {
            if (client_req) client_req->bincount = value;
            return MSG_BIN_ACK;
        }
    }
    return MSG_UNKNOWN;
}

/**
 * @brief   This is the thread function to process bin query requests
 * 
 * @param   [void *arg]:  It requires [Ringbuffer *task_queues->bin_query_tasks]
 * 
 * @return  [NULL]
 */
void *bin_query_handler(void *arg){
    RingBuffer *queue = (RingBuffer *)arg;
    log_debug("Bin query handler task started");
    while(!thread_stop){
        ClientRequest *task = NULL;
        if (!rb_timed_pop(queue, (void**)&task, 500)) continue;
        log_debug("Handling task (%s) from client (%d)", task->message, task->client_fd);

        char str[256] = "";
        pthread_mutex_lock(&bincount_mutex);
        int length = snprintf(str, sizeof(str), "{BIN=%d;}", bincount);
        pthread_mutex_unlock(&bincount_mutex);
        SgTcpServer_send(g_srv, task->client_fd, (uint8_t*)str, length);

        free(task);
    }
    log_debug("Bin query handler exiting");
    return NULL;
}

/**
 * @brief   This is the thread function to process bin clear requests
 * 
 * @param   [void *arg]:  It requires [Ringbuffer *task_queues->bin_clr_tasks]
 * 
 * @return  [NULL]
 */
void *bin_clr_handler(void *arg){
    RingBuffer *queue = (RingBuffer *)arg;
    log_debug("Bin clear handler task started");
    while(!thread_stop){
        ClientRequest *task = NULL;
        if(!rb_timed_pop(queue, (void**)&task, 500)) continue;
        log_debug("Handling task (%s) from client (%d)", task->message, task->client_fd);

        char str[256] = "";
        pthread_mutex_lock(&bincount_mutex);
        bincount = 0;
        int res_write = write_bincount_to_file(BINCOUNT_FILE_NAME, bincount);
        int res_read = read_bincount_from_file(BINCOUNT_FILE_NAME, &bincount);
        sprintf(str, "{BIN=%d;}", bincount);
        pthread_mutex_unlock(&bincount_mutex);
        SgTcpServer_broadcast(g_srv, (const uint8_t *)str, strlen(str));
        if (!res_read || !res_read || bincount){
            log_error("Error when clearing bincounts: read-%d, write-%d, bincount-%d",res_read, res_write, bincount);
        }else{
            log_debug("Bincount cleared successfully!");
        }
        free(task);
    }
    log_debug("Bin clear handler exiting");

    return NULL;
}

/**
 * @brief   This is the thread function to process bin ack requests
 * 
 * @param   [void *arg]:  It required [Ringbuffer *task_queues->bin_ack_tasks]
 * 
 * @return  [NULL]
 */
void *bin_ack_handler(void *arg){
    RingBuffer *queue = (RingBuffer *)arg;
    log_debug("Bin ack handler task started");
    while(!thread_stop){
        ClientRequest *task = NULL;
        if(!rb_timed_pop(queue, (void**)&task, 500)) continue;
        log_debug("Handling task (%s) from client (%d)", task->message, task->client_fd);
        pthread_mutex_lock(&pending_mtx);
        for (PendingAck *p = pending_head; p; p = p->next) {
            if (p->client_fd == task->client_fd && p->bincount == task->bincount) {
                p->acked = 1; break;
            }
        }
        pthread_mutex_unlock(&pending_mtx);
        log_debug("Received ACK from client (%d), bincount (%d)", task->client_fd, task->bincount);

        free(task);
    }
    log_debug("Bin ack handler exiting");

    return NULL;
}

/**
 * @brief   This is the GPIO handler callback. Send bin lift json and reverse messages when triggered
 * 
 * @param   [const GPIOChangeInfo *info]:  GPIO info struct
 * 
 * @return  [NULL]
 */
static void gpio_change_callback(const GPIOChangeInfo *info){
    if((info->inputs[BINLIFT_PORT_NUM].changed == true) && (info->inputs[BINLIFT_PORT_NUM].rising_edge == gpio_bias[BINLIFT_PORT_NUM].enable)){
        char *bin_message = NULL;
        pthread_mutex_lock(&bincount_mutex);
        bincount++;
        write_bincount_to_file(BINCOUNT_FILE_NAME, bincount);
        bin_message = assemble_binlift_message(bincount, esn_data);
        pthread_mutex_unlock(&bincount_mutex);

        SgTcpServer_broadcast(g_srv, (uint8_t *)bin_message, strlen(bin_message));
        int *clist = NULL; size_t cnt = 0;
        if (SgTcpServer_get_clients(g_srv, &clist, &cnt) == 0) {
            for (size_t i = 0; i < cnt; i++){
                add_pending(clist[i], bin_message, bincount);
            }
            free(clist);
        }
        free(bin_message);
    }
    
    if(info->inputs[REVERSE_PORT_NUM].changed){
        char str[100];
        if(info->inputs[REVERSE_PORT_NUM].rising_edge == gpio_bias[REVERSE_PORT_NUM].enable){
            assemble_reverse_message(str, true);
        }else{
            assemble_reverse_message(str, false);
        }

        SgTcpServer_broadcast(g_srv, (uint8_t *)str, strlen(str));
    }
}

/**
 * @brief   Assemble the bin lift josn
 * 
 * @param   [const GPIOChangeInfo *info]:  GPIO info struct
 * 
 * @return  [char *json_str] JSON string
 */
static char *assemble_binlift_message(int bin_count, const char *esn) {
    cJSON *root = cJSON_CreateObject();

    cJSON_AddNumberToObject(root, "refid", bin_count);
    char esn_str[48];
    sprintf(esn_str, "sargas-%s", esn);
    cJSON_AddStringToObject(root, "id", esn_str);
    cJSON_AddStringToObject(root, "messagetype", "BINLIFT");

    cJSON *lift = cJSON_CreateObject();
    cJSON_AddNumberToObject(lift, "liftid", bin_count);
    cJSON *tags = cJSON_CreateArray();
    cJSON_AddItemToObject(lift, "tags", tags);
    cJSON_AddItemToObject(root, "lift", lift);

    char *json_str = cJSON_PrintUnformatted(root);  // 推荐用 Unformatted 节省空间
    cJSON_Delete(root);
    return json_str;
}


static void assemble_reverse_message(char *str, bool state){
    if(str){
        sprintf(str,"{REVERSE=%d;}", state && gpio_bias[2].enable);
    }
}

int start_sg_server(){
    thread_stop = 0;
    pthread_mutex_init(&bincount_mutex, NULL);

    task_queues = calloc(1, sizeof(TaskQueueSet));
    task_queues->bin_query_tasks = rb_create(128);
    task_queues->bin_clear_tasks = rb_create(128);
    task_queues->bin_ack_tasks = rb_create(128);

    // if(task_queues->bin_query_tasks != NULL && task_queues->bin_clear_tasks != NULL && task_queues->bin_ack_tasks != NULL){
        g_srv = SgTcpServer_new("0.0.0.0", SERVER_PORT, 128);
        SgTcpServer_set_on_message(g_srv, on_message, task_queues);
        SgTcpServer_start(g_srv);

        int ret1 = pthread_create(&bin_query_thread, NULL, bin_query_handler, task_queues->bin_query_tasks);
        int ret2 = pthread_create(&bin_clr_thread, NULL, bin_clr_handler, task_queues->bin_clear_tasks);
        int ret3 = pthread_create(&bin_ack_thread, NULL, bin_ack_handler, task_queues->bin_ack_tasks);
        int ret4 = pthread_create(&bin_resend_thread, NULL, ack_manager, NULL);

        set_gpio_change_callback(gpio_change_callback);
        if(!start_gpio_monitor()){
            log_error("Failed to start GPIO monitor thread");
            gpio_deinit();
            return EXIT_FAILURE;
        }

        // if(!(ret1 /*|| ret2 || ret3 || ret4*/)){
        //     log_info("Server and client request handlers started!");
        //     return EXIT_SUCCESS;
        // }
    // }

    // log_error("Failed to start server or client handlers!");
    // return EXIT_FAILURE;

    return EXIT_SUCCESS;
}

void stop_sg_server(){
    thread_stop = 1;
    log_info("Server and client handlers exiting");

    stop_gpio_monitor();
    log_info("GPIO monitor stopped");

    pthread_join(bin_query_thread, NULL);
    pthread_join(bin_clr_thread, NULL);
    pthread_join(bin_ack_thread, NULL);
    pthread_join(bin_resend_thread, NULL);

    rb_destroy(task_queues->bin_query_tasks);
    rb_destroy(task_queues->bin_clear_tasks);
    rb_destroy(task_queues->bin_ack_tasks);
    free(task_queues);

    SgTcpServer_free(g_srv);
    pthread_mutex_destroy(&bincount_mutex);

    log_info("Server and client handlers exited");
}