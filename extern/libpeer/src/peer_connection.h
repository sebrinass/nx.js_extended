/**
 * @file peer_connection.h
 * @brief Struct PeerConnection
 */
#ifndef PEER_CONNECTION_H_
#define PEER_CONNECTION_H_

#include <stdint.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum SdpType {
  SDP_TYPE_OFFER = 0,
  SDP_TYPE_ANSWER,
} SdpType;

typedef enum PeerConnectionState {

  PEER_CONNECTION_CLOSED = 0,
  PEER_CONNECTION_NEW,
  PEER_CONNECTION_CHECKING,
  PEER_CONNECTION_CONNECTED,
  PEER_CONNECTION_COMPLETED,
  PEER_CONNECTION_FAILED,
  PEER_CONNECTION_DISCONNECTED,

} PeerConnectionState;

typedef enum DataChannelType {

  DATA_CHANNEL_NONE = 0,
  DATA_CHANNEL_STRING,
  DATA_CHANNEL_BINARY,

} DataChannelType;

typedef enum DecpChannelType {
  DATA_CHANNEL_RELIABLE = 0x00,
  DATA_CHANNEL_RELIABLE_UNORDERED = 0x80,
  DATA_CHANNEL_PARTIAL_RELIABLE_REXMIT = 0x01,
  DATA_CHANNEL_PARTIAL_RELIABLE_REXMIT_UNORDERED = 0x81,
  DATA_CHANNEL_PARTIAL_RELIABLE_TIMED = 0x02,
  DATA_CHANNEL_PARTIAL_RELIABLE_TIMED_UNORDERED = 0x82,
} DecpChannelType;

typedef enum MediaCodec {

  CODEC_NONE = 0,

  /* Video */
  CODEC_H264,
  CODEC_H265,   // RFC 7798 (HEVC over RTP)
  CODEC_VP8,    // not implemented yet
  CODEC_MJPEG,  // not implemented yet

  /* Audio */
  CODEC_OPUS,  // not implemented yet
  CODEC_PCMA,
  CODEC_PCMU,

} MediaCodec;

typedef struct IceServer {
  const char* urls;
  const char* username;
  const char* credential;

} IceServer;

typedef struct PeerAudioPacket {
  uint8_t* data;
  // 0018: 契约字段名是 len（设计文档旧拼写 size 已改名；引擎侧按契约引用）。
  size_t len;
  uint32_t timestamp;
  uint32_t ssrc;
  uint16_t sequence;
  uint8_t payload_type;
  uint8_t marker;
  // 0018: RED 冗余块回填（修改5）。RED（RFC 2198）包剥离后，除 primary
  // 外再多交出**第一个冗余块**（就是上一个 20ms 的免费重传）：
  //   redundant             冗余块数据指针（裸 opus / 无冗余时为 NULL）
  //   redundant_len         冗余块长度（块头 24 bit 的低 10 bit）
  //   redundant_ts_offset   冗余块时间戳偏移（块头 24 bit 的高 14 bit）
  //   red_stripped          1 = 本包是 RED 且剥离成功；0 = 裸 opus
  // ★ data 与 redundant 同指网络缓冲，**回调返回即失效**，消费者必须在
  //   回调内对两块各自 memcpy（双 memcpy），不得存指针。
  const uint8_t* redundant;
  size_t redundant_len;
  uint32_t redundant_ts_offset;
  int red_stripped;
} PeerAudioPacket;

typedef struct PeerVideoPacket {
  uint8_t* data;
  size_t size;
  uint32_t timestamp;
  uint32_t ssrc;
} PeerVideoPacket;

