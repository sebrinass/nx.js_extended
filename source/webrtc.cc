// Native WebRTC binding (V8 half).
//
// Layout (see docs/17-native绑定设计.md):
//
//   facade (TS)                      packages/runtime/src/native-webrtc.ts
//        |  globalThis.__nx_webrtc            |
//        v                                    v
//   $.webrtc*  (this file)  --->  source/webrtc_backend.h  --->  extern/libpeer (C)
//
// This file is the *portable* half: it compiles on the device AND in the host
// test binary (packages/runtime/test). The platform half is the backend
// (source/webrtc_backend_libpeer.cc on device, a fake in the host test
// binary), so the binding logic is testable without libpeer.
//
// House style: engine/source/BINDINGS.md.
//   - C++20, -fno-rtti -fno-exceptions.
//   - Every V8 API call happens on the loop thread. libpeer is poll-based
//     (single-threaded), so there is NO network thread to marshal: the whole
//     stack is pumped by $.webrtcTick().
//   - Never abort: use nx_throw() + return, or .ToLocal(&out) + return.
//   - nx_str() is for literals only; use nx_str_lossy() for network bytes.

#include "webrtc.h"

#include "error.h"
#include "types.h"
#include "webrtc_backend.h"

#include <memory>
#include <stdlib.h>
#include <string.h>

using namespace v8;

