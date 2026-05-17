// Copyright 2026 syzkaller project authors. All rights reserved.
// Use of this source code is governed by Apache 2 LICENSE that can be found in the LICENSE file.

#ifndef SUT_BACKEND_H
#define SUT_BACKEND_H

#include <cstdint>
#include <stdint.h>

struct call_t;
struct cover_t;

typedef unsigned long long ull;

class SutBackend {
public:
	virtual ~SutBackend() = default;

	virtual bool Init() = 0;
	virtual bool BeginProgram(ull req_id, ull proc_id) = 0;
	virtual bool EndProgram() = 0;

	virtual bool CopyIn(char* addr, const uint8_t* data, ull size) = 0;
	virtual bool CopyOut(char* addr, ull size, ull* value) = 0;
	virtual bool Syscall(const call_t* call, intptr_t* args, intptr_t* ret, uint32_t* err_no) = 0;

	// Called after Syscall() to inject coverage data into the cover buffer.
	// Remote backends override this to populate cov->data with PCs received from the SUT.
	// Local backends rely on kcov and leave this as a no-op.
	virtual void InjectCoverage(cover_t* cov) { (void)cov; }
};

#endif // SUT_BACKEND_H
