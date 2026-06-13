// Copyright 2026 syzkaller project authors. All rights reserved.
// Use of this source code is governed by Apache 2 LICENSE that can be found in the LICENSE file.

#ifndef SUT_BACKEND_REMOTE_H
#define SUT_BACKEND_REMOTE_H

#include <cerrno>
#include <chrono>
#include <errno.h>
#include <iomanip>
#include <poll.h>
#include <sstream>
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
#include <vector>

#include <netdb.h>

#include "sut_backend.h"

std::string timestamp_now()
{
	using namespace std::chrono;

	auto now = system_clock::now();

	// Split into seconds + milliseconds
	auto ms = duration_cast<milliseconds>(
		      now.time_since_epoch()) %
		  1000;

	auto tt = system_clock::to_time_t(now);

	std::tm tm{};
	localtime_r(&tt, &tm); // local timezone (POSIX)

	std::ostringstream oss;

	oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S")
	    << '.'
	    << std::setw(3)
	    << std::setfill('0')
	    << ms.count();

	return oss.str();
}

class JsonRpcTcpClient
{
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
		recv_buf_.clear();
	}

	bool Connect()
	{
		if (fd_ != -1)
			return true;
		if (endpoint_.empty()) {
			debug("[Executor] SUT connect failed: endpoint is empty\n");
			return false;
		}
		std::string host;
		std::string port;
		if (!SplitEndpoint(endpoint_, &host, &port)) {
			debug("[Executor] SUT connect failed: invalid endpoint '%s' (expected host:port or [ipv6]:port)\n",
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
			debug("[Executor] SUT connect failed: DNS/addr resolution for %s:%s failed: %s\n",
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
				debug("[Executor] SUT connect attempt %d failed: socket(family=%d,type=%d,proto=%d): errno=%d (%s)\n",
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
			debug("[Executor] SUT connect attempt %d to %s:%s failed: errno=%d (%s)\n",
			      attempts, host.c_str(), port.c_str(), last_errno, strerror(last_errno));
			close(sock);
		}
		freeaddrinfo(result);
		if (attempts == 0) {
			debug("[Executor] SUT connect failed: no resolved addresses for %s:%s\n",
			      host.c_str(), port.c_str());
		} else {
			debug("[Executor] SUT connect failed after %d attempt(s) to %s:%s (last errno=%d: %s)\n",
			      attempts, host.c_str(), port.c_str(), last_errno, strerror(last_errno));
		}
		return false;
	}

	bool Call(const char* method, const std::string& params_json, std::string* result_json)
	{
		if (!IsConnected() && !Connect()) {
			debug("[Executor] Failed to connect to SUT backend at %s\n", endpoint_.c_str());
			return false;
		}

		const uint64_t request_id = next_rpc_id_++;
		std::string req;
		if (!BuildRequest(request_id, method, params_json, &req)) {
			return false;
		}

		if (!SendAll(req.data(), req.size())) {
			debug("[Executor] Failed to send request to SUT backend at %s\n", endpoint_.c_str());
			return false;
		}
		debug("[%s] [%s] >>> %s\n", method, timestamp_now().c_str(), req.c_str());

		std::string resp;
		if (!RecvJsonObject(&resp)) {
			return false;
		}
		debug("[%s] [%s] <<< %s\n", method, timestamp_now().c_str(), resp.c_str());

		return ParseResponse(resp, request_id, result_json);
	}

	static bool ExtractTopLevelField(const std::string& json, const char* field, std::string* value)
	{
		size_t pos = 0;
		SkipWs(json, &pos);
		if (pos >= json.size() || json[pos] != '{')
			return false;
		pos++;

		for (;;) {
			SkipWs(json, &pos);
			if (pos >= json.size())
				return false;
			if (json[pos] == '}')
				return false;
			if (json[pos] != '"')
				return false;

			size_t key_begin = pos + 1;
			if (!ConsumeString(json, &pos))
				return false;
			size_t key_end = pos - 1;
			std::string key = json.substr(key_begin, key_end - key_begin);

			SkipWs(json, &pos);
			if (pos >= json.size() || json[pos] != ':')
				return false;
			pos++;
			SkipWs(json, &pos);
			size_t value_begin = pos;
			if (!ConsumeValue(json, &pos))
				return false;
			size_t value_end = pos;

			if (key == field) {
				*value = json.substr(value_begin, value_end - value_begin);
				return true;
			}

			SkipWs(json, &pos);
			if (pos >= json.size())
				return false;
			if (json[pos] == ',') {
				pos++;
				continue;
			}
			if (json[pos] == '}')
				return false;
			return false;
		}
	}

private:
	int fd_ = -1;
	uint32_t timeout_ms_ = 3000;
	std::string endpoint_ = "127.0.0.1:12100";
	std::string recv_buf_;
	uint64_t next_rpc_id_ = 1;

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

	bool RecvJsonObject(std::string* out)
	{
		out->clear();
		for (;;) {
			if (ExtractOneJson(&recv_buf_, out))
				return true;

			if (!Wait(POLLIN)) {
				debug("[Executor] SUT backend recv timeout after %u ms\n", timeout_ms_);
				return false;
			}

			char chunk[4096];
			ssize_t n = recv(fd_, chunk, sizeof(chunk), 0);
			if (n > 0) {
				recv_buf_.append(chunk, static_cast<size_t>(n));
				if (recv_buf_.size() > (1u << 20)) {
					debug("[Executor] SUT backend recv buffer overflow\n");
					return false;
				}
				continue;
			}
			if (n == 0) {
				debug("[Executor] SUT backend closed the connection\n");
				return false;
			}
			if (errno == EINTR || errno == EAGAIN)
				continue;

			debug("[Executor] SUT backend recv failed: errno=%d (%s)\n", errno, strerror(errno));
			return false;
		}
	}

	static void SkipWs(const std::string& s, size_t* pos)
	{
		while (*pos < s.size() && isspace(static_cast<unsigned char>(s[*pos])))
			(*pos)++;
	}

	static bool ConsumeString(const std::string& s, size_t* pos)
	{
		if (*pos >= s.size() || s[*pos] != '"')
			return false;
		(*pos)++;
		bool esc = false;
		while (*pos < s.size()) {
			char ch = s[*pos];
			(*pos)++;
			if (esc) {
				esc = false;
				continue;
			}
			if (ch == '\\') {
				esc = true;
				continue;
			}
			if (ch == '"')
				return true;
		}
		return false;
	}

	static bool ConsumeValue(const std::string& s, size_t* pos)
	{
		SkipWs(s, pos);
		if (*pos >= s.size())
			return false;
		if (s[*pos] == '"')
			return ConsumeString(s, pos);
		if (s[*pos] == '{' || s[*pos] == '[') {
			char open = s[*pos];
			char close = (open == '{') ? '}' : ']';
			int depth = 0;
			bool in_string = false;
			bool esc = false;
			for (; *pos < s.size(); (*pos)++) {
				char ch = s[*pos];
				if (in_string) {
					if (esc) {
						esc = false;
						continue;
					}
					if (ch == '\\') {
						esc = true;
						continue;
					}
					if (ch == '"')
						in_string = false;
					continue;
				}
				if (ch == '"') {
					in_string = true;
					continue;
				}
				if (ch == open)
					depth++;
				else if (ch == close)
					depth--;
				if (depth == 0) {
					(*pos)++;
					return true;
				}
			}
			return false;
		}
		while (*pos < s.size()) {
			char ch = s[*pos];
			if (ch == ',' || ch == '}' || ch == ']' || isspace(static_cast<unsigned char>(ch)))
				break;
			(*pos)++;
		}
		return true;
	}

	static bool ExtractOneJson(std::string* buf, std::string* obj)
	{
		size_t start = 0;
		SkipWs(*buf, &start);
		if (start >= buf->size()) {
			buf->clear();
			return false;
		}
		if ((*buf)[start] != '{')
			return false;
		size_t pos = start;
		if (!ConsumeValue(*buf, &pos))
			return false;
		obj->assign(buf->substr(start, pos - start));
		buf->erase(0, pos);
		return true;
	}

	static bool BuildRequest(uint64_t id, const char* method, const std::string& params_json, std::string* req)
	{
		if (!method || !method[0])
			return false;
		const std::string& params = params_json.empty() ? std::string("{}") : params_json;
		char head[192];
		int n = snprintf(head, sizeof(head), "{\"id\":%llu,\"method\":\"%s\",\"params\":",
				 static_cast<unsigned long long>(id), method);
		if (n <= 0 || static_cast<size_t>(n) >= sizeof(head))
			return false;
		req->assign(head, static_cast<size_t>(n));
		req->append(params);
		req->append("}");
		return true;
	}

	static bool ParseResponse(const std::string& resp, uint64_t expected_id, std::string* result_json)
	{
		std::string id_value;
		if (!ExtractTopLevelField(resp, "id", &id_value))
			return false;
		errno = 0;
		char* end = nullptr;
		unsigned long long id = strtoull(id_value.c_str(), &end, 10);
		if (end == id_value.c_str() || *end != '\0' || errno != 0)
			return false;
		if (id != expected_id)
			return false;

		std::string error_value;
		if (!ExtractTopLevelField(resp, "error", &error_value))
			return false;
		size_t p = 0;
		SkipWs(error_value, &p);
		if (error_value.compare(p, 4, "null") != 0)
			return false;

		return ExtractTopLevelField(resp, "result", result_json);
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

class RemoteSutBackend final : public SutBackend
{
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

	bool BeginProgram(ull req_id, uint64 proc_id) override
	{
		(void)req_id;
		(void)proc_id;
		return true;
	}

	bool EndProgram() override
	{
		return true;
	}

	bool CopyIn(char* addr, const uint8_t* data, uint64 size) override
	{
		debug("[Executor] RemoteSutBackend::CopyIn addr=%p size=%llu\n", addr, size);
		std::lock_guard<std::mutex> guard(mu_);

		for (int attempt = 0; attempt <= retries_; attempt++) {
			std::string params_json;
			if (!BuildCopyInRequest(reinterpret_cast<uintptr_t>(addr), data, size, &params_json))
				return false;
			std::string result_json;
			if (client_.Call("Executor.CopyIn", params_json, &result_json) &&
			    ParseSuccessResponse(result_json))
				return true;

			debug("[Executor] CopyIn request failed (attempt %d/%d)\n", attempt + 1, retries_ + 1);
			client_.Close();
		}

		debug("[Executor] All attempts to CopyIn at %p failed\n", addr);
		return false;
	}

	bool CopyOut(char* addr, uint64 size, ull* value) override
	{
		debug("[Executor] RemoteSutBackend::CopyOut addr=%p size=%llu\n", addr, size);
		std::lock_guard<std::mutex> guard(mu_);

		for (int attempt = 0; attempt <= retries_; attempt++) {
			std::string params_json;
			if (!BuildCopyOutRequest(reinterpret_cast<uintptr_t>(addr), size, &params_json))
				return false;
			std::string result_json;
			if (client_.Call("Executor.CopyOut", params_json, &result_json) &&
			    ParseCopyOutResponse(result_json, value))
				return true;

			debug("[Executor] CopyOut request failed (attempt %d/%d)\n", attempt + 1, retries_ + 1);
			client_.Close();
		}

		debug("[Executor] All attempts to CopyOut at %p failed\n", addr);
		return false;
	}

	bool Syscall(const call_t* call, intptr_t* args, intptr_t* ret, uint32_t* err_no) override
	{
		debug("[Executor] RemoteSutBackend::Syscall name=%s sys_nr=%d mode=%s\n",
		      call->name, call->sys_nr, call->call ? "local-callback" : "remote-rpc");
		// TODO: What to do in this case
		if (call->call) {
			*ret = -1;
			errno = EFAULT;
			last_coverage_pcs_.clear();
			bool ok = NONFAILING(*ret = execute_syscall(call, args));
			*err_no = errno;
			return ok;
		}

		std::lock_guard<std::mutex> guard(mu_);
		*ret = -1;
		*err_no = EIO;
		last_coverage_pcs_.clear();

		for (int attempt = 0; attempt <= retries_; attempt++) {
			std::string params_json;
			if (!BuildSyscallRequest(call->sys_nr, args, &params_json)) {
				*err_no = EPROTO;
				return false;
			}
			std::string result_json;
			if (!client_.Call("Executor.ExecuteSyscall", params_json, &result_json)) {
				// RPC transport failure — retry with a fresh connection.
				debug("[Executor] Syscall RPC failed (attempt %d/%d): %s\n",
				      attempt + 1, retries_ + 1, strerror(errno));
				*err_no = EIO;
				client_.Close();
				continue;
			}
			// RPC succeeded — parse the application-level response.
			// ParseSyscallResponse calls failmsg on crash, sets err_no on error.
			if (ParseSyscallResponse(result_json, ret, err_no)) {
				ParseCoverageFromResponse(result_json);
				return true;
			}

			// Server explicitly returned failure
			return false;
		}

		debug("[Executor] All attempts to execute syscall %d failed\n", call->sys_nr);
		return false;
	}

	void InjectCoverage(cover_t* cov) override
	{
		if (!cov)
			return;

		// In remote mode, the kcov buffer may be mapped read-only (PROT_READ)
		// and/or pkey-protected, making direct writes impossible.
		// We replace cov->data with our own writable buffer on first use.
		EnsureWritableCoverBuffer(cov);

		if (last_coverage_pcs_.empty()) {
			// No coverage from this call — ensure count is zero.
			if (is_kernel_64_bit)
				*(uint64*)cov->data = 0;
			else
				*(uint32*)cov->data = 0;
			return;
		}

		// Write PCs into our writable buffer in KCOV format:
		// [count] [pc0] [pc1] ... where each element is uint64 or uint32
		// depending on is_kernel_64_bit. The subsequent cover_collect() will
		// read the count and set cov->size accordingly.
		debug("[Executor] Is kernel_64_bit=%d", is_kernel_64_bit);
		if (is_kernel_64_bit) {
			uint64* buf = (uint64*)cov->data;
			size_t max_pcs = (cov->data_end - cov->data) / sizeof(uint64) - 1;
			size_t n = last_coverage_pcs_.size();
			if (n > max_pcs) {
				n = max_pcs;
				cov->overflow = true;
			}
			buf[0] = n;
			memcpy(&buf[1], last_coverage_pcs_.data(), n * sizeof(uint64));
		} else {
			uint32* buf = (uint32*)cov->data;
			size_t max_pcs = (cov->data_end - cov->data) / sizeof(uint32) - 1;
			size_t n = last_coverage_pcs_.size();
			if (n > max_pcs) {
				n = max_pcs;
				cov->overflow = true;
			}
			buf[0] = (uint32)n;
			for (size_t i = 0; i < n; i++)
				buf[i + 1] = (uint32)last_coverage_pcs_[i];
		}
		debug("[Executor] Injected %zu coverage PCs from remote\n", last_coverage_pcs_.size());
	}

private:
	std::mutex mu_;
	JsonRpcTcpClient client_;
	std::string endpoint_ = "127.0.0.1:9001";
	uint32_t timeout_ms_ = 200;
	int retries_ = 1;
	std::vector<uint64> last_coverage_pcs_;

	// Replace the kcov-backed (potentially read-only) buffer with a writable
	// private allocation. This is idempotent — once replaced, subsequent calls
	// are no-ops. We keep the same data_size so cover_collect and write_signal
	// work unchanged.
	void EnsureWritableCoverBuffer(cover_t* cov)
	{
		// If data hasn't been mapped yet (delayed kcov mmap), allocate from scratch.
		// If data is within the kcov mmap region, replace it with a writable buffer.
		bool needs_alloc = false;
		uint32 size = cov->data_size;
		if (!size)
			size = 256 * 1024; // Fallback: 256KB (enough for ~32K PCs)
		if (!cov->data) {
			needs_alloc = true;
		} else if (cov->mmap_alloc_ptr &&
			   cov->data >= cov->mmap_alloc_ptr &&
			   cov->data < cov->mmap_alloc_ptr + cov->mmap_alloc_size) {
			needs_alloc = true;
		}
		if (!needs_alloc)
			return;
		char* buf = (char*)mmap(NULL, size, PROT_READ | PROT_WRITE,
					MAP_PRIVATE | MAP_ANON, -1, 0);
		if (buf == MAP_FAILED)
			exitf("failed to allocate remote coverage buffer");
		memset(buf, 0, size);
		cov->data = buf;
		cov->data_end = buf + size;
		cov->data_size = size;
		cov->data_offset = is_kernel_64_bit ? sizeof(uint64) : sizeof(uint32);
		cov->pc_offset = 0;
		debug("[Executor] Allocated writable remote coverage buffer (%u bytes)\n", size);
	}

	bool BuildSyscallRequest(int32_t sys_nr, const intptr_t* args, std::string* params_json)
	{
		char buf[320];
		int n = snprintf(buf, sizeof(buf),
				 "{\"sys_nr\":%d,\"args\":[%lld,%lld,%lld,%lld,%lld,%lld]}",
				 static_cast<int>(sys_nr),
				 static_cast<long long>(args[0]), static_cast<long long>(args[1]),
				 static_cast<long long>(args[2]), static_cast<long long>(args[3]),
				 static_cast<long long>(args[4]), static_cast<long long>(args[5]));
		if (n <= 0 || static_cast<size_t>(n) >= sizeof(buf))
			return false;
		params_json->assign(buf, static_cast<size_t>(n));
		return true;
	}

	bool BuildCopyInRequest(uintptr_t addr, const uint8_t* data, uint64 size, std::string* params_json)
	{
		// Header: {"addr":<addr>,"size":<size>,"data":"
		// Hex data: 2 chars per byte
		// Trailer: "}
		// Conservative upper bound for header+trailer.
		const size_t hex_len = static_cast<size_t>(size) * 2;
		const size_t overhead = 80;
		params_json->clear();
		params_json->reserve(overhead + hex_len);

		char head[80];
		int n = snprintf(head, sizeof(head),
				 "{\"addr\":%llu,\"size\":%llu,\"data\":\"",
				 static_cast<unsigned long long>(addr),
				 static_cast<unsigned long long>(size));
		if (n <= 0 || static_cast<size_t>(n) >= sizeof(head))
			return false;
		params_json->append(head, static_cast<size_t>(n));

		for (ull i = 0; i < size; i++) {
			char byte_hex[3];
			snprintf(byte_hex, sizeof(byte_hex), "%02x", data[i]);
			params_json->append(byte_hex, 2);
		}
		params_json->append("\"}");
		return true;
	}

	bool BuildCopyOutRequest(uintptr_t addr, uint64 size, std::string* params_json)
	{
		char buf[80];
		int n = snprintf(buf, sizeof(buf),
				 "{\"addr\":%llu,\"size\":%llu}",
				 static_cast<unsigned long long>(addr),
				 static_cast<unsigned long long>(size));
		if (n <= 0 || static_cast<size_t>(n) >= sizeof(buf))
			return false;
		params_json->assign(buf, static_cast<size_t>(n));
		return true;
	}

	bool ParseCopyOutResponse(const std::string& result_json, ull* value)
	{
		// Expects {"success": true, "value":<number>}
		std::string success_str;
		if (!JsonRpcTcpClient::ExtractTopLevelField(result_json, "success", &success_str) || success_str != "true")
			return false;

		std::string val_str;
		if (!JsonRpcTcpClient::ExtractTopLevelField(result_json, "value", &val_str))
			return false;
		char* end = nullptr;
		errno = 0;
		unsigned long long v = strtoull(val_str.c_str(), &end, 10);
		if (end == val_str.c_str() || errno != 0)
			return false;
		*value = v;
		return true;
	}

	bool ParseSuccessResponse(const std::string& result_json)
	{
		std::string success_str;
		return JsonRpcTcpClient::ExtractTopLevelField(result_json, "success", &success_str) && success_str == "true";
	}

	bool ParseSyscallResponse(const std::string& result_json, intptr_t* ret, uint32_t* err_no)
	{
		if (ParseSuccessResponse(result_json)) {
			*ret = 0;
			*err_no = 0;
			return true;
		}
		// Extract the error message from the response. If it indicates a
		// target crash, terminate so the monitor can pick up the message.
		std::string error_str;
		if (JsonRpcTcpClient::ExtractTopLevelField(result_json, "error", &error_str)) {
			// Strip surrounding quotes from the JSON string value.
			if (error_str.size() >= 2 && error_str.front() == '"' && error_str.back() == '"')
				error_str = error_str.substr(1, error_str.size() - 2);
			if (error_str.find("sut crash") != std::string::npos) {
				client_.Close();
				// Exit immediately; crash details are propagated via proxy console output.
				doexit(kFailStatus);
			}
		}
		*ret = -1;
		*err_no = EFAULT;
		return false;
	}

	// Parse the "coverage" array from the ExecuteSyscall response.
	// Expected format: "coverage":[123456,789012,...] (array of uint64 PC addresses)
	void ParseCoverageFromResponse(const std::string& result_json)
	{
		last_coverage_pcs_.clear();
		std::string cov_str;
		if (!JsonRpcTcpClient::ExtractTopLevelField(result_json, "coverage", &cov_str))
			return;
		// cov_str should be "[num,num,num,...]"
		if (cov_str.size() < 2 || cov_str.front() != '[' || cov_str.back() != ']')
			return;
		// Parse the comma-separated uint64 values between [ and ]
		const char* p = cov_str.c_str() + 1;
		const char* end = cov_str.c_str() + cov_str.size() - 1;
		while (p < end) {
			while (p < end && (*p == ' ' || *p == ','))
				p++;
			if (p >= end)
				break;
			char* next = nullptr;
			errno = 0;
			unsigned long long val = strtoull(p, &next, 0);
			if (next == p || errno != 0)
				break;
			last_coverage_pcs_.push_back((uint64)val);
			p = next;
		}
		debug("[Executor] Parsed %zu coverage PCs from response\n", last_coverage_pcs_.size());
	}
};

#endif // SUT_BACKEND_REMOTE_H