typedef struct PeerVideoRtpStats {
  uint32_t packets_received;
  uint32_t sequence_gaps;
  uint32_t reordered_packets;
  uint32_t late_packets_dropped;
  uint32_t forced_sequence_skips;
  uint32_t reorder_buffered_packets;
  uint32_t access_units_completed;
  uint32_t access_units_dropped;
  uint32_t nack_requests;
  uint32_t nack_packets_requested;
  // 0018: how many NACKed sequence numbers came back (any copy -- a late
  // original or the retransmission itself).  The companion counters
  // video_nack_giveups / video_nack_table_clears live on the connection.
  uint32_t nack_retrans_returned;
  // 0011: the key frame / PLI bookkeeping.
  //
  //   keyframes_received  how many key frames the depacketizer saw
  //                       (H.264 NAL type 5 = IDR; H.265 IRAP = 16..23).
  //                       Counted when the NAL unit is ASSEMBLED, so a key
  //                       frame that was thrown away by the reassembler is
  //                       counted as well -- that is what tells "the cloud
  //                       never sent one" apart from "we lost it".
  //   pli_sent            how many RTCP PLIs ("please send a key frame") we
  //                       sent.  It was ZERO in every run so far: the
  //                       function that builds the PLI had no caller.
  //   pli_last_sent_ms    ★ AGE: how long ago the last PLI went out
  //                       (0 = never).
  //   last_keyframe_ms    ★ AGE: how long ago the last key frame arrived
  //                       (0 = never).
  //
  //   ★ The two "ms" fields are AGES here; the absolute wall clock stamps
  //   live in the decoder (RtpDecoder.last_keyframe_ms / .pli_last_sent_ms).
  uint32_t keyframes_received;
  uint32_t pli_sent;
  uint32_t pli_last_sent_ms;
  uint32_t last_keyframe_ms;
  // 0018: PLI policy counters (修改3), see RtpDecoder for the semantics.
  uint32_t pli_sent_total;
  uint32_t pli_suppressed_stale;
  uint32_t pli_from_forced_skips;
} PeerVideoRtpStats;

typedef struct PeerConfiguration {
  IceServer ice_servers[5];

  MediaCodec audio_codec;
  MediaCodec video_codec;
  DataChannelType datachannel;

  void (*onaudiotrack)(uint8_t* data, size_t size, void* userdata);
  void (*onaudiopacket)(const PeerAudioPacket* packet, void* userdata);
  void (*onvideotrack)(uint8_t* data, size_t size, void* userdata);
  void (*onvideopacket)(const PeerVideoPacket* packet, void* userdata);
  void (*onrtpsenderreport)(uint32_t ssrc, uint64_t ntp_us, uint32_t rtp_timestamp, void* userdata);
  void (*on_request_keyframe)(void* userdata);
  void* user_data;

} PeerConfiguration;

typedef struct PeerConnection PeerConnection;

int peer_connection_get_video_rtp_stats(PeerConnection* pc, PeerVideoRtpStats* stats);

const char* peer_connection_state_to_string(PeerConnectionState state);

PeerConnectionState peer_connection_get_state(PeerConnection* pc);

void* peer_connection_get_sctp(PeerConnection* pc);

PeerConnection* peer_connection_create(PeerConfiguration* config);

/**
 * @brief switch the video codec of a live connection.
 *
 * The video codec is negotiated, not fixed: the remote offer tells which
 * codecs the peer can send and the answer has to name one of them.  This
 * call therefore changes BOTH the codec that is advertised in the local
 * description (the description is built when it is created, so call this
 * BEFORE peer_connection_create_answer()) and the codec the inbound RTP
 * depacketizer runs.
 *
 * @param[in] pc     the peer connection
 * @param[in] codec  CODEC_H264 or CODEC_H265
 * @return 0 on success, -1 when the codec is not supported
 */
int peer_connection_set_video_codec(PeerConnection* pc, MediaCodec codec);

/**
 * @brief tell the video depacketizer which RTP payload type carries H.265.
 *
 * The payload type is NOT a constant: the peer numbers the codecs in ITS
 * offer and the answer has to use the very same numbers, so the number of
 * the H.265 stream has to be handed over.  When it is not (0), every video
 * packet is treated as H.264.
 *
 * @param[in] pc            the peer connection
 * @param[in] payload_type  the RTP payload type of the H.265 stream
 * @return 0 on success, -1 on error
 */
