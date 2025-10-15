// sg_ws_client.c
#define _POSIX_C_SOURCE 200809L

#include "sg-ws-client.h"
#include "mongoose.h"

#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <stdatomic.h>
#include <time.h>

// ----------------- 默认值 -----------------
#define DEF_MAX_CONNS             64
#define DEF_POLL_MS               20
#define DEF_SENDQ_CAP             256
#define DEF_RECVQ_CAP             256
#define DEF_CONN_TIMEOUT_MS       6000
#define DEF_AR_ENABLED            1
#define DEF_AR_MAX_ATTEMPTS       -1
#define DEF_AR_BASE_MS            300
#define DEF_AR_MAX_MS             5000
#define DEF_AR_JITTER_PCT         20
#define DEF_PING_INTERVAL_MS      0
#define DEF_PIN_MISSED_TIMEOUT_MS 0
#define DEF_LOG_LEVEL             4

#ifndef WEBSOCKET_OP_TEXT
#define WEBSOCKET_OP_CONTINUE   0x0
#define WEBSOCKET_OP_TEXT       0x1
#define WEBSOCKET_OP_BINARY     0x2
#define WEBSOCKET_OP_CLOSE      0x8
#define WEBSOCKET_OP_PING       0x9
#define WEBSOCKET_OP_PONG       0xA
#endif

static void ts_after_ms(clockid_t clk, struct timespec* ts, long ms) {
  clock_gettime(clk, ts);
  ts->tv_sec  += ms / 1000;
  ts->tv_nsec += (ms % 1000) * 1000000L;
  if (ts->tv_nsec >= 1000000000L) { ts->tv_sec++; ts->tv_nsec -= 1000000000L; }
}
static uint64_t now_ms(void) { return mg_millis(); }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int next_backoff_ms(int base, int cap, int jitter_pct, int attempt) {
  long d = base;
  for (int i = 0; i < attempt; ++i) { d <<= 1; if (d > cap) { d = cap; break; } }
  int jp = clampi(jitter_pct, 0, 100);
  if (jp > 0) {
    long delta = (d * jp) / 100;
    long r = (long)(rand() % (2 * delta + 1)) - delta;
    d += r; if (d < 0) d = 0;
  }
  return (int)d;
}

// ----------------- 环形队列 -----------------
typedef struct {
  void**          buf;
  int             cap, head, tail, count, closed;
  pthread_mutex_t m;
  pthread_cond_t  not_empty, not_full;
} ring_q_t;

static int ring_init(ring_q_t* q, int capacity) {
  memset(q, 0, sizeof(*q));
  q->buf = (void**) calloc((size_t)capacity, sizeof(void*));
  if (!q->buf) return -1;
  q->cap = capacity;
  pthread_mutex_init(&q->m, NULL);
  pthread_condattr_t a; pthread_condattr_init(&a);
  pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
  pthread_cond_init(&q->not_empty, &a);
  pthread_cond_init(&q->not_full,  &a);
  pthread_condattr_destroy(&a);
  return 0;
}
static void ring_close(ring_q_t* q) {
  pthread_mutex_lock(&q->m);
  q->closed = 1;
  pthread_cond_broadcast(&q->not_empty);
  pthread_cond_broadcast(&q->not_full);
  pthread_mutex_unlock(&q->m);
}
static void ring_free(ring_q_t* q) {
  if (!q) return;
  pthread_mutex_destroy(&q->m);
  pthread_cond_destroy(&q->not_empty);
  pthread_cond_destroy(&q->not_full);
  free(q->buf);
}
static int ring_push_timed(ring_q_t* q, void* item, int timeout_ms) {
  struct timespec abs;
  pthread_mutex_lock(&q->m);
  while (q->count == q->cap && !q->closed) {
    ts_after_ms(CLOCK_MONOTONIC, &abs, timeout_ms > 0 ? timeout_ms : 0);
    int rc = timeout_ms > 0 ? pthread_cond_timedwait(&q->not_full, &q->m, &abs) : 0;
    if (timeout_ms <= 0 || rc == ETIMEDOUT) { pthread_mutex_unlock(&q->m); return 0; }
  }
  if (q->closed) { pthread_mutex_unlock(&q->m); return -1; }
  q->buf[q->tail] = item;
  q->tail = (q->tail + 1) % q->cap;
  q->count++;
  pthread_cond_signal(&q->not_empty);
  pthread_mutex_unlock(&q->m);
  return 1;
}
static int ring_pop_timed(ring_q_t* q, void** out, int timeout_ms) {
  struct timespec abs;
  pthread_mutex_lock(&q->m);
  while (q->count == 0 && !q->closed) {
    ts_after_ms(CLOCK_MONOTONIC, &abs, timeout_ms > 0 ? timeout_ms : 0);
    int rc = timeout_ms > 0 ? pthread_cond_timedwait(&q->not_empty, &q->m, &abs) : 0;
    if (timeout_ms <= 0 || rc == ETIMEDOUT) { pthread_mutex_unlock(&q->m); return 0; }
  }
  if (q->count == 0 && q->closed) { pthread_mutex_unlock(&q->m); return -1; }
  *out = q->buf[q->head];
  q->buf[q->head] = NULL;
  q->head = (q->head + 1) % q->cap;
  q->count--;
  pthread_cond_signal(&q->not_full);
  pthread_mutex_unlock(&q->m);
  return 1;
}

