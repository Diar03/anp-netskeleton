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
#include "subuff.h"
#include "tcp.h"
#include "utilities.h"
#include <stdlib.h>

#define TCP_MIN_DATA_OFFSET     0b01010000

LIST_HEAD(messages_array);
pthread_mutex_t msg_arr_lock = PTHREAD_MUTEX_INITIALIZER;

struct tcp_hdr *tcp_hdr_from_sub(struct subuff *sub){
    struct iphdr *ih = IP_HDR_FROM_SUB(sub);
    return (struct tcp_hdr *)ih->data;
}

void add_msg_to_arr(struct tcb *tcb, struct subuff *sub, int retries){
    struct tcp_message *msg = calloc(sizeof(struct tcp_message), 1);
    if(msg == NULL){
        printf("Error: could not allocate memory for TCP args\n");
        errno = -ENOMEM;
        return;
    }
    list_init(&msg->list);
    msg->tcp_len = sub->len;
    msg->tcp_data_len = sub->dlen;
    msg->tcp_ptr = sub->data;
    msg->tcb = tcb;
    msg->sub = sub;
    msg->retries = retries;
    

    pthread_mutex_lock(&msg_arr_lock);
    list_add_tail(&msg->list, &messages_array);
    pthread_mutex_unlock(&msg_arr_lock);
}

void save_data_and_ack(struct tcb *tcb, struct subuff *sub){

    struct tcp_hdr *tcp_ptr = tcp_hdr_from_sub(sub);
    uint32_t space_left = TCP_RCV_MAX - tcb->rcv_buff_len;  // The maximum buffer size - the current buffer size gives us how much space we have left 
    if(space_left > TCP_RCV_MAX){
        printf("\n\nError: space left is greater than the maximum buffer size\n\n");
    }
    uint32_t final_copy_size = ANP_MIN(sub->dlen, space_left);
    memcpy(tcb->rcv_buff + tcb->rcv_buff_len, sub->payload, final_copy_size);
    tcb->rcv_nxt += final_copy_size;
    tcb->rcv_buff_len += final_copy_size;

    if(final_copy_size > 0){
        tcp_tx(tcb, NULL, 0, TCP_ACK, RETRIES_SENDONCE); // Acknowledge the bytes read. 
    }
    
    if(tcb->rcv_buff_len == TCP_RCV_MAX || tcp_ptr->flags & TCP_PSH){         // If buffer is full or PSH flag is set, wake up receiver
        pthread_cond_signal(&tcb->tcb_cond);  
    }
    free_sub(sub);

    return;
}


uint32_t read_bytes(struct tcb *tcb, char *buf, uint32_t bytes_to_read){
    
    pthread_mutex_lock(&tcb->tcb_lock);
    if(tcb->rcv_buff_len == 0){
        pthread_cond_wait(&tcb->tcb_cond, &tcb->tcb_lock);
    }
    if(tcb->rcv_buff_len < bytes_to_read){
        bytes_to_read = tcb->rcv_buff_len;
    }
    uint32_t readable_size = ANP_MIN(bytes_to_read, tcb->rcv_buff_len);

    memcpy(buf, tcb->rcv_buff, readable_size);
    tcb->rcv_buff_len -= readable_size;
    if(tcb->rcv_buff_len > 0){
        // Shift the remaining bytes to the beggining of the buffer
        memmove(tcb->rcv_buff, tcb->rcv_buff + readable_size, tcb->rcv_buff_len);

    }
    pthread_mutex_unlock(&tcb->tcb_lock);
        
    return readable_size;
}

struct subuff* create_tcp_packet(struct tcb *tcb, char *data, uint32_t data_len, uint8_t flags){
    
    struct subuff *sub = alloc_sub(ETH_HDR_LEN + IP_HDR_LEN + sizeof(struct tcp_hdr) + data_len);
    if(sub == NULL){
        printf("Error: could not allocate memory for subuff\n");
        return NULL;
    }
    sub_reserve(sub, ETH_HDR_LEN + IP_HDR_LEN + sizeof(struct tcp_hdr) + data_len);

    struct tcp_hdr *tcp_ptr = (struct tcp_hdr *)sub_push(sub, sizeof(struct tcp_hdr) + data_len);

    sub->protocol = IPP_TCP;
    sub->len = sizeof(struct tcp_hdr) + data_len;
    sub->dlen = data_len;
    
    tcp_ptr->src_port = htons(tcb->local_port);
    tcp_ptr->dst_port = htons(tcb->remote_port);
    tcp_ptr->seq_num = tcb->snd_nxt;
    tcp_ptr->ack_num = tcb->rcv_nxt;
    tcp_ptr->data_offset = TCP_MIN_DATA_OFFSET;
    tcp_ptr->flags = flags;                       // ACK flag, might need to include PSH flag
    if(flags & TCP_SYN || flags & TCP_FIN){
        tcb->snd_nxt += 1;
    }
    tcp_ptr->window_size = htons(65535U);  // We set the window size to maximum value
    tcp_ptr->urgent_ptr = 0;

