// Use of this source code is governed by Apache 2 LICENSE that can be found in the LICENSE file.

#include <fcntl.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

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

	is_kernel_64_bit = false;
}

static inline intptr_t xtratum_syscall_svc(intptr_t nr,
					   intptr_t a0, intptr_t a1, intptr_t a2,
					   intptr_t a3, intptr_t a4, intptr_t a5)
{
	// register intptr_t r0 asm("r0") = a0;
	// register intptr_t r1 asm("r1") = a1;
	// register intptr_t r2 asm("r2") = a2;
	// register intptr_t r3 asm("r3") = a3;
	// register intptr_t r4 asm("r4") = a4;
	// register intptr_t r5 asm("r5") = a5;
	// register intptr_t r7 asm("r7") = nr;
	//
	// asm volatile("svc #0"
	// 	     : "+r"(r0)
	// 	     : "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5), "r"(r7)
	// 	     : "memory", "cc");
	//
	// return r0;
	return syscall(nr, a0, a1, a2, a3, a4, a5);
}

static intptr_t execute_syscall(const call_t* c, intptr_t a[kMaxArgs])
{
	if (c->call)
		return c->call(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8]);
	return xtratum_syscall_svc(c->sys_nr, a[0], a[1], a[2], a[3], a[4], a[5]);
}

static void cover_open(cover_t* cov, bool extra)
{
}

static void cover_mmap(cover_t* cov)
{
	// if (cov->mmap_alloc_ptr != NULL)
	// 	fail("cover_mmap invoked on an already mmapped cover_t object");
	// cov->mmap_alloc_size = cov->data_size;
	// void* mmap_ptr = mmap(NULL, cov->mmap_alloc_size, PROT_READ | PROT_WRITE,
	// 		      MAP_SHARED, cov->fd, 0);
	// if (mmap_ptr == MAP_FAILED)
	// 	fail("cover mmap failed");
	// cov->mmap_alloc_ptr = (char*)mmap_ptr;
	// cov->data = (char*)mmap_ptr;
	// cov->data_end = cov->data + cov->mmap_alloc_size;
	cov->data_offset = is_kernel_64_bit ? sizeof(uint64_t) : sizeof(uint32_t);
	cov->pc_offset = 0;
}

static void cover_protect(cover_t* cov)
{
}

static void cover_unprotect(cover_t* cov)
{
}

static void cover_enable(cover_t* cov, bool collect_comps, bool extra)
{
}

static void cover_reset(cover_t* cov)
{
	if (!cov->data)
		return;
	*(uint32*)cov->data = 0;
}

static void cover_collect(cover_t* cov)
{
	cov->size = *(uint32*)cov->data;
}