// ----------------- WS 数据结构 -----------------
typedef struct {
  int    is_text;
  size_t len;
  char*  data;
} send_item_t;

typedef struct {
  int    is_text;
  size_t len;
  char*  data;
} recv_item_t;

typedef enum { WS_C_NEW=0, WS_C_CONNECTING, WS_C_OPEN, WS_C_CLOSING, WS_C_CLOSED } ws_state_e;

struct sg_ws_conn {
  struct sg_ws_client* cli;
  char*      url;
  char*      extra_headers;

  ring_q_t   sendq;
  ring_q_t   recvq;

  struct mg_connection* mc;

  pthread_mutex_t  m;
  pthread_cond_t   cv_state;
  ws_state_e       state;

  int      open_result;   // 1=OPEN；-1=失败；0=未知
  int      want_close;

  // 自动重连
  int   ar_enabled;
  int   ar_max_attempts;    // -1 无限
  int   ar_base_ms;
  int   ar_max_ms;
  int   ar_jitter_pct;
  int   ar_attempts;
  uint64_t ar_due_ms;

  // keepalive
  int   ping_interval_ms;
  uint64_t last_ping_ms;
  uint64_t last_pong_ms;

  struct sg_ws_conn* next;
};

typedef enum { CMD_CONNECT=1, CMD_SEND, CMD_CLOSE } cmd_type_e;
typedef struct { cmd_type_e type; struct sg_ws_conn* conn; send_item_t* send_item; } cmd_item_t;

struct sg_ws_client {
  struct mg_mgr  mgr;
  pthread_t      worker;
  atomic_int     running;

  int poll_ms;
  int connect_timeout_ms;

  // 默认/上限
  int max_conns;
  int sendq_cap;
  int recvq_cap;

  // 自动重连默认
  int ar_enabled;
  int ar_max_attempts;
  int ar_base_ms;
  int ar_max_ms;
  int ar_jitter_pct;

  int ping_interval_ms;
  int ping_miss_timeout_ms;

  // TLS 选项
  char* tls_ca_path;
  char* tls_cert_path;
  char* tls_key_path;
  char* tls_server_name;

  int mgr_log_level;

  ring_q_t cmdq;
  struct sg_ws_conn* conns;   // 仅 worker 遍历
};

