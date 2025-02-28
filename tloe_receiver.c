#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include "tloe_receiver.h"
#include "tloe_frame.h"
#include "tloe_endpoint.h"
#include "tloe_common.h"
#include "tloe_connection.h"
#include "retransmission.h"
#include "timeout.h"

static void serve_ack(tloe_endpoint_t *e, tloe_frame_t *recv_tloeframe) {
    // Slide retransmission buffer for flushing ancester frames
    // Note that ACK/NAK transmit the sequence number of the received frame as seq_num_ack
    slide_window(e, recv_tloeframe->header.seq_num_ack);

    // Additionally, NAK re-transmit the frames in the retransmission buffer
    if (recv_tloeframe->header.ack == TLOE_NAK)  // NAK
        retransmit(e, tloe_seqnum_next(recv_tloeframe->header.seq_num_ack));

    // Update sequence numbers
    // Note that next_rx_seq must not be increased
    e->acked_seq = recv_tloeframe->header.seq_num_ack; // we need the acked_seq?
    e->ack_cnt++;

    if (e->ack_cnt % 100 == 0) {
        fprintf(stderr, "next_tx: %d, ackd: %d, next_rx: %d, ack_cnt: %d\n",
                e->next_tx_seq, e->acked_seq, e->next_rx_seq, e->ack_cnt);
    }
}

static void serve_oos_request(tloe_endpoint_t *e, tloe_frame_t *recv_tloeframe, uint32_t seq_num) {
    int enqueued;
    // The received TLoE frame is out of sequence, indicating that some frames were lost
    // The frame should be dropped, NEXT_RX_SEQ is not updated
    // A negative acknowledgment (NACK) is sent using the last properly received sequence number
    uint32_t last_proper_rx_seq = seq_num;
    tloe_frame_t *tloeframe = (tloe_frame_t *)malloc(sizeof(tloe_frame_t));

    fprintf(stderr, "TLoE frame is out of sequence with "
            "seq_num: %d, next_rx_seq: %d, last: %d\n",
            recv_tloeframe->header.seq_num, e->next_rx_seq, last_proper_rx_seq);

    // If the received frame contains data, enqueue it in the message buffer
    BUG_ON(is_ack_msg(recv_tloeframe), "received frame must not be an ack frame.");

    *tloeframe = *recv_tloeframe;
    tloeframe->header.seq_num_ack = last_proper_rx_seq;
    tloeframe->header.ack = TLOE_NAK;  // NAK
    tloe_set_mask(tloeframe, 0);  // To indicate ACK
    enqueued = enqueue(e->ack_buffer, (void *) tloeframe);
    BUG_ON(!enqueued, "failed to enqueue ack frame.");

    init_timeout_rx(&(e->iteration_ts), &(e->timeout_rx));

    e->oos_cnt++;
}

static int serve_normal_request(tloe_endpoint_t *e, tloe_frame_t *recv_tloeframe) {
    // printf("RX: Send pakcet to Tx channel for replying ACK/NAK with seq_num: %d, seq_num_ack: %d, ack: %d\n",
    //    tloeframe->header.seq_num, tloeframe->header.seq_num_ack, tloeframe->header.ack);
    // Handle TileLink Msg
    tl_msg_t *tlmsg = (tl_msg_t *)malloc(sizeof(tl_msg_t));
    tloe_get_tlmsg(recv_tloeframe, tlmsg, 0);

    // if tlmsg_buffer is full, send NAK 
    // else send ACK
    BUG_ON(is_queue_full(e->tl_msg_buffer), "tl_msg_buffer is full");

    // Delayed ACK
    if (e->timeout_rx.ack_pending == 0) {
        e->timeout_rx.ack_pending = 1;
        e->timeout_rx.ack_time = get_current_timestamp(&(e->iteration_ts));
    }
    e->timeout_rx.last_ack_seq = recv_tloeframe->header.seq_num;

    // Enqueue to tl_msg_buffer for processing TileLink message if not NAK
    // Update sequence numbers
    e->next_rx_seq = tloe_seqnum_next(recv_tloeframe->header.seq_num);
    e->acked_seq = recv_tloeframe->header.seq_num_ack;
    e->timeout_rx.last_channel = tlmsg->header.chan;
    e->timeout_rx.last_credit = get_tlmsg_credit(tlmsg);

    if (!enqueue(e->tl_msg_buffer, (void *) tlmsg)) { 
        fprintf(stderr, "tl_msg_buffer overflow.\n");
        exit(1);
    }

    e->timeout_rx.ack_cnt++;
    e->delay_cnt++;
}

