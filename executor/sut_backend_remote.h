// Copyright 2026 syzkaller project authors. All rights reserved.
// Use of this source code is governed by Apache 2 LICENSE that can be found in the LICENSE file.

#ifndef SUT_BACKEND_REMOTE_H
#define SUT_BACKEND_REMOTE_H

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

#include <netdb.h>

#include "sut_backend.h"

class JsonRpcTcpClient {
public:
	void Configure(const std::string& endpoint, uint32_t timeout_ms)
	{
		endpoint_ = endpoint;
		timeout_ms_ = timeout_ms;
		Close();
	}

	bool IsConnected() const
	{
		return fd_ != -1;
	}

	void Close()
	{
		if (fd_ != -1) {
			close(fd_);
			fd_ = -1;
		}
	}

	bool Connect()
	{
		if (fd_ != -1)
			return true;
		if (endpoint_.empty()) {
			fprintf(stderr, "[Executor] SUT connect failed: endpoint is empty\n");
			return false;
		}
		std::string host;
		std::string port;
		if (!SplitEndpoint(endpoint_, &host, &port)) {
			fprintf(stderr,
				"[Executor] SUT connect failed: invalid endpoint '%s' (expected host:port or [ipv6]:port)\n",
				endpoint_.c_str());
			return false;
		}

		addrinfo hints = {};
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_protocol = IPPROTO_TCP;

		addrinfo* result = nullptr;
		int gai_err = getaddrinfo(host.c_str(), port.c_str(), &hints, &result);
		if (gai_err != 0) {
			fprintf(stderr, "[Executor] SUT connect failed: DNS/addr resolution for %s:%s failed: %s\n",
				host.c_str(), port.c_str(), gai_strerror(gai_err));
			return false;
		}

		int attempts = 0;
		int last_errno = 0;
		for (addrinfo* ai = result; ai != nullptr; ai = ai->ai_next) {
			attempts++;
			int sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
			if (sock == -1) {
				last_errno = errno;
				fprintf(stderr,
					"[Executor] SUT connect attempt %d failed: socket(family=%d,type=%d,proto=%d): errno=%d (%s)\n",
					attempts, ai->ai_family, ai->ai_socktype, ai->ai_protocol,
					last_errno, strerror(last_errno));
				continue;
			}
			if (connect(sock, ai->ai_addr, ai->ai_addrlen) == 0) {
				fd_ = sock;
				freeaddrinfo(result);
				return true;
			}
			last_errno = errno;
			fprintf(stderr,
				"[Executor] SUT connect attempt %d to %s:%s failed: errno=%d (%s)\n",
				attempts, host.c_str(), port.c_str(), last_errno, strerror(last_errno));
			close(sock);
		}
		freeaddrinfo(result);
		if (attempts == 0) {
			fprintf(stderr, "[Executor] SUT connect failed: no resolved addresses for %s:%s\n",
				host.c_str(), port.c_str());
		} else {
			fprintf(stderr,
				"[Executor] SUT connect failed after %d attempt(s) to %s:%s (last errno=%d: %s)\n",
				attempts, host.c_str(), port.c_str(), last_errno, strerror(last_errno));
		}
		return false;
	}

	bool Request(const std::string& req, std::string* resp)
	{
		if (!IsConnected() && !Connect()) {
			fprintf(stderr, "[Executor] Failed to connect to SUT backend at %s\n", endpoint_.c_str());

			return false;
		}

		if (!SendAll(req.data(), req.size())) {
			fprintf(stderr, "[Executor] Failed to send request to SUT backend at %s\n", endpoint_.c_str());
			return false;
		}

		fprintf(stderr, "[Executor] >>> %s\n", req.c_str());
		return RecvLine(resp);
	}

private:
	int fd_ = -1;
	uint32_t timeout_ms_ = 200;
	std::string endpoint_ = "127.0.0.1:12100";

	bool Wait(short events)
	{
		pollfd pfd = {};
		pfd.fd = fd_;
		pfd.events = events;
		for (;;) {
			int rv = poll(&pfd, 1, static_cast<int>(timeout_ms_));
			if (rv > 0)
				return (pfd.revents & events) != 0;
			if (rv == 0)
				return false;
			if (errno != EINTR)
				return false;
		}
	}

	bool SendAll(const char* data, size_t size)
	{
		for (size_t sent = 0; sent < size;) {
			if (!Wait(POLLOUT))
				return false;
			ssize_t n = send(fd_, data + sent, size - sent, 0);
			if (n > 0) {
				sent += static_cast<size_t>(n);
				continue;
			}
			if (n == -1 && (errno == EINTR || errno == EAGAIN))
				continue;
			return false;
		}
		return true;
	}

	bool RecvLine(std::string* out)
	{
		out->clear();
		for (;;) {
			if (!Wait(POLLIN))
				return false;
			char ch = 0;
			ssize_t n = recv(fd_, &ch, 1, 0);
			if (n == 1) {
				if (ch == '\n')
					return true;
				out->push_back(ch);
				if (out->size() > 16 << 10)
					return false;
				continue;
			}
			if (n == 0)
				return false;
			if (errno == EINTR || errno == EAGAIN)
				continue;
			return false;
		}
	}