int peer_connection_set_video_h265_payload_type(PeerConnection* pc, int payload_type);

/**
 * @brief enable / disable the DONL field of the H.265 payload (RFC 7798).
 *
 * The 16 bit decoding order number is only present when the SDP announces
 * sprop-max-don-diff > 0.  The cloud does not use it.
 *
 * @param[in] pc     the peer connection
 * @param[in] enable 1 when the DONL field is present
 * @return 0 on success, -1 on error
 */
int peer_connection_set_video_h265_don(PeerConnection* pc, int enable);

void peer_connection_set_diagnostics_enabled(int enabled);
void peer_connection_set_diagnostic_callback(void (*callback)(const char*));

void peer_connection_destroy(PeerConnection* pc);

void peer_connection_close(PeerConnection* pc);

// Returns 1 when a media/control datagram was consumed, otherwise 0.
int peer_connection_loop(PeerConnection* pc);

int peer_connection_create_datachannel(PeerConnection* pc, DecpChannelType channel_type, uint16_t priority, uint32_t reliability_parameter, char* label, char* protocol);

int peer_connection_create_datachannel_sid(PeerConnection* pc, DecpChannelType channel_type, uint16_t priority, uint32_t reliability_parameter, char* label, char* protocol, uint16_t sid);

/**
 * @brief send message to data channel
 * @param[in] peer connection
 * @param[in] message buffer
 * @param[in] length of message
 */
int peer_connection_datachannel_send(PeerConnection* pc, char* message, size_t len);

int peer_connection_datachannel_send_sid(PeerConnection* pc, char* message, size_t len, uint16_t sid);

int peer_connection_datachannel_send_binary_sid(PeerConnection* pc, char* message, size_t len, uint16_t sid);

int peer_connection_send_audio(PeerConnection* pc, const uint8_t* packet, size_t bytes);

int peer_connection_send_video(PeerConnection* pc, const uint8_t* packet, size_t bytes);

int peer_connection_request_video_keyframe(PeerConnection* pc);

void peer_connection_set_remote_description(PeerConnection* pc, const char* sdp, SdpType sdp_type);

/**
 * @brief tell libpeer that the answer went out to the cloud (0037).
 *
 * The ICE connectivity check must not start before the answer is on the
 * wire: the cloud cannot answer a check that arrives before it has
 * processed our answer (it does not know our ICE credentials yet), and it
 * drops it.  The engine calls this when the page reports that the answer
 * has been sent over the signalling channel.
 *
 * If the report never arrives the check starts anyway after
 * PEER_ANSWER_SENT_FALLBACK_MS, so a page that does not implement the
 * report still connects.
 *
 * @param[in] pc   the peer connection
 * @param[in] sent 1 = the answer is on the wire
 */
void peer_connection_set_answer_sent(PeerConnection* pc, int sent);

void peer_connection_set_local_description(PeerConnection* pc, const char* sdp, SdpType sdp_type);

const char* peer_connection_create_offer(PeerConnection* pc);

const char* peer_connection_create_answer(PeerConnection* pc);

/**
 * @brief register callback function to handle packet loss from RTCP receiver report
 * @param[in] peer connection
 * @param[in] callback function void (*cb)(float fraction_loss, uint32_t total_loss, void *userdata)
 * @param[in] userdata for callback function
 */
void peer_connection_on_receiver_packet_loss(PeerConnection* pc,
                                             void (*on_receiver_packet_loss)(float fraction_loss, uint32_t total_loss, void* userdata));

/**
 * @brief Set the callback function to handle onicecandidate event.
 * @param A PeerConnection.
 * @param A callback function to handle onicecandidate event.
 * @param A userdata which is pass to callback function.
 */
