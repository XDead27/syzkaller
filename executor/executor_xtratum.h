// Use of this source code is governed by Apache 2 LICENSE that can be found in the LICENSE file.

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

#include "nocover.h"

static void os_init(int argc, char** argv, void* data, size_t data_size)
{
	void* got = mmap(data, data_size, PROT_READ | PROT_WRITE | PROT_EXEC,
			 MAP_ANON | MAP_PRIVATE | MAP_FIXED_EXCLUSIVE, -1, 0);
	if (data != got)
		failmsg("mmap of data segment failed", "want %p, got %p", data, got);

	// Makes sure the file descriptor limit is sufficient to map control pipes.
	struct rlimit rlim;
	rlim.rlim_cur = rlim.rlim_max = kMaxFd;
	setrlimit(RLIMIT_NOFILE, &rlim);

	// xtratum target currently assumes 32-bit syscall ABI semantics.
	is_kernel_64_bit = false;
}

// Optional transport hook implemented by proxy integration code.
// Return 0 on transport success and fill ret/err_no with remote syscall result.
// Return non-zero on transport/protocol failure.
extern "C" int xtratum_proxy_call(int32_t sys_nr,
				   intptr_t a0, intptr_t a1, intptr_t a2,
				   intptr_t a3, intptr_t a4, intptr_t a5,
				   intptr_t* ret, int* err_no) __attribute__((weak));

static intptr_t xtratum_syscall_proxy(int32_t sys_nr,
				      intptr_t a0, intptr_t a1, intptr_t a2,
				      intptr_t a3, intptr_t a4, intptr_t a5)
{
	if (!xtratum_proxy_call) {
		errno = ENOSYS;
		return -1;
	}

	intptr_t ret = -1;
	int err_no = ENOSYS;
	if (xtratum_proxy_call(sys_nr, a0, a1, a2, a3, a4, a5, &ret, &err_no) != 0) {
		errno = EIO;
		return -1;
	}

	errno = err_no;
	return ret;
}

static intptr_t execute_syscall(const call_t* c, intptr_t a[kMaxArgs])
{
	if (c->call)
		return c->call(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8]);
	return xtratum_syscall_proxy(c->sys_nr, a[0], a[1], a[2], a[3], a[4], a[5]);
}

