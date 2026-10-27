#ifndef RTP_H_
#define RTP_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __BYTE_ORDER
#define __BIG_ENDIAN 4321
#define __LITTLE_ENDIAN 1234
#elif defined(__SWITCH__)
#define __BIG_ENDIAN 4321
#define __LITTLE_ENDIAN 1234
#define __BYTE_ORDER __LITTLE_ENDIAN
#elif __APPLE__
#include <machine/endian.h>
#else
#include <endian.h>
#endif

#include "config.h"
#include "peer_connection.h"

typedef enum RtpPayloadType {

  PT_PCMU = 0,
  PT_PCMA = 8,
  PT_G722 = 9,
  PT_H264 = 96,
  PT_H265 = 97,
  PT_OPUS = 111

} RtpPayloadType;

typedef enum RtpSsrc {

  SSRC_H264 = 1,
  SSRC_PCMA = 4,
  SSRC_PCMU = 5,
  SSRC_OPUS = 6,

} RtpSsrc;

typedef struct RtpHeader {
#if __BYTE_ORDER == __BIG_ENDIAN
  uint16_t version : 2;
  uint16_t padding : 1;
  uint16_t extension : 1;
  uint16_t csrccount : 4;
  uint16_t markerbit : 1;
  uint16_t type : 7;
#elif __BYTE_ORDER == __LITTLE_ENDIAN
  uint16_t csrccount : 4;
  uint16_t extension : 1;
  uint16_t padding : 1;
  uint16_t version : 2;
  uint16_t type : 7;
  uint16_t markerbit : 1;
#endif
  uint16_t seq_number;
  uint32_t timestamp;
  uint32_t ssrc;
  uint32_t csrc[0];

} RtpHeader;

typedef struct RtpPacket {
  RtpHeader header;
  uint8_t payload[0];

} RtpPacket;

typedef struct RtpMap {
  int pt_h264;
  int pt_opus;
  int pt_pcma;

} RtpMap;

typedef struct RtpEncoder RtpEncoder;
typedef struct RtpDecoder RtpDecoder;
typedef void (*RtpOnPacket)(uint8_t* packet, size_t bytes, void* user_data);
typedef void (*RtpOnAudioPacket)(const PeerAudioPacket* packet, void* user_data);
typedef void (*RtpOnVideoPacket)(const PeerVideoPacket* packet, void* user_data);

// Dynamic 720p frames regularly span more than eight RTP packets. A tiny
// window turns normal UDP burst reordering into artificial packet loss.
#define RTP_REORDER_WINDOW 128
#define RTP_REORDER_MAX_HOLD_PACKETS 64
// 0018: RTP_REORDER_MAX_HOLD_MS is the PER-HOLE MINIMUM wait (单洞最小等待).
// A missing sequence number ("hole") is never abandoned before this many ms,
// so a merely-reordered packet cannot lose the race against a timer tick.
// The real give-up deadline lives much further out, per hole, in
// RTP_GAP_HOLE_MAX_HOLD_MS.  (The old semantic was "the whole gap gets swept
// after 30 ms" -- one late hole threw away every hole in the window at once.)
#define RTP_REORDER_MAX_HOLD_MS 30
// 0018: per-hole ledger.  Every hole lives its own life: registered when it
// is first observed missing, abandoned only after 1500 ms or more than 19
// NACK retries -- and never while a NACK chase is still in flight.  The old
// sweep semantics are what produced the 27 s forced-skip storms
// (forcedSkips=1074 in the worst window).
#define RTP_GAP_HOLE_LEDGER_SIZE 64
#define RTP_GAP_HOLE_MAX_HOLD_MS 1500
#define RTP_GAP_HOLE_MAX_RETRIES 19
// One reorder slot has to hold a whole decrypted inbound packet, so it uses
// the same size as the receive buffer.  With the old CONFIG_MTU-based size a
// full-size packet was rejected by rtp_decoder_store_reordered() and lost.
#define RTP_REORDER_PACKET_CAPACITY (CONFIG_PACKET_BUFFER_SIZE)

// 0018: one ledger entry per missing sequence number ("hole").  Slot index is
// sequence % RTP_GAP_HOLE_LEDGER_SIZE; within one gap run the outstanding
// sequences span less than RTP_REORDER_MAX_HOLD_PACKETS, so they never
// collide in the ledger.
typedef struct RtpGapHole {
  uint16_t sequence;
  uint8_t used;
  // The NACK chase (peer_connection.c) fills these two; rtp.c only reads
  // them when deciding whether a hole may be given up.  See 修改2.
  uint8_t nack_in_flight;
  uint8_t retries;
  uint32_t first_seen_ms;
} RtpGapHole;

