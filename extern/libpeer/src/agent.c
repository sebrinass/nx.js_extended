#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#include "agent.h"
#include "base64.h"
#include "ice.h"
#include "ice_candidate_pair_policy.h"
#include "ports.h"
#include "socket.h"
#include "stun.h"
#include "utils.h"

#define AGENT_POLL_TIMEOUT 1
#define AGENT_STUN_RECV_MAXTIMES 100

// 0037: the ICE retransmission schedule (RFC 8445 section 7.2.5).
//
// The retransmission interval is a WALL CLOCK deadline.  It used to be a
// FRAME count (`conncheck % AGENT_CONNCHECK_PERIOD == 0`), and peer_connection
// _loop() is driven by the render frame, so the ICE rhythm followed the frame
// rate (23..31 fps on the device) instead of the clock.
//
// 100 ms -> 200 ms -> 400 ms -> ... -> 2000 ms (cap).
#define AGENT_CONNCHECK_INTERVAL_BASE_MS 100
#define AGENT_CONNCHECK_INTERVAL_MAX_MS 2000

// 0037: how long a nominated pair may stay in progress.  The old code counted
// 1000 loop iterations, which is 16.7 s at 60 fps and 47.6 s at the 21 fps
// the device really runs at -- the "48 seconds without any reaction".
#define AGENT_CONNCHECK_PAIR_SWITCH_MS 3000

// 0039: the give-up budget of ONE pair, in wall clock.
//
// 0037 already replaced the FRAME COUNT that used to live here
// (`AGENT_CONNCHECK_MAX 1000`) with a wall clock deadline, so "1000 -> 300"
// has no referent any more: the count was only a proxy for "give up after
// about half a minute", and the proxy is gone.
//
// The cloud asked for the give-up to happen at 300 ticks instead of 1000,
// because 1000 ticks are 47.6 s at the 21 fps the device really runs at
// while 300 ticks are 14.3 s.  In the wall clock world that request is a
// request for a ~14 s budget -- and that is what this constant is now.
//
// ★ The criterion really moves: `in_progress_ms` in
//   agent_select_candidate_pair() is the wall clock since THIS pair was
//   nominated, so it grows on every loop iteration.  It is not a constant
//   that could sit at 0 and silently disable the give-up.
//
// ★ peer_connection.c carries the budget of the whole ICE round
//   (PEER_ICE_CHECKING_TIMEOUT_MS).  The two have to agree: the shorter one
//   wins and the other one is dead code.
#define AGENT_CONNCHECK_PAIR_TIMEOUT_MS 14000

// 0037: how many datagrams one tick may take off the socket.
// (AGENT_RECV_DRAIN_MAX lives in agent.h: peer_connection.c drains too.)

// 0040: forward declarations of the candidate pair helpers.  They are used
// by agent_pair_remote_candidate() (the peer reflexive path, further up) and
// by agent_sort_candidate_pairs() / agent_update_candidate_pairs() (further
// down), so they are declared once here.
static int agent_pair_exists_for_remote(Agent* agent, const Address* addr);
static IceCandidate* agent_best_local_candidate(Agent* agent, int family);
static int agent_pair_rank_before(const IceCandidatePair* other,
                                  const IceCandidatePair* candidate);

void agent_clear_candidates(Agent* agent) {
  agent->local_candidates_count = 0;
  agent->remote_candidates_count = 0;
  agent->candidate_pairs_num = 0;
  agent->selected_pair = NULL;
  agent->nominated_pair = NULL;
  agent->last_binding_request_sent_ms = 0;
  agent->smoothed_binding_rtt_ms = 0;
  agent->last_consent_request_ms = 0;
  agent->consent_attempts = 0;
  agent->consent_requests_sent = 0;
  agent->binding_responses_received = 0;
  // 0037: reset the ICE retransmission schedule together with the candidates.
  agent->last_conncheck_sent_ms = 0;
  agent->conncheck_interval_ms = AGENT_CONNCHECK_INTERVAL_BASE_MS;
  agent->conncheck_requests_sent = 0;
  agent->nominated_pair_started_ms = 0;
  agent->non_stun_packets_received = 0;
  // 0037: the ICE attributes of the peer and the outstanding checks.
  agent->remote_use_candidate = 0;
  agent->remote_is_controlling = 0;
  agent->remote_tie_breaker = 0;
  agent->conncheck_transaction_id_next = 0;
  agent->rejected_binding_responses = 0;
  // 0040/F3: the first check of a NEW round has to be stamped again.
  agent->first_conncheck_ms = 0;
}

// Generate the ICE tie-breaker for this session.
//
// RFC 8445 section 5 requires it to be a non-zero 64-bit value, and it has
// to be stable for the lifetime of the session.  The old code used a fixed
// 0, which strict peers (the Xbox family, for one) reject outright.
//
// The value is seeded from the wall clock plus the monotonic clock so two
// sessions on the same boot do not end up with the same tie-breaker, and
// then mixed through splitmix64 so the low bits are not simply the clock.
static uint64_t agent_generate_tie_breaker(void) {
  uint64_t seed = (uint64_t)ports_get_epoch_time() * 1000000u +
                   (uint64_t)(ports_get_monotonic_time_us() % 1000000u);
  uint64_t value = seed;
  for (int i = 0; i < 4; i++) {
    value += 0x9E3779B97F4A7C15ULL;
    uint64_t z = value;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    value = z ^ (z >> 31);
  }
  if (value == 0) {
    value = 1;
  }
  return value;
}

int agent_create(Agent* agent) {
  int ret;
  // 0037: remember how many sockets are really open.  With CONFIG_IPV6 == 0
  // only udp_sockets[0] is opened and udp_sockets[1].fd stays 0 (calloc),
  // which used to be fed into FD_ISSET() -- harmless today (fd 0 is stdin and
  // is never in the set) but a read past the end of the fd_set the moment
  // something else grabs fd 0.
  agent->udp_socket_count = 0;
  if ((ret = udp_socket_open(&agent->udp_sockets[0], AF_INET, 0)) < 0) {
    LOGE("Failed to create UDP socket.");
    return ret;
  }
  agent->udp_socket_count = 1;
  LOGI("create IPv4 UDP socket: %d", agent->udp_sockets[0].fd);

#if CONFIG_IPV6
  if ((ret = udp_socket_open(&agent->udp_sockets[1], AF_INET6, 0)) < 0) {
    LOGE("Failed to create IPv6 UDP socket.");
    return ret;
  }
  agent->udp_socket_count = 2;
  LOGI("create IPv6 UDP socket: %d", agent->udp_sockets[1].fd);
#endif

  agent_clear_candidates(agent);
  memset(agent->remote_ufrag, 0, sizeof(agent->remote_ufrag));
  memset(agent->remote_upwd, 0, sizeof(agent->remote_upwd));
  agent->tie_breaker = agent_generate_tie_breaker();
  return 0;
}

