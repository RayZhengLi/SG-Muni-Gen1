// sg_http_client.c
// 构建建议： -pthread -std=c11 -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L
// 老系统若 clock_gettime 链接报错，可再加 -lrt

#define _POSIX_C_SOURCE 200809L
#include "sg-http-client.h"
#include "mongoose.h"

#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <stdatomic.h>

// ----------------- 默认值 -----------------
#define DEF_REQ_CAP            128
#define DEF_RSP_CAP            128
#define DEF_MAX_INFLIGHT        16
#define DEF_CONN_TIMEOUT_MS   6000
#define DEF_POLL_INTERVAL_MS    50
#define DEF_REQ_WAIT_MS          20
#define DEF_RSP_WAIT_MS         500

#define DEF_RETRY_MAX_ATTEMPTS    2   // 重试 2 次 => 最多尝试 3 次
#define DEF_RETRY_BASE_MS       300
#define DEF_RETRY_MAX_MS       5000
#define DEF_RETRY_JITTER_PCT     20
#define DEF_RETRY_ON_TIMEOUT      1
#define DEF_RETRY_ON_NETERR       1
#define DEF_RETRY_ON_5XX          1
#define DEF_RETRY_ON_429          1

// ----------------- MONOTONIC 绝对时刻 -----------------
static void ts_after_ms(clockid_t clk, struct timespec* ts, long ms) {
  clock_gettime(clk, ts);
  ts->tv_sec  += ms / 1000;
  ts->tv_nsec += (ms % 1000) * 1000000L;
  if (ts->tv_nsec >= 1000000000L) { ts->tv_sec++; ts->tv_nsec -= 1000000000L; }
}

static uint64_t now_ms(void) { return mg_millis(); }  // 受 -D_GNU_SOURCE 影响，优先 MONOTONIC_RAW

// ----------------- 环形队列（请求/响应） -----------------
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

// ----------------- 请求/响应的内部项 -----------------
typedef struct {
  int            id;
  sg_http_method method;
  char*          url;      // 深拷贝
  void*          body;     // 深拷贝
  size_t         body_len;
  int            max_retries;  // 覆盖值（<0 使用全局默认）
} req_item_t;

typedef struct {
  int     id;
  int     ok;
  char*   data;
  size_t  len;
  int     attempts;
  int     http_status;
} rsp_item_t;

// ----------------- 重试调度 -----------------
struct retry_item {
  req_item_t            req;       // 深拷贝
  int                   attempt;   // 已尝试次数（下次 = attempt+1）
  uint64_t              due_ms;    // 执行时间
  struct retry_item*    next;
};

// ----------------- 等待者：按 id 精准唤醒 -----------------
typedef struct waiter {
  int id;
  pthread_mutex_t m;
  pthread_cond_t  cv;
  int ready;                // 1=已有结果
  sg_http_response resp;    // data 归调用方
  struct waiter* next;
} waiter_t;

// ----------------- 客户端句柄 -----------------
struct sg_http_client {
  // 队列
  ring_q_t        reqq;
  ring_q_t        rspq;

  // 并发与轮询
  int             max_inflight;
  int             conn_timeout_ms;
  int             poll_interval_ms;
  int             req_wait_ms;
  int             rsp_wait_ms;

  // 重试策略（默认）
  int             retry_max_attempts;
  int             retry_base_delay_ms;
  int             retry_max_delay_ms;
  int             retry_jitter_pct;
  int             retry_on_timeout;
  int             retry_on_network_error;
  int             retry_on_5xx;
  int             retry_on_429;

  // TLS 配置（默认）
  char*           tls_ca_path;
  char*           tls_cert_path;
  char*           tls_key_path;
  char*           tls_server_name;

  // 工作线程状态
  pthread_t       worker;
  atomic_int      running;         // 1=运行；0=退出
  atomic_int      inflight;        // 当前活动连接数

  // 重试链
  struct retry_item* retry_head;   // 仅 worker 线程访问

