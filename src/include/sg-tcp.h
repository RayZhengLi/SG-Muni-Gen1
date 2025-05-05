// File: sg-tcp.h
#ifndef SG_TCP_H
#define SG_TCP_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SgTcpServer SgTcpServer;
typedef struct {
    int      client_fd;
    uint8_t *data;
    size_t   len;
} SgTcpMessage;

typedef void (*SgOnMessage)(SgTcpServer *srv, const SgTcpMessage *msg, void *user_data);
typedef void (*SgOnDisconnect)(SgTcpServer *srv, int client_fd, void *user_data);

/** 创建并绑定服务器，queue_capacity 为读/写队列容量 */
SgTcpServer *SgTcpServer_new(const char *bind_ip, uint16_t port, size_t queue_capacity);
void         SgTcpServer_free(SgTcpServer *srv);

/** 启动/停止 */
int          SgTcpServer_start(SgTcpServer *srv);
void         SgTcpServer_stop(SgTcpServer *srv);

/** 注册回调 */
void         SgTcpServer_set_on_message(SgTcpServer *srv, SgOnMessage cb, void *user_data);
void         SgTcpServer_set_on_disconnect(SgTcpServer *srv, SgOnDisconnect cb, void *user_data);

/** 发送数据到单个客户端 */
int          SgTcpServer_send(SgTcpServer *srv, int client_fd, const uint8_t *data, size_t len);
/** 向所有已在线客户端广播 */
int          SgTcpServer_broadcast(SgTcpServer *srv, const uint8_t *data, size_t len);
/** 获取当前所有在线客户端的 FD 列表，调用者负责 free(list) */
int          SgTcpServer_get_clients(SgTcpServer *srv, int **list, size_t *count);

#ifdef __cplusplus
}
#endif

#endif /* SG_TCP_H */