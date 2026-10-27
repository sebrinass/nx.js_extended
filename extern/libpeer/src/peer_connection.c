#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "agent.h"
#include "config.h"
#include "dtls_srtp.h"
#include "ice_candidate_pair_policy.h"
#include "peer_connection.h"
#include "ports.h"
#include "rtcp.h"
#include "rtp.h"
#include "sctp.h"
#include "sdp.h"
#include "twcc.h"

#define STATE_CHANGED(pc, curr_state)                                  \
  if (pc->state != curr_state) {                                       \
    pc->state = curr_state;                                            \
    if (pc->oniceconnectionstatechange) {                              \
      pc->oniceconnectionstatechange(curr_state, pc->config.user_data); \
    }                                                                 \
  }

// 0037: how long the ICE "checking" state may take before it gives up.
//
// Before this the CHECKING state had no timeout at all: with an empty
// candidate pair table peer_connection_loop() just `break`ed and the peer
// connection stayed in "checking" forever (the "48 seconds without any
// reaction").
//
// 0039: 5 s was too short, because this is the budget that actually decides
// the give-up.  agent_select_candidate_pair() hands out ONE pair at a time
// and the per pair budget (AGENT_CONNCHECK_PAIR_TIMEOUT_MS) is 14 s, so a
// 5 s watchdog cancelled the round before the per pair budget could ever
// expire -- the 15 s budget was dead code.
//
// 14 s is the wall clock equivalent of the "300 ticks" the cloud asked for
// (300 ticks are 14.3 s at the 21 fps the device really runs at, 10 s at
// 30 fps).  One ICE round now concludes in ~10-14 s, which is the same
// order of magnitude as the signalling retries (3/9/15/20 s, ~47 s in
// total) -- the two timers no longer fight each other.
//
// ★ The criterion really moves: it is the wall clock since the CHECKING
//   state was entered (reset for every new answer and every new remote),
//   not a counter that could be stuck.
#define PEER_ICE_CHECKING_TIMEOUT_MS 14000

// 0037: how often the CHECKING diagnostic is repeated (ms).
#define PEER_ICE_CHECKING_DIAG_INTERVAL_MS 1000

// 0037: how long the ICE connectivity check waits for the "the answer went
// out" report before it starts anyway.
//
// The report comes from the page (the facade sends the answer over the
// signalling channel and tells the engine).  A page that never reports it
// must not deadlock the connection, so the check starts anyway after this
// many milliseconds.  It is far above the signalling round trip, so the race
// the fallback reintroduces is a rare corner case, not the normal path.
#define PEER_ANSWER_SENT_FALLBACK_MS 1200

// 0040: how long ONE DTLS endpoint may take before it is given up.
//
// It used to be 3500 ms.  That was shorter than the DTLS handshake's own
// retransmission schedule: mbedTLS repeats the handshake flight after 1 s,
// 3 s and 7 s (mbedtls_ssl_conf_handshake_timeout(1000, 4000) in
// dtls_srtp_init()), so at 3.5 s the endpoint was given up BEFORE the
// second retransmission could arrive.
//
// A single lost ClientHello therefore killed the whole DTLS handshake --
// and the recovery path (agent_fail_nominated_remote()) throws the ICE
// round away and starts over, which is a lot of damage for one lost
// packet.
//
// 9 s is above the 7 s at which mbedTLS gives up on its own, so the
// endpoint timeout is now the BACKSTOP and not the primary deadline.
#define PEER_DTLS_ENDPOINT_TIMEOUT_MS 9000

// 0040/R2 (task A): REMB declares AVAILABLE BANDWIDTH ("how much could
// you send me"), never the rate we just happened to receive -- but a
// constant declaration has no convergence either: pinned at 8 Mbit/s it
// would outrun our own receive ceiling (about 6.5 Mbit/s at 21 fps before
// the drain fix, about 13 Mbit/s after) and turn starvation into packet
// loss.  The declaration is therefore an AIMD estimate:
//   quiet window (no receive-side stress)  -> +PEER_REMB_AI_STEP_BPS;
//       (0018/修改4 SUPERSEDED: the additive climb is gone -- the
//        declaration is the sliding-window measurement x 1.1, see
//        PEER_REMB_MEASURED_*; the net-stress decrease and warmup below
//        are kept.)
//   stressed window (gaps / late drops / forced skips / dropped access
//   units / SRTP decrypt failures at PEER_REMB_LOSS_PCT or more of the
//   window's packets) -> x PEER_REMB_MD_NUM / PEER_REMB_MD_DEN;
//   clamped into [PEER_REMB_FLOOR_BPS, PEER_REMB_CEIL_BPS].
// This is what separates "the cloud throttles us" (quiet + low measured
// -> ask for MORE) from "we cannot keep up" (stress -> back OFF): the two
// states need OPPOSITE declarations, and the stress counters are the
// observable that tells them apart.
//
// R3 (2026-10-24 review) refines the AIMD in three ways:
//   T1 the five stress counters split into two families.  net_stress
//      (sequence gaps + late drops + SRTP decrypt failures) is the
//      network's fault and stays the only driver of the multiplicative
//      decrease.  self_stress (forced sequence skips + dropped access
//      units) is us failing to keep up with what ALREADY arrives;
//      lowering the declared bitrate cannot stop us from dropping our
//      own frames, so it no longer triggers the decrease and is only
//      reported (selfStress= in the rtcp_remb diagnostic line).
//   T2 the ceiling drops from 20 Mbit/s to 8 Mbit/s (see
//      PEER_REMB_CEIL_BPS below for why, and for why the review's
//      "clamp against the measured rate" suggestion is NOT adopted).
//   T3 warmup: the first PEER_REMB_WARMUP_WINDOWS windows are
//      additive-only, so the declaration climbs from PEER_REMB_START_BPS
//      to a sane level before the decrease is allowed to fire at all.
//
// The decision lines further down are written flat with plain local names
// ON PURPOSE: 0008-remb-semantics-check.mjs extracts those exact
// expressions from this patch and evaluates them -- keep member access
// out of the extracted expressions.
// 0018: the estimator is the sliding-window MEASUREMENT now (修改4) -- see
// PEER_REMB_MEASURED_* below.  START is only the value of the very first
// REMB (before any window has been measured); FLOOR / CEIL clamp every
// measured-based declaration into [0.5, 8] Mbit/s.
#define PEER_REMB_START_BPS 200000u    /* first declaration of a session */
#define PEER_REMB_FLOOR_BPS 500000u    /* never declare below 0.5 Mbit/s */
// R3/T2: the ceiling is 8 Mbit/s, not the 20 Mbit/s R2 had.  A
// declaration far above what this build can actually receive only
// invites the encoder to overshoot into loss; 8 Mbit/s is already
// generous against the measured receive ceiling of this build.
//
// The review also asked for a second clamp here:
//   declared = min(declared, max(median of the last 8 measured window
//                                rates, PEER_REMB_FLOOR_BPS))
// so that the declaration can never outrun what we actually receive by
// much.  NOT adopted, and the reasoning matters.  When the cloud
// throttles us (a quiet window with a LOW measured rate -- scenario A),
// the measured median sits below the declaration exactly because the
// SENDER is holding back; asking for more than we currently receive is
// the whole point of the declaration in that state.  The clamp would pin
// the declaration at max(median, FLOOR), that is, at the floor while the
// sender starves us -- which resurrects the v0 death spiral (declare
// what you receive, receive less, declare less) the R2 estimate was
// written to escape.  The clamp only ever helps in the state where the
// declaration is already above the true receive ceiling AND the link is
// stressed -- and that state is what the net-stress multiplicative
// decrease already handles.  Only the ceiling tightening is therefore
// taken, and a recent-measured ring buffer for the median is not added
// at all.
// 0018: the ceiling goes back up (修改4).  The 1.2 Mbit/s cap was a
// deliberate 2026-10-26 stopgap ("raise it afterwards") against the AIMD
// that climbed blindly to 8 Mbit/s while the console only absorbed ~0.85
// Mbit/s.  The climb is GONE: the declaration is now DERIVED from the
// sliding-window measurement (x 1.1), so a blind overshoot cannot happen
// any more and the clamp is simply [PEER_REMB_FLOOR_BPS, 8] Mbit/s around
// the measurement.  (The R3/T2 review text above still explains why a
// separate "clamp against a measured median" is NOT added.)
#define PEER_REMB_CEIL_BPS 8000000u    /* never declare above 8 Mbit/s */
// 0018: declared = measured x 11/10 (修改4).  The additive climb
// (+PEER_REMB_AI_STEP_BPS = +0.5 Mbit/s per quiet second) is deleted --
// the measurement is the estimator.
#define PEER_REMB_MEASURED_NUM 11u     /* declared = measured * 11 / 10 */
#define PEER_REMB_MEASURED_DEN 10u
#define PEER_REMB_MD_NUM 7u            /* on net stress: declared * 7/10 */
#define PEER_REMB_MD_DEN 10u
#define PEER_REMB_LOSS_PCT 2u          /* stress at >= 2 percent of packets */
// R3/T3: the warmup window count.  The first five REMB windows of a
// session are additive-only: the declaration starts at 2 Mbit/s, far
// below the steady state, and stress that early is much more likely
// startup turbulence (decoder priming, the first key frame burst) than
// steady state congestion.  Cutting the declaration during warmup would
// pin it near the start value for the whole session.  During warmup a
// stressed window is only recorded (warmup=1 in the rtcp_remb line),
// never acted on.
#define PEER_REMB_WARMUP_WINDOWS 5u
#define PEER_REMB_STATE_INTERVAL_MS 2000 /* how often the timeline line goes out */

// 0040: how often a REMB is sent, and over which window the receive bit
// rate is measured.
#define PEER_REMB_INTERVAL_MS 1000

// 0011: how often a RTCP PLI ("please send a key frame") is repeated.
//
//   WHY: we never sent a single PLI.  peer_connection_request_video_keyframe()
//   was implemented (it builds a real RTCP PLI and sends it) but **nothing in
//   the tree ever called it** -- so the cloud was never asked for a key frame.
//   The official client asks for one on a regular schedule (the reverse
//   engineering of the APK found a "Pli interval:" parameter and
//   "MHYRtcPli ... requesting keyframe"), and it keeps asking until it gets
//   one: its decoder waits for an IDR and shows nothing without it
//   (AndroidDecoder::waitKeyFrame).
//
//   The cloud sends an IDR by itself when a stream starts, but a STATIC
//   picture makes the encoder produce no new IDR at all (the PC capture of
//   the very same session: every frame is 69 bytes -- an all-skip P frame),
//   so without a request there is simply nothing to decode.
//
//   The request is repeated every LIBPEER_PLI_INTERVAL_MS until the first key
//   frame arrives; after that we stop (asking for more key frames would only
//   waste bandwidth).
#ifndef LIBPEER_PLI_INTERVAL_MS
#define LIBPEER_PLI_INTERVAL_MS 300
#endif
// The interval is clamped: below 100 ms the cloud would be flooded, above
// 5000 ms a lost request would take seconds to be repeated.
#ifndef LIBPEER_PLI_INTERVAL_MS_MIN
#define LIBPEER_PLI_INTERVAL_MS_MIN 100
#endif
#ifndef LIBPEER_PLI_INTERVAL_MS_MAX
#define LIBPEER_PLI_INTERVAL_MS_MAX 5000
#endif
// 0011: how long after the transport came up the FIRST PLI goes out.
//
//   The cloud pushes an IDR when the stream starts, so the first request only
//   goes out after the cloud had its chance -- otherwise we would ask for a
//   key frame that is already on the way.
#ifndef LIBPEER_PLI_FIRST_DELAY_MS
#define LIBPEER_PLI_FIRST_DELAY_MS 500
#endif
#ifndef LIBPEER_PLI_FIRST_DELAY_MS_MAX
#define LIBPEER_PLI_FIRST_DELAY_MS_MAX 5000
#endif
// 0011/B2: how STALE the last key frame may be before we ask for another
// one.  The first contract stopped asking after the FIRST key frame --
// the official client's luxury: it does not lose packets, so it never
// needs to ask again.  We DO lose packets (NACK 44/491, RTT 183->519 ms
// in the 2026-10-24 capture) and the cloud inserts NO periodic IDR (one
// in 65 s for the official client), so after the first key frame every
// lost packet corrupted the picture FOREVER -- the "yellow -> black ->
// yellow" loop.  A key frame fresher than this window suppresses the
// request; an older one makes us ask again (and keep asking, at the
// interval, until a new one arrives).
#ifndef LIBPEER_PLI_STALE_MS
// 0018: was 3000 -- too generous.  With per-hole NACK recovery a healthy
// stream heals inside a second; 1.3 s of stale picture already means the
// key frame did not survive and it is cheaper to ask again than to keep
// showing corruption until the next manual PLI.
#define LIBPEER_PLI_STALE_MS 1300
#endif
// The staleness window is clamped: below 1 s we would re-ask for key
// frames a healthy stream does not need, above 30 s a corrupted picture
// would stay on screen for half a minute.
#ifndef LIBPEER_PLI_STALE_MS_MIN
#define LIBPEER_PLI_STALE_MS_MIN 1000
#endif
#ifndef LIBPEER_PLI_STALE_MS_MAX
#define LIBPEER_PLI_STALE_MS_MAX 30000
#endif
// 0011: how often the PLI state is written to the diagnostic log (ms).
#define PEER_PLI_DIAG_INTERVAL_MS 2000

// 0012: how often the RTP classification is written to the diagnostic
// log (ms).  Time-based on purpose: the line must keep printing when
// there is nothing to report -- "audio=0 while total grows" is exactly
// the finding it exists for.
#define PEER_RTP_CLASS_DIAG_INTERVAL_MS 5000

// 0018: the NACK chase (修改2).  Every gap gets its own ledger entry and its
// own retry clock instead of one global "resend everything every 10 ms":
//   first request   5 ms after the gap is first observed
//   retry interval  exponential backoff (越试越慢): 90 / 94 / 98 ms, then
//                   x 1.5 per retry, clamped [5, 300] ms -- see
//                   peer_nack_retry_interval_ms() (纠偏后为递增)
//   give up         after more than 18 retries or 1500 ms of age
// The table is deliberately big enough to hold every gap the reorder window
// can express several times over; hitting the ceiling means something is
// deeply wrong, so the table is cleared and a PLI is fired instead of
// growing without bound.
#define PEER_NACK_CHASE_MAX 1101
#define PEER_NACK_FIRST_DELAY_MS 5
#define PEER_NACK_MAX_RETRIES 18
#define PEER_NACK_MAX_AGE_MS 1500
// 0018: "sequences we once NACKed", a small ring indexed by seq % 256.  An
// arrival whose sequence number is in the ring is a returned retransmission.
#define PEER_NACK_EVER_RING 256

typedef struct PeerNackChase {
  uint16_t sequence;
  uint8_t used;
  uint8_t retries;
  uint32_t created_ms;
  uint32_t last_retry_ms;
} PeerNackChase;

struct PeerConnection {
  PeerConfiguration config;
  PeerConnectionState state;
  Agent agent;
  DtlsSrtp dtls_srtp;
  Sctp sctp;

  char sdp[CONFIG_SDP_BUFFER_SIZE];

  void (*onicecandidate)(char* sdp, void* user_data);
  void (*oniceconnectionstatechange)(PeerConnectionState state, void* user_data);
  void (*on_connected)(void* userdata);
  void (*on_receiver_packet_loss)(float fraction_loss, uint32_t total_loss, void* user_data);
  void (*ondatachannelmessage)(char* msg, size_t len, void* userdata, uint16_t sid);
  void (*ondatachannelopen)(void* userdata);
  void (*ondatachannelclose)(void* userdata);

  /* 0014: fires when the peer's DCEP OPEN arrived -- the only moment at
   *   which the sid and the label of that channel exist. */
  void (*ondatachannelopened)(uint16_t sid, const char* label, void* userdata);

  // temp_buf receives the decrypted DTLS stream (SCTP) and holds the local
  // SDP while it is being assembled; agent_buf receives the raw UDP
  // datagrams (STUN / DTLS / SRTP / SRTCP).  Both have to be able to hold a
  // full inbound datagram, otherwise SRTP packets are truncated.
  uint8_t temp_buf[CONFIG_PACKET_BUFFER_SIZE];
  uint8_t agent_buf[CONFIG_PACKET_BUFFER_SIZE];
  int agent_ret;
  int dtls_pending;
  int b_local_description_created;
  int remote_ice_lite;

  RtpEncoder artp_encoder;
  RtpEncoder vrtp_encoder;
  RtpDecoder vrtp_decoder;
  // The RTP payload type of the H.265 stream (RFC 7798).  It is not a
  // constant: the peer numbers it in its offer.
  int video_h265_payload_type;
  // Whether the H.265 payload carries the DONL field (RFC 7798 4.4.3).
  int video_h265_don;
  RtpDecoder artp_decoder;

  uint32_t remote_assrc;
  uint32_t remote_vssrc;
  uint16_t video_last_nack_expected;
  uint32_t video_last_nack_ms;
  int video_has_last_nack;
  uint32_t video_nack_requests;
  uint32_t video_nack_packets_requested;
  // 0018: the per-gap NACK chase ledger and the "ever NACKed" ring (修改2).
  PeerNackChase video_nack_chase[PEER_NACK_CHASE_MAX];
  uint16_t video_nack_ever_sequences[PEER_NACK_EVER_RING];
  uint8_t video_nack_ever_used[PEER_NACK_EVER_RING];
  uint32_t video_nack_retrans_returned;
  uint32_t video_nack_giveups;
  uint32_t video_nack_table_clears;
  // 0011: the wall clock of the moment the transport became usable.  It is
  //   the anchor of the PLI timer: the first "please send a key frame" goes
  //   out LIBPEER_PLI_FIRST_DELAY_MS after it.
  uint32_t video_pli_anchor_ms;
  // 0011: the wall clock of the last PLI state log line.
  uint32_t video_pli_diag_ms;
  // 0018: forced_sequence_skips snapshot for the burst detector (修改3) --
  // the same delta pattern as remb_prev_forced_skips.
  uint32_t pli_prev_forced_skips;
  RtcpReceiverStats video_receiver_stats;
  TwccReceiver twcc;
  uint32_t twcc_send_failures;