  // 等待者表（供 send_and_wait 使用）
  pthread_mutex_t  wait_mtx;
  waiter_t*        waiters;
};

// ----------------- 小工具 -----------------
static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static int parse_http_status(const struct mg_http_message* hm) {
  const char* s = hm->message.buf;
  size_t n = hm->message.len;
  if (n < 12) return 0;
  const char* sp = (const char*) memchr(s, ' ', n);
  if (!sp || sp + 4 > s + n || sp - s < 5) return 0;
  int code = 0; if (sscanf(sp + 1, "%3d", &code) == 1) return code;
  return 0;
}

static int next_backoff_ms(const sg_http_client_t* cli, int attempt) {
  long base = cli->retry_base_delay_ms;
  long cap  = cli->retry_max_delay_ms;
  long d = base;
  for (int i = 0; i < attempt; ++i) { d <<= 1; if (d > cap) { d = cap; break; } }
  int jp = clampi(cli->retry_jitter_pct, 0, 100);
  if (jp > 0) {
    long delta = (d * jp) / 100;
    long r = (long)(rand() % (2 * delta + 1)) - delta;  // [-delta, +delta]
    d += r; if (d < 0) d = 0;
  }
  return (int)d;
}

// waiter：注册/注销/投递
static waiter_t* waiter_register(sg_http_client_t* cli, int id) {
  waiter_t* w = (waiter_t*) calloc(1, sizeof(waiter_t));
  if (!w) return NULL;
  w->id = id; w->ready = 0; w->next = NULL;
  pthread_mutex_init(&w->m, NULL);
  pthread_condattr_t a; pthread_condattr_init(&a);
  pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
  pthread_cond_init(&w->cv, &a);
  pthread_condattr_destroy(&a);
  pthread_mutex_lock(&cli->wait_mtx);
  w->next = cli->waiters; cli->waiters = w;
  pthread_mutex_unlock(&cli->wait_mtx);
  return w;
}
static void waiter_unregister(sg_http_client_t* cli, waiter_t* w) {
  if (!w) return;
  pthread_mutex_lock(&cli->wait_mtx);
  waiter_t** p = &cli->waiters;
  while (*p && *p != w) p = &(*p)->next;
  if (*p) *p = w->next;
  pthread_mutex_unlock(&cli->wait_mtx);
  pthread_cond_destroy(&w->cv);
  pthread_mutex_destroy(&w->m);
  free(w);
}
static int deliver_to_waiter(sg_http_client_t* cli, int id, int ok,
                             const char* data, size_t len,
                             int attempts, int http_status) {
  int done = 0;
  pthread_mutex_lock(&cli->wait_mtx);
  for (waiter_t* w = cli->waiters; w; w = w->next) {
    if (w->id == id) {
      pthread_mutex_lock(&w->m);
      w->resp.id = id; w->resp.ok = ok; w->resp.attempts = attempts; w->resp.http_status = http_status;
      if (data && len > 0) {
        w->resp.data = (char*) malloc(len);
        if (w->resp.data) { memcpy(w->resp.data, data, len); w->resp.len = len; }
      }
      w->ready = 1;
      pthread_cond_signal(&w->cv);
      pthread_mutex_unlock(&w->m);
      done = 1; break;
    }
  }
  pthread_mutex_unlock(&cli->wait_mtx);
  return done;
}

// 结果交付：优先给 waiter；否则入全局响应队列
static void rsp_deliver(sg_http_client_t* cli, int id, int ok,
                        const char* data, size_t len,
                        int attempts, int http_status) {
  if (deliver_to_waiter(cli, id, ok, data, len, attempts, http_status)) return;
  rsp_item_t* r = (rsp_item_t*) calloc(1, sizeof(rsp_item_t));
  if (!r) return;
  r->id = id; r->ok = ok; r->attempts = attempts; r->http_status = http_status;
  if (data && len > 0) {
    r->data = (char*) malloc(len);
    if (r->data) { memcpy(r->data, data, len); r->len = len; }
  }
  (void) ring_push_timed(&cli->rspq, r, cli->rsp_wait_ms);
}

