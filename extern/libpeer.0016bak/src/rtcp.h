#ifndef RTCP_H_
#define RTCP_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __BYTE_ORDER
#define __BIG_ENDIAN 4321
#define __LITTLE_ENDIAN 1234
#elif defined(__SWITCH__) || defined(_WIN32)
#define __BIG_ENDIAN 4321
#define __LITTLE_ENDIAN 1234
#define __BYTE_ORDER __LITTLE_ENDIAN
#elif __APPLE__
#include <machine/endian.h>
#else
#include <endian.h>
#endif

typedef enum RtcpType {

  RTCP_FIR = 192,
  RTCP_SR = 200,
  RTCP_RR = 201,
  RTCP_SDES = 202,
  RTCP_BYE = 203,
  RTCP_APP = 204,
  RTCP_RTPFB = 205,
  RTCP_PSFB = 206,
  RTCP_XR = 207,

} RtcpType;

typedef struct RtcpHeader {
#if __BYTE_ORDER == __BIG_ENDIAN
  uint16_t version : 2;
  uint16_t padding : 1;
  uint16_t rc : 5;
  uint16_t type : 8;
#elif __BYTE_ORDER == __LITTLE_ENDIAN
  uint16_t rc : 5;
  uint16_t padding : 1;
  uint16_t version : 2;
  uint16_t type : 8;
#endif
  uint16_t length : 16;

} RtcpHeader;

typedef struct RtcpReportBlock {
  uint32_t ssrc;
  uint32_t flcnpl;
  uint32_t ehsnr;
  uint32_t jitter;
  uint32_t lsr;
  uint32_t dlsr;

} RtcpReportBlock;

typedef struct RtcpRr {
  RtcpHeader header;
  uint32_t ssrc;
  RtcpReportBlock report_block[1];

} RtcpRr;

typedef struct RtcpFir {
  uint32_t ssrc;
  uint32_t seqnr;

} RtcpFir;

typedef struct RtcpFb {
  RtcpHeader header;
  uint32_t ssrc;
  uint32_t media;
  char fci[1];

} RtcpFb;

typedef struct RtcpPacketView {
  const uint8_t* data;
  size_t size;
  size_t content_size;
  uint8_t type;
  uint8_t count;
} RtcpPacketView;

typedef struct RtcpReceiverStats {
  uint32_t ssrc;
  uint16_t base_sequence;
  uint16_t max_sequence;
  uint32_t bad_sequence;
  uint64_t cycles;
  uint64_t received;
  uint64_t expected_prior;
  uint64_t received_prior;
  uint64_t jitter_q4;
  uint32_t last_arrival_ms;
  uint32_t last_rtp_timestamp;
  uint32_t last_report_ms;
  uint32_t last_sr;
  uint32_t last_sr_received_ms;
  uint8_t initialized;
  uint8_t have_sr;
} RtcpReceiverStats;

#define RTCP_RECEIVER_REPORT_INTERVAL_MS 1000

int rtcp_parse_packet(const uint8_t* packet, size_t size, RtcpPacketView* view);
int rtcp_validate_compound(const uint8_t* packet, size_t size);
int rtcp_receiver_record_rtp(RtcpReceiverStats* stats, const uint8_t* packet,
                             size_t size, uint32_t now_ms);
void rtcp_receiver_record_sr(RtcpReceiverStats* stats, const RtcpPacketView* view,
                              uint32_t now_ms);
int rtcp_receiver_report_due(const RtcpReceiverStats* stats, uint32_t now_ms);
void rtcp_receiver_report_sent(RtcpReceiverStats* stats, uint32_t now_ms, int success);
int rtcp_get_receiver_report(uint8_t* packet, size_t capacity, uint32_t sender_ssrc,
                             const RtcpReceiverStats* stats, uint32_t now_ms,
                             const char* cname);

int rtcp_probe(uint8_t* packet, size_t size);

int rtcp_get_pli(uint8_t* packet, int len, uint32_t sender_ssrc, uint32_t media_ssrc);

int rtcp_get_nack(uint8_t* packet, int len, uint32_t sender_ssrc,
                  uint32_t media_ssrc, uint16_t packet_id, uint16_t bitmask);

int rtcp_get_fir(uint8_t* packet, int len, int* seqnr);

/**
 * @brief build a REMB packet (draft-alvestrand-rmcat-remb-00).
 *
 * REMB = "Receiver Estimated Maximum Bitrate".  It is the feedback a
 * RECEIVER sends to tell the sender "I can take this much" -- without it
 * the sender has to guess and stays at the lowest rate.
 *
 * The packet is an RTCP PSFB with FMT = 15 ("application layer feedback"),
 * PT = 205 (RFC 4585 section 6.1).  Its FCI is the REMB payload, which
 * starts with the four ASCII bytes "REMB":
 *
 *   0                   1                   2                   3
 *   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |V=2|P|  FMT=15 |    PT=205     |           length=5           |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |                  SSRC of packet sender                        |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |            SSRC of media source (0 for REMB)                 |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |                       'R' 'E' 'M' 'B'                        |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |  number of SSRC entries (=1)                                  |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |  n e r e      |  BR Exp       |         Mantissa              |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |                        SSRC of media source                   |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 * n = number of SSRC entries, BR Exp = 6 bit exponent, Mantissa = 18 bit.
 * The bit rate is `mantissa << exponent` bits per second.
 *
 * Returns the number of bytes written (24), or -1 when the arguments are
 * unusable.
 */
int rtcp_get_remb(uint8_t* packet, int len, uint32_t sender_ssrc,
                  uint32_t media_ssrc, uint32_t bitrate_bps);

RtcpRr rtcp_parse_rr(uint8_t* packet);

#endif  // RTCP_H_