void agent_destroy(Agent* agent) {
  if (agent->udp_sockets[0].fd > 0) {
    udp_socket_close(&agent->udp_sockets[0]);
  }

#if CONFIG_IPV6
  if (agent->udp_sockets[1].fd > 0) {
    udp_socket_close(&agent->udp_sockets[1]);
  }
#endif
}

static int agent_socket_recv(Agent* agent, Address* addr, uint8_t* buf, int len, int timeout_ms) {
  int ret = -1;
  int i = 0;
  int maxfd = -1;
  fd_set rfds;
  struct timeval tv;
  int addr_type[] = { AF_INET,
#if CONFIG_IPV6
                      AF_INET6,
#endif
  };

  tv.tv_sec = 0;
  tv.tv_usec = timeout_ms * 1000;
  FD_ZERO(&rfds);

  for (i = 0; i < sizeof(addr_type) / sizeof(addr_type[0]); i++) {
    if (agent->udp_sockets[i].fd > maxfd) {
      maxfd = agent->udp_sockets[i].fd;
    }
    if (agent->udp_sockets[i].fd >= 0) {
      FD_SET(agent->udp_sockets[i].fd, &rfds);
    }
  }

  ret = select(maxfd + 1, &rfds, NULL, NULL, &tv);
  if (ret < 0) {
    LOGE("select error");
  } else if (ret == 0) {
    // timeout
  } else {
    // 0037: only walk the sockets that were really opened.  The old loop ran
    // over both entries, and with CONFIG_IPV6 == 0 udp_sockets[1].fd is 0
    // (calloc), so FD_ISSET(0, ...) was evaluated on a descriptor that does
    // not belong to us.
    for (i = 0; i < agent->udp_socket_count; i++) {
      if (agent->udp_sockets[i].fd < 0) {
        continue;
      }
      if (FD_ISSET(agent->udp_sockets[i].fd, &rfds)) {
        memset(buf, 0, len);
        ret = udp_socket_recvfrom(&agent->udp_sockets[i], addr, buf, len);
        break;
      }
    }
  }

  return ret;
}

static int agent_socket_recv_attempts(Agent* agent, Address* addr, uint8_t* buf, int len, int maxtimes) {
  int ret = -1;
  int i = 0;
  for (i = 0; i < maxtimes; i++) {
    if ((ret = agent_socket_recv(agent, addr, buf, len, AGENT_POLL_TIMEOUT)) != 0) {
      break;
    }
  }
  return ret;
}

static int agent_socket_send(Agent* agent, Address* addr, const uint8_t* buf, int len) {
  switch (addr->family) {
    case AF_INET6:
      return udp_socket_sendto(&agent->udp_sockets[1], addr, buf, len);
    case AF_INET:
    default:
      return udp_socket_sendto(&agent->udp_sockets[0], addr, buf, len);
  }
  return -1;
}

static int agent_create_host_addr(Agent* agent) {
  int i, j;
  const char* iface_prefx[] = {CONFIG_IFACE_PREFIX};
  IceCandidate* ice_candidate;
  int addr_type[] = { AF_INET,
#if CONFIG_IPV6
                      AF_INET6,
#endif
  };

  for (i = 0; i < sizeof(addr_type) / sizeof(addr_type[0]); i++) {
    for (j = 0; j < sizeof(iface_prefx) / sizeof(iface_prefx[0]); j++) {
      ice_candidate = agent->local_candidates + agent->local_candidates_count;
      // only copy port and family to addr of ice candidate
      ice_candidate_create(ice_candidate, agent->local_candidates_count, ICE_CANDIDATE_TYPE_HOST,
                           &agent->udp_sockets[i].bind_addr);
      // if resolve host addr, add to local candidate
      if (ports_get_host_addr(&ice_candidate->addr, iface_prefx[j])) {
        agent->local_candidates_count++;
      }
    }
  }

  return 0;
}

static int agent_create_stun_addr(Agent* agent, Address* serv_addr) {
  int ret = -1;
  Address bind_addr;
  StunMessage send_msg;
  StunMessage recv_msg;
  memset(&send_msg, 0, sizeof(send_msg));
  memset(&recv_msg, 0, sizeof(recv_msg));

  stun_msg_create(&send_msg, STUN_CLASS_REQUEST | STUN_METHOD_BINDING);

  ret = agent_socket_send(agent, serv_addr, send_msg.buf, send_msg.size);

  if (ret == -1) {
    LOGE("Failed to send STUN Binding Request.");
    return ret;
  }

  ret = agent_socket_recv_attempts(agent, NULL, recv_msg.buf, sizeof(recv_msg.buf), AGENT_STUN_RECV_MAXTIMES);
  if (ret <= 0) {
    LOGD("Failed to receive STUN Binding Response.");
    return ret;
  }

  stun_parse_msg_buf(&recv_msg);
  memcpy(&bind_addr, &recv_msg.mapped_addr, sizeof(Address));
  IceCandidate* ice_candidate = agent->local_candidates + agent->local_candidates_count++;
  ice_candidate_create(ice_candidate, agent->local_candidates_count, ICE_CANDIDATE_TYPE_SRFLX, &bind_addr);
  return ret;
}

static int agent_create_turn_addr(Agent* agent, Address* serv_addr, const char* username, const char* credential) {
  int ret = -1;
  uint32_t attr = ntohl(0x11000000);
  Address turn_addr;
  StunMessage send_msg;
  StunMessage recv_msg;
  memset(&recv_msg, 0, sizeof(recv_msg));
  memset(&send_msg, 0, sizeof(send_msg));
  stun_msg_create(&send_msg, STUN_METHOD_ALLOCATE);
  stun_msg_write_attr(&send_msg, STUN_ATTR_TYPE_REQUESTED_TRANSPORT, sizeof(attr), (char*)&attr);  // UDP
  stun_msg_write_attr(&send_msg, STUN_ATTR_TYPE_USERNAME, strlen(username), (char*)username);

  ret = agent_socket_send(agent, serv_addr, send_msg.buf, send_msg.size);
  if (ret == -1) {
    LOGE("Failed to send TURN Binding Request.");
    return -1;
  }

  ret = agent_socket_recv_attempts(agent, NULL, recv_msg.buf, sizeof(recv_msg.buf), AGENT_STUN_RECV_MAXTIMES);
  if (ret <= 0) {
    LOGD("Failed to receive STUN Binding Response.");
    return ret;
  }

  stun_parse_msg_buf(&recv_msg);

  if (recv_msg.stunclass == STUN_CLASS_ERROR && recv_msg.stunmethod == STUN_METHOD_ALLOCATE) {
    memset(&send_msg, 0, sizeof(send_msg));
    stun_msg_create(&send_msg, STUN_METHOD_ALLOCATE);
    stun_msg_write_attr(&send_msg, STUN_ATTR_TYPE_REQUESTED_TRANSPORT, sizeof(attr), (char*)&attr);  // UDP
    stun_msg_write_attr(&send_msg, STUN_ATTR_TYPE_USERNAME, strlen(username), (char*)username);
    stun_msg_write_attr(&send_msg, STUN_ATTR_TYPE_NONCE, strlen(recv_msg.nonce), recv_msg.nonce);
    stun_msg_write_attr(&send_msg, STUN_ATTR_TYPE_REALM, strlen(recv_msg.realm), recv_msg.realm);
    stun_msg_finish(&send_msg, STUN_CREDENTIAL_LONG_TERM, credential, strlen(credential));
  } else {
    LOGE("Invalid TURN Binding Response.");
    return -1;
  }

  ret = agent_socket_send(agent, serv_addr, send_msg.buf, send_msg.size);
  if (ret < 0) {
    LOGE("Failed to send TURN Binding Request.");
    return -1;
  }

  agent_socket_recv_attempts(agent, NULL, recv_msg.buf, sizeof(recv_msg.buf), AGENT_STUN_RECV_MAXTIMES);
  if (ret <= 0) {
    LOGD("Failed to receive TURN Binding Response.");
    return ret;
  }

  stun_parse_msg_buf(&recv_msg);
  memcpy(&turn_addr, &recv_msg.relayed_addr, sizeof(Address));
  IceCandidate* ice_candidate = agent->local_candidates + agent->local_candidates_count++;
  ice_candidate_create(ice_candidate, agent->local_candidates_count, ICE_CANDIDATE_TYPE_RELAY, &turn_addr);
  return ret;
}

