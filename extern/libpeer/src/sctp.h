#ifndef SCTP_H_
#define SCTP_H_

#include "config.h"
#include "dtls_srtp.h"
#include "utils.h"

typedef enum DecpMsgType {

  DATA_CHANNEL_OPEN = 0x03,
  DATA_CHANNEL_ACK = 0x02,

} DecpMsgType;

typedef enum DataChannelPpid {

  DATA_CHANNEL_PPID_CONTROL = 50,
  DATA_CHANNEL_PPID_DOMSTRING = 51,
  DATA_CHANNEL_PPID_BINARY_PARTIAL = 52,
  DATA_CHANNEL_PPID_BINARY = 53,
  DATA_CHANNEL_PPID_DOMSTRING_PARTIAL = 54

} DataChannelPpid;

#if !CONFIG_USE_USRSCTP

typedef struct SctpChunkParam {
  uint16_t type;
  uint16_t length;
  uint8_t value[0];

} SctpChunkParam;

typedef enum SctpParamType {

  SCTP_PARAM_STATE_COOKIE = 7,

} SctpParamType;

typedef enum SctpHeaderType {

  SCTP_DATA = 0,
  SCTP_INIT = 1,
  SCTP_INIT_ACK = 2,
  SCTP_SACK = 3,
  SCTP_HEARTBEAT = 4,
  SCTP_HEARTBEAT_ACK = 5,
  SCTP_ABORT = 6,
  SCTP_SHUTDOWN = 7,
  SCTP_SHUTDOWN_ACK = 8,
  SCTP_ERROR = 9,
  SCTP_COOKIE_ECHO = 10,
  SCTP_COOKIE_ACK = 11,
  SCTP_ECNE = 12,
  SCTP_CWR = 13,
  SCTP_SHUTDOWN_COMPLETE = 14,
  SCTP_AUTH = 15,
  SCTP_ASCONF_ACK = 128,
  SCTP_ASCONF = 130,
  SCTP_FORWARD_TSN = 192

} SctpHeaderType;

typedef struct SctpChunkCommon {
  uint8_t type;
  uint8_t flags;
  uint16_t length;

} SctpChunkCommon;

typedef struct SctpForwardTsnChunk {
  SctpChunkCommon common;
  uint32_t new_cumulative_tsn;
  uint16_t stream_number;
  uint16_t stream_sequence_number;

} SctpForwardTsnChunk;

typedef struct SctpHeader {
  uint16_t source_port;
  uint16_t destination_port;
  uint32_t verification_tag;
  uint32_t checksum;

} SctpHeader;

typedef struct SctpPacket {
  SctpHeader header;
  uint8_t chunks[0];

} SctpPacket;

typedef struct SctpSackChunk {
  SctpChunkCommon common;
  uint32_t cumulative_tsn_ack;
  uint32_t a_rwnd;
  uint16_t number_of_gap_ack_blocks;
  uint16_t number_of_dup_tsns;
  uint8_t blocks[0];

} SctpSackChunk;

typedef struct SctpDataChunk {
  uint8_t type;
  uint8_t iube;
  uint16_t length;
  uint32_t tsn;
  uint16_t sid;
  uint16_t sqn;
  uint32_t ppid;
  uint8_t data[0];

} SctpDataChunk;

typedef struct SctpInitChunk {
  SctpChunkCommon common;
  uint32_t initiate_tag;
  uint32_t a_rwnd;
  uint16_t number_of_outbound_streams;
  uint16_t number_of_inbound_streams;
  uint32_t initial_tsn;
  SctpChunkParam param[0];

} SctpInitChunk;

typedef struct SctpCookieEchoChunk {
  SctpChunkCommon common;
  uint8_t cookie[0];
} SctpCookieEchoChunk;

#endif

typedef enum SctpDataPpid {

  PPID_CONTROL = 50,
  PPID_STRING = 51,
  PPID_BINARY = 53,
  PPID_STRING_EMPTY = 56,
  PPID_BINARY_EMPTY = 57

} SctpDataPpid;

#define SCTP_MAX_STREAMS 5
#define SCTP_SEND_BUFFER_SIZE (16 * 1024)
#define SCTP_RECEIVE_BUFFER_SIZE (64 * 1024)
#define SCTP_MAX_MESSAGE_SIZE SCTP_RECEIVE_BUFFER_SIZE

typedef struct {
  char label[32];  // Stream label
  uint16_t sid;    // Stream ID
  uint8_t channel_type;
  uint32_t reliability;
  int ack_pending;
} SctpStreamEntry;

/**
 * @brief Why sctp_create_association() failed (0041).
 *
 * Before 0041 the ONLY thing the caller got was a -1 and a errno, and the
 * errno was swallowed by the diagnostic channel (the SCTP channel was never
 * enabled), so the real device logged nothing but "SCTP create socket
 * failed".
 *
 * The values are stable.  sctp_create_fail_reason_name() turns them into
 * the strings the engine reports in getStats:
 *
 *   "none"                    -- no failure (never tried, or it worked)
 *   "runtime_not_initialized" -- sctp_usrsctp_init() never ran
 *   "socket_present"          -- the association already has a socket
 *   "allocation_failed"       -- out of memory
 *   "socket_failed"           -- usrsctp_socket() failed
 *   "setup_failed"            -- bind/connect/nonblocking setup failed
 *   "invalid_context"         -- the caller passed a null sctp/dtls_srtp
 */