struct RtpDecoder {
  RtpPayloadType type;
  RtpOnPacket on_packet;
  RtpOnAudioPacket on_audio_packet;
  RtpOnVideoPacket on_video_packet;
  int (*decode_func)(RtpDecoder* rtp_decoder, uint8_t* data, size_t size);
  void* user_data;
  uint8_t* nalu_buf;
  size_t nalu_capacity;
  size_t nalu_offset;
  uint8_t* au_buf;
  size_t au_capacity;
  size_t au_offset;
  uint32_t au_timestamp;
  uint32_t au_ssrc;
  uint8_t au_started;
  uint8_t au_has_vcl;
  uint8_t au_has_sps;
  uint8_t au_has_pps;
  uint8_t au_damaged;
  uint8_t cached_sps[512];
  size_t cached_sps_size;
  uint8_t cached_pps[256];
  size_t cached_pps_size;
  uint16_t last_seq_number;
  uint8_t has_last_seq_number;
  uint8_t fragment_started;
  uint8_t fragment_damaged;
  uint32_t packets_received;
  uint32_t sequence_gaps;
  uint32_t nal_units_completed;
  uint32_t access_units_completed;
  uint32_t access_units_dropped;
  uint32_t single_packets;
  uint32_t stap_packets;
  uint32_t fu_packets;
  // H.265 (RFC 7798) state.  The payload type of the H.265 stream is NOT
  // fixed: the peer numbers it in the offer and the answer has to use the
  // very same number, so the owner of the connection hands it over.
  // h265_payload_type == 0 means "unknown" -> every video packet is treated
  // as H.264 (the historical behaviour).
  uint8_t h265_payload_type;
  // RFC 7798 section 4.4.3: the 16 bit DONL field is only present when the
  // SDP says sprop-max-don-diff > 0 (and, for an FU, only in the first
  // fragment).  The cloud does not use it, but the decoder supports it.
  uint8_t h265_has_donl;
  uint8_t au_has_vps;
  uint8_t cached_vps[64];
  size_t cached_vps_size;
  uint8_t cached_sps_h265[512];
  size_t cached_sps_h265_size;
  uint8_t cached_pps_h265[64];
  size_t cached_pps_h265_size;
  uint32_t h265_single_packets;
  uint32_t h265_ap_packets;
  uint32_t h265_fu_packets;
  uint8_t* reorder_buf;
  size_t reorder_sizes[RTP_REORDER_WINDOW];
  uint16_t reorder_sequences[RTP_REORDER_WINDOW];
  uint8_t reorder_used[RTP_REORDER_WINDOW];
  uint16_t reorder_expected_sequence;
  uint8_t reorder_has_expected_sequence;
  uint8_t reorder_gap_active;
  uint32_t reorder_gap_started_ms;
  uint32_t reorder_buffered_packets;
  uint32_t reordered_packets;
  uint32_t late_packets_dropped;
  uint32_t forced_sequence_skips;
  // 0018: the per-hole ledger, see RtpGapHole above.
  RtpGapHole gap_holes[RTP_GAP_HOLE_LEDGER_SIZE];
  // 0011: the key frame / PLI bookkeeping.
  //
  //   keyframes_received  how many key frames the depacketizer has seen
  //                       (H.264: NAL type 5 = IDR; H.265: IRAP = 16..23,
  //                       in practice 19 = IDR_W_RADL, 20 = IDR_N_LP,
  //                       21 = CRA)
  //   last_keyframe_ms    wall clock (epoch ms) of the LAST key frame;
  //                       0 = none so far
  //   pli_sent            how many RTCP PLIs ("please send a key frame") we
  //                       have sent
  //   pli_last_sent_ms    wall clock (epoch ms) of the LAST PLI; 0 = never
  //
  //   ★ The two "ms" fields here are ABSOLUTE stamps.  The owner's
  //   PeerVideoRtpStats turns them into AGES ("how long ago") -- see
  //   peer_connection_get_video_rtp_stats().
  //
  //   ★ A key frame is counted **when its NAL unit is assembled**, not when
  //   the access unit is delivered: a key frame that arrives inside a
  //   damaged access unit is still a key frame the cloud sent us (it just
  //   did not survive the reassembler).  Counting it at the delivery point
  //   would hide exactly the case we have to tell apart ("the cloud never
  //   sent a key frame" vs "we threw it away").
  //
  //   ★ The parameter sets (H.264 NAL 7 = SPS / 8 = PPS) are NOT key frames:
  //   they carry no picture, and the cloud sends them in band at the start of
  //   the stream.  They are counted separately by the depacketizer
  //   (au_has_sps / au_has_pps) and they are not what "wait for a key frame"
  //   means.
  uint32_t keyframes_received;
  uint32_t last_keyframe_ms;
  uint32_t pli_sent;
  uint32_t pli_last_sent_ms;
  // 0018: PLI policy counters (修改3).
  //   pli_sent_total        every PLI that actually went out (all paths)
  //   pli_suppressed_stale  requests held back by the key-frame-stale gate
  //   pli_from_forced_skips PLIs triggered by a forced_sequence_skips burst
  uint32_t pli_sent_total;
  uint32_t pli_suppressed_stale;
  uint32_t pli_from_forced_skips;
  // 0012/B: the audio chain counters.  "received" lives on the owner
  // (PeerConnection, classified where the streams are told apart);
  // these start one step deeper:
  //   audio_packets_parsed  rtp_parse_payload() accepted the packet
  //                         (a payload was extracted);
  //   audio_units_produced  the audio callback really fired (a payload
  //                         was handed upward);
  //   audio_fail_short      parse failed: the packet is shorter than an
  //                         RTP header;
  //   audio_fail_shape      parse failed: the CSRC / extension / padding
  //                         / version family ("the shape is wrong");
  //   audio_red_packets     parse OK and payload type 63 (RED, RFC 2198)
  //                         -- NOT unwrapped here, but visibly so;
  //   audio_opus_packets    parse OK and payload type 111 (OPUS).
  // Note: "opus parameters wrong" is NOT observable at this layer
  // (libpeer does not parse opus); that verdict belongs to the engine's
  // decoder error line one layer up.
  uint32_t audio_packets_parsed;
  uint32_t audio_units_produced;
  uint32_t audio_fail_short;
  uint32_t audio_fail_shape;
  uint32_t audio_red_packets;
  uint32_t audio_opus_packets;
  // 0018: 毒包闸门（修改5）-- PT==63 但 RED 剥离失败的整包丢弃数。解不开的
  // RED 交给 opus 只会得到 0017 记录的那种失真，宁可丢。
  uint32_t red_unstripped_dropped;
  // 0013: the heal loop.  keyframes_received counts key frames the
  // depacketizer SAW (at NAL assembly -- a key frame inside a damaged
  // access unit still counts there); keyframes_delivered counts the
  // ones that survived into a COMPLETE access unit handed to the video
  // callback.  The difference is "the cloud sent it, we threw it away".
  // au_had_idr is the transient per-access-unit flag that carries the
  // fact from the NAL assembly to the flush.
  uint32_t keyframes_delivered;
  int au_had_idr;
};

