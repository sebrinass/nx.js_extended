#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "address.h"
#include "config.h"
#include "peer_connection.h"
#include "ports.h"
#include "rtp.h"
#include "utils.h"

typedef enum RtpH264Type {

  NALU = 23,
  STAP_A = 24,
  FU_A = 28,

} RtpH264Type;

typedef struct RtpPayloadView {
  uint8_t* payload;
  size_t payload_size;
  uint8_t marker;
  uint8_t payload_type;
  uint16_t seq_number;
  uint32_t timestamp;
  uint32_t ssrc;
} RtpPayloadView;

typedef struct NaluHeader {
  uint8_t type : 5;
  uint8_t nri : 2;
  uint8_t f : 1;
} NaluHeader;

typedef struct FuHeader {
  uint8_t type : 5;
  uint8_t r : 1;
  uint8_t e : 1;
  uint8_t s : 1;
} FuHeader;

#define RTP_PAYLOAD_SIZE (CONFIG_MTU - sizeof(RtpHeader))
#define FU_PAYLOAD_SIZE (CONFIG_MTU - sizeof(RtpHeader) - sizeof(FuHeader) - sizeof(NaluHeader))

static uint16_t read_be16(const uint8_t* data) {
  return (uint16_t)((data[0] << 8) | data[1]);
}

static uint32_t read_be32(const uint8_t* data) {
  return ((uint32_t)data[0] << 24) |
         ((uint32_t)data[1] << 16) |
         ((uint32_t)data[2] << 8) |
         (uint32_t)data[3];
}

static int rtp_parse_payload(uint8_t* packet, size_t size, RtpPayloadView* view) {
  if (!packet || !view || size < 12)
    return -1;

  if ((packet[0] >> 6) != 2)
    return -1;

  const uint8_t padding = (packet[0] & 0x20) != 0;
  const uint8_t extension = (packet[0] & 0x10) != 0;
  const uint8_t csrc_count = packet[0] & 0x0f;
  size_t offset = 12 + ((size_t)csrc_count * 4);
  size_t payload_end = size;

  if (offset > size)
    return -1;

  if (extension) {
    if (offset + 4 > size)
      return -1;

    const size_t extension_size = (size_t)read_be16(packet + offset + 2) * 4;
    offset += 4;
    if (offset + extension_size > size)
      return -1;
    offset += extension_size;
  }

  if (padding) {
    const uint8_t padding_size = packet[size - 1];
    if (padding_size == 0 || padding_size > payload_end - offset)
      return -1;
    payload_end -= padding_size;
  }

  if (offset >= payload_end)
    return -1;

  view->payload = packet + offset;
  view->payload_size = payload_end - offset;
  view->marker = (packet[1] & 0x80) != 0;
  view->payload_type = packet[1] & 0x7f;
  view->seq_number = read_be16(packet + 2);
  view->timestamp = read_be32(packet + 4);
  view->ssrc = read_be32(packet + 8);
  return 0;
}

int rtp_packet_validate(uint8_t* packet, size_t size) {
  if (size < 12)
    return 0;

  if ((packet[0] >> 6) != 2)
    return 0;

  const uint8_t payload_type = packet[1] & 0x7f;
  return ((payload_type < 64) || (payload_type >= 96));
}

uint32_t rtp_get_ssrc(uint8_t* packet) {
  return read_be32(packet + 8);
}

static int rtp_encoder_encode_h264_single(RtpEncoder* rtp_encoder, uint8_t* buf, size_t size) {
  RtpPacket* rtp_packet = (RtpPacket*)rtp_encoder->buf;

  rtp_packet->header.version = 2;
  rtp_packet->header.padding = 0;
  rtp_packet->header.extension = 0;
  rtp_packet->header.csrccount = 0;
  rtp_packet->header.markerbit = 0;
  rtp_packet->header.type = rtp_encoder->type;
  rtp_packet->header.seq_number = htons(rtp_encoder->seq_number++);
  rtp_packet->header.timestamp = htonl(rtp_encoder->timestamp);
  rtp_packet->header.ssrc = htonl(rtp_encoder->ssrc);

  // I frame and P frame
  if ((*buf & 0x1f) == 0x05 || (*buf & 0x1f) == 0x01) {
    rtp_packet->header.markerbit = 1;
    rtp_encoder->timestamp += rtp_encoder->timestamp_increment;
  }
#if 0
  LOGI("markbit: %d, timestamp: %d, nalu type: %d", rtp_packet->header.markerbit, rtp_encoder->timestamp, buf[0] & 0x1f);
#endif

  memcpy(rtp_packet->payload, buf, size);
  rtp_encoder->on_packet(rtp_encoder->buf, size + sizeof(RtpHeader), rtp_encoder->user_data);
  return 0;
}

static int rtp_encoder_encode_h264_fu_a(RtpEncoder* rtp_encoder, uint8_t* buf, size_t size) {
  RtpPacket* rtp_packet = (RtpPacket*)rtp_encoder->buf;

  rtp_packet->header.version = 2;
  rtp_packet->header.padding = 0;
  rtp_packet->header.extension = 0;
  rtp_packet->header.csrccount = 0;
  rtp_packet->header.markerbit = 0;
  rtp_packet->header.type = rtp_encoder->type;
  rtp_packet->header.timestamp = htonl(rtp_encoder->timestamp);
  rtp_packet->header.ssrc = htonl(rtp_encoder->ssrc);
  uint8_t type = buf[0] & 0x1f;
  uint8_t nri = (buf[0] & 0x60) >> 5;
  buf = buf + 1;
  size = size - 1;

  // increase timestamp if I, P frame
  if (type == 0x05 || type == 0x01) {
    rtp_encoder->timestamp += rtp_encoder->timestamp_increment;
  }

  NaluHeader* fu_indicator = (NaluHeader*)rtp_packet->payload;
  FuHeader* fu_header = (FuHeader*)rtp_packet->payload + sizeof(NaluHeader);
  fu_header->s = 1;

  while (size > 0) {
    fu_indicator->type = FU_A;
    fu_indicator->nri = nri;
    fu_indicator->f = 0;
    fu_header->type = type;
    fu_header->r = 0;
    rtp_packet->header.seq_number = htons(rtp_encoder->seq_number++);

    if (size <= FU_PAYLOAD_SIZE) {
      fu_header->e = 1;
      rtp_packet->header.markerbit = 1;
      memcpy(rtp_packet->payload + sizeof(NaluHeader) + sizeof(FuHeader), buf, size);
      rtp_encoder->on_packet(rtp_encoder->buf, size + sizeof(RtpHeader) + sizeof(NaluHeader) + sizeof(FuHeader), rtp_encoder->user_data);
      break;
    }

    fu_header->e = 0;

    memcpy(rtp_packet->payload + sizeof(NaluHeader) + sizeof(FuHeader), buf, FU_PAYLOAD_SIZE);
    rtp_encoder->on_packet(rtp_encoder->buf, CONFIG_MTU, rtp_encoder->user_data);
    size -= FU_PAYLOAD_SIZE;
    buf += FU_PAYLOAD_SIZE;

    fu_header->s = 0;
  }
  return 0;
}

static uint8_t* h264_find_nalu(uint8_t* buf_start, uint8_t* buf_end) {
  uint8_t* p = buf_start + 2;

  while (p < buf_end) {
    if (*(p - 2) == 0x00 && *(p - 1) == 0x00 && *p == 0x01)
      return p + 1;
    p++;
  }

  return buf_end;
}

static int rtp_encoder_encode_h264(RtpEncoder* rtp_encoder, uint8_t* buf, size_t size) {
  uint8_t* buf_end = buf + size;
  uint8_t *pstart, *pend;
  size_t nalu_size;

  for (pstart = h264_find_nalu(buf, buf_end); pstart < buf_end; pstart = pend) {
    pend = h264_find_nalu(pstart, buf_end);
    nalu_size = pend - pstart;

    if (pend != buf_end)
      nalu_size--;

    while (pstart[nalu_size - 1] == 0x00)
      nalu_size--;

    if (nalu_size <= RTP_PAYLOAD_SIZE) {
      rtp_encoder_encode_h264_single(rtp_encoder, pstart, nalu_size);

    } else {
      rtp_encoder_encode_h264_fu_a(rtp_encoder, pstart, nalu_size);
    }
  }

  return 0;
}

static int rtp_encoder_encode_generic(RtpEncoder* rtp_encoder, uint8_t* buf, size_t size) {
  RtpHeader* rtp_header = (RtpHeader*)rtp_encoder->buf;
  rtp_header->version = 2;
  rtp_header->padding = 0;
  rtp_header->extension = 0;
  rtp_header->csrccount = 0;
  rtp_header->markerbit = 0;
  rtp_header->type = rtp_encoder->type;
  rtp_header->seq_number = htons(rtp_encoder->seq_number++);
  rtp_header->timestamp = htonl(rtp_encoder->timestamp);
  rtp_encoder->timestamp += rtp_encoder->timestamp_increment;
  rtp_header->ssrc = htonl(rtp_encoder->ssrc);
  memcpy(rtp_encoder->buf + sizeof(RtpHeader), buf, size);

  rtp_encoder->on_packet(rtp_encoder->buf, size + sizeof(RtpHeader), rtp_encoder->user_data);

  return 0;
}

