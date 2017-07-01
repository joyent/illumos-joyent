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
 * Copyright (c) 2017, Joyent, Inc.
 */

#ifndef _T1_H
#define	_T1_H

/*
 * Definitions for the T=1 protocol.
 */

#include <sys/types.h>
#include <sys/stream.h>

#ifdef __cplusplus
extern "C" {
#endif

#pragma pack(1)
typedef struct t1_hdr {
	uint8_t		t1h_nad;
	uint8_t		t1h_pcb;
	uint8_t		t1h_len;
	uint8_t		t1h_data[];
} t1_hdr_t;
#pragma pack()

/*
 * Per ISO/IEC 7816-3:2006 11.3.1 the maximum amount of data that we can put in
 * the len member of structure is 254 bytes. The value 255 is reserved for
 * future use.
 */
#define	T1_SIZE_MAX	254

/*
 * The buffer size used for a full T=1 command that includes the maximum size
 * message (T1_SIZE_MAX), the largest size checksum length (2), and a header
 * size (3 bytes), plus one extra byte of padding. 254 + 3 + 2 + 1 = 260.
 */
#define	T1_BUF_MAX	260

/*
 * Per ISO/IEC 7816-3:2006 11.4.2, the default values of the IFSC and IFSD are
 * 32 bytes.
 */
#define	T1_IFSC_DEFAULT	32
#define	T1_IFSD_DEFAULT	32

/*
 * These macros are used to determine what type the data is. An I-Block has the
 * msb set to zero; however, the other types use two bits to determine what the
 * type is.
 */
#define	T1_TYPE_IBLOCK	0x00
#define	T1_TYPE_RBLOCK	0x80
#define	T1_TYPE_SBLOCK	0xc0
#define	T1_TYPE_IMASK	0x80
#define	T1_TYPE_RSMASK	0xc0

#define	T1_IBLOCK_NS	0x40
#define	T1_IBLOCK_M	0x20
#define	T1_IBLOCK_RSVD	0x1f

/*
 * The T1 NS sequence must always start at 0 per ISO/IEC 7816-3:2006 11.6.2.1.
 * This is a one bit counter. To increment it we always do an xor with 1.
 */
#define	T1_IBLOCK_NS_DEFVAL	0

#define	T1_RBLOCK_NR	0x10
/*
 * The way that the specification describes the values of the PCB, any case
 * where the following bits are non-zero is reserved for future use.
 */
#define	T1_RBLOCK_RESV_MASK	0x2c
#define	T1_RBLOCK_STATUS_MASK	0x03

typedef enum t1_rblock_status {
	T1_RBLOCK_STATUS_OK 	= 0x00,
	T1_RBLOCK_STATUS_PARITY	= 0x01,
	T1_RBLOCK_STATUS_ERROR	= 0x02
} t1_rblock_status_t;

#define	T1_SBLOCK_OP_MASK	0x3f

typedef enum t1_sblock_op {
	T1_SBLOCK_REQ_RESYNCH	= 0x00,
	T1_SBLOCK_RESP_RSYNCH	= 0x20,
	T1_SBLOCK_REQ_IFS	= 0x01,
	T1_SBLOCK_RESP_IFS	= 0x21,
	T1_SBLOCK_REQ_ABORT	= 0x02,
	T1_SBLOCK_RESP_ABORT	= 0x22,
	T1_SBLOCK_REQ_WTX	= 0x03,
	T1_SBLOCK_RESP_WTX	= 0x23
} t1_sblock_op_t;

/*
 * Size in bytes for the IFS related requests.
 */
#define	T1_SBLOCK_IFS_SIZE	1

/*
 * The default node address value (used in t1h_nad). The node address is a
 * potential way to try and multiplex a series of messages going to an ICC. We
 * always use the default address and never change this, which is zero. Not all
 * CCID readers support sending with alternative addresses.
 */
#define	T1_DEFAULT_NAD	0

/*
 * Reserved value for INF (t1h_len). Per ISO/IEC 7816-3:2006, 11.3.2.3, a value
 * of 0xff is always reserved.
 */
#define	T1_INF_RESERVED	0xff

/*
 * Length of the checksum in bytes. There are two different checksums defined by
 * ISO/IEC 7816-3:2006. The one in use is determined based on the ATR.
 */
#define	T1_LRC_LENGTH	1
#define	T1_CRC_LENGTH	2

/*
 * List of status values we might return when parsing a response.
 */
typedef enum {
	T1_VALIDATE_OK	= 0,
	T1_VALIDATE_SHORT,
	T1_VALIDATE_BAD_CKSUM,
	T1_VALIDATE_BAD_PCB,
	T1_VALIDATE_BAD_NAD,
	T1_VALIDATE_BAD_SBLOCK_OP,
	T1_VALIDATE_BAD_LEN,
	T1_VALIDATE_RESV_LEN,
	T1_VALIDATE_BAD_IFS,
	T1_VALIDATE_BAD_IBLOCK,
	T1_VALIDATE_BAD_NS,
	T1_VALIDATE_BAD_RBLOCK,
	T1_VALIDATE_BAD_NR,
} t1_validate_t;

typedef enum t1_state_flags {
	T1_F_ICC_INIT		= 1 << 0,
	T1_F_CMD_SENDING 	= 1 << 1,
	T1_F_CMD_RECEIVING 	= 1 << 2,
	T1_F_CMD_ERROR		= 1 << 3,
	T1_F_CMD_DONE		= 1 << 4,
	T1_F_CMD_SRESP		= 1 << 5,
	T1_F_DATA_VALID		= 1 << 6,
	T1_F_DONE_SENDING	= 1 << 7,
	T1_F_CMD_RESEND		= 1 << 8,
} t1_state_flags_t;

#define	T1_F_CMD_MASK	(T1_F_CMD_SENDING | T1_F_CMD_RECEIVING | \
    T1_F_CMD_DONE | T1_F_CMD_SRESP | T1_F_CMD_ERROR)