    tcp_ptr->seq_num = htonl(tcp_ptr->seq_num);
    tcp_ptr->ack_num = htonl(tcp_ptr->ack_num);
    tcp_ptr->window_size = htons(tcp_ptr->window_size);

    if(data_len > 0){
        memcpy(tcp_ptr->data, data, data_len);
    }
    
    tcp_ptr->checksum = 0;
    tcp_ptr->checksum = do_tcp_csum((uint8_t *)tcp_ptr, sub->len, htons(IPP_TCP), htonl(tcb->local_ip), htonl(tcb->remote_ip));

    return sub;
}

// retries should only be RETRIES_INFINITE, RETRIES_CONN_MAX, or RETRIES_SENDONCE
uint32_t tcp_tx(struct tcb *tcb, char *data, uint32_t data_size, uint8_t flags, int retries){

    if(retries != RETRIES_INFINITE && retries != RETRIES_CONN_MAX && retries != RETRIES_SENDONCE){
        printf("Error: invalid retry value\n");
        return -1;
    }

    char *curr_data = (char *)data;
    uint32_t data_sent = 0;
    while (data_sent <= data_size) {
        pthread_mutex_lock(&tcb->send_lock);
        uint32_t nxt_seg_size = ANP_MIN(tcb->snd_wnd, ANP_MIN(TCP_MAX_SEG_SIZE, data_size - data_sent)); 
        struct subuff *sub = create_tcp_packet(tcb, curr_data, nxt_seg_size, flags);
        if(sub == NULL){
            printf("Error: could not create TCP packet\n");
            return -1;
        }
        curr_data += nxt_seg_size;
        data_sent += nxt_seg_size;
        tcb->snd_nxt += nxt_seg_size;

        add_msg_to_arr(tcb, sub, retries);
        
        if(data_sent == data_size){ 
            pthread_mutex_unlock(&tcb->send_lock);
            break; 
        }                // Break the loop if all data has been sent
        if(tcb->snd_nxt - tcb->snd_una >= tcb->snd_wnd){    // If the window is full, wait for the sender to wake us up
            pthread_cond_wait(&tcb->send_cond, &tcb->send_lock);
        }
        pthread_mutex_unlock(&tcb->send_lock);
    }
    return data_sent;
}

// This function is called when we need to stop retransmitting messages that have been acknowledged
void stop_retransmit(struct tcb *tcb){
    pthread_mutex_lock(&msg_arr_lock);
    struct list_head *item;
    struct tcp_message *entry;
    list_for_each(item, &messages_array) {
        entry = list_entry(item, struct tcp_message, list);

        // TODO: Checking pointers for now, not good in terms of security
        if(entry->tcb == tcb){
            struct tcp_hdr *tcp_ptr = (struct tcp_hdr *)entry->tcp_ptr;

            // Remove message if it has been acknowledged
            tcp_ptr->seq_num = ntohl(tcp_ptr->seq_num);
            if(tcp_ptr->seq_num < tcb->snd_una){
                if(tcp_ptr->seq_num + entry->tcp_data_len > tcb->snd_una){
                    uint32_t bytes_acked = tcb->snd_una - tcp_ptr->seq_num;
                    uint32_t new_size = entry->tcp_data_len - bytes_acked;
                    // Create new packet with the remaining data   
                    struct subuff *new_sub = create_tcp_packet(tcb, tcp_ptr->data + bytes_acked, new_size, TCP_ACK);
                    if(new_sub == NULL){
                        printf("Error: could not create TCP packet\n");
                        pthread_mutex_unlock(&tcb->send_lock);
                        return;
                    }
                    new_sub->dlen = new_size;
                    new_sub->payload = new_sub->data + sizeof(struct tcp_hdr);
                    free_sub(entry->sub);
                    entry->sub = new_sub;
                }else{
                    free_sub(entry->sub);
                    list_del(&entry->list);
                    free(entry);
                }
            }
            tcp_ptr->seq_num = htonl(tcp_ptr->seq_num);
        }
    }
    pthread_mutex_unlock(&msg_arr_lock);
}

void* close_connection(void *arg){
    struct tcb *tcb = (struct tcb *)arg;
    pthread_mutex_lock(&tcb->tcb_lock);
    stop_retransmit(tcb);
    tcb->state = TCP_CLOSED;
    pthread_cond_signal(&tcb->tcb_cond);
    pthread_mutex_unlock(&tcb->tcb_lock);
    return NULL;
}