void rtp_encoder_init(RtpEncoder* rtp_encoder, MediaCodec codec, RtpOnPacket on_packet, void* user_data) {
  rtp_encoder->on_packet = on_packet;
  rtp_encoder->user_data = user_data;
  rtp_encoder->timestamp = 0;
  rtp_encoder->seq_number = 0;

  switch (codec) {
    case CODEC_H264:
      rtp_encoder->type = PT_H264;
      rtp_encoder->ssrc = SSRC_H264;
      rtp_encoder->timestamp_increment = 90000 / 30;  // 30 FPS.
      rtp_encoder->encode_func = rtp_encoder_encode_h264;
      break;
    case CODEC_PCMA:
      rtp_encoder->type = PT_PCMA;
      rtp_encoder->ssrc = SSRC_PCMA;
      rtp_encoder->timestamp_increment = CONFIG_AUDIO_DURATION * 8000 / 1000;
      rtp_encoder->encode_func = rtp_encoder_encode_generic;
      break;
    case CODEC_PCMU:
      rtp_encoder->type = PT_PCMU;
      rtp_encoder->ssrc = SSRC_PCMU;
      rtp_encoder->timestamp_increment = CONFIG_AUDIO_DURATION * 8000 / 1000;
      rtp_encoder->encode_func = rtp_encoder_encode_generic;
      break;
    case CODEC_OPUS:
      rtp_encoder->type = PT_OPUS;
      rtp_encoder->ssrc = SSRC_OPUS;
      rtp_encoder->timestamp_increment = CONFIG_AUDIO_DURATION * 48000 / 1000;
      rtp_encoder->encode_func = rtp_encoder_encode_generic;
      break;
    default:
      break;
  }
}

int rtp_encoder_encode(RtpEncoder* rtp_encoder, const uint8_t* buf, size_t size) {
  return rtp_encoder->encode_func(rtp_encoder, (uint8_t*)buf, size);
}

static void rtp_decoder_reset_fragment(RtpDecoder* rtp_decoder) {
  rtp_decoder->nalu_offset = 0;
  rtp_decoder->fragment_started = 0;
  rtp_decoder->fragment_damaged = 0;
}

static int rtp_decoder_append(RtpDecoder* rtp_decoder, const uint8_t* data, size_t size) {
  if (!rtp_decoder->nalu_buf || rtp_decoder->nalu_offset + size > rtp_decoder->nalu_capacity) {
    rtp_decoder->fragment_damaged = 1;
    LOGW("RTP H264: dropping oversized NALU, partial size=%zu add=%zu capacity=%zu",
         rtp_decoder->nalu_offset, size, rtp_decoder->nalu_capacity);
    return -1;
  }

  memcpy(rtp_decoder->nalu_buf + rtp_decoder->nalu_offset, data, size);
  rtp_decoder->nalu_offset += size;
  return 0;
}

static void rtp_decoder_reset_access_unit(RtpDecoder* rtp_decoder) {
  rtp_decoder->au_offset = 0;
  rtp_decoder->au_timestamp = 0;
  rtp_decoder->au_ssrc = 0;
  rtp_decoder->au_started = 0;
  rtp_decoder->au_has_vcl = 0;
  rtp_decoder->au_has_sps = 0;
  rtp_decoder->au_has_pps = 0;
  rtp_decoder->au_damaged = 0;
  // 0013: the "this access unit contains a key frame" fact does not
  // survive the reset -- by design, one flag per access unit.
  rtp_decoder->au_had_idr = 0;
}

static int rtp_decoder_append_access_unit_bytes(RtpDecoder* rtp_decoder, const uint8_t* data, size_t size) {
  if (!rtp_decoder->au_buf || rtp_decoder->au_offset + size > rtp_decoder->au_capacity) {
    rtp_decoder->au_damaged = 1;
    LOGW("RTP H264: access unit overflow offset=%zu add=%zu capacity=%zu",
         rtp_decoder->au_offset, size, rtp_decoder->au_capacity);
    return -1;
  }

  memcpy(rtp_decoder->au_buf + rtp_decoder->au_offset, data, size);
  rtp_decoder->au_offset += size;
  return 0;
}

static int rtp_decoder_append_access_unit_nalu(RtpDecoder* rtp_decoder, const uint8_t* data, size_t size) {
  static const uint8_t nalu_start_4bytecode[] = {0x00, 0x00, 0x00, 0x01};

  if (!data || size == 0)
    return -1;

  const uint8_t type = data[0] & 0x1f;
  if (type == 7 && size <= sizeof(rtp_decoder->cached_sps)) {
    memcpy(rtp_decoder->cached_sps, data, size);
    rtp_decoder->cached_sps_size = size;
    rtp_decoder->au_has_sps = 1;
  } else if (type == 8 && size <= sizeof(rtp_decoder->cached_pps)) {
    memcpy(rtp_decoder->cached_pps, data, size);
    rtp_decoder->cached_pps_size = size;
    rtp_decoder->au_has_pps = 1;
  }

  if (type == 5) {
    // ★ 0011: an IDR (H.264 NAL type 5) IS a key frame.  Count it here --
    //   where the NAL unit is assembled -- and not where the access unit is
    //   delivered: a key frame that arrives in a damaged access unit is
    //   still a key frame the cloud sent us.
    rtp_decoder->keyframes_received++;
    rtp_decoder->last_keyframe_ms = ports_get_epoch_time();
    // 0013: ... and remember that THIS access unit carries one, so the
    // flush can tell "a key frame survived into a delivered picture"
    // apart from "the cloud sent one, the reassembler threw it away".
    rtp_decoder->au_had_idr = 1;
    if (!rtp_decoder->au_has_sps && rtp_decoder->cached_sps_size > 0) {
      if (rtp_decoder_append_access_unit_bytes(rtp_decoder, nalu_start_4bytecode, sizeof(nalu_start_4bytecode)) != 0 ||
          rtp_decoder_append_access_unit_bytes(rtp_decoder, rtp_decoder->cached_sps, rtp_decoder->cached_sps_size) != 0)
        return -1;
      rtp_decoder->au_has_sps = 1;
    }
    if (!rtp_decoder->au_has_pps && rtp_decoder->cached_pps_size > 0) {
      if (rtp_decoder_append_access_unit_bytes(rtp_decoder, nalu_start_4bytecode, sizeof(nalu_start_4bytecode)) != 0 ||
          rtp_decoder_append_access_unit_bytes(rtp_decoder, rtp_decoder->cached_pps, rtp_decoder->cached_pps_size) != 0)
        return -1;
      rtp_decoder->au_has_pps = 1;
    }
  }

  if (rtp_decoder_append_access_unit_bytes(rtp_decoder, nalu_start_4bytecode, sizeof(nalu_start_4bytecode)) != 0 ||
      rtp_decoder_append_access_unit_bytes(rtp_decoder, data, size) != 0)
    return -1;

  if (type >= 1 && type <= 5)
    rtp_decoder->au_has_vcl = 1;
  rtp_decoder->nal_units_completed++;

  return 0;
}

static void rtp_decoder_flush_access_unit(RtpDecoder* rtp_decoder) {
  if (!rtp_decoder->au_started)
    return;

  if (!rtp_decoder->au_damaged && rtp_decoder->au_has_vcl && rtp_decoder->au_offset > 0) {
    if (rtp_decoder->on_video_packet != NULL) {
      PeerVideoPacket packet = {
        .data = rtp_decoder->au_buf,
        .size = rtp_decoder->au_offset,
        .timestamp = rtp_decoder->au_timestamp,
        .ssrc = rtp_decoder->au_ssrc,
      };
      rtp_decoder->on_video_packet(&packet, rtp_decoder->user_data);
    } else if (rtp_decoder->on_packet != NULL)
      rtp_decoder->on_packet(rtp_decoder->au_buf, rtp_decoder->au_offset, rtp_decoder->user_data);
    rtp_decoder->access_units_completed++;
    // 0013: a key frame that made it INTO a delivered access unit -- the
    // "the picture is whole again" moment of the heal loop.  The gap
    // between keyframes_received and keyframes_delivered is ours (the
    // reassembler dropped it), not the cloud's.
    if (rtp_decoder->au_had_idr) {
      rtp_decoder->keyframes_delivered++;
    }
  } else if (rtp_decoder->au_damaged) {
    rtp_decoder->access_units_dropped++;
  }

  rtp_decoder_reset_access_unit(rtp_decoder);
}

static void rtp_decoder_begin_access_unit(RtpDecoder* rtp_decoder, uint32_t timestamp) {
  if (rtp_decoder->au_started && rtp_decoder->au_timestamp != timestamp) {
    if (rtp_decoder->fragment_started)
      rtp_decoder->au_damaged = 1;
    rtp_decoder_reset_fragment(rtp_decoder);
    rtp_decoder_flush_access_unit(rtp_decoder);
  }

  if (!rtp_decoder->au_started) {
    rtp_decoder->au_started = 1;
    rtp_decoder->au_timestamp = timestamp;
  }
}

