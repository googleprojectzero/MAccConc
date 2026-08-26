/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/* This is copied from the Linux kernel source tree. */
#ifndef _LINUX_KCOV_IOCTLS_H
#define _LINUX_KCOV_IOCTLS_H

#include <linux/types.h>
#include <linux/ioctl.h>

/*
 * Argument for KCOV_REMOTE_ENABLE ioctl, see Documentation/dev-tools/kcov.rst
 * and the comment before kcov_remote_start() for usage details.
 */
struct kcov_remote_arg {
	__u32		trace_mode;	/* KCOV_TRACE_PC or KCOV_TRACE_CMP */
	__u32		area_size;	/* Length of coverage buffer in words */
	__u32		num_handles;	/* Size of handles array */
	__aligned_u64	common_handle;
	__aligned_u64	handles[];
};

#define KCOV_REMOTE_MAX_HANDLES		0x100

#define KCOV_INIT_TRACE			_IOR('c', 1, unsigned long)
#define KCOV_ENABLE			_IO('c', 100)
#define KCOV_DISABLE			_IO('c', 101)
#define KCOV_REMOTE_ENABLE		_IOW('c', 102, struct kcov_remote_arg)
#define KCOV_GET_MEMORY_RECORD_SIZE	_IO('c', 103)
#define KCOV_SET_DI			_IOW('c', 104, struct kcov_set_di_arg)
#define KCOV_RESET_DI_FLAGS		_IO('c', 105)
#define KCOV_WAKE_DI_FLAG		_IO('c', 106)
#define KCOV_SPINWAIT_DI_FLAG		_IO('c', 107)

enum {
	/*
	 * Tracing coverage collection mode.
	 * Covered PCs are collected in a per-task buffer.
	 * In new KCOV version the mode is chosen by calling
	 * ioctl(fd, KCOV_ENABLE, mode). In older versions the mode argument
	 * was supposed to be 0 in such a call. So, for reasons of backward
	 * compatibility, we have chosen the value KCOV_TRACE_PC to be 0.
	 */
	KCOV_TRACE_PC = 0,
	/* Collecting comparison operands mode. */
	KCOV_TRACE_CMP = 1,
	/*
	 * Extended PC coverage collection mode.
	 * In this mode, the top byte of the PC is replaced with flag bits
	 * (KCOV_RECORDFLAG_*).
	 */
	KCOV_TRACE_PC_EXT = 2,
	/* Extended PC coverage mode with tracing of memory accesses. */
	KCOV_TRACE_MEMORY_ACCESS = 3,
};

#define KCOV_RECORD_IP_MASK         0x00ffffffffffffff
#define KCOV_RECORDFLAG_TYPEMASK    0xf000000000000000
#define KCOV_RECORDFLAG_TYPE_NORMAL 0xf000000000000000
#define KCOV_RECORDFLAG_TYPE_ENTRY  0x0000000000000000
#define KCOV_RECORDFLAG_TYPE_EXIT   0x1000000000000000
/* Summarized entry/exit events that occurred in an untraced region. */
#define KCOV_RECORDFLAG_TYPE_EESUM  0x2000000000000000
#define KCOV_RECORDFLAG_TYPE_MEMORY 0x3000000000000000
/* these two record types have a flag index in the low bits */
#define KCOV_RECORDFLAG_TYPE_WAIT   0x4000000000000000
#define KCOV_RECORDFLAG_TYPE_WAKE   0x5000000000000000
/* set in KCOV_RECORDFLAG_TYPE_WAIT record to mark that the wait timed out */
#define KCOV_WAIT_TIMEOUT           0x0010000000000000

/*
 * The format for the types of collected comparisons.
 *
 * Bit 0 shows whether one of the arguments is a compile-time constant.
 * Bits 1 & 2 contain log2 of the argument size, up to 8 bytes.
 */
#define KCOV_CMP_CONST          (1 << 0)
#define KCOV_CMP_SIZE(n)        ((n) << 1)
#define KCOV_CMP_MASK           KCOV_CMP_SIZE(3)

#define KCOV_SUBSYSTEM_COMMON	(0x00ull << 56)
#define KCOV_SUBSYSTEM_USB	(0x01ull << 56)

#define KCOV_SUBSYSTEM_MASK	(0xffull << 56)
#define KCOV_INSTANCE_MASK	(0xffffffffull)

static inline __u64 kcov_remote_handle(__u64 subsys, __u64 inst)
{
	if (subsys & ~KCOV_SUBSYSTEM_MASK || inst & ~KCOV_INSTANCE_MASK)
		return 0;
	return subsys | inst;
}

/*
 * Data format for memory access tracing mode.
 * This is an extensible struct (it can be extended by appending elements);
 * userspace can query the struct size used by the running kernel with
 * KCOV_GET_MEMORY_ACCESS_RECORD_SIZE.
 */
#define MEMORY_ACCESS_RECORD_TYPE_MASK 0xf
#define MEMORY_ACCESS_RECORD_TYPE_ACCESS 0x0
/* flags for MEMORY_ACCESS_RECORD_TYPE_ACCESS */
#define MEMORY_ACCESS_RECORD_WRITE 0x10
#define MEMORY_ACCESS_RECORD_RMW 0x20
#define MEMORY_ACCESS_RECORD_ATOMIC 0x40
#define MEMORY_ACCESS_RECORD_FREE 0x80
#define MEMORY_ACCESS_RECORD_VALUE 0x100 /* value field is valid */
struct memory_access_record {
	__aligned_u64 ip_address_and_kcov_flags;
	__aligned_u64 data_address;
	__u32 size;
	__u32 flags; /* MEMORY_ACCESS_RECORD_* */
	__aligned_u64 time;
	__aligned_u64 value;
} __attribute__((aligned(8)));


/*
 * Delay Injection API
 */
struct kcov_di_stack_elem {
	__aligned_u64 ip;
	__aligned_u64 parent_idx;
};
enum di_stack_type {
	DI_STACK_WAIT = 0,
	DI_STACK_WAKE_PRE,
	DI_STACK_WAKE_POST
};
struct kcov_di_stack {
	__aligned_u64 elems;
	__u32 num_elems;
	enum di_stack_type type;
	__u32 flagidx;
};
struct kcov_set_di_arg {
	/*
	 * Pointer to array of struct kcov_di_stack.
	 * The array consists of function entry instruction addresses, with a
	 * memory access instruction address at the end.
	 * These must be addresses as reported in KCOV_RECORDFLAG_TYPE_ENTRY and
	 * KCOV_RECORDFLAG_TYPE_MEMORY events (so they are not the addresses
	 * where functions begin).
	 */
	__aligned_u64 stacks;
	__u32 num_stacks;

	int sync_bits_fd;
	__aligned_u64 spin_limit;
};

#endif /* _LINUX_KCOV_IOCTLS_H */
