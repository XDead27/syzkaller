package xtratum

import (
	"github.com/google/syzkaller/prog"
)

func InitTarget(target *prog.Target) {
	// xtratum descriptions currently don't define syz_mmap/mmap, and data mapping
	// is handled by executor os_init. So don't emit any data-mmap calls.
	target.MakeDataMmap = func() []*prog.Call {
		return nil
	}
}