static int rtp_decode_h264_stap_a(
    RtpDecoder* rtp_decoder,
    uint8_t* payload,
    size_t payload_size,
    uint32_t timestamp) {
  size_t offset = 1;
  int appended = 0;

  while (offset + 2 <= payload_size) {
    const uint16_t nalu_size = read_be16(payload + offset);
    offset += 2;

    if (nalu_size == 0)
      return -1;

    if (offset + nalu_size > payload_size) {
      LOGW("RTP H264: invalid STAP-A packet");
      return -1;
    }

    const uint8_t nalu_type = payload[offset] & 0x1f;
    if (nalu_type == 0 || nalu_type >= STAP_A)
      return -1;

    (void)timestamp;
    if (rtp_decoder_append_access_unit_nalu(rtp_decoder, payload + offset, nalu_size) == 0)
      appended++;

    offset += nalu_size;
  }

  return appended > 0 && offset == payload_size ? (int)payload_size : -1;
}

static int rtp_decode_h264_fu_a(
    RtpDecoder* rtp_decoder,
    uint8_t* payload,
    size_t payload_size,
    uint32_t timestamp,
    uint8_t marker) {
  static const uint8_t nalu_start_4bytecode[] = {0x00, 0x00, 0x00, 0x01};

  if (payload_size < 3)
    return -1;

  const uint8_t fu_indicator = payload[0];
  const uint8_t fu_header = payload[1];
  const uint8_t start = (fu_header & 0x80) != 0;
  const uint8_t end = (fu_header & 0x40) != 0;
  const uint8_t nal_type = fu_header & 0x1f;
  const uint8_t reconstructed_header = (fu_indicator & 0xe0) | nal_type;

  if ((start && end) || nal_type == 0 || nal_type >= STAP_A)
    return -1;

  if (start) {
    if (rtp_decoder->fragment_started)
      rtp_decoder->au_damaged = 1;
    rtp_decoder->nalu_offset = 0;
    rtp_decoder->fragment_started = 1;
    rtp_decoder->fragment_damaged = 0;

    if (rtp_decoder_append(rtp_decoder, nalu_start_4bytecode, sizeof(nalu_start_4bytecode)) != 0 ||
        rtp_decoder_append(rtp_decoder, &reconstructed_header, 1) != 0 ||
        rtp_decoder_append(rtp_decoder, payload + 2, payload_size - 2) != 0) {
      rtp_decoder_reset_fragment(rtp_decoder);
      return -1;
    }
  } else {
    if (!rtp_decoder->fragment_started ||
        rtp_decoder->nalu_buf[sizeof(nalu_start_4bytecode)] != reconstructed_header)
      return -1;

    if (rtp_decoder_append(rtp_decoder, payload + 2, payload_size - 2) != 0) {
      rtp_decoder_reset_fragment(rtp_decoder);
      return -1;
    }
  }

  if (end) {
    if (!rtp_decoder->fragment_damaged && rtp_decoder->nalu_offset > sizeof(nalu_start_4bytecode)) {
      if (rtp_decoder_append_access_unit_nalu(
              rtp_decoder,
              rtp_decoder->nalu_buf + sizeof(nalu_start_4bytecode),
              rtp_decoder->nalu_offset - sizeof(nalu_start_4bytecode)) != 0)
        rtp_decoder->au_damaged = 1;
    } else {
      rtp_decoder->au_damaged = 1;
    }
    rtp_decoder_reset_fragment(rtp_decoder);
  }

  (void)timestamp;
  (void)marker;

  return (int)payload_size;
}

static int rtp_decode_h264(RtpDecoder* rtp_decoder, uint8_t* buf, size_t size) {
  RtpPayloadView view;
  if (rtp_parse_payload(buf, size, &view) != 0)
    return -1;

  rtp_decoder->packets_received++;

  if (rtp_decoder->packets_received % 600 == 0) {
    LOGD("RTP H264 stats packets=%u gaps=%u reordered=%u late=%u forced=%u nalus=%u auOk=%u auDrop=%u",
         rtp_decoder->packets_received,
         rtp_decoder->sequence_gaps,
         rtp_decoder->reordered_packets,
         rtp_decoder->late_packets_dropped,
         rtp_decoder->forced_sequence_skips,
         rtp_decoder->nal_units_completed,
         rtp_decoder->access_units_completed,
         rtp_decoder->access_units_dropped);
  }

  const int sequence_gap = rtp_decoder->has_last_seq_number &&
      (uint16_t)(rtp_decoder->last_seq_number + 1) != view.seq_number;
  if (sequence_gap) {
    rtp_decoder->sequence_gaps++;
    // ★ 0010: the hole is charged to the access unit that is CURRENT when it
    //   is detected -- and to that ONE only.
    //
    //   The old code did it TWICE: once here (right: the access unit that is
    //   open now is the one that contains the hole) and once again after
    //   rtp_decoder_begin_access_unit() (wrong: by then the access unit that
    //   contains the hole has been flushed -- and the NEXT frame has
    //   started.  That frame is complete and was thrown away for nothing).
    //
    //   One lost packet cost TWO frames; now it costs one.
    rtp_decoder->au_damaged = 1;
    if (rtp_decoder->fragment_started) {
      LOGW("RTP H264: sequence gap inside fragmented NALU (%u -> %u)",
           rtp_decoder->last_seq_number, view.seq_number);
      rtp_decoder_reset_fragment(rtp_decoder);
    }
  }

  rtp_decoder->last_seq_number = view.seq_number;
  rtp_decoder->has_last_seq_number = 1;
  rtp_decoder_begin_access_unit(rtp_decoder, view.timestamp);
  rtp_decoder->au_ssrc = view.ssrc;
  // ★ 0010: the second, unconditional "sequence_gap => au_damaged" is GONE.
  //   It is the one that killed the frame AFTER the hole.

  const uint8_t nalu_type = view.payload[0] & 0x1f;

  if (nalu_type > 0 && nalu_type < STAP_A) {
    rtp_decoder->single_packets++;
    if (rtp_decoder->fragment_started)
      rtp_decoder->au_damaged = 1;
    rtp_decoder_reset_fragment(rtp_decoder);
    if (rtp_decoder_append_access_unit_nalu(rtp_decoder, view.payload, view.payload_size) != 0)
      return -1;
    if (view.marker)
      rtp_decoder_flush_access_unit(rtp_decoder);
    return (int)size;
  }

  switch (nalu_type) {
    case STAP_A:
      rtp_decoder->stap_packets++;
      if (rtp_decoder->fragment_started)
        rtp_decoder->au_damaged = 1;
      rtp_decoder_reset_fragment(rtp_decoder);
      if (rtp_decode_h264_stap_a(rtp_decoder, view.payload, view.payload_size, view.timestamp) < 0) {
        rtp_decoder->au_damaged = 1;
        if (view.marker)
          rtp_decoder_flush_access_unit(rtp_decoder);
        return -1;
      }
      if (view.marker)
        rtp_decoder_flush_access_unit(rtp_decoder);
      return (int)size;
    case FU_A: {
      rtp_decoder->fu_packets++;
      const int result = rtp_decode_h264_fu_a(
          rtp_decoder, view.payload, view.payload_size, view.timestamp, view.marker);
      if (result < 0 || (view.marker && rtp_decoder->fragment_started)) {
        rtp_decoder->au_damaged = 1;
        rtp_decoder_reset_fragment(rtp_decoder);
      }
      if (view.marker)
        rtp_decoder_flush_access_unit(rtp_decoder);
      return result;
    }
    default:
      LOGD("RTP H264: unsupported packetization type %u", nalu_type);
      rtp_decoder->au_damaged = 1;
      rtp_decoder_reset_fragment(rtp_decoder);
      return 0;
  }
}

// ---------------------------------------------------------------------------
// H.265 / HEVC (RFC 7798)
//
// The RTP payload format of HEVC has four packet types (RFC 7798 section
// 4.4).  The Type field of the 2 byte payload header tells them apart:
//
//   1..47  single NAL unit packet  (the payload header IS the NAL header)
//   48     aggregation packet (AP) -- several small NAL units in one packet
//   49     fragmentation unit (FU)  -- one big NAL unit split over packets
//   50     PACI                        (not needed here)
//
// The payload header is the HEVC NAL unit header:
//
//    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
//   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//   |F|   Type    |  LayerId  |   TID   |
//   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
// and the FU adds ONE byte of FU header (S | E | FuType) plus an optional
// 16 bit DONL field.
//
// The parameter sets (VPS = 32, SPS = 33, PPS = 34) travel IN BAND: the
// cloud's offer carries no sprop-parameter-sets, so the decoder caches them
// and puts them back in front of every IRAP picture that needs them.
// ---------------------------------------------------------------------------

#define H265_PAYLOAD_AP 48
#define H265_PAYLOAD_FU 49
#define H265_PAYLOAD_PACI 50

#define H265_NAL_VPS 32
#define H265_NAL_SPS 33
#define H265_NAL_PPS 34