void agent_gather_candidate(Agent* agent, const char* urls, const char* username, const char* credential) {
  char* pos;
  int port;
  char hostname[64];
  char addr_string[ADDRSTRLEN];
  int i;
  int addr_type[1] = {AF_INET};  // ipv6 no need stun
  Address resolved_addr;
  memset(hostname, 0, sizeof(hostname));

  if (urls == NULL) {
    agent_create_host_addr(agent);
    return;
  }

  if ((pos = strstr(urls + 5, ":")) == NULL) {
    LOGE("Invalid URL");
    return;
  }

  port = atoi(pos + 1);
  if (port <= 0) {
    LOGE("Cannot parse port");
    return;
  }

  snprintf(hostname, pos - urls - 5 + 1, "%s", urls + 5);

  for (i = 0; i < sizeof(addr_type) / sizeof(addr_type[0]); i++) {
    if (ports_resolve_addr(hostname, &resolved_addr) == 0) {
      addr_set_port(&resolved_addr, port);
      addr_to_string(&resolved_addr, addr_string, sizeof(addr_string));
      LOGI("Resolved stun/turn server %s:%d", addr_string, port);

      if (strncmp(urls, "stun:", 5) == 0) {
        LOGD("Create stun addr");
        agent_create_stun_addr(agent, &resolved_addr);
      } else if (strncmp(urls, "turn:", 5) == 0) {
        LOGD("Create turn addr");
        agent_create_turn_addr(agent, &resolved_addr, username, credential);
      }
    }
  }
}

void agent_create_ice_credential(Agent* agent) {
  memset(agent->local_ufrag, 0, sizeof(agent->local_ufrag));
  memset(agent->local_upwd, 0, sizeof(agent->local_upwd));

  utils_random_string(agent->local_ufrag, 4);
  utils_random_string(agent->local_upwd, 24);
}

void agent_get_local_description(Agent* agent, char* description, int length) {
  for (int i = 0; i < agent->local_candidates_count; i++) {
    ice_candidate_to_description(&agent->local_candidates[i], description + strlen(description), length - strlen(description));
  }

  // remove last \n
  description[strlen(description)] = '\0';
  LOGD("local description:\n%s", description);
}

int agent_send(Agent* agent, const uint8_t* buf, int len) {
  return agent_socket_send(agent, &agent->nominated_pair->remote->addr, buf, len);
}

static void agent_create_binding_response(Agent* agent, StunMessage* msg, Address* addr) {
  int size = 0;
  char username[584];
  char mapped_address[32];
  uint8_t mask[16];
  StunHeader* header;
  stun_msg_create(msg, STUN_CLASS_RESPONSE | STUN_METHOD_BINDING);
  header = (StunHeader*)msg->buf;
  memcpy(header->transaction_id, agent->transaction_id, sizeof(header->transaction_id));
  snprintf(username, sizeof(username), "%s:%s", agent->local_ufrag, agent->remote_ufrag);
  *((uint32_t*)mask) = htonl(MAGIC_COOKIE);
  memcpy(mask + 4, agent->transaction_id, sizeof(agent->transaction_id));
  size = stun_set_mapped_address(mapped_address, mask, addr);
  stun_msg_write_attr(msg, STUN_ATTR_TYPE_XOR_MAPPED_ADDRESS, size, mapped_address);
  stun_msg_write_attr(msg, STUN_ATTR_TYPE_USERNAME, strlen(username), username);
  stun_msg_finish(msg, STUN_CREDENTIAL_SHORT_TERM, agent->local_upwd, strlen(agent->local_upwd));
}

static void agent_create_binding_request(Agent* agent, StunMessage* msg, int nominate, uint32_t sequence) {
  // RFC 8445 section 5: the ICE-CONTROLLING / ICE-CONTROLLED attribute is a
  // 64-bit value in NETWORK byte order.  The old code wrote the raw host
  // order u64, which a little-endian host byte-swaps on the wire.
  uint8_t tie_breaker[8];
  for (int i = 0; i < 8; i++) {
    tie_breaker[i] = (uint8_t)(agent->tie_breaker >> (56 - i * 8));
  }
  // send binding request
  stun_msg_create(msg, STUN_CLASS_REQUEST | STUN_METHOD_BINDING);
  if (sequence != 0)
    ((StunHeader*)msg->buf)->transaction_id[2] = htonl(sequence);
  char username[584];
  memset(username, 0, sizeof(username));
  snprintf(username, sizeof(username), "%s:%s", agent->remote_ufrag, agent->local_ufrag);
  stun_msg_write_attr(msg, STUN_ATTR_TYPE_USERNAME, strlen(username), username);
  stun_msg_write_attr(msg, STUN_ATTR_TYPE_PRIORITY, 4, (char*)&agent->nominated_pair->priority);
  if (agent->mode == AGENT_MODE_CONTROLLING) {
    if (nominate)
      stun_msg_write_attr(msg, STUN_ATTR_TYPE_USE_CANDIDATE, 0, NULL);
    stun_msg_write_attr(msg, STUN_ATTR_TYPE_ICE_CONTROLLING, 8, (char*)tie_breaker);
  } else {
    stun_msg_write_attr(msg, STUN_ATTR_TYPE_ICE_CONTROLLED, 8, (char*)tie_breaker);
  }
  stun_msg_finish(msg, STUN_CREDENTIAL_SHORT_TERM, agent->remote_upwd, strlen(agent->remote_upwd));
}

