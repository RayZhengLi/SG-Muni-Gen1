// sg_http_client.h
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sg_http_client sg_http_client_t;

typedef enum {
  SG_HTTP_GET = 0,
  SG_HTTP_POST = 1,
} sg_http_method;

typedef struct {
  // 队列与轮询
  int req_capacity;         // 请求队列容量（默认 128）
  int rsp_capacity;         // 响应队列容量（默认 128）
  int max_inflight;         // 最大并发连接数（默认 16）
  int conn_timeout_ms;      // 每条连接超时（默认 6000ms）
  int poll_interval_ms;     // mg_mgr_poll 间隔（默认 50ms）
  int req_wait_ms;          // 请求队列 cond wait（默认 20ms）
  int rsp_wait_ms;          // 响应队列 cond wait（默认 500ms）

  // 重试策略（全局默认，可被每请求覆盖“次数”）
  int retry_max_attempts;   // 最大重试次数（默认 2；总尝试=1+重试）
  int retry_base_delay_ms;  // 退避初值（默认 300ms）
  int retry_max_delay_ms;   // 退避上限（默认 5000ms）
  int retry_jitter_pct;     // 抖动百分比 0~100（默认 20）

  // 触发条件（默认开启）
  int retry_on_timeout;         // 连接/响应超时
  int retry_on_network_error;   // MG_EV_ERROR 等网络错误
  int retry_on_5xx;             // HTTP 5xx
  int retry_on_429;             // HTTP 429 Too Many Requests

  // --- 新增：TLS 配置（全局默认） ---
  char *tls_ca_path;
  char *tls_cert_path;
  char *tls_key_path;
  char *tls_server_name;
} sg_http_client_opts;

typedef struct {
  int             id;           // 业务自定义 ID（唯一）
  sg_http_method  method;       // GET / POST
  const char*     url;          // 例如 "http://example.com/"
  const void*     body;         // POST body（可为 NULL）
  size_t          body_len;     // body 长度
  int             max_retries;  // 覆盖本请求最大重试次数（<0 使用全局默认）
} sg_http_request;

typedef struct {
  int      id;          // 对应请求 ID
  int      ok;          // 1=成功；0=失败（最终）
  char*    data;        // 原始 HTTP 报文（或错误信息）
  size_t   len;         // data 长度
  int      attempts;    // 实际尝试次数（1..N）
  int      http_status; // 有 HTTP 响应时的状态码，否则 0
} sg_http_response;

// -------- API --------

// 用默认值初始化配置（推荐在 start 前调用）
void sg_http_client_opts_init(sg_http_client_opts* o);

// 启动客户端（内部创建工作线程）
int  sg_http_client_start(const sg_http_client_opts* opts, sg_http_client_t** out_cli);

// 停止并销毁客户端（join 工作线程、释放资源）
void sg_http_client_stop(sg_http_client_t* cli);

// 异步提交请求：1=成功入队；0=队列满且超时；-1=客户端已关闭/错误
int  sg_http_client_submit(sg_http_client_t* cli, const sg_http_request* req, int timeout_ms);

// 异步领取响应：1=取到；0=超时无响应；-1=客户端已关闭且为空
int  sg_http_client_recv(sg_http_client_t* cli, sg_http_response* out, int timeout_ms);

// 同步便捷：提交并等待“本请求”的最终结果（含自动重试）
// 返回：1=拿到结果；0=等待超时；-1=提交失败或客户端已关闭
int  sg_http_client_send_and_wait(sg_http_client_t* cli,
                                  const sg_http_request* req,
                                  int submit_timeout_ms,
                                  int wait_timeout_ms,
                                  sg_http_response* out);

// 释放 sg_http_response 中的 data
void sg_http_response_free(sg_http_response* r);

#ifdef __cplusplus
}
#endif
