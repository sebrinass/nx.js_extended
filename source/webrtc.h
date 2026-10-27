#pragma once
#include "types.h"

// Native WebRTC binding (libpeer backend).
//
// See docs/17-native绑定设计.md for the full design; the short version:
//
//   facade (TS)  <->  $.webrtc*  <->  source/webrtc.cc  <->  source/webrtc_backend*.cc
//                                                       <->  extern/libpeer (C)
//
// `source/webrtc.cc` is the portable V8 half (it compiles on the device AND in
// the host test binary). The platform half is the backend:
//   - source/webrtc_backend_libpeer.cc  -> real libpeer (device only)
//   - packages/runtime/test/src/webrtc-backend-stub.cc -> host fake
void nx_init_webrtc(v8::Isolate *iso, v8::Local<v8::Object> init_obj);
