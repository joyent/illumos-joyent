/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright 2019 Joyent, Inc.
 */

/*
 * This implements the T=1 protocol state machine. It is implemented in common
 * code so it can be plugged into testing frameworks in userland without needing
 * the full CCID driver.
 */

#include <atr.h>
#include <ccid_t1.h>

#include <sys/sysmacros.h>
#include <sys/debug.h>
#include <sys/strsun.h>
#ifdef _KERNEL
#include <sys/types.h>
#include <sys/sunddi.h>
#include <sys/varargs.h>
#include <sys/strsubr.h>
#else
#include <strings.h>
#include <stdio.h>
#include <stdarg.h>
#endif

/*
 * Track the state of the T=1 protocol. In general, there is a notion of us
 * sending a series of one or more blocks that may be chained. Each block will
 * receive an acknowledgement. When we send the last block that we care about,
 * then we will receive a response which will consist of one or more blocks.
 *
 * These data blocks are called I-blocks. The acknowledgement blocks are called
 * R-blocks. There are also a special third type of block called S-blocks. The
 * S-Blocks are not used for data, but for protocol-level requests.
 *
 * The OS will always initiate requests. The ICC cannot initiate requests.
 *
 * There are a series of conditions that may cause the transmission to fail. In
 * the T=1 specification, these transition us to a series of escalating retry
 * steps:
 *
 *   o Retransmission
 *   o Use of S(RESYNCH) request
 *   o Warm reset
 *
 * The rest of this file provides routines to construct headers, epilogues, and
 * more.
 */

typedef enum {
	T1_T_IBLOCK,
	T1_T_RBLOCK,
	T1_T_SBLOCK
} t1_block_type_t;

const char *
t1_errmsg(t1_state_t *t1)
{
	return (t1->t1_msgbuf);
}

static t1_validate_t
t1_invalid(t1_state_t *t1, t1_validate_t v, const char *fmt, ...)
{
	va_list ap;

	t1->t1_validate = v;
	va_start(ap, fmt);
	(void) vsnprintf(t1->t1_msgbuf, sizeof (t1->t1_msgbuf), fmt, ap);
	va_end(ap);
	return (v);
}

/*
 * Initialize all of our T=1 state when a new ICC has been inserted.
 */
void
t1_state_icc_init(t1_state_t *t1, atr_data_t *atr, size_t maxlen)
{
	size_t csz;
	uint8_t t1len, ifsc;

	/*
	 * Before we reinitialize, if any reply chain had been left behind, free
	 * it.
	 */
	freemsgchain(t1->t1_reply_chain);
	bzero(t1, sizeof (t1_state_t));

	t1len = sizeof (t1_hdr_t);
	t1->t1_checksum = atr_t1_checksum(atr);
	switch (t1->t1_checksum) {
	case ATR_T1_CHECKSUM_LRC:
		t1len += T1_LRC_LENGTH;
		break;
	case ATR_T1_CHECKSUM_CRC:
		t1len += T1_CRC_LENGTH;
		break;
	}

	/*
	 * When looking at our maximum buffer size, we need to subtract both the
	 * CCID header length and the length of a t1 prologue and epilogue.
	 * The length field for a T1 header is a uint8_t. Therefore, if the
	 * card's size is larger for some reason, we further shrink that amount
	 * to fit within our constraints.
	 */
	csz = maxlen - sizeof (ccid_header_t) - t1len;
	if (csz > T1_SIZE_MAX)
		csz = T1_SIZE_MAX;
	ifsc = atr_t1_ifsc(atr);
	t1->t1_maxlen = MIN((uint8_t)csz, ifsc);
	t1->t1_protlen = t1len;
	t1->t1_send_ns = T1_IBLOCK_NS_DEFVAL;
	t1->t1_recv_ns = T1_IBLOCK_NS_DEFVAL;

	t1->t1_flags |= T1_F_ICC_INIT;
}