struct RtpEncoder {
  RtpPayloadType type;
  RtpOnPacket on_packet;
  int (*encode_func)(RtpEncoder* rtp_encoder, uint8_t* data, size_t size);
  void* user_data;
  uint16_t seq_number;
  uint32_t ssrc;
  uint32_t timestamp;
  uint32_t timestamp_increment;
  uint8_t buf[CONFIG_MTU + 128];
};

int rtp_packet_validate(uint8_t* packet, size_t size);

void rtp_encoder_init(RtpEncoder* rtp_encoder, MediaCodec codec, RtpOnPacket on_packet, void* user_data);

int rtp_encoder_encode(RtpEncoder* rtp_encoder, const uint8_t* data, size_t size);

void rtp_decoder_init(RtpDecoder* rtp_decoder, MediaCodec codec, RtpOnPacket on_packet, void* user_data);

void rtp_decoder_set_audio_callback(RtpDecoder* rtp_decoder, RtpOnAudioPacket on_packet);
void rtp_decoder_set_video_callback(RtpDecoder* rtp_decoder, RtpOnVideoPacket on_packet);

void rtp_decoder_cleanup(RtpDecoder* rtp_decoder);

int rtp_decoder_decode(RtpDecoder* rtp_decoder, const uint8_t* data, size_t size);

void rtp_decoder_poll(RtpDecoder* rtp_decoder);

// 0018: look up the hole ledger entry for a missing sequence number.
// Returns NULL when the sequence is not (or no longer) tracked as a hole.
// The NACK chase in peer_connection.c uses this to stamp retries /
// nack_in_flight and to detect arrival of a NACKed packet.
RtpGapHole* rtp_decoder_hole_find(RtpDecoder* rtp_decoder, uint16_t sequence);

uint32_t rtp_get_ssrc(uint8_t* packet);

#endif  // RTP_H_