static void free_req_item(req_item_t* it) {
  if (!it) return;
  free(it->url); free(it->body); free(it);
}
static void free_retry_item(struct retry_item* ri) {
  if (!ri) return;
  free(ri->req.url); free(ri->req.body); free(ri);
}

// 在重试链表中按 due_ms 升序插入（仅 worker 线程调用）
static void schedule_retry(sg_http_client_t* cli, const req_item_t* src, int attempt, uint64_t due_ms) {
  struct retry_item* ri = (struct retry_item*) calloc(1, sizeof(*ri));
  if (!ri) return;
  ri->req = *src;  // 先拷贝标量
  size_t ulen = strlen(src->url);
  ri->req.url = (char*) malloc(ulen + 1); if (!ri->req.url) { free(ri); return; }
  memcpy(ri->req.url, src->url, ulen + 1);
  if (src->method == SG_HTTP_POST && src->body && src->body_len > 0) {
    ri->req.body = malloc(src->body_len);
    if (!ri->req.body) { free(ri->req.url); free(ri); return; }
    memcpy(ri->req.body, src->body, src->body_len);
    ri->req.body_len = src->body_len;
  }
  ri->attempt = attempt; ri->due_ms = due_ms; ri->next = NULL;

  struct retry_item** p = &cli->retry_head;
  while (*p && (*p)->due_ms <= due_ms) p = &(*p)->next;
  ri->next = *p; *p = ri;
}

// 取出到期的重试项（最多 allow 个），返回数量
static int pop_due_retries(sg_http_client_t* cli, struct retry_item** out_list, int allow) {
  uint64_t now = now_ms();
  int taken = 0; *out_list = NULL;
  struct retry_item** tail = out_list;
  while (cli->retry_head && cli->retry_head->due_ms <= now && taken < allow) {
    struct retry_item* n = cli->retry_head;
    cli->retry_head = n->next;
    n->next = NULL;
    *tail = n; tail = &n->next;
    taken++;
  }
  return taken;
}

// ----------------- 重试判定 -----------------
typedef struct { sg_http_client_t* cli; req_item_t req; int attempt; uint64_t start_ms; int sent;} conn_ctx_t;

static int should_retry_http(sg_http_client_t* cli, const conn_ctx_t* cx, int http_status) {
  int maxr = (cx->req.max_retries >= 0) ? cx->req.max_retries : cli->retry_max_attempts;
  if (cx->attempt >= maxr) return 0;
  if (cli->retry_on_5xx && http_status >= 500 && http_status <= 599) return 1;
  if (cli->retry_on_429 && http_status == 429) return 1;
  return 0;
}
static int should_retry_timeout(sg_http_client_t* cli, const conn_ctx_t* cx) {
  int maxr = (cx->req.max_retries >= 0) ? cx->req.max_retries : cli->retry_max_attempts;
  if (cx->attempt >= maxr) return 0;
  return cli->retry_on_timeout ? 1 : 0;
}
static int should_retry_neterr(sg_http_client_t* cli, const conn_ctx_t* cx) {
  int maxr = (cx->req.max_retries >= 0) ? cx->req.max_retries : cli->retry_max_attempts;
  if (cx->attempt >= maxr) return 0;
  return cli->retry_on_network_error ? 1 : 0;
}

static void send_http_request(struct mg_connection* c, conn_ctx_t* cx) {
  struct mg_str host = mg_url_host(cx->req.url);
  const char*   uri  = mg_url_uri(cx->req.url);
  if (cx->req.method == SG_HTTP_GET) {
    mg_printf(c,
      "GET %s HTTP/1.1\r\n"
      "Host: %.*s\r\n"
      "Connection: close\r\n"
      "\r\n", uri, (int)host.len, host.buf);
  } else {
    mg_printf(c,
      "POST %s HTTP/1.1\r\n"
      "Host: %.*s\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: %d\r\n"
      "Connection: close\r\n"
      "\r\n",
      uri, (int)host.len, host.buf, (int)cx->req.body_len);
    if (cx->req.body_len > 0) mg_send(c, cx->req.body, cx->req.body_len);
  }
  cx->start_ms = now_ms();
}