void
t1_state_icc_fini(t1_state_t *t1)
{
	VERIFY(t1->t1_flags & T1_F_ICC_INIT);
	VERIFY0(t1->t1_flags & T1_F_ALL_CMD_FLAGS);
	VERIFY3P(t1->t1_reply_chain, ==, NULL);

	bzero(t1, sizeof (*t1));
}

/*
 * A new command has been started. Reset all of our state tracking for this new
 * command.
 */
void
t1_state_cmd_init(t1_state_t *t1, const void *ubuf, size_t ulen)
{
	VERIFY((t1->t1_flags & T1_F_ICC_INIT) != 0);

	t1->t1_ubuf = ubuf;
	t1->t1_ulen = ulen;
	t1->t1_uoff = 0;

	bzero(t1->t1_cmdbuf, sizeof (t1->t1_cmdbuf));
	t1->t1_cmdlen = 0;
	bzero(t1->t1_altbuf, sizeof (t1->t1_altbuf));
	t1->t1_altlen = 0;

	/*
	 * If any mblk chain has been left behind, clean it up now.
	 */
	freemsgchain(t1->t1_reply_chain);
	t1->t1_reply_chain = NULL;
	t1->t1_flags |= T1_F_CMD_SENDING;
}

mblk_t *
t1_state_cmd_reply_take(t1_state_t *t1)
{
	mblk_t *mp;
	VERIFY3U(t1->t1_flags & T1_F_CMD_DONE, !=, 0);
	mp = t1->t1_reply_chain;
	t1->t1_reply_chain = NULL;
	return (mp);
}

/*
 * The user has told us that they're done. Clean up some of the initial state to
 * make it ready again.
 */
void
t1_state_cmd_fini(t1_state_t *t1)
{
	t1->t1_flags &= ~T1_F_ALL_CMD_FLAGS;
	t1->t1_ubuf = NULL;
	t1->t1_ulen = 0;
	t1->t1_uoff = 0;
	bzero(t1->t1_cmdbuf, sizeof (t1->t1_cmdbuf));
	bzero(t1->t1_altbuf, sizeof (t1->t1_altbuf));
	t1->t1_validate = T1_VALIDATE_OK;
	bzero(t1->t1_msgbuf, sizeof (t1->t1_msgbuf));
	t1->t1_resend_count = 0;
	freemsgchain(t1->t1_reply_chain);
	t1->t1_reply_chain = NULL;
}

static void
t1_header_iblock(t1_hdr_t *hdr, uint8_t ns, boolean_t chain, uint8_t len)
{
	VERIFY3U(len, <=, T1_SIZE_MAX);

	hdr->t1h_nad = 0;
	hdr->t1h_pcb = T1_TYPE_IBLOCK;

	if ((ns & 0x1) != 0) {
		hdr->t1h_pcb |= T1_IBLOCK_NS;
	}

	if (chain) {
		hdr->t1h_pcb |= T1_IBLOCK_M;
	}

	hdr->t1h_len = len;
}

static void
t1_header_rblock(t1_hdr_t *hdr, uint8_t nr, t1_rblock_status_t status)
{
	hdr->t1h_nad = 0;
	hdr->t1h_pcb = T1_TYPE_RBLOCK;
	if ((nr & 0x1) != 0) {
		hdr->t1h_pcb |= T1_RBLOCK_NR;
	}
	hdr->t1h_pcb |= status;
	hdr->t1h_len = 0;
}

static void
t1_header_sblock(t1_hdr_t *hdr, t1_sblock_op_t op, uint8_t len)
{
	hdr->t1h_nad = 0;
	hdr->t1h_pcb = T1_TYPE_SBLOCK | op;
	hdr->t1h_len = len;
}

/*
 * Checksum len bytes of buf and then store the checksum.
 */
static uint8_t
t1_checksum_lrc(const uint8_t *buf, size_t len)
{
	uint8_t cksum = 0;
	size_t i;

	for (i = 0; i < len; i++) {
		cksum ^= buf[i];
	}

	return (cksum);
}