// 0037: pair a remote candidate with every local candidate of the same
// family.
//
// agent_update_candidate_pairs() cannot be reused for this: it adds a pair
// for EVERY local x remote combination, so calling it a second time would
// duplicate all the pairs that already exist.
//
// 0040: the peer reflexive candidate goes through the SAME de-duplication as
// every other remote candidate -- one pair per remote ENDPOINT.  Without
// that, a peer reflexive candidate whose address is already in the remote
// candidate table would get a SECOND pair for the same endpoint, and the
// per pair budget of the round would be split between two pairs that
// address the very same peer.
//
// (The peer reflexive candidate is a REMOTE candidate -- libpeer never
// creates a local peer reflexive candidate -- so the per pair response
// counter of 0040/F1 is counted on this side.)
static void agent_pair_remote_candidate(Agent* agent, IceCandidate* remote) {
  IceCandidate* local;

  if (!agent || !remote) {
    return;
  }

  // 0040: one pair per remote endpoint -- the peer reflexive candidate must
  // not duplicate a pair that already exists for the same address.
  if (agent_pair_exists_for_remote(agent, &remote->addr)) {
    return;
  }

  local = agent_best_local_candidate(agent, remote->addr.family);
  if (!local) {
    return;  // no local candidate of that family
  }
  if (agent->candidate_pairs_num >= AGENT_MAX_CANDIDATE_PAIRS) {
    LOGE("candidate pair table is full");
    return;
  }
  IceCandidatePair* pair = &agent->candidate_pairs[agent->candidate_pairs_num];
  pair->local = local;
  pair->remote = remote;
  pair->priority = ice_candidate_pair_priority(local->priority,
                                               remote->priority,
                                               agent->mode == AGENT_MODE_CONTROLLING);
  pair->state = ICE_CANDIDATE_STATE_FROZEN;
  pair->conncheck = 0;
  // 0040/F1: a fresh pair has not seen a single response yet.
  pair->responses_received = 0;
  agent->candidate_pairs_num++;
}

// 0037: create a peer reflexive remote candidate for `addr`.
//
// The cloud (ICE-CONTROLLING) learned our NAT mapping from our own
// connectivity check and sends its check back to that address.  That address
// is not in remote_candidates, so -- without this -- there is no pair the
// cloud could nominate and ICE never left "checking".
//
// This is libwebrtc's MaybeAddPeerReflexiveCandidate(): the priority of the
// candidate is the PRIORITY the peer put into ITS request (the peer computed
// it for the pair it is checking), and it falls back to the local formula
// (type preference 110 for prflx, local preference 65535) when the request
// carries no PRIORITY.
static void agent_add_peer_reflexive_candidate(Agent* agent, Address* addr,
                                                int has_priority, uint32_t priority) {
  int i;
  char addr_string[ADDRSTRLEN];

  for (i = 0; i < agent->remote_candidates_count; i++) {
    if (addr_equal(&agent->remote_candidates[i].addr, addr)) {
      return;  // we know this address already
    }
  }

  if (agent->remote_candidates_count >= AGENT_MAX_CANDIDATES) {
    LOGW("remote candidate table is full -- cannot add the peer reflexive candidate");
    return;
  }

  IceCandidate* candidate = &agent->remote_candidates[agent->remote_candidates_count];
  // ice_candidate_create() computes the formula priority
  // (type preference 110 for prflx, local preference 65535, component 1).
  ice_candidate_create(candidate, agent->remote_candidates_count, ICE_CANDIDATE_TYPE_PRFLX, addr);
  candidate->raddr = *addr;
  if (has_priority) {
    // 0037: the PRIORITY of the request WINS -- the peer computed it for the
    // pair it is checking, and libwebrtc takes it as it is
    // (P2PTransportChannel::MaybeAddPeerReflexiveCandidate).
    candidate->priority = priority;
  }
  agent->remote_candidates_count++;

  addr_to_string(addr, addr_string, sizeof(addr_string));
  LOGI("added a peer reflexive remote candidate: %s:%d priority=%u (remote=%d)",
       addr_string, addr->port, candidate->priority, agent->remote_candidates_count);

  agent_pair_remote_candidate(agent, candidate);
  agent_sort_candidate_pairs(agent);
}

// 0037: the peer nominated a pair.
//
// RFC 8445 section 7.2.5.1: the controlled agent nominates the pair the
// USE-CANDIDATE request arrived on.  Before this the attribute was dropped
// in stun.c, so the nomination of the ICE-CONTROLLING peer had NO effect.
static void agent_nominate_pair_for_remote(Agent* agent, Address* addr) {
  int i;
  for (i = 0; i < agent->candidate_pairs_num; i++) {
    IceCandidatePair* pair = &agent->candidate_pairs[i];
    if (!pair->remote || !addr_equal(&pair->remote->addr, addr)) {
      continue;
    }
    pair->state = ICE_CANDIDATE_STATE_SUCCEEDED;
    agent->selected_pair = pair;
    agent->nominated_pair = pair;
    agent->use_candidate = 1;
    agent->remote_use_candidate = 1;
    LOGI("the peer nominated the candidate pair (USE-CANDIDATE)");
    return;
  }
  LOGW("USE-CANDIDATE received, but no candidate pair matches the source address");
}

void agent_process_stun_request(Agent* agent, StunMessage* stun_msg, Address* addr) {
  StunMessage msg;
  StunHeader* header;
  switch (stun_msg->stunmethod) {
    case STUN_METHOD_BINDING:
      if (stun_msg_is_valid(stun_msg->buf, stun_msg->size, agent->local_upwd) == 0) {
        header = (StunHeader*)stun_msg->buf;
        memcpy(agent->transaction_id, header->transaction_id, sizeof(header->transaction_id));

        // 0037: the source of the request may be an address we do not know
        // yet -- the cloud learned our NAT mapping from our own check and now
        // sends its check back to it.  Create the peer reflexive candidate for
        // it, otherwise the cloud's USE-CANDIDATE has nothing to nominate.
        agent_add_peer_reflexive_candidate(agent, addr, stun_msg->has_priority, stun_msg->priority);

        // 0037: we are the CONTROLLED agent (the cloud's offer has no
        // a=ice-lite), so the peer's nomination is the one that counts.
        if (stun_msg->has_ice_controlling) {
          agent->remote_is_controlling = 1;
          agent->remote_tie_breaker = stun_msg->ice_tie_breaker;
        }
        if (stun_msg->has_use_candidate) {
          agent->remote_use_candidate = 1;
          agent_nominate_pair_for_remote(agent, addr);
        }

        agent_create_binding_response(agent, &msg, addr);
        agent_socket_send(agent, addr, msg.buf, msg.size);
        agent->binding_request_time = ports_get_epoch_time();
      }
      break;
    default:
      break;
  }
}