void peer_connection_onicecandidate(PeerConnection* pc, void (*onicecandidate)(char* sdp_text, void* userdata));

/**
 * @brief Set the callback function to handle oniceconnectionstatechange event.
 * @param A PeerConnection.
 * @param A callback function to handle oniceconnectionstatechange event.
 * @param A userdata which is pass to callback function.
 */
void peer_connection_oniceconnectionstatechange(PeerConnection* pc,
                                                void (*oniceconnectionstatechange)(PeerConnectionState state, void* userdata));

/**
 * @brief register callback function to handle event of datachannel
 * @param[in] peer connection
 * @param[in] callback function when message received
 * @param[in] callback function when connection is opened
 * @param[in] callback function when connection is closed
 */
void peer_connection_ondatachannel(PeerConnection* pc,
                                   void (*onmessage)(char* msg, size_t len, void* userdata, uint16_t sid),
                                   void (*onopen)(void* userdata),
                                   void (*onclose)(void* userdata));

/**
 * @brief register the callback that fires when the peer's DataChannel opens.
 *
 * 0014: the onopen callback of peer_connection_ondatachannel() fires when
 * the SCTP ASSOCIATION comes up -- that is BEFORE the peer's DCEP OPEN, so
 * it cannot carry a sid.  This callback fires when the channel itself
 * opens and carries the sid and the label of that channel.
 */
void peer_connection_ondatachannelopened(PeerConnection* pc,
                                          void (*onopen)(uint16_t sid, const char* label, void* userdata));

/**
 * @brief the sid of the DataChannel the peer opened (0014).
 *
 * The sid exists only after the peer's DCEP OPEN arrived; before that this
 * returns -1.  Unlike peer_connection_lookup_sid() this does not depend on
 * the channel's label.
 */
int peer_connection_get_datachannel_sid(PeerConnection* pc, uint16_t* sid);

/**
 * @brief the smoothed RTT of the SCTP transport itself (0014).
 *
 * peer_connection_get_rtt_ms() prefers the ICE candidate pair and only
 * falls back to SCTP; this one is always the SCTP transport's own srtt,
 * i.e. the RTT of the DataChannel path.
 */
int peer_connection_get_sctp_rtt_ms(PeerConnection* pc);

int peer_connection_lookup_sid(PeerConnection* pc, const char* label, uint16_t* sid);

char* peer_connection_lookup_sid_label(PeerConnection* pc, uint16_t sid);

/**
 * @brief adds a new remote candidate to the peer connection
 * @param[in] peer connection
 * @param[in] ice candidate
 */
int peer_connection_add_ice_candidate(PeerConnection* pc, char* ice_candidate);

int peer_connection_get_ice_candidate_pair_stats(PeerConnection* pc,
                                                 int* total,
                                                 int* frozen,
                                                 int* inprogress,
                                                 int* succeeded,
                                                 int* failed);
/**
 * @brief How many candidate pairs / local candidates a diagnostics snapshot
 *        carries.
 *
 * libpeer keeps up to AGENT_MAX_CANDIDATE_PAIRS (100) pairs.  The snapshot is
 * capped so the structure stays small enough to fill on the stack of the
 * engine's JS loop thread; the real counts are reported next to the arrays.
 */
#define PEER_DIAG_MAX_CANDIDATE_PAIRS 32
#define PEER_DIAG_MAX_LOCAL_CANDIDATES 10

/** One ICE candidate pair, flattened for a JSON dump. */
typedef struct PeerCandidatePairDiag {
  char local_ip[64];
  uint16_t local_port;
  char local_type[12];
  char remote_ip[64];
  uint16_t remote_port;
  char remote_type[12];
  char state[12];
  int conncheck;
  int selected;
  int nominated;
  // 0040/F1: how many STUN binding responses this pair received.
  //
  // THIS is the number that answers "did the cloud answer this IP?".
  // conncheck only says "how many checks did WE send"; the app side
  // deliberately leaves it out of its comparison, so it is only reliable in
  // the first snapshot.
  uint32_t responses_received;
} PeerCandidatePairDiag;