static void
t1_checksum(t1_state_t *t1, void *buf, size_t len)
{
	uint8_t *u8 = buf;
	switch (t1->t1_checksum) {
	case ATR_T1_CHECKSUM_LRC:
		u8[len] = t1_checksum_lrc(buf, len);
		break;
	case ATR_T1_CHECKSUM_CRC:
	default:
		/* XXX Implement me */
		VERIFY(0);
	}
}

static boolean_t
t1_checksum_check(t1_state_t *t1, const void *buf, size_t len)
{
	uint8_t val;
	switch (t1->t1_checksum) {
	case ATR_T1_CHECKSUM_LRC:
		val = t1_checksum_lrc(buf, len);
		return (val == 0);
	case ATR_T1_CHECKSUM_CRC:
	default:
		/* XXX Implement me */
		return (B_FALSE);
	}

}

static t1_validate_t
t1_validate_hdr(t1_state_t *t1, const void *buf, size_t len,
    t1_block_type_t *typep)
{
	uint8_t seq;
	const t1_hdr_t *hdr;

	/*
	 * Do we have enough data to cover the protocol prologue and epilogue?
	 */
	if (len < t1->t1_protlen) {
		return (t1_invalid(t1, T1_VALIDATE_SHORT, "data payload (%ld) "
		    "less than required protocol length (%u)", len,
		    t1->t1_protlen));
	}

	/*
	 * We have a slight Chicken and Egg problem. We want to look at the
	 * contents of the T=1 header, but it may not have passed its checksum.
	 * To deal with that we start with the assumption that we got all the
	 * data that we expect. In other words that the ccid length equals the
	 * message block length. We'll validate the checksum based on that raw
	 * data. Later, we'll go back and make sure that the header makes
	 * semantic sense.
	 */
	if (!t1_checksum_check(t1, buf, len)) {
		return (t1_invalid(t1, T1_VALIDATE_BAD_CKSUM,
		    "invalid checksum"));
	}

	hdr = buf;
	if (hdr->t1h_nad != T1_DEFAULT_NAD) {
		return (t1_invalid(t1, T1_VALIDATE_BAD_NAD, "received invalid "
		    "NAD value %u, expected %u", hdr->t1h_nad, T1_DEFAULT_NAD));
	}

	if ((hdr->t1h_pcb & T1_TYPE_IMASK) == T1_TYPE_IBLOCK) {
		*typep = T1_T_IBLOCK;

		if ((hdr->t1h_pcb & T1_IBLOCK_RSVD) != 0) {
			return (t1_invalid(t1, T1_VALIDATE_BAD_RBLOCK,
			    "received I-block with non-zero reserved bits: %x",
			    hdr->t1h_pcb & T1_IBLOCK_RSVD));
		}

		/*
		 * Check the length. We save checking for a length of zero in
		 * other conditions, as it may or may not be valid depending on
		 * the chaining situation.
		 */
		if (hdr->t1h_len == T1_INF_RESERVED) {
			return (t1_invalid(t1, T1_VALIDATE_RESV_LEN,
			    "received I-block with reserved length: %u",
			    T1_INF_RESERVED));
		}

		/*
		 * Check the sequence value. Because we've received a I-block,
		 * we need to check against the receiving value.
		 */
		seq = (hdr->t1h_pcb & T1_IBLOCK_NS) != 0;
		if (seq != t1->t1_recv_ns) {
			return (t1_invalid(t1, T1_VALIDATE_BAD_NS,
			    "received I-block with opposite NS value, expected "
			    "%u", t1->t1_recv_ns));
		}
	} else if ((hdr->t1h_pcb & T1_TYPE_RSMASK) == T1_TYPE_RBLOCK) {
		*typep = T1_T_RBLOCK;

		if ((hdr->t1h_pcb & T1_RBLOCK_RESV_MASK) != 0) {
			return (t1_invalid(t1, T1_VALIDATE_BAD_RBLOCK,
			    "received R-block with reserved bits set in PCB: "
			    "0x%x", hdr->t1h_pcb));
		}

		switch (hdr->t1h_pcb & T1_RBLOCK_STATUS_MASK) {
		case T1_RBLOCK_STATUS_OK:
		case T1_RBLOCK_STATUS_PARITY:
		case T1_RBLOCK_STATUS_ERROR:
			break;
		default:
			return (t1_invalid(t1, T1_VALIDATE_BAD_RBLOCK,
			    "received R-block with reserved status: 0x%x",
			    hdr->t1h_pcb & T1_RBLOCK_STATUS_MASK));
		}

		/*
		 * We explicitly don't validate the value in N(R). Because the
		 * way that it is used will vary between normal and error based
		 * operation.
		 */
		if (hdr->t1h_len != 0) {
			return (t1_invalid(t1, T1_VALIDATE_BAD_LEN, "invalid "
			    "header length, expected 0, received %u",
			    hdr->t1h_len));
		}
	} else if ((hdr->t1h_pcb & T1_TYPE_RSMASK) == T1_TYPE_SBLOCK) {
		uint8_t explen;

		*typep = T1_T_SBLOCK;

		switch (hdr->t1h_pcb & T1_SBLOCK_OP_MASK) {
		case T1_SBLOCK_REQ_RESYNCH:
		case T1_SBLOCK_RESP_RSYNCH:
		case T1_SBLOCK_REQ_ABORT:
		case T1_SBLOCK_RESP_ABORT:
			explen = 0;
			break;
		case T1_SBLOCK_REQ_WTX:
		case T1_SBLOCK_RESP_WTX:
		case T1_SBLOCK_REQ_IFS:
		case T1_SBLOCK_RESP_IFS:
			explen = 1;
			break;
		default:
			return (t1_invalid(t1, T1_VALIDATE_BAD_SBLOCK_OP,
			    "Found invalid S-block operation: %x",
			    hdr->t1h_pcb & T1_SBLOCK_OP_MASK));
		}

		if (explen != hdr->t1h_len) {
			return (t1_invalid(t1, T1_VALIDATE_BAD_LEN, "header "
			    "length value (%d) does not match length required "
			    "for S-block (%d)", hdr->t1h_len, explen));
		}
	} else {
		return (t1_invalid(t1, T1_VALIDATE_BAD_PCB, "received invalid "
		    "PCB header type: %u", hdr->t1h_pcb & T1_TYPE_RSMASK));
	}

	if (hdr->t1h_len + t1->t1_protlen != len) {
		return (t1_invalid(t1, T1_VALIDATE_BAD_LEN, "t1 message "
		    "logical length (%u), does not match actual length (%u)",
		    hdr->t1h_len + t1->t1_protlen, len));
	}

	return (T1_VALIDATE_OK);
}

