// sg_ws_client.h — WebSocket 客户端（基于 Mongoose 7.19）
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sg_ws_client sg_ws_client_t;
typedef struct sg_ws_conn   sg_ws_conn_t;

typedef struct {
  int max_conns;            // 最大同时连接数（默认 64）
  int poll_interval_ms;     // mg_mgr_poll 周期（默认 20ms）
  int sendq_capacity;       // 每连接发送队列容量（默认 256）
  int recvq_capacity;       // 每连接接收队列容量（默认 256）
  int connect_timeout_ms;   // 首次连接超时（默认 6000）

  // 自动重连
  int auto_reconnect;       // 1=启用；0=关闭（默认 1）
  int retry_max_attempts;   // 最大重连次数（默认 -1 无限；0=不重连）
  int retry_base_delay_ms;  // 初始退避（默认 300ms）
  int retry_max_delay_ms;   // 最大退避（默认 5000ms）
  int retry_jitter_pct;     // 抖动百分比（默认 20）

  // keepalive
  int ping_interval_ms;        // >0 则周期 Ping（默认 0 不发）
  int ping_missed_timeout_ms;  // 未见Pong的最大间隔（默认0=禁用）。需配合 ping_interval_ms>0

  // WSS(TLS)
  const char* tls_ca_path;     // CA 证书文件路径（PEM），可为 NULL
  const char* tls_cert_path;   // 客户端证书（可选，mTLS）
  const char* tls_key_path;    // 客户端私钥（可选，mTLS）
  const char* tls_server_name; // SNI/主机名校验，默认取 URL 的 host

  // Debug log lebel
  int log_level;
} sg_ws_opts;

typedef struct {
  int    is_text;     // 1=text，0=binary
  char*  data;        // malloc 持有
  size_t len;
} sg_ws_msg;

// ---- 客户端级别 ----
void sg_ws_opts_init(sg_ws_opts* o);
int  sg_ws_client_start(const sg_ws_opts* o, sg_ws_client_t** out_cli);
void sg_ws_client_stop(sg_ws_client_t* cli);

// ---- 连接级别 ----
int  sg_ws_connect(sg_ws_client_t* cli, const char* url,
                   const char* extra_headers, sg_ws_conn_t** out_conn);
int  sg_ws_wait_open(sg_ws_conn_t* conn, int timeout_ms);
int  sg_ws_send(sg_ws_conn_t* conn, const void* data, size_t len,
                int is_text, int timeout_ms);
int  sg_ws_recv(sg_ws_conn_t* conn, sg_ws_msg* out, int timeout_ms);
void sg_ws_close(sg_ws_conn_t* conn);

// 释放消息
void sg_ws_msg_free(sg_ws_msg* m);

#ifdef __cplusplus
}
#endif