  int dtls_handshake_attempts;
  uint32_t dtls_handshake_started_ms;
  // 0035: the verdict of the last handshake attempt.  It used to be a local
  // variable of peer_connection_loop() and was lost with the stack frame.
  int dtls_last_handshake_ret;
  int dtls_handshake_ret_valid;
  // The offer's a=setup: value ("actpass" / "active" / "passive") and the
  // fingerprint the offer really carries -- both for diagnostics only, the
  // role and the comparison keep using the values below.
  char dtls_setup_attr[16];
  char dtls_offer_fingerprint_alg[16];
  char dtls_offer_fingerprint[DTLS_SRTP_FINGERPRINT_LENGTH];
  int sctp_create_attempted;
  /* 0041: what happened when the SCTP association was created. */
  int sctp_create_attempts;
  int sctp_create_ret;
  int sctp_create_fail_reason;
  int completed_udp_packets;
  int completed_rtcp_packets;
  int completed_dtls_packets;
  int completed_rtp_packets;
  int completed_unknown_packets;
  int rtp_decrypt_failures;
  int rtcp_decrypt_failures;
  int sctp_read_events;
  // 0037: the ICE "checking" watchdog.
  //
  // The CHECKING state had NO timeout at all: when the candidate pair table
  // was empty the loop just `break`ed and the peer connection stayed in
  // "checking" forever -- the "48 seconds without any reaction".
  uint32_t ice_checking_started_ms;  // wall clock of the start of CHECKING
  uint32_t ice_checking_diag_ms;     // wall clock of the last CHECKING log
  // 0037: the answer has to be on the wire before the first connectivity
  // check goes out.
  //
  // The check used to leave one frame after create_answer(), i.e. BEFORE
  // the answer had been sent to the cloud.  The cloud had not processed our
  // answer yet, so it did not know our ICE credentials and DROPPED the
  // check; the next one went out 100 frames later (~3.3 s at 30 fps).  That
  // is the "sometimes it connects after 3 seconds" number.
  int answer_sent;          // 1 = the app reported that the answer went out
  uint32_t answer_created_ms; // wall clock of create_answer()
  // 0040/F3: the wall clock of the moment the app reported that the answer
  // went out.  It is the ANCHOR of iceFirstCheckMs: the cloud drops a
  // connectivity check that arrives before it has processed our answer, so
  // "how long after the answer did the first check leave" is the direct
  // measure of the 0037 race fix.  0 = the report never arrived.
  uint32_t answer_sent_ms;
  // 0040/R2: the REMB bookkeeping (AIMD; see the constants block above).
  //
  //   remb_declared_bps   what we last declared (0 = nothing sent yet)
  //   remb_bitrate_bps    the same value, kept for the diagnostics struct
  //   remb_prev_*         the stress counters as of the PREVIOUS window --
  //                       the window delta is what feeds the AIMD decision
  //   remb_stress_total   cumulative stress events (for the timeline line)
  //   remb_last_measured_bps / remb_state_diag_ms feed the remb_state line
  uint64_t video_bytes_since_remb;
  uint32_t video_last_remb_ms;
  uint32_t remb_sent;
  uint32_t remb_bitrate_bps;
  uint32_t remb_declared_bps;
  uint32_t remb_prev_seq_gaps;
  uint32_t remb_prev_late_dropped;
  uint32_t remb_prev_forced_skips;
  uint32_t remb_prev_au_dropped;
  uint32_t remb_prev_decrypt_failures;
  uint32_t remb_prev_packets;
  // 0018: the previous measurement window, kept for the 1~2 s sliding
  // span of the receive-rate estimator (修改4).
  uint64_t remb_prev_window_bytes;
  uint32_t remb_prev_window_ms;
  // R3/T3: how many REMB windows have closed since the transport came
  // up.  It is the warmup clock of the AIMD (see
  // peer_connection_maybe_send_remb): the first PEER_REMB_WARMUP_WINDOWS
  // windows are additive-only.
  uint32_t remb_window_count;
  uint32_t remb_stress_total;
  uint32_t remb_last_measured_bps;
  uint32_t remb_state_diag_ms;
  // 0012: the RTP classification counters.  "Does the cloud send audio
  // at all?" is a question the video-only path could never answer -- the
  // audio decoder got no packets, so no counter anywhere ever moved.
  //
  //   rtp_packets_total      every RTP packet that reached classification
  //                          (== audio + video + unmatched)
  //   rtp_audio_packets      matched the audio stream (SSRC or audio PT)
  //   rtp_video_packets      matched the video stream
  //   rtp_unmatched_packets  matched NEITHER -- the silent drops
  //   rtp_learned_audio_ssrc the SSRC the PT-sniffing learned (0 = it
  //                          never fired; remote_assrc may still have
  //                          come from the SDP)
  //   rtp_class_last_log_ms  wall clock of the last classification line
  uint32_t rtp_packets_total;
  uint32_t rtp_audio_packets;
  uint32_t rtp_video_packets;
  uint32_t rtp_unmatched_packets;
  uint32_t rtp_learned_audio_ssrc;
  uint32_t rtp_class_last_log_ms;
  // 0013: the heal loop bookkeeping (peer_connection_account_heal_keyframe
  // is the writer, peer_connection_log_heal_state the reader).
  //   heal_seen_keyframes  keyframes_received as of the last poll;
  //   heal_pli_answered    key frames that landed inside the answer
  //                        window of a PLI;
  //   heal_seen_delivered  keyframes_delivered as of the last poll;
  //   heal_healed_ms       wall clock of the last key frame that
  //                        survived into a delivered access unit;
  //   heal_diag_ms         the 2 s line timer.
  uint32_t heal_seen_keyframes;
  uint32_t heal_pli_answered;
  uint32_t heal_seen_delivered;
  uint32_t heal_healed_ms;
  uint32_t heal_diag_ms;
};

static int peer_connection_send_rtcp_nack(PeerConnection* pc, uint32_t ssrc,
                                          uint16_t packet_id, uint16_t bitmask);

// 0011: the PLI sender lives further down (next to the NACK sender) -- it is
// part of libpeer's public surface (it is what
// peer_connection_request_video_keyframe() calls).
int peer_connection_send_rtcp_pil(PeerConnection* pc, uint32_t ssrc);

static atomic_int peer_connection_diagnostics_enabled;
typedef void (*PeerDiagnosticCallback)(const char*);
static _Atomic(PeerDiagnosticCallback) peer_diagnostic_callback;

void peer_connection_set_diagnostic_callback(void (*callback)(const char*)) {
  atomic_store(&peer_diagnostic_callback, callback);
  sctp_set_diagnostic_callback(callback);
}

void peer_connection_set_diagnostics_enabled(int enabled) {
  peer_connection_diagnostics_enabled = enabled != 0;
  sctp_set_diagnostics_enabled(enabled);
}

static void peer_connection_diag_log(const char* fmt, ...) {
  if (!peer_connection_diagnostics_enabled)
    return;
  PeerDiagnosticCallback callback = atomic_load(&peer_diagnostic_callback);
  if (!callback)
    return;
  const int saved_errno = errno;
  char message[2048] = "LIBPEER ";
  va_list args;
  va_start(args, fmt);
  vsnprintf(message + 8, sizeof(message) - 8, fmt, args);
  va_end(args);
  callback(message);
  errno = saved_errno;
}

static int peer_connection_video_packet_buffered(const RtpDecoder* decoder,
                                                 uint16_t sequence) {
  const size_t slot = sequence % RTP_REORDER_WINDOW;
  return decoder->reorder_used[slot] && decoder->reorder_sequences[slot] == sequence;
}

static void peer_connection_datachannel_message(char* msg, size_t len,
                                               void* userdata, uint16_t sid) {
  PeerConnection* pc = userdata;
  if (pc->ondatachannelmessage)
    pc->ondatachannelmessage(msg, len, pc->config.user_data, sid);
}

static void peer_connection_datachannel_open(void* userdata) {
  PeerConnection* pc = userdata;
  if (pc->ondatachannelopen)
    pc->ondatachannelopen(pc->config.user_data);
}

/* 0014: the peer's DataChannel opened -- the sid and the label are known
 *   here and nowhere earlier: the association-up callback
 *   (peer_connection_datachannel_open) fires before the peer's DCEP OPEN
 *   and therefore has no sid to hand out. */
static void peer_connection_datachannel_opened(uint16_t sid, const char* label, void* userdata) {
  PeerConnection* pc = userdata;
  if (pc->ondatachannelopened)
    pc->ondatachannelopened(sid, label, pc->config.user_data);
}

static void peer_connection_datachannel_close(void* userdata) {
  PeerConnection* pc = userdata;
  if (pc->ondatachannelclose)
    pc->ondatachannelclose(pc->config.user_data);
  if (pc->config.datachannel &&
      (pc->state == PEER_CONNECTION_CONNECTED || pc->state == PEER_CONNECTION_COMPLETED)) {
    peer_connection_diag_log("sctp_association_closed");
    STATE_CHANGED(pc, PEER_CONNECTION_FAILED);
  }
}

static uint32_t peer_connection_nack_packet_count(uint16_t bitmask) {
  uint32_t count = 1;
  while (bitmask != 0) {
    count += bitmask & 1u;
    bitmask >>= 1;
  }
  return count;
}

// 0011: the effective PLI interval (clamped to a sane range).
static uint32_t peer_connection_pli_interval_ms(void) {
  uint32_t interval = LIBPEER_PLI_INTERVAL_MS;
  if (interval < LIBPEER_PLI_INTERVAL_MS_MIN)
    interval = LIBPEER_PLI_INTERVAL_MS_MIN;
  if (interval > LIBPEER_PLI_INTERVAL_MS_MAX)
    interval = LIBPEER_PLI_INTERVAL_MS_MAX;
  return interval;
}

// 0011: the delay before the FIRST PLI (clamped).
static uint32_t peer_connection_pli_first_delay_ms(void) {
  uint32_t delay = LIBPEER_PLI_FIRST_DELAY_MS;
  if (delay > LIBPEER_PLI_FIRST_DELAY_MS_MAX)
    delay = LIBPEER_PLI_FIRST_DELAY_MS_MAX;
  return delay;
}

// 0011/B2: the key frame staleness window (clamped).
static uint32_t peer_connection_pli_stale_ms(void) {
  uint32_t stale = LIBPEER_PLI_STALE_MS;
  if (stale < LIBPEER_PLI_STALE_MS_MIN)
    stale = LIBPEER_PLI_STALE_MS_MIN;
  if (stale > LIBPEER_PLI_STALE_MS_MAX)
    stale = LIBPEER_PLI_STALE_MS_MAX;
  return stale;
}

// 0011: send ONE PLI and count it.  Returns 1 when a PLI really went out.
//
//   The counters live in the video decoder next to the key frame counter:
//   both are read by peer_connection_get_video_rtp_stats(), and both are
//   reset together with the decoder (a new video SSRC means a new stream
//   and therefore a new key frame history).
static int peer_connection_send_pli_counted(PeerConnection* pc, uint32_t ssrc) {
  if (!pc || ssrc == 0)
    return 0;
  if (peer_connection_send_rtcp_pil(pc, ssrc) < 0)
    return 0;
  pc->vrtp_decoder.pli_sent++;
  // 0018: the same event under the new all-paths counter (修改3).
  pc->vrtp_decoder.pli_sent_total++;
  pc->vrtp_decoder.pli_last_sent_ms = ports_get_epoch_time();
  peer_connection_diag_log("pli_sent count=%u total=%u fromForced=%u suppressedStale=%u mediaSsrc=%u keyframes=%u",
                           pc->vrtp_decoder.pli_sent,
                           pc->vrtp_decoder.pli_sent_total,
                           pc->vrtp_decoder.pli_from_forced_skips,
                           pc->vrtp_decoder.pli_suppressed_stale,
                           ssrc,
                           pc->vrtp_decoder.keyframes_received);
  return 1;
}

// 0018: a PLI that BYPASSES the "the last key frame is fresh" (STALE) gate
// but keeps the 300 ms interval gate (修改3).  Used when the reorder layer
// itself is bleeding sequence numbers: a fresh key frame that keeps losing
// packets is not a healthy picture, so asking again is right.  Do NOT route
// this through peer_connection_maybe_send_pli() -- its STALE gate would
// press the request back.
static int peer_connection_send_pli_bypass_stale(PeerConnection* pc) {
  if (!pc || pc->state != PEER_CONNECTION_COMPLETED)
    return 0;
  const uint32_t ssrc = pc->remote_vssrc;
  if (ssrc == 0)
    return 0;
  const uint32_t now_ms = ports_get_epoch_time();
  if (pc->vrtp_decoder.pli_last_sent_ms != 0 &&
      (uint32_t)(now_ms - pc->vrtp_decoder.pli_last_sent_ms) < peer_connection_pli_interval_ms())
    return 0;
  return peer_connection_send_pli_counted(pc, ssrc);
}

// 0011/B2: ask the cloud for a key frame -- and KEEP asking while the
// picture is stale.
//
//   The rules (every one of them is observable in the device report):
//     · only on a completed transport (the RTCP goes out over SRTCP);
//     · the media SSRC has to be known: it comes from the remote description
//       (the cloud's offer carries "a=ssrc:<id>") and, as a fallback, from
//       the first video RTP packet;
//     · not before the first delay (the cloud pushes an IDR when the stream
//       starts, so the very first request would only race with it);
//     · ★ B2: not while the last key frame is FRESH -- a key frame that
//       arrived inside LIBPEER_PLI_STALE_MS made the picture whole, and
//       asking for another one would only waste bandwidth.  The FIRST
//       version of this rule was "stop after the first key frame"
//       ("the official client stops asking as well") -- a premise that
//       does not hold for us: the official client does not lose packets
//       and never needs to ask again, while we lose packets on every
//       capture and the cloud inserts no periodic IDR, so after the
//       first key frame every loss corrupted the picture forever;
//     · not more often than the interval.
static void peer_connection_maybe_send_pli(PeerConnection* pc) {
  if (!pc || pc->state != PEER_CONNECTION_COMPLETED)
    return;
  RtpDecoder* decoder = &pc->vrtp_decoder;
  const uint32_t ssrc = pc->remote_vssrc;
  if (ssrc == 0)
    return;

  const uint32_t now_ms = ports_get_epoch_time();
  const uint32_t anchor_ms = pc->video_pli_anchor_ms ? pc->video_pli_anchor_ms : now_ms;
  if ((uint32_t)(now_ms - anchor_ms) < peer_connection_pli_first_delay_ms())
    return;
  if (decoder->pli_last_sent_ms != 0 &&
      (uint32_t)(now_ms - decoder->pli_last_sent_ms) < peer_connection_pli_interval_ms())
    return;
  // B2: a FRESH key frame suppresses the request; a stale one (or none at
  // all -- last_keyframe_ms == 0) lets it through.  This replaces the
  // old "stop after the first key frame" termination.
  // 0018: the interval gate moved ahead of this one so that
  // pli_suppressed_stale counts exactly the requests the STALE gate alone
  // held back (修改3).
  if (decoder->last_keyframe_ms != 0 &&
      (uint32_t)(now_ms - decoder->last_keyframe_ms) < peer_connection_pli_stale_ms()) {
    decoder->pli_suppressed_stale++;
    return;
  }

  peer_connection_send_pli_counted(pc, ssrc);
}

// 0018: forced_sequence_skips burst detector (修改3).  Called right after
// rtp_decoder_poll() -- the same delta pattern as the REMB estimator's
// remb_prev_forced_skips.  A burst of forced skips means the reorder layer
// is bleeding sequence numbers; a key frame is the cheapest repair, so one
// is requested through the STALE-bypass (but still at the 300 ms interval).
#define PEER_PLI_FORCED_SKIP_BURST 8

static void peer_connection_pli_on_forced_skips(PeerConnection* pc) {
  if (!pc)
    return;
  RtpDecoder* decoder = &pc->vrtp_decoder;
  const uint32_t forced_now = decoder->forced_sequence_skips;
  const uint32_t forced_prev = pc->pli_prev_forced_skips;
  pc->pli_prev_forced_skips = forced_now;
  const uint32_t forced_ev = forced_now - forced_prev;
  if (forced_ev <= PEER_PLI_FORCED_SKIP_BURST)
    return;
  if (peer_connection_send_pli_bypass_stale(pc)) {
    decoder->pli_from_forced_skips++;
    peer_connection_diag_log("[pli-link] forced-skip burst=%u total=%u",
                             forced_ev, forced_now);
  }
}

// 0011: the PLI state in the diagnostic log (every PEER_PLI_DIAG_INTERVAL_MS).
//
//   This is the line that answers "did we ask for a key frame, and did one
//   arrive?" on the device -- the final counters alone cannot show WHEN.
static void peer_connection_log_pli_state(PeerConnection* pc) {
  if (!pc)
    return;
  const uint32_t now_ms = ports_get_epoch_time();
  if (pc->video_pli_diag_ms != 0 &&
      (uint32_t)(now_ms - pc->video_pli_diag_ms) < PEER_PLI_DIAG_INTERVAL_MS)
    return;
  pc->video_pli_diag_ms = now_ms;
  const uint32_t keyframe_ago = pc->vrtp_decoder.last_keyframe_ms
      ? (uint32_t)(now_ms - pc->vrtp_decoder.last_keyframe_ms) : 0;
  const uint32_t pli_ago = pc->vrtp_decoder.pli_last_sent_ms
      ? (uint32_t)(now_ms - pc->vrtp_decoder.pli_last_sent_ms) : 0;
  peer_connection_diag_log(
      "pli_state sent=%u keyframes=%u mediaSsrc=%u lastKeyframeAgoMs=%u lastPliAgoMs=%u",
      pc->vrtp_decoder.pli_sent,
      pc->vrtp_decoder.keyframes_received,
      pc->remote_vssrc,
      keyframe_ago,
      pli_ago);
}