/*
 * Check that it's an sblock and that it's the right op.
 */
static t1_validate_t
t1_validate_sblock(t1_state_t *t1, const t1_hdr_t *hdr, t1_sblock_op_t op)
{
	if ((hdr->t1h_pcb & T1_TYPE_RSMASK) != T1_TYPE_SBLOCK) {
		return (t1_invalid(t1, T1_VALIDATE_BAD_PCB, "invalid pcb mode "
		    "bits for S-block. Expected %u, found %u", T1_TYPE_SBLOCK,
		    hdr->t1h_pcb & T1_TYPE_RSMASK));
	}

	if ((hdr->t1h_pcb & T1_SBLOCK_OP_MASK) != op) {
		return (t1_invalid(t1, T1_VALIDATE_BAD_SBLOCK_OP, "found wrong "
		    "S-block operation. Expected %x, found %x", op,
		    hdr->t1h_pcb & T1_SBLOCK_OP_MASK));
	}
	return (T1_VALIDATE_OK);
}

/*
 * Validate that the reply to the IFSD request is valid and also matches our
 * request.
 */
t1_validate_t
t1_ifsd_resp(t1_state_t *t1, const void *buf, size_t len)
{
	t1_validate_t t;
	t1_block_type_t type;
	const t1_hdr_t *reqhdr, *resphdr;

	if ((t = t1_validate_hdr(t1, buf, len, &type)) != T1_VALIDATE_OK) {
		return (t);
	}

	if ((t = t1_validate_sblock(t1, buf, T1_SBLOCK_RESP_IFS)) !=
	    T1_VALIDATE_OK) {
		return (t);
	}

	reqhdr = (const t1_hdr_t *)t1->t1_altbuf;
	resphdr = buf;
	if (reqhdr->t1h_data[0] != resphdr->t1h_data[0]) {
		return (t1_invalid(t1, T1_VALIDATE_BAD_IFS, "ICC did not echo "
		    "requested IFS (%u), received %u", reqhdr->t1h_data[0],
		    resphdr->t1h_data[0]));
	}

	return (T1_VALIDATE_OK);
}