void tcp_rx(struct subuff *sub){

    struct tcp_hdr *tcp_ptr = tcp_hdr_from_sub(sub);
    struct iphdr *ip_ptr = IP_HDR_FROM_SUB(sub);
    

    if (IP_PAYLOAD_LEN(ip_ptr) < sizeof(struct tcp_hdr)) { // TCP min header size
        printf("IP payload is too short for TCP, expected at least 20 bytes, got %hu\n", (ip_ptr->len));
        goto drop_pkt;
    }

    // shift 8 bits to the right to get the correct value
    uint16_t proto = ip_ptr->proto << 8;
    uint16_t csum = -1;
    int total_tcp_len = IP_PAYLOAD_LEN(ip_ptr);
    csum = do_tcp_csum((uint8_t *)tcp_ptr, total_tcp_len, ntohs(proto), ntohl(ip_ptr->saddr), ntohl(ip_ptr->daddr));
    if (csum != 0) {
        printf("Error: invalid checksum, dropping packet\n");
        goto drop_pkt;
    }

    // Segment length is the total length of the TCP packet minus the header length
    uint32_t segment_len = IP_PAYLOAD_LEN(ip_ptr) - (tcp_ptr->data_offset >> 4) * 4;

    tcp_ptr->src_port = ntohs(tcp_ptr->src_port);
    tcp_ptr->dst_port = ntohs(tcp_ptr->dst_port);
    tcp_ptr->seq_num = ntohl(tcp_ptr->seq_num);
    tcp_ptr->ack_num = ntohl(tcp_ptr->ack_num);
    tcp_ptr->window_size = ntohs(tcp_ptr->window_size);
    ip_ptr->daddr = ntohl(ip_ptr->daddr);

    bool unlocked_tcb = false;
    bool unlocked_send = false;

    // We reverse the source and destination ports and addresses when we look for the TCB
    struct tcb *tcb = get_tcb(tcp_ptr->dst_port, ip_ptr->daddr, tcp_ptr->src_port, ip_ptr->saddr);
    if(tcb == NULL){
        goto drop_pkt;
    }

    pthread_mutex_lock(&tcb->tcb_lock);
    pthread_mutex_lock(&tcb->send_lock);


    if(tcb->state == TCP_CLOSED) {
        // if ack bit is on, send a reset with the ack bit set and the sequence number set to the incoming ack number
        goto drop_pkt;
    }


    bool acceptable_ack = false;
    if(tcb->state == TCP_SYN_SENT){

        if(tcp_ptr->flags & TCP_ACK){

            if(tcb->snd_una <= tcp_ptr->ack_num && tcp_ptr->ack_num <= tcb->snd_nxt){
                acceptable_ack = true;
            }
        }

        if(tcp_ptr->flags & TCP_SYN){
            tcb->rcv_nxt = tcp_ptr->seq_num + 1;
            tcb->irs = tcp_ptr->seq_num;
            if(acceptable_ack){
                tcb->snd_una = tcp_ptr->ack_num;
            }

            stop_retransmit(tcb);

            if(tcb->snd_una > tcb->iss){
                tcb->snd_wnd = tcp_ptr->window_size;
                tcb->state = TCP_ESTABLISHED;
                pthread_mutex_unlock(&tcb->send_lock);
                tcp_tx(tcb, NULL, 0, TCP_ACK, RETRIES_SENDONCE);
                pthread_cond_signal(&tcb->tcb_cond);                // Wakes up waiting thread and unlocks
                pthread_mutex_unlock(&tcb->tcb_lock);
                unlocked_tcb = true;
                unlocked_send = true;

            } else{

                tcb->state = TCP_SYN_RECEIVED;
                tcb->snd_nxt = tcb->iss;
                pthread_mutex_unlock(&tcb->send_lock);
                tcp_tx(tcb, NULL, 0, TCP_SYNACK, RETRIES_CONN_MAX);
                pthread_mutex_unlock(&tcb->tcb_lock);

                unlocked_send = true;
                unlocked_tcb = true;
            }
            return;
        }

        // Drop segment and return if neither SYN nor RST is set
        goto drop_pkt;

    }

    if(!(tcp_ptr->flags & TCP_ACK)){
        goto drop_pkt;
    }

    switch (tcb->state)
    {
    case TCP_SYN_RECEIVED:
        if(tcb->snd_una < tcp_ptr->ack_num && tcp_ptr->ack_num <= tcb->snd_nxt){
            stop_retransmit(tcb);
            tcb->state = TCP_ESTABLISHED;
            tcb->snd_wnd = tcp_ptr->window_size;
            pthread_mutex_unlock(&tcb->send_lock);
            tcp_tx(tcb, NULL, 0, TCP_ACK, RETRIES_SENDONCE);
            pthread_cond_signal(&tcb->tcb_cond);
            unlocked_send = true;
        }else{
            goto drop_pkt;
        }
        break;
    
    case TCP_ESTABLISHED:
        if(tcb->snd_una < tcp_ptr->ack_num && tcp_ptr->ack_num <= tcb->snd_nxt){
            tcb->snd_una = tcp_ptr->ack_num;
            stop_retransmit(tcb);                       // Stops retransmitting messages that have been acknowledged
        }

        tcb->snd_wnd = tcp_ptr->window_size;            // Send window is updated
        pthread_cond_signal(&tcb->send_cond);        // Wake up any threads waiting to send data
        pthread_mutex_unlock(&tcb->send_lock);
        unlocked_send = true;
        break;
    
    case TCP_FIN_WAIT_1:
        /*  
            In addition to the processing for the ESTABLISHED state, if
            our FIN is now acknowledged then enter FIN-WAIT-2 and continue
            processing in that state.
        */
        tcb->state = TCP_FIN_WAIT_2;
        pthread_mutex_unlock(&tcb->send_lock);
        unlocked_send = true;
        break;

    default:
        // Since only case 1 is possible, TCP_CLOSE_WAIT, TCP_CLOSING and TCP_LAST_ACK are ignored
        goto drop_pkt;
    }

    // sixth, check the URG bit -- SKIP

    if(segment_len != 0){
    // seventh, process the segment text
        if(tcp_ptr->seq_num != tcb->rcv_nxt){
        // Out of order packets should be dropped
        goto drop_pkt;
    }
        sub->dlen = segment_len;
        sub->payload = tcp_ptr->data;
        save_data_and_ack(tcb, sub);
    }

    // TODO: eighth, check the FIN bit 

    if(tcb->state == TCP_CLOSED || tcb->state == TCP_SYN_SENT){
        goto drop_pkt;
    }
    if(tcp_ptr->flags & TCP_FIN){
        tcb->rcv_nxt++;
        if(tcb->state == TCP_FIN_WAIT_1){
            tcb->state = TCP_TIME_WAIT;
            tcp_tx(tcb, NULL, 0, TCP_ACK, RETRIES_SENDONCE);
            pthread_mutex_unlock(&tcb->tcb_lock);
            unlocked_tcb = true;
            timer_oneshot(TCP_WAIT_TIME, close_connection, tcb);
        }else if(tcb->state == TCP_FIN_WAIT_2){
            tcb->state = TCP_TIME_WAIT;
            tcp_tx(tcb, NULL, 0, TCP_ACK, RETRIES_SENDONCE);
            pthread_mutex_unlock(&tcb->tcb_lock);
            unlocked_tcb = true;
            timer_oneshot(TCP_WAIT_TIME, close_connection, tcb);
        }
        return;
    }
    
    if(unlocked_tcb == false)
        pthread_mutex_unlock(&tcb->tcb_lock);
    if(unlocked_send == false)
        pthread_mutex_unlock(&tcb->send_lock);
    return;
    drop_pkt:
    if(unlocked_tcb == false)
        pthread_mutex_unlock(&tcb->tcb_lock);
    if(unlocked_send == false)
        pthread_mutex_unlock(&tcb->send_lock);
    free_sub(sub);
    return;
}