// ----------------- Mongoose 回调 -----------------
static void http_client_fn(struct mg_connection* c, int ev, void* ev_data) {
  conn_ctx_t* cx = (conn_ctx_t*) c->fn_data;
  sg_http_client_t* cli = cx ? cx->cli : NULL;

  switch (ev) {
    case MG_EV_CONNECT: {
    // HTTPS? 初始化 TLS
    if (mg_url_is_ssl(cx->req.url)) {
      struct mg_tls_opts topts; memset(&topts, 0, sizeof(topts));

      // 读文件到内存（返回 mg_str：buf/len）
      struct mg_str ca   = mg_str_n(NULL, 0);
      struct mg_str cert = mg_str_n(NULL, 0);
      struct mg_str key  = mg_str_n(NULL, 0);
      if (cx->cli->tls_ca_path)   ca   = mg_file_read(&mg_fs_posix, cx->cli->tls_ca_path);
      if (cx->cli->tls_cert_path) cert = mg_file_read(&mg_fs_posix, cx->cli->tls_cert_path);
      if (cx->cli->tls_key_path)  key  = mg_file_read(&mg_fs_posix, cx->cli->tls_key_path);

      topts.ca   = ca;       // 直接赋 mg_str（内容），不是路径
      topts.cert = cert;
      topts.key  = key;

      // SNI/主机名
      if (cx->cli->tls_server_name && *cx->cli->tls_server_name) {
        topts.name.buf = cx->cli->tls_server_name;
      } else {
        struct mg_str host = mg_url_host(cx->req.url);
        char* host_c = NULL;
        if (host.len) host_c = mg_mprintf("%.*s", (int)host.len, host.buf);
        topts.name.buf = host_c;   // 我们分配的，init 后要 free
      }

      mg_tls_init(c, &topts);

      // 释放 mg_file_read/mg_mprintf 的内存
      if (!cx->cli->tls_server_name && topts.name.buf) mg_free((void*) topts.name.buf);
      if (ca.buf)   mg_free((void*) ca.buf);
      if (cert.buf) mg_free((void*) cert.buf);
      if (key.buf)  mg_free((void*) key.buf);

      // 等待 TLS 握手完成（MG_EV_TLS_HS）后再发
      cx->sent = 0;
    } else {
      // 非 TLS：立即发请求
      send_http_request(c, cx);
      cx->sent = 1;
    }
  } break;

    case MG_EV_TLS_HS: {
      if (!cx->sent) {
        send_http_request(c, cx);
        cx->sent = 1;
      }
    } break;

    case MG_EV_HTTP_MSG: {
      struct mg_http_message* hm = (struct mg_http_message*) ev_data;
      int status = parse_http_status(hm);
      if (status >= 200 && status <= 299) {
        rsp_deliver(cli, cx->req.id, 1, hm->message.buf, hm->message.len, cx->attempt + 1, status);
      } else {
        if (should_retry_http(cli, cx, status)) {
          int delay = next_backoff_ms(cli, cx->attempt);
          schedule_retry(cli, &cx->req, cx->attempt + 1, now_ms() + (uint64_t)delay);
        } else {
          rsp_deliver(cli, cx->req.id, 0, hm->message.buf, hm->message.len, cx->attempt + 1, status);
        }
      }
      c->is_closing = 1;
    } break;

    case MG_EV_ERROR: {
      const char* err = (const char*) ev_data;
      if (should_retry_neterr(cli, cx)) {
        int delay = next_backoff_ms(cli, cx->attempt);
        schedule_retry(cli, &cx->req, cx->attempt + 1, now_ms() + (uint64_t)delay);
      } else {
        char buf[256]; int n = snprintf(buf, sizeof(buf), "MG_EV_ERROR: %s", err ? err : "(null)");
        rsp_deliver(cli, cx ? cx->req.id : -1, 0, buf, (size_t)(n > 0 ? n : 0), cx ? (cx->attempt + 1) : 1, 0);
      }
      c->is_closing = 1;
    } break;

    case MG_EV_POLL: {
      if (cx && cx->start_ms > 0) {
        uint64_t elapsed = now_ms() - cx->start_ms;
        if (elapsed > (uint64_t) cli->conn_timeout_ms) {
          if (should_retry_timeout(cli, cx)) {
            int delay = next_backoff_ms(cli, cx->attempt);
            schedule_retry(cli, &cx->req, cx->attempt + 1, now_ms() + (uint64_t)delay);
          } else {
            char buf[256]; int n = snprintf(buf, sizeof(buf), "timeout after %d ms: %s",
                                            cli->conn_timeout_ms, cx->req.url);
            rsp_deliver(cli, cx->req.id, 0, buf, (size_t)(n > 0 ? n : 0), cx->attempt + 1, 0);
          }
          c->is_closing = 1;
        }
      }
    } break;

    case MG_EV_CLOSE: {
      if (cli) atomic_fetch_sub(&cli->inflight, 1);
      if (cx) { free(cx); c->fn_data = NULL; }
    } break;

    default: break;
  }
}