void
t1_ifsd(t1_state_t *t1, size_t ifsd, const void **cmdbuf, size_t *lenp)
{
	t1_hdr_t *hdr;
	uint8_t val;

	VERIFY((t1->t1_flags & T1_F_ICC_INIT) != 0);

	if (ifsd > T1_SIZE_MAX) {
		val = T1_SIZE_MAX;
	} else {
		val = (uint8_t)ifsd;
	}

	/*
	 * Per ISO/IEC 7816-3:2006, 0x00, and 0xff are reserved values.
	 */
	VERIFY3U(val, !=, 0x00);
	VERIFY3U(val, !=, 0xff);

	t1->t1_altlen = t1->t1_protlen + T1_SBLOCK_IFS_SIZE;
	hdr = (t1_hdr_t *)t1->t1_altbuf;
	t1_header_sblock(hdr, T1_SBLOCK_REQ_IFS, T1_SBLOCK_IFS_SIZE);
	hdr->t1h_data[0] = ifsd;
	t1_checksum(t1, t1->t1_altbuf, sizeof (t1_hdr_t) + T1_SBLOCK_IFS_SIZE);

	*cmdbuf = t1->t1_altbuf;
	*lenp = t1->t1_altlen;
}

/*
 * Generate the next I-block which may be part of a chain.
 */
static void
t1_generate_iblock(t1_state_t *t1)
{
	t1_hdr_t *hdr;
	uint8_t len;
	size_t mrem;
	boolean_t chain;

	/*
	 * First, determine how much data we need to send and if well end up
	 * having more data to send after this.
	 */
	mrem = t1->t1_ulen - t1->t1_uoff;
	VERIFY3U(mrem, !=, 0);

	if (mrem > t1->t1_maxlen) {
		len = t1->t1_maxlen;
		chain = B_TRUE;
	} else {
		len = (uint8_t)mrem;
		chain = B_FALSE;
		t1->t1_flags |= T1_F_DONE_SENDING;
	}
	len = MIN(t1->t1_maxlen, t1->t1_ulen - t1->t1_uoff);
	VERIFY3U(len, !=, 0);

	hdr = (t1_hdr_t *)t1->t1_cmdbuf;
	t1_header_iblock(hdr, t1->t1_send_ns, chain, len);
	bcopy(t1->t1_ubuf + t1->t1_uoff, hdr->t1h_data, len);
	t1_checksum(t1, t1->t1_cmdbuf, len + sizeof (t1_hdr_t));
	t1->t1_cmdlen = len + t1->t1_protlen;
	t1->t1_uoff += len;
	t1->t1_flags |= T1_F_DATA_VALID;
}

/*
 * When sending an R-block, we always use the current expected receive sequence
 * number. This value is incremented every time we receive a message. If we are
 * sending a retransmit, then the current receive sequence number will be the
 * value we expected to receive. Otherwise, it will be the value we next expect,
 * which is what we acknowledge with.
 */