	static bool SplitEndpoint(const std::string& endpoint, std::string* host, std::string* port)
	{
		if (endpoint.empty())
			return false;
		if (endpoint[0] == '[') {
			size_t end = endpoint.find(']');
			if (end == std::string::npos || end + 2 > endpoint.size() || endpoint[end + 1] != ':')
				return false;
			*host = endpoint.substr(1, end - 1);
			*port = endpoint.substr(end + 2);
			return !host->empty() && !port->empty();
		}
		size_t colon = endpoint.rfind(':');
		if (colon == std::string::npos)
			return false;
		*host = endpoint.substr(0, colon);
		*port = endpoint.substr(colon + 1);
		return !host->empty() && !port->empty();
	}
};

class RemoteSutBackend final : public SutBackend {
public:
	void Configure(const std::string& endpoint, uint32_t timeout_ms, int retries)
	{
		endpoint_ = endpoint;
		timeout_ms_ = timeout_ms;
		retries_ = retries < 0 ? 0 : retries;
		client_.Configure(endpoint_, timeout_ms_);
	}

	bool Init() override
	{
		client_.Configure(endpoint_, timeout_ms_);
		return true;
	}

	bool BeginProgram(ull req_id, ull proc_id) override
	{
		(void)req_id;
		(void)proc_id;
		return true;
	}

	bool EndProgram() override
	{
		return true;
	}

	bool CopyIn(char* addr, const uint8_t* data, ull size) override
	{
		// Phase 1: keep copyin local.
		return NONFAILING(memcpy(addr, data, size));
	}

	bool CopyOut(char* addr, ull size, ull* value) override
	{
		// Phase 1: keep copyout local.
		return NONFAILING(copyout(addr, size, value));
	}

	bool Syscall(const call_t* call, intptr_t* args, intptr_t* ret, uint32_t* err_no) override
	{
		// Keep pseudo-syscalls and userspace helper callbacks local.
		if (call->call) {
			*ret = -1;
			errno = EFAULT;
			bool ok = NONFAILING(*ret = execute_syscall(call, args));
			*err_no = errno;
			return ok;
		}

		std::lock_guard<std::mutex> guard(mu_);
		*ret = -1;
		*err_no = EIO;

		for (int attempt = 0; attempt <= retries_; attempt++) {
			std::string req;
			if (!BuildSyscallRequest(call->sys_nr, args, &req)) {
				*err_no = EPROTO;
				return false;
			}
			std::string resp;
			if (client_.Request(req, &resp) && ParseSyscallResponse(resp, ret, err_no))
				return true;

			fprintf(stderr, "[Executor] Syscall request failed (attempt %d/%d): %s\n", attempt + 1, retries_ + 1, req.c_str());
			client_.Close();
		}

		*ret = -1;
		*err_no = EIO;
		return false;
	}

private:
	std::mutex mu_;
	JsonRpcTcpClient client_;
	std::string endpoint_ = "127.0.0.1:9001";
	uint32_t timeout_ms_ = 200;
	int retries_ = 1;
	uint64_t next_rpc_id_ = 1;

	void copyout(char* addr, ull size, ull* value)
	{
		switch (size) {
		case 1:
			*value = *(uint8_t*)addr;
			break;
		case 2:
			*value = *(uint16_t*)addr;
			break;
		case 4:
			*value = *(uint32_t*)addr;
			break;
		case 8:
			*value = *(ull*)addr;
			break;
		default:
			failmsg("copyout: bad argument size", "size=%llu", size);
		}
	}

	bool BuildSyscallRequest(int32_t sys_nr, const intptr_t* args, std::string* req)
	{
		char buf[640];
		int n = snprintf(buf, sizeof(buf),
				 "{\"jsonrpc\":\"2.0\",\"id\":%llu,\"method\":\"Executor.ExecuteSyscall\","
				 "\"params\":{\"sys_nr\":%d,\"args\":[%lld,%lld,%lld,%lld,%lld,%lld]}}\n",
				 static_cast<unsigned long long>(next_rpc_id_++), static_cast<int>(sys_nr),
				 static_cast<long long>(args[0]), static_cast<long long>(args[1]),
				 static_cast<long long>(args[2]), static_cast<long long>(args[3]),
				 static_cast<long long>(args[4]), static_cast<long long>(args[5]));
		if (n <= 0 || static_cast<size_t>(n) >= sizeof(buf))
			return false;
		req->assign(buf, static_cast<size_t>(n));
		return true;
	}

	bool ParseSyscallResponse(const std::string& resp, intptr_t* ret, uint32_t* err_no)
	{
		if (resp.find("\"error\"") != std::string::npos)
			return false;
		long long ret_ll = 0;
		long long errno_ll = 0;
		if (!ExtractIntegerField(resp, "ret", &ret_ll))
			return false;
		if (!ExtractIntegerField(resp, "errno", &errno_ll))
			return false;
		if (errno_ll < 0)
			return false;
		*ret = static_cast<intptr_t>(ret_ll);
		*err_no = static_cast<uint32_t>(errno_ll);
		return true;
	}

	static bool ExtractIntegerField(const std::string& json, const char* name, long long* value)
	{
		std::string key = "\"" + std::string(name) + "\"";
		size_t pos = json.find(key);
		if (pos == std::string::npos)
			return false;
		pos = json.find(':', pos + key.size());
		if (pos == std::string::npos)
			return false;
		pos++;
		while (pos < json.size() && isspace(static_cast<unsigned char>(json[pos])))
			pos++;
		const char* start = json.c_str() + pos;
		char* end = nullptr;
		errno = 0;
		long long parsed = strtoll(start, &end, 10);
		if (start == end || errno != 0)
			return false;
		*value = parsed;
		return true;
	}
};

#endif // SUT_BACKEND_REMOTE_H