// 0012: the RTP classification in the diagnostic log.  This is the line
// that answers "does the cloud send audio at all?" on the device:
//   audio=0 while total grows  -> the sender is not sending audio;
//   audio>0 with no sound      -> the loss is on OUR side of the decoder.
static void peer_connection_log_rtp_class(PeerConnection* pc) {
  if (!pc)
    return;
  const uint32_t now_ms = ports_get_epoch_time();
  if (pc->rtp_class_last_log_ms != 0 &&
      (uint32_t)(now_ms - pc->rtp_class_last_log_ms) < PEER_RTP_CLASS_DIAG_INTERVAL_MS)
    return;
  pc->rtp_class_last_log_ms = now_ms;
  peer_connection_diag_log("udp_class=rtp audio=%u video=%u unmatched=%u total=%u audioSsrc=%u",
                           pc->rtp_audio_packets,
                           pc->rtp_video_packets,
                           pc->rtp_unmatched_packets,
                           pc->rtp_packets_total,
                           pc->rtp_learned_audio_ssrc);
  // 0012/B (task B): the audio chain one step deeper than the counters
  // above -- received (classified) vs parsed (payload extracted by
  // rtp_parse_payload) vs produced (handed to the audio callback).
  //   recv > parsed  == parse failures (failShort / failShape say which);
  //   parsed > produced == no audio callback registered;
  //   redPt > 0 with opusPt == 0 == the sender speaks RED only, and
  //   nobody unwraps it on this side (see the patch prose).
  peer_connection_diag_log("udp_class=rtp audioChain recv=%u parsed=%u produced=%u failShort=%u failShape=%u redPt=%u opusPt=%u",
                           pc->rtp_audio_packets,
                           pc->artp_decoder.audio_packets_parsed,
                           pc->artp_decoder.audio_units_produced,
                           pc->artp_decoder.audio_fail_short,
                           pc->artp_decoder.audio_fail_shape,
                           pc->artp_decoder.audio_red_packets,
                           pc->artp_decoder.audio_opus_packets);
}

// 0013: how long after a PLI a key frame may land and still count as
// "the PLI was answered".  Attribution is by time window -- an RTCP PLI
// carries no packet identity, so this is the strongest attribution the
// protocol allows.  3 s covers the B2 stale window plus one interval.
#define PEER_HEAL_ANSWER_WINDOW_MS 3000

// 0013: how often the heal loop line is written.
#define PEER_HEAL_DIAG_INTERVAL_MS 2000

// 0013 (task C): one step of the heal accounting, called every loop
// tick.  Kept flat with plain locals ON PURPOSE: the self test extracts
// these exact statements and executes them.
//   A key frame that arrives while a PLI is "open" (sent at most
//   PEER_HEAL_ANSWER_WINDOW_MS ago) counts as answered.
static void peer_connection_account_heal_keyframe(PeerConnection* pc, uint32_t now_ms) {
  const uint32_t kf_now = pc->vrtp_decoder.keyframes_received;
  const uint32_t kf_prev = pc->heal_seen_keyframes;
  const uint32_t pli_ms = pc->vrtp_decoder.pli_last_sent_ms;
  const int fresh_pli = pli_ms != 0 && now_ms - pli_ms <= PEER_HEAL_ANSWER_WINDOW_MS;
  if (kf_now != kf_prev) {
    if (fresh_pli) {
      pc->heal_pli_answered++;
    }
    pc->heal_seen_keyframes = kf_now;
  }
  const uint32_t kfd_now = pc->vrtp_decoder.keyframes_delivered;
  const uint32_t kfd_prev = pc->heal_seen_delivered;
  if (kfd_now != kfd_prev) {
    pc->heal_healed_ms = now_ms;
    pc->heal_seen_delivered = kfd_now;
  }
}

// 0013 (task C): the heal loop line.  The three channels that can repair
// a corrupted picture, one line, every 2 s:
//   pli    engine-side RTCP PLI (the B2 contract): pliSent total,
//          pliAnswered = key frames that landed inside the answer
//          window of a PLI.  pliSent growing with pliAnswered stuck at
//          0 is the "the cloud does not answer RTCP PLI" verdict
//          (the official browser session may never use RTCP at all --
//          then this channel is structurally dead and 20032 is the
//          only live request path);
//   idr    idrSeen counted at NAL assembly vs idrDelivered survived
//          into a completed access unit; the gap is OUR loss, not the
//          cloud's;
//   nack   nackReq / nackPkts = what we asked to retransmit;
//          recovered = out-of-order packets that filled their slot
//          (the retransmission landing, approximately);
//   au     auOk / auDrop = whether production resumed after the key
//          frame ("the picture is whole again": healedAgoMs small).
// The 20032 signaling channel is app-side; correlate its send times
// against idrDelivered growth in the same log.
static void peer_connection_log_heal_state(PeerConnection* pc) {
  if (!pc) {
    return;
  }
  const uint32_t now_ms = ports_get_epoch_time();
  if (pc->heal_diag_ms != 0 &&
      (uint32_t)(now_ms - pc->heal_diag_ms) < PEER_HEAL_DIAG_INTERVAL_MS) {
    return;
  }
  pc->heal_diag_ms = now_ms;
  const uint32_t healed_ago = pc->heal_healed_ms
                                  ? (uint32_t)(now_ms - pc->heal_healed_ms)
                                  : 0;
  peer_connection_diag_log("heal_state pliSent=%u pliAnswered=%u idrSeen=%u idrDelivered=%u nackReq=%u nackPkts=%u recovered=%u auOk=%u auDrop=%u healedAgoMs=%u",
                           pc->vrtp_decoder.pli_sent,
                           pc->heal_pli_answered,
                           pc->vrtp_decoder.keyframes_received,
                           pc->vrtp_decoder.keyframes_delivered,
                           pc->video_nack_requests,
                           pc->video_nack_packets_requested,
                           pc->vrtp_decoder.reordered_packets,
                           pc->vrtp_decoder.access_units_completed,
                           pc->vrtp_decoder.access_units_dropped,
                           healed_ago);
}

// 0018: NACK chase ledger helpers (修改2).  Index is
// sequence % PEER_NACK_CHASE_MAX: inside one reorder window the live gap
// sequences never collide, and a colliding leftover (a gap the decoder force-
// skipped long ago) is either stale enough to evict or the trigger for the
// clear-everything overflow valve.
static PeerNackChase* peer_nack_chase_find(PeerConnection* pc, uint16_t sequence) {
  PeerNackChase* entry = &pc->video_nack_chase[sequence % PEER_NACK_CHASE_MAX];
  return (entry->used && entry->sequence == sequence) ? entry : NULL;
}

static void peer_nack_chase_release(PeerConnection* pc, PeerNackChase* entry) {
  const uint16_t sequence = entry->sequence;
  entry->used = 0;
  entry->retries = 0;
  // The chase is over one way or another: the decoder must stop holding the
  // hole open for us, or expire_gap() would never give it up.
  RtpGapHole* hole = rtp_decoder_hole_find(&pc->vrtp_decoder, sequence);
  if (hole)
    hole->nack_in_flight = 0;
}

static void peer_nack_chase_clear_all(PeerConnection* pc) {
  for (int i = 0; i < PEER_NACK_CHASE_MAX; i++) {
    if (pc->video_nack_chase[i].used)
      peer_nack_chase_release(pc, &pc->video_nack_chase[i]);
  }
}

static PeerNackChase* peer_nack_chase_touch(PeerConnection* pc,
                                            uint16_t sequence,
                                            uint32_t now_ms,
                                            int* overflowed) {
  PeerNackChase* entry = &pc->video_nack_chase[sequence % PEER_NACK_CHASE_MAX];
  *overflowed = 0;
  if (entry->used && entry->sequence != sequence) {
    const uint32_t age_ms = (uint32_t)(now_ms - entry->created_ms);
    if (entry->retries <= PEER_NACK_MAX_RETRIES && age_ms <= PEER_NACK_MAX_AGE_MS) {
      *overflowed = 1;  // live collision: the table is in trouble
      return NULL;
    }
    peer_nack_chase_release(pc, entry);  // stale leftover: evict quietly
  }
  if (entry->used && entry->sequence == sequence)
    return entry;
  entry->sequence = sequence;
  entry->used = 1;
  entry->retries = 0;
  entry->created_ms = now_ms;
  entry->last_retry_ms = 0;
  // The hole is being chased from now on: the decoder holds it open.
  RtpGapHole* hole = rtp_decoder_hole_find(&pc->vrtp_decoder, sequence);
  if (hole)
    hole->nack_in_flight = 1;
  return entry;
}

// 0018: the retry clock: exponential BACKOFF (越试越慢).  The first three
// retries are 100 ms x 0.90 / 0.94 / 0.98 (90 / 94 / 98 ms), from the
// fourth on the previous interval x 1.5, clamped into [5, 300] ms overall.
// (纠偏: the first cut had pow(0.9x, retries) -- a SHRINKING interval that
// would turn the chase into a NACK storm, exactly the 1142-per-27s failure
// mode this replaces.)  No libm needed.  retries = how many NACKs are
// already out (>= 1).
static uint32_t peer_nack_retry_interval_ms(uint8_t retries) {
  double interval;
  if (retries <= 1)
    interval = 90.0;  /* retry 0: 100 x 0.90 */
  else if (retries == 2)
    interval = 94.0;  /* retry 1: 100 x 0.94 */
  else if (retries == 3)
    interval = 98.0;  /* retry 2: 100 x 0.98 */
  else {
    interval = 98.0;
    for (uint8_t i = 4; i <= retries; i++)
      interval *= 1.5;
  }
  if (interval < 5.0)
    interval = 5.0;
  if (interval > 300.0)
    interval = 300.0;
  return (uint32_t)(interval + 0.5);
}

static void peer_nack_chase_mark_sent(PeerConnection* pc, uint16_t sequence,
                                      uint32_t now_ms) {
  PeerNackChase* entry = peer_nack_chase_find(pc, sequence);
  if (entry) {
    entry->retries++;
    entry->last_retry_ms = now_ms;
    RtpGapHole* hole = rtp_decoder_hole_find(&pc->vrtp_decoder, sequence);
    if (hole) {
      hole->nack_in_flight = 1;
      hole->retries = entry->retries;
    }
  }
  const size_t slot = sequence % PEER_NACK_EVER_RING;
  pc->video_nack_ever_sequences[slot] = sequence;
  pc->video_nack_ever_used[slot] = 1;
}

// 0018: a video packet just landed.  If its sequence number was ever NACKed
// it is a returned retransmission -- count it and close the chase entry.
static void peer_connection_nack_on_arrival(PeerConnection* pc, uint16_t sequence) {
  const size_t slot = sequence % PEER_NACK_EVER_RING;
  const int ever_nacked = pc->video_nack_ever_used[slot] &&
                          pc->video_nack_ever_sequences[slot] == sequence;
  PeerNackChase* entry = peer_nack_chase_find(pc, sequence);
  if (ever_nacked || entry) {
    pc->video_nack_retrans_returned++;
    if (ever_nacked)
      pc->video_nack_ever_used[slot] = 0;
    if (entry)
      peer_nack_chase_release(pc, entry);
  }
}

static void peer_connection_maybe_send_video_nacks(PeerConnection* pc,
                                                   uint16_t newest_sequence) {
  RtpDecoder* decoder = &pc->vrtp_decoder;
  if (!decoder->reorder_has_expected_sequence || pc->remote_vssrc == 0)
    return;

  const uint16_t expected = decoder->reorder_expected_sequence;
  int16_t distance = (int16_t)(newest_sequence - expected);
  if (distance <= 0)
    return;
  if (distance > RTP_REORDER_MAX_HOLD_PACKETS)
    distance = RTP_REORDER_MAX_HOLD_PACKETS;

  const uint32_t now_ms = ports_get_epoch_time();

  // 0018: pass 1 -- per-gap chase bookkeeping.  Every gap gets its own entry
  // and its own retry clock (first request 5 ms after the gap is seen, then
  // the exponential-backoff clock above -- 越试越慢), and is given up -- with
  // its hole handed back to the decoder -- after 18 retries or 1500 ms.
  // due[] collects the gaps whose clock says "ask again now".
  uint8_t due[RTP_REORDER_MAX_HOLD_PACKETS];
  for (int16_t i = 0; i < distance; i++)
    due[i] = 0;

  for (int16_t offset = 0; offset < distance; offset++) {
    const uint16_t sequence = (uint16_t)(expected + offset);
    if (peer_connection_video_packet_buffered(decoder, sequence))
      continue;

    int overflowed = 0;
    PeerNackChase* chase = peer_nack_chase_touch(pc, sequence, now_ms, &overflowed);
    if (overflowed) {
      // The table overflowed with LIVE entries.  Clear it and ask for a key
      // frame instead of growing without bound -- through the STALE bypass:
      // an overflow means the picture is broken regardless of key frame age.
      peer_nack_chase_clear_all(pc);
      pc->video_nack_table_clears++;
      peer_connection_diag_log("[nack-hold] table-full cleared seq=%u", sequence);
      peer_connection_send_pli_bypass_stale(pc);
      return;
    }
    if (!chase)
      continue;

    const uint32_t age_ms = (uint32_t)(now_ms - chase->created_ms);
    if (chase->retries > PEER_NACK_MAX_RETRIES || age_ms > PEER_NACK_MAX_AGE_MS) {
      pc->video_nack_giveups++;
      peer_connection_diag_log("[nack-hold] giveup seq=%u retries=%u ageMs=%u",
                               sequence, (unsigned)chase->retries, age_ms);
      peer_nack_chase_release(pc, chase);
      continue;
    }

    if (chase->retries == 0) {
      if (age_ms >= PEER_NACK_FIRST_DELAY_MS)
        due[offset] = 1;
    } else if ((uint32_t)(now_ms - chase->last_retry_ms) >=
               peer_nack_retry_interval_ms(chase->retries)) {
      due[offset] = 1;
    }
  }

  // Pass 2 -- pack the due gaps into RTCP NACK runs and send them.
  int offset = 0;
  while (offset < distance) {
    while (offset < distance && !due[offset])
      offset++;
    if (offset >= distance)
      break;

    const uint16_t packet_id = (uint16_t)(expected + offset);
    uint16_t bitmask = 0;
    for (int bit = 0; bit < 16 && offset + bit + 1 < distance; ++bit) {
      if (due[offset + bit + 1])
        bitmask |= (uint16_t)(1u << bit);
    }

    if (peer_connection_send_rtcp_nack(
          pc, pc->remote_vssrc, packet_id, bitmask) >= 0) {
      pc->video_nack_requests++;
      pc->video_nack_packets_requested += peer_connection_nack_packet_count(bitmask);
      peer_nack_chase_mark_sent(pc, packet_id, now_ms);
      for (int bit = 0; bit < 16; ++bit) {
        if (bitmask & (uint16_t)(1u << bit))
          peer_nack_chase_mark_sent(pc, (uint16_t)(packet_id + bit + 1), now_ms);
      }
    }
    offset += 17;
  }
}

static void peer_connection_hex_preview(const uint8_t* data, int len, char* out, size_t out_len) {
  if (!out || out_len == 0) {
    return;
  }
  out[0] = '\0';
  if (!data || len <= 0) {
    return;
  }

  int count = len < 12 ? len : 12;
  size_t pos = 0;
  for (int i = 0; i < count && pos + 4 < out_len; ++i) {
    pos += snprintf(out + pos, out_len - pos, "%s%02x", i == 0 ? "" : " ", data[i]);
  }
  if (len > count && pos + 5 < out_len) {
    snprintf(out + pos, out_len - pos, " ...");
  }
}

static const char* peer_connection_ice_state_name(IceCandidateState state) {
  switch (state) {
    case ICE_CANDIDATE_STATE_FROZEN:
      return "frozen";
    case ICE_CANDIDATE_STATE_WAITING:
      return "waiting";
    case ICE_CANDIDATE_STATE_INPROGRESS:
      return "inprogress";
    case ICE_CANDIDATE_STATE_SUCCEEDED:
      return "succeeded";
    case ICE_CANDIDATE_STATE_FAILED:
      return "failed";
    default:
      return "unknown";
  }
}

static const char* peer_connection_ice_type_name(IceCandidateType type) {
  switch (type) {
    case ICE_CANDIDATE_TYPE_HOST:
      return "host";
    case ICE_CANDIDATE_TYPE_SRFLX:
      return "srflx";
    case ICE_CANDIDATE_TYPE_PRFLX:
      return "prflx";
    case ICE_CANDIDATE_TYPE_RELAY:
      return "relay";
    default:
      return "unknown";
  }
}

// 0037: why did ICE never finish?
//
// The three cases the next device run has to tell apart:
//   no_candidate_pairs     the pair table is empty -- not one check was sent
//   pair_never_nominated   there are pairs, but none was ever nominated
//   no_binding_response    we sent checks, the peer never answered
//   response_not_selected  the peer DID answer, but ICE still did not
//                          finish (the libpeer SUCCEEDED-pair defect)
static const char* peer_connection_ice_checking_reason(PeerConnection* pc) {
  if (pc->agent.candidate_pairs_num == 0) {
    return "no_candidate_pairs";
  }
  if (!pc->agent.nominated_pair) {
    return "pair_never_nominated";
  }
  if (pc->agent.binding_responses_received == 0) {
    return "no_binding_response";
  }
  return "response_not_selected";
}

