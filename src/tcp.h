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

#ifndef ANPNETSTACK_TCP_H
#define ANPNETSTACK_TCP_H

#include "systems_headers.h"
#include "subuff.h"
#include "ip.h"
#include "anpwrapper.h"
#include "timer.h"


#define TCP_FIN             0x01
#define TCP_SYN             0x02
#define TCP_RST             0x04
#define TCP_PSH             0x08
#define TCP_ACK             0x10
#define TCP_FINACK          0x11
#define TCP_SYNACK          0x12
#define TCP_URG             0x20

#define TCP_WAIT_TIME       10 //ms
#define TCP_MAX_RETRIES     5
#define TCP_WAKEUP_WAIT     TCP_WAIT_TIME * 4  // Must be at least TCP_WAIT_TIME

#define RETRIES_INFINITE        -1      // Used for data transmission
#define RETRIES_CONN_MAX        10       // Used for connection establishment
#define RETRIES_SENDONCE        -2      // Used for sending single ACKs

#define TCP_MAX_SEG_SIZE    1460
#define TCP_RCV_MAX         65535U

#define MAX_UINT32          0xFFFFFFFFU

//tcp state machine https://www.nsnam.org/docs/release/3.27/models/html/_images/tcp-state-machine.png
enum tcp_states {
    TCP_LISTEN, /* represents waiting for a connection request from any remote
                   TCP and port. */
    TCP_SYN_SENT, /* represents waiting for a matching connection request
                     after having sent a connection request. */
    TCP_SYN_RECEIVED, /* represents waiting for a confirming connection
                         request acknowledgment after having both received and sent a
                         connection request. */
    TCP_ESTABLISHED, /* represents an open connection, data received can be
                        delivered to the user.  The normal state for the data transfer phase
                        of the connection. */
    TCP_FIN_WAIT_1, /* represents waiting for a connection termination request
                       from the remote TCP, or an acknowledgment of the connection
                       termination request previously sent. */
    TCP_FIN_WAIT_2, /* represents waiting for a connection termination request
                       from the remote TCP. */
    TCP_CLOSED, /* represents no connection state at all. */
    TCP_CLOSE_WAIT, /* represents waiting for a connection termination request
                       from the local user. */
    TCP_CLOSING, /* represents waiting for a connection termination request
                    acknowledgment from the remote TCP. */
    TCP_LAST_ACK, /* represents waiting for an acknowledgment of the
                     connection termination request previously sent to the remote TCP
                     (which includes an acknowledgment of its connection termination
                     request). */
    TCP_TIME_WAIT, /* represents waiting for enough time to pass to be sure
                      the remote TCP received the acknowledgment of its connection
                      termination request. */
};

//https://rsjakob.gitbooks.io/iqt-network-programming/osi-layer-4/tcp-header.html
// FIXME: define a TCP header format
struct tcp_hdr {
    uint16_t src_port;   
    uint16_t dst_port;   
    uint32_t seq_num;    
    uint32_t ack_num;    
    uint8_t data_offset;  // Includes the 4 reserved bit field. Should be faster than bit fields
    uint8_t flags;       
    uint16_t window_size; 
    uint16_t checksum;   
    uint16_t urgent_ptr; 
    uint8_t data[];
} __attribute__((packed));

// This is a Transmission Control Block (TCB) data structure as defined in RFC 793
struct tcb {

    enum tcp_states state;
    uint16_t local_port;
    uint16_t remote_port;
    uint32_t local_ip;
    uint32_t remote_ip;

    pthread_mutex_t tcb_lock;
    pthread_cond_t tcb_cond; 


    uint32_t rcv_nxt; 
    uint32_t rcv_wnd; 
    uint32_t irs;

    uint32_t rcv_buff_len;
    char rcv_buff[TCP_RCV_MAX]; 
    

    pthread_mutex_t send_lock; // used for synchronizing sending variables
    pthread_cond_t send_cond; 
    uint32_t snd_una; 
    uint32_t snd_nxt; 
    uint32_t snd_wnd; 
    uint32_t iss; 
};

struct tcp_message{
    struct tcb *tcb;
    struct subuff *sub;
    uint8_t *tcp_ptr;
    uint32_t tcp_len;
    uint32_t tcp_data_len;
    uint32_t retries;
    struct list_head list;
};

struct tcp_hdr *tcp_hdr_from_sub(struct subuff *sub);
void tcp_rx(struct subuff *sub);
uint32_t tcp_tx(struct tcb *tcb, char *data, uint32_t data_size, uint8_t flags, int retries);

void save_data_and_ack(struct tcb *tcb, struct subuff *sub);
uint32_t read_bytes(struct tcb *tcb, char *buf, uint32_t bytes_to_read);

void free_tcb(struct tcb *tcb);

void* tcp_retransmit_handler();
#endif //ANPNETSTACK_TCP_H
