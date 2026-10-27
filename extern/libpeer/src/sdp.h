#ifndef SDP_H_
#define SDP_H_

#include <string.h>
#include "config.h"

#define SDP_ATTR_LENGTH 128

#ifndef ICE_LITE
#define ICE_LITE 0
#endif

void sdp_append_h264(char* sdp);

/**
 * Advertise H.264 AND H.265 in the same video section.
 *
 * The answerer may answer with a payload type list that is a subset of the
 * offer's (RFC 3264 section 6.1).  Offering both codecs keeps the
 * connection usable when one of the two is dropped later on (the peer may
 * remove the H.265 payload types from the answer), while the RTP payload
 * type tells the depacketizer which codec actually arrives.
 */
void sdp_append_h264_h265(char* sdp);

void sdp_append_pcma(char* sdp);

void sdp_append_pcmu(char* sdp);

void sdp_append_opus(char* sdp);

void sdp_append_datachannel(char* sdp);

void sdp_create(char* sdp, int b_video, int b_audio, int b_datachannel);

int sdp_append(char* sdp, const char* format, ...);

void sdp_reset(char* sdp);

#endif  // SDP_H_