namespace {

// ---------------------------------------------------------------------------
// Event sink
// ---------------------------------------------------------------------------

// The single JS callback that receives every native event:
//
//     sink(peerId: number, kind: string, payload: object)
//
// The payload is a plain JS object, NOT a JSON string, because DataChannel
// payloads are binary (input packets) and JSON cannot carry them.
// The docs/08 §6.2 `onEvent(peerId, kind, payloadJson)` shape is layered on
// top of this in packages/runtime/src/native-webrtc.ts.
Global<Function> g_event_sink;

// ---------------------------------------------------------------------------
// Peer handles
// ---------------------------------------------------------------------------

// Peer ids are allocated by this module; the backend keys its state on them.
int g_next_peer_id = 1;

const char *nx_webrtc_kind_name(int kind) {
	switch (kind) {
	case NX_WEBRTC_EV_ICE_CANDIDATE:
		return "icecandidate";
	case NX_WEBRTC_EV_DATACHANNEL:
		return "datachannel";
	case NX_WEBRTC_EV_DATACHANNEL_OPEN:
		return "datachannelopen";
	case NX_WEBRTC_EV_DATACHANNEL_CLOSE:
		return "datachannelclose";
	case NX_WEBRTC_EV_DATACHANNEL_MESSAGE:
		return "datachannelmessage";
	case NX_WEBRTC_EV_TRACK:
		return "track";
	case NX_WEBRTC_EV_ICE_STATE:
		return "icestate";
	case NX_WEBRTC_EV_CONN_STATE:
		return "connstate";
	case NX_WEBRTC_EV_ERROR:
	default:
		return "error";
	}
}

// Set a property on a freshly created object. The key is always one of our
// own literals and the object is fresh, so the Set() cannot fail for a
// reason other than OOM -- same invariant the other modules rely on when
// they call .Check() (see source/usb.cc, source/software-keyboard.cc).
void nx_webrtc_set(Isolate *iso, Local<Context> ctx, Local<Object> obj,
                   const char *key, Local<Value> value) {
	obj->Set(ctx, nx_str(iso, key), value).Check();
}

// Build the JS payload for one event and push it into the JS sink.
void nx_webrtc_emit(const nx_webrtc_event *ev, void *userdata) {
	Isolate *iso = static_cast<Isolate *>(userdata);
	if (g_event_sink.IsEmpty())
		return;

	HandleScope scope(iso);
	Local<Context> ctx = iso->GetCurrentContext();
	if (ctx.IsEmpty())
		return;
	Context::Scope ctx_scope(ctx);

	Local<Function> sink = g_event_sink.Get(iso);
	if (sink.IsEmpty())
		return;

	Local<Object> payload = Object::New(iso);

	switch (ev->kind) {
	case NX_WEBRTC_EV_ICE_CANDIDATE:
		// `text` is the candidate in W3C form ("candidate:..." without the
		// "a=" prefix). `text2` is the sdpMid, `num` the sdpMLineIndex.
		if (ev->text) {
			nx_webrtc_set(iso, ctx, payload, "candidate",
			              nx_str_lossy(iso, ev->text, (int)ev->text_len));
		}
		if (ev->text2) {
			nx_webrtc_set(iso, ctx, payload, "sdpMid",
			              nx_str_lossy(iso, ev->text2, -1));
		}
		nx_webrtc_set(iso, ctx, payload, "sdpMLineIndex",
		              Integer::New(iso, ev->num));
		break;

	case NX_WEBRTC_EV_DATACHANNEL:
		nx_webrtc_set(iso, ctx, payload, "sid", Integer::New(iso, ev->num));
		if (ev->text) {
			nx_webrtc_set(iso, ctx, payload, "label",
			              nx_str_lossy(iso, ev->text, (int)ev->text_len));
		}
		break;

	case NX_WEBRTC_EV_DATACHANNEL_OPEN:
	case NX_WEBRTC_EV_DATACHANNEL_CLOSE:
		nx_webrtc_set(iso, ctx, payload, "sid", Integer::New(iso, ev->num));
		break;

	case NX_WEBRTC_EV_DATACHANNEL_MESSAGE: {
		nx_webrtc_set(iso, ctx, payload, "sid", Integer::New(iso, ev->num));
		// The backend's buffer is only valid for the duration of this
		// callback, so the payload MUST be copied into a BackingStore that
		// owns its memory.
		size_t len = ev->bytes_len;
		if (ev->bytes && len > 0) {
			uint8_t *copy = static_cast<uint8_t *>(malloc(len));
			if (copy) {
				memcpy(copy, ev->bytes, len);
				std::unique_ptr<BackingStore> bs = ArrayBuffer::NewBackingStore(
				    copy, len,
				    [](void *data, size_t, void *) { free(data); }, nullptr);
				Local<ArrayBuffer> ab = ArrayBuffer::New(iso, std::move(bs));
				nx_webrtc_set(iso, ctx, payload, "data", ab);
			}
		}
		break;
	}

	case NX_WEBRTC_EV_TRACK:
		if (ev->text) {
			nx_webrtc_set(iso, ctx, payload, "kind",
			              nx_str_lossy(iso, ev->text, (int)ev->text_len));
		}
		if (ev->text2) {
			nx_webrtc_set(iso, ctx, payload, "mediaType",
			              nx_str_lossy(iso, ev->text2, -1));
		}
		nx_webrtc_set(iso, ctx, payload, "sdpMid",
		              Integer::New(iso, ev->num));
		// The media payload: one H.264 access unit (Annex-B) or one Opus
		// packet.  Same copy-into-a-BackingStore dance as the datachannel
		// message -- the backend's buffer is only valid for this callback.
		{
			size_t len = ev->bytes_len;
			if (ev->bytes && len > 0) {
				uint8_t *copy = static_cast<uint8_t *>(malloc(len));
				if (copy) {
					memcpy(copy, ev->bytes, len);
					std::unique_ptr<BackingStore> bs = ArrayBuffer::NewBackingStore(
					    copy, len,
					    [](void *data, size_t, void *) { free(data); }, nullptr);
					Local<ArrayBuffer> ab = ArrayBuffer::New(iso, std::move(bs));
					nx_webrtc_set(iso, ctx, payload, "data", ab);
				}
			}
		}
		break;

	case NX_WEBRTC_EV_ICE_STATE:
	case NX_WEBRTC_EV_CONN_STATE:
		if (ev->text) {
			nx_webrtc_set(iso, ctx, payload, "state",
			              nx_str_lossy(iso, ev->text, (int)ev->text_len));
		}
		break;

	case NX_WEBRTC_EV_ERROR:
	default:
		if (ev->text) {
			nx_webrtc_set(iso, ctx, payload, "message",
			              nx_str_lossy(iso, ev->text, (int)ev->text_len));
		}
		break;
	}

	Local<Value> args[3] = {
	    Integer::New(iso, ev->peer_id),
	    nx_str(iso, nx_webrtc_kind_name(ev->kind)),
	    payload,
	};

	// A throwing JS sink must not take the engine down: report it through
	// the engine's own error path (same as the frame/exit handlers).
	TryCatch try_catch(iso);
	Local<Value> ret;
	if (!sink->Call(ctx, Null(iso), 3, args).ToLocal(&ret)) {
		nx_emit_error_event(iso, &try_catch);
	}
}

// ---------------------------------------------------------------------------
// $.webrtc* functions
// ---------------------------------------------------------------------------

// Read an int argument. Returns false (and leaves an exception pending) on
// a non-numeric value.
bool nx_webrtc_arg_int(const FunctionCallbackInfo<Value> &info, int index,
                       int *out) {
	Isolate *iso = info.GetIsolate();
	Local<Context> ctx = iso->GetCurrentContext();
	if (index >= info.Length()) {
		*out = 0;
		return true;
	}
	return info[index]->Int32Value(ctx).To(out);
}

// Read a string argument into a NUL-terminated buffer. Returns false on
// failure (an exception is pending).
bool nx_webrtc_arg_string(const FunctionCallbackInfo<Value> &info, int index,
                          char *buf, size_t buf_len) {
	Isolate *iso = info.GetIsolate();
	if (index >= info.Length()) {
		buf[0] = '\0';
		return true;
	}
	String::Utf8Value v(iso, info[index]);
	if (*v == nullptr) {
		buf[0] = '\0';
		return false;
	}
	size_t n = (size_t)v.length();
	if (n >= buf_len)
		n = buf_len - 1;
	memcpy(buf, *v, n);
	buf[n] = '\0';
	return true;
}

// Read a JS array of strings into a caller-provided slot list.
//
// Two properties matter here:
//   - INDEX ALIGNMENT: out[i] is the i-th element of the JS array (NULL when
//     that element is missing or not a string). The three parallel arrays
//     (urls / usernames / credentials) must stay aligned with each other.
//   - OWNERSHIP: the strings are strdup()'d. A String::Utf8Value's buffer is
//     freed when the Utf8Value goes out of scope, so handing `*v` to the
//     caller would dangle. The caller frees the slots with
//     nx_webrtc_free_string_array().
//
// Returns the number of slots used (the JS array length, capped at max_out).
int nx_webrtc_arg_string_array(const FunctionCallbackInfo<Value> &info,
                               int index, const char **out, int max_out) {
	Isolate *iso = info.GetIsolate();
	Local<Context> ctx = iso->GetCurrentContext();
	for (int i = 0; i < max_out; i++)
		out[i] = nullptr;
	if (index >= info.Length() || !info[index]->IsArray())
		return 0;
	Local<Array> arr = info[index].As<Array>();
	uint32_t len = arr->Length();
	if (len > (uint32_t)max_out)
		len = (uint32_t)max_out;
	for (uint32_t i = 0; i < len; i++) {
		Local<Value> item;
		if (!arr->Get(ctx, i).ToLocal(&item))
			break;
		if (!item->IsString())
			continue;
		String::Utf8Value v(iso, item);
		if (*v == nullptr)
			continue;
		out[i] = strdup(*v);
	}
	return (int)len;
}

// Free the slots filled in by nx_webrtc_arg_string_array().
void nx_webrtc_free_string_array(const char **arr, int count) {
	if (arr == nullptr)
		return;
	for (int i = 0; i < count; i++) {
		free(const_cast<char *>(arr[i]));
		arr[i] = nullptr;
	}
}

// $.webrtcCreatePeer(urls: string[], usernames: string[], credentials:
// string[]): number
//
// `urls` entries are STUN/TURN URLs ("stun:host:port"). libpeer keeps at most
// 5 ICE servers (PeerConfiguration.ice_servers[5]).
void nx_webrtc_create_peer(const FunctionCallbackInfo<Value> &info) {
	Isolate *iso = info.GetIsolate();
	HandleScope scope(iso);

	const char *urls[8];
	const char *users[8];
	const char *creds[8];
	int n_urls = nx_webrtc_arg_string_array(info, 0, urls, 8);
	int n_users = nx_webrtc_arg_string_array(info, 1, users, 8);
	int n_creds = nx_webrtc_arg_string_array(info, 2, creds, 8);
	int count = n_urls > n_users ? n_urls : n_users;
	if (n_creds > count)
		count = n_creds;

	int peer_id = g_next_peer_id++;
	if (peer_id <= 0) {
		g_next_peer_id = 1;
		peer_id = g_next_peer_id++;
	}
	int rc = nx_webrtc_backend_create(peer_id, count, urls, users, creds);
	nx_webrtc_free_string_array(urls, count);
	nx_webrtc_free_string_array(users, count);
	nx_webrtc_free_string_array(creds, count);
	if (rc != 0) {
		// 0041: the backend tells us WHY it could not come up.  Without the
		// runtime (libsrtp + usrsctp) the SCTP association can never be
		// built, so the peer would die right after the DTLS handshake --
		// saying "failed to create peer" would hide that completely.
		nx_throw(iso, rc == NX_WEBRTC_BACKEND_ERR_INIT
		                 ? "webrtc: backend init failed (libsrtp srtp_init)"
		                 : "webrtc: failed to create peer");
		return;
	}
	info.GetReturnValue().Set(Integer::New(iso, peer_id));
}

// $.webrtcDestroyPeer(peerId: number): void
void nx_webrtc_destroy_peer(const FunctionCallbackInfo<Value> &info) {
	int peer_id = 0;
	if (!nx_webrtc_arg_int(info, 0, &peer_id))
		return;
	nx_webrtc_backend_destroy(peer_id);
}

// $.webrtcSetRemoteDescription(peerId: number, sdp: string, isOffer:
// boolean): void
void nx_webrtc_set_remote_description(const FunctionCallbackInfo<Value> &info) {
	Isolate *iso = info.GetIsolate();
	HandleScope scope(iso);
	int peer_id = 0;
	if (!nx_webrtc_arg_int(info, 0, &peer_id))
		return;

	// The SDP can be several KB; heap-allocate rather than cap it.
	String::Utf8Value sdp(iso, info.Length() > 1 ? info[1]
	                                                 : Local<Value>());
	if (*sdp == nullptr) {
		nx_throw(iso, "webrtc: setRemoteDescription expects an SDP string");
		return;
	}
	// IsTrue() is stable across V8 versions; BooleanValue() changed its
	// return type (bool -> Maybe<bool>) between releases.
	int is_offer = (info.Length() > 2 && info[2]->IsTrue()) ? 1 : 0;

	int rc = nx_webrtc_backend_set_remote_description(peer_id, *sdp, is_offer);
	if (rc != 0) {
		nx_throw(iso, "webrtc: setRemoteDescription failed");
		return;
	}
}

// $.webrtcCreateAnswer(peerId: number): string
void nx_webrtc_create_answer(const FunctionCallbackInfo<Value> &info) {
	Isolate *iso = info.GetIsolate();
	HandleScope scope(iso);
	int peer_id = 0;
	if (!nx_webrtc_arg_int(info, 0, &peer_id))
		return;

	char *sdp = nx_webrtc_backend_create_answer(peer_id);
	if (sdp == nullptr) {
		nx_throw(iso, "webrtc: createAnswer failed");
		return;
	}
	Local<String> out;
	if (!String::NewFromUtf8(iso, sdp, v8::NewStringType::kNormal)
	         .ToLocal(&out)) {
		free(sdp);
		return; // exception pending
	}
	free(sdp);
	info.GetReturnValue().Set(out);
}

// $.webrtcSetLocalDescription(peerId: number, sdp: string): void
//
// Accepts the SDP AFTER the page SDK munged it (docs/08 §4.4 risk 3).
void nx_webrtc_set_local_description(const FunctionCallbackInfo<Value> &info) {
	Isolate *iso = info.GetIsolate();
	HandleScope scope(iso);
	int peer_id = 0;
	if (!nx_webrtc_arg_int(info, 0, &peer_id))
		return;
	String::Utf8Value sdp(iso, info.Length() > 1 ? info[1]
	                                                 : Local<Value>());
	if (*sdp == nullptr) {
		nx_throw(iso, "webrtc: setLocalDescription expects an SDP string");
		return;
	}
	nx_webrtc_backend_set_local_description(peer_id, *sdp);
}

// $.webrtcNotifyAnswerSent(peerId: number): void
//
// Patch 0037. The page reports that the ANSWER went out to the cloud.
//
// Until then no ICE connectivity check leaves: the cloud cannot answer a
// check that arrives before it has processed our answer (it does not know
// our ICE credentials yet) and drops it.  Without this the first check left
// one frame after createAnswer() -- before the answer had been sent at all
// -- and the next one only 100 frames (~3.3 s) later.
//
// The page calls this right after it has sent the answer over the
// signalling channel.  If it never does, the backend starts the check
// anyway after a short fallback, so the connection still comes up.
void nx_webrtc_notify_answer_sent(const FunctionCallbackInfo<Value> &info) {
	int peer_id = 0;
	if (!nx_webrtc_arg_int(info, 0, &peer_id))
		return;
	nx_webrtc_backend_notify_answer_sent(peer_id);
}

// $.webrtcAddIceCandidate(peerId: number, candidate: string): void
void nx_webrtc_add_ice_candidate(const FunctionCallbackInfo<Value> &info) {
	Isolate *iso = info.GetIsolate();
	HandleScope scope(iso);
	int peer_id = 0;
	if (!nx_webrtc_arg_int(info, 0, &peer_id))
		return;
	String::Utf8Value candidate(iso, info.Length() > 1 ? info[1]
	                                                    : Local<Value>());
	if (*candidate == nullptr) {
		nx_throw(iso, "webrtc: addIceCandidate expects a candidate string");
		return;
	}
	if (nx_webrtc_backend_add_ice_candidate(peer_id, *candidate) != 0) {
		nx_throw(iso, "webrtc: addIceCandidate failed");
		return;
	}
}

// $.webrtcClosePeer(peerId: number): void
void nx_webrtc_close_peer(const FunctionCallbackInfo<Value> &info) {
	int peer_id = 0;
	if (!nx_webrtc_arg_int(info, 0, &peer_id))
		return;
	nx_webrtc_backend_destroy(peer_id);
}

// $.webrtcGetStats(peerId: number): string
//
// Returns a JSON blob. It carries the three counters the WebRTC self test
// needs on real hardware -- the local ICE candidate list, the ICE
// tie-breaker and the last DTLS write result -- next to the original
// {"rttMs":n,"packetsLost":n,"framesReceived":n} fields.
//
// The buffer is 16 KB: the candidate list is a JSON array of strings and
// 0035 adds one object per ICE candidate pair on top of that.  4 KB would
// truncate the JSON in the middle.
void nx_webrtc_get_stats(const FunctionCallbackInfo<Value> &info) {
	Isolate *iso = info.GetIsolate();
	HandleScope scope(iso);
	int peer_id = 0;
	if (!nx_webrtc_arg_int(info, 0, &peer_id))
		return;

	char buf[16384];
	buf[0] = '\0';
	nx_webrtc_backend_get_stats(peer_id, buf, sizeof(buf));
	Local<String> out;
	if (!String::NewFromUtf8(iso, buf, v8::NewStringType::kNormal)
	         .ToLocal(&out))
		return;
	info.GetReturnValue().Set(out);
}

// $.webrtcDcSend(peerId: number, sid: number, data: ArrayBuffer): void
void nx_webrtc_dc_send(const FunctionCallbackInfo<Value> &info) {
	Isolate *iso = info.GetIsolate();
	HandleScope scope(iso);

	int peer_id = 0;
	int sid = 0;
	if (!nx_webrtc_arg_int(info, 0, &peer_id))
		return;
	if (!nx_webrtc_arg_int(info, 1, &sid))
		return;

	Local<Value> v = info.Length() > 2 ? info[2] : Local<Value>();
	if (v.IsEmpty() || !v->IsArrayBuffer()) {
		nx_throw(iso, "webrtc: dcSend expects an ArrayBuffer");
		return;
	}
	Local<ArrayBuffer> ab = v.As<ArrayBuffer>();
	auto store = ab->GetBackingStore();
	if (!store) {
		nx_throw(iso, "webrtc: dcSend got a detached ArrayBuffer");
		return;
	}
	const uint8_t *data = static_cast<const uint8_t *>(store->Data());
	size_t len = store->ByteLength();
	if (nx_webrtc_backend_dc_send(peer_id, sid, data, len) != 0) {
		nx_throw(iso, "webrtc: dcSend failed");
		return;
	}
}

// $.webrtcDcClose(peerId: number, sid: number): void
void nx_webrtc_dc_close(const FunctionCallbackInfo<Value> &info) {
	int peer_id = 0;
	int sid = 0;
	if (!nx_webrtc_arg_int(info, 0, &peer_id))
		return;
	if (!nx_webrtc_arg_int(info, 1, &sid))
		return;
	nx_webrtc_backend_dc_close(peer_id, sid);
}

// $.webrtcOnEvent(cb: (peerId, kind, payload) => void): void
void nx_webrtc_on_event(const FunctionCallbackInfo<Value> &info) {
	Isolate *iso = info.GetIsolate();
	HandleScope scope(iso);

	Local<Value> cb = info.Length() > 0 ? info[0] : Local<Value>();
	if (cb.IsEmpty() || !cb->IsFunction()) {
		nx_throw(iso, "webrtc: onEvent expects a function");
		return;
	}
	g_event_sink.Reset(iso, cb.As<Function>());
}

// $.webrtcTick(): void
//
// Pumps the WebRTC state machine and drains pending events into the JS sink.
// MUST run on the loop thread; the TS glue calls it from a uv timer.
void nx_webrtc_tick(const FunctionCallbackInfo<Value> &info) {
	Isolate *iso = info.GetIsolate();
	HandleScope scope(iso);

	nx_webrtc_backend_tick();
	nx_webrtc_backend_poll_events(nx_webrtc_emit, iso);
}

} // namespace

