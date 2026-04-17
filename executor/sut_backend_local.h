// Copyright 2026 syzkaller project authors. All rights reserved.
// Use of this source code is governed by Apache 2 LICENSE that can be found in the LICENSE file.

#ifndef SUT_BACKEND_LOCAL_H
#define SUT_BACKEND_LOCAL_H

#include <errno.h>
#include <string.h>

#include "sut_backend.h"

class LocalSutBackend final : public SutBackend {
public:
	bool Init() override
	{
		return true;
	}

	bool BeginProgram(uint64_t req_id, uint64_t proc_id) override
	{
		(void)req_id;
		(void)proc_id;
		return true;
	}

	bool EndProgram() override
	{
		return true;
	}

	bool CopyIn(char* addr, const uint8_t* data, uint64_t size) override
	{
		return NONFAILING(memcpy(addr, data, size));
	}

	bool CopyOut(char* addr, uint64_t size, uint64_t* value) override
	{
		return NONFAILING(copyout(addr, size, value));
	}

	bool Syscall(const call_t* call, intptr_t* args, intptr_t* ret, int* err_no) override
	{
		// For pseudo-syscalls and user-space functions NONFAILING can abort before assigning return value.
		*ret = -1;
		errno = EFAULT;
		bool ok = NONFAILING(*ret = execute_syscall(call, args));
		*err_no = errno;
		return ok;
	}

private:
	void copyout(char* addr, uint64_t size, uint64_t* value)
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
			    *value = *(uint64_t*)addr;
			    break;
		    default:
			    failmsg("copyout: bad argument size", "size=%llu", size);
		}
	}
};

#endif // SUT_BACKEND_LOCAL_H