/** One local ICE candidate, flattened for a JSON dump. */
typedef struct PeerLocalCandidateDiag {
  char ip[64];
  uint16_t port;
  char type[12];
  char proto[8];
  char state[12];
} PeerLocalCandidateDiag;

/**
 * @brief A snapshot of the DTLS and ICE internals (0035).
 *
 * WHY: until now the only thing the engine could report was "the last DTLS
 * write returned N", which comes from dtls_srtp_write() -- that is the SCTP
 * payload write, NOT the handshake.  The handshake's own verdict lived in a
 * local variable of peer_connection_loop() and died with the stack frame, so
 * a failed DTLS handshake was completely invisible.
 *
 * The fingerprints are only exposed as algorithm + length + the first 8
 * characters: they are public negotiated values, but there is no reason to
 * put the whole thing into a log file.
 */
typedef struct PeerConnectionDiagnostics {
  /* --- DTLS --- */
  int dtls_state;               /* DtlsSrtpState: 0 init / 1 handshake / 2 connected */
  int dtls_role;                /* DtlsSrtpRole: 0 client / 1 server */
  int dtls_handshake_ret;       /* return code of the last dtls_srtp_handshake() */
  int dtls_handshake_ret_valid; /* 1 once the handshake ran at least once */
  int dtls_mbedtls_ret;         /* raw mbedTLS code of the last handshake step */
  int dtls_fingerprint_check;   /* 0 unknown / 1 match / -1 mismatch / -2 no peer cert */
  int dtls_handshake_attempts;
  uint32_t dtls_handshake_started_ms;
  uint32_t dtls_handshake_elapsed_ms;
  int dtls_last_write_result;
  int dtls_last_write_is_handshake; /* 1 = the last DTLS record out was a handshake record */
  char dtls_fingerprint_alg[16];    /* our own fingerprint (the one in our answer) */
  int dtls_fingerprint_len;
  char dtls_fingerprint_prefix[16];
  char dtls_remote_fingerprint_alg[16]; /* the fingerprint libpeer compares against */
  int dtls_remote_fingerprint_len;
  char dtls_remote_fingerprint_prefix[16];
  char dtls_offer_fingerprint_alg[16];  /* the fingerprint the offer actually carries */
  int dtls_offer_fingerprint_len;
  char dtls_offer_fingerprint_prefix[16];
  char dtls_setup_attr[16];             /* the offer's a=setup: value */

  /* --- ICE --- */
  uint32_t ice_consent_requests_sent;
  uint32_t ice_consent_attempts;
  uint32_t ice_binding_responses_received;
  uint32_t ice_rtt_ms;
  int ice_local_candidate_count;
  int ice_candidate_pair_count;
  int ice_selected_pair_index;  /* index into ice_candidate_pairs, -1 when none */
  int ice_nominated_pair_index; /* index into ice_candidate_pairs, -1 when none */
  // 0040/F2: the binding responses that arrived but were DROPPED (foreign
  // transaction id, wrong source address or a failed integrity check).
  // 0 means "the cloud never sent us a response we had to throw away".
  uint32_t ice_rejected_responses;
  // 0040/F3: how long after the answer went out did the FIRST connectivity
  // check leave?  -1 = no check has gone out yet.  Before the 0037 fix this
  // was ~3.3 s (100 frames); after it, a few tens of milliseconds.
  int ice_first_check_ms;
  // 0040/F3: the absolute wall clock stamps, so the app can compute any
  // other delta it cares about.  0 = "did not happen".
  uint32_t ice_first_check_at_ms; /* when the first check went out */
  uint32_t ice_answer_sent_ms;    /* when the answer was reported sent */
  // 0040: what the DTLS read path (the bio) did.
  //   dtls_bio_read_calls   every read of the DTLS transport
  //   dtls_bio_want_reads   the reads that found no data (normal)
  //   dtls_bio_eof_returns  the reads that returned 0 -- MUST STAY 0, a 0
  //                         is what mbedTLS reads as EOF (-0x7280)
  //   dtls_bio_max_call_us  the worst single read, in microseconds
  uint32_t dtls_bio_read_calls;
  uint32_t dtls_bio_want_reads;
  uint32_t dtls_bio_eof_returns;
  uint32_t dtls_bio_max_call_us;
  // 0040: the REMB (picture quality) counters.
  uint32_t remb_sent;
  uint32_t remb_bitrate_bps;
  PeerCandidatePairDiag ice_candidate_pairs[PEER_DIAG_MAX_CANDIDATE_PAIRS];
  PeerLocalCandidateDiag ice_local_candidates[PEER_DIAG_MAX_LOCAL_CANDIDATES];
} PeerConnectionDiagnostics;