// ----------------- 回调 -----------------
static void ws_handler(struct mg_connection* c, int ev, void* ev_data) {
  struct sg_ws_conn* conn = (struct sg_ws_conn*) c->fn_data;
  if (!conn) return;

  switch (ev) {
    case MG_EV_CONNECT: {
      const char* url = conn->url ? conn->url : "";
      if (mg_url_is_ssl(url)) {
        struct mg_tls_opts topts; memset(&topts, 0, sizeof(topts));
        struct mg_str ca   = mg_str_n(NULL, 0);
        struct mg_str cert = mg_str_n(NULL, 0);
        struct mg_str key  = mg_str_n(NULL, 0);
        if (conn->cli->tls_ca_path)   ca   = mg_file_read(&mg_fs_posix, conn->cli->tls_ca_path);
        if (conn->cli->tls_cert_path) cert = mg_file_read(&mg_fs_posix, conn->cli->tls_cert_path);
        if (conn->cli->tls_key_path)  key  = mg_file_read(&mg_fs_posix, conn->cli->tls_key_path);

        topts.ca   = ca;
        topts.cert = cert;
        topts.key  = key;

        if (conn->cli->tls_server_name && *conn->cli->tls_server_name) {
          topts.name = mg_str(conn->cli->tls_server_name);  // ✅ 统一 mg_str
        } else {
          struct mg_str host = mg_url_host(url);
          char* host_c = host.len ? mg_mprintf("%.*s", (int)host.len, host.buf) : NULL;
          topts.name = mg_str(host_c);                      // 我们分配的，init后释放
        }

        mg_tls_init(c, &topts);

        if (!conn->cli->tls_server_name && topts.name.buf) mg_free((void*) topts.name.buf);
        if (ca.buf)   mg_free((void*) ca.buf);
        if (cert.buf) mg_free((void*) cert.buf);
        if (key.buf)  mg_free((void*) key.buf);
      }
    } break;


    case MG_EV_WS_OPEN: {
      pthread_mutex_lock(&conn->m);
      conn->state = WS_C_OPEN;
      conn->open_result = 1;
      conn->mc = c;
      conn->ar_attempts = 0;
      conn->last_ping_ms = now_ms();
      conn->last_pong_ms = conn->last_ping_ms;   // 新增：OPEN时刷新last_pong_ms
      pthread_cond_broadcast(&conn->cv_state);
      pthread_mutex_unlock(&conn->m);
    } break;

    case MG_EV_WS_MSG: {
      struct mg_ws_message* wm = (struct mg_ws_message*) ev_data;

      // 先取 opcode
      uint8_t op = (uint8_t)(wm->flags & 0x0F);

      if (op == WEBSOCKET_OP_PONG) {
        // 收到 PONG：刷新存活时间
        conn->last_pong_ms = now_ms();
        break;  // 不投递到业务队列
      }
      if (op == WEBSOCKET_OP_PING) {
        // 收到对端 PING：立即回 PONG（Mongoose 不会自动回）
        mg_ws_send(c, wm->data.buf, wm->data.len, WEBSOCKET_OP_PONG);
        conn->last_pong_ms = now_ms();
        break;
      }

      // 普通数据帧：入接收队列
      recv_item_t* it = (recv_item_t*) calloc(1, sizeof(recv_item_t));
      if (!it) break;
      it->is_text = (op == WEBSOCKET_OP_TEXT) ? 1 : 0;
      it->len = wm->data.len;
      it->data = (char*) malloc(wm->data.len);
      if (it->data && wm->data.len) memcpy(it->data, wm->data.buf, wm->data.len);
      (void) ring_push_timed(&conn->recvq, it, 200);

       conn->last_pong_ms = now_ms();  // 任意入站帧”视为存活
    } break;

    case MG_EV_POLL: {
      uint64_t nms = now_ms();

      if (conn->state == WS_C_OPEN) {
        // 1) flush 发送队列（只在 OPEN 后）
        send_item_t* it = NULL;
        while (ring_pop_timed(&conn->sendq, (void**)&it, 0) == 1 && it) {
          int op = it->is_text ? WEBSOCKET_OP_TEXT : WEBSOCKET_OP_BINARY;
          mg_ws_send(c, it->data, it->len, op);
          free(it->data); free(it);
        }

        // 2) 周期 Ping（只在 OPEN 后）
        if (conn->ping_interval_ms > 0 &&
            nms - conn->last_ping_ms >= (uint64_t) conn->ping_interval_ms) {
          mg_ws_send(c, "", 0, WEBSOCKET_OP_PING);
          conn->last_ping_ms = nms;
        }

        // 3) 未见 Pong 超时（只在 OPEN 后才判）
        if (conn->cli->ping_miss_timeout_ms > 0 &&
            nms - conn->last_pong_ms >= (uint64_t) conn->cli->ping_miss_timeout_ms) {
          c->is_closing = 1;  // 触发自动重连
        }
      }
    } break;

    case MG_EV_CLOSE: {
      pthread_mutex_lock(&conn->m);
      conn->mc = NULL;
      if (conn->state != WS_C_CLOSING) conn->state = WS_C_CLOSED;
      if (conn->open_result == 0) conn->open_result = -1;
      pthread_cond_broadcast(&conn->cv_state);
      pthread_mutex_unlock(&conn->m);
    } break;

    case MG_EV_ERROR: {
      (void) ev_data; // 自动重连由 worker 循环调度
    } break;

    default: break;
  }
}