// VCL NAL units are 0..31, the IRAP VCL ones are 16..23.
#define H265_NAL_MAX_VCL 31
#define H265_NAL_IRAP_FIRST 16
#define H265_NAL_IRAP_LAST 23
// A single NAL unit packet carries a NAL unit of type 1..47 (the VCL ones
// plus VPS / SPS / PPS / AUD / SEI / ...).  48..50 are the special
// packetization types and never appear as a NAL unit type.
#define H265_NAL_MAX_SINGLE 47

// The HEVC NAL unit header is two bytes wide.
#define H265_NAL_HEADER_SIZE 2

// Split the 2 byte HEVC payload header into its fields.
static void h265_parse_payload_header(const uint8_t* payload, uint8_t* f, uint8_t* type,
                                       uint8_t* layer_id, uint8_t* tid) {
  *f = (uint8_t)((payload[0] & 0x80) >> 7);
  *type = (uint8_t)((payload[0] & 0x7e) >> 1);
  *layer_id = (uint8_t)(((payload[0] & 0x01) << 5) | (payload[1] >> 3));
  *tid = (uint8_t)(payload[1] & 0x07);
}

// Rebuild a HEVC NAL unit header out of the payload header fields.
static void h265_write_nal_header(uint8_t* out, uint8_t f, uint8_t type, uint8_t layer_id,
                                  uint8_t tid) {
  out[0] = (uint8_t)((f << 7) | ((type & 0x3f) << 1) | ((layer_id >> 5) & 0x01));
  out[1] = (uint8_t)(((layer_id & 0x1f) << 3) | (tid & 0x07));
}

// The NAL unit type of a HEVC NAL unit (the payload header is the NAL
// header for a single NAL unit packet).
static uint8_t h265_nal_type(const uint8_t* nal) {
  return (uint8_t)((nal[0] & 0x7e) >> 1);
}

// Append one complete HEVC NAL unit to the access unit that is being
// assembled.  Mirrors rtp_decoder_append_access_unit_nalu() of the H.264
// path, but with the HEVC parameter sets.
//
// The NAL unit header and the NAL unit payload do not have to be
// contiguous: RFC 7798 section 4.4.1 puts the DONL between them in a
// single NAL unit packet that carries one.
static int rtp_decoder_append_h265_nalu_parts(RtpDecoder* rtp_decoder, const uint8_t* header,
                                               size_t header_size, const uint8_t* body, size_t body_size) {
  static const uint8_t nalu_start_4bytecode[] = {0x00, 0x00, 0x00, 0x01};

  if (!header || header_size < H265_NAL_HEADER_SIZE)
    return -1;

  const uint8_t type = h265_nal_type(header);
  if (type == H265_NAL_VPS && header_size + body_size <= sizeof(rtp_decoder->cached_vps)) {
    memcpy(rtp_decoder->cached_vps, header, header_size);
    if (body_size)
      memcpy(rtp_decoder->cached_vps + header_size, body, body_size);
    rtp_decoder->cached_vps_size = header_size + body_size;
    rtp_decoder->au_has_vps = 1;
  } else if (type == H265_NAL_SPS && header_size + body_size <= sizeof(rtp_decoder->cached_sps_h265)) {
    memcpy(rtp_decoder->cached_sps_h265, header, header_size);
    if (body_size)
      memcpy(rtp_decoder->cached_sps_h265 + header_size, body, body_size);
    rtp_decoder->cached_sps_h265_size = header_size + body_size;
    rtp_decoder->au_has_sps = 1;
  } else if (type == H265_NAL_PPS && header_size + body_size <= sizeof(rtp_decoder->cached_pps_h265)) {
    memcpy(rtp_decoder->cached_pps_h265, header, header_size);
    if (body_size)
      memcpy(rtp_decoder->cached_pps_h265 + header_size, body, body_size);
    rtp_decoder->cached_pps_h265_size = header_size + body_size;
    rtp_decoder->au_has_pps = 1;
  }

  // An IRAP picture needs the parameter sets.  When the cloud sends them
  // in band (which is what it does) they are already part of the access
  // unit; when they are missing we put the cached ones back in front.
  if (type >= H265_NAL_IRAP_FIRST && type <= H265_NAL_IRAP_LAST) {
    // ★ 0011: an IRAP picture IS a key frame (H.265 NAL types 16..23; in
    //   practice 19 = IDR_W_RADL, 20 = IDR_N_LP, 21 = CRA).  Counted here,
    //   where the NAL unit is assembled -- see the note in struct RtpDecoder.
    rtp_decoder->keyframes_received++;
    rtp_decoder->last_keyframe_ms = ports_get_epoch_time();
    // 0013: same as the H.264 side -- this access unit carries a key
    // frame; the flush decides whether it survives.
    rtp_decoder->au_had_idr = 1;
    if (!rtp_decoder->au_has_vps && rtp_decoder->cached_vps_size > 0) {
      if (rtp_decoder_append_access_unit_bytes(rtp_decoder, nalu_start_4bytecode, sizeof(nalu_start_4bytecode)) != 0 ||
          rtp_decoder_append_access_unit_bytes(rtp_decoder, rtp_decoder->cached_vps, rtp_decoder->cached_vps_size) != 0)
        return -1;
      rtp_decoder->au_has_vps = 1;
    }
    if (!rtp_decoder->au_has_sps && rtp_decoder->cached_sps_h265_size > 0) {
      if (rtp_decoder_append_access_unit_bytes(rtp_decoder, nalu_start_4bytecode, sizeof(nalu_start_4bytecode)) != 0 ||
          rtp_decoder_append_access_unit_bytes(rtp_decoder, rtp_decoder->cached_sps_h265, rtp_decoder->cached_sps_h265_size) != 0)
        return -1;
      rtp_decoder->au_has_sps = 1;
    }
    if (!rtp_decoder->au_has_pps && rtp_decoder->cached_pps_h265_size > 0) {
      if (rtp_decoder_append_access_unit_bytes(rtp_decoder, nalu_start_4bytecode, sizeof(nalu_start_4bytecode)) != 0 ||
          rtp_decoder_append_access_unit_bytes(rtp_decoder, rtp_decoder->cached_pps_h265, rtp_decoder->cached_pps_h265_size) != 0)
        return -1;
      rtp_decoder->au_has_pps = 1;
    }
  }

  if (rtp_decoder_append_access_unit_bytes(rtp_decoder, nalu_start_4bytecode, sizeof(nalu_start_4bytecode)) != 0 ||
      rtp_decoder_append_access_unit_bytes(rtp_decoder, header, header_size) != 0)
    return -1;
  if (body_size && rtp_decoder_append_access_unit_bytes(rtp_decoder, body, body_size) != 0)
    return -1;

  if (type <= H265_NAL_MAX_VCL)
    rtp_decoder->au_has_vcl = 1;
  rtp_decoder->nal_units_completed++;

  return 0;
}

// Append one complete HEVC NAL unit (header and payload are contiguous).
static int rtp_decoder_append_h265_nalu(RtpDecoder* rtp_decoder, const uint8_t* data, size_t size) {
  if (!data || size < H265_NAL_HEADER_SIZE)
    return -1;
  return rtp_decoder_append_h265_nalu_parts(rtp_decoder, data, H265_NAL_HEADER_SIZE,
                                            data + H265_NAL_HEADER_SIZE, size - H265_NAL_HEADER_SIZE);
}

