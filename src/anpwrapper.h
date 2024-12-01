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

#ifndef ANPNETSTACK_ANPWRAPPER_H
#define ANPNETSTACK_ANPWRAPPER_H

#include "linklist.h"
#include "tcp.h"
#include <pthread.h>

#define FD_START_VAL        1000000
#define FD_MAX_VAL          1048575  // Max number of file descriptors as shown by ulimit -n, - 1 

extern struct list_head anp_sockets_cache;
extern pthread_mutex_t fd_lock;
extern bool fd_in_use[FD_MAX_VAL - FD_START_VAL];

struct anp_socket{
    int fd;
    int type;
    int protocol;
    int domain;
    struct tcb *conn_info;
    struct list_head list;
};

static inline void free_fd_entry(int fd){
    pthread_mutex_lock(&fd_lock);
    fd_in_use[fd-FD_START_VAL] = false;
    pthread_mutex_unlock(&fd_lock);
}

struct anp_socket *get_anp_socket(int fd);
void free_anp_socket(struct anp_socket *sock);
struct tcb *get_tcb(uint16_t local_port, uint32_t local_ip, uint16_t remote_port, uint32_t remote_ip);



void _function_override_init();

#endif //ANPNETSTACK_ANPWRAPPER_H
