// File: sg-tcp.c
#include "sg-tcp.h"
#include "sg-ringbuffer.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/eventfd.h>  // 添加 eventfd 支持

#define MAX_EVENTS    64
#define MAX_MESSAGE 4096

struct SgTcpServer {
    int               listen_fd;
    int               epoll_fd;
    int               event_fd;
    pthread_t         io_thread, worker_thread, send_thread;
    RingBuffer       *recv_q, *send_q;
    pthread_mutex_t   clients_mtx;
    int              *clients;
    size_t            clients_cap, clients_cnt;
    SgOnMessage       on_msg;
    void             *msg_ud;
    SgOnDisconnect    on_disc;
    void             *disc_ud;
    volatile int      stopped;
};

static int create_eventfd() {
    return eventfd(0, EFD_NONBLOCK);
}

static int set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void cleanup_connection(int fd, SgTcpServer *srv) {
    epoll_ctl(srv->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
    close(fd);
    pthread_mutex_lock(&srv->clients_mtx);
    for (size_t i = 0; i < srv->clients_cnt; i++) {
        if (srv->clients[i] == fd) {
            srv->clients[i] = srv->clients[--srv->clients_cnt];
            break;
        }
    }
    pthread_mutex_unlock(&srv->clients_mtx);
    if (srv->on_disc) srv->on_disc(srv, fd, srv->disc_ud);
}

static void *io_loop(void *arg) {
    printf("[tcp] io_loop started\n");
    SgTcpServer *srv = arg;
    struct epoll_event evs[MAX_EVENTS];
    while (!srv->stopped) {
        int n = epoll_wait(srv->epoll_fd, evs, MAX_EVENTS, -1);
        if (n < 0) {
            if (srv->stopped) break;  // 新增：退出 epoll_wait
            continue;
        }
        for (int i = 0; i < n; i++) {
            int fd = evs[i].data.fd;
            uint32_t e = evs[i].events;
            if (fd == srv->listen_fd) {
                if (fd == srv->event_fd) {  // [新增] 判断是否为退出事件
                    uint64_t u;
                    read(fd, &u, sizeof(u));
                    printf("[tcp] received eventfd exit\n");
                    break;
                }
                if (srv->stopped) break;
                int c = accept(srv->listen_fd, NULL, NULL);
                set_nonblock(c);
                struct epoll_event ev = { .events = EPOLLIN|EPOLLHUP|EPOLLERR, .data = { .fd = c } };
                epoll_ctl(srv->epoll_fd, EPOLL_CTL_ADD, c, &ev);
                pthread_mutex_lock(&srv->clients_mtx);
                if (srv->clients_cnt == srv->clients_cap) {
                    size_t newcap = srv->clients_cap ? srv->clients_cap*2 : 16;
                    srv->clients = realloc(srv->clients, newcap * sizeof(int));
                    srv->clients_cap = newcap;
                }
                srv->clients[srv->clients_cnt++] = c;
                pthread_mutex_unlock(&srv->clients_mtx);
            } else if (e & (EPOLLHUP|EPOLLERR)) {
                cleanup_connection(fd, srv);
            } else if (e & EPOLLIN) {
                uint8_t *buf = malloc(MAX_MESSAGE);
                ssize_t r = read(fd, buf, MAX_MESSAGE);
                if (r > 0) {
                    SgTcpMessage *msg = malloc(sizeof(*msg));
                    msg->client_fd = fd;
                    msg->data      = buf;
                    msg->len       = r;
                    rb_push(srv->recv_q, msg);
                } else {
                    free(buf);
                    cleanup_connection(fd, srv);
                }
            }
        }
    }
    printf("[tcp] io_loop exiting\n");
    return NULL;
}

static void *worker_loop(void *arg) {
    SgTcpServer *srv = arg;
    printf("[tcp] worker_loop started\n");
    while (!srv->stopped) {
        SgTcpMessage *msg = NULL;
        if (!rb_timed_pop(srv->recv_q, (void**)&msg, 500)) {
            continue;  // timeout
        }
        srv->on_msg(srv, msg, srv->msg_ud);
        free(msg->data);
        free(msg);
    }
    printf("[tcp] worker_loop exiting\n");
    return NULL;
}

static void *send_loop(void *arg) {
    SgTcpServer *srv = arg;
    printf("[tcp] send_loop started\n");
    while (!srv->stopped) {
        SgTcpMessage *msg = NULL;
        if (!rb_timed_pop(srv->send_q, (void**)&msg, 500)) {
            continue;  // timeout, check stopped again
        }
        ssize_t s = send(msg->client_fd, msg->data, msg->len, 0);
        if (s <= 0) cleanup_connection(msg->client_fd, srv);
        free(msg->data);
        free(msg);
    }
    printf("[tcp] send_loop exiting\n");
    return NULL;
}

SgTcpServer *SgTcpServer_new(const char *bind_ip, uint16_t port, size_t queue_capacity) {
    SgTcpServer *srv = calloc(1, sizeof(*srv));
    srv->recv_q = rb_create(queue_capacity);
    srv->send_q = rb_create(queue_capacity);
    pthread_mutex_init(&srv->clients_mtx, NULL);
    srv->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    // Set reuse the addr
    int opt = 1;
    setsockopt(srv->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    set_nonblock(srv->listen_fd);
    struct sockaddr_in addr;
    memset(&addr,0,sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    addr.sin_addr.s_addr = inet_addr(bind_ip);
    bind(srv->listen_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(srv->listen_fd, SOMAXCONN);

    srv->epoll_fd = epoll_create1(0);
    struct epoll_event ev = { .events = EPOLLIN, .data = { .fd = srv->listen_fd } };
    epoll_ctl(srv->epoll_fd, EPOLL_CTL_ADD, srv->listen_fd, &ev);

    // 新增：创建并添加 eventfd 到 epoll
    srv->event_fd = create_eventfd();
    struct epoll_event eev = { .events = EPOLLIN, .data.fd = srv->event_fd };
    epoll_ctl(srv->epoll_fd, EPOLL_CTL_ADD, srv->event_fd, &eev);
    return srv;
}

void SgTcpServer_free(SgTcpServer *srv) {
    SgTcpServer_stop(srv);
    rb_destroy(srv->recv_q);
    rb_destroy(srv->send_q);
    close(srv->listen_fd);
    close(srv->epoll_fd);
    free(srv->clients);
    pthread_mutex_destroy(&srv->clients_mtx);
    free(srv);
}

void SgTcpServer_set_on_message(SgTcpServer *srv, SgOnMessage cb, void *user_data) {
    srv->on_msg = cb;
    srv->msg_ud = user_data;
}

void SgTcpServer_set_on_disconnect(SgTcpServer *srv, SgOnDisconnect cb, void *user_data) {
    srv->on_disc = cb;
    srv->disc_ud = user_data;
}

int SgTcpServer_start(SgTcpServer *srv) {
    srv->stopped = 0;
    pthread_create(&srv->io_thread, NULL, io_loop, srv);
    pthread_create(&srv->worker_thread, NULL, worker_loop, srv);
    pthread_create(&srv->send_thread, NULL, send_loop, srv);
    return 0;
}

void SgTcpServer_stop(SgTcpServer *srv) {
    printf("[tcp] stop called\n");
    srv->stopped = 1;
    printf(">>> closing listen_fd...\n");
    // 通知 epoll_wait 退出 [使用 eventfd 机制]
    uint64_t u = 1;
    write(srv->event_fd, &u, sizeof(u));
    printf(">>> broadcasting condition variables...\n");
    pthread_cond_broadcast(&srv->recv_q->not_empty);
    pthread_cond_broadcast(&srv->send_q->not_empty);
    printf("[tcp] waiting for io_thread...\n");
    pthread_join(srv->io_thread, NULL);
    printf("[tcp] io_thread exited\n");
    pthread_join(srv->worker_thread, NULL);
    printf(">>> worker_thread exited\n");
    pthread_join(srv->send_thread, NULL);
    printf("[tcp] send_thread exited\n");
}

int SgTcpServer_send(SgTcpServer *srv, int client_fd, const uint8_t *data, size_t len) {
    uint8_t *buf = malloc(len);
    memcpy(buf, data, len);
    SgTcpMessage *msg = malloc(sizeof(*msg));
    msg->client_fd = client_fd;
    msg->data      = buf;
    msg->len       = len;
    rb_push(srv->send_q, msg);
    return 0;
}

int SgTcpServer_broadcast(SgTcpServer *srv, const uint8_t *data, size_t len) {
    pthread_mutex_lock(&srv->clients_mtx);
    for (size_t i = 0; i < srv->clients_cnt; i++) {
        SgTcpServer_send(srv, srv->clients[i], data, len);
    }
    pthread_mutex_unlock(&srv->clients_mtx);
    return 0;
}

int SgTcpServer_get_clients(SgTcpServer *srv, int **list, size_t *count) {
    pthread_mutex_lock(&srv->clients_mtx);
    *count = srv->clients_cnt;
    *list = malloc((*count) * sizeof(int));
    if (!*list) {
        pthread_mutex_unlock(&srv->clients_mtx);
        return -1;
    }
    memcpy(*list, srv->clients, (*count) * sizeof(int));
    pthread_mutex_unlock(&srv->clients_mtx);
    return 0;
}