// 0040/F3: how long after the answer went out did the FIRST connectivity
// check leave?
//
// The anchor is the moment the app reported that the answer went out
// (peer_connection_set_answer_sent) -- that is the moment the cloud could
// first have known our ICE credentials, so it is the only meaningful
// zero point.  When that report never arrives the fallback in
// peer_connection_loop() releases the gate and the anchor becomes the
// moment the answer was CREATED.
//
// Returns -1 when no check has gone out yet.
//
// ★ The criterion really moves: before the 0037 fix this was ~3.3 s (100
//   frames at 30 fps); after it, a few tens of milliseconds.
static int peer_connection_ice_first_check_ms(PeerConnection* pc) {
  if (!pc || pc->agent.first_conncheck_ms == 0) {
    return -1;
  }
  const uint32_t anchor = pc->answer_sent_ms ? pc->answer_sent_ms : pc->answer_created_ms;
  if (anchor == 0) {
    return -1;
  }
  return (int)(pc->agent.first_conncheck_ms - anchor);
}

static void peer_connection_log_candidate_pairs(PeerConnection* pc, const char* reason) {
  if (!pc) {
    return;
  }

  peer_connection_diag_log("ice_pairs reason=%s total=%d local=%d remote=%d mode=%d iceLite=%d",
                           reason ? reason : "unknown",
                           pc->agent.candidate_pairs_num,
                           pc->agent.local_candidates_count,
                           pc->agent.remote_candidates_count,
                           pc->agent.mode,
                           pc->remote_ice_lite);

  for (int i = 0; i < pc->agent.candidate_pairs_num && i < 12; ++i) {
    IceCandidatePair* pair = &pc->agent.candidate_pairs[i];
    char local[ADDRSTRLEN] = {0};
    char remote[ADDRSTRLEN] = {0};
    if (pair->local) {
      addr_to_string(&pair->local->addr, local, sizeof(local));
    }
    if (pair->remote) {
      addr_to_string(&pair->remote->addr, remote, sizeof(remote));
    }
    peer_connection_diag_log("ice_pair[%d] state=%s conncheck=%d selected=%d nominated=%d local=%s/%s:%u remote=%s/%s:%u priority=%" PRIu64,
                             i,
                             peer_connection_ice_state_name(pair->state),
                             pair->conncheck,
                             pair == pc->agent.selected_pair,
                             pair == pc->agent.nominated_pair,
                             pair->local
                                 ? peer_connection_ice_type_name(pair->local->type)
                                 : "none",
                             local,
                             pair->local ? pair->local->addr.port : 0,
                             pair->remote
                                 ? peer_connection_ice_type_name(pair->remote->type)
                                 : "none",
                             remote,
                             pair->remote ? pair->remote->addr.port : 0,
                             pair->priority);
  }
}

static void peer_connection_outgoing_rtp_packet(uint8_t* data, size_t size, void* user_data) {
  PeerConnection* pc = (PeerConnection*)user_data;
  dtls_srtp_encrypt_rtp_packet(&pc->dtls_srtp, data, (int*)&size);
  agent_send(&pc->agent, data, size);
}

static int peer_connection_dtls_srtp_recv(void* ctx, unsigned char* buf, size_t len) {
  DtlsSrtp* dtls_srtp = (DtlsSrtp*)ctx;
  PeerConnection* pc = (PeerConnection*)dtls_srtp->user_data;

  // 0040: this callback is the DTLS READ PATH, and it has two hard rules:
  //
  //   1) it must NOT block.  The peer connection loop runs on the render
  //      thread; a blocking read here would starve the ICE retransmission
  //      timer, the consent freshness timer and the media pump.
  //   2) it must NEVER return 0.  mbedTLS reads a 0 from the bio as "the
  //      peer closed the connection" and fails the handshake with
  //      MBEDTLS_ERR_SSL_CONN_EOF (-0x7280).  "No data right now" is
  //      MBEDTLS_ERR_SSL_WANT_READ, which is what this returns.
  //
  // The counters in dtls_srtp_bio_recv() (dtls_srtp.c) measure both rules:
  // bio_eof_returns has to stay 0 and bio_max_call_us has to stay small.
  if (pc->agent_ret > 0) {
    const int bytes = pc->agent_ret;
    pc->agent_ret = -1;
    pc->dtls_pending = 0;
    if ((size_t)bytes > len)
      return MBEDTLS_ERR_SSL_BUFFER_TOO_SMALL;
    memcpy(buf, pc->agent_buf, bytes);
    return bytes;
  }

  if (pc->state == PEER_CONNECTION_COMPLETED)
    return MBEDTLS_ERR_SSL_WANT_READ;

  const int ret = agent_recv_nonblocking(&pc->agent, buf, len);
  if (ret > 0)
    return ret;

  return MBEDTLS_ERR_SSL_WANT_READ;
}

static int peer_connection_dtls_srtp_send(void* ctx, const uint8_t* buf, size_t len) {
  DtlsSrtp* dtls_srtp = (DtlsSrtp*)ctx;
  PeerConnection* pc = (PeerConnection*)dtls_srtp->user_data;

  // 0035: remember whether the record that goes out is a handshake record
  // (ClientHello / Certificate / Finished ...) or application data.
  dtls_srtp->last_write_is_handshake = dtls_srtp->handshake_active ? 1 : 0;

  // LOGD("send %.4x %.4x, %ld", *(uint16_t*)buf, *(uint16_t*)(buf + 2), len);
  return agent_send(&pc->agent, buf, len);
}

static void peer_connection_incoming_rtcp(PeerConnection* pc, uint8_t* buf, size_t len) {
  if (!rtcp_validate_compound(buf, len))
    return;
  size_t pos = 0;

  while (pos < len) {
    RtcpPacketView view;
    if (rtcp_parse_packet(buf + pos, len - pos, &view) != 0)
      return;

    switch (view.type) {
      case RTCP_SR:
        {
          uint32_t sender_ssrc_net, ntp_seconds_net, ntp_fraction_net, rtp_timestamp_net;
          memcpy(&sender_ssrc_net, buf + pos + 4, 4);
          memcpy(&ntp_seconds_net, buf + pos + 8, 4);
          memcpy(&ntp_fraction_net, buf + pos + 12, 4);
          memcpy(&rtp_timestamp_net, buf + pos + 16, 4);
          const uint32_t sender_ssrc = ntohl(sender_ssrc_net);
          const uint32_t ntp_seconds = ntohl(ntp_seconds_net);
          const uint32_t ntp_fraction = ntohl(ntp_fraction_net);
          const uint32_t rtp_timestamp = ntohl(rtp_timestamp_net);
          const uint64_t ntp_us = (uint64_t)ntp_seconds * 1000000ULL +
                                  (((uint64_t)ntp_fraction * 1000000ULL) >> 32);
          if (sender_ssrc == pc->remote_vssrc)
            rtcp_receiver_record_sr(&pc->video_receiver_stats, &view, ports_get_monotonic_time());
          if (pc->config.onrtpsenderreport)
            pc->config.onrtpsenderreport(sender_ssrc, ntp_us, rtp_timestamp, pc->config.user_data);
        }
        break;
      case RTCP_RR:
        LOGD("RTCP_PR");
        if (view.count > 0) {
// TODO: REMB, GCC ...etc
#if 0
          RtcpRr rtcp_rr = rtcp_parse_rr(buf);
          uint32_t fraction = ntohl(rtcp_rr.report_block[0].flcnpl) >> 24;
          uint32_t total = ntohl(rtcp_rr.report_block[0].flcnpl) & 0x00FFFFFF;
          if(pc->on_receiver_packet_loss && fraction > 0) {

            pc->on_receiver_packet_loss((float)fraction/256.0, total, pc->config.user_data);
          }
#endif
        }
        break;
      case RTCP_PSFB: {
        int fmt = view.count;
        LOGD("RTCP_PSFB %d", fmt);
        // PLI and FIR
        if ((fmt == 1 || fmt == 4) && pc->config.on_request_keyframe) {
          pc->config.on_request_keyframe(pc->config.user_data);
        }
      }
      default:
        break;
    }

    pos += view.size;
  }
}

static void peer_connection_maybe_send_receiver_report(PeerConnection* pc) {
  const uint32_t now_ms = ports_get_monotonic_time();
  if (pc->state != PEER_CONNECTION_COMPLETED || pc->remote_vssrc == 0 ||
      !rtcp_receiver_report_due(&pc->video_receiver_stats, now_ms))
    return;
  uint8_t packet[128];
  int size = rtcp_get_receiver_report(packet, sizeof(packet) - 32,
      pc->vrtp_encoder.ssrc, &pc->video_receiver_stats, now_ms, "webrtc-h264");
  int success = 0;
  if (size > 0 && dtls_srtp_encrypt_rctp_packet(&pc->dtls_srtp, packet, &size) == 0)
    success = agent_send(&pc->agent, packet, size) == size;
  rtcp_receiver_report_sent(&pc->video_receiver_stats, now_ms, success);
}

// 0040: send a REMB -- "Receiver Estimated Maximum Bitrate".
//
// WHY: we are the RECEIVER.  A sender that gets no bandwidth feedback
// has to guess, and the safe guess is the lowest rate the encoder can
// produce -- which is exactly the "smeared trickle" picture.  A REMB that
// says "I can take 8 Mbit/s" is what lets the cloud raise the encoder.
//
// ★ This is a PICTURE QUALITY feature, NOT a connectivity feature.  A
//   session without REMB still shows video -- it is just encoded at the
//   starvation floor.  REMB has nothing to do with whether ICE or DTLS
//   came up, and a REMB that never goes out must never be read as "the
//   connection is broken".
//
// R2 (task A): the number in a REMB is a CAPACITY DECLARATION ("how much
//   could you send me"), never a measurement report.  The first
//   implementation sent the bit rate we had just received, which the
//   sender read as "this link can only take 13.8 kbit/s": the encoder was
//   pinned to its starvation floor, we received even less, we reported
//   even less -- a spiral that could only go down.  The second
//   implementation (B1) fixed the direction but pinned the declaration at
//   a constant 8 Mbit/s, which outruns what this build can actually
//   receive.  This version (R2) makes the declaration an AIMD estimate
//   driven by the receive-side stress counters:
//     "cloud throttles us"  = measured low, stress quiet  -> ask for more;
//     "we cannot keep up"   = stress (drops at the window's packet rate)
//                             -> back off, or the loss turns into smear.
//   Behavior in the two named scenarios:
//     cloud suddenly drops its rate: our window shows fewer bytes but no
//       new stress -> the estimate keeps climbing (additive), inviting the
//       cloud back up; if the drop was caused by real network loss, that
//       loss shows up as gaps/late -> multiplicative decrease instead;
//     we cannot keep up on the NETWORK: gaps / late drops / decrypt
//       failures grow in the window -> declared x 7/10 per window until
//       the stress clears, then additive again from the sustainable
//       point.  (R3: forced skips and dropped access units no longer
//       count here -- see the R3/T1 note in the constants block.)
static void peer_connection_maybe_send_remb(PeerConnection* pc) {
  const uint32_t now_ms = ports_get_monotonic_time();
  if (!pc || pc->state != PEER_CONNECTION_COMPLETED || pc->remote_vssrc == 0) {
    return;
  }
  if (pc->video_last_remb_ms != 0 &&
      (uint32_t)(now_ms - pc->video_last_remb_ms) < PEER_REMB_INTERVAL_MS) {
    return;
  }

  // Close the measurement window.  On the very first call there is no
  // window yet -- it counts as one interval, and with no bytes arriving
  // the declaration starts at PEER_REMB_START_BPS (staying silent would
  // read as "no feedback" and the sender would guess low again).
  RtpDecoder* decoder = &pc->vrtp_decoder;
  const uint64_t bytes = pc->video_bytes_since_remb;
  uint32_t elapsed_ms = pc->video_last_remb_ms
                            ? (uint32_t)(now_ms - pc->video_last_remb_ms)
                            : PEER_REMB_INTERVAL_MS;
  pc->video_bytes_since_remb = 0;
  pc->video_last_remb_ms = now_ms;
  if (elapsed_ms == 0) {
    elapsed_ms = PEER_REMB_INTERVAL_MS;  // paranoia: never divide by zero
  }

  // R2: the window deltas of the stress counters.  The *_now / *_prev
  // locals exist so that every expression the self test extracts is made
  // of plain identifiers (see the constants block above).
  const uint32_t gaps_now = decoder->sequence_gaps;
  const uint32_t gaps_prev = pc->remb_prev_seq_gaps;
  const uint32_t late_now = decoder->late_packets_dropped;
  const uint32_t late_prev = pc->remb_prev_late_dropped;
  const uint32_t forced_now = decoder->forced_sequence_skips;
  const uint32_t forced_prev = pc->remb_prev_forced_skips;
  const uint32_t audrop_now = decoder->access_units_dropped;
  const uint32_t audrop_prev = pc->remb_prev_au_dropped;
  const uint32_t decfail_now = (uint32_t)pc->rtp_decrypt_failures;
  const uint32_t decfail_prev = pc->remb_prev_decrypt_failures;
  const uint32_t pkts_now = decoder->packets_received;
  const uint32_t pkts_prev = pc->remb_prev_packets;
  pc->remb_prev_seq_gaps = gaps_now;
  pc->remb_prev_late_dropped = late_now;
  pc->remb_prev_forced_skips = forced_now;
  pc->remb_prev_au_dropped = audrop_now;
  pc->remb_prev_decrypt_failures = decfail_now;
  pc->remb_prev_packets = pkts_now;

  const uint32_t gap_ev = gaps_now - gaps_prev;
  const uint32_t late_ev = late_now - late_prev;
  const uint32_t forced_ev = forced_now - forced_prev;
  const uint32_t audrop_ev = audrop_now - audrop_prev;
  const uint32_t decfail_ev = decfail_now - decfail_prev;
  const uint32_t pkts = pkts_now - pkts_prev;
  // R3/T1: the five stress sources split into two families.
  //   net_stress  = sequence gaps + late drops + SRTP decrypt failures.
  //                 The network lost, delayed or corrupted packets.  Only
  //                 against THIS family does a lower declaration actually
  //                 reduce the loss, so only it drives the multiplicative
  //                 decrease.
  //   self_stress = forced sequence skips + dropped access units.  WE
  //                 failed to keep up with what already arrived.  A lower
  //                 declaration cannot stop us from dropping our own
  //                 frames (the sender is not the bottleneck here), so
  //                 counting this family into the decrease trigger would
  //                 slash the bitrate for a problem the declaration
  //                 cannot fix -- self-inflicted stress lowering the rate
  //                 treats nothing.  It stays observable: selfStress= in
  //                 the diagnostic line below, and it still counts into
  //                 stress / remb_stress_total for the timeline.
  const uint32_t net_stress = gap_ev + late_ev + decfail_ev;
  const uint32_t self_stress = forced_ev + audrop_ev;
  const uint32_t stress = net_stress + self_stress;
  pc->remb_stress_total = pc->remb_stress_total + stress;
  const int congested = net_stress > 0 &&
      (pkts == 0 || (net_stress * 100u) / pkts >= PEER_REMB_LOSS_PCT);

  // 0018: the 1~2 s sliding-window measurement (修改4).  The span is the
  // window that just closed PLUS the previous one (1 s hop, ~2 s span);
  // the declaration is derived from THIS, not from a blind climb.
  const uint64_t span_bytes = bytes + pc->remb_prev_window_bytes;
  const uint32_t span_ms = elapsed_ms + pc->remb_prev_window_ms;
  pc->remb_prev_window_bytes = bytes;
  pc->remb_prev_window_ms = elapsed_ms;
  const uint64_t measured_bps = span_ms ? (span_bytes * 8u * 1000u) / span_ms : 0;
  pc->remb_last_measured_bps = (uint32_t)measured_bps;

  // R3/T3: the warmup clock.  window_index is the ordinal of the window
  // that just closed (0-based); the counter is bumped immediately, so it
  // advances even in the windows where the decision below is overridden.
  const uint32_t window_index = pc->remb_window_count;
  pc->remb_window_count = window_index + 1u;
  // R3/T3: during warmup (the first PEER_REMB_WARMUP_WINDOWS windows) a
  // stressed window is only recorded (warmup=1 in the diagnostic line),
  // never acted on: stress that early is startup turbulence, and the
  // measurement is still converging before the decrease may fire.
  // (0018/修改4: the "climb from PEER_REMB_START_BPS" rationale is gone
  // with the additive climb; the warmup itself is kept.)
  const int warmup = window_index < PEER_REMB_WARMUP_WINDOWS;

  // 0018: the estimate update (修改4) -- the AIMD climb is GONE.  The
  // declaration is what we actually receive x 1.1, clamped into
  // [PEER_REMB_FLOOR_BPS, PEER_REMB_CEIL_BPS]; on net stress (out of
  // warmup) it falls back to x 7/10 of that same estimate.  The net-stress
  // multiplicative decrease (R3/T1) and the warmup (R3/T3) are kept.
  // Written flat with plain local names, in the spirit of the R2 extractable
  // expression it replaces (0008-remb-semantics-check.mjs: the extracted
  // expression CHANGED -- see the 0018 report).
  const int back_off = congested && !warmup;
  uint32_t declared = pc->remb_declared_bps;
  if (span_bytes == 0) {
    // Nothing measured (yet) or the stream went quiet: the modest start
    // value, so a silent sender is not fed a floor-sized guess.
    declared = PEER_REMB_START_BPS;
  } else {
    const uint64_t estimate =
        (measured_bps * PEER_REMB_MEASURED_NUM) / PEER_REMB_MEASURED_DEN;
    declared = estimate > 0xffffffffull ? 0xffffffffu : (uint32_t)estimate;
    if (back_off) {
      declared = declared * PEER_REMB_MD_NUM / PEER_REMB_MD_DEN;
    }
    if (declared < PEER_REMB_FLOOR_BPS) {
      declared = PEER_REMB_FLOOR_BPS;
    }
    if (declared > PEER_REMB_CEIL_BPS) {
      declared = PEER_REMB_CEIL_BPS;
    }
  }
  pc->remb_declared_bps = declared;
  pc->remb_bitrate_bps = declared;
  const uint64_t bitrate = (uint64_t)declared;
  // 0018: dir is the mode actually taken this window: start (cold, no
  // measurement), md (net-stress fallback x 7/10, out of warmup) or est
  // (the plain measurement x 1.1).
  const char* dir = span_bytes == 0 ? "start" : (back_off ? "md" : "est");

  uint8_t packet[64];
  int size = rtcp_get_remb(packet, sizeof(packet), pc->vrtp_encoder.ssrc,
                           pc->remote_vssrc, (uint32_t)bitrate);
  if (size <= 0) {
    return;
  }
  dtls_srtp_encrypt_rctp_packet(&pc->dtls_srtp, packet, &size);
  const int ret = agent_send(&pc->agent, packet, size);
  if (ret == size) {
    pc->remb_sent++;
    peer_connection_diag_log("rtcp_remb sent=1 bitrateBps=%u measuredBps=%llu stressEv=%u netStress=%u selfStress=%u warmup=%u windowPkts=%u dir=%s",
                             (unsigned)bitrate, (unsigned long long)measured_bps,
                             (unsigned)stress, (unsigned)net_stress,
                             (unsigned)self_stress, (unsigned)warmup,
                             (unsigned)pkts, dir);
  } else {
    peer_connection_diag_log("rtcp_remb sent=0 bitrateBps=%u measuredBps=%llu stressEv=%u netStress=%u selfStress=%u warmup=%u windowPkts=%u dir=%s ret=%d",
                             (unsigned)bitrate, (unsigned long long)measured_bps,
                             (unsigned)stress, (unsigned)net_stress,
                             (unsigned)self_stress, (unsigned)warmup,
                             (unsigned)pkts, dir, ret);
  }
}

