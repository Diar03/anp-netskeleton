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

#include "icmp.h"
#include "utilities.h"
#include "timer.h"

void icmp_rx(struct subuff *sub)
{
    struct iphdr *ip_ptr = IP_HDR_FROM_SUB(sub);
    struct icmp *icmp_ptr = icmp_hdr(sub);
    uint16_t csum = -1;

    if (IP_PAYLOAD_LEN(ip_ptr) < ICMP_HDR_SIZE) {
        printf("IP payload is too short for ICMP, expected at least 4 bytes, got %hu\n", (ip_ptr->len));
        goto drop_pkt;
    }

    csum = do_csum(icmp_ptr, ip_ptr->len, 0);
    if (csum != 0) {
        printf("Error: invalid checksum, dropping packet\n");
        goto drop_pkt;
    }

    switch (icmp_ptr->type) {
        case ICMP_V4_ECHO:
            icmp_reply(sub);
            break;
        case ICMP_V4_REPLY:
            // In theory, we should never get a reply as we do not send echo requests from our netstack
            goto drop_pkt;
        default:
            perror("Unsupported ICMP type. Dropping...\n");
            goto drop_pkt;
    }

    return;

    drop_pkt:
        free(sub);
        return;
}

void* icmp_reply_handler(void* arg)
{
    struct icmp_reply_info *reply_info = (struct icmp_reply_info *)arg;
    struct iphdr *ip_ptr = IP_HDR_FROM_SUB(reply_info->sub);

    // Reset the sub on subsequent tries since ip_output changes it.
    // We also convert the IP length to the host-byte order since ip_output changes to network-byte order
    reply_info->sub->len = reply_info->icmp_len;
    reply_info->sub->data = (uint8_t *) icmp_hdr(reply_info->sub);

    int status = ip_output(reply_info->dest_addr, reply_info->sub);

    if (status == -EAGAIN) {
        if (reply_info->retries <= 0) {
            goto free_resources;
        }
        reply_info->retries--;
        timer_oneshot(ICMP_RETRY_WAIT_TIME, icmp_reply_handler, arg);  // Schedule the next retry after 100ms
        
    } else {
        goto free_resources;
    }

    return NULL;
    free_resources:
        free_sub(reply_info->sub);
        free(reply_info);
        return NULL;
}

void icmp_reply(struct subuff *sub)
{
    struct iphdr *ip_ptr = IP_HDR_FROM_SUB(sub);
    uint16_t icmp_len = IP_PAYLOAD_LEN(ip_ptr);

    // Go to the end of the packet
    sub_reserve(sub, ETH_HDR_LEN + IP_HDR_LEN + icmp_len);

    struct icmp *icmp_ptr = (struct icmp *)sub_push(sub, icmp_len);

    icmp_ptr->type = ICMP_V4_REPLY;
    icmp_ptr->code = 0;
    // Calculate checksum. According to RFC 792, we set checksum field to 0 when calculating it
    icmp_ptr->checksum = 0;
    icmp_ptr->checksum = do_csum(icmp_ptr, icmp_len, 0);

    sub->protocol = IPP_NUM_ICMP;
    sub->len = icmp_len;

    // The source address now becomes the destination address
    // It has already been converted to host-byte order in ip_rx
    uint32_t dest_addr = ip_ptr->saddr;

    // Populate the reply args
    struct icmp_reply_info *reply_args = calloc(sizeof(struct icmp_reply_info), 1);
    if (reply_args == NULL) {
        goto drop_pkt;
    }
    reply_args->dest_addr = dest_addr;
    reply_args->sub = sub;
    reply_args->retries = ICMP_RETRY_COUNT;            
    reply_args->icmp_len = icmp_len;

    icmp_reply_handler((void *) reply_args);

    return;
    
    drop_pkt:
        free(sub);
        return;
}