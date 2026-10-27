#ifndef AGENT_H_
#define AGENT_H_

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "base64.h"
#include "ice.h"
#include "socket.h"
#include "stun.h"
#include "utils.h"

#ifndef AGENT_MAX_DESCRIPTION
#define AGENT_MAX_DESCRIPTION 40960
#endif

#ifndef AGENT_MAX_CANDIDATES
#define AGENT_MAX_CANDIDATES 10
#endif

#ifndef AGENT_MAX_CANDIDATE_PAIRS
#define AGENT_MAX_CANDIDATE_PAIRS 100
#endif

/**
 * @brief how many connectivity check transaction ids are kept (0037).
 *
 * A binding response answers ONE of the checks that are still in flight, so
 * the ids of the last few checks have to be remembered to tell a legitimate
 * response from a foreign one.
 */
#ifndef AGENT_CONNCHECK_TXID_SLOTS
#define AGENT_CONNCHECK_TXID_SLOTS 4
#endif

typedef enum AgentState {

  AGENT_STATE_GATHERING_ENDED = 0,
  AGENT_STATE_GATHERING_STARTED,
  AGENT_STATE_GATHERING_COMPLETED,

} AgentState;

typedef enum AgentMode {

  AGENT_MODE_CONTROLLED = 0,
  AGENT_MODE_CONTROLLING

} AgentMode;

typedef struct Agent Agent;

struct Agent {
  char remote_ufrag[ICE_UFRAG_LENGTH + 1];
  char remote_upwd[ICE_UPWD_LENGTH + 1];

  char local_ufrag[ICE_UFRAG_LENGTH + 1];
  char local_upwd[ICE_UPWD_LENGTH + 1];

  IceCandidate local_candidates[AGENT_MAX_CANDIDATES];
  IceCandidate remote_candidates[AGENT_MAX_CANDIDATES];

  int local_candidates_count;
  int remote_candidates_count;

  UdpSocket udp_sockets[2];

  Address host_addr;
  int b_host_addr;
  uint64_t binding_request_time;
  uint32_t last_binding_request_sent_ms;
  uint32_t smoothed_binding_rtt_ms;
  uint32_t last_consent_request_ms;
  uint32_t consent_attempts;
  uint32_t consent_requests_sent;
  uint32_t binding_responses_received;
  // 0037: the ICE retransmission schedule.
  //
  // The old code decided when the next connectivity check goes out from
  // `nominated_pair->conncheck`, a counter that is bumped once per
  // peer_connection_loop() call -- and peer_connection_loop() is driven by
  // the RENDER FRAME.  The ICE rhythm therefore followed the frame rate
  // (23..31 fps on the device) instead of the clock, which is why the same
  // failure showed up as "connected in 3 seconds" sometimes and "nothing
  // happens for 48 seconds" other times.
  //
  // `conncheck` stays as the "how many did we send" counter; the TIME BASE is
  // the wall clock now.
  uint32_t last_conncheck_sent_ms;  // wall clock of the last binding request
  uint32_t conncheck_interval_ms;   // current retransmission interval (backoff)
  uint32_t conncheck_requests_sent; // how many binding requests went out
  uint32_t nominated_pair_started_ms; // wall clock of the nomination
  // How many UDP sockets are really open (CONFIG_IPV6 decides).
  int udp_socket_count;
  // Datagrams that were NOT STUN (media / DTLS).  Counted so that "the peer
  // sent something and we dropped it" is visible instead of silent.
  uint32_t non_stun_packets_received;
  AgentState state;

  AgentMode mode;

  // RFC 8445 ICE tie-breaker.
  //
  // It must not be 0: strict ICE implementations (the Xbox family, for one)
  // reject a session whose tie-breaker is zero.  It also has to stay the
  // same for the whole session, so it is generated once per Agent instead of
  // per Binding request.
  uint64_t tie_breaker;

  IceCandidatePair candidate_pairs[AGENT_MAX_CANDIDATE_PAIRS];
  IceCandidatePair* selected_pair;
  IceCandidatePair* nominated_pair;