// 0040/R2 (task D): the REMB timeline -- what we declared, what actually
// arrived, how much stress accumulated, how old the last key frame is.
// Read this line against rtcp_remb: if declared keeps climbing while
// measured stays flat and stressTotal stays 0, the cloud does not follow
// our REMB at all (its encoder is gated somewhere else), and no receiver
// estimation will move it -- that is the "R2 is not the bottleneck"
// verdict, observable directly on the device.
static void peer_connection_log_remb_state(PeerConnection* pc) {
  if (!pc) {
    return;
  }
  const uint32_t now_ms = ports_get_epoch_time();
  if (pc->remb_state_diag_ms != 0 &&
      (uint32_t)(now_ms - pc->remb_state_diag_ms) < PEER_REMB_STATE_INTERVAL_MS) {
    return;
  }
  pc->remb_state_diag_ms = now_ms;
  const uint32_t keyframe_ago = pc->vrtp_decoder.last_keyframe_ms
                                    ? (uint32_t)(now_ms - pc->vrtp_decoder.last_keyframe_ms)
                                    : 0;
  peer_connection_diag_log("remb_state declared=%u measuredLast=%u stressTotal=%u keyframeAgoMs=%u",
                           pc->remb_declared_bps,
                           pc->remb_last_measured_bps,
                           pc->remb_stress_total,
                           keyframe_ago);
}

static void peer_connection_maybe_send_transport_feedback(PeerConnection* pc) {
  const uint64_t now_us = ports_get_monotonic_time_us();
  if (pc->state != PEER_CONNECTION_COMPLETED || !twcc_receiver_feedback_due(&pc->twcc, now_us))
    return;
  for (int i = 0; i < 4; i++) {
    uint8_t packet[TWCC_MAX_FEEDBACK_SIZE + 32];
    int size = twcc_receiver_build_feedback(&pc->twcc, pc->vrtp_encoder.ssrc,
                                            packet, TWCC_MAX_FEEDBACK_SIZE);
    if (size <= 0)
      break;
    if (dtls_srtp_encrypt_rctp_packet(&pc->dtls_srtp, packet, &size) != 0 ||
        agent_send(&pc->agent, packet, size) != size) {
      pc->twcc_send_failures++;
      break;
    }
  }
  twcc_receiver_feedback_sent(&pc->twcc, now_us);
  if (pc->twcc.feedback_sent <= 3 || pc->twcc.feedback_sent % 1200 == 0) {
    peer_connection_diag_log("twcc_feedback sent=%" PRIu32 " recorded=%" PRIu32 " late=%" PRIu32 " failures=%" PRIu32,
                             pc->twcc.feedback_sent, pc->twcc.packets_recorded,
                             pc->twcc.late_packets, pc->twcc_send_failures);
  }
}

const char* peer_connection_state_to_string(PeerConnectionState state) {
  switch (state) {
    case PEER_CONNECTION_NEW:
      return "new";
    case PEER_CONNECTION_CHECKING:
      return "checking";
    case PEER_CONNECTION_CONNECTED:
      return "connected";
    case PEER_CONNECTION_COMPLETED:
      return "completed";
    case PEER_CONNECTION_FAILED:
      return "failed";
    case PEER_CONNECTION_CLOSED:
      return "closed";
    case PEER_CONNECTION_DISCONNECTED:
      return "disconnected";
    default:
      return "unknown";
  }
}

PeerConnectionState peer_connection_get_state(PeerConnection* pc) {
  return pc->state;
}

void* peer_connection_get_sctp(PeerConnection* pc) {
  return &pc->sctp;
}

PeerConnection* peer_connection_create(PeerConfiguration* config) {
  PeerConnection* pc = calloc(1, sizeof(PeerConnection));
  if (!pc) {
    return NULL;
  }

  memcpy(&pc->config, config, sizeof(PeerConfiguration));
  pc->state = PEER_CONNECTION_NEW;

  agent_create(&pc->agent);

  memset(&pc->sctp, 0, sizeof(pc->sctp));
  peer_connection_ondatachannel(pc, NULL, NULL, NULL);

  if (pc->config.audio_codec) {
    rtp_encoder_init(&pc->artp_encoder, pc->config.audio_codec,
                     peer_connection_outgoing_rtp_packet, (void*)pc);

    rtp_decoder_init(&pc->artp_decoder, pc->config.audio_codec,
                     pc->config.onaudiotrack, pc->config.user_data);
    rtp_decoder_set_audio_callback(&pc->artp_decoder, pc->config.onaudiopacket);
  }

  if (pc->config.video_codec) {
    rtp_encoder_init(&pc->vrtp_encoder, pc->config.video_codec,
                     peer_connection_outgoing_rtp_packet, (void*)pc);

    rtp_decoder_init(&pc->vrtp_decoder, pc->config.video_codec,
                     pc->config.onvideotrack, pc->config.user_data);
    rtp_decoder_set_video_callback(&pc->vrtp_decoder, pc->config.onvideopacket);
  }

  return pc;
}

int peer_connection_set_video_codec(PeerConnection* pc, MediaCodec codec) {
  if (!pc)
    return -1;
  if (codec != CODEC_H264 && codec != CODEC_H265)
    return -1;
  if (pc->config.video_codec == codec)
    return 0;

  pc->config.video_codec = codec;

  // The decoder is rebuilt for the new codec.  The negotiated payload
  // types and the DONL flag have to survive that.
  rtp_decoder_cleanup(&pc->vrtp_decoder);
  rtp_decoder_init(&pc->vrtp_decoder, codec, pc->config.onvideotrack, pc->config.user_data);
  rtp_decoder_set_video_callback(&pc->vrtp_decoder, pc->config.onvideopacket);
  pc->vrtp_decoder.h265_payload_type = (uint8_t)pc->video_h265_payload_type;
  pc->vrtp_decoder.h265_has_donl = (uint8_t)(pc->video_h265_don ? 1 : 0);
  pc->video_has_last_nack = 0;
  memset(&pc->video_receiver_stats, 0, sizeof(pc->video_receiver_stats));
  return 0;
}

int peer_connection_set_video_h265_payload_type(PeerConnection* pc, int payload_type) {
  if (!pc)
    return -1;
  if (payload_type < 0 || payload_type > 127)
    return -1;
  pc->video_h265_payload_type = payload_type;
  pc->vrtp_decoder.h265_payload_type = (uint8_t)payload_type;
  return 0;
}

int peer_connection_set_video_h265_don(PeerConnection* pc, int enable) {
  if (!pc)
    return -1;
  pc->video_h265_don = enable ? 1 : 0;
  pc->vrtp_decoder.h265_has_donl = (uint8_t)(enable ? 1 : 0);
  return 0;
}

void peer_connection_destroy(PeerConnection* pc) {
  if (pc) {
    rtp_decoder_cleanup(&pc->artp_decoder);
    rtp_decoder_cleanup(&pc->vrtp_decoder);
    sctp_destroy_association(&pc->sctp);
    dtls_srtp_deinit(&pc->dtls_srtp);
    agent_destroy(&pc->agent);
    free(pc);
    pc = NULL;
  }
}

void peer_connection_close(PeerConnection* pc) {
  pc->state = PEER_CONNECTION_CLOSED;
}

int peer_connection_send_audio(PeerConnection* pc, const uint8_t* buf, size_t len) {
  if (pc->state != PEER_CONNECTION_COMPLETED) {
    // LOGE("dtls_srtp not connected");
    return -1;
  }
  return rtp_encoder_encode(&pc->artp_encoder, buf, len);
}

int peer_connection_send_video(PeerConnection* pc, const uint8_t* buf, size_t len) {
  if (pc->state != PEER_CONNECTION_COMPLETED) {
    // LOGE("dtls_srtp not connected");
    return -1;
  }
  return rtp_encoder_encode(&pc->vrtp_encoder, buf, len);
}

int peer_connection_datachannel_send(PeerConnection* pc, char* message, size_t len) {
  return peer_connection_datachannel_send_sid(pc, message, len, 0);
}

int peer_connection_datachannel_send_sid(PeerConnection* pc, char* message, size_t len, uint16_t sid) {
  if (!sctp_is_connected(&pc->sctp)) {
    LOGE("sctp not connected");
    return -1;
  }
  if (pc->config.datachannel == DATA_CHANNEL_STRING)
    return sctp_outgoing_data(&pc->sctp, message, len, PPID_STRING, sid);
  else
    return sctp_outgoing_data(&pc->sctp, message, len, PPID_BINARY, sid);
}

int peer_connection_datachannel_send_binary_sid(PeerConnection* pc, char* message, size_t len, uint16_t sid) {
  if (!sctp_is_connected(&pc->sctp)) {
    LOGE("sctp not connected");
    return -1;
  }
  return sctp_outgoing_data(&pc->sctp, message, len, PPID_BINARY, sid);
}

int peer_connection_create_datachannel(PeerConnection* pc, DecpChannelType channel_type, uint16_t priority, uint32_t reliability_parameter, char* label, char* protocol) {
  return peer_connection_create_datachannel_sid(pc, channel_type, priority, reliability_parameter, label, protocol, 0);
}

int peer_connection_create_datachannel_sid(PeerConnection* pc, DecpChannelType channel_type, uint16_t priority, uint32_t reliability_parameter, char* label, char* protocol, uint16_t sid) {
  int rtrn = -1;

  if (!sctp_is_connected(&pc->sctp)) {
#if !CONFIG_USE_USRSCTP
    peer_connection_diag_log("sctp_not_connected waiting_handshake sid=%u label=%s", sid, label ? label : "");
#endif
    LOGE("sctp not connected");
    return rtrn;
  }

  //  0                   1                   2                   3
  //  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
  // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  // |  Message Type |  Channel Type |            Priority           |
  // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  // |                    Reliability Parameter                      |
  // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  // |         Label Length          |       Protocol Length         |
  // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  // |                                                               |
  // |                             Label                             |
  // |                                                               |
  // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  // |                                                               |
  // |                            Protocol                           |
  // |                                                               |
  // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  int msg_size = 12 + strlen(label) + strlen(protocol);
  uint16_t priority_big_endian = htons(priority);
  uint32_t reliability_big_endian = htonl(reliability_parameter);
  uint16_t label_length = htons(strlen(label));
  uint16_t protocol_length = htons(strlen(protocol));
  char* msg = calloc(1, msg_size);
  if (!msg) {
    return rtrn;
  }

  msg[0] = DATA_CHANNEL_OPEN;
  msg[1] = (char)channel_type;
  memcpy(msg + 2, &priority_big_endian, sizeof(uint16_t));
  memcpy(msg + 4, &reliability_big_endian, sizeof(uint32_t));
  memcpy(msg + 8, &label_length, sizeof(uint16_t));
  memcpy(msg + 10, &protocol_length, sizeof(uint16_t));
  memcpy(msg + 12, label, strlen(label));
  memcpy(msg + 12 + strlen(label), protocol, strlen(protocol));

  rtrn = sctp_outgoing_data(&pc->sctp, msg, msg_size, PPID_CONTROL, sid);
  if (rtrn >= 0) {
    sctp_add_stream_mapping(&pc->sctp, label, sid);
  }
  free(msg);
  return rtrn;
}

static char* peer_connection_dtls_role_setup_value(DtlsSrtpRole d) {
  return d == DTLS_SRTP_ROLE_SERVER ? "a=setup:passive" : "a=setup:active";
}

static int peer_connection_read_dtls(PeerConnection* pc) {
  int ret = dtls_srtp_read(&pc->dtls_srtp, pc->temp_buf, sizeof(pc->temp_buf));
  LOGD("Got DTLS data %d", ret);

  if (ret > 0) {
    pc->sctp_read_events++;
    if (peer_connection_diagnostics_enabled &&
        (pc->sctp_read_events <= 10 || pc->sctp_read_events % 600 == 0)) {
      char sctp_preview[64];
      peer_connection_hex_preview(pc->temp_buf, ret, sctp_preview, sizeof(sctp_preview));
      peer_connection_diag_log("dtls_read_appdata ret=%d sctpEvents=%d preview=%s",
                               ret, pc->sctp_read_events, sctp_preview);
    }
    sctp_incoming_data(&pc->sctp, (char*)pc->temp_buf, ret);
  } else if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
    peer_connection_diag_log("dtls_peer_close_notify packets=%d sctpConnected=%d",
                             pc->completed_dtls_packets, sctp_is_connected(&pc->sctp));
    STATE_CHANGED(pc, PEER_CONNECTION_CLOSED);
  } else if (ret < 0 && (pc->completed_dtls_packets <= 10 || pc->completed_dtls_packets % 600 == 0)) {
    peer_connection_diag_log("dtls_read_no_appdata ret=%d dtlsPackets=%d",
                             ret, pc->completed_dtls_packets);
  }
  if (ret <= 0 && ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
    pc->dtls_pending = 0;
    pc->agent_ret = -1;
  }
  return ret;
}