// 0037: remember the transaction id of a connectivity check that went out.
static void agent_conncheck_remember_transaction_id(Agent* agent, const uint32_t* transaction_id) {
  const uint32_t slot = agent->conncheck_transaction_id_next % AGENT_CONNCHECK_TXID_SLOTS;
  memcpy(agent->conncheck_transaction_ids[slot], transaction_id, sizeof(uint32_t) * 3);
  agent->conncheck_transaction_id_next++;
}

// 0037: does this binding response belong to one of our checks?
//
// (a) it has to come from the candidate the check was sent to, and
// (b) it has to carry the transaction id of a check that is still in flight.
//
// Without the two checks anybody could push us into "connected" with a single
// forged datagram -- and the media is plain RTP.
static int agent_conncheck_response_matches(Agent* agent, StunMessage* stun_msg, Address* addr) {
  StunHeader* header = (StunHeader*)stun_msg->buf;
  uint32_t i;
  int found = 0;

  if (agent->conncheck_requests_sent == 0) {
    LOGW("binding response without an outstanding connectivity check -- dropped");
    return 0;
  }

  for (i = 0; i < AGENT_CONNCHECK_TXID_SLOTS; i++) {
    if (memcmp(agent->conncheck_transaction_ids[i],
               header->transaction_id,
               sizeof(header->transaction_id)) == 0) {
      found = 1;
      break;
    }
  }
  if (!found) {
    LOGW("binding response with a foreign transaction id -- dropped");
    return 0;
  }

  if (!agent->nominated_pair || !agent->nominated_pair->remote ||
      !addr_equal(&agent->nominated_pair->remote->addr, addr)) {
    LOGW("binding response from an address we did not check -- dropped");
    return 0;
  }

  return 1;
}

void agent_process_stun_response(Agent* agent, StunMessage* stun_msg, Address* addr) {
  switch (stun_msg->stunmethod) {
    case STUN_METHOD_BINDING:
      // 0037: the response has to belong to one of our checks.
      //
      // All THREE of the checks libwebrtc does are in place:
      //   1) MESSAGE-INTEGRITY + FINGERPRINT -- stun_msg_is_valid() below
      //      verifies both, with the ICE short term credential
      //      (agent->remote_upwd, i.e. the "a=ice-pwd" of the peer);
      //   2) the source address      -- agent_conncheck_response_matches();
      //   3) the transaction id      -- agent_conncheck_response_matches().
      if (stun_msg_is_valid(stun_msg->buf, stun_msg->size, agent->remote_upwd) == 0) {
        if (!agent_conncheck_response_matches(agent, stun_msg, addr)) {
          agent->rejected_binding_responses++;
          // ★ a dropped response does NOT trigger an immediate retry: the
          //   retransmission is driven by the wall clock schedule
          //   (agent_conncheck_due), so a flood of bad responses cannot
          //   amplify the traffic.
          return;
        }
        const uint32_t now_ms = ports_get_epoch_time();
        if (agent->last_binding_request_sent_ms != 0) {
          const uint32_t sample_ms =
              now_ms - agent->last_binding_request_sent_ms;
          if (sample_ms > 0 && sample_ms <= 5000) {
            agent->smoothed_binding_rtt_ms =
                agent->smoothed_binding_rtt_ms == 0
                    ? sample_ms
                    : (agent->smoothed_binding_rtt_ms * 7 + sample_ms) / 8;
          }
          agent->last_binding_request_sent_ms = 0;
        }
        agent->binding_responses_received++;
        // 0040/F1: the response belongs to the pair it came from.
        //
        // agent_conncheck_response_matches() only accepts a response whose
        // SOURCE ADDRESS is the address of the nominated pair, so the
        // nominated pair IS the pair this response answers.  Counting it
        // there is what makes "how many responses did the cloud send for
        // this IP" answerable per IP.
        //
        // ★ The criterion really moves: it stays 0 for every remote address
        //   the cloud never answered.
        agent->nominated_pair->responses_received++;
        agent->nominated_pair->state = ICE_CANDIDATE_STATE_SUCCEEDED;
        agent->binding_request_time = now_ms;
      } else {
        // 0040/F2: a response that failed the integrity check (wrong
        // MESSAGE-INTEGRITY or FINGERPRINT) was DROPPED, but the old code
        // dropped it SILENTLY -- the counter only covered the "foreign
        // transaction id" and "wrong source address" cases.
        //
        // That gap matters: "the cloud answered and we threw the answer
        // away" and "the cloud never answered at all" look identical from
        // the outside, and only this counter tells them apart.
        agent->rejected_binding_responses++;
        LOGW("binding response failed the integrity check -- dropped");
      }
      break;
    default:
      break;
  }
}

// 0037: read ONE datagram and say what it was.
//
// `agent_recv_with_timeout()` used to return 0 for a STUN message that was
// received AND processed -- and 0 is exactly what a caller cannot tell from
// "nothing to read".  A drain loop built on that return value stops at the
// first STUN message and the datagram behind it stays in the socket forever.
//
// out_is_stun is set to 1 when the datagram was a STUN message (already
// processed here), out_len receives the number of bytes for everything else.
int agent_recv_datagram(Agent* agent, uint8_t* buf, int len, int timeout_ms, int* out_is_stun) {
  int ret = -1;
  StunMessage stun_msg;
  Address addr;

  *out_is_stun = 0;
  if ((ret = agent_socket_recv(agent, &addr, buf, len, timeout_ms)) <= 0) {
    return ret;  // < 0: the socket is dry (timeout) or the read failed
  }

  agent->binding_request_time = ports_get_epoch_time();

  if (stun_probe(buf, len) == 0) {
    memcpy(stun_msg.buf, buf, ret);
    stun_msg.size = ret;
    stun_parse_msg_buf(&stun_msg);
    switch (stun_msg.stunclass) {
      case STUN_CLASS_REQUEST:
        agent_process_stun_request(agent, &stun_msg, &addr);
        break;
      case STUN_CLASS_RESPONSE:
        agent_process_stun_response(agent, &stun_msg, &addr);
        break;
      case STUN_CLASS_ERROR:
        break;
      default:
        break;
    }
    *out_is_stun = 1;
    return 0;
  }

  // Not STUN: media / DTLS / SRTP.  The caller gets the bytes.
  return ret;
}

int agent_recv(Agent* agent, uint8_t* buf, int len) {
  int is_stun = 0;
  return agent_recv_datagram(agent, buf, len, AGENT_POLL_TIMEOUT, &is_stun);
}

int agent_recv_nonblocking(Agent* agent, uint8_t* buf, int len) {
  int is_stun = 0;
  return agent_recv_datagram(agent, buf, len, 0, &is_stun);
}

