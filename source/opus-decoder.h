#pragma once
#include "types.h"

// Raw Opus (RFC 6716) decoding for the engine.
//
// WHY THIS FILE EXISTS
// The engine could decode an Opus stream that arrives inside a CONTAINER
// (source/media-decoder.cc: `nx_media_decode_audio` handles MP3/WAV/OGG/Opus
// in a file), but a WebRTC audio track is NOT a container: RFC 7587 (RTP
// payload format for Opus) says the RTP payload of an audio packet is
// exactly one Opus packet, so the bytes that arrive on the wire can be fed
// to a decoder as-is -- no demuxer, no container, no clock.
//
// The decoder is ffmpeg's NATIVE opus decoder (ff_opus_decoder), which the
// Switch's switch-ffmpeg 7.1 build ships.  Verified at the symbol level
// (not "it probably compiles"):
//
//   aarch64-none-elf-nm --defined-only \
//     C:/devkitPro/portlibs/switch/lib/libavcodec.a
//     -> D ff_opus_decoder        (the AVCodec struct)
//     -> T ff_opus_parse_packet   (the packet parser)
//     -> T opus_decode_packet     (the native libopus decoder itself)
//     -> T ff_opus_dsp_init, opus_decode_init, ...
//
// so no external libopus is needed and nothing has to be added to the
// portlibs.
//
// Opus always decodes at 48 kHz, which is also the rate the engine's audio
// graph runs at on the Switch, so the common path needs no resampling.
//
// THREADING CONTRACT (revised by 0062)
//   - Decoding runs on a per-track decode thread.  `nx_opus_track_push` is
//     called on the main (JS loop) thread but only memcpy's the packet into
//     a bounded queue -- the thread does the decode -> resample -> ring
//     write.  This keeps audio flowing while video-decode long tasks block
//     the main thread for hundreds of ms (measured p90 390 ms / max 671 ms
//     on device).
//   - The graph's own render thread does the actual output.

struct nx_audio_graph;
struct nx_audio_node;

// A decoder for one raw Opus stream.
typedef struct nx_opus_decoder nx_opus_decoder_t;

// What an Opus packet carries (RFC 6716 section 3.2.1, the TOC byte).
struct nx_opus_packet_info {
	int config = 0;        // TOC bits 7..3
	int frame_code = 0;    // TOC bits 2..0
	int frame_count = 0;   // number of Opus frames in the packet
	int frame_tenths_ms = 0; // duration of ONE frame, in tenths of a millisecond
	int total_tenths_ms = 0;  // duration of the whole packet
	uint64_t samples = 0;  // samples at 48 kHz
	bool exact = false;    // false for CELT, whose frame length sets the duration
};

// Parse the TOC byte of an Opus packet.
bool nx_opus_packet_parse(const uint8_t *data, size_t size,
                          nx_opus_packet_info *out);

// Create a decoder producing `channels` (1 or 2) interleaved float32
// channels.  Returns NULL and fills `errbuf` on failure.
nx_opus_decoder_t *nx_opus_decoder_create(int channels, char *errbuf,
                                          size_t errbuf_size);

// Decode one Opus packet into `out` as INTERLEAVED float32 samples.
// `out` must have room for `max_frames * channels` floats.  Returns 0 on
// success and -1 on failure (the decoder stays usable either way).
int nx_opus_decoder_decode(nx_opus_decoder_t *d, const uint8_t *data,
                            size_t size, float *out, int max_frames,
                            int *out_frames);

// 0018: packet-loss concealment (design 3.6 level 3).
//
// libopus exposes PLC as "decode a NULL packet"; libavcodec's send/receive
// API has NO per-packet loss signal -- an empty packet (data NULL, size 0)
// is a FLUSH packet (avcodec.h: "considered a flush packet, which signals
// the end of the stream").  The codec's decode callback still runs once with
// a zero-size payload, which is exactly the libopus-style PLC trigger, so
// the FIRST call yields one concealed frame when this ffmpeg build's opus
// decoder supports it.  Afterwards the decoder is in draining mode:
//   - further PLC calls fail fast (and this function disables itself), and
//   - the next real decode is rescued inside nx_opus_decoder_decode() with
//     avcodec_flush_buffers() + one retry.
// Returns 0 when the call ran (*out_frames may still be 0 -> no concealment
// output, the caller should use its PCM-domain EXPAND instead) and -1 when
// PLC is unavailable/disabled.  Either way the decoder stays usable.
int nx_opus_decoder_plc(nx_opus_decoder_t *d, float *out, int max_frames,
                        int *out_frames);

