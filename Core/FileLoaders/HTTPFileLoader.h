// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#pragma once

#include <vector>

#include <retro_atomic.h>

#include "Common/File/Path.h"
#include "Common/Net/Cancel.h"
#include "Common/Net/HTTPClient.h"
#include "Common/Net/Resolve.h"
#include "Common/Net/URL.h"
#include "Common/CommonTypes.h"
#include "Core/Loaders.h"

// Prepared (FileSize/Exists) before reads start, then read from one thread at a time: the worker
// of the CachingFileLoader that ConstructFileLoader wraps it in. Cancel and LatestError are any thread.
class HTTPFileLoader : public FileLoader {
public:
	HTTPFileLoader(const ::Path &filename);
	~HTTPFileLoader();

	bool IsRemote() override {
		return true;
	}
	bool Exists() override;
	bool ExistsFast() override;
	bool IsDirectory() override;
	s64 FileSize() override;
	Path GetPath() const override;

	size_t ReadAt(s64 absolutePos, size_t bytes, size_t count, void *data, Flags flags = Flags::NONE) override {
		return ReadAt(absolutePos, bytes * count, data, flags) / bytes;
	}
	size_t ReadAt(s64 absolutePos, size_t bytes, void *data, Flags flags = Flags::NONE) override;

	void Cancel() override {
		cancel_.Cancel();
	}

	std::string LatestError() const override {
		return (const char *)retro_atomic_load_acquire_ptr(const_cast<retro_atomic_ptr_t *>(&latestError_));
	}

private:
	void Prepare();
	void SetError(const char *error) {
		retro_atomic_store_release_ptr(&latestError_, (void *)error);
	}
	int SendHEAD(const Url &url, std::vector<std::string> &responseHeaders);

	void Connect(double timeout);

	void Disconnect() {
		if (connected_) {
			client_.Disconnect();
		}
		connected_ = false;
	}

	s64 filesize_ = 0;
	s64 filepos_ = 0;
	Url url_;
	http::Client client_;
	net::RequestProgress progress_;
	::Path filename_;
	bool connected_ = false;
	net::CancelToken cancel_;
	// Always a string literal.
	retro_atomic_ptr_t latestError_{ (void *)"" };

	bool prepared_ = false;
};