// RFC 7798 section 4.4.3 -- fragmentation unit.
//
//   PayloadHdr (2 bytes, Type = 49) | FU header (1 byte) | DONL (cond) | payload
//
// The FU header is S | E | FuType.  The DONL field is present only when the
// SDP says sprop-max-don-diff > 0 and only in the first fragment (S = 1).
static int rtp_decode_h265_fu(RtpDecoder* rtp_decoder, uint8_t* payload, size_t payload_size,
                               uint32_t timestamp, uint8_t marker) {
  static const uint8_t nalu_start_4bytecode[] = {0x00, 0x00, 0x00, 0x01};

  if (payload_size < H265_NAL_HEADER_SIZE + 1)
    return -1;

  uint8_t f = 0, payload_type = 0, layer_id = 0, tid = 0;
  h265_parse_payload_header(payload, &f, &payload_type, &layer_id, &tid);

  const uint8_t fu_header = payload[H265_NAL_HEADER_SIZE];
  const uint8_t start = (fu_header & 0x80) != 0;
  const uint8_t end = (fu_header & 0x40) != 0;
  const uint8_t nal_type = (uint8_t)(fu_header & 0x3f);

  if (start && end)
    return -1;
  if (nal_type == 0 || nal_type > H265_NAL_MAX_SINGLE)
    return -1;

  // Skip the optional DONL field.  It is only there in the first fragment
  // and only when the peer announced sprop-max-don-diff > 0.
  size_t offset = H265_NAL_HEADER_SIZE + 1;
  if (rtp_decoder->h265_has_donl && start) {
    if (payload_size < offset + 2)
      return -1;
    offset += 2;
  }

  if (start) {
    uint8_t nal_header[H265_NAL_HEADER_SIZE];
    h265_write_nal_header(nal_header, f, nal_type, layer_id, tid);

    if (rtp_decoder->fragment_started)
      rtp_decoder->au_damaged = 1;
    rtp_decoder->nalu_offset = 0;
    rtp_decoder->fragment_started = 1;
    rtp_decoder->fragment_damaged = 0;

    if (rtp_decoder_append(rtp_decoder, nalu_start_4bytecode, sizeof(nalu_start_4bytecode)) != 0 ||
        rtp_decoder_append(rtp_decoder, nal_header, sizeof(nal_header)) != 0 ||
        rtp_decoder_append(rtp_decoder, payload + offset, payload_size - offset) != 0) {
      rtp_decoder_reset_fragment(rtp_decoder);
      return -1;
    }
  } else {
    if (!rtp_decoder->fragment_started)
      return -1;
    // The reconstructed NAL unit header has to stay the same for every
    // fragment of the same NAL unit: the first byte carries F, the NAL
    // unit type and the top bit of the layer id.
    if (rtp_decoder->nalu_offset <= sizeof(nalu_start_4bytecode) + H265_NAL_HEADER_SIZE)
      return -1;
    if (rtp_decoder->nalu_buf[sizeof(nalu_start_4bytecode)] !=
        (uint8_t)((f << 7) | ((nal_type & 0x3f) << 1) | ((layer_id >> 5) & 0x01)))
      return -1;

    if (rtp_decoder_append(rtp_decoder, payload + offset, payload_size - offset) != 0) {
      rtp_decoder_reset_fragment(rtp_decoder);
      return -1;
    }
  }

  if (end) {
    if (!rtp_decoder->fragment_damaged &&
        rtp_decoder->nalu_offset > sizeof(nalu_start_4bytecode) + H265_NAL_HEADER_SIZE) {
      // The NAL unit header that was rebuilt out of the payload header is
      // part of the NAL unit, so it goes into the access unit as well.
      if (rtp_decoder_append_h265_nalu(rtp_decoder,
                                       rtp_decoder->nalu_buf + sizeof(nalu_start_4bytecode),
                                       rtp_decoder->nalu_offset - sizeof(nalu_start_4bytecode)) != 0)
        rtp_decoder->au_damaged = 1;
    } else {
      rtp_decoder->au_damaged = 1;
    }
    rtp_decoder_reset_fragment(rtp_decoder);
  }

  (void)timestamp;
  (void)marker;

  return (int)payload_size;
}

// RFC 7798 section 4.4.2 -- aggregation packet.
//
//   PayloadHdr (2 bytes, Type = 48) | aggregation unit | aggregation unit | ...
//
// The first aggregation unit may carry a 16 bit DONL field, every following
// one a 8 bit DOND field.  Both are only there when the peer announced
// sprop-max-don-diff > 0.  Every aggregation unit is a 16 bit NAL unit size
// (the NAL unit header included) followed by the NAL unit itself.
static int rtp_decode_h265_ap(RtpDecoder* rtp_decoder, uint8_t* payload, size_t payload_size,
                               uint32_t timestamp) {
  size_t offset = H265_NAL_HEADER_SIZE;
  int appended = 0;

  (void)timestamp;

  while (offset < payload_size) {
    if (rtp_decoder->h265_has_donl) {
      // The first aggregation unit carries the DONL, the others the DOND.
      const size_t don_size = (appended == 0) ? 2 : 1;
      if (offset + don_size > payload_size) {
        LOGW("RTP H265: truncated DON field in AP");
        return -1;
      }
      offset += don_size;
    }

    if (offset + 2 > payload_size) {
      LOGW("RTP H265: truncated AP size field");
      return -1;
    }

    const uint16_t nalu_size = read_be16(payload + offset);
    offset += 2;

    if (nalu_size < H265_NAL_HEADER_SIZE) {
      LOGW("RTP H265: invalid AP NALU size %u", nalu_size);
      return -1;
    }

    if (offset + nalu_size > payload_size) {
      LOGW("RTP H265: invalid AP packet");
      return -1;
    }

    const uint8_t nalu_type = h265_nal_type(payload + offset);
    if (nalu_type == H265_PAYLOAD_AP || nalu_type == H265_PAYLOAD_FU || nalu_type == H265_PAYLOAD_PACI) {
      LOGW("RTP H265: nested packetization in AP");
      return -1;
    }

    if (rtp_decoder_append_h265_nalu(rtp_decoder, payload + offset, nalu_size) == 0)
      appended++;

    offset += nalu_size;
  }

  return appended > 0 ? (int)payload_size : -1;
}

// The H.265 video depacketizer.
static int rtp_decode_h265(RtpDecoder* rtp_decoder, uint8_t* buf, size_t size) {
  RtpPayloadView view;
  if (rtp_parse_payload(buf, size, &view) != 0)
    return -1;

  rtp_decoder->packets_received++;

  const int sequence_gap = rtp_decoder->has_last_seq_number &&
      (uint16_t)(rtp_decoder->last_seq_number + 1) != view.seq_number;
  if (sequence_gap) {
    rtp_decoder->sequence_gaps++;
    // ★ 0010: the hole is charged to the access unit that is CURRENT when it
    //   is detected -- and to that ONE only.
    //
    //   The old code did it TWICE: once here (right: the access unit that is
    //   open now is the one that contains the hole) and once again after
    //   rtp_decoder_begin_access_unit() (wrong: by then the access unit that
    //   contains the hole has been flushed -- and the NEXT frame has
    //   started.  That frame is complete and was thrown away for nothing).
    //
    //   One lost packet cost TWO frames; now it costs one.
    rtp_decoder->au_damaged = 1;
    if (rtp_decoder->fragment_started) {
      LOGW("RTP H265: sequence gap inside fragmented NALU (%u -> %u)",
           rtp_decoder->last_seq_number, view.seq_number);
      rtp_decoder_reset_fragment(rtp_decoder);
    }
  }

  rtp_decoder->last_seq_number = view.seq_number;
  rtp_decoder->has_last_seq_number = 1;
  rtp_decoder_begin_access_unit(rtp_decoder, view.timestamp);
  rtp_decoder->au_ssrc = view.ssrc;
  // ★ 0010: the second, unconditional "sequence_gap => au_damaged" is GONE.
  //   It is the one that killed the frame AFTER the hole.

  if (view.payload_size < H265_NAL_HEADER_SIZE)
    return -1;

  uint8_t f = 0, payload_type = 0, layer_id = 0, tid = 0;
  h265_parse_payload_header(view.payload, &f, &payload_type, &layer_id, &tid);

  switch (payload_type) {
    case H265_PAYLOAD_AP:
      rtp_decoder->h265_ap_packets++;
      if (rtp_decoder->fragment_started)
        rtp_decoder->au_damaged = 1;
      rtp_decoder_reset_fragment(rtp_decoder);
      if (rtp_decode_h265_ap(rtp_decoder, view.payload, view.payload_size, view.timestamp) < 0) {
        rtp_decoder->au_damaged = 1;
        if (view.marker)
          rtp_decoder_flush_access_unit(rtp_decoder);
        return -1;
      }
      if (view.marker)
        rtp_decoder_flush_access_unit(rtp_decoder);
      return (int)size;
    case H265_PAYLOAD_FU: {
      rtp_decoder->h265_fu_packets++;
      const int result = rtp_decode_h265_fu(rtp_decoder, view.payload, view.payload_size,
                                            view.timestamp, view.marker);
      if (result < 0 || (view.marker && rtp_decoder->fragment_started)) {
        rtp_decoder->au_damaged = 1;
        rtp_decoder_reset_fragment(rtp_decoder);
      }
      if (view.marker)
        rtp_decoder_flush_access_unit(rtp_decoder);
      return result;
    }
    case H265_PAYLOAD_PACI:
      LOGD("RTP H265: PACI packet, ignored");
      return 0;
    default:
      break;
  }

  // A single NAL unit packet: the payload header IS the NAL unit header,
  // so the whole payload is the NAL unit.
  if (payload_type == 0 || payload_type > H265_NAL_MAX_SINGLE) {
    LOGD("RTP H265: unsupported packetization type %u", payload_type);
    rtp_decoder->au_damaged = 1;
    rtp_decoder_reset_fragment(rtp_decoder);
    return 0;
  }

  rtp_decoder->h265_single_packets++;
  if (rtp_decoder->fragment_started)
    rtp_decoder->au_damaged = 1;
  rtp_decoder_reset_fragment(rtp_decoder);
  // RFC 7798 section 4.4.1: the DONL field sits between the payload header
  // and the NAL unit payload.  It is only there when the SDP announces
  // "sprop-max-don-diff > 0".
  if (rtp_decoder->h265_has_donl) {
    if (view.payload_size < H265_NAL_HEADER_SIZE + 2)
      return -1;
    if (rtp_decoder_append_h265_nalu_parts(rtp_decoder, view.payload, H265_NAL_HEADER_SIZE,
                                            view.payload + H265_NAL_HEADER_SIZE + 2,
                                            view.payload_size - H265_NAL_HEADER_SIZE - 2) != 0)
      return -1;
  } else if (rtp_decoder_append_h265_nalu(rtp_decoder, view.payload, view.payload_size) != 0) {
    return -1;
  }
  if (view.marker)
    rtp_decoder_flush_access_unit(rtp_decoder);
  return (int)size;
}

