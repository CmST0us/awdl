/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef WA_NAN_UAPI_H
#define WA_NAN_UAPI_H

/* NL80211_CMD_VENDOR, attributes inside NL80211_ATTR_VENDOR_DATA.
 * Integers use native netlink byte order. FRAME is an entire 802.11 MPDU
 * without FCS; oversize frames fail with EMSGSIZE, never truncation.
 */
#define WA_NAN_VENDOR_ID 0x00e04c
#define WA_NAN_VENDOR_SUBCMD 0x4e01
#define WA_NAN_ABI_VERSION 4
#define WA_NAN_FRAME_MAX 2304
#define WA_NAN_SCHEDULE_SIZE 24
#define WA_NAN_HT_CAP_SIZE 26
#define WA_NAN_VHT_CAP_SIZE 12
#define WA_NAN_HE_CAP_SIZE 21 /* MAC(6), PHY(11), RX/TX MCS80(4) */
#define WA_NAN_CHANNEL_MAX 96

/* Native width encoding: 0=20, 1=40, 2=80 MHz. */
struct wa_nan_channel { unsigned char primary, center, width; };

enum wa_nan_op {
	WA_NAN_OP_GET_CAPS = 1,
	WA_NAN_OP_START,
	WA_NAN_OP_STOP,
	WA_NAN_OP_TX,
	WA_NAN_OP_PEER_ADD,
	WA_NAN_OP_PEER_DEL,
	WA_NAN_OP_KEY_ADD,
	WA_NAN_OP_KEY_DEL,
	WA_NAN_OP_GET_SEQ,
	WA_NAN_OP_SET_SCHEDULE,
	WA_NAN_OP_NDI_ADD,
	WA_NAN_OP_NDI_DEL,
	WA_NAN_OP_SCHEDULE_PROBE, /* diagnostic only; no availability commitment */
	WA_NAN_OP_NDI_CONNECT,
	WA_NAN_OP_NDI_IF_ADD, /* persistent ndi0; no peer or key required */
	WA_NAN_OP_NDI_IF_DEL, /* requires all NDP resources released */
	WA_NAN_OP_NDI_STA_ADD, /* station identity/GTK; no NDP id or initiator */
};