// 0037: take everything that is pending off the socket (bounded).
//
// The engine used to read ONE datagram per rendered frame, so at 60 fps the
// whole session could not exceed ~60 packets per second -- and less at the
// 23..31 fps the device really runs at.  Everything above that was dropped
// by the kernel, which shows up as media loss.
//
// The loop stops as soon as the socket runs dry.  Only the FIRST read pays
// the poll timeout; the rest are non blocking, because the datagram is
// already sitting in the socket.
int agent_recv_drain(Agent* agent, uint8_t* buf, int len, int max_packets, int timeout_ms) {
  int received = 0;
  int is_stun = 0;
  int ret;

  while (received < max_packets) {
    ret = agent_recv_datagram(agent, buf, len, timeout_ms, &is_stun);
    if (ret < 0) {
      break;  // the socket is dry
    }
    if (!is_stun) {
      agent->non_stun_packets_received++;
    }
    received++;
    timeout_ms = 0;  // the rest of the drain must not block
  }

  return received;
}

void agent_set_remote_description(Agent* agent, char* description) {
  /*
  a=ice-ufrag:Iexb
  a=ice-pwd:IexbSoY7JulyMbjKwISsG9
  a=candidate:1 1 UDP 1 36.231.28.50 38143 typ srflx
  */
  int i;

  LOGD("Set remote description:\n%s", description);

  char* line_start = description;
  char* line_end = NULL;

  while ((line_end = strstr(line_start, "\r\n")) != NULL) {
    if (strncmp(line_start, "a=ice-ufrag:", strlen("a=ice-ufrag:")) == 0) {
      strncpy(agent->remote_ufrag, line_start + strlen("a=ice-ufrag:"), line_end - line_start - strlen("a=ice-ufrag:"));

    } else if (strncmp(line_start, "a=ice-pwd:", strlen("a=ice-pwd:")) == 0) {
      strncpy(agent->remote_upwd, line_start + strlen("a=ice-pwd:"), line_end - line_start - strlen("a=ice-pwd:"));

    } else if (strncmp(line_start, "a=candidate:", strlen("a=candidate:")) == 0) {
      if (ice_candidate_from_description(&agent->remote_candidates[agent->remote_candidates_count], line_start, line_end) == 0) {
        for (i = 0; i < agent->remote_candidates_count; i++) {
          if (strcmp(agent->remote_candidates[i].foundation, agent->remote_candidates[agent->remote_candidates_count].foundation) == 0) {
            break;
          }
        }
        if (i == agent->remote_candidates_count) {
          agent->remote_candidates_count++;
        }
      }
    }

    line_start = line_end + 2;
  }

  LOGD("remote ufrag: %s", agent->remote_ufrag);
  LOGD("remote upwd: %s", agent->remote_upwd);
}

void agent_sort_candidate_pairs(Agent* agent) {
  int i;
  if (!agent || agent->nominated_pair || agent->selected_pair)
    return;

  // 0040: sort by (reachability, priority).
  //
  // The primary key is the REACHABILITY RANK of the remote address: a
  // remote candidate that a public peer can not possibly be reached at
  // (a private / loopback / link local address) is tried LAST.  The
  // secondary key stays the pair priority, so the RFC 8445 order is kept
  // inside each rank.
  //
  // WHY the rank has to come first: libpeer checks ONE pair at a time and
  // gives every pair only AGENT_CONNCHECK_PAIR_SWITCH_MS (3 s).  When the
  // unreachable candidate happens to have the higher priority, it burns
  // the budget of the whole ICE round on an address that can never
  // answer, and the reachable candidates never even get their turn.
  //
  // ★ The private candidates are NOT dropped -- they are only demoted.  A
  //   cloud that publishes a LAN candidate (the console publishes one)
  //   is reachable exactly when the client is on the same LAN, and then
  //   the LAN candidate is the FAST one.
  for (i = 1; i < agent->candidate_pairs_num; ++i) {
    IceCandidatePair candidate = agent->candidate_pairs[i];
    int insertion = i;
    while (insertion > 0 && agent_pair_rank_before(&agent->candidate_pairs[insertion - 1],
                                                  &candidate)) {
      agent->candidate_pairs[insertion] =
          agent->candidate_pairs[insertion - 1];
      --insertion;
    }
    agent->candidate_pairs[insertion] = candidate;
  }
}

// 0040: the reachability rank of a pair's remote address.
//
// 0 = the remote address is a normal (public) address -- try it early.
// 1 = the remote address is one that a peer OUTSIDE of our own LAN can
//     not be reached at -- try it last.
//
// The point is NOT to delete such a candidate (a console that publishes a
// LAN candidate IS reachable when both are on the same LAN), it is to make
// sure that the address which can answer from the internet is checked
// FIRST, so the 3 s per pair budget is not burned on an address that
// cannot answer at all.
//
// ★ HONEST NOTE: the miHoYo cloud currently publishes three PUBLIC
//   addresses (119.6.222.11 / 182.138.254.11 / 112.45.114.203), so for
//   the data we have today this rank changes NOTHING -- every candidate
//   gets rank 0.  It is a DEFENSIVE mechanism for a cloud that also
//   publishes a LAN candidate (the Xbox cloud does), not a fix for the
//   problem we are hitting right now.
static int agent_addr_reachability_rank(const Address* addr) {
  if (!addr) {
    return 1;
  }
  if (addr->family == AF_INET) {
    const uint32_t ip = ntohl(addr->sin.sin_addr.s_addr);
    const uint8_t o1 = (uint8_t)((ip >> 24) & 0xff);
    const uint8_t o2 = (uint8_t)((ip >> 16) & 0xff);
    // 10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16 -- RFC 1918
    if (o1 == 10) {
      return 1;
    }
    if (o1 == 192 && o2 == 168) {
      return 1;
    }
    if (o1 == 172 && o2 >= 16 && o2 <= 31) {
      return 1;
    }
    // 127.0.0.0/8 -- loopback
    if (o1 == 127) {
      return 1;
    }
    // 169.254.0.0/16 -- link local
    if (o1 == 169 && o2 == 254) {
      return 1;
    }
    // 0.0.0.0 -- "any": a placeholder, never a real peer
    if (ip == 0) {
      return 1;
    }
    return 0;
  }
  if (addr->family == AF_INET6) {
    // ::1 loopback, fe80::/10 link local, fc00::/7 unique local, :: unspecified
    const uint8_t* b = addr->sin6.sin6_addr.s6_addr;
    if (memcmp(b, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\1", 16) == 0) {
      return 1;
    }
    if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80) {
      return 1;
    }
    if ((b[0] & 0xfe) == 0xfc) {
      return 1;
    }
    int all_zero = 1;
    for (int i = 0; i < 16; i++) {
      if (b[i] != 0) {
        all_zero = 0;
        break;
      }
    }
    return all_zero ? 1 : 0;
  }
  // An address family we do not know: treat it as unreachable rather than
  // let it eat the budget of the round.
  return 1;
}

static int agent_pair_rank(const IceCandidatePair* pair) {
  if (!pair || !pair->remote) {
    return 1;
  }
  return agent_addr_reachability_rank(&pair->remote->addr);
}

// 0040: does `candidate` have to be checked before `other`?
//
// The rule: the reachable remote addresses first, and inside one rank the
// higher pair priority first (that is the order RFC 8445 asks for).
static int agent_pair_rank_before(const IceCandidatePair* other,
                                  const IceCandidatePair* candidate) {
  const int other_rank = agent_pair_rank(other);
  const int candidate_rank = agent_pair_rank(candidate);
  if (candidate_rank != other_rank) {
    return candidate_rank < other_rank;
  }
  return other->priority < candidate->priority;
}

// 0040: is there already a pair for this remote ENDPOINT (ip:port)?
//
// The pair table used to be the full local x remote product.  With M local
// candidates and N remote candidates that is M*N pairs, and libpeer checks
// ONE pair at a time with a 3 s budget each -- so a cloud that publishes 3
// addresses and a host that has 3 local candidates burns 9 x 3 s = 27 s on
// a round whose budget is 14 s, and only about a third of the addresses
// ever get checked.
//
// ONE pair per remote endpoint keeps the table at N entries.  The local
// candidate that goes with it is the one with the HIGHEST priority (the
// best local address we have), because that is the one with the best
// chance of being routable back.
static int agent_pair_exists_for_remote(Agent* agent, const Address* addr) {
  int i;
  if (!agent || !addr) {
    return 0;
  }
  for (i = 0; i < agent->candidate_pairs_num; i++) {
    const IceCandidatePair* pair = &agent->candidate_pairs[i];
    if (pair->remote && addr_equal(&pair->remote->addr, addr)) {
      return 1;
    }
  }
  return 0;
}

// 0040: the local candidate that goes with a remote one.
//
// The local candidates are the host address and the (STUN derived)
// server reflexive address.  The one with the higher priority is the one
// the peer is most likely to be able to reach, so it is the one the pair
// is built with.
static IceCandidate* agent_best_local_candidate(Agent* agent, int family) {
  int i;
  IceCandidate* best = NULL;
  for (i = 0; i < agent->local_candidates_count; i++) {
    IceCandidate* candidate = &agent->local_candidates[i];
    if (candidate->addr.family != family) {
      continue;
    }
    if (!best || candidate->priority > best->priority) {
      best = candidate;
    }
  }
  return best;
}

void agent_update_candidate_pairs(Agent* agent) {
  int j;
  // Please set gather candidates before set remote description
  //
  // 0040: ONE pair per remote ENDPOINT (ip:port), not one per local x
  // remote combination.  See agent_pair_exists_for_remote() for why.
  for (j = 0; j < agent->remote_candidates_count; j++) {
    IceCandidate* remote = &agent->remote_candidates[j];
    IceCandidate* local;
    if (agent_pair_exists_for_remote(agent, &remote->addr)) {
      continue;  // this endpoint already has a pair
    }
    local = agent_best_local_candidate(agent, remote->addr.family);
    if (!local) {
      continue;  // no local candidate of that family
    }
    if (agent->candidate_pairs_num >= AGENT_MAX_CANDIDATE_PAIRS) {
      LOGE("candidate pair table is full");
      return;
    }
    agent->candidate_pairs[agent->candidate_pairs_num].local = local;
    agent->candidate_pairs[agent->candidate_pairs_num].remote = remote;
    agent->candidate_pairs[agent->candidate_pairs_num].priority =
        ice_candidate_pair_priority(
            local->priority,
            remote->priority,
            agent->mode == AGENT_MODE_CONTROLLING);
    agent->candidate_pairs[agent->candidate_pairs_num].state = ICE_CANDIDATE_STATE_FROZEN;
    agent->candidate_pairs[agent->candidate_pairs_num].conncheck = 0;
    agent->candidate_pairs[agent->candidate_pairs_num].responses_received = 0;
    agent->candidate_pairs_num++;
  }
  agent_sort_candidate_pairs(agent);
  LOGD("candidate pairs num: %d", agent->candidate_pairs_num);
}

static int agent_has_frozen_candidate_pair(Agent* agent, int start) {
  int i;
  for (i = start; i < agent->candidate_pairs_num; i++) {
    if (agent->candidate_pairs[i].state == ICE_CANDIDATE_STATE_FROZEN) {
      return 1;
    }
  }
  return 0;
}

// 0037: is the next connectivity check due?
//
// The decision is a WALL CLOCK deadline.  `conncheck` is only the "how many
// did we send" counter -- it used to be the time base, and since
// peer_connection_loop() runs once per rendered frame the ICE rhythm
// followed the frame rate (23..31 fps on the device).
//
// When the deadline expires without an answer the interval is doubled
// (RFC 8445 section 7.2.5): 100 -> 200 -> 400 -> ... -> 2000 ms.
static int agent_conncheck_due(Agent* agent, uint32_t now_ms) {
  if (agent->conncheck_requests_sent == 0) {
    return 1;  // the first check goes out right away
  }

  const uint32_t elapsed = now_ms - agent->last_conncheck_sent_ms;
  if (elapsed < agent->conncheck_interval_ms) {
    return 0;
  }

  uint32_t next = agent->conncheck_interval_ms * 2u;
  agent->conncheck_interval_ms =
      (next > AGENT_CONNCHECK_INTERVAL_MAX_MS) ? AGENT_CONNCHECK_INTERVAL_MAX_MS : next;
  return 1;
}

static void agent_conncheck_sent(Agent* agent, uint32_t now_ms) {
  agent->last_conncheck_sent_ms = now_ms;
  agent->conncheck_requests_sent++;
}

int agent_connectivity_check(Agent* agent) {
  char addr_string[ADDRSTRLEN];
  uint8_t buf[1400];
  StunMessage msg;

  if (!agent->nominated_pair) {
    return -1;
  }

  // 0037: a pair that already got a binding response is a SUCCESS, even when
  // the response arrived outside of this call.  The old code only accepted
  // ICE_CANDIDATE_STATE_INPROGRESS here, so a pair that had succeeded made
  // agent_connectivity_check() bail out with -1 forever and the peer
  // connection stayed in "checking" even though ICE was done -- the third
  // libpeer defect, see patches/0037-NOTES.md.
  if (agent->nominated_pair->state == ICE_CANDIDATE_STATE_SUCCEEDED) {
    agent->selected_pair = agent->nominated_pair;
    return 0;
  }

  if (agent->nominated_pair->state != ICE_CANDIDATE_STATE_INPROGRESS) {
    LOGI("nominated pair is not in progress");
    return -1;
  }

  memset(&msg, 0, sizeof(msg));

  const uint32_t now_ms = ports_get_epoch_time();
  if (agent_conncheck_due(agent, now_ms)) {
    addr_to_string(&agent->nominated_pair->remote->addr, addr_string, sizeof(addr_string));
    LOGD("send binding request to remote ip: %s, port: %d", addr_string, agent->nominated_pair->remote->addr.port);
    // 0037: every connectivity check gets its OWN transaction id, so that a
    // binding response can be matched against the check it answers.  The old
    // code used the constant id of stun_msg_create() for all of them.
    agent_create_binding_request(agent, &msg, 1, agent->conncheck_requests_sent + 1);
    if (agent_socket_send(
            agent, &agent->nominated_pair->remote->addr, msg.buf, msg.size) >= 0) {
      agent->last_binding_request_sent_ms = now_ms;
      agent_conncheck_remember_transaction_id(agent, ((StunHeader*)msg.buf)->transaction_id);
      agent_conncheck_sent(agent, now_ms);
      // 0040/F3: stamp the FIRST check that really went out.
      //
      // The engine could not tell "the candidate table was handed over" from
      // "a check actually left", and the delay between the answer going out
      // and the first check is the direct measure of the 0037 race fix
      // (the cloud drops a check that arrives before it knows our ICE
      // credentials).  Before the fix this number was ~3.3 s (100 frames).
      //
      // ★ The criterion really moves: it is a wall clock stamp of the first
      //   transmission, not a counter and not a constant.
      if (agent->first_conncheck_ms == 0) {
        agent->first_conncheck_ms = now_ms;
        LOGI("first connectivity check went out at %u", now_ms);
      }
    }
  }

  // 0037: drain the socket instead of taking ONE datagram per tick.
  agent_recv_drain(agent, buf, sizeof(buf), AGENT_RECV_DRAIN_MAX, AGENT_POLL_TIMEOUT);

  if (agent->nominated_pair->state == ICE_CANDIDATE_STATE_SUCCEEDED) {
    agent->selected_pair = agent->nominated_pair;
    return 0;
  }

  return -1;
}