void nx_init_webrtc(Isolate *iso, Local<Object> init_obj) {
	NX_SET_FUNC(init_obj, "webrtcCreatePeer", nx_webrtc_create_peer);
	NX_SET_FUNC(init_obj, "webrtcDestroyPeer", nx_webrtc_destroy_peer);
	NX_SET_FUNC(init_obj, "webrtcSetRemoteDescription",
	            nx_webrtc_set_remote_description);
	NX_SET_FUNC(init_obj, "webrtcCreateAnswer", nx_webrtc_create_answer);
	NX_SET_FUNC(init_obj, "webrtcSetLocalDescription",
	            nx_webrtc_set_local_description);
	NX_SET_FUNC(init_obj, "webrtcNotifyAnswerSent",
	            nx_webrtc_notify_answer_sent);
	NX_SET_FUNC(init_obj, "webrtcAddIceCandidate", nx_webrtc_add_ice_candidate);
	NX_SET_FUNC(init_obj, "webrtcClosePeer", nx_webrtc_close_peer);
	NX_SET_FUNC(init_obj, "webrtcGetStats", nx_webrtc_get_stats);
	NX_SET_FUNC(init_obj, "webrtcDcSend", nx_webrtc_dc_send);
	NX_SET_FUNC(init_obj, "webrtcDcClose", nx_webrtc_dc_close);
	NX_SET_FUNC(init_obj, "webrtcOnEvent", nx_webrtc_on_event);
	NX_SET_FUNC(init_obj, "webrtcTick", nx_webrtc_tick);
}