enum wa_nan_attr {
	WA_NAN_ATTR_UNSPEC,
	WA_NAN_ATTR_VERSION,       /* u32, required in commands */
	WA_NAN_ATTR_OP,            /* u32 */
	WA_NAN_ATTR_COOKIE,        /* u64, nonzero host cookie for TX */
	WA_NAN_ATTR_FRAME,         /* binary, 24..WA_NAN_FRAME_MAX */
	WA_NAN_ATTR_FREQ,          /* u32 MHz; 0 means unknown for RX */
	WA_NAN_ATTR_STATUS,        /* s32, 0 or negative errno */
	WA_NAN_ATTR_TX_STATUS,     /* u32, enum rtw_tx_status */
	WA_NAN_ATTR_ACKED,         /* u8, enum wa_nan_ack */
	WA_NAN_ATTR_RX_FLAGS,      /* u32, enum wa_nan_rx_flags */
	WA_NAN_ATTR_RX_COUNTER,    /* u32 raw freerun_cnt, NOT TSF */
	WA_NAN_ATTR_EVENT,         /* u32, enum wa_nan_event */
	WA_NAN_ATTR_FEATURES,      /* u32, enum wa_nan_feature */
	WA_NAN_ATTR_NMI_ADDR,      /* 6 bytes */
	WA_NAN_ATTR_CLUSTER_ID,    /* 6 bytes; GET_CAPS default or native CLUSTER event */
	WA_NAN_ATTR_MAX_FRAME,     /* u32 */
	WA_NAN_ATTR_EVENT_DROPS,   /* u32, allocation/netlink encoding failures */
	WA_NAN_ATTR_RX_OVERSIZE,   /* u32, rejected complete MPDUs */
	WA_NAN_ATTR_PAD,
	WA_NAN_ATTR_PEER_ADDR,     /* 6 bytes, unicast remote NMI */
	WA_NAN_ATTR_KEY_CIPHER,    /* u32, IEEE cipher suite selector */
	WA_NAN_ATTR_KEY_INDEX,     /* u32, pairwise 0 or local IGTK 4/5 */
	WA_NAN_ATTR_KEY_DATA,      /* binary key; KEY_ADD only */
	WA_NAN_ATTR_KEY_OWNER,     /* u32, enum wa_nan_key_owner; absent = pairwise */
	WA_NAN_ATTR_TX_PROTECTION, /* u32, enum wa_nan_tx_protection */
	WA_NAN_ATTR_KEY_COOKIE,    /* u64 fresh TK identity; nonzero */
	WA_NAN_ATTR_RX_PN,         /* 6 bytes LE, verified CCMP PN */
	WA_NAN_ATTR_SCHEDULE_ACTION, /* u32, enum wa_nan_schedule_action */
	WA_NAN_ATTR_SCHEDULE, /* 24 bytes: LE16 period, u8 duration/length,
		LE16 offset16, u8 channel/bw, 16-byte bitmap; SET_SCHEDULE */
	WA_NAN_ATTR_TX_PN, /* 6 bytes LE, initial local BIGTK allocation lower bound */
	WA_NAN_ATTR_NDI_CONFIG, /* struct wa_nan_ndi_config; key material */
	WA_NAN_ATTR_NDI_ADDR, /* 6 bytes, local NDI */
	WA_NAN_ATTR_NDI_IFINDEX, /* u32, actual registered Ethernet interface */
	WA_NAN_ATTR_DW_START, /* u32, native DW start TSF low; DW event */
	WA_NAN_ATTR_HT_CAP, /* 26-byte 2.4 GHz HT capability IE body; GET_CAPS */
	WA_NAN_ATTR_HT_CAP_5G, /* 26-byte 5 GHz HT capability IE body */
	WA_NAN_ATTR_VHT_CAP, /* 12-byte 5 GHz VHT capability IE body */
	WA_NAN_ATTR_CHANNELS, /* array of currently usable wa_nan_channel */
	WA_NAN_ATTR_DATA_CHANNEL, /* wa_nan_channel for peer SET_SCHEDULE */
	WA_NAN_ATTR_HE_CAP, /* normalized 5 GHz HE SU <=80 MHz capability body */
	__WA_NAN_ATTR_AFTER_LAST,
	WA_NAN_ATTR_MAX = __WA_NAN_ATTR_AFTER_LAST - 1
};

/* Single NDP: ADD reserves identity and the local GTK; CONNECT installs the
 * derived data TK and peer GTK. STA_ADD uses the same layout with ndp_id and
 * initiator zero; hostap owns those protocol identities. All fields are octets,
 * with no native padding. */
struct wa_nan_ndi_config {
	unsigned char local[6], peer_nmi[6], peer[6], initiator[6];
	unsigned char ndp_id, gtk_index, gtk[16], tk[16];
	unsigned char peer_gtk_index, peer_gtk[16], peer_pn[6];
	unsigned char ht_mcs[2], ht_info[2]; /* negotiated RX mask and LE HT flags */
	struct wa_nan_channel channel;
	unsigned char vht, vht_mcs[2], vht_info[4];
	unsigned char ampdu_density, ampdu_exp; /* peer RX spacing and max length exponent */
	unsigned char he, he_mcs[2], he_ldpc, he_gi;
	/* he_gi: bit0 SU 1xLTF/0.8us, bit1 SU 4xLTF/0.8us.
	 * Per-width packet extension: 0=0us, 1=8us, 2=16us. */
	unsigned char he_padding[3];
};

enum wa_nan_schedule_action {
	WA_NAN_SCHED_INIT = 1, WA_NAN_SCHED_ADD,
	WA_NAN_SCHED_DELETE, WA_NAN_SCHED_READ,
};

