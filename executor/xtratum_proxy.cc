// Use of this source code is governed by Apache 2 LICENSE that can be found in the LICENSE file.

#include <errno.h>
#include <stdint.h>

extern "C" int xtratum_proxy_call(int32_t sys_nr,
				   intptr_t a0, intptr_t a1, intptr_t a2,
				   intptr_t a3, intptr_t a4, intptr_t a5,
				   intptr_t* ret, int* err_no)
{
	(void)sys_nr;
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	(void)a5;

	if (ret)
		*ret = -1;
	if (err_no)
		*err_no = ENOSYS;

	// Non-zero means transport/proxy failure in executor_xtratum.h.
	// For the stub, report success with ENOSYS semantic result.
	return 0;
}