  int candidate_pairs_num;
  int use_candidate;
  // 0037: the ICE attributes of the peer.
  //
  // We are the CONTROLLED agent (the cloud's offer has no a=ice-lite), so the
  // nomination belongs to the peer: it tells us WHICH pair is nominated with
  // the USE-CANDIDATE attribute of its connectivity check.  The attribute
  // used to be parsed and thrown away ("Do nothing"), so the nomination had
  // no effect at all.
  int remote_use_candidate;   // 1 = the peer sent USE-CANDIDATE
  int remote_is_controlling;  // 1 = the peer sent ICE-CONTROLLING
  uint64_t remote_tie_breaker;
  // 0037: the transaction ids of the connectivity checks that are still in
  // flight.  A response may answer ANY of them (the peer may answer check
  // #1 after we already sent check #2), so a small ring is kept.
  uint32_t conncheck_transaction_ids[AGENT_CONNCHECK_TXID_SLOTS][3];
  uint32_t conncheck_transaction_id_next;
  // 0037: binding responses that were dropped because they did not come
  // from the candidate we checked, or carried a foreign transaction id.
  uint32_t rejected_binding_responses;
  // 0040/F3: the wall clock of the FIRST connectivity check that really went
  // out (0 = no check has gone out yet).
  //
  // WHY: the engine could only see "the candidate table was handed over",
  // never "the first check actually left".  The cloud drops a check that
  // arrives before it has processed our answer (it does not know our ICE
  // credentials yet), so the DELAY between "the answer is on the wire" and
  // "the first check went out" is the direct measure of whether the 0037
  // race fix works: ~3.3 s (100 frames) before the fix, a few tens of ms
  // after it.
  //
  // ★ The criterion really moves: it is a wall clock stamp, not a counter.
  uint32_t first_conncheck_ms;
  uint32_t transaction_id[3];
};

typedef struct AgentCandidatePairStats {
  int total;
  int frozen;
  int inprogress;
  int succeeded;
  int failed;
} AgentCandidatePairStats;

void agent_gather_candidate(Agent* agent, const char* urls, const char* username, const char* credential);

void agent_create_ice_credential(Agent* agent);

void agent_get_local_description(Agent* agent, char* description, int length);

int agent_send(Agent* agent, const uint8_t* buf, int len);

int agent_recv(Agent* agent, uint8_t* buf, int len);

int agent_recv_nonblocking(Agent* agent, uint8_t* buf, int len);

/**
 * @brief how many datagrams one tick may take off the socket (0037).
 *
 * The engine used to read ONE datagram per rendered frame, which capped the
 * whole session at ~60 packets per second (less at the 23..31 fps the device
 * really runs at) and dropped everything above it.
 */
#ifndef AGENT_RECV_DRAIN_MAX
#define AGENT_RECV_DRAIN_MAX 32
#endif

/**
 * @brief read ONE datagram and report what it was (0037).
 *
 * Returns -1 when the socket is dry, 0 when the datagram was a STUN message
 * (it has already been processed here) and the number of bytes for anything
 * else (media / DTLS / SRTP).  *out_is_stun tells the two "0" cases apart --
 * agent_recv() cannot, which is why the drain loop needs this one.
 */
int agent_recv_datagram(Agent* agent, uint8_t* buf, int len, int timeout_ms, int* out_is_stun);

/**
 * @brief take everything that is pending off the socket, bounded (0037).
 *
 * Only the first read pays `timeout_ms`; the rest are non blocking.
 */
int agent_recv_drain(Agent* agent, uint8_t* buf, int len, int max_packets, int timeout_ms);

void agent_set_remote_description(Agent* agent, char* description);

int agent_select_candidate_pair(Agent* agent);

int agent_fail_nominated_remote(Agent* agent);

int agent_connectivity_check(Agent* agent);

int agent_maybe_send_consent_request(Agent* agent, uint32_t now_ms);

void agent_clear_candidates(Agent* agent);

int agent_create(Agent* agent);

void agent_destroy(Agent* agent);

void agent_update_candidate_pairs(Agent* agent);

void agent_sort_candidate_pairs(Agent* agent);

void agent_get_candidate_pair_stats(Agent* agent, AgentCandidatePairStats* stats);

int agent_get_rtt_ms(Agent* agent);

#endif  // AGENT_H_