// The video depacketizer used when both codecs are on the table: the RTP
// payload type decides which one runs.  The payload types are NOT fixed --
// the peer numbers them in its offer -- so the owner of the connection
// hands the H.265 one over (h265_payload_type == 0 -> H.264 only).
static int rtp_decode_video(RtpDecoder* rtp_decoder, uint8_t* buf, size_t size) {
  RtpPayloadView view;
  if (rtp_parse_payload(buf, size, &view) != 0)
    return -1;

  if (rtp_decoder->h265_payload_type != 0 && view.payload_type == rtp_decoder->h265_payload_type)
    return rtp_decode_h265(rtp_decoder, buf, size);

  return rtp_decode_h264(rtp_decoder, buf, size);
}

// 0016: strip the fixed five byte RED wrapper that precedes the opus frame.
// Audio path only -- the video depacketizers below never call this.
//
// Ground truth from a capture on the real Switch (12 audio packets, 7 of them
// undecodable): every packet the opus decoder refused starts with
//
//   ef 0f 00 LL 6f  <opus TOC> ...
//
// and the ones that decode start at the TOC with no wrapper at all.  Parsing
// each of the 7 failing packets at every offset 0..8 yields a valid TOC
// (config=31 / stereo / frame count 1 -- the same triple the working packets
// give at offset 0) at offset 5 and nowhere else.  p[4] == 0x6f is the
// F0/PT111 header of the RED primary block, i.e. "opus 111 follows", so the
// bare opus packet begins at p[5].
//
// The shape is hard-coded on purpose: no generic RFC 2198 chain walker, no
// fallback guess.  A packet that does not match every byte is forwarded
// untouched -- "cannot decode" is the old behaviour, corrupting a packet that
// does decode is not acceptable.
static int rtp_red5_reports = 0;
// 0017: 前三个包的一次性样本可能全是裸 opus（pt 111），那样就看不到 RED 的
// 处理结果了。这两个累计量每 200 个 RED 包打一行，保证日志里一定有 RED 的
// 「进来多少 / 剥掉多少 / 原样放行多少」，也能顺手暴露「剥不动」的形状。
static int rtp_red5_red_total = 0;
static int rtp_red5_red_stripped = 0;
static int rtp_red5_red_kept = 0;

// 0017: 0016 剥对了「头」，但交错了「数据」。
//
// 0016 剥掉固定 5 字节后把 payload+5 .. end 整段交给了 opus。那一段并不是
// primary block —— RFC 2198 的数据区是 [冗余块1][冗余块2]...[primary]，所以
// payload+5 里前 L 个字节是**上一个 20ms 的重传**，primary 排在它后面。
// 12 个真机包对账（payload type 63 = RED）：
//
//   payload                        hdr  block1  primary   0016 交出  正确
//   ef 0f 00 03 6f fc ff fe fc..   (5)   (3)      (3)        6        3
//   ef 0f 00 a1 6f fc ..           (5)   (161)    (161)      322      161
//   ef 0f 01 0b 6f fc ..           (5)   (267)    (215)      482      215
//   ef 0f 00 d7 6f fc ..           (5)   (215)    (161)      376      161
//
// opus 收到 322 字节就按 TOC 把 322 字节当**一帧**解（CELT 20ms 单帧允许到
// 1275 字节，所以它不报错），解出来的东西是失真的：幅度冲到 1.38 被
// audio-graph 的 clampf(-1,1) 削成方波，听感就是「洞穴里的破音 + 勉强能听出
// 旋律」。帧数、时长全对，所以统计上一直像「解码没问题」。
//
// 正确的切法：block1 的长度在那 24 bit 的低 10 bit 里
// （payload[2] 的低 2 位 + payload[3]），primary 从 hdr+block1_len 开始。
//
// 冗余块本身是有用的（它就是丢包的免费重传），但要拿它得先有 jitter buffer
// 和「这个序列号真的没到」的判定 —— 那是 0018 的事。这里只保证交出对的
// primary，不猜、不拼。
// 0018: 冗余块回填（修改5）：除 primary 外再多交出**第一个冗余块**的指针、
// 长度与时间戳偏移（24 bit 块头：高 14 bit = offset，低 10 bit = length），
// 供上层 jitter buffer 在"这个序列号真的没到"时直接取用。0017 的 primary
// 解析语义一字不动。剥离失败时三个 out_* 一律置空。
static int rtp_red5_strip(uint8_t pt, uint8_t* payload, size_t size, uint8_t** out_payload,
                          size_t* out_size, const uint8_t** out_redundant,
                          size_t* out_redundant_len, uint32_t* out_redundant_ts_offset) {
  if (out_redundant != NULL)
    *out_redundant = NULL;
  if (out_redundant_len != NULL)
    *out_redundant_len = 0;
  if (out_redundant_ts_offset != NULL)
    *out_redundant_ts_offset = 0;
  if (payload == NULL || out_payload == NULL || out_size == NULL)
    return 0;
  // 0017: 只有 SDP 协商成 RED 的包才允许动。0016 只看 payload[0]，而裸 opus
  // 的 TOC（0xfc）最高位也是 1 —— 猜错就会把一个好包从中间劈开。
  if (pt != 63)
    return 0;
  // 至少一个 primary 头 + 一个数据字节。
  if (size < 6)
    return 0;

  // 走完 block header 区：F=1 的块占 4 字节（offset 14 bit + length 10 bit），
  // F=0 的是 primary，只占 1 字节，数据排在最后。
  size_t pos = 0;
  size_t redundant_bytes = 0;
  int redundant_blocks = 0;
  size_t first_red_len = 0;
  uint32_t first_red_ts_offset = 0;
  while (pos < size) {
    const uint8_t b = payload[pos];
    if ((b & 0x80) != 0) {
      if (pos + 4 > size)
        return 0;
      if ((b & 0x7f) != 111)
        return 0;  // 冗余块必须是 opus，否则形状不认识，交给毒包闸门丢弃
      // 24 bit 块头：高 14 bit = offset（时间戳偏移），低 10 bit = length。
      const uint32_t ts_offset =
          ((uint32_t)payload[pos + 1] << 6) | ((uint32_t)payload[pos + 2] >> 2);
      const size_t block_len =
          (((size_t)(payload[pos + 2] & 0x03)) << 8) | (size_t)payload[pos + 3];
      if (redundant_blocks == 0) {
        first_red_ts_offset = ts_offset;
        first_red_len = block_len;
      }
      redundant_bytes += block_len;
      pos += 4;
      if (++redundant_blocks > 4)
        return 0;
    } else {
      if ((b & 0x7f) != 111)
        return 0;
      pos += 1;
      break;
    }
  }

  // ★ 0017 的全部关键：primary 在「header 区 + 所有冗余数据」之后。
  if (pos + redundant_bytes >= size)
    return 0;
  *out_payload = payload + pos + redundant_bytes;
  *out_size = size - pos - redundant_bytes;

  // 拿出来的东西还得像个 opus 包：帧数 code 3 要求后续计数字节是 1..3。
  if (((*out_payload)[0] & 0x03) == 0x03) {
    if (*out_size < 2 || (*out_payload)[1] == 0 || ((*out_payload)[1] & 0x3f) > 3)
      return 0;
  }

  // 0018: 第一冗余块就在数据区开头（[冗余块1][冗余块2]...[primary]）。
  // primary 校验全过才交出，免得半成品指针流出去。
  if (redundant_blocks > 0 && out_redundant != NULL && out_redundant_len != NULL) {
    *out_redundant = payload + pos;
    *out_redundant_len = first_red_len;
    if (out_redundant_ts_offset != NULL)
      *out_redundant_ts_offset = first_red_ts_offset;
  }
  return 1;
}