static void
t1_generate_rblock(t1_state_t *t1, t1_rblock_status_t status)
{
	t1_hdr_t *hdr;

	hdr = (t1_hdr_t *)t1->t1_cmdbuf;
	t1_header_rblock(hdr, t1->t1_recv_ns, status);
	t1_checksum(t1, t1->t1_cmdbuf, sizeof (t1_hdr_t));
	t1->t1_cmdlen = t1->t1_protlen;
	t1->t1_flags |= T1_F_DATA_VALID;
}

t1_action_t
t1_step(t1_state_t *t1)
{
	if ((t1->t1_flags & T1_F_CMD_SMASK) != 0) {
		/* XXX Implement me */
		VERIFY(0);
		return (T1_ACTION_SEND_COMMAND);
	}

	if (t1->t1_flags & T1_F_CMD_ERROR) {
		/*
		 * XXX In theory when we have a validation error or otherwise,
		 * we should actually use this as a chance to follow the series
		 * of actions to resynch, among other things. However, at this
		 * time, we instead opt to reset.
		 */
		return (T1_ACTION_WARM_RESET);
	}

	if (t1->t1_flags & T1_F_CMD_RESEND) {
		/*
		 * XXX We should check the resend count here and resend it if
		 * the count is not too high. For the time being, issue a reset.
		 */
		return (T1_ACTION_WARM_RESET);
	}

	switch (t1->t1_flags & T1_F_CMD_MASK) {
	case T1_F_CMD_SENDING:
		t1_generate_iblock(t1);
		break;
	case T1_F_CMD_RECEIVING:
		t1_generate_rblock(t1, T1_RBLOCK_STATUS_OK);
		break;
	case T1_F_CMD_DONE:
		return (T1_ACTION_DONE);
	default:
		/* XXX */
		return (T1_ACTION_WARM_RESET);
	}

	return (T1_ACTION_SEND_COMMAND);
}

/*
 * When we receive an I-Block from the ICC that will be done only once we have
 * finished sending. Note, the sequence number on the I-Block has already been
 * checked. Here we need to:
 *
 *  o Make sure that the state machine is in a state where we should be
 *    receiving I-Blocks
 *  o If this was in response to the last sent block, then we need to both
 *    toggle the expected sending sequence and toggle the state flags to say
 *    that we're receiving.
 *  o Check for the zero-length case
 *  o Save the data for future processing
 *  o Toggle the expected sequence for reciving
 */
