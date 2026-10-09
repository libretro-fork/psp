#include "ppsspp_config.h"

#include <string_view>
#include <cstring>

#include "Common/System/Request.h"
#include "Common/System/System.h"
#include "Common/Log.h"
#include "Common/File/Path.h"
#include "Common/TimeUtil.h"

#if PPSSPP_PLATFORM(ANDROID)

// Maybe not the most natural place for this, but not sure what would be. It needs to be in the Common project
// unless we want to make another System_ function to retrieve it.

#include <jni.h>

JavaVM *gJvm = nullptr;

#endif

RequestManager g_requestManager;

const char *RequestTypeAsString(SystemRequestType type) {
	switch (type) {
	case SystemRequestType::BROWSE_FOR_IMAGE: return "BROWSE_FOR_IMAGE";
	case SystemRequestType::BROWSE_FOR_FILE: return "BROWSE_FOR_FILE";
	case SystemRequestType::BROWSE_FOR_FOLDER: return "BROWSE_FOR_FOLDER";
	case SystemRequestType::BROWSE_FOR_FILE_SAVE: return "BROWSE_FOR_FILE_SAVE";
	case SystemRequestType::INPUT_TEXT_MODAL: return "INPUT_TEXT_MODAL";
	case SystemRequestType::ASK_USERNAME_PASSWORD: return "ASK_USERNAME_PASSWORD";
	case SystemRequestType::EXIT_APP: return "EXIT_APP";
	case SystemRequestType::RESTART_APP: return "RESTART_APP";
	case SystemRequestType::RECREATE_ACTIVITY: return "RECREATE_ACTIVITY";
	case SystemRequestType::COPY_TO_CLIPBOARD: return "COPY_TO_CLIPBOARD";
	case SystemRequestType::SHARE_TEXT: return "SHARE_TEXT";
	case SystemRequestType::SET_WINDOW_TITLE: return "SET_WINDOW_TITLE";
	case SystemRequestType::APPLY_FULLSCREEN_STATE: return "SET_FULLSCREEN_STATE";
	case SystemRequestType::GRAPHICS_BACKEND_FAILED_ALERT: return "GRAPHICS_BACKEND_FAILED_ALERT";
	case SystemRequestType::CREATE_GAME_SHORTCUT: return "CREATE_GAME_SHORTCUT";
	case SystemRequestType::SHOW_FILE_IN_FOLDER: return "SHOW_FILE_IN_FOLDER";
	case SystemRequestType::NOTIFY_UI_EVENT: return "NOTIFY_UI_EVENT";
	case SystemRequestType::SET_KEEP_SCREEN_BRIGHT: return "SET_KEEP_SCREEN_BRIGHT";
	case SystemRequestType::CAMERA_COMMAND: return "CAMERA_COMMAND";
	case SystemRequestType::GPS_COMMAND: return "GPS_COMMAND";
	case SystemRequestType::INFRARED_COMMAND: return "INFRARED_COMMAND";
	case SystemRequestType::MICROPHONE_COMMAND: return "MICROPHONE_COMMAND";
	case SystemRequestType::RUN_CALLBACK_IN_WNDPROC: return "RUN_CALLBACK_IN_WNDPROC";
	case SystemRequestType::MOVE_TO_TRASH: return "MOVE_TO_TRASH";
	case SystemRequestType::IAP_RESTORE_PURCHASES: return "IAP_RESTORE_PURCHASES";
	case SystemRequestType::IAP_MAKE_PURCHASE: return "IAP_MAKE_PURCHASE";
	default: return "N/A";
	}
}

RequestManager::RequestManager() {
	for (Slot &slot : slots_) {
		slot.link.self = &slot;
		retro_atomic_int_init(&slot.state, SLOT_FREE);
		retro_atomic_int_init(&slot.forgotten, 0);
	}
	mpsc_stack_init(&responses_);
	retro_atomic_int_init(&idCounter_, 10);
	retro_atomic_int_init(&tokenGen_, 20000);
}

bool RequestManager::MakeSystemRequest(SystemRequestType type, RequesterToken token, RequestCallback callback, RequestFailedCallback failedCallback, std::string_view param1, std::string_view param2, int64_t param3, int64_t param4) {
	if (token == NO_REQUESTER_TOKEN) {
		_dbg_assert_(!callback);
		_dbg_assert_(!failedCallback);
	}
	if (callback || failedCallback) {
		_dbg_assert_(token != NO_REQUESTER_TOKEN);
	}

	const int requestId = retro_atomic_fetch_add_int(&idCounter_, 1);

	// NOTE: We need to register immediately, in order to support synchronous implementations.
	Slot *slot = nullptr;
	if (callback || failedCallback) {
		for (Slot &s : slots_) {
			if (retro_atomic_cas_int(&s.state, SLOT_FREE, SLOT_FILLING)) {
				slot = &s;
				break;
			}
		}
		if (!slot) {
			ERROR_LOG(Log::System, "Too many outstanding system requests, dropping %s", RequestTypeAsString(type));
			return false;
		}
		slot->callback = std::move(callback);
		slot->failedCallback = std::move(failedCallback);
		slot->token = token;
		retro_atomic_store_relaxed_int(&slot->forgotten, 0);
		retro_atomic_store_release_int(&slot->state, requestId);
	}

	VERBOSE_LOG(Log::System, "Making system request %s: id %d", RequestTypeAsString(type), requestId);

	std::string p1(param1);
	std::string p2(param2);
	// TODO: Convert to string_view
	if (!System_MakeRequest(type, requestId, p1, p2, param3, param4)) {
		// Not handled. Unless it was answered on the spot, nobody will answer it now.
		if (slot && retro_atomic_cas_int(&slot->state, requestId, SLOT_FILLING))
			Release(slot);
		return false;
	}
	return true;
}