// ----------------- Worker 命令 -----------------
static void do_cmd_connect(struct sg_ws_client* cli, struct sg_ws_conn* conn) {
  conn->state = WS_C_CONNECTING;
  conn->open_result = 0;
  conn->mc = NULL;
  // 重置心跳相关的基线，避免新连接在握手期被“未见Pong”误杀
  uint64_t n = now_ms();
  conn->last_ping_ms = n;
  conn->last_pong_ms = n;

  struct mg_connection* c =
    mg_ws_connect(&cli->mgr, conn->url, ws_handler, conn,
                  "%s", conn->extra_headers ? conn->extra_headers : "");
  if (c == NULL) {
    pthread_mutex_lock(&conn->m);
    conn->state = WS_C_CLOSED;
    conn->open_result = -1;
    pthread_cond_broadcast(&conn->cv_state);
    pthread_mutex_unlock(&conn->m);
  }
}

static void do_cmd_send(struct sg_ws_conn* conn, send_item_t* it) {
  (void) ring_push_timed(&conn->sendq, it, 200);
}

static void do_cmd_close(struct sg_ws_conn* conn) {
  pthread_mutex_lock(&conn->m);
  conn->want_close = 1;
  if (conn->mc) conn->mc->is_closing = 1;
  pthread_mutex_unlock(&conn->m);
}

// ----------------- Worker -----------------
static void* worker_thread(void* arg) {
  struct sg_ws_client* cli = (struct sg_ws_client*) arg;
  mg_log_set(cli->mgr_log_level);
  mg_mgr_init(&cli->mgr);

  while (atomic_load(&cli->running)) {
    // 1) 命令
    cmd_item_t* cmd = NULL;
    while (ring_pop_timed(&cli->cmdq, (void**)&cmd, 0) == 1 && cmd) {
      switch (cmd->type) {
        case CMD_CONNECT: do_cmd_connect(cli, cmd->conn); break;
        case CMD_SEND:    do_cmd_send(cmd->conn, cmd->send_item); break;
        case CMD_CLOSE:   do_cmd_close(cmd->conn); break;
        default: break;
      }
      free(cmd);
    }

    // 2) 自动重连
    uint64_t n = now_ms();
    for (struct sg_ws_conn* p = cli->conns; p; p = p->next) {
      pthread_mutex_lock(&p->m);
      int need_reconnect = 0;
      if (p->state == WS_C_CLOSED && !p->want_close && p->ar_enabled) {
        if (p->ar_max_attempts < 0 || p->ar_attempts < p->ar_max_attempts) {
          if (p->ar_due_ms == 0 || n >= p->ar_due_ms) need_reconnect = 1;
        }
      }
      pthread_mutex_unlock(&p->m);

      if (need_reconnect) {
        int delay = next_backoff_ms(p->ar_base_ms, p->ar_max_ms, p->ar_jitter_pct, p->ar_attempts);
        p->ar_attempts++;
        p->ar_due_ms = n + (uint64_t) delay;

        cmd_item_t* cmd2 = (cmd_item_t*) calloc(1, sizeof(cmd_item_t));
        if (cmd2) { cmd2->type = CMD_CONNECT; cmd2->conn = p; ring_push_timed(&cli->cmdq, cmd2, 50); }
      }
    }

    mg_mgr_poll(&cli->mgr, cli->poll_ms);
  }

  mg_mgr_free(&cli->mgr);
  return NULL;
}

// ----------------- 公共 API -----------------
void sg_ws_opts_init(sg_ws_opts* o) {
  if (!o) return;
  memset(o, 0, sizeof(*o));
  o->max_conns = DEF_MAX_CONNS;
  o->poll_interval_ms = DEF_POLL_MS;
  o->sendq_capacity = DEF_SENDQ_CAP;
  o->recvq_capacity = DEF_RECVQ_CAP;
  o->connect_timeout_ms = DEF_CONN_TIMEOUT_MS;

  o->auto_reconnect = DEF_AR_ENABLED;
  o->retry_max_attempts = DEF_AR_MAX_ATTEMPTS;
  o->retry_base_delay_ms = DEF_AR_BASE_MS;
  o->retry_max_delay_ms = DEF_AR_MAX_MS;
  o->retry_jitter_pct   = DEF_AR_JITTER_PCT;

  o->ping_interval_ms = DEF_PING_INTERVAL_MS;
  o->ping_missed_timeout_ms = DEF_PIN_MISSED_TIMEOUT_MS;

  o->tls_ca_path = NULL;
  o->tls_cert_path = NULL;
  o->tls_key_path  = NULL;
  o->tls_server_name = NULL;
}

