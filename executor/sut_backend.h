// Copyright 2026 syzkaller project authors. All rights reserved.
// Use of this source code is governed by Apache 2 LICENSE that can be found in the LICENSE file.

#ifndef SUT_BACKEND_H
#define SUT_BACKEND_H

#include <stdint.h>

struct call_t;

class SutBackend {
public:
	virtual ~SutBackend() = default;

	virtual bool Init() = 0;
	virtual bool BeginProgram(uint64_t req_id, uint64_t proc_id) = 0;
	virtual bool EndProgram() = 0;

	virtual bool CopyIn(char* addr, const uint8_t* data, uint64_t size) = 0;
	virtual bool CopyOut(char* addr, uint64_t size, uint64_t* value) = 0;
	virtual bool Syscall(const call_t* call, intptr_t* args, intptr_t* ret, int* err_no) = 0;
};

#endif // SUT_BACKEND_H
