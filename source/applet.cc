#include "error.h"
#include "types.h"

using namespace v8;

namespace {

void nx_applet_illuminance(const FunctionCallbackInfo<Value> &info) {
	Isolate *iso = info.GetIsolate();
	float illuminance = 0.0f;
	Result rc = appletGetCurrentIlluminance(&illuminance);
	if (R_FAILED(rc)) {
		nx_throw_libnx_error(iso, rc, "appletGetCurrentIlluminance");
		return;
	}
	info.GetReturnValue().Set(Number::New(iso, illuminance));
}

void nx_applet_get_applet_type(const FunctionCallbackInfo<Value> &info) {
	info.GetReturnValue().Set(
	    Integer::New(info.GetIsolate(), appletGetAppletType()));
}

void nx_applet_get_operation_mode(const FunctionCallbackInfo<Value> &info) {
	info.GetReturnValue().Set(
	    Integer::New(info.GetIsolate(), appletGetOperationMode()));
}

void nx_applet_set_media_playback_state(
    const FunctionCallbackInfo<Value> &info) {
	Isolate *iso = info.GetIsolate();
	bool state = info[0]->BooleanValue(iso);
	Result rc = appletSetMediaPlaybackState(state);
	if (R_FAILED(rc)) {
		nx_throw_libnx_error(iso, rc, "appletSetMediaPlaybackState");
	}
}

// Combined keep-awake switch (patch 0051). Declares "media playback" (the
// official video-app path: the system will not dim the screen or auto-sleep)
// AND toggles the auto-sleep-disabled flag directly (belt and braces). Unlike
// the single-purpose bindings above this one does NOT throw: it returns the
// first failing rc (0 on success) as a number, so the caller can log/report
// the rc without try/catch. Both calls always run: the two knobs are
// independent system state and each alone is sufficient to block auto-sleep.
// The engine restores both to false on every teardown path (see main.cc).
void nx_applet_set_keep_awake(const FunctionCallbackInfo<Value> &info) {
	Isolate *iso = info.GetIsolate();
	bool state = info[0]->BooleanValue(iso);
	Result rc_media = appletSetMediaPlaybackState(state);
	Result rc_sleep = appletSetAutoSleepDisabled(state);
	Result rc = R_FAILED(rc_media) ? rc_media : rc_sleep;
	info.GetReturnValue().Set(Integer::New(iso, rc));
}

} // namespace

void nx_init_applet(Isolate *iso, Local<Object> init_obj) {
	NX_SET_FUNC(init_obj, "appletIlluminance", nx_applet_illuminance);
	NX_SET_FUNC(init_obj, "appletGetAppletType", nx_applet_get_applet_type);
	NX_SET_FUNC(init_obj, "appletGetOperationMode",
	            nx_applet_get_operation_mode);
	NX_SET_FUNC(init_obj, "appletSetMediaPlaybackState",
	            nx_applet_set_media_playback_state);
	NX_SET_FUNC(init_obj, "appletSetKeepAwake", nx_applet_set_keep_awake);
}