int sg_ws_client_start(const sg_ws_opts* o, sg_ws_client_t** out_cli) {
  if (!out_cli) return -1;
  struct sg_ws_client* cli = (struct sg_ws_client*) calloc(1, sizeof(*cli));
  if (!cli) return -2;

  cli->max_conns  = (o && o->max_conns > 0) ? o->max_conns : DEF_MAX_CONNS;
  cli->poll_ms    = (o && o->poll_interval_ms > 0) ? o->poll_interval_ms : DEF_POLL_MS;
  cli->sendq_cap  = (o && o->sendq_capacity > 0) ? o->sendq_capacity : DEF_SENDQ_CAP;
  cli->recvq_cap  = (o && o->recvq_capacity > 0) ? o->recvq_capacity : DEF_RECVQ_CAP;
  cli->connect_timeout_ms = (o && o->connect_timeout_ms > 0) ? o->connect_timeout_ms : DEF_CONN_TIMEOUT_MS;

  cli->ar_enabled       = (o ? o->auto_reconnect : DEF_AR_ENABLED);
  cli->ar_max_attempts  = (o ? o->retry_max_attempts : DEF_AR_MAX_ATTEMPTS);
  cli->ar_base_ms       = (o ? o->retry_base_delay_ms : DEF_AR_BASE_MS);
  cli->ar_max_ms        = (o ? o->retry_max_delay_ms  : DEF_AR_MAX_MS);
  cli->ar_jitter_pct    = (o ? o->retry_jitter_pct    : DEF_AR_JITTER_PCT);
  cli->ping_interval_ms = (o ? o->ping_interval_ms    : DEF_PING_INTERVAL_MS);
  cli->ping_miss_timeout_ms = (o ? o->ping_missed_timeout_ms : DEF_PIN_MISSED_TIMEOUT_MS);

  cli->tls_ca_path     = o && o->tls_ca_path     ? strdup(o->tls_ca_path)     : NULL;
  cli->tls_cert_path   = o && o->tls_cert_path   ? strdup(o->tls_cert_path)   : NULL;
  cli->tls_key_path    = o && o->tls_key_path    ? strdup(o->tls_key_path)    : NULL;
  cli->tls_server_name = o && o->tls_server_name ? strdup(o->tls_server_name) : NULL;

  cli->mgr_log_level   =  (o ? o->log_level    : DEF_PING_INTERVAL_MS);

  if (ring_init(&cli->cmdq, 1024) != 0) { free(cli); return -3; }

  srand((unsigned) time(NULL));
  atomic_store(&cli->running, 1);
  if (pthread_create(&cli->worker, NULL, worker_thread, cli) != 0) {
    ring_free(&cli->cmdq);
    free(cli->tls_ca_path); free(cli->tls_cert_path);
    free(cli->tls_key_path); free(cli->tls_server_name);
    free(cli); return -4;
  }

  *out_cli = cli;
  return 0;
}

void sg_ws_client_stop(sg_ws_client_t* cli) {
  if (!cli) return;
  atomic_store(&cli->running, 0);
  ring_close(&cli->cmdq);
  pthread_join(cli->worker, NULL);

  // 释放连接链
  struct sg_ws_conn* p = cli->conns;
  while (p) {
    struct sg_ws_conn* n = p->next;
    ring_close(&p->sendq); ring_close(&p->recvq);
    void* v = NULL;
    while (ring_pop_timed(&p->recvq, &v, 0) == 1) { recv_item_t* it = (recv_item_t*) v; free(it->data); free(it); }
    while (ring_pop_timed(&p->sendq, &v, 0) == 1) { send_item_t* it = (send_item_t*) v; free(it->data); free(it); }
    ring_free(&p->sendq); ring_free(&p->recvq);
    pthread_cond_destroy(&p->cv_state);
    pthread_mutex_destroy(&p->m);
    free(p->url); free(p->extra_headers);
    free(p);
    p = n;
  }

  ring_free(&cli->cmdq);

  free(cli->tls_ca_path); free(cli->tls_cert_path);
  free(cli->tls_key_path); free(cli->tls_server_name);
  free(cli);
}

