#include "sg-comm.h"
#include "sg-tcp.h"
#include "sg-gpio.h"
#include "sg-ringbuffer.h"
#include "sg-log.h"
#include "sg-gpio-monitor.h"
#include "sg-http-client.h"
#include "sg-file-ringbuffer.h"
#include "sg-gps.h"
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

#define WEB_REQUEST_MSG_TYPE 1
#define WEB_SERVER_URL "https://httpbin.org/post"

static PendingAck      *pending_head  = NULL;
static pthread_mutex_t  pending_mtx   = PTHREAD_MUTEX_INITIALIZER;
static TaskQueueSet    *task_queues   = NULL;   // CAUTION: the tasks in this queue must be freed in [task threads] and should not be freed in [on_message] function
                                                // CAUTION: this ringbuffer should not be freed before end of the whole programe
static pthread_t bin_query_thread;
static pthread_t bin_clr_thread;
static pthread_t bin_ack_thread;
static pthread_t bin_resend_thread;
static pthread_t web_client_thread;
static pthread_t gps_thread;

static int       thread_stop          = 1;

static sg_http_client_t *web_cli = NULL;        // The HTTP(s) client for Asgard X

static FileRingBuffer frb;

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
    pipeline->web_client_tasks = rb_create(128);

    if(pipeline->bin_ack_tasks && pipeline->bin_clear_tasks && pipeline->bin_query_tasks && pipeline->web_client_tasks){
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

void gps_handler(void *arg){
    RingBuffer *queue = (RingBuffer *)arg;
    log_debug("GPS handler task started");
    while(!thread_stop){
        WebRequest *web_task = calloc(1, sizeof(WebRequest));
        web_task->type = MSG_WEB_GPS;
        strncpy(web_task->url, WEB_SERVER_URL, sizeof(web_task->url)-1);
        GPSData loc;
        memset(&loc, 0, sizeof(loc));
        char *web_gpio_json = assemble_web_gps_message(esn_data);
        if(web_gpio_json){
            strncpy(web_task->body, web_gpio_json, strlen(web_gpio_json));
            web_task->retry_msg = false;
            if(!rb_timed_push(queue, web_task, 100)){
                log_error("Failed to push web gps task to the queue, dropping");
                free(web_task);
            }else{
                log_debug("Web gps task has been pushed to the queue");
            }   
            free(web_gpio_json);
        }
        sleep(1);
    }
}

void *web_client_handler(void *arg){
    RingBuffer *queue = (RingBuffer *)arg;
    log_debug("Web client handler task started");

    // Init the web client here
    sg_http_client_opts opts;
    sg_http_client_opts_init(&opts);
    opts.max_inflight = 16;
    opts.retry_max_attempts = 5;
    opts.conn_timeout_ms = 10000;
    opts.tls_ca_path = "/etc/ssl/certs/ca-certificates.crt";

    if (sg_http_client_start(&opts, &web_cli) != 0) {
        log_error("Failed to start HTTP client");
        return NULL;
    }
    log_info("HTTP client started");
    while(!thread_stop){
        WebRequest *task = NULL;
        static int req_id = 1;
        if(!rb_timed_pop(queue, (void**)&task, 500)){
            // Check if there are failed requests in the local file ring buffer
            log_info("No web client task in the queue, checking old failed requests in the file buffer");
            uint8_t *buf = (uint8_t*)malloc(sizeof(WebRequest));
            if (!buf) { log_error("malloc failed in file buffer read"); return NULL; }
            uint16_t type = 0;
            size_t need = sizeof(WebRequest);
            int rc = file_ringbuffer_peek_bytes(&frb, buf, need, type);
            switch(rc){
                case 0:{
                    if(type == WEB_REQUEST_MSG_TYPE && need == sizeof(WebRequest)){
                        WebRequest *failed_req = (WebRequest*)buf;
                        rb_timed_push(queue, failed_req, 100);
                        log_info("A failed web request type %d has been read from the local file and pushed to the web client queue", type);
                    }else{
                        log_error("Invalid data in the local file, type %d, size %zu", type, need);
                        free(buf);
                    }
                    break;
                }
                case -1:
                    log_error("File buffer read error");
                    free(buf);
                    break;
                case -5:
                    log_info("File buffer too small, need %zu bytes", need);
                    free(buf);
                    break;
                default:
                    free(buf);
                    break;
            }
            continue;
        } 
        log_debug("Handling web request task (%s) to URL (%s)", task->body, task->url);
        if(web_cli){
            sg_http_request req = {0};
            if(req_id >= INT32_MAX){
                req_id = 0;
            }
            req.id = req_id++;
            switch (task->type){
                case MSG_WEB_GPIO:
                    req.method = SG_HTTP_POST;
                    req.url    = task->url;
                    req.body   = task->body;
                    req.body_len = strlen(task->body);
                    req.max_retries = 3;
                    break;
                case MSG_WEB_GPS:
                    req.method = SG_HTTP_POST;
                    req.url    = task->url;
                    req.body   = task->body;
                    req.body_len = strlen(task->body);
                    req.max_retries = 3;
                    break;
            }
            sg_http_response rsp = {0};
            int ok = sg_http_client_send_and_wait(web_cli, &req, 1000, 5000, &rsp);
            if(ok == 1){
                char output[2048];
                sprintf(output, "Received response\n"
                                "---------------------------------------------\n"
                                "> [WEB-CLIENT-RECV] id=%d ok=%d http=%d attempts=%d len=%zu <\n"
                                "%s\n"
                                "---------------------------------------------\n", rsp.id, rsp.ok, rsp.http_status, rsp.attempts, rsp.len, rsp.data);
                log_info("%s", output);
                sg_http_response_free(&rsp);
                // If this is a retry message, delete it from the file buffer
                if(task->retry_msg){
                    file_ringbuffer_delete(&frb);
                }
            }else {
                log_error("Failed to get response for request id=%d", req.id);
                if(task->retry_msg){
                    log_error("The retry message failed again, do not delete it");
                }
                // Stored the send failed request to local file ring buffer
                task->retry_msg = true;
                if(file_ringbuffer_write_bytes_timed(&frb, task, sizeof(WebRequest), WEB_REQUEST_MSG_TYPE, 100) != 0){
                    log_error("Failed to store the failed web request to the local file");
                }else{
                    log_info("Failed web request type %d has been stored to the local file",task->type);
                }
            }
            free(task);
        }
    }
    sg_http_client_stop(web_cli);
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
    // Assemble the web client message
    char *web_gpio_json = assemble_web_gpio_message(esn_data, info);
    if(web_gpio_json){
        WebRequest *web_task = calloc(1, sizeof(WebRequest));
        web_task->type = MSG_WEB_GPIO;
        strncpy(web_task->url, WEB_SERVER_URL, sizeof(web_task->url)-1);
        strncpy(web_task->body, web_gpio_json, strlen(web_gpio_json));
        web_task->retry_msg = false;
        if(!rb_timed_push(task_queues->web_client_tasks, web_task, 100)){
            log_error("Failed to push web gpio task to the queue, dropping");
            free(web_task);
        }else{
            log_debug("Web gpio task has been pushed to the queue");
        }   
        free(web_gpio_json);
    }

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
 * @brief   Assemble the gpio josn for web client
 * 
 * @param   const [GPIOChangeInfo *info]:  GPIO info struct
 * @param   const [char *]esn: The device esn number
 * @return  [char *json_str]: JSON string
 */
static char *assemble_web_gpio_message(const char *esn, const GPIOChangeInfo *info){
    cJSON *root = cJSON_CreateObject();

    char esn_str[48];
    sprintf(esn_str, "sargas-%s", esn);
    cJSON_AddStringToObject(root, "id", esn_str);

    // Gnerate a UUID
    char uuid_str[37];
    uuid_t uuid;
    uuid_generate(uuid);
    uuid_unparse(uuid, uuid_str);
    cJSON_AddStringToObject(root, "uuid", uuid_str);

    // Get current time stamp
    time_t now = time(NULL);
    if(now == (time_t)-1){
        log_error("Failed to get timestamp");
        cJSON_AddNumberToObject(root, "timestamp", 0);
    }
    cJSON_AddNumberToObject(root, "timestamp", now);

    cJSON_AddStringToObject(root, "event", "input");

    cJSON *payload = cJSON_CreateObject();
    for (int i = 0; i < MAX_INPUTS; ++i) {
        if(info->inputs[i].changed){
            cJSON *p_obj = cJSON_CreateObject();
            if(info->inputs[i].rising_edge == gpio_bias[i].enable){
                cJSON_AddNumberToObject(p_obj, "value",1);
            }else{
                cJSON_AddNumberToObject(p_obj, "value",0);
            }
            cJSON_AddNumberToObject(p_obj, "value", info->inputs[i].rising_edge ? 1 : 0);
            cJSON_AddNumberToObject(p_obj, "count", info->inputs[i].counter);
            char port_str[8];
            sprintf(port_str, "port%d", i);
            cJSON_AddItemToObject(payload, port_str, p_obj);
        }
    }
    cJSON_AddItemToObject(root, "payload", payload);
    cJSON *location = cJSON_CreateObject();
    GPSData loc;
    memset(&loc, 0, sizeof(loc));
    if(gps_get_current_location(&loc)){
        cJSON_AddNumberToObject(location, "lat", loc.lat);
        cJSON_AddNumberToObject(location, "lon", loc.lon);
        cJSON_AddNumberToObject(location, "alt", loc.alt);
        cJSON_AddNumberToObject(location, "speed", loc.speed);
        cJSON_AddNumberToObject(location, "heading", loc.heading);
        cJSON_AddNumberToObject(location, "hdop", loc.hdop);
        cJSON_AddNumberToObject(location, "nsats", loc.nsats);
        cJSON_AddNumberToObject(location, "fixstatus", loc.fixstatus);
    }
    cJSON_AddItemToObject(root, "location", location);
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    return json_str;
}

/**
 * @brief   Assemble the GPS josn for web client
 * 
 * @param   const [char *]esn: The device esn number
 * @return  [char *json_str]: JSON string
 */
static char *assemble_web_gps_message(const char *esn){
    cJSON *root = cJSON_CreateObject();

    char esn_str[48];
    sprintf(esn_str, "sargas-%s", esn);
    cJSON_AddStringToObject(root, "id", esn_str);

    // Gnerate a UUID
    char uuid_str[37];
    uuid_t uuid;
    uuid_generate(uuid);
    uuid_unparse(uuid, uuid_str);
    cJSON_AddStringToObject(root, "uuid", uuid_str);

    // Get current time stamp
    time_t now = time(NULL);
    if(now == (time_t)-1){
        log_error("Failed to get timestamp");
        cJSON_AddNumberToObject(root, "timestamp", 0);
    }
    cJSON_AddNumberToObject(root, "timestamp", now);

    cJSON_AddStringToObject(root, "event", "gps");
    cJSON *payload = cJSON_CreateObject();
    GPSData loc;
    memset(&loc, 0, sizeof(loc));
    if(gps_get_current_location(&loc)){
        cJSON_AddNumberToObject(payload, "lat", loc.lat);
        cJSON_AddNumberToObject(payload, "lon", loc.lon);
        cJSON_AddNumberToObject(payload, "alt", loc.alt);
        cJSON_AddNumberToObject(payload, "speed", loc.speed);
        cJSON_AddNumberToObject(payload, "heading", loc.heading);
        cJSON_AddNumberToObject(payload, "hdop", loc.hdop);
        cJSON_AddNumberToObject(payload, "nsats", loc.nsats);
        cJSON_AddNumberToObject(payload, "fixstatus", loc.fixstatus);
    }
    cJSON_AddItemToObject(root, "payload", payload);
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    return json_str;
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
    // Initialise the file ring buffer
    if (file_ringbuffer_init(&frb, 5000, "failed_req.dat") != 0) {
        printf("Failed to initialize buffer.\n");
        return -1;
    } 
    file_ringbuffer_enable_fsync(&frb, true);

    thread_stop = 0;
    pthread_mutex_init(&bincount_mutex, NULL);

    task_queues = calloc(1, sizeof(TaskQueueSet));
    task_queues->bin_query_tasks  = rb_create(128);
    task_queues->bin_clear_tasks  = rb_create(128);
    task_queues->bin_ack_tasks    = rb_create(128);
    task_queues->web_client_tasks = rb_create(128);

    if(task_queues->bin_query_tasks != NULL && task_queues->bin_clear_tasks != NULL && task_queues->bin_ack_tasks != NULL){
        g_srv = SgTcpServer_new("0.0.0.0", SERVER_PORT, 128);
        SgTcpServer_set_on_message(g_srv, on_message, task_queues);
        SgTcpServer_start(g_srv);

        int ret1 = pthread_create(&bin_query_thread, NULL, bin_query_handler, task_queues->bin_query_tasks);
        int ret2 = pthread_create(&bin_clr_thread, NULL, bin_clr_handler, task_queues->bin_clear_tasks);
        int ret3 = pthread_create(&bin_ack_thread, NULL, bin_ack_handler, task_queues->bin_ack_tasks);
        int ret4 = pthread_create(&bin_resend_thread, NULL, ack_manager, NULL);
        int ret5 = pthread_create(&web_client_thread, NULL, web_client_handler, task_queues->web_client_tasks);
        int ret5 = pthread_create(&gps_thread, NULL, gps_handler, task_queues->web_client_tasks);

        set_gpio_change_callback(gpio_change_callback);
        if(!start_gpio_monitor()){
            log_error("Failed to start GPIO monitor thread");
            gpio_deinit();
            return EXIT_FAILURE;
        }

        if(!(ret1 || ret2 || ret3 || ret4)){
            log_info("Server and client request handlers started!");
            return EXIT_SUCCESS;
        }
    }

    log_error("Failed to start server or client handlers!");
    return EXIT_FAILURE;
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
    pthread_join(web_client_thread, NULL);
    pthread_join(gps_thread, NULL);

    rb_destroy(task_queues->bin_query_tasks);
    rb_destroy(task_queues->bin_clear_tasks);
    rb_destroy(task_queues->bin_ack_tasks);
    rb_destroy(task_queues->web_client_tasks);
    free(task_queues);

    file_ringbuffer_destroy(&frb);

    SgTcpServer_free(g_srv);
    pthread_mutex_destroy(&bincount_mutex);

    log_info("Server and client handlers exited");
}