static int rtp_decode_generic(RtpDecoder* rtp_decoder, uint8_t* buf, size_t size) {
  RtpPayloadView view;
  if (rtp_parse_payload(buf, size, &view) != 0) {
    // 0012/B: say WHY nothing was produced.  rtp_parse_payload() itself
    // only answers "no"; the short-vs-shape split is re-derived here
    // (the raw shape is still in buf).
    if (size < 12) {
      rtp_decoder->audio_fail_short++;
    } else {
      rtp_decoder->audio_fail_shape++;
    }
    return -1;
  }

  rtp_decoder->audio_packets_parsed++;
  if (buf != NULL) {
    const uint8_t pt = ((RtpHeader*)buf)->type;
    if (pt == 63) {
      // 0012/B: counting RED separately makes "the sender speaks RED only"
      // visible instead of silent.
      rtp_decoder->audio_red_packets++;
    } else if (pt == PT_OPUS) {
      rtp_decoder->audio_opus_packets++;
    }
  }

  // 0016/0017: the opus decoder has to see the bare frame, not the RED wrapper.
  // 0018: 剥离失败不再"原样放行"，见下面的毒包闸门（修改5）。
  uint8_t* audio_payload = view.payload;
  size_t audio_size = view.payload_size;
  const uint8_t audio_pt = (buf != NULL) ? ((RtpHeader*)buf)->type : 0xff;
  const uint8_t* red_payload = NULL;
  size_t red_len = 0;
  uint32_t red_ts_offset = 0;
  const int stripped = rtp_red5_strip(audio_pt, view.payload, view.payload_size, &audio_payload,
                                      &audio_size, &red_payload, &red_len, &red_ts_offset);
  // One-shot proof for the first three packets: stderr, never stdout -- libnx
  // binds stdout to the console renderer, see the note in src/utils.h.
  if (rtp_red5_reports < 3) {
    rtp_red5_reports++;
    fprintf(stderr, "[red5] pt=%d in=%d out=%d stripped=%d\n", (int)audio_pt,
            (int)view.payload_size, (int)audio_size, stripped);
  }
  // 0017: rolling verdict -- 每 200 个 RED 包一行，前三个样本落在裸 opus 上
  // 也照样能判断剥离有没有生效。last in/out 是最近一次的实测量。
  // 0018: "kept" 现在计的是**被毒包闸门丢掉**的包（修改5）。
  if (audio_pt == 63) {
    rtp_red5_red_total++;
    if (stripped)
      rtp_red5_red_stripped++;
    else
      rtp_red5_red_kept++;
    if ((rtp_red5_red_total % 200) == 0) {
      fprintf(stderr, "[red5] red total=%d stripped=%d dropped=%d unstrippedDropped=%d last in=%d out=%d\n",
              rtp_red5_red_total, rtp_red5_red_stripped, rtp_red5_red_kept,
              rtp_decoder->red_unstripped_dropped,
              (int)view.payload_size, (int)audio_size);
    }
  }

  // 0018: 毒包闸门（修改5）：PT==63 但剥离失败 → 整包丢弃。把解不开的 RED
  // 交给 opus 只会得到 0017 记录的那种失真（几百字节被当一帧解、削成方波）。
  if (audio_pt == 63 && !stripped) {
    rtp_decoder->red_unstripped_dropped++;
    return (int)size;
  }

  if (rtp_decoder->on_audio_packet != NULL) {
    // 0018: 冗余块回填（修改5）。data 与 redundant 同指网络缓冲，
    // **回调返回即失效**，消费者必须对两块各自 memcpy（双 memcpy）。
    PeerAudioPacket packet = {
      .data = audio_payload,
      .len = audio_size,
      .timestamp = view.timestamp,
      .ssrc = rtp_get_ssrc(buf),
      .sequence = read_be16(buf + 2),
      .payload_type = ((RtpHeader*)buf)->type,
      .marker = view.marker,
      .redundant = red_payload,
      .redundant_len = red_len,
      .redundant_ts_offset = red_ts_offset,
      .red_stripped = stripped,
    };
    rtp_decoder->on_audio_packet(&packet, rtp_decoder->user_data);
    // 0012/B: "produced" is the callback really firing -- the difference
    // parsed-minus-produced is the "no consumer registered" verdict.
    rtp_decoder->audio_units_produced++;
  } else if (rtp_decoder->on_packet != NULL)
    rtp_decoder->on_packet(audio_payload, audio_size, rtp_decoder->user_data);
  // even if there is no callback set, assume everything is ok for caller and do not return an error
  return (int)size;
}

void rtp_decoder_set_audio_callback(RtpDecoder* rtp_decoder, RtpOnAudioPacket on_packet) {
  if (rtp_decoder)
    rtp_decoder->on_audio_packet = on_packet;
}

void rtp_decoder_set_video_callback(RtpDecoder* rtp_decoder, RtpOnVideoPacket on_packet) {
  if (rtp_decoder)
    rtp_decoder->on_video_packet = on_packet;
}

void rtp_decoder_init(RtpDecoder* rtp_decoder, MediaCodec codec, RtpOnPacket on_packet, void* user_data) {
  memset(rtp_decoder, 0, sizeof(*rtp_decoder));
  rtp_decoder->on_packet = on_packet;
  rtp_decoder->user_data = user_data;

  switch (codec) {
    case CODEC_H264:
      rtp_decoder->type = PT_H264;
      rtp_decoder->nalu_capacity = CONFIG_MAX_NALU_SIZE;
      rtp_decoder->nalu_buf = malloc(rtp_decoder->nalu_capacity);
      rtp_decoder->au_capacity = CONFIG_MAX_NALU_SIZE;
      rtp_decoder->au_buf = malloc(rtp_decoder->au_capacity);
      rtp_decoder->reorder_buf = malloc(RTP_REORDER_WINDOW * RTP_REORDER_PACKET_CAPACITY);
      if (!rtp_decoder->nalu_buf || !rtp_decoder->au_buf || !rtp_decoder->reorder_buf) {
        LOGE("RTP H264: failed to allocate decoder buffers");
        free(rtp_decoder->nalu_buf);
        free(rtp_decoder->au_buf);
        free(rtp_decoder->reorder_buf);
        rtp_decoder->nalu_buf = NULL;
        rtp_decoder->au_buf = NULL;
        rtp_decoder->reorder_buf = NULL;
        rtp_decoder->nalu_capacity = 0;
        rtp_decoder->au_capacity = 0;
        break;
      }
      rtp_decoder->decode_func = rtp_decode_h264;
      break;
    case CODEC_H265:
      rtp_decoder->type = PT_H265;
      rtp_decoder->nalu_capacity = CONFIG_MAX_NALU_SIZE;
      rtp_decoder->nalu_buf = malloc(rtp_decoder->nalu_capacity);
      rtp_decoder->au_capacity = CONFIG_MAX_NALU_SIZE;
      rtp_decoder->au_buf = malloc(rtp_decoder->au_capacity);
      rtp_decoder->reorder_buf = malloc(RTP_REORDER_WINDOW * RTP_REORDER_PACKET_CAPACITY);
      if (!rtp_decoder->nalu_buf || !rtp_decoder->au_buf || !rtp_decoder->reorder_buf) {
        LOGE("RTP H265: failed to allocate decoder buffers");
        free(rtp_decoder->nalu_buf);
        free(rtp_decoder->au_buf);
        free(rtp_decoder->reorder_buf);
        rtp_decoder->nalu_buf = NULL;
        rtp_decoder->au_buf = NULL;
        rtp_decoder->reorder_buf = NULL;
        rtp_decoder->nalu_capacity = 0;
        rtp_decoder->au_capacity = 0;
        break;
      }
      // The default is the payload type this build advertises.  The peer
      // numbers the H.265 stream in ITS offer, so the owner of the
      // connection has to hand the real number over (see
      // peer_connection_set_video_h265_payload_type()).
      rtp_decoder->h265_payload_type = PT_H265;
      rtp_decoder->decode_func = rtp_decode_video;
      break;
    case CODEC_PCMA:
      rtp_decoder->type = PT_PCMA;
      rtp_decoder->decode_func = rtp_decode_generic;
      break;
    case CODEC_PCMU:
      rtp_decoder->type = PT_PCMU;
      rtp_decoder->decode_func = rtp_decode_generic;
      break;
    case CODEC_OPUS:
      rtp_decoder->type = PT_OPUS;
      rtp_decoder->decode_func = rtp_decode_generic;
      break;
    default:
      break;
  }
}

void rtp_decoder_cleanup(RtpDecoder* rtp_decoder) {
  if (!rtp_decoder)
    return;

  free(rtp_decoder->nalu_buf);
  free(rtp_decoder->au_buf);
  free(rtp_decoder->reorder_buf);
  rtp_decoder->nalu_buf = NULL;
  rtp_decoder->au_buf = NULL;
  rtp_decoder->reorder_buf = NULL;
  rtp_decoder->nalu_capacity = 0;
  rtp_decoder->nalu_offset = 0;
  rtp_decoder->au_capacity = 0;
  rtp_decoder_reset_access_unit(rtp_decoder);
  rtp_decoder->fragment_started = 0;
  rtp_decoder->fragment_damaged = 0;
}

static int rtp_decoder_drain_reorder_buffer(RtpDecoder* rtp_decoder) {
  int result = 0;

  while (rtp_decoder->reorder_has_expected_sequence) {
    const size_t slot = rtp_decoder->reorder_expected_sequence % RTP_REORDER_WINDOW;
    if (!rtp_decoder->reorder_used[slot] ||
        rtp_decoder->reorder_sequences[slot] != rtp_decoder->reorder_expected_sequence) {
      break;
    }

    uint8_t* packet = rtp_decoder->reorder_buf + (slot * RTP_REORDER_PACKET_CAPACITY);
    result = rtp_decoder->decode_func(rtp_decoder, packet, rtp_decoder->reorder_sizes[slot]);
    rtp_decoder->reorder_used[slot] = 0;
    if (rtp_decoder->reorder_buffered_packets > 0)
      rtp_decoder->reorder_buffered_packets--;
    rtp_decoder->reorder_expected_sequence++;
  }

  return result;
}