// ----------------- 工作线程 -----------------
static void* worker_thread(void* arg) {
  sg_http_client_t* cli = (sg_http_client_t*) arg;
  struct mg_mgr mgr;
  mg_mgr_init(&mgr);

  while (atomic_load(&cli->running)) {
    int capacity = cli->max_inflight - atomic_load(&cli->inflight);
    if (capacity > 0) {
      // 先处理到期的重试
      struct retry_item* due = NULL;
      pop_due_retries(cli, &due, capacity);
      while (due && capacity > 0) {
        struct retry_item* ri = due; due = ri->next; ri->next = NULL;

        conn_ctx_t* cx = (conn_ctx_t*) calloc(1, sizeof(conn_ctx_t));
        if (!cx) { free_retry_item(ri); continue; }
        cx->cli = cli; cx->attempt = ri->attempt; cx->req = ri->req; // 所有权转移
        ri->req.url = NULL; ri->req.body = NULL; free(ri);

        struct mg_connection* c = mg_http_connect(&mgr, cx->req.url, http_client_fn, cx);
        if (c == NULL) {
          if (should_retry_neterr(cli, cx)) {
            int delay = next_backoff_ms(cli, cx->attempt);
            schedule_retry(cli, &cx->req, cx->attempt + 1, now_ms() + (uint64_t)delay);
            free(cx); // req 已复制到重试项
          } else {
            const char* msg = "connect failed (retry)";
            rsp_deliver(cli, cx->req.id, 0, msg, strlen(msg), cx->attempt + 1, 0);
            free(cx->req.url); free(cx->req.body); free(cx);
          }
        } else {
          atomic_fetch_add(&cli->inflight, 1);
          capacity--;
        }
      }

      // 再拉取新请求
      while (capacity > 0) {
        req_item_t* it = NULL;
        int rc = ring_pop_timed(&cli->reqq, (void**)&it, cli->req_wait_ms);
        if (rc == 1 && it) {
          conn_ctx_t* cx = (conn_ctx_t*) calloc(1, sizeof(conn_ctx_t));
          if (!cx) { free_req_item(it); continue; }
          cx->cli = cli; cx->attempt = 0; cx->req = *it; free(it);

          struct mg_connection* c = mg_http_connect(&mgr, cx->req.url, http_client_fn, cx);
          if (c == NULL) {
            if (should_retry_neterr(cli, cx)) {
              int delay = next_backoff_ms(cli, cx->attempt);
              schedule_retry(cli, &cx->req, cx->attempt + 1, now_ms() + (uint64_t)delay);
              free(cx);
            } else {
              const char* msg = "connect failed";
              rsp_deliver(cli, cx->req.id, 0, msg, strlen(msg), 1, 0);
              free(cx->req.url); free(cx->req.body); free(cx);
            }
          } else {
            atomic_fetch_add(&cli->inflight, 1);
            capacity--;
          }
        } else break;
      }
    }
    mg_mgr_poll(&mgr, cli->poll_interval_ms);
  }

  // 清理未触发的重试项
  while (cli->retry_head) { struct retry_item* n = cli->retry_head; cli->retry_head = n->next; free_retry_item(n); }
  mg_mgr_free(&mgr);
  return NULL;
}