int sg_ws_connect(sg_ws_client_t* cli, const char* url,
                  const char* extra_headers, sg_ws_conn_t** out_conn) {
  if (!cli || !url || !out_conn) return -1;

  struct sg_ws_conn* c = (struct sg_ws_conn*) calloc(1, sizeof(*c));
  if (!c) return -2;
  c->cli = cli;
  c->url = strdup(url);
  c->extra_headers = extra_headers ? strdup(extra_headers) : NULL;
  c->state = WS_C_NEW;
  pthread_mutex_init(&c->m, NULL);
  pthread_condattr_t a; pthread_condattr_init(&a);
  pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
  pthread_cond_init(&c->cv_state, &a);
  pthread_condattr_destroy(&a);

  if (ring_init(&c->sendq, cli->sendq_cap) != 0 || ring_init(&c->recvq, cli->recvq_cap) != 0) {
    ring_free(&c->sendq); ring_free(&c->recvq);
    pthread_cond_destroy(&c->cv_state); pthread_mutex_destroy(&c->m);
    free(c->url); free(c->extra_headers); free(c);
    return -3;
  }

  c->ar_enabled      = cli->ar_enabled;
  c->ar_max_attempts = cli->ar_max_attempts;
  c->ar_base_ms      = cli->ar_base_ms;
  c->ar_max_ms       = cli->ar_max_ms;
  c->ar_jitter_pct   = cli->ar_jitter_pct;
  c->ping_interval_ms= cli->ping_interval_ms;
  c->last_pong_ms    = now_ms();
  c->last_ping_ms    = now_ms();

  c->next = cli->conns; cli->conns = c;

  cmd_item_t* cmd = (cmd_item_t*) calloc(1, sizeof(cmd_item_t));
  if (!cmd) return -4;
  cmd->type = CMD_CONNECT; cmd->conn = c;
  ring_push_timed(&cli->cmdq, cmd, 100);

  *out_conn = c;
  return 0;
}

int sg_ws_wait_open(sg_ws_conn_t* conn, int timeout_ms) {
  if (!conn) return -1;
  struct timespec abs;
  pthread_mutex_lock(&conn->m);
  while (conn->state != WS_C_OPEN && conn->open_result == 0) {
    ts_after_ms(CLOCK_MONOTONIC, &abs, timeout_ms > 0 ? timeout_ms : 0);
    int rc = (timeout_ms > 0) ? pthread_cond_timedwait(&conn->cv_state, &conn->m, &abs) : 0;
    if (timeout_ms <= 0 || rc == ETIMEDOUT) { pthread_mutex_unlock(&conn->m); return 0; }
  }
  int ret = (conn->state == WS_C_OPEN && conn->open_result == 1) ? 1 : -1;
  pthread_mutex_unlock(&conn->m);
  return ret;
}

int sg_ws_send(sg_ws_conn_t* conn, const void* data, size_t len, int is_text, int timeout_ms) {
  if (!conn || !data || len == 0) return -1;
  send_item_t* it = (send_item_t*) calloc(1, sizeof(send_item_t));
  if (!it) return -1;
  it->is_text = is_text ? 1 : 0;
  it->len = len;
  it->data = (char*) malloc(len);
  if (!it->data) { free(it); return -1; }
  memcpy(it->data, data, len);

  cmd_item_t* cmd = (cmd_item_t*) calloc(1, sizeof(cmd_item_t));
  if (!cmd) { free(it->data); free(it); return -1; }
  cmd->type = CMD_SEND; cmd->conn = conn; cmd->send_item = it;

  int rc = ring_push_timed(&conn->cli->cmdq, cmd, timeout_ms);
  if (rc <= 0) { free(it->data); free(it); free(cmd); }
  return rc;
}

int sg_ws_recv(sg_ws_conn_t* conn, sg_ws_msg* out, int timeout_ms) {
  if (!conn || !out) return -1;
  void* v = NULL;
  int rc = ring_pop_timed(&conn->recvq, &v, timeout_ms);
  if (rc == 1 && v) {
    recv_item_t* it = (recv_item_t*) v;
    out->is_text = it->is_text; out->len = it->len; out->data = it->data;
    free(it);
    return 1;
  }
  return rc;
}

void sg_ws_close(sg_ws_conn_t* conn) {
  if (!conn) return;
  conn->ar_enabled = 0;
  cmd_item_t* cmd = (cmd_item_t*) calloc(1, sizeof(cmd_item_t));
  if (cmd) { cmd->type = CMD_CLOSE; cmd->conn = conn; ring_push_timed(&conn->cli->cmdq, cmd, 100); }

  struct timespec abs;
  pthread_mutex_lock(&conn->m);
  while (conn->state != WS_C_CLOSED) {
    ts_after_ms(CLOCK_MONOTONIC, &abs, 2000);
    pthread_cond_timedwait(&conn->cv_state, &conn->m, &abs);
    break;
  }
  pthread_mutex_unlock(&conn->m);
}

void sg_ws_msg_free(sg_ws_msg* m) {
  if (!m) {
    return;
  } 
  free(m->data); m->data = NULL; m->len = 0;
}