int peer_connection_loop(PeerConnection* pc) {
  uint32_t ssrc = 0;
  int packet_processed = 0;
  if (!pc->dtls_pending) {
    memset(pc->agent_buf, 0, sizeof(pc->agent_buf));
    pc->agent_ret = -1;
  }

  switch (pc->state) {
    case PEER_CONNECTION_NEW:
      // 0037: the ICE connectivity check must not start before the answer is
      // on the wire.
      //
      // The check used to leave one frame after create_answer(), i.e. BEFORE
      // the answer had been sent to the cloud.  The cloud had not processed
      // our answer yet, so it did not know our ICE credentials and DROPPED
      // the check; the next one went out 100 frames later (~3.3 s at 30 fps).
      // That is the "sometimes it connects after 3 seconds" number.
      //
      // The gate is released by peer_connection_set_answer_sent() (the engine
      // calls it when the page reports that the answer went out).  The
      // fallback keeps ICE alive when that report never arrives.
      if (pc->answer_created_ms != 0) {
        const uint32_t now_ms = ports_get_epoch_time();
        const int answer_ready =
            pc->answer_sent ||
            (uint32_t)(now_ms - pc->answer_created_ms) >= PEER_ANSWER_SENT_FALLBACK_MS;
        if (answer_ready) {
          peer_connection_diag_log("ice_start -- the answer is on the wire (reported=%d)",
                                   pc->answer_sent);
          pc->ice_checking_started_ms = 0;
          pc->ice_checking_diag_ms = 0;
          STATE_CHANGED(pc, PEER_CONNECTION_CHECKING);
        }
      }
      break;

    case PEER_CONNECTION_CHECKING: {
      // 0037: the CHECKING state gets a real watchdog.  Before this the
      // state had NO timeout at all, so a peer connection whose pair table
      // was empty stayed in "checking" forever.
      const uint32_t now_ms = ports_get_epoch_time();
      if (pc->ice_checking_started_ms == 0) {
        pc->ice_checking_started_ms = now_ms;
        pc->ice_checking_diag_ms = 0;
      }

      // 0040: the pair table is asked FIRST, the watchdog SECOND.
      //
      // WHY this order: the two verdicts are DIFFERENT and must not
      // swallow each other.
      //   * "every pair hit its own give-up budget" is a verdict about the
      //     TABLE    -> ice_checking_failed reason=all_pairs_failed
      //   * "the round ran out of time while the table still had a live
      //     pair" is a verdict about the ROUND -> ice_checking_timeout
      //     reason=<why>
      // Both budgets are 14 s, so asking the watchdog first would always
      // win and the table verdict would never be printed.
      //
      // The table is only asked when it is not empty: an empty table has
      // nothing to select, and agent_select_candidate_pair() would report
      // "all pairs failed" for a table that never had a pair in it.
      if (pc->agent.candidate_pairs_num > 0) {
        if (agent_select_candidate_pair(&pc->agent) < 0) {
          peer_connection_diag_log(
              "ice_checking_failed reason=all_pairs_failed subreason=%s pairs=%d "
              "elapsedMs=%u requestsSent=%u responses=%u rejected=%u firstCheckMs=%d",
              peer_connection_ice_checking_reason(pc),
              pc->agent.candidate_pairs_num,
              (uint32_t)(now_ms - pc->ice_checking_started_ms),
              pc->agent.conncheck_requests_sent,
              pc->agent.binding_responses_received,
              pc->agent.rejected_binding_responses,
              peer_connection_ice_first_check_ms(pc));
          STATE_CHANGED(pc, PEER_CONNECTION_FAILED);
          break;
        }
        if (agent_connectivity_check(&pc->agent) == 0) {
          STATE_CHANGED(pc, PEER_CONNECTION_CONNECTED);
          break;
        }
      }

      // 0040: the round watchdog -- for EVERY shape of the failure.
      //
      //   pairs == 0          -> reason=no_candidate_pairs
      //   pairs, never sent   -> reason=pair_never_nominated
      //   sent, no answer     -> reason=no_binding_response
      //   answered, not used  -> reason=response_not_selected
      if ((uint32_t)(now_ms - pc->ice_checking_started_ms) >= PEER_ICE_CHECKING_TIMEOUT_MS) {
        peer_connection_diag_log(
            "ice_checking_timeout reason=%s elapsedMs=%u pairs=%d local=%d remote=%d mode=%d iceLite=%d "
            "requestsSent=%u responses=%u rejected=%u firstCheckMs=%d",
            peer_connection_ice_checking_reason(pc),
            (uint32_t)(now_ms - pc->ice_checking_started_ms),
            pc->agent.candidate_pairs_num,
            pc->agent.local_candidates_count,
            pc->agent.remote_candidates_count,
            pc->agent.mode,
            pc->remote_ice_lite,
            pc->agent.conncheck_requests_sent,
            pc->agent.binding_responses_received,
            pc->agent.rejected_binding_responses,
            peer_connection_ice_first_check_ms(pc));
        STATE_CHANGED(pc, PEER_CONNECTION_FAILED);
        break;
      }

      // 0037: log the pairs WHILE we are still checking.  The diagnostic
      // used to be printed only from dtls_start / transport_completed, so an
      // ICE that never succeeded printed nothing at all -- this line is the
      // only thing that shows "which check, when, and what state".
      if (pc->ice_checking_diag_ms == 0 ||
          (uint32_t)(now_ms - pc->ice_checking_diag_ms) >= PEER_ICE_CHECKING_DIAG_INTERVAL_MS) {
        pc->ice_checking_diag_ms = now_ms;
        const uint32_t last_check_ms = pc->agent.last_conncheck_sent_ms;
        const uint32_t last_check_ago_ms =
            last_check_ms ? (now_ms - last_check_ms) : 0;
        peer_connection_diag_log(
            "ice_checking total=%d local=%d remote=%d mode=%d iceLite=%d conncheck=%d "
            "requestsSent=%u intervalMs=%u lastCheckAgoMs=%u elapsedMs=%u responses=%u nonStun=%u",
            pc->agent.candidate_pairs_num,
            pc->agent.local_candidates_count,
            pc->agent.remote_candidates_count,
            pc->agent.mode,
            pc->remote_ice_lite,
            pc->agent.nominated_pair ? pc->agent.nominated_pair->conncheck : -1,
            pc->agent.conncheck_requests_sent,
            pc->agent.conncheck_interval_ms,
            last_check_ago_ms,
            (uint32_t)(now_ms - pc->ice_checking_started_ms),
            pc->agent.binding_responses_received,
            pc->agent.non_stun_packets_received);
        peer_connection_log_candidate_pairs(pc, "checking");
      }
      break;
    }

    case PEER_CONNECTION_CONNECTED:
      if (pc->dtls_handshake_attempts == 0) {
        pc->dtls_handshake_started_ms = ports_get_epoch_time();
        peer_connection_diag_log("dtls_start datachannel=%d candidatePairs=%d",
                                 pc->config.datachannel,
                                 pc->agent.candidate_pairs_num);
        peer_connection_log_candidate_pairs(pc, "dtls_start");
      }

      pc->dtls_handshake_attempts++;

      int dtls_ret = dtls_srtp_handshake(&pc->dtls_srtp, NULL);
      // 0035: keep the verdict.  It used to die with this stack frame, which
      // is why a failed DTLS handshake was completely invisible.
      pc->dtls_last_handshake_ret = dtls_ret;
      pc->dtls_handshake_ret_valid = 1;
      if (dtls_ret == 0) {
        LOGD("DTLS-SRTP handshake done");
        peer_connection_diag_log("dtls_done attempts=%d datachannel=%d",
                                 pc->dtls_handshake_attempts,
                                 pc->config.datachannel);

        if (pc->config.datachannel && !pc->sctp_create_attempted) {
          LOGI("SCTP create socket");
#if CONFIG_USE_USRSCTP
          peer_connection_diag_log("sctp_backend=usrsctp");
#else
          peer_connection_diag_log("sctp_backend=internal");
#endif
          peer_connection_diag_log("sctp_create_start");
          pc->sctp_create_attempted = 1;
          pc->sctp.userdata = pc;
          int sctp_ret = sctp_create_association(&pc->sctp, &pc->dtls_srtp);
          /* 0041: keep the verdict.  It used to die with this stack frame --
           * that is why the device only logged "SCTP create socket failed"
           * and the report could not say WHY. */
          pc->sctp_create_attempts++;
          pc->sctp_create_ret = sctp_ret;
          pc->sctp_create_fail_reason = pc->sctp.create_fail_reason;
          peer_connection_diag_log("sctp_create_done ret=%d reason=%s", sctp_ret,
                                   sctp_create_fail_reason_name(pc->sctp.create_fail_reason));
          if (sctp_ret != 0) {
            LOGE("SCTP create socket failed");
            STATE_CHANGED(pc, PEER_CONNECTION_FAILED);
            break;
          }
          if (pc->state != PEER_CONNECTION_CONNECTED)
            break;
        }

        peer_connection_log_candidate_pairs(pc, "transport_completed");
        peer_connection_diag_log("transport_completed");
        // 0011: the anchor of the PLI timer.
        pc->video_pli_anchor_ms = ports_get_epoch_time();
        STATE_CHANGED(pc, PEER_CONNECTION_COMPLETED);
      } else if ((uint32_t)(ports_get_epoch_time() - pc->dtls_handshake_started_ms) >=
                 PEER_DTLS_ENDPOINT_TIMEOUT_MS) {
        int failed_pairs = agent_fail_nominated_remote(&pc->agent);
        peer_connection_diag_log("dtls_endpoint_timeout attempts=%d failedPairs=%d; trying next remote",
                                 pc->dtls_handshake_attempts,
                                 failed_pairs);
        dtls_srtp_reset_session(&pc->dtls_srtp);
        pc->dtls_handshake_attempts = 0;
        pc->dtls_handshake_started_ms = 0;
        if (failed_pairs > 0) {
          // 0037: the next remote gets a FRESH checking budget.
          pc->ice_checking_started_ms = 0;
          pc->ice_checking_diag_ms = 0;
          STATE_CHANGED(pc, PEER_CONNECTION_CHECKING);
        } else {
          STATE_CHANGED(pc, PEER_CONNECTION_FAILED);
        }
      } else if (pc->dtls_handshake_attempts == 1 || pc->dtls_handshake_attempts % 600 == 0) {
        peer_connection_diag_log("dtls_pending ret=%d attempts=%d",
                                 dtls_ret,
                                 pc->dtls_handshake_attempts);
      }
      break;
    case PEER_CONNECTION_COMPLETED:
      sctp_tick(&pc->sctp);
      if (pc->state != PEER_CONNECTION_COMPLETED)
        break;
      if (pc->dtls_pending || mbedtls_ssl_check_pending(&pc->dtls_srtp.ssl)) {
        packet_processed = peer_connection_read_dtls(pc) > 0;
        if (packet_processed || pc->dtls_pending || pc->state != PEER_CONNECTION_COMPLETED)
          break;
      }
      // 0037: DRAIN the socket instead of taking ONE datagram per tick.
      //
      // One datagram per rendered frame capped the whole session at ~60
      // packets per second (less at the 23..31 fps the device really runs
      // at) and everything above it was dropped by the kernel -- that is the
      // media loss.  The drain stops as soon as the socket is dry.
      for (int drained = 0; drained < AGENT_RECV_DRAIN_MAX; drained++) {
        int is_stun = 0;
        pc->agent_ret = agent_recv_datagram(&pc->agent, pc->agent_buf, sizeof(pc->agent_buf), 0, &is_stun);
        if (pc->agent_ret < 0) {
          break;  // the socket is dry
        }
        if (is_stun) {
          // A STUN message (binding response / consent): the agent already
          // processed it.  Keep draining -- the old code stopped here and
          // the media packet behind the STUN one waited a whole frame.
          continue;
        }
        const uint64_t arrival_us = ports_get_monotonic_time_us();
        packet_processed = 1;
        LOGD("agent_recv %d", pc->agent_ret);
        pc->completed_udp_packets++;
        if (dtls_srtp_probe(pc->agent_buf)) {
          pc->dtls_pending = 1;
          pc->completed_dtls_packets++;
          if (peer_connection_diagnostics_enabled &&
              (pc->completed_dtls_packets <= 10 || pc->completed_dtls_packets % 600 == 0)) {
            char preview[64];
            peer_connection_hex_preview(pc->agent_buf, pc->agent_ret, preview, sizeof(preview));
            peer_connection_diag_log("udp_class=dtls count=%d bytes=%d preview=%s",
                                     pc->completed_dtls_packets,
                                     pc->agent_ret,
                                     preview);
          }
          peer_connection_read_dtls(pc);

        } else if (rtcp_probe(pc->agent_buf, pc->agent_ret)) {
          LOGD("Got RTCP packet");
          pc->completed_rtcp_packets++;
          if (peer_connection_diagnostics_enabled &&
              (pc->completed_rtcp_packets <= 5 || pc->completed_rtcp_packets % 600 == 0)) {
            char preview[64];
            peer_connection_hex_preview(pc->agent_buf, pc->agent_ret, preview, sizeof(preview));
            peer_connection_diag_log("udp_class=rtcp count=%d bytes=%d preview=%s",
                                     pc->completed_rtcp_packets,
                                     pc->agent_ret,
                                     preview);
          }
          if (dtls_srtp_decrypt_rtcp_packet(&pc->dtls_srtp, pc->agent_buf, &pc->agent_ret) != 0) {
            pc->rtcp_decrypt_failures++;
            peer_connection_diag_log("rtcp_decrypt_failed failures=%d bytesAfter=%d",
                                     pc->rtcp_decrypt_failures,
                                     pc->agent_ret);
            break;  // 0037: leaves the drain loop (it used to leave the switch)
          }
          peer_connection_incoming_rtcp(pc, pc->agent_buf, pc->agent_ret);

        } else if (rtp_packet_validate(pc->agent_buf, pc->agent_ret)) {
          LOGD("Got RTP packet");
          pc->completed_rtp_packets++;

          if (peer_connection_diagnostics_enabled &&
              (pc->completed_rtp_packets <= 10 || pc->completed_rtp_packets % 1200 == 0)) {
            char preview[64];
            peer_connection_hex_preview(pc->agent_buf, pc->agent_ret, preview, sizeof(preview));
            peer_connection_diag_log("udp_class=rtp count=%d bytes=%d preview=%s",
                                     pc->completed_rtp_packets,
                                     pc->agent_ret,
                                     preview);
          }

          if (dtls_srtp_decrypt_rtp_packet(&pc->dtls_srtp, pc->agent_buf, &pc->agent_ret) != 0) {
            pc->rtp_decrypt_failures++;
            peer_connection_diag_log("rtp_decrypt_failed failures=%d bytesAfter=%d",
                                     pc->rtp_decrypt_failures,
                                     pc->agent_ret);
            break;  // 0037: leaves the drain loop (it used to leave the switch)
          }

          twcc_receiver_record(&pc->twcc, pc->agent_buf, pc->agent_ret, arrival_us);
          ssrc = rtp_get_ssrc(pc->agent_buf);
          RtpPacket* rtp = (RtpPacket*)pc->agent_buf;
          const int is_audio_payload = rtp->header.type == PT_PCMU ||
                                       rtp->header.type == PT_PCMA ||
                                       rtp->header.type == PT_OPUS ||
                                       rtp->header.type == 63;
          // 0012: classify EVERY RTP packet right where the streams are
          // told apart -- "does the cloud send audio at all?" is answered
          // by these counters, not by the decoder (rtp_decode_generic()
          // keeps no counters of its own).  The audio/video decision is
          // the one this site already makes (is_audio_payload + the two
          // SSRCs); no separate payload-type table is introduced here.
          pc->rtp_packets_total++;
          if (ssrc == pc->remote_assrc || (pc->remote_assrc == 0 && is_audio_payload)) {
            pc->rtp_audio_packets++;
            if (pc->remote_assrc == 0) {
              pc->remote_assrc = ssrc;
              // 0012: remember the SSRC the PT-sniffing learned -- kept
              // distinct from remote_assrc, which may also come from the
              // SDP; 0 here means the sniffing never fired.
              pc->rtp_learned_audio_ssrc = ssrc;
              LOGI("Learned remote audio SSRC from RTP: %" PRIu32, pc->remote_assrc);
              peer_connection_diag_log("learned_audio_ssrc=%" PRIu32 " pt=%u",
                                       pc->remote_assrc, rtp->header.type);
            }
            rtp_decoder_decode(&pc->artp_decoder, pc->agent_buf, pc->agent_ret);
          } else if (ssrc == pc->remote_vssrc ||
                     (pc->remote_vssrc == 0 &&
                      rtp->header.type != PT_PCMU &&
                      rtp->header.type != PT_PCMA &&
                      rtp->header.type != PT_OPUS &&
                      rtp->header.type != 63)) {
            pc->rtp_video_packets++;
            if (pc->remote_vssrc == 0) {
              pc->remote_vssrc = ssrc;
              LOGI("Learned remote video SSRC from RTP: %" PRIu32, pc->remote_vssrc);
              // ★ 0011: a PLI addresses a media SSRC, so before the first
              //   video packet arrives there is nothing to address.  The
              //   cloud's offer normally carries the SSRC ("a=ssrc:<id>"),
              //   but if it did not, THIS is the moment we learn it -- and
              //   the stream has just started, which is exactly when a key
              //   frame is worth asking for.
              if (pc->vrtp_decoder.keyframes_received == 0) {
                peer_connection_send_pli_counted(pc, pc->remote_vssrc);
              }
            }
            const uint16_t sequence =
                (uint16_t)(((uint16_t)pc->agent_buf[2] << 8) | pc->agent_buf[3]);
            // 0018: a sequence number we once NACKed landing here is a
            // returned retransmission (修改2).
            peer_connection_nack_on_arrival(pc, sequence);
            rtcp_receiver_record_rtp(&pc->video_receiver_stats,
                                     pc->agent_buf, pc->agent_ret, ports_get_monotonic_time());
            // 0040/B1: remember how many VIDEO bytes arrived -- the REMB
            // declares max(this measurement, the floor).
            pc->video_bytes_since_remb += (uint64_t)pc->agent_ret;
            peer_connection_maybe_send_video_nacks(pc, sequence);
            rtp_decoder_decode(&pc->vrtp_decoder, pc->agent_buf, pc->agent_ret);
            if (pc->completed_rtp_packets % 6000 == 0) {
              peer_connection_diag_log(
                  "rtp_h264 packets=%u gaps=%u reordered=%u late=%u forced=%u buffered=%u nalus=%u auOk=%u auDrop=%u keyframes=%u pliSent=%u",
                  pc->vrtp_decoder.packets_received,
                  pc->vrtp_decoder.sequence_gaps,
                  pc->vrtp_decoder.reordered_packets,
                  pc->vrtp_decoder.late_packets_dropped,
                  pc->vrtp_decoder.forced_sequence_skips,
                  pc->vrtp_decoder.reorder_buffered_packets,
                  pc->vrtp_decoder.nal_units_completed,
                  pc->vrtp_decoder.access_units_completed,
                  pc->vrtp_decoder.access_units_dropped,
                  pc->vrtp_decoder.keyframes_received,
                  pc->vrtp_decoder.pli_sent);
            }
          } else {
            // 0012: RTP that matched NEITHER stream.  The old code dropped
            // it silently; now it is counted, because "packets that belong
            // to no stream" is its own failure mode (e.g. the cloud
            // sending on an SSRC we never negotiated).
            pc->rtp_unmatched_packets++;
          }

        } else {
          pc->completed_unknown_packets++;
          if (peer_connection_diagnostics_enabled &&
              (pc->completed_unknown_packets <= 10 || pc->completed_unknown_packets % 600 == 0)) {
            char preview[64];
            peer_connection_hex_preview(pc->agent_buf, pc->agent_ret, preview, sizeof(preview));
            peer_connection_diag_log("udp_class=unknown count=%d bytes=%d preview=%s totals udp/rtcp/dtls/rtp/unknown=%d/%d/%d/%d/%d",
                                     pc->completed_unknown_packets,
                                     pc->agent_ret,
                                     preview,
                                     pc->completed_udp_packets,
                                     pc->completed_rtcp_packets,
                                     pc->completed_dtls_packets,
                                     pc->completed_rtp_packets,
                                     pc->completed_unknown_packets);
          }
          LOGW("Unknown data");
        }
      }

      if (CONFIG_KEEPALIVE_TIMEOUT > 0 &&
          pc->agent.binding_request_time > 0 &&
          (ports_get_epoch_time() - pc->agent.binding_request_time) > CONFIG_KEEPALIVE_TIMEOUT) {
        LOGI("binding request timeout; keeping completed transport alive");
        pc->agent.binding_request_time = ports_get_epoch_time();
      }
      int consent_ret = agent_maybe_send_consent_request(&pc->agent, ports_get_epoch_time());
      if (consent_ret != 0 &&
          (pc->agent.consent_attempts <= 3 || pc->agent.consent_attempts % 24 == 0 || consent_ret < 0)) {
        peer_connection_diag_log("ice_consent ret=%d attempts=%" PRIu32 " sent=%" PRIu32 " responses=%" PRIu32 " rttMs=%" PRIu32,
                                 consent_ret,
                                 pc->agent.consent_attempts,
                                 pc->agent.consent_requests_sent,
                                 pc->agent.binding_responses_received,
                                 pc->agent.smoothed_binding_rtt_ms);
      }
      // 0040/R2: the timeline line (task D -- observation only).  It runs
      // before the media pump so that its window closes just before the
      // REMB decision of the same tick.
      peer_connection_log_remb_state(pc);
      rtp_decoder_poll(&pc->vrtp_decoder);
      // 0018: a forced-skips burst during the poll window triggers a PLI
      // through the STALE bypass (修改3).
      peer_connection_pli_on_forced_skips(pc);
      peer_connection_maybe_send_receiver_report(pc);
      // 0040: the REMB.  It is a PICTURE QUALITY feedback, not a
      // connectivity one -- see peer_connection_maybe_send_remb().
      peer_connection_maybe_send_remb(pc);
      peer_connection_maybe_send_transport_feedback(pc);
      // 0011/B2: ask for a key frame while the picture is stale -- at
      //   most one PLI per loop tick (12 bytes; the cost is negligible).
      peer_connection_maybe_send_pli(pc);
      peer_connection_log_pli_state(pc);
      // 0012: the audio observation line (see peer_connection_log_rtp_class()).
      peer_connection_log_rtp_class(pc);
      // 0013: the heal loop accounting + line (task C).
      peer_connection_account_heal_keyframe(pc, ports_get_epoch_time());
      peer_connection_log_heal_state(pc);

      break;
    case PEER_CONNECTION_FAILED:
      break;
    case PEER_CONNECTION_DISCONNECTED:
      break;
    case PEER_CONNECTION_CLOSED:
      break;
    default:
      break;
  }

  return packet_processed;
}