static void serve_duplicate_request(tloe_endpoint_t *e, tloe_frame_t *recv_tloeframe) {
    int seq_num = recv_tloeframe->header.seq_num;
    fprintf(stderr, "TLoE frame is a duplicate. "
            "seq_num: %d, next_rx_seq: %d\n",
            seq_num, e->next_rx_seq);

    // If the received frame contains data, enqueue it in the message buffer
    BUG_ON(is_ack_msg(recv_tloeframe), "received frame must not be an ack frame.");

    // Delayed ACK
    if (e->timeout_rx.ack_pending == 0) {
        e->timeout_rx.ack_pending = 1;
        e->timeout_rx.ack_time = get_current_timestamp(&(e->iteration_ts));
        e->timeout_rx.last_ack_seq = recv_tloeframe->header.seq_num;
    } else if(tloe_seqnum_cmp(recv_tloeframe->header.seq_num, e->timeout_rx.last_ack_seq) > 0) {
        e->timeout_rx.last_ack_seq = recv_tloeframe->header.seq_num;
    } 

    e->timeout_rx.ack_cnt++;
    e->delay_cnt++;
    e->dup_cnt++;
}

static void enqueue_ack_frame(tloe_endpoint_t *e, tloe_frame_t *recv_tloeframe) {
	int enqueued;
	tloe_frame_t *frame = malloc(sizeof(tloe_frame_t));
	BUG_ON(!frame, "failed to allocate memory for ack frame.");

	*frame = *recv_tloeframe;
	frame->header.seq_num = e->timeout_rx.last_ack_seq;
	frame->header.seq_num_ack = e->timeout_rx.last_ack_seq;
	frame->header.ack = TLOE_ACK;
	tloe_set_mask(frame, 0);
	frame->header.chan = e->timeout_rx.last_channel;
	frame->header.credit = e->timeout_rx.last_credit;
	enqueued = enqueue(e->ack_buffer, (void *) frame);
	BUG_ON(!enqueued, "failed to enqueue ack frame.");

	e->timeout_rx.ack_pending = 0;
	e->timeout_rx.ack_cnt = 0;
	e->delay_cnt--;
}

void RX(tloe_endpoint_t *e) {
	int size;
	chan_credit_t chan_credit;
	tloe_rx_req_type_t req_type;
	char recv_buffer[MAX_BUFFER_SIZE];
	tloe_frame_t *recv_tloeframe = (tloe_frame_t *)malloc(sizeof(tloe_frame_t));
	if (!recv_tloeframe) {
		fprintf(stderr, "%s[%d] failed to allocate memory for recv_tloeframe\n", __FILE__, __LINE__);
		goto out;
	}

	// Receive a frame from the Ethernet layer
	size = tloe_fabric_recv(e, recv_buffer, sizeof(recv_buffer));
	if (size < 0) {
		free(recv_tloeframe);
		goto process_ack;
	}

    // Convert packet into tloe_frame
	packet_to_tloe_frame(recv_buffer, size, recv_tloeframe);

	// ACK/NAK (zero-TileLink)
	if (is_ack_msg(recv_tloeframe)) {
		serve_ack(e, recv_tloeframe);
		free(recv_tloeframe);

		goto process_ack;
	}

#ifdef TEST_TIMEOUT_DROP // (Test) Delayed ACK: Drop a certain number of normal packets
    if (e->master == 0) {
        if (e->drop_npacket_size == 0 && ((rand() % 1000) == 0))
            e->drop_npacket_size = 4;
        if (e->drop_npacket_size > 0) {
            e->drop_npacket_cnt++;
            e->drop_npacket_size--;
            free(recv_tloeframe);
            goto process_ack;
        }
    }
#endif
	
	// printf("RX: Received packet with seq_num: %d, seq_num_ack: %d, ack: %d\n",
	//     tloeframe->seq_num, tloeframe->seq_num_ack, tloeframe->ack);
	req_type = tloe_rx_get_req_type(e, recv_tloeframe);
	switch (req_type) {
		case REQ_NORMAL:

#ifdef TEST_NORMAL_FRAME_DROP
			if ((rand() % 10000) == 99) {
				e->drop_cnt++;
				free(recv_tloeframe);
				goto process_ack;
			}
#endif
            // Normal request packet
            // Handle and enqueue it into the message buffer
            // Handle normal request packet
            serve_normal_request(e, recv_tloeframe);
            free(recv_tloeframe);
            break;
		case REQ_DUPLICATE:
			serve_duplicate_request(e, recv_tloeframe);
			free(recv_tloeframe);
			break;
		case REQ_OOS:
			// recv_tloeframe is not freed here because of the enqueue
			serve_oos_request(e, recv_tloeframe, tloe_seqnum_prev(e->next_rx_seq));
			free(recv_tloeframe);
			break;
	}

process_ack:
	// Send a delayed ACK if the timeout has occurred
	if (is_send_delayed_ack(&(e->iteration_ts), &(e->timeout_rx)))
		enqueue_ack_frame(e, recv_tloeframe);
out:
}

