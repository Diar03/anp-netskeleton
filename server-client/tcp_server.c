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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <netinet/in.h>
#include <pthread.h>

#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <assert.h>
#include "common.h"

static int show_help(){
    printf("Usage: anp_server -a ip_address -p port -c -h \n");
    printf("-a : a.b.c.d IPv4 address to bind the server to (default 0.0.0.0 (all))\n");
    printf("-p : port, as an integer (default, %d). \n", PORT);
    printf("-c : 1, 2, 3 which of the 3 configs (1=small, 2=medium, 3=large) to test, default is 1=small (4KB (default), 32KB, and 1MB). \n");
    printf("-h : shows help, and exits with success. No argument needed\n");
    return 0;
}


struct thread_args{
    int client_fd;
    int test_buffer_sz;
    struct sockaddr_in client_addr;
};

void* run_test(void *args)
{
    printf("Running test with buffer size %d \n", ((struct thread_args*)args)->test_buffer_sz);
    struct thread_args *targs = (struct thread_args*)args;
    int test_buffer_sz = targs->test_buffer_sz;
    struct sockaddr_in client_addr = targs->client_addr;
    int client_fd = targs->client_fd;
    // debug
    char debug_buffer[INET_ADDRSTRLEN];
    int len = 0, ret = -1, so_far = 0;
    int optval = 1;
    
    

    char *test_buffer = calloc (1, test_buffer_sz);
    if(NULL == test_buffer){
        printf("buffer allocation failed %d \n", -ENOMEM);
        errno = ENOMEM;
        return NULL;
    }

    inet_ntop( AF_INET, &client_addr.sin_addr, debug_buffer, sizeof(debug_buffer));
    printf("new incoming connection from %s \n", debug_buffer);
    // first recv the buffer, then tx it back as it is
    so_far = 0;
    while (so_far < test_buffer_sz) {
        ret = recv(client_fd, test_buffer + so_far, test_buffer_sz - so_far, 0);
        if( 0 > ret){
            printf("Error: recv failed with ret %d and errno %d \n", ret, errno);
            return NULL;
        }
        so_far+=ret;
        printf("\t [receive loop] %d bytes, looping again, so_far %d target %d \n", ret, so_far, test_buffer_sz);
    }
    printf("OK: buffer received ok, pattern match : %s  \n", match_pattern(test_buffer, test_buffer_sz));
    // then tx it back as it is
    so_far = 0;
    while (so_far < test_buffer_sz){
        ret = send(client_fd, test_buffer + so_far, test_buffer_sz - so_far, 0);
        if( 0 > ret){
            printf("Error: send failed with ret %d and errno %d \n", ret, errno);
            return NULL;

        }
        so_far+=ret;
        printf("\t [send loop] %d bytes, looping again, so_far %d target %d \n", ret, so_far, test_buffer_sz);
    }
    printf("OK: buffer tx backed \n");

    // in order to initiate the connection close from the client side, we wait here indefinitely to receive more
    ret = recv(client_fd, test_buffer, test_buffer_sz, 0);
    printf("ret from the recv is %d errno %d \n", ret, errno);

    // close the two fds
    ret =close(client_fd);
    if(ret){
        printf("Error: client shutdown was not clean , ret %d errno %d \n ", ret, errno);
        return NULL;
    }
    //ret = close(listen_fd);
    if(ret){
        printf("Error: server listen shutdown was not clean , ret %d errno %d \n ", ret, errno);
        return NULL;
    }
    printf("OK: server and client sockets closed\n");
    free(test_buffer);
    return NULL;
}

int main(int argc, char** argv){
    char c;
    int config = 1;
    int port = PORT, ret;
    // debug
    char debug_buffer[INET_ADDRSTRLEN];
    int num_threads = 1;
    // set the default values
    struct sockaddr_in server_addr;
    bzero(&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(port);

    while ((c = getopt(argc, argv, "a:p:c:h:P:")) != -1) {
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
            case 'P':
                num_threads = strtol(optarg, NULL, 0);
                break;
            default:
                show_help();
                exit(-1);
        }
    }
    inet_ntop( AF_INET, &server_addr.sin_addr, debug_buffer, sizeof(debug_buffer));
    printf("[server] working with the following IP: %s and port %d (config: %d) \n", debug_buffer, ntohs(server_addr.sin_port), config);

    int listen_fd;
    int optval = 1;

    // create the listening socket
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if ( -1 == listen_fd) {
        printf("Error: listen socket failed, ret %d and errno %d \n", listen_fd, errno);
        return(-errno);
    }
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval));
    printf("Socket successfully created, fd = %d \n", listen_fd);

    // bind the socket
    ret = bind(listen_fd, (struct sockaddr *)&server_addr, sizeof(server_addr));
    if (0 != ret) {
        printf("Error: socket bind failed, ret %d, errno %d \n", ret, errno);
        exit(-errno);
    }
    printf("Socket successfully bind'ed\n");
    // listen on the socket
    
    pthread_t threads[num_threads];
    struct thread_args args;
    
    ret = listen(listen_fd, num_threads); // only 1 backlog
    if (0 != ret) {
        printf("Error: listen failed ret %d and errno %d \n", ret, errno);
        exit(-errno);
    }
    printf("Server listening.\n");

    int connections = 0;

    printf("Waiting for %d connections\n", num_threads);
    while(connections < num_threads){
        struct thread_args *args = calloc(1, sizeof(struct thread_args));
        switch (config) {
            case 1:
                args->test_buffer_sz = SMALL_BUF;
                break;
            case 2:
                args->test_buffer_sz = MEDIUM_BUF;
                break;
            case 3:
                args->test_buffer_sz = LARGE_BUF;
                break;
            default:
                printf("Wrong config number %d \n", config);
                show_help();
                exit(-1);
        }
        int len = sizeof(args->client_addr);
        args->client_fd = accept(listen_fd, (struct sockaddr*)&args->client_addr, &len);
        if ( 0 > args->client_fd) {
            printf("Error: accept failed ret %d errno %d \n", args->client_fd, errno);
            exit(-errno);
        }

        // Dispatch thread
        pthread_create(&threads[connections], NULL, run_test, (void*)args);
        printf("Thread %d created\n", connections);
        connections++;
    }
    
    for(int i = 0; i < num_threads; i++){
        pthread_join(threads[i], NULL);
        printf("Thread %d joined\n", i);
    }

    printf("All threads joined\n");

    close(listen_fd);
    
    return ret;
}