// 0035 (diagnostics only): parse an "a=fingerprint:<alg> <value>" line the way
// it really reads.
//
// The comparison in peer_connection_set_remote_description() copies the value
// from a FIXED offset (buf + 22), which assumes the algorithm name is exactly
// "sha-256".  This helper does not change that behaviour -- it only produces
// an independent, honest view of the line so that the engine can tell
// "the offer says X" from "libpeer compares against Y".
static void peer_connection_parse_fingerprint_line(const char* line,
                                                   char* alg, size_t alg_len,
                                                   char* value, size_t value_len,
                                                   char* compare_alg,
                                                   size_t compare_alg_len) {
  const char* p;
  size_t n = 0;

  if (alg && alg_len > 0) {
    alg[0] = '\0';
  }
  if (value && value_len > 0) {
    value[0] = '\0';
  }
  if (compare_alg && compare_alg_len > 0) {
    compare_alg[0] = '\0';
  }
  if (!line || !alg || alg_len == 0 || !value || value_len == 0) {
    return;
  }

  p = strstr(line, "a=fingerprint:");
  if (!p) {
    return;
  }
  p += strlen("a=fingerprint:");

  while (p[n] != '\0' && p[n] != ' ' && p[n] != '\r' && p[n] != '\n' &&
         n + 1 < alg_len) {
    alg[n] = p[n];
    n++;
  }
  alg[n] = '\0';

  while (p[n] == ' ') {
    n++;
  }

  {
    size_t v = 0;
    while (p[n] != '\0' && p[n] != '\r' && p[n] != '\n' && v + 1 < value_len) {
      value[v] = p[n];
      v++;
      n++;
    }
    value[v] = '\0';
  }

  if (compare_alg && compare_alg_len > 0) {
    strncpy(compare_alg, alg, compare_alg_len - 1);
    compare_alg[compare_alg_len - 1] = '\0';
  }
}

void peer_connection_set_remote_description(PeerConnection* pc, const char* sdp, SdpType type) {
  char* start = (char*)sdp;
  char* line = NULL;
  char buf[1024];
  char* val_start = NULL;
  uint32_t* ssrc = NULL;
  DtlsSrtpRole role = DTLS_SRTP_ROLE_SERVER;
  int is_update = 0;
  int video_ssrc_seen = 0;
  int audio_ssrc_seen = 0;
  Agent* agent = &pc->agent;

  // 0035: a new description means a new fingerprint verdict.
  pc->dtls_setup_attr[0] = '\0';
  pc->dtls_offer_fingerprint_alg[0] = '\0';
  pc->dtls_offer_fingerprint[0] = '\0';
  pc->dtls_srtp.remote_fingerprint_alg[0] = '\0';
  pc->dtls_srtp.fingerprint_check = 0;

  while ((line = strstr(start, "\r\n"))) {
    line = strstr(start, "\r\n");
    size_t line_len = (size_t)(line - start);
    if (line_len >= sizeof(buf))
      line_len = sizeof(buf) - 1;
    memcpy(buf, start, line_len);
    buf[line_len] = '\0';

    if (strstr(buf, "a=setup:passive")) {
      role = DTLS_SRTP_ROLE_CLIENT;
    }

    // 0035: remember what the offer actually said about the DTLS role.  The
    // role libpeer picks (always CLIENT for an answer) is wrong when the
    // offer says "a=setup:active" -- the peer then insists on being the
    // client and the handshake cannot succeed.
    if (strstr(buf, "a=setup:")) {
      const char* setup_value = strstr(buf, "a=setup:") + strlen("a=setup:");
      size_t setup_len = 0;
      while (setup_value[setup_len] != '\0' &&
             setup_value[setup_len] != '\r' &&
             setup_value[setup_len] != '\n' &&
             setup_len + 1 < sizeof(pc->dtls_setup_attr)) {
        pc->dtls_setup_attr[setup_len] = setup_value[setup_len];
        setup_len++;
      }
      pc->dtls_setup_attr[setup_len] = '\0';
    }

    if (strstr(buf, "a=fingerprint")) {
      strncpy(pc->dtls_srtp.remote_fingerprint, buf + 22, DTLS_SRTP_FINGERPRINT_LENGTH);

      // 0035 (diagnostics only -- the comparison above is left exactly as it
      // was): parse the line the way it really reads, so the engine can show
      // what the offer carries next to what libpeer compares against.
      peer_connection_parse_fingerprint_line(buf,
                                             pc->dtls_offer_fingerprint_alg,
                                             sizeof(pc->dtls_offer_fingerprint_alg),
                                             pc->dtls_offer_fingerprint,
                                             sizeof(pc->dtls_offer_fingerprint),
                                             pc->dtls_srtp.remote_fingerprint_alg,
                                             sizeof(pc->dtls_srtp.remote_fingerprint_alg));
    }

    if (strstr(buf, "a=ice-ufrag") &&
        strlen(agent->remote_ufrag) != 0 &&
        (strncmp(buf + strlen("a=ice-ufrag:"), agent->remote_ufrag, strlen(agent->remote_ufrag)) == 0)) {
      is_update = 1;
    }

    const int twcc_extension_id = twcc_parse_extension_id(buf);
    if (twcc_extension_id > 0 && pc->twcc.extension_id == 0) {
      twcc_receiver_init(&pc->twcc, (uint8_t)twcc_extension_id);
      peer_connection_diag_log("twcc_extension id=%d", twcc_extension_id);
    }

    if (strstr(buf, "m=video")) {
      ssrc = &pc->remote_vssrc;
    } else if (strstr(buf, "m=audio")) {
      ssrc = &pc->remote_assrc;
    }

    if ((val_start = strstr(buf, "a=ssrc:")) && ssrc) {
      int* ssrc_seen = ssrc == &pc->remote_vssrc
          ? &video_ssrc_seen : &audio_ssrc_seen;
      if (*ssrc_seen) {
        start = line + 2;
        continue;
      }
      *ssrc_seen = 1;
      uint32_t parsed_ssrc = strtoul(val_start + 7, NULL, 10);
      if (*ssrc == 0) {
        *ssrc = parsed_ssrc;
        LOGD("SSRC: %" PRIu32, *ssrc);
        peer_connection_diag_log("remote_ssrc_selected media=%s ssrc=%" PRIu32,
                                 ssrc == &pc->remote_vssrc ? "video" : "audio",
                                 *ssrc);
      } else if (*ssrc != parsed_ssrc) {
        const int video = ssrc == &pc->remote_vssrc;
        peer_connection_diag_log("remote_ssrc_changed media=%s previous=%" PRIu32 " current=%" PRIu32,
                                 video ? "video" : "audio", *ssrc, parsed_ssrc);
        *ssrc = parsed_ssrc;
        if (video) {
          rtp_decoder_cleanup(&pc->vrtp_decoder);
          rtp_decoder_init(&pc->vrtp_decoder, pc->config.video_codec,
                           pc->config.onvideotrack, pc->config.user_data);
          rtp_decoder_set_video_callback(&pc->vrtp_decoder, pc->config.onvideopacket);
          pc->video_has_last_nack = 0;
          memset(&pc->video_receiver_stats, 0, sizeof(pc->video_receiver_stats));
        } else {
          rtp_decoder_cleanup(&pc->artp_decoder);
          rtp_decoder_init(&pc->artp_decoder, pc->config.audio_codec,
                           pc->config.onaudiotrack, pc->config.user_data);
          rtp_decoder_set_audio_callback(&pc->artp_decoder, pc->config.onaudiopacket);
        }
      }
    }

    start = line + 2;
  }

  if (is_update) {
    return;
  }

  pc->remote_ice_lite = strstr(sdp, "a=ice-lite") != NULL;
  agent_set_remote_description(&pc->agent, (char*)sdp);
  if (type == SDP_TYPE_ANSWER) {
    agent_update_candidate_pairs(&pc->agent);
    // 0037: a fresh CHECKING budget for this answer.
    pc->ice_checking_started_ms = 0;
    pc->ice_checking_diag_ms = 0;
    STATE_CHANGED(pc, PEER_CONNECTION_CHECKING);
  }
}

static const char* peer_connection_create_sdp(PeerConnection* pc, SdpType sdp_type) {
  char* description = (char*)pc->temp_buf;

  memset(pc->temp_buf, 0, sizeof(pc->temp_buf));
  DtlsSrtpRole role = DTLS_SRTP_ROLE_SERVER;

  pc->sctp.connected = 0;

  switch (sdp_type) {
    case SDP_TYPE_OFFER:
      role = DTLS_SRTP_ROLE_SERVER;
      agent_clear_candidates(&pc->agent);
      pc->agent.mode = AGENT_MODE_CONTROLLING;
      break;
    case SDP_TYPE_ANSWER:
      role = DTLS_SRTP_ROLE_CLIENT;
      pc->agent.mode = pc->remote_ice_lite ? AGENT_MODE_CONTROLLING : AGENT_MODE_CONTROLLED;
      break;
    default:
      break;
  }

  dtls_srtp_reset_session(&pc->dtls_srtp);
  pc->dtls_handshake_attempts = 0;
  pc->dtls_handshake_started_ms = 0;
  dtls_srtp_init(&pc->dtls_srtp, role, pc);
  pc->dtls_srtp.udp_recv = peer_connection_dtls_srtp_recv;
  pc->dtls_srtp.udp_send = peer_connection_dtls_srtp_send;

  memset(pc->sdp, 0, sizeof(pc->sdp));
  // TODO: check if we have video or audio codecs
  sdp_create(pc->sdp,
             pc->config.video_codec != CODEC_NONE,
             pc->config.audio_codec != CODEC_NONE,
             pc->config.datachannel);

  agent_create_ice_credential(&pc->agent);
  sdp_append(pc->sdp, "a=ice-ufrag:%s", pc->agent.local_ufrag);
  sdp_append(pc->sdp, "a=ice-pwd:%s", pc->agent.local_upwd);
  sdp_append(pc->sdp, "a=fingerprint:sha-256 %s", pc->dtls_srtp.local_fingerprint);
  sdp_append(pc->sdp, peer_connection_dtls_role_setup_value(role));

  if (pc->config.video_codec == CODEC_H264) {
    sdp_append_h264(pc->sdp);
  } else if (pc->config.video_codec == CODEC_H265) {
    // Both codecs are advertised: the peer may drop the H.265 payload
    // types from the answer (RFC 3264 section 6.1 allows the answerer to
    // reduce the payload type list), and the connection has to stay alive
    // in that case.  The RTP payload type tells the depacketizer which
    // codec really arrives.
    sdp_append_h264_h265(pc->sdp);
  }

  switch (pc->config.audio_codec) {
    case CODEC_PCMA:
      sdp_append_pcma(pc->sdp);
      break;
    case CODEC_PCMU:
      sdp_append_pcmu(pc->sdp);
      break;
    case CODEC_OPUS:
      sdp_append_opus(pc->sdp);
    default:
      break;
  }

  if (pc->config.datachannel) {
    sdp_append_datachannel(pc->sdp);
  }

  pc->b_local_description_created = 1;

  agent_gather_candidate(&pc->agent, NULL, NULL, NULL);  // host address
  for (int i = 0; i < sizeof(pc->config.ice_servers) / sizeof(pc->config.ice_servers[0]); ++i) {
    if (pc->config.ice_servers[i].urls) {
      LOGI("ice server: %s", pc->config.ice_servers[i].urls);
      agent_gather_candidate(&pc->agent, pc->config.ice_servers[i].urls, pc->config.ice_servers[i].username, pc->config.ice_servers[i].credential);
    }
  }

  agent_get_local_description(&pc->agent, description, sizeof(pc->temp_buf));
  sdp_append(pc->sdp, description);

  if (pc->onicecandidate) {
    pc->onicecandidate(pc->sdp, pc->config.user_data);
  }

  return pc->sdp;
}

const char* peer_connection_create_offer(PeerConnection* pc) {
  return peer_connection_create_sdp(pc, SDP_TYPE_OFFER);
}

const char* peer_connection_create_answer(PeerConnection* pc) {
  const char* sdp = peer_connection_create_sdp(pc, SDP_TYPE_ANSWER);
  agent_update_candidate_pairs(&pc->agent);
  // 0037: do NOT enter CHECKING here.
  //
  // The answer has not been sent yet: the cloud has not seen our ICE
  // credentials, so the first connectivity check would arrive before the
  // cloud could answer it and would be dropped.  The transition into CHECKING
  // happens in peer_connection_loop() once the answer is on the wire (see
  // peer_connection_set_answer_sent() and PEER_ANSWER_SENT_FALLBACK_MS).
  pc->answer_sent = 0;
  pc->answer_created_ms = ports_get_epoch_time();
  pc->ice_checking_started_ms = 0;
  pc->ice_checking_diag_ms = 0;
  return sdp;
}

// 0037: tell libpeer that the answer went out to the cloud.
//
// The engine calls this when the page reports that the answer has been sent
// over the signalling channel.  Until then no connectivity check goes out:
// the cloud cannot answer a check that arrives before it has processed our
// answer (it does not know our ICE credentials yet).
//
// @param[in] pc   the peer connection
// @param[in] sent 1 = the answer is on the wire
void peer_connection_set_answer_sent(PeerConnection* pc, int sent) {
  if (!pc) {
    return;
  }
  pc->answer_sent = sent != 0;
  if (pc->answer_sent) {
    // 0040/F3: remember WHEN the answer went out.  It is the anchor of
    // iceFirstCheckMs -- the delay between "the answer is on the wire" and
    // "the first connectivity check left" is the direct measure of whether
    // the race that 0037 fixed is really gone.
    pc->answer_sent_ms = ports_get_epoch_time();
    peer_connection_diag_log("answer_sent -- the ICE connectivity check may start");
  }
}

int peer_connection_send_rtcp_pil(PeerConnection* pc, uint32_t ssrc) {
  uint8_t plibuf[128];
  int size = 12;

  if (!pc || pc->state != PEER_CONNECTION_COMPLETED || ssrc == 0)
    return -1;

  if (rtcp_get_pli(plibuf, size, pc->vrtp_encoder.ssrc, ssrc) != size)
    return -1;

  dtls_srtp_encrypt_rctp_packet(&pc->dtls_srtp, plibuf, &size);
  const int ret = agent_send(&pc->agent, plibuf, size);
  peer_connection_diag_log("rtcp_pli sent=%d mediaSsrc=%" PRIu32 " bytes=%d",
                           ret,
                           ssrc,
                           size);
  return ret;
}

static int peer_connection_send_rtcp_nack(PeerConnection* pc, uint32_t ssrc,
                                          uint16_t packet_id, uint16_t bitmask) {
  uint8_t nackbuf[64];
  int size = 16;

  if (!pc || pc->state != PEER_CONNECTION_COMPLETED || ssrc == 0)
    return -1;
  if (rtcp_get_nack(nackbuf, size, pc->vrtp_encoder.ssrc, ssrc,
                    packet_id, bitmask) != size)
    return -1;

  dtls_srtp_encrypt_rctp_packet(&pc->dtls_srtp, nackbuf, &size);
  return agent_send(&pc->agent, nackbuf, size);
}

int peer_connection_request_video_keyframe(PeerConnection* pc) {
  if (!pc)
    return -1;
  return peer_connection_send_rtcp_pil(pc, pc->remote_vssrc);
}