t1_validate_t
t1_reply_iblock(t1_state_t *t1, mblk_t *mp)
{
	const t1_hdr_t *t1h;
	t1_state_flags_t donesend = T1_F_CMD_SENDING | T1_F_DONE_SENDING;

	/*
	 * Check that either we're done sending or that we're in the receiving
	 * state.
	 */
	if ((t1->t1_flags & donesend) == donesend) {
		t1->t1_flags &= ~donesend;
		t1->t1_flags |= T1_F_CMD_RECEIVING;
		t1->t1_send_ns ^= 1;
	} else if ((t1->t1_flags & T1_F_CMD_RECEIVING) == 0) {
		t1->t1_flags |= T1_F_CMD_ERROR;
		return (t1_invalid(t1, T1_VALIDATE_BAD_IBLOCK, "received "
		    "unexpected I-block per state flags (0x%x)", t1->t1_flags));
	}

	/*
	 * Check if the more flag is set. If not, note that this is the last
	 * I-block that we expect to receive.
	 */
	t1h = (const t1_hdr_t *)mp->b_rptr;
	if ((t1h->t1h_pcb & T1_IBLOCK_M) == 0) {
		t1->t1_flags |= T1_F_CMD_DONE;
		t1->t1_flags &= ~T1_F_CMD_RECEIVING;
	}

	/*
	 * Check for a zero-length I-Block. This is only allowed as the last
	 * entry in a chain. If not, then this is an error. The presence of a
	 * chain is done based on having received message blocks.
	 */
	if (t1h->t1h_len == 0 && ((t1->t1_flags & T1_F_CMD_DONE) != 0 ||
	    t1->t1_reply_chain == NULL)) {
		t1->t1_flags |= T1_F_CMD_ERROR;
		return (t1_invalid(t1, T1_VALIDATE_BAD_IBLOCK, "received "
		    "zero length I-block with more bit set"));
	}

	/*
	 * Append the mblock. We need to increment the read pointer for the
	 * prologue and decrement the write pointer for the checksum.
	 */
	mp->b_rptr += sizeof (t1_hdr_t);
	switch (t1->t1_checksum) {
	case ATR_T1_CHECKSUM_LRC:
		mp->b_wptr -= T1_LRC_LENGTH;
		break;
	case ATR_T1_CHECKSUM_CRC:
		mp->b_wptr -= T1_CRC_LENGTH;
		break;
	}

	if (t1->t1_reply_chain == NULL) {
		t1->t1_reply_chain = mp;
	} else {
		mblk_t *last = t1->t1_reply_chain;
		while (last->b_cont != NULL)
			last = last->b_cont;
		last->b_cont = mp;
	}

	t1->t1_recv_ns ^= 1;

	return (T1_VALIDATE_OK);
}

/*
 * When we receive an R-Block from the ICC there will be two different cases
 * that arise:
 *
 *   1. We have sent a chain and it is acknowledging it
 *   2. We have received an error and it is asking us to retransmit
 *
 * At this time, any occurence of instance two means that we issue a warm reset
 * request. We don't really support the full proper execution of ISO/IEC
 * 7816-3:2006.
 *
 * In the case of the first case, now that we've had a successful
 * acknowledgement, we need to go through and do the following:
 *
 *   o Toggle the expected sending sequence
 */
t1_validate_t
t1_reply_rblock(t1_state_t *t1, const t1_hdr_t *hdr)
{
	uint8_t nr;
	boolean_t seqmatch;
	t1_rblock_status_t status;

	/*
	 * To determine if this was trying to indicate to us that we received an
	 * error, we need to check two different things. We need to see if the
	 * sequence value matches what we last sent or not. If not, then it is
	 * an acknowledgement and the status in the R-Block should be zero.
	 *
	 * If instead it does match, then the status should be non-zero and that
	 * should tell us to retransmit.
	 *
	 * If we get a case where the R-Block doesn't make semantic sense then
	 * we'll consider that an error and issue a warm reset. Though in theory
	 * we could request a retransmit from the ICC.
	 */
	status = hdr->t1h_pcb & T1_RBLOCK_STATUS_MASK;
	nr = (hdr->t1h_pcb & T1_RBLOCK_NR) != 0;
	seqmatch = nr == t1->t1_send_ns;
	if ((seqmatch && status == T1_RBLOCK_STATUS_OK) ||
	    (!seqmatch && status != T1_RBLOCK_STATUS_OK)) {
		/*
		 * This R-Block doesn't make semantic sense.
		 */
		t1->t1_flags |= T1_F_CMD_ERROR;
		return (t1_invalid(t1, T1_VALIDATE_BAD_RBLOCK, "sequence match "
		    "(%u) does not make sense with R-block status code (%u)",
		    seqmatch, status));
	}

	if (seqmatch) {
		/*
		 * This is the retransmit case. We should resend things.
		 */
		t1->t1_resend_count++;
		t1->t1_flags |= T1_F_CMD_RESEND;
		return (T1_VALIDATE_OK);
	}

	/*
	 * Check if we're expecting a chaining style acknowledgement. If not,
	 * then this is a bad block.
	 */
	if ((t1->t1_flags & T1_F_CMD_SENDING) == 0 ||
	    (t1->t1_flags & T1_F_DONE_SENDING) != 0) {
		t1->t1_flags |= T1_F_CMD_ERROR;
		return (t1_invalid(t1, T1_VALIDATE_BAD_RBLOCK, "received "
		    "acknowledgement R-block not in receipt to a chain"));
	}

	/*
	 * Now that we've finally acknowledged this block we've sent, go ahead
	 * and update the sequence number.
	 */
	t1->t1_send_ns ^= 1;

	return (T1_VALIDATE_OK);
}