static int rtp_decoder_expected_packet_buffered(const RtpDecoder* rtp_decoder) {
  const size_t slot = rtp_decoder->reorder_expected_sequence % RTP_REORDER_WINDOW;
  return rtp_decoder->reorder_used[slot] &&
         rtp_decoder->reorder_sequences[slot] == rtp_decoder->reorder_expected_sequence;
}

static void rtp_decoder_update_gap_timer(RtpDecoder* rtp_decoder, uint32_t now_ms) {
  if (rtp_decoder->reorder_buffered_packets == 0 ||
      rtp_decoder_expected_packet_buffered(rtp_decoder)) {
    rtp_decoder->reorder_gap_active = 0;
    return;
  }
  if (!rtp_decoder->reorder_gap_active) {
    rtp_decoder->reorder_gap_active = 1;
    rtp_decoder->reorder_gap_started_ms = now_ms;
  }
}

// 0018: per-hole ledger.  Slot = sequence % RTP_GAP_HOLE_LEDGER_SIZE; inside
// one gap run the outstanding sequences span less than
// RTP_REORDER_MAX_HOLD_PACKETS, so two live holes never share a slot.
RtpGapHole* rtp_decoder_hole_find(RtpDecoder* rtp_decoder, uint16_t sequence) {
  RtpGapHole* hole = &rtp_decoder->gap_holes[sequence % RTP_GAP_HOLE_LEDGER_SIZE];
  return (hole->used && hole->sequence == sequence) ? hole : NULL;
}

static RtpGapHole* rtp_decoder_hole_touch(RtpDecoder* rtp_decoder,
                                          uint16_t sequence,
                                          uint32_t now_ms) {
  RtpGapHole* hole = &rtp_decoder->gap_holes[sequence % RTP_GAP_HOLE_LEDGER_SIZE];
  if (hole->used && hole->sequence == sequence)
    return hole;  // already tracked: keep first_seen_ms, its clock does not restart
  hole->sequence = sequence;
  hole->used = 1;
  hole->nack_in_flight = 0;
  hole->retries = 0;
  hole->first_seen_ms = now_ms;
  return hole;
}

static void rtp_decoder_hole_release(RtpGapHole* hole) {
  hole->used = 0;
  hole->nack_in_flight = 0;
  hole->retries = 0;
}

static void rtp_decoder_expire_gap(RtpDecoder* rtp_decoder, uint32_t now_ms) {
  if (!rtp_decoder->reorder_gap_active) {
    return;
  }

  // Deliver whatever has become in-order first; holes whose packet made it
  // into the reorder buffer were already released when the packet arrived.
  rtp_decoder_drain_reorder_buffer(rtp_decoder);

  // 0018: per-hole expiry.  Each missing sequence number is abandoned on its
  // OWN deadline (RTP_GAP_HOLE_MAX_HOLD_MS or > RTP_GAP_HOLE_MAX_RETRIES NACK
  // retries) -- never swept together with the other holes in the window like
  // the old 30 ms gap timer did (one late hole threw away every hole behind
  // it: forcedSkips=1074 in a 27 s window).  RTP_REORDER_MAX_HOLD_MS is only
  // the PER-HOLE MINIMUM wait now.  A hole whose NACK chase is still in
  // flight is held open so the retransmission can land in order.
  while (rtp_decoder->reorder_buffered_packets > 0 &&
         !rtp_decoder_expected_packet_buffered(rtp_decoder)) {
    RtpGapHole* hole = rtp_decoder_hole_touch(
        rtp_decoder, rtp_decoder->reorder_expected_sequence, now_ms);
    const uint32_t age_ms = (uint32_t)(now_ms - hole->first_seen_ms);
    if (hole->nack_in_flight)
      break;  // being NACK-chased: do not skip this hole
    if (age_ms < RTP_REORDER_MAX_HOLD_MS)
      break;  // per-hole minimum wait
    if (age_ms < RTP_GAP_HOLE_MAX_HOLD_MS &&
        hole->retries <= RTP_GAP_HOLE_MAX_RETRIES)
      break;  // still inside its own 1.5 s / 19-retry budget
    // This hole -- and only this hole -- is done for.
    rtp_decoder->reorder_expected_sequence++;
    rtp_decoder->forced_sequence_skips++;
    rtp_decoder_hole_release(hole);
    rtp_decoder_drain_reorder_buffer(rtp_decoder);
  }

  rtp_decoder_drain_reorder_buffer(rtp_decoder);
  rtp_decoder->reorder_gap_active = 0;
  rtp_decoder_update_gap_timer(rtp_decoder, now_ms);
}

void rtp_decoder_poll(RtpDecoder* rtp_decoder) {
  if (rtp_decoder && (rtp_decoder->type == PT_H264 || rtp_decoder->type == PT_H265))
    rtp_decoder_expire_gap(rtp_decoder, ports_get_epoch_time());
}

static int rtp_decoder_decode_reordered(RtpDecoder* rtp_decoder, const uint8_t* buf, size_t size) {
  RtpPayloadView view;
  if (!rtp_decoder->reorder_buf || size > RTP_REORDER_PACKET_CAPACITY ||
      rtp_parse_payload((uint8_t*)buf, size, &view) != 0)
    return -1;

  const uint16_t sequence = view.seq_number;
  const uint32_t now_ms = ports_get_epoch_time();
  if (!rtp_decoder->reorder_has_expected_sequence) {
    rtp_decoder->reorder_has_expected_sequence = 1;
    rtp_decoder->reorder_expected_sequence = sequence;
  }

  int16_t distance = (int16_t)(sequence - rtp_decoder->reorder_expected_sequence);
  if (distance < 0) {
    rtp_decoder->late_packets_dropped++;
    return 0;
  }

  // Leave roughly one network RTT for retransmission, but do not block the
  // decoder behind the full allocation window after permanent packet loss.
  while (distance >= RTP_REORDER_MAX_HOLD_PACKETS) {
    RtpGapHole* skipped = rtp_decoder_hole_find(
        rtp_decoder, rtp_decoder->reorder_expected_sequence);
    if (skipped)
      rtp_decoder_hole_release(skipped);
    rtp_decoder->reorder_expected_sequence++;
    rtp_decoder->forced_sequence_skips++;
    rtp_decoder_drain_reorder_buffer(rtp_decoder);
    distance = (int16_t)(sequence - rtp_decoder->reorder_expected_sequence);
  }

  if (distance == 0) {
    RtpGapHole* resolved = rtp_decoder_hole_find(
        rtp_decoder, rtp_decoder->reorder_expected_sequence);
    if (resolved)
      rtp_decoder_hole_release(resolved);
    const int result = rtp_decoder->decode_func(rtp_decoder, (uint8_t*)buf, size);
    rtp_decoder->reorder_expected_sequence++;
    rtp_decoder_drain_reorder_buffer(rtp_decoder);
    rtp_decoder_update_gap_timer(rtp_decoder, now_ms);
    return result;
  }

  // 0018: register every hole this packet just made visible, so each one
  // starts its own 1.5 s life NOW instead of when it crawls to the head of
  // the queue (which would chain K holes into K x 1.5 s stalls).  Already
  // buffered sequence numbers are NOT holes.
  for (int16_t i = 0; i < distance; i++) {
    const uint16_t hole_sequence =
        (uint16_t)(rtp_decoder->reorder_expected_sequence + i);
    const size_t hole_slot = hole_sequence % RTP_REORDER_WINDOW;
    if (rtp_decoder->reorder_used[hole_slot] &&
        rtp_decoder->reorder_sequences[hole_slot] == hole_sequence)
      continue;
    rtp_decoder_hole_touch(rtp_decoder, hole_sequence, now_ms);
  }

  const size_t slot = sequence % RTP_REORDER_WINDOW;
  if (rtp_decoder->reorder_used[slot]) {
    if (rtp_decoder->reorder_sequences[slot] == sequence)
      return 0;
    rtp_decoder->late_packets_dropped++;
    return 0;
  }

  memcpy(rtp_decoder->reorder_buf + (slot * RTP_REORDER_PACKET_CAPACITY), buf, size);
  rtp_decoder->reorder_sizes[slot] = size;
  rtp_decoder->reorder_sequences[slot] = sequence;
  rtp_decoder->reorder_used[slot] = 1;
  rtp_decoder->reorder_buffered_packets++;
  rtp_decoder->reordered_packets++;
  // The hole this packet fills is resolved the moment it lands in the buffer.
  RtpGapHole* filled = rtp_decoder_hole_find(rtp_decoder, sequence);
  if (filled)
    rtp_decoder_hole_release(filled);
  rtp_decoder_update_gap_timer(rtp_decoder, now_ms);
  rtp_decoder_expire_gap(rtp_decoder, now_ms);
  return 0;
}

int rtp_decoder_decode(RtpDecoder* rtp_decoder, const uint8_t* buf, size_t size) {
  if (rtp_decoder->decode_func == NULL)
    return -1;

  if (rtp_decoder->type == PT_H264 || rtp_decoder->type == PT_H265)
    return rtp_decoder_decode_reordered(rtp_decoder, buf, size);

  return rtp_decoder->decode_func(rtp_decoder, (uint8_t*)buf, size);
}