void* tcp_retransmit_handler(){
    pthread_mutex_lock(&msg_arr_lock);
    struct list_head *item;
    struct tcp_message *entry;
    list_for_each(item, &messages_array) {
        entry = list_entry(item, struct tcp_message, list);
        
        entry->sub->data = entry->tcp_ptr;
        entry->sub->len = entry->tcp_len;

        // Make sure that the message is on the wire
        while(ip_output(entry->tcb->remote_ip, entry->sub) == -EAGAIN){
            entry->sub->data = entry->tcp_ptr;
            entry->sub->len = entry->tcp_len;
        }

        if(entry->retries == RETRIES_SENDONCE){
            list_del(&entry->list);
            free_sub(entry->sub);
            free(entry);
            continue;
        } else if(entry->retries == RETRIES_INFINITE){
            continue;
        } else{
            entry->retries--;
            if(entry->retries == 0){
                pthread_mutex_lock(&entry->tcb->tcb_lock);
                pthread_cond_signal(&entry->tcb->tcb_cond);
                pthread_mutex_unlock(&entry->tcb->tcb_lock);
                list_del(&entry->list);
                free_sub(entry->sub);
                free(entry);
            }
        }
    }
    pthread_mutex_unlock(&msg_arr_lock);
    
    timer_oneshot(TCP_WAIT_TIME, tcp_retransmit_handler, NULL);
    return NULL;    
}

void free_tcb(struct tcb *tcb){
    pthread_mutex_lock(&tcb->send_lock);
    tcb->snd_nxt = MAX_UINT32;              // We want to stop retransmitting all packets associated with this tcb
    pthread_mutex_unlock(&tcb->send_lock);
    stop_retransmit(tcb);
    free(tcb);
}