// callbacks
void peer_connection_on_connected(PeerConnection* pc, void (*on_connected)(void* userdata)) {
  pc->on_connected = on_connected;
}

void peer_connection_on_receiver_packet_loss(PeerConnection* pc,
                                             void (*on_receiver_packet_loss)(float fraction_loss, uint32_t total_loss, void* userdata)) {
  pc->on_receiver_packet_loss = on_receiver_packet_loss;
}

void peer_connection_onicecandidate(PeerConnection* pc, void (*onicecandidate)(char* sdp, void* userdata)) {
  pc->onicecandidate = onicecandidate;
}

void peer_connection_oniceconnectionstatechange(PeerConnection* pc,
                                                void (*oniceconnectionstatechange)(PeerConnectionState state, void* userdata)) {
  pc->oniceconnectionstatechange = oniceconnectionstatechange;
}

void peer_connection_ondatachannel(PeerConnection* pc,
                                   void (*onmessage)(char* msg, size_t len, void* userdata, uint16_t sid),
                                   void (*onopen)(void* userdata),
                                   void (*onclose)(void* userdata)) {
  if (pc) {
    pc->ondatachannelmessage = onmessage;
    pc->ondatachannelopen = onopen;
    pc->ondatachannelclose = onclose;
    pc->sctp.userdata = pc;
    sctp_onopen(&pc->sctp, peer_connection_datachannel_open);
    sctp_onclose(&pc->sctp, peer_connection_datachannel_close);
    sctp_onmessage(&pc->sctp, peer_connection_datachannel_message);
    /* 0014: the channel-open callback (sid + label) is wired here, so it
     *   fires exactly once per DCEP OPEN of the peer. */
    sctp_ondatachannel(&pc->sctp, peer_connection_datachannel_opened);
  }
}

void peer_connection_ondatachannelopened(PeerConnection* pc,
                                          void (*onopen)(uint16_t sid, const char* label, void* userdata)) {
  if (pc)
    pc->ondatachannelopened = onopen;
}

int peer_connection_get_datachannel_sid(PeerConnection* pc, uint16_t* sid) {
  if (!pc || !sid)
    return -1;
  /* 0014: the first entry of the stream table is the channel the peer
   *   opened (the cloud SDK uses a single DataChannel).  The sid exists
   *   only after the peer's DCEP OPEN arrived -- before that the table is
   *   empty and this returns -1. */
  if (pc->sctp.stream_count <= 0)
    return -1;
  *sid = pc->sctp.stream_table[0].sid;
  return 0;
}

int peer_connection_get_sctp_rtt_ms(PeerConnection* pc) {
  if (!pc)
    return -1;
  return sctp_get_rtt_ms(&pc->sctp);
}

int peer_connection_lookup_sid(PeerConnection* pc, const char* label, uint16_t* sid) {
  for (int i = 0; i < pc->sctp.stream_count; i++) {
    if (strncmp(pc->sctp.stream_table[i].label, label, sizeof(pc->sctp.stream_table[i].label)) == 0) {
      *sid = pc->sctp.stream_table[i].sid;
      return 0;
    }
  }
  return -1;  // Not found
}

char* peer_connection_lookup_sid_label(PeerConnection* pc, uint16_t sid) {
  for (int i = 0; i < pc->sctp.stream_count; i++) {
    if (pc->sctp.stream_table[i].sid == sid) {
      return pc->sctp.stream_table[i].label;
    }
  }
  return NULL;  // Not found
}

int peer_connection_add_ice_candidate(PeerConnection* pc, char* candidate) {
  Agent* agent = &pc->agent;
  if (agent->remote_candidates_count >= AGENT_MAX_CANDIDATES) {
    LOGE("Remote ICE candidate table is full");
    return -1;
  }
  if (ice_candidate_from_description(&agent->remote_candidates[agent->remote_candidates_count], candidate, candidate + strlen(candidate)) != 0) {
    return -1;
  }
  LOGD("Add candidate: %s", candidate);
  IceCandidate* remote = &agent->remote_candidates[agent->remote_candidates_count];
  agent->remote_candidates_count++;

  for (int i = 0; i < agent->local_candidates_count; i++) {
    if (agent->local_candidates[i].addr.family != remote->addr.family)
      continue;
    if (agent->candidate_pairs_num >= AGENT_MAX_CANDIDATE_PAIRS) {
      LOGE("ICE candidate pair table is full");
      return -1;
    }

    IceCandidatePair* pair = &agent->candidate_pairs[agent->candidate_pairs_num++];
    pair->local = &agent->local_candidates[i];
    pair->remote = remote;
    pair->priority = ice_candidate_pair_priority(
        pair->local->priority,
        pair->remote->priority,
        agent->mode == AGENT_MODE_CONTROLLING);
    pair->state = ICE_CANDIDATE_STATE_FROZEN;
  }
  agent_sort_candidate_pairs(agent);

  return 0;
}

int peer_connection_get_ice_candidate_pair_stats(PeerConnection* pc,
                                                 int* total,
                                                 int* frozen,
                                                 int* inprogress,
                                                 int* succeeded,
                                                 int* failed) {
  if (!pc) {
    return -1;
  }

  AgentCandidatePairStats stats;
  agent_get_candidate_pair_stats(&pc->agent, &stats);
  if (total)
    *total = stats.total;
  if (frozen)
    *frozen = stats.frozen;
  if (inprogress)
    *inprogress = stats.inprogress;
  if (succeeded)
    *succeeded = stats.succeeded;
  if (failed)
    *failed = stats.failed;
  return 0;
}

int peer_connection_get_rtt_ms(PeerConnection* pc) {
  if (!pc)
    return -1;

  const int ice_rtt_ms = agent_get_rtt_ms(&pc->agent);
  return ice_rtt_ms >= 0 ? ice_rtt_ms : sctp_get_rtt_ms(&pc->sctp);
}

/* 0041: the SCTP association creation result.  See the declaration in
 * peer_connection.h for what the three out parameters mean. */
int peer_connection_get_sctp_create_info(PeerConnection* pc,
                                          int* out_attempts,
                                          int* out_ret,
                                          const char** out_fail_reason) {
  if (!pc)
    return -1;

  if (out_attempts)
    *out_attempts = pc->sctp_create_attempts;
  if (out_ret)
    *out_ret = pc->sctp_create_ret;
  if (out_fail_reason)
    *out_fail_reason = pc->sctp_create_attempts > 0
                           ? sctp_create_fail_reason_name(pc->sctp_create_fail_reason)
                           : "none";
  return 0;
}

/* 0041: the SCTP output ring buffer depth.  See the declaration in
 * peer_connection.h for how to read it. */
int peer_connection_get_sctp_out_queued(PeerConnection* pc) {
  if (!pc)
    return -1;
  return sctp_get_output_queue_count(&pc->sctp);
}

uint64_t peer_connection_get_ice_tie_breaker(PeerConnection* pc) {
  if (!pc)
    return 0;
  return pc->agent.tie_breaker;
}

int peer_connection_get_last_dtls_write_result(PeerConnection* pc) {
  if (!pc)
    return 0;
  return dtls_srtp_get_last_write_result();
}

int peer_connection_get_video_rtp_stats(PeerConnection* pc, PeerVideoRtpStats* stats) {
  uint32_t now_ms;
  if (pc == NULL || stats == NULL)
    return -1;
  now_ms = ports_get_epoch_time();
  stats->packets_received = pc->vrtp_decoder.packets_received;
  stats->sequence_gaps = pc->vrtp_decoder.sequence_gaps;
  stats->reordered_packets = pc->vrtp_decoder.reordered_packets;
  stats->late_packets_dropped = pc->vrtp_decoder.late_packets_dropped;
  stats->forced_sequence_skips = pc->vrtp_decoder.forced_sequence_skips;
  stats->reorder_buffered_packets = pc->vrtp_decoder.reorder_buffered_packets;
  stats->access_units_completed = pc->vrtp_decoder.access_units_completed;
  stats->access_units_dropped = pc->vrtp_decoder.access_units_dropped;
  stats->nack_requests = pc->video_nack_requests;
  stats->nack_packets_requested = pc->video_nack_packets_requested;
  stats->nack_retrans_returned = pc->video_nack_retrans_returned;
  // 0011: the key frame / PLI counters.
  //
  //   The two "ms" fields are AGES ("how long ago"), 0 = never happened.
  //   The absolute stamps live in the decoder (RtpDecoder.last_keyframe_ms
  //   / .pli_last_sent_ms) -- this is where they become "3 s ago".
  stats->keyframes_received = pc->vrtp_decoder.keyframes_received;
  stats->pli_sent = pc->vrtp_decoder.pli_sent;
  stats->pli_last_sent_ms = pc->vrtp_decoder.pli_last_sent_ms
      ? (uint32_t)(now_ms - pc->vrtp_decoder.pli_last_sent_ms) : 0;
  stats->last_keyframe_ms = pc->vrtp_decoder.last_keyframe_ms
      ? (uint32_t)(now_ms - pc->vrtp_decoder.last_keyframe_ms) : 0;
  // 0018: the PLI policy counters (修改3).
  stats->pli_sent_total = pc->vrtp_decoder.pli_sent_total;
  stats->pli_suppressed_stale = pc->vrtp_decoder.pli_suppressed_stale;
  stats->pli_from_forced_skips = pc->vrtp_decoder.pli_from_forced_skips;
  return 0;
}

// 0035: turn a fingerprint into "algorithm + length + first 8 characters".
// The fingerprint is a public negotiated value, but there is no reason to put
// the whole thing into a log file or a stats blob.
static void peer_connection_fingerprint_summary(const char* fingerprint,
                                                const char* alg,
                                                char* out_alg, size_t out_alg_len,
                                                int* out_len,
                                                char* out_prefix, size_t out_prefix_len) {
  int len = 0;
  int i;

  if (out_alg && out_alg_len > 0) {
    out_alg[0] = '\0';
  }
  if (out_prefix && out_prefix_len > 0) {
    out_prefix[0] = '\0';
  }
  if (out_len) {
    *out_len = 0;
  }
  if (!fingerprint) {
    return;
  }
  while (len < DTLS_SRTP_FINGERPRINT_LENGTH && fingerprint[len] != '\0') {
    len++;
  }
  if (out_len) {
    *out_len = len;
  }
  if (out_alg && out_alg_len > 0 && alg) {
    strncpy(out_alg, alg, out_alg_len - 1);
    out_alg[out_alg_len - 1] = '\0';
  }
  if (out_prefix && out_prefix_len > 0) {
    for (i = 0; i < len && i < 8 && (size_t)(i + 1) < out_prefix_len; ++i) {
      out_prefix[i] = fingerprint[i];
    }
    out_prefix[i] = '\0';
  }
}

int peer_connection_get_diagnostics(PeerConnection* pc, PeerConnectionDiagnostics* out) {
  int i;

  if (!pc || !out) {
    return -1;
  }
  memset(out, 0, sizeof(*out));

  // --- DTLS ---------------------------------------------------------
  out->dtls_state = (int)pc->dtls_srtp.state;
  out->dtls_role = (int)pc->dtls_srtp.role;
  out->dtls_handshake_ret = pc->dtls_handshake_ret_valid ? pc->dtls_last_handshake_ret : 0;
  out->dtls_handshake_ret_valid = pc->dtls_handshake_ret_valid;
  out->dtls_mbedtls_ret = pc->dtls_srtp.last_mbedtls_ret;
  out->dtls_fingerprint_check = pc->dtls_srtp.fingerprint_check;
  out->dtls_handshake_attempts = pc->dtls_handshake_attempts;
  out->dtls_handshake_started_ms = pc->dtls_handshake_started_ms;
  out->dtls_handshake_elapsed_ms =
      pc->dtls_handshake_started_ms
          ? (uint32_t)(ports_get_epoch_time() - pc->dtls_handshake_started_ms)
          : 0;
  out->dtls_last_write_result = dtls_srtp_get_last_write_result();
  out->dtls_last_write_is_handshake = pc->dtls_srtp.last_write_is_handshake;

  // our own fingerprint -- the one that goes into our answer
  peer_connection_fingerprint_summary(pc->dtls_srtp.local_fingerprint,
                                      "sha-256",
                                      out->dtls_fingerprint_alg,
                                      sizeof(out->dtls_fingerprint_alg),
                                      &out->dtls_fingerprint_len,
                                      out->dtls_fingerprint_prefix,
                                      sizeof(out->dtls_fingerprint_prefix));

  // the fingerprint libpeer compares the peer's certificate against
  peer_connection_fingerprint_summary(pc->dtls_srtp.remote_fingerprint,
                                      pc->dtls_srtp.remote_fingerprint_alg[0]
                                          ? pc->dtls_srtp.remote_fingerprint_alg
                                          : "?",
                                      out->dtls_remote_fingerprint_alg,
                                      sizeof(out->dtls_remote_fingerprint_alg),
                                      &out->dtls_remote_fingerprint_len,
                                      out->dtls_remote_fingerprint_prefix,
                                      sizeof(out->dtls_remote_fingerprint_prefix));

  // the fingerprint the offer really carries
  peer_connection_fingerprint_summary(pc->dtls_offer_fingerprint,
                                      pc->dtls_offer_fingerprint_alg[0]
                                          ? pc->dtls_offer_fingerprint_alg
                                          : "?",
                                      out->dtls_offer_fingerprint_alg,
                                      sizeof(out->dtls_offer_fingerprint_alg),
                                      &out->dtls_offer_fingerprint_len,
                                      out->dtls_offer_fingerprint_prefix,
                                      sizeof(out->dtls_offer_fingerprint_prefix));

  strncpy(out->dtls_setup_attr, pc->dtls_setup_attr, sizeof(out->dtls_setup_attr) - 1);
  out->dtls_setup_attr[sizeof(out->dtls_setup_attr) - 1] = '\0';

  // --- ICE ----------------------------------------------------------
  out->ice_consent_requests_sent = pc->agent.consent_requests_sent;
  out->ice_consent_attempts = pc->agent.consent_attempts;
  out->ice_binding_responses_received = pc->agent.binding_responses_received;
  out->ice_rtt_ms = pc->agent.smoothed_binding_rtt_ms;
  out->ice_local_candidate_count = pc->agent.local_candidates_count;
  out->ice_candidate_pair_count = pc->agent.candidate_pairs_num;
  out->ice_selected_pair_index = -1;
  out->ice_nominated_pair_index = -1;
  // 0040/F2: the responses we received and threw away.
  out->ice_rejected_responses = pc->agent.rejected_binding_responses;
  // 0040/F3: when the first check went out, and how long after the answer.
  out->ice_first_check_ms = peer_connection_ice_first_check_ms(pc);
  out->ice_first_check_at_ms = pc->agent.first_conncheck_ms;
  out->ice_answer_sent_ms = pc->answer_sent_ms;
  // 0040: what the DTLS read path did.
  out->dtls_bio_read_calls = pc->dtls_srtp.bio_read_calls;
  out->dtls_bio_want_reads = pc->dtls_srtp.bio_want_reads;
  out->dtls_bio_eof_returns = pc->dtls_srtp.bio_eof_returns;
  out->dtls_bio_max_call_us = pc->dtls_srtp.bio_max_call_us;
  // 0040: the REMB counters.
  out->remb_sent = pc->remb_sent;
  out->remb_bitrate_bps = pc->remb_bitrate_bps;
  for (i = 0; i < pc->agent.candidate_pairs_num && i < PEER_DIAG_MAX_CANDIDATE_PAIRS; ++i) {
    IceCandidatePair* pair = &pc->agent.candidate_pairs[i];
    PeerCandidatePairDiag* diag = &out->ice_candidate_pairs[i];

    memset(diag, 0, sizeof(*diag));
    strncpy(diag->state,
            peer_connection_ice_state_name(pair->state),
            sizeof(diag->state) - 1);
    diag->conncheck = pair->conncheck;
    diag->selected = (pair == pc->agent.selected_pair) ? 1 : 0;
    diag->nominated = (pair == pc->agent.nominated_pair) ? 1 : 0;
    // 0040/F1: the per pair response counter -- "did the cloud answer
    // THIS address?".
    diag->responses_received = pair->responses_received;
    if (diag->selected) {
      out->ice_selected_pair_index = i;
    }
    if (diag->nominated) {
      out->ice_nominated_pair_index = i;
    }

    if (pair->local) {
      addr_to_string(&pair->local->addr, diag->local_ip, sizeof(diag->local_ip));
      diag->local_port = pair->local->addr.port;
      strncpy(diag->local_type,
              peer_connection_ice_type_name(pair->local->type),
              sizeof(diag->local_type) - 1);
    }
    if (pair->remote) {
      addr_to_string(&pair->remote->addr, diag->remote_ip, sizeof(diag->remote_ip));
      diag->remote_port = pair->remote->addr.port;
      strncpy(diag->remote_type,
              peer_connection_ice_type_name(pair->remote->type),
              sizeof(diag->remote_type) - 1);
    }
  }

  for (i = 0; i < pc->agent.local_candidates_count && i < PEER_DIAG_MAX_LOCAL_CANDIDATES; ++i) {
    IceCandidate* candidate = &pc->agent.local_candidates[i];
    PeerLocalCandidateDiag* diag = &out->ice_local_candidates[i];

    memset(diag, 0,sizeof(*diag));
    addr_to_string(&candidate->addr, diag->ip, sizeof(diag->ip));
    diag->port = candidate->addr.port;
    strncpy(diag->type,
            peer_connection_ice_type_name(candidate->type),
            sizeof(diag->type) - 1);
    strncpy(diag->proto, candidate->transport, sizeof(diag->proto) - 1);
    strncpy(diag->state,
            peer_connection_ice_state_name(candidate->state),
            sizeof(diag->state) - 1);
  }

  return 0;
}