typedef enum SctpCreateFailReason {
  SCTP_CREATE_FAIL_NONE = 0,
  SCTP_CREATE_FAIL_RUNTIME_NOT_INITIALIZED = 1,
  SCTP_CREATE_FAIL_SOCKET_PRESENT = 2,
  SCTP_CREATE_FAIL_ALLOCATION_FAILED = 3,
  SCTP_CREATE_FAIL_SOCKET_FAILED = 4,
  SCTP_CREATE_FAIL_SETUP_FAILED = 5,
  SCTP_CREATE_FAIL_INVALID_CONTEXT = 6,
} SctpCreateFailReason;

typedef struct Sctp {
  struct socket* sock;

  int local_port;
  int remote_port;
  int connected;
  /* 0041: the reason of the LAST sctp_create_association() failure. */
  int create_fail_reason;
  uint32_t verification_tag;
  uint32_t tsn;
  uint32_t last_outbound_tsn;
  uint32_t last_outbound_sent_ms;
  uint32_t smoothed_rtt_ms;
  DtlsSrtp* dtls_srtp;
  int stream_count;
  SctpStreamEntry stream_table[SCTP_MAX_STREAMS];
#if CONFIG_USE_USRSCTP
  struct SctpTransport* transport;
  uint8_t* message_buf;
  size_t message_len;
  uint16_t message_sid;
  uint32_t message_ppid;
  int receive_failed;
  int closing;
#endif

  /* datachannel */
  void (*onmessage)(char* msg, size_t len, void* userdata, uint16_t sid);
  void (*onopen)(void* userdata);
  void (*onclose)(void* userdata);

  /* 0014: the peer's DataChannel really opened (its DCEP OPEN arrived).
   *   onopen fires when the SCTP ASSOCIATION comes up, which is BEFORE the
   *   peer's DCEP OPEN -- so onopen cannot carry a sid.  This one fires
   *   when the channel itself opens and carries that channel's sid+label. */
  void (*ondatachannel)(uint16_t sid, const char* label, void* userdata);

  void* userdata;
  // The DTLS read buffer is CONFIG_PACKET_BUFFER_SIZE bytes, so the SCTP
  // packet buffer has to be at least that big: sctp_incoming_data() copies a
  // decrypted datagram into it and a smaller buffer overflows.
  uint8_t buf[CONFIG_PACKET_BUFFER_SIZE];
} Sctp;

int sctp_create_association(Sctp* sctp, DtlsSrtp* dtls_srtp);

/* 0041: the stable name of a SctpCreateFailReason. */
const char* sctp_create_fail_reason_name(int reason);

/**
 * @brief How many SCTP packets are still sitting in the output ring buffer.
 *
 * WHY (0041): sctp_outgoing_data_cb() can only ENQUEUE -- it runs inside
 * usrsctp and has no way to write to the DTLS channel -- and sctp_drain()
 * is what actually writes them out.  A queue that never drains means the
 * INIT never left the device, which is a far sharper symptom than "the
 * association never reached COMM_UP".
 *
 * Returns the queue depth (0 when there is no association yet).
 */
int sctp_get_output_queue_count(Sctp* sctp);

void sctp_set_diagnostics_enabled(int enabled);
void sctp_set_diagnostic_callback(void (*callback)(const char*));

void sctp_destroy_association(Sctp* sctp);

void sctp_usrsctp_init();

void sctp_usrsctp_deinit();

void sctp_tick(Sctp* sctp);

int sctp_is_connected(Sctp* sctp);

int sctp_get_rtt_ms(Sctp* sctp);

void sctp_incoming_data(Sctp* sctp, char* buf, size_t len);

int sctp_outgoing_data(Sctp* sctp, char* buf, size_t len, SctpDataPpid ppid, uint16_t sid);

void sctp_add_stream_mapping(Sctp* sctp, const char* label, uint16_t sid);

void sctp_onmessage(Sctp* sctp, void (*onmessage)(char* msg, size_t len, void* userdata, uint16_t sid));

void sctp_onopen(Sctp* sctp, void (*onopen)(void* userdata));

/**
 * @brief register the callback that fires when the peer's DCEP OPEN arrives.
 *
 * 0014: this is the ONLY moment at which the sid and the label of the
 * remote DataChannel exist -- sctp_record_open() fills the stream table
 * with them.  The association-up callback (sctp_onopen) fires earlier and
 * therefore cannot carry a sid.
 */
void sctp_ondatachannel(Sctp* sctp, void (*ondatachannel)(uint16_t sid, const char* label, void* userdata));

void sctp_onclose(Sctp* sctp, void (*onclose)(void* userdata));

#endif  // SCTP_H_
