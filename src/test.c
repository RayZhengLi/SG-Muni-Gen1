// Online C compiler to run C program online
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    MSG_UNKNOWN,
    MSG_BIN_QUERY,
    MSG_CLR_QUERY,
    MSG_BIN_ACK,
    MSG_WEB_GPIO,
    MSG_WEB_GPS,
} MsgType;

typedef struct{
    MsgType type;
    char url[128];
    char body[256];
} WebRequest;

typedef struct {
  int             id;           // 业务自定义 ID（唯一）
  MsgType  method;       // GET / POST
  const char*     url;          // 例如 "http://example.com/"
  const void*     body;         // POST body（可为 NULL）
  size_t          body_len;     // body 长度
  int             max_retries;  // 覆盖本请求最大重试次数（<0 使用全局默认）
} sg_http_request;

int main(void) {
    for(int i=0; i<5; i++){
        WebRequest *req = malloc(sizeof(*req));
        if (!req) { perror("malloc"); return 1; }
    
        req->type = MSG_WEB_GPIO;
        memcpy(req->body, "Hello world\n", sizeof("Hello world\n"));
        memcpy(req->url, "www.google.com\n", sizeof("www.google.com\n"));
        
        printf("Request body: %s", req->body);
        printf("Request url: %s", req->url ? req->url : "(null)");
        printf("Request type: %d\n\n", req->type);
        
        sg_http_request task = {0};
        task.url  = req->url;
        task.body = req->body;
        printf("Request body: %s", task.body);
        printf("Request url: %s\n", task.url ? task.url : "(null)");
        // 用完后一次性释放自己拥有的资源
        free(req);
        
        printf("--------------free--------------\n");
        // printf("Request body: %s", task.body);
        // printf("Request url: %s\n", task.url);
    }
    return 0;
}