void nx_opus_decoder_destroy(nx_opus_decoder_t *d);

int nx_opus_decoder_channels(const nx_opus_decoder_t *d);
uint32_t nx_opus_decoder_sample_rate(const nx_opus_decoder_t *d);
// PCM frames produced so far.
uint64_t nx_opus_decoder_frames(const nx_opus_decoder_t *d);
// Opus packets fed to the decoder so far.
uint64_t nx_opus_decoder_packets(const nx_opus_decoder_t *d);
// RMS and peak of the decoded PCM so far (peak is absolute, 0..1+).
double nx_opus_decoder_rms(const nx_opus_decoder_t *d);
double nx_opus_decoder_peak(const nx_opus_decoder_t *d);
// Packets that could not be decoded.
uint64_t nx_opus_decoder_errors(const nx_opus_decoder_t *d);

// A LIVE Opus track: a decoder plus the audio-graph stream-source node the
// decoded PCM is written into, so the audio actually reaches the speakers.
//
// The node is an NX_AUDIO_NODE_STREAM_SOURCE, the same kind the media
// elements use: the render thread drains it a quantum at a time, and its
// consumed-frame counter is the media clock.
typedef struct nx_opus_track nx_opus_track_t;

// `graph` must outlive the track.  The track creates its own
// NX_AUDIO_NODE_STREAM_SOURCE node on `graph`.
nx_opus_track_t *nx_opus_track_create(nx_audio_graph *graph, int channels,
                                       char *errbuf, size_t errbuf_size);

// Feed one Opus packet.  0062: the packet is only queued (memcpy) for the
// track's decode thread; `out_frames` is always written 0 and the return
// value is the enqueue result (0 = queued, -1 = rejected).  Decoding errors
// surface in the `errors` stat, as before.
//
// 0018: this is the LEGACY entry (no RTP metadata).  The live WebRTC path
// no longer comes through here -- the webrtc backend feeds
// nx_opus_track_push_net() straight from its onaudiopacket callback, with
// timestamp/sequence/RED, into the jitter buffer.  A call to this function
// therefore feeds the same jitter buffer with SYNTHESIZED sequence/timestamp
// continuation values (each call = one 20 ms frame further along a private
// timeline), which keeps direct users (tests, pages driving an OpusTrack by
// hand) working through the same 20 ms pull pipeline.  Pushes here are
// dropped once the track has seen real network traffic (the two must not
// interleave on one timeline).
int nx_opus_track_push(nx_opus_track_t *t, const uint8_t *data, size_t size,
                       uint32_t *out_frames);

// 0018: network-side ingest (design 1.1/A2).  Called on the JS loop thread
// from the webrtc backend's onaudiopacket callback for every audio RTP
// packet, WITH the metadata the jitter buffer needs.  `data` and `redundant`
// are BORROWED (libpeer's network buffer -- both die when the callback
// returns); the jitter buffer copies both immediately.  Routes to the live
// Opus track (the first one created; see the registry in opus-decoder.cc) --
// there is no JS round-trip and no event queue in this path any more.
// Returns 0 = accepted, -1 = dropped (no track / poison / late / dup).
int nx_opus_track_push_net(const uint8_t *data, size_t size, uint32_t rtp_ts,
                           uint32_t ssrc, uint16_t seq, uint8_t payload_type,
                           int red_stripped, const uint8_t *redundant,
                           size_t redundant_size,
                           uint32_t redundant_ts_offset);

// Start/stop the output (false = the node emits silence and consumes
// nothing, which is how the media elements implement pause).
void nx_opus_track_set_playing(nx_opus_track_t *t, bool playing);

// The graph node the decoded PCM is written into.
nx_audio_node *nx_opus_track_node(nx_opus_track_t *t);

// Copy the most recently decoded PCM out of the track.
//
// `out` holds interleaved stereo float32 samples; the function fills up to
// `out_len / 2` frames with the newest ones and returns how many frames it
// wrote.  This is what a diagnostic page draws its waveform from.
uint32_t nx_opus_track_read(nx_opus_track_t *t, float *out, uint32_t out_len);

void nx_opus_track_destroy(nx_opus_track_t *t);

void nx_init_opus_decoder(v8::Isolate *iso, v8::Local<v8::Object> init_obj);
