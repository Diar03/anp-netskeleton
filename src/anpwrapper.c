/*
 * Copyright [2020] [Animesh Trivedi]
 *
 * This code is part of the Advanced Network Programming (ANP) course
 * at VU Amsterdam.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *        http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
*/

//XXX: _GNU_SOURCE must be defined before including dlfcn to get RTLD_NEXT symbols
#define _GNU_SOURCE

#include <dlfcn.h>
#include "systems_headers.h"
#include "linklist.h"
#include "anpwrapper.h"
#include "init.h"
#include "anp_netdev.h" 
#include "ethernet.h"
#include "ip.h"
#include "utilities.h"
#include "route.h"

#define PORTS_STARTING      49154U
#define PORTS_ENDING        65535U

pthread_mutex_t port_lock = PTHREAD_MUTEX_INITIALIZER;

uint16_t available_port = PORTS_STARTING;

LIST_HEAD(anp_sockets_cache);
pthread_mutex_t fd_lock = PTHREAD_MUTEX_INITIALIZER;
bool fd_in_use[FD_MAX_VAL - FD_START_VAL] = {false};

static int (*__start_main)(int (*main) (int, char * *, char * *), int argc, \
                           char * * ubp_av, void (*init) (void), void (*fini) (void), \
                           void (*rtld_fini) (void), void (* stack_end));

static ssize_t (*_send)(int fd, const void *buf, size_t n, int flags) = NULL;
static ssize_t (*_recv)(int fd, void *buf, size_t n, int flags) = NULL;

static int (*_connect)(int sockfd, const struct sockaddr *addr, socklen_t addrlen) = NULL;
static int (*_socket)(int domain, int type, int protocol) = NULL;
static int (*_close)(int sockfd) = NULL;

struct tcb *get_tcb(uint16_t local_port, uint32_t local_ip, uint16_t remote_port, uint32_t remote_ip){
    struct list_head *item, *tmp = NULL;
    struct anp_socket *sock;
    struct tcb *tcb;
    list_for_each_safe(item, tmp, &anp_sockets_cache){
        if (!item) {
            continue;
        }
        sock = list_entry(item, struct anp_socket, list);
        if(sock->conn_info == NULL){
            continue;
        }
        tcb = sock->conn_info;
        if((tcb->local_port == local_port) && (tcb->local_ip == local_ip) 
            && (tcb->remote_port == remote_port) && (tcb->remote_ip == remote_ip)){
            return tcb;
        }
    }
    return NULL;
}

static inline int get_next_fd(){
    pthread_mutex_lock(&fd_lock);
    for(int i = 0; i < FD_MAX_VAL-FD_START_VAL; i++){
        if(!fd_in_use[i]){
            fd_in_use[i] = true;
            pthread_mutex_unlock(&fd_lock);
            return (i+FD_START_VAL);
        }
    }
    pthread_mutex_unlock(&fd_lock);
    return -1;
}

struct anp_socket *get_anp_socket(int fd){
    struct list_head *item, *tmp = NULL;
    struct anp_socket *sock;

    list_for_each_safe(item, tmp, &anp_sockets_cache){
        if (!item) {
            continue;
        }
        sock = list_entry(item, struct anp_socket, list);
        if(sock->fd == fd){
            return sock;
        }
    }
    return NULL;
}

void free_anp_socket(struct anp_socket *sock){
    free_fd_entry(sock->fd);
    list_del(&sock->list);
    if(sock->conn_info != NULL){
        free(sock->conn_info);
    }
    free(sock);
}


uint16_t get_port(){
    // Need somewhere to store value since right after 
    // unlocking the port can be incremented by another thread
    uint16_t ret = 0;
    pthread_mutex_lock(&port_lock);
    if(available_port > PORTS_ENDING){
        return -1;
    }
    available_port++;
    ret = available_port;
    pthread_mutex_unlock(&port_lock);
    return ret;
}

static bool is_anp_sockfd(int fd){
    pthread_mutex_lock(&fd_lock);
    if(fd_in_use[fd-FD_START_VAL]){
        pthread_mutex_unlock(&fd_lock);
        return true;
    }
    pthread_mutex_unlock(&fd_lock);
    return false;
}
static int is_socket_supported(int domain, int type, int protocol)
{
    // we are only going to handle TCP STREAM sockets on the IPv4
    if (domain != AF_INET){
        return 0;
    }
    if (!(type & SOCK_STREAM)) {
        return 0;
    }
    if (protocol != 0 && protocol != IPPROTO_TCP) {
        return 0;
    }
    printf("supported socket domain %d type %d and protocol %d \n", domain, type, protocol);
    return 1;
}

// TODO: ANP milestones 3 and 4
int socket(int domain, int type, int protocol) {

    if(!is_socket_supported(domain, type, protocol)){
        // if this is not what anpnetstack support, let it go, let it go!
        return _socket(domain, type, protocol);
    }

    struct anp_socket *sock = calloc(sizeof(struct anp_socket), 1);
    if(sock == NULL){
        errno = ENOMEM;
        return -1;
    }
    sock->fd = get_next_fd();
    if(sock->fd == -1){
        free(sock);
        errno = EMFILE;
        return -1;
    }

    sock->domain = domain;
    sock->type = type;
    sock->protocol = protocol;
    sock->conn_info = NULL;         // Connect() creates the connection info
    list_init(&sock->list);
    list_add_tail(&sock->list, &anp_sockets_cache);
    return sock->fd;
}