enum wa_nan_key_owner {
	WA_NAN_KEY_OWNER_PAIRWISE = 0,
	WA_NAN_KEY_OWNER_LOCAL_NMI_IGTK = 1,
	WA_NAN_KEY_OWNER_LOCAL_NMI_BIGTK = 2,
	WA_NAN_KEY_OWNER_LOCAL_NDI_GTK = 3,
};

enum wa_nan_tx_protection {
	WA_NAN_TX_NONE, WA_NAN_TX_PAIRWISE_CCMP128,
};

enum wa_nan_event { WA_NAN_EVENT_RX = 1, WA_NAN_EVENT_TX_DONE,
	WA_NAN_EVENT_CLUSTER, WA_NAN_EVENT_DW,
	/* Unicast AWDL data from another sender; PEER_ADDR identifies it. */
	WA_NAN_EVENT_AWDL_SENDER };
enum wa_nan_ack { WA_NAN_ACK_UNKNOWN, WA_NAN_ACK_NO, WA_NAN_ACK_YES };
enum wa_nan_feature {
	WA_NAN_FEAT_MGMT_TX = 1U << 0,
	WA_NAN_FEAT_MGMT_RX = 1U << 1,
	WA_NAN_FEAT_RX_RAW = 1U << 2,
	WA_NAN_FEAT_PEER = 1U << 3,
	WA_NAN_FEAT_PAIRWISE_CCMP128 = 1U << 4, /* installation, not PN or crypto TX */
	WA_NAN_FEAT_LOCAL_IGTK_CAM = 1U << 5, /* one CMAC128 slot; no PN/BIP protection */
	WA_NAN_FEAT_MGMT_PAIRWISE_CCMP128 = 1U << 6,
	WA_NAN_FEAT_LOCAL_BIGTK_TX = 1U << 7, /* native local beacon BIP; no peer RX or NDP PN */
	WA_NAN_FEAT_NDI = 1U << 8, /* single NDP, native FAW, Ethernet data TX/RX */
	WA_NAN_FEAT_PERSISTENT_NDI = 1U << 9,
	WA_NAN_FEAT_NDI_STA = 1U << 10,
	WA_NAN_FEAT_LOCAL_DATA_SCHEDULE = 1U << 11,
};
enum wa_nan_rx_flags {
	WA_NAN_RX_PROTECTED = 1U << 0,
	WA_NAN_RX_HW_DEC = 1U << 1,
	WA_NAN_RX_SW_DEC = 1U << 2, /* descriptor requests software decryption */
	WA_NAN_RX_ICV_ERROR = 1U << 3,
	WA_NAN_RX_CRC_ERROR = 1U << 4,
	WA_NAN_RX_RAW = 1U << 5,
	WA_NAN_RX_CCMP_VERIFIED = 1U << 6,
	WA_NAN_RX_AMPDU = 1U << 7,
};

/* TX ACK is submission acceptance only. A TX_DONE event can arrive before
 * the netlink ACK and carries the same host cookie. A negative submission
 * result has no completion unless ownership already transferred to hardware.
 * RX_RAW preserves hardware bytes, including crypto headers/trailers. HW_DEC
 * is a descriptor bit, not MIC, replay, BIP or peer authentication validation.
 * CCMP_VERIFIED retains FC Protected, removes CCMP header/MIC, and clears
 * RAW. PEER_ADDR, KEY_INDEX=0, KEY_COOKIE and RX_PN identify the verified key.
 * KEY_ADD with KEY_COOKIE installs a fresh TK with TX/RX PN=0. Reinstalling
 * an old TK with reset PN is not supported. DEL must carry its original cookie.
 * TX protection requires plaintext category9 NAN SDF/NAF and peer/index/cookie selectors.
 * Unsupported operations return EOPNOTSUPP; absent features are not promises.
 */
#endif