void RequestManager::Release(Slot *slot) {
	slot->callback = nullptr;
	slot->failedCallback = nullptr;
	slot->responseString.clear();
	retro_atomic_store_release_int(&slot->state, SLOT_FREE);
}

void RequestManager::ForgetRequestsWithToken(RequesterToken token) {
	for (Slot &slot : slots_) {
		// Only filled slots have a token worth reading.
		const int state = retro_atomic_load_acquire_int(&slot.state);
		if ((state > 0 || state == SLOT_POSTED) && slot.token == token) {
			INFO_LOG(Log::System, "Forgetting about requester with token %d", token);
			retro_atomic_store_release_int(&slot.forgotten, 1);
		}
	}
}

void RequestManager::Post(int requestId, bool success, std::string_view responseString, int responseValue) {
	for (Slot &slot : slots_) {
		if (retro_atomic_cas_int(&slot.state, requestId, SLOT_POSTING)) {
			slot.success = success;
			slot.responseString = responseString;
			slot.responseValue = responseValue;
			retro_atomic_store_release_int(&slot.state, SLOT_POSTED);
			mpsc_stack_push(&responses_, &slot.link.node);
			return;
		}
	}
	ERROR_LOG(Log::System, "%s: Unexpected request ID %d", success ? "PostSystemSuccess" : "PostSystemFailure", requestId);
}

void RequestManager::PostSystemSuccess(int requestId, std::string_view responseString, int responseValue) {
	DEBUG_LOG(Log::System, "PostSystemSuccess: Request %d (%.*s, %d)", requestId, (int)responseString.size(), responseString.data(), responseValue);
	Post(requestId, true, responseString, responseValue);
}

void RequestManager::PostSystemFailure(int requestId, int responseValue) {
	WARN_LOG(Log::System, "PostSystemFailure: Request %d failed", requestId);
	Post(requestId, false, "", responseValue);
}

void RequestManager::ProcessRequests() {
	// Oldest response first.
	mpsc_stack_node_t *node = mpsc_stack_reverse(mpsc_stack_drain(&responses_));
	while (node) {
		mpsc_stack_node_t *next = node->next;
		Slot *slot = ((SlotLink *)node)->self;
		if (!retro_atomic_load_acquire_int(&slot->forgotten)) {
			if (slot->success) {
				if (slot->callback)
					slot->callback(slot->responseString.c_str(), slot->responseValue);
			} else if (slot->failedCallback) {
				slot->failedCallback(slot->responseValue);
			}
		}
		Release(slot);
		node = next;
	}
}

void RequestManager::Clear() {
	mpsc_stack_node_t *node = mpsc_stack_drain(&responses_);
	while (node) {
		mpsc_stack_node_t *next = node->next;
		Release(((SlotLink *)node)->self);
		node = next;
	}
	// Requests still waiting for an answer won't get one delivered.
	for (Slot &slot : slots_)
		retro_atomic_store_release_int(&slot.forgotten, 1);
}

void System_CreateGameShortcut(const Path &path, std::string_view title) {
	g_requestManager.MakeSystemRequest(SystemRequestType::CREATE_GAME_SHORTCUT, NO_REQUESTER_TOKEN, nullptr, nullptr, path.ToString(), title, 0);
}

// Also acts as just show folder, if you pass in a folder.
void System_ShowFileInFolder(const Path &path) {
	g_requestManager.MakeSystemRequest(SystemRequestType::SHOW_FILE_IN_FOLDER, NO_REQUESTER_TOKEN, nullptr, nullptr, path.ToString(), "", 0);
}

void System_BrowseForFolder(RequesterToken token, std::string_view title, const Path &initialPath, RequestCallback callback, RequestFailedCallback failedCallback) {
	g_requestManager.MakeSystemRequest(SystemRequestType::BROWSE_FOR_FOLDER, token, callback, failedCallback, title, initialPath.ToCString(), 0);
}

void System_RunCallbackInWndProc(void (*callback)(void *, void *), void *userdata) {
	int64_t castPtr = (int64_t)callback;
	int64_t castUserData = (int64_t)userdata;
	g_requestManager.MakeSystemRequest(SystemRequestType::RUN_CALLBACK_IN_WNDPROC, NO_REQUESTER_TOKEN, nullptr, nullptr, "", "", castPtr, castUserData);
}

void System_MoveToTrash(const Path &path) {
	g_requestManager.MakeSystemRequest(SystemRequestType::MOVE_TO_TRASH, NO_REQUESTER_TOKEN, nullptr, nullptr, path.ToString(), "", 0);
}