// ----------------- 公共 API -----------------
void sg_http_client_opts_init(sg_http_client_opts* o) {
  if (!o) return;
  memset(o, 0, sizeof(*o));
  o->req_capacity       = DEF_REQ_CAP;
  o->rsp_capacity       = DEF_RSP_CAP;
  o->max_inflight       = DEF_MAX_INFLIGHT;
  o->conn_timeout_ms    = DEF_CONN_TIMEOUT_MS;
  o->poll_interval_ms   = DEF_POLL_INTERVAL_MS;
  o->req_wait_ms        = DEF_REQ_WAIT_MS;
  o->rsp_wait_ms        = DEF_RSP_WAIT_MS;

  o->retry_max_attempts = DEF_RETRY_MAX_ATTEMPTS;
  o->retry_base_delay_ms= DEF_RETRY_BASE_MS;
  o->retry_max_delay_ms = DEF_RETRY_MAX_MS;
  o->retry_jitter_pct   = DEF_RETRY_JITTER_PCT;

  o->retry_on_timeout       = DEF_RETRY_ON_TIMEOUT;
  o->retry_on_network_error = DEF_RETRY_ON_NETERR;
  o->retry_on_5xx           = DEF_RETRY_ON_5XX;
  o->retry_on_429           = DEF_RETRY_ON_429;

  o->tls_ca_path     =  NULL;
  o->tls_cert_path   =  NULL;
  o->tls_key_path    =  NULL;
  o->tls_server_name =  NULL;
}

