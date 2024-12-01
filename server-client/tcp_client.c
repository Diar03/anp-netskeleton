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

#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/socket.h>
#include <errno.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <assert.h>
#include <pthread.h>

#include "common.h"



// sudo tcpdump -i wlp2s0 tcp port 43211
int run_test(int test_buffer_sz, struct sockaddr_in *server_addr)
{
    int server_fd = -1, ret = -1, so_far = 0;
    char debug_buffer[INET_ADDRSTRLEN];
    char *tx_buffer = calloc(1, test_buffer_sz);
    if(tx_buffer == NULL){
        printf("tx buffer allocation failed %d \n", -ENOMEM);
        return(-ENOMEM);
    }
    char *rx_buffer = calloc(1, test_buffer_sz);
    if(rx_buffer == NULL){
        free(tx_buffer);
        printf("rx buffer allocation failed %d \n", -ENOMEM);
        return(-ENOMEM);
    }
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if ( 0 > server_fd) {
        printf("socket creation failed, ret code: %d errno : %d \n", ret, errno);
        ret = -errno;
        goto free_bufs;
    }
    printf("OK: socket created, fd is %d \n", server_fd);
    ret = connect(server_fd, (struct sockaddr*)server_addr, sizeof(*server_addr));
    if ( 0 != ret) {
        printf("Error: connection with the server failed, errno %d \n", errno);
        ret = -errno;
        goto free_bufs;
    }
    inet_ntop( AF_INET, &server_addr->sin_addr, debug_buffer, sizeof(debug_buffer));
    printf("OK: connected to the server at %s \n", debug_buffer);

    // write a pattern
    write_pattern(tx_buffer, test_buffer_sz);

    // send test buffer
    while (so_far < test_buffer_sz){
        ret = send(server_fd, tx_buffer + so_far, test_buffer_sz - so_far, 0);
        if( 0 > ret){
            printf("Error: send failed with ret %d and errno %d \n", ret, errno);
            goto free_bufs;
        }
        so_far+=ret;
        printf("\t [send loop] %d bytes, looping again, so_far %d target %d \n", ret, so_far, test_buffer_sz);
    }

    printf("OK: buffer sent successfully \n");
    printf("OK: waiting to receive data \n");
    // receive test buffer
    so_far = 0;
    while (so_far < test_buffer_sz) {
        ret = recv(server_fd, rx_buffer + so_far, test_buffer_sz - so_far, 0);
        if( 0 > ret){
            printf("Error: recv failed with ret %d and errno %d \n", ret, errno);
            goto free_bufs;
        }
        so_far+=ret;
        //printf("\t [receive loop] %d bytes, looping again, so_far %d target %d \n", ret, so_far, test_buffer_sz);
    }
    printf("Results of pattern matching: %s \n", match_pattern(rx_buffer, test_buffer_sz));
    
    // close the socket
    // now we sleep a bit to drain the queues and then trigger the close logic
    printf("A 5 sec wait before calling close \n");
    sleep(5);
    printf("OK: closing the socket \n");
    ret = close(server_fd);
    if(ret){
        printf("Shutdown was not clean , ret %d errno %d \n ", ret, errno);
        goto free_bufs;
    }
    printf("OK: shutdown was fine. Good bye!\n");
    free_bufs:
    free(tx_buffer);
    free(rx_buffer);
    printf("OK: buffers freed \n");
    return ret;
}


static int show_help(){
    printf("Usage: anp_client -a ip_address -p port -c -h \n");
    printf("-a : a.b.c.d IPv4 address of the server (REQUIRED!)\n");
    printf("-p : port, as an integer (default, %d). \n", PORT);
    printf("-c : 1, 2, 3 which of the 3 configs (1=small, 2=medium, 3=large) to test, default is 1=small (4KB (default), 32KB, and 1MB).\n");
    printf("-w : wait for user input before starting the test. This is useful to listen on the tap0 interface, which is only created while the program is running.\n");
    printf("-h : shows help, and exits with success. No argument needed\n");
    return 0;
}

struct thread_args {
    int config;
    struct sockaddr_in *server_addr;
};


void *thread_func(void *arg)
{
    struct thread_args *args = (struct thread_args *) arg;
    long s = epoch_time();
    int ret;
    switch (args->config) {
        case 1:
            ret = run_test(SMALL_BUF, args->server_addr);
            assert(ret == 0);
            break;
        case 2:
            ret = run_test(MEDIUM_BUF, args->server_addr);
            assert(ret == 0);
            break;
        case 3:
            ret = run_test(LARGE_BUF, args->server_addr);
            assert(ret == 0);
            break;
        default:
            printf("Wrong config number %d \n", args->config);
            show_help();
            exit(-1);
    }
    s = epoch_time() - s;
    s-=5000000; //5second sleeping time
    printf("Time for the test is (microseconds): %lu \n", s);
    return NULL;
}

int main(int argc, char** argv)
{
    char c;
    int config = 1;
    int port = PORT;
    int ret = 0;
    // debug
    char debug_buffer[INET_ADDRSTRLEN];
    int num_threads = 1;
    // set the default values
    struct sockaddr_in server_addr;
    bzero(&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    server_addr.sin_port = htons(port);

    bool wait = false;

    while ((c = getopt(argc, argv, "a:p:c:h:w:P:")) != -1) {
        switch (c) {
            case 'h':
                show_help();
                exit(0);
            case 'c':
                config = strtol(optarg, NULL, 0);
                break;
            case 'a':
                // it overwrites the port value, so must be reset the port value
                ret = get_addr(optarg, (struct sockaddr*) &server_addr);
                if (ret) {
                    printf("Invalid IP %s \n", optarg);
                    return ret;
                }
                server_addr.sin_port = htons(port);
                break;
            case 'p':
                port = strtol(optarg, NULL, 0);
                // does not touch the IP address, so ok
                server_addr.sin_port = htons(port);
                break;
            case 'w':
                wait = true;
                break;
            case 'P':
                num_threads = strtol(optarg, NULL, 0);
                break;
            default:
                show_help();
                exit(-1);
        }
    }
    inet_ntop( AF_INET, &server_addr.sin_addr, debug_buffer, sizeof(debug_buffer));
    printf("[client] working with the following IP: %s and port %d (config: %d) \n", debug_buffer, ntohs(server_addr.sin_port), config);

    struct thread_args args;
    args.config = config;
    args.server_addr = &server_addr;

    pthread_t threads[num_threads];
    for (int i = 0; i < num_threads; i++) {
        pthread_create(&threads[i], NULL, thread_func, &args);
    }
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }
    
    return ret;
}