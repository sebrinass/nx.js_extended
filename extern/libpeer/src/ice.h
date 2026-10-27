#ifndef ICE_H_
#define ICE_H_

#include <stdlib.h>
#include <time.h>

#include "address.h"
#include "stun.h"

#define ICE_UFRAG_LENGTH 256
#define ICE_UPWD_LENGTH 256

typedef enum IceCandidateState {

  ICE_CANDIDATE_STATE_FROZEN,
  ICE_CANDIDATE_STATE_WAITING,
  ICE_CANDIDATE_STATE_INPROGRESS,
  ICE_CANDIDATE_STATE_SUCCEEDED,
  ICE_CANDIDATE_STATE_FAILED,

} IceCandidateState;

typedef enum IceCandidateType {

  ICE_CANDIDATE_TYPE_HOST,
  ICE_CANDIDATE_TYPE_SRFLX,
  ICE_CANDIDATE_TYPE_PRFLX,
  ICE_CANDIDATE_TYPE_RELAY,

} IceCandidateType;

typedef struct IceCandidate IceCandidate;

struct IceCandidate {
  char foundation[32 + 1];
  int component;
  uint32_t priority;
  char transport[32 + 1];
  IceCandidateType type;
  IceCandidateState state;
  Address addr;
  Address raddr;
};

typedef struct IceCandidatePair IceCandidatePair;

struct IceCandidatePair {
  IceCandidateState state;
  IceCandidate* local;
  IceCandidate* remote;
  int conncheck;
  uint64_t priority;
  // 0040/F1: how many STUN binding responses THIS pair received.
  //
  // WHY per pair and not only globally: the engine can only prove that we
  // HANDED a candidate to the native layer ("we sent it"), never that the
  // cloud ANSWERED it.  The only thing that answers that question is the
  // binding response, and it arrives on one specific pair -- the pair whose
  // remote address it came from.
  //
  // With the 0040 de-duplication there is exactly ONE pair per remote
  // endpoint, so this number IS the "how often did the cloud answer this
  // IP" number.  (Before the de-duplication the same remote address had
  // M pairs -- one per local candidate -- and the answer was counted on
  // the nominated one, which is still the right one: the response is
  // accepted only when its source address matches the nominated pair.)
  //
  // ★ The criterion really moves: it is 0 for every IP the cloud never
  //   answered and >= 1 for every IP it did.
  uint32_t responses_received;
};

void ice_candidate_create(IceCandidate* ice_candidate, int foundation, IceCandidateType type, Address* addr);

void ice_candidate_to_description(IceCandidate* candidate, char* description, int length);

int ice_candidate_from_description(IceCandidate* candidate, char* description, char* end);

int ice_candidate_get_local_address(IceCandidate* candidate, Address* address);

#endif  // ICE_H_