int sg_http_client_start(const sg_http_client_opts* o, sg_http_client_t** out_cli) {
  if (!out_cli) return -1;
  sg_http_client_t* cli = (sg_http_client_t*) calloc(1, sizeof(*cli));
  if (!cli) return -2;

  int req_cap  = (o && o->req_capacity   > 0) ? o->req_capacity   : DEF_REQ_CAP;
  int rsp_cap  = (o && o->rsp_capacity   > 0) ? o->rsp_capacity   : DEF_RSP_CAP;
  cli->max_inflight      = (o && o->max_inflight      > 0) ? o->max_inflight      : DEF_MAX_INFLIGHT;
  cli->conn_timeout_ms   = (o && o->conn_timeout_ms   > 0) ? o->conn_timeout_ms   : DEF_CONN_TIMEOUT_MS;
  cli->poll_interval_ms  = (o && o->poll_interval_ms  > 0) ? o->poll_interval_ms  : DEF_POLL_INTERVAL_MS;
  cli->req_wait_ms       = (o && o->req_wait_ms       > 0) ? o->req_wait_ms       : DEF_REQ_WAIT_MS;
  cli->rsp_wait_ms       = (o && o->rsp_wait_ms       > 0) ? o->rsp_wait_ms       : DEF_RSP_WAIT_MS;

  cli->retry_max_attempts   = (o && o->retry_max_attempts   >= 0) ? o->retry_max_attempts   : DEF_RETRY_MAX_ATTEMPTS;
  cli->retry_base_delay_ms  = (o && o->retry_base_delay_ms  >  0) ? o->retry_base_delay_ms  : DEF_RETRY_BASE_MS;
  cli->retry_max_delay_ms   = (o && o->retry_max_delay_ms   >  0) ? o->retry_max_delay_ms   : DEF_RETRY_MAX_MS;
  cli->retry_jitter_pct     = (o && o->retry_jitter_pct     >= 0) ? o->retry_jitter_pct     : DEF_RETRY_JITTER_PCT;

  cli->retry_on_timeout        = (o ? o->retry_on_timeout        : DEF_RETRY_ON_TIMEOUT);
  cli->retry_on_network_error  = (o ? o->retry_on_network_error  : DEF_RETRY_ON_NETERR);
  cli->retry_on_5xx            = (o ? o->retry_on_5xx            : DEF_RETRY_ON_5XX);
  cli->retry_on_429            = (o ? o->retry_on_429            : DEF_RETRY_ON_429);

  cli->tls_ca_path     = (o && o->tls_ca_path)     ? strdup(o->tls_ca_path)     : NULL;
  cli->tls_cert_path   = (o && o->tls_cert_path)   ? strdup(o->tls_cert_path)   : NULL;
  cli->tls_key_path    = (o && o->tls_key_path)    ? strdup(o->tls_key_path)    : NULL;
  cli->tls_server_name = (o && o->tls_server_name) ? strdup(o->tls_server_name) : NULL;

  if (ring_init(&cli->reqq, req_cap) != 0 || ring_init(&cli->rspq, rsp_cap) != 0) {
    ring_free(&cli->reqq); ring_free(&cli->rspq); free(cli); return -3;
  }

  srand((unsigned) time(NULL));
  atomic_store(&cli->running, 1);
  atomic_store(&cli->inflight, 0);
  cli->retry_head = NULL;

  pthread_mutex_init(&cli->wait_mtx, NULL);
  cli->waiters = NULL;

  if (pthread_create(&cli->worker, NULL, worker_thread, cli) != 0) {
    ring_free(&cli->reqq); ring_free(&cli->rspq);
    pthread_mutex_destroy(&cli->wait_mtx);
    free(cli); return -4;
  }

  *out_cli = cli;
  return 0;
}

void sg_http_client_stop(sg_http_client_t* cli) {
  if (!cli) return;
  atomic_store(&cli->running, 0);
  ring_close(&cli->reqq);
  ring_close(&cli->rspq);

  // 唤醒所有等待者：返回“客户端已关闭”
  pthread_mutex_lock(&cli->wait_mtx);
  for (waiter_t* w = cli->waiters; w; w = w->next) {
    pthread_mutex_lock(&w->m);
    if (!w->ready) {
      static const char msg[] = "client stopped";
      w->resp.id = w->id; w->resp.ok = 0; w->resp.http_status = 0; w->resp.attempts = 0;
      w->resp.data = (char*) malloc(sizeof msg);
      if (w->resp.data) { memcpy(w->resp.data, msg, sizeof msg); w->resp.len = sizeof msg; }
      w->ready = 1;
      pthread_cond_signal(&w->cv);
    }
    pthread_mutex_unlock(&w->m);
  }
  pthread_mutex_unlock(&cli->wait_mtx);

  pthread_join(cli->worker, NULL);

  // 清理队列里残留项
  void* p = NULL;
  while (ring_pop_timed(&cli->reqq, &p, 0) == 1) { req_item_t* it = (req_item_t*) p; free(it->url); free(it->body); free(it); }
  while (ring_pop_timed(&cli->rspq, &p, 0) == 1) { rsp_item_t* r = (rsp_item_t*) p; free(r->data); free(r); }

  // 清理未到期的重试项
  while (cli->retry_head) { struct retry_item* n = cli->retry_head; cli->retry_head = n->next; free_retry_item(n); }

  // 释放等待者（若有残留）
  pthread_mutex_lock(&cli->wait_mtx);
  while (cli->waiters) {
    waiter_t* n = cli->waiters; cli->waiters = n->next;
    pthread_cond_destroy(&n->cv); pthread_mutex_destroy(&n->m);
    if (n->resp.data) free(n->resp.data);
    free(n);
  }
  pthread_mutex_unlock(&cli->wait_mtx);
  pthread_mutex_destroy(&cli->wait_mtx);

  ring_free(&cli->reqq); ring_free(&cli->rspq);
  free(cli);
  free(cli->tls_ca_path);
  free(cli->tls_cert_path);
  free(cli->tls_key_path);
  free(cli->tls_server_name);

}

