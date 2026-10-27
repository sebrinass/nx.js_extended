#pragma once
#include "types.h"

// W3C WebCodecs video trio (VideoDecoder / VideoFrame / EncodedVideoChunk).
//
// The engine had NO W3C-shaped video decoder before this: `video-decoder.cc`
// binds the legacy `Switch.VideoDecoder` ("hand it a URL, it plays") API from
// the QuickJS era, which is a different shape entirely.
//
// This module adds the W3C shape on top of a RAW-BITSTREAM ffmpeg decoder:
//
//   EncodedVideoChunk  --(Annex-B / AVCC bytes)-->  VideoDecoder
//                                                        |
//                                                        v
//                                            VideoFrame (an nx_image_t wrapper)
//                                                        |
//                                                        v
//                                          canvas 2D drawImage()
//
// Why a separate decoder instead of reusing nx_media_open(): the nx_media_*
// pipeline is an AVFORMAT pipeline — it needs a CONTAINER (it demuxes, probes
// streams, slaves a clock). WebCodecs hands us bare access units with no
// container and no clock, so it needs a raw AVCodec entry point. See
// patches/0008-NOTES.md for the full rationale.
//
// Threading contract:
//   - Every function here is called on the main (JS loop) thread.
//   - The decode thread is internal; chunks go in through a mutex-guarded
//     queue and decoded frames come out through another one. The main thread
//     drains the output queue via webcodecsVideoDecoderPoll().

void nx_init_webcodecs(v8::Isolate *iso, v8::Local<v8::Object> init_obj);