/**
 * @brief Fill `out` with a snapshot of the DTLS and ICE internals.
 *
 * Returns 0 on success, -1 when the arguments are unusable.
 */
int peer_connection_get_diagnostics(PeerConnection* pc, PeerConnectionDiagnostics* out);

int peer_connection_get_rtt_ms(PeerConnection* pc);

/**
 * @brief The result of the SCTP association creation (0041).
 *
 * WHY: the SCTP association is created right after the DTLS handshake
 * succeeds, and a failure there used to be COMPLETELY invisible: the
 * caller only got a -1, the errno was swallowed, and the SCTP diagnostic
 * channel was never enabled by the engine, so the only thing that reached
 * the log was "SCTP create socket failed".
 *
 * @param pc             the peer connection
 * @param out_attempts   how often sctp_create_association() was tried
 *                       (0 = never: the DTLS handshake never completed)
 * @param out_ret        the return value of the last
 *                       sctp_create_association(): 0 = ok, -1 = failed
 * @param out_fail_reason  why it failed, as a STABLE string:
 *                       "none" | "runtime_not_initialized" | "socket_present"
 *                       | "allocation_failed" | "socket_failed"
 *                       | "setup_failed" | "invalid_context" | "unknown"
 *
 * Returns 0 on success, -1 when the arguments are unusable.
 */
int peer_connection_get_sctp_create_info(PeerConnection* pc,
                                          int* out_attempts,
                                          int* out_ret,
                                          const char** out_fail_reason);

/**
 * @brief How many SCTP packets are waiting in the output ring buffer (0041).
 *
 * This is a LIVE counter (it changes every tick), unlike the frozen verdict
 * of peer_connection_get_sctp_create_info().
 *
 *   0            -> nothing is waiting: either the association was never
 *                   created, or everything (the INIT included) went out.
 *   > 0, staying -> the packets are NOT leaving the device: the drain path
 *                   (sctp_drain() -> dtls_srtp_write()) is broken.  Look for
 *                   "dtls_write_failed" in the diagnostic log.
 *
 * Returns the queue depth, or -1 when the arguments are unusable.
 */
int peer_connection_get_sctp_out_queued(PeerConnection* pc);

/**
 * @brief The ICE tie-breaker of this session.
 *
 * RFC 8445 requires it to be non-zero and stable for the whole session.
 * The value is returned as a 64 bit integer; the caller has to print it in
 * hexadecimal, a JSON number cannot carry 64 bits exactly.
 */
uint64_t peer_connection_get_ice_tie_breaker(PeerConnection* pc);

/**
 * @brief The raw mbedTLS return value of the last DTLS write.
 *
 * Positive = number of bytes written, negative = mbedTLS error code.
 * See dtls_srtp_get_last_write_result().
 */
int peer_connection_get_last_dtls_write_result(PeerConnection* pc);

#ifdef __cplusplus
}
#endif

#endif  // PEER_CONNECTION_H_