int sg_http_client_submit(sg_http_client_t* cli, const sg_http_request* req, int timeout_ms) {
  if (!cli || !req || !req->url) return -1;
  req_item_t* it = (req_item_t*) calloc(1, sizeof(req_item_t));
  if (!it) return -1;

  it->id = req->id;
  it->method = (req->method == SG_HTTP_POST) ? SG_HTTP_POST : SG_HTTP_GET;
  it->max_retries = req->max_retries;

  size_t ulen = strlen(req->url);
  it->url = (char*) malloc(ulen + 1); if (!it->url) { free(it); return -1; }
  memcpy(it->url, req->url, ulen + 1);

  if (it->method == SG_HTTP_POST && req->body && req->body_len > 0) {
    it->body = malloc(req->body_len); if (!it->body) { free(it->url); free(it); return -1; }
    memcpy(it->body, req->body, req->body_len); it->body_len = req->body_len;
  }

  int rc = ring_push_timed(&cli->reqq, it, timeout_ms);
  if (rc <= 0) { free(it->url); free(it->body); free(it); }
  return rc; // 1=ok, 0=timeout(full), -1=closed
}

int sg_http_client_recv(sg_http_client_t* cli, sg_http_response* out, int timeout_ms) {
  if (!cli || !out) return -1;
  void* p = NULL;
  int rc = ring_pop_timed(&cli->rspq, &p, timeout_ms);
  if (rc == 1 && p) {
    rsp_item_t* r = (rsp_item_t*) p;
    out->id = r->id; out->ok = r->ok; out->data = r->data; out->len = r->len;
    out->attempts = r->attempts; out->http_status = r->http_status;
    free(r);
    return 1;
  }
  return rc;
}

int sg_http_client_send_and_wait(sg_http_client_t* cli,
                                 const sg_http_request* req,
                                 int submit_timeout_ms,
                                 int wait_timeout_ms,
                                 sg_http_response* out) {
  if (!cli || !req || !out) return -1;

  waiter_t* w = waiter_register(cli, req->id);
  if (!w) return -1;

  int rc = sg_http_client_submit(cli, req, submit_timeout_ms);
  if (rc != 1) { waiter_unregister(cli, w); return (rc == 0) ? 0 : -1; }

  struct timespec abs;
  int ret = 0;
  pthread_mutex_lock(&w->m);
  while (!w->ready) {
    clock_gettime(CLOCK_MONOTONIC, &abs);
    abs.tv_sec  += wait_timeout_ms / 1000;
    abs.tv_nsec += (wait_timeout_ms % 1000) * 1000000L;
    if (abs.tv_nsec >= 1000000000L) { abs.tv_sec++; abs.tv_nsec -= 1000000000L; }
    int rcw = pthread_cond_timedwait(&w->cv, &w->m, &abs);
    if (rcw == ETIMEDOUT) break;
  }
  if (w->ready) {
    *out = w->resp;
    w->resp.data = NULL; w->resp.len = 0; // 转移所有权
    ret = 1;
  }
  pthread_mutex_unlock(&w->m);

  waiter_unregister(cli, w);
  return ret; // 1=成功；0=超时
}

void sg_http_response_free(sg_http_response* r) {
  if (!r) return;
  free(r->data);
  r->data = NULL; r->len = 0;
}