int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
    if(!is_anp_sockfd(sockfd)){
        // the default path
        return _connect(sockfd, addr, addrlen);
    }

    struct anp_socket *sock = get_anp_socket(sockfd);
    if(sock == NULL){
        errno = EBADF;
        return -1;
    }

    struct tcb *conn_info = calloc(1, sizeof(struct tcb));
    if(conn_info == NULL){
        free_anp_socket(sock);
        errno = ENOMEM;
        return -1;
    }


    sock->conn_info = conn_info;
    conn_info->local_port = get_port();
    conn_info->remote_port = ntohs(((struct sockaddr_in *)addr)->sin_port);
    conn_info->remote_ip = ntohl(((struct sockaddr_in *)addr)->sin_addr.s_addr);

    
    struct rtentry *rt = route_lookup(conn_info->remote_ip);
    if(rt == NULL){
        errno = ENETUNREACH;
        return -1;
    }

    conn_info->local_ip = ntohl(rt->dev->addr);
    conn_info->state = TCP_CLOSED;


    conn_info->iss = rand();
    conn_info->snd_una = conn_info->iss-1;
    conn_info->snd_nxt = conn_info->iss;
    conn_info->irs = 0;
    conn_info->rcv_nxt = conn_info->irs;

    conn_info->rcv_buff_len = 0;
    
    pthread_mutex_init(&conn_info->tcb_lock, NULL);
    pthread_mutex_init(&conn_info->send_lock, NULL);

    pthread_cond_init(&conn_info->tcb_cond, NULL);
    pthread_cond_init(&conn_info->send_cond, NULL);


    pthread_mutex_lock(&conn_info->tcb_lock);

    tcp_tx(conn_info, NULL, 0, TCP_SYN, RETRIES_CONN_MAX);
    
    conn_info->state = TCP_SYN_SENT;
    
    pthread_cond_wait(&conn_info->tcb_cond, &conn_info->tcb_lock);
    if(conn_info->state != TCP_ESTABLISHED){
        pthread_mutex_unlock(&conn_info->tcb_lock);
        return -1;
    }else{
        pthread_mutex_unlock(&conn_info->tcb_lock);
        return 0;
    }

    
}

// TODO: ANP milestone 5 -- implement the send, recv, and close calls
ssize_t send(int sockfd, const void *buf, size_t len, int flags)
{

    if(!is_anp_sockfd(sockfd)){
        // the default path
        return _send(sockfd, buf, len, flags);
    }

    struct anp_socket *sock = get_anp_socket(sockfd);
    if(sock == NULL){
        errno = EBADF;
        return -1;
    }

    struct tcb *conn_info = sock->conn_info;
    if(conn_info == NULL){
        errno = ENOTCONN;
        return -1;
    }

    if(conn_info->state != TCP_ESTABLISHED){
        errno = ENOTCONN;
        return -1;
    }
    
    // tcp_tx with data returns only if all data is sent
    tcp_tx(conn_info, (char *)buf, len, TCP_ACK, RETRIES_INFINITE);

    return len;

}

ssize_t recv(int sockfd, void *buf, size_t len, int flags) {
    if (!is_anp_sockfd(sockfd)) {
        // the default path
        return _recv(sockfd, buf, len, flags);
    }

    struct anp_socket *sock = get_anp_socket(sockfd);
    if (sock == NULL) {
        errno = EBADF;
        return -1;
    }

    struct tcb *conn_info = sock->conn_info;
    if (conn_info == NULL) {
        errno = ENOTCONN;
        return -1;
    }
    
            
    return read_bytes(conn_info, buf, len);
}

int close (int sockfd){
    if(!is_anp_sockfd(sockfd)){
        // the default path
        return _close(sockfd);
    }

    printf("Closing socket \n");

    struct anp_socket *sock = get_anp_socket(sockfd);
    if(sock == NULL){
        errno = EBADF;
        return -1;
    }

    struct tcb *conn_info = sock->conn_info;
    if(conn_info == NULL){
        errno = ENOTCONN;
        return -1;
    }

    pthread_mutex_lock(&conn_info->tcb_lock);

    if(conn_info->state != TCP_ESTABLISHED){
        errno = ENOTCONN;
        return -1;
    }


    conn_info->state = TCP_FIN_WAIT_1;
    tcp_tx(conn_info, NULL, 0, TCP_FINACK, RETRIES_SENDONCE);
    
    pthread_cond_wait(&conn_info->tcb_cond, &conn_info->tcb_lock);

    if(conn_info->state != TCP_CLOSED){
        pthread_mutex_unlock(&conn_info->tcb_lock);
        return -1;
    }
    
    free(conn_info);
    sock->conn_info = NULL;
    pthread_mutex_unlock(&conn_info->tcb_lock);
    return 0;

}

void _function_override_init()
{
    __start_main = dlsym(RTLD_NEXT, "__libc_start_main");
    _socket = dlsym(RTLD_NEXT, "socket");
    _connect = dlsym(RTLD_NEXT, "connect");
    _send = dlsym(RTLD_NEXT, "send");
    _recv = dlsym(RTLD_NEXT, "recv");
    _close = dlsym(RTLD_NEXT, "close");
}