/*
 * It is possible to receive an S-block at any time. Depending on the type of
 * S-block, different operations will need to occur. There are four different
 * types of S-Block commands:
 *
 *  o RESYNCH - This is used to reset the communication betwen the reader and
 *		the ICC. In theory only the reader is allowed to issue this. If
 *		we receive any kind of RESYNCH request, we note that as an error
 *		and will issue a warm reset.
 *
 *  o IFS -	This is used to change the size of the data that can be
 *		transmitted. This may be issued by the reader or the ICC. The
 *		issuer is describing what size it is that they need to change.
 *		When the ICC is first detected, we will issue an IFS request to
 *		increase the reader's IFS. At any time, the ICC is allowed to
 *		issue an IFS request. However, the initial value is determined
 *		based on the ATR.
 *
 *  o ABORT -	This is used to cancel a chain or series of commands. At this
 *		time, the framework does not issue aborts. If we receive one
 *		from the card, then we will issue a warm reset and fail the
 *		command.
 *
 *  o WTX -	This is used by the card to indicate that it is still
 *		processing; however it requires additional time. When this
 *		is received, we must acknowledge it. However, we must also
 *		increase the default waiting time here and in the command.
 */
t1_validate_t
t1_reply_sblock(t1_state_t *t1, const t1_hdr_t *hdr)
{
	/*
	 * XXX We need to implement this. For the time being state that there's
	 * an error on this that says we need to reset the device.
	 */
	t1->t1_flags |= T1_F_CMD_ERROR;
	return (T1_VALIDATE_OK);
}

/*
 * We've received an arbitrary reply from the ICC. Depending on where we are
 * in working with a chain, this may be an I-block or it may be an R-block.
 * However, the ICC is allowed to interrupt us with an S-block at various times.
 *
 * XXX We should maybe work in the fact that we had a time out into this for
 * proper state machine execution or we can keep treating a timeout as a fatal
 * error. In this case we're referring to a CCID level timeout, not a USB level
 * timeout.
 */
t1_validate_t
t1_reply(t1_state_t *t1, mblk_t *mp)
{
	t1_validate_t t;
	t1_block_type_t type;
	const t1_hdr_t *hdr;

	/*
	 * First validate that the blob of data that we've received makes some
	 * amount of sense. If it does, then we'll see if it makes semantic
	 * sense (in other words, it's something that we expected to receive).
	 *
	 * XXX In a number of these cases we should probably consider
	 * transmitting an R-block rather than just resetting the device.
	 */
	if ((t = t1_validate_hdr(t1, mp->b_rptr, MBLKL(mp), &type)) !=
	    T1_VALIDATE_OK) {
		t1->t1_flags |= T1_F_CMD_ERROR;
		return (t);
	}

	hdr = (const t1_hdr_t *)mp->b_rptr;
	switch (type) {
	case T1_T_IBLOCK:
		return (t1_reply_iblock(t1, mp));
	case T1_T_RBLOCK:
		return (t1_reply_rblock(t1, hdr));
	case T1_T_SBLOCK:
		return (t1_reply_sblock(t1, hdr));
	}

	return (T1_ACTION_WARM_RESET);
}

void
t1_data(t1_state_t *t1, const void **bufp, size_t *lenp)
{
	VERIFY((t1->t1_flags & T1_F_DATA_VALID) != 0);
	*bufp = t1->t1_cmdbuf;
	*lenp = t1->t1_cmdlen;
}