#define	T1_F_CMD_SMASK	(T1_F_CMD_SRESP)
#define	T1_F_ALL_CMD_FLAGS	(~T1_F_ICC_INIT)

/*
 * State tracking structure that is used for T=1 operations.
 */
typedef struct t1_state {
	t1_state_flags_t	t1_flags;
	/*
	 * The type of T=1 checksum that is in use.
	 */
	atr_t1_checksum_t	t1_checksum;
	/*
	 * The maximum size of the user data that we can use for a T=1 message.
	 */
	uint8_t			t1_maxlen;
	/*
	 * The number of bytes we need to allocate to cover the prologue and
	 * eiplogue of a message.
	 */
	uint8_t			t1_protlen;
	/*
	 * The value of the sending and receiving sequence number that we
	 * expect. The sending and receiving values are separate.
	 */
	uint8_t			t1_send_ns;
	uint8_t			t1_recv_ns;
	/*
	 * Pointer to the command buffer with the user data to send and the
	 * buffer's length.
	 */
	const void		*t1_ubuf;
	size_t			t1_ulen;
	/*
	 * Offset into the user's command buffer.
	 */
	size_t			t1_uoff;
	/*
	 * Buffer to assemble a command in.
	 */
	uint8_t			t1_cmdbuf[T1_BUF_MAX];
	size_t			t1_cmdlen;
	/*
	 * A secondary buffer to use in case we get a reply that has an S-Block.
	 */
	uint8_t			t1_altbuf[T1_BUF_MAX];
	size_t			t1_altlen;
	/*
	 * Used to place validation messages.
	 */
	t1_validate_t		t1_validate;
	char			t1_msgbuf[1024];
	/*
	 * Running count of how many times we've reissued a given command.
	 */
	uint8_t			t1_resend_count;
	/*
	 * mblk(9S) reply data.
	 */
	mblk_t			*t1_reply_chain;
} t1_state_t;

/*
 * Called when a new ICC has been inserted or reset to initialize the T=1 state
 * again for the ICC.
 */
extern void t1_state_icc_init(t1_state_t *, atr_data_t *, size_t);
extern void t1_state_icc_fini(t1_state_t *);

/*
 * Called when a new command should be sent out the ICC.
 */
extern void t1_state_cmd_init(t1_state_t *, const void *, size_t);
extern mblk_t *t1_state_cmd_reply_take(t1_state_t *);
extern void t1_state_cmd_fini(t1_state_t *);

/*
 * Called to generate the data buffer for sending a T=1 IFSD request.
 */
extern void t1_ifsd(t1_state_t *, size_t, const void **, size_t *);
extern t1_validate_t t1_ifsd_resp(t1_state_t *, const void *, size_t);

/*
 * Obtain a more detailed error message when a T=1 validation error occurs.
 */
extern const char *t1_errmsg(t1_state_t *);

typedef enum {
	T1_ACTION_SEND_COMMAND,
	T1_ACTION_WARM_RESET,
	T1_ACTION_DONE
} t1_action_t;

/*
 * These three functions are used to advance the T=1 state machine. The
 * t1_reply() functino should be used when we receive a reply from the ICC.
 *
 * The t1_step() function is used to basically figure out what to do next. This
 * may mean preparing another command or realizing that we're done with
 * everything that we need to. It may also mean that we encounter an error and
 * need to reset the card.
 *
 * The t1_data() function is used to get the data for the next command to
 * execute and its length so a command can be executed.
 */

extern t1_validate_t t1_reply(t1_state_t *, mblk_t *);
extern t1_action_t t1_step(t1_state_t *);
extern void t1_data(t1_state_t *, const void **, size_t *);

/*
 * Used to indicate that we're done processing a command.
 */
extern void t1_finicmd(t1_state_t *, const mblk_t **);

#ifdef __cplusplus
}
#endif

#endif /* _T1_H */