int agent_maybe_send_consent_request(Agent* agent, uint32_t now_ms) {
  StunMessage msg;

  if (!agent->selected_pair || !agent->selected_pair->remote)
    return 0;
  if (agent->consent_attempts > 0 &&
      (uint32_t)(now_ms - agent->last_consent_request_ms) < CONFIG_ICE_CONSENT_INTERVAL_MS)
    return 0;

  agent->consent_attempts++;
  agent->last_consent_request_ms = now_ms;
  memset(&msg, 0, sizeof(msg));
  agent_create_binding_request(agent, &msg, 0, agent->consent_attempts);
  if (agent_socket_send(agent, &agent->selected_pair->remote->addr, msg.buf, msg.size) < 0)
    return -1;
  agent->consent_requests_sent++;
  agent->last_binding_request_sent_ms = now_ms;
  return 1;
}

int agent_select_candidate_pair(Agent* agent) {
  int i;

  // 0037: a SUCCEEDED pair wins.
  //
  // The peer (ICE-CONTROLLING) nominates the pair it used with
  // USE-CANDIDATE, and that pair is marked SUCCEEDED.  The old loop returned
  // at the first FROZEN pair and overwrote nominated_pair with it, which
  // threw the nomination away -- and agent_connectivity_check() only ever
  // looks at nominated_pair, so ICE stayed in "checking" forever.
  for (i = 0; i < agent->candidate_pairs_num; i++) {
    if (agent->candidate_pairs[i].state == ICE_CANDIDATE_STATE_SUCCEEDED) {
      agent->selected_pair = &agent->candidate_pairs[i];
      agent->nominated_pair = &agent->candidate_pairs[i];
      return 0;
    }
  }

  for (i = 0; i < agent->candidate_pairs_num; i++) {
    if (agent->candidate_pairs[i].state == ICE_CANDIDATE_STATE_FROZEN) {
      // nominate this pair
      agent->nominated_pair = &agent->candidate_pairs[i];
      agent->candidate_pairs[i].conncheck = 0;
      agent->candidate_pairs[i].state = ICE_CANDIDATE_STATE_INPROGRESS;
      // 0037: the nomination gets a WALL CLOCK budget.
      agent->nominated_pair_started_ms = ports_get_epoch_time();
      agent->conncheck_interval_ms = AGENT_CONNCHECK_INTERVAL_BASE_MS;
      agent->conncheck_requests_sent = 0;
      agent->last_conncheck_sent_ms = 0;
      return 0;
    } else if (agent->candidate_pairs[i].state == ICE_CANDIDATE_STATE_INPROGRESS) {
      agent->candidate_pairs[i].conncheck++;
      // 0037: the give-up is a wall clock budget, not a frame count.  The old
      // code counted 1000 loop iterations, which is 16.7 s at 60 fps and
      // 47.6 s at the 21 fps the device really runs at.
      const uint32_t now_ms = ports_get_epoch_time();
      const uint32_t in_progress_ms =
          agent->nominated_pair_started_ms ? (now_ms - agent->nominated_pair_started_ms) : 0;
      if (agent_has_frozen_candidate_pair(agent, i + 1) &&
          in_progress_ms >= AGENT_CONNCHECK_PAIR_SWITCH_MS) {
        agent->candidate_pairs[i].state = ICE_CANDIDATE_STATE_FAILED;
        continue;
      }
      if (in_progress_ms < AGENT_CONNCHECK_PAIR_TIMEOUT_MS) {
        return 0;
      }
      agent->candidate_pairs[i].state = ICE_CANDIDATE_STATE_FAILED;
    } else if (agent->candidate_pairs[i].state == ICE_CANDIDATE_STATE_FAILED) {
    }
  }
  // all candidate pairs are failed
  return -1;
}

int agent_fail_nominated_remote(Agent* agent) {
  int failed = 0;
  IceCandidate* remote;
  int i;

  if (!agent || !agent->nominated_pair || !agent->nominated_pair->remote) {
    return 0;
  }

  remote = agent->nominated_pair->remote;
  for (i = 0; i < agent->candidate_pairs_num; i++) {
    if (agent->candidate_pairs[i].remote == remote) {
      agent->candidate_pairs[i].state = ICE_CANDIDATE_STATE_FAILED;
      failed++;
    }
  }

  agent->selected_pair = NULL;
  agent->nominated_pair = NULL;
  return failed;
}

void agent_get_candidate_pair_stats(Agent* agent, AgentCandidatePairStats* stats) {
  if (!agent || !stats) {
    return;
  }

  memset(stats, 0, sizeof(*stats));
  stats->total = agent->candidate_pairs_num;
  for (int i = 0; i < agent->candidate_pairs_num; i++) {
    switch (agent->candidate_pairs[i].state) {
      case ICE_CANDIDATE_STATE_FROZEN:
        stats->frozen++;
        break;
      case ICE_CANDIDATE_STATE_INPROGRESS:
        stats->inprogress++;
        break;
      case ICE_CANDIDATE_STATE_SUCCEEDED:
        stats->succeeded++;
        break;
      case ICE_CANDIDATE_STATE_FAILED:
        stats->failed++;
        break;
      case ICE_CANDIDATE_STATE_WAITING:
      default:
        break;
    }
  }
}

int agent_get_rtt_ms(Agent* agent) {
  return agent && agent->smoothed_binding_rtt_ms > 0
      ? (int)agent->smoothed_binding_rtt_ms
      : -1;
}
