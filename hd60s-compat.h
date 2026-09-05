/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Kernel API differences, for building out of tree
 *
 * Copyright (c) 2026 Doug Brown <doug@schmorgal.com>
 *
 * The driver is written against the current kernel API. Everything an older
 * kernel is missing is filled in here and nowhere else, so that the .c files
 * stay free of version conditionals.
 */
#ifndef _HD60S_COMPAT_H_
#define _HD60S_COMPAT_H_

#include <linux/version.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 10, 0)
#error "hd60s needs Linux 5.10 or newer"
#endif

/*
 * get_unaligned_le16() and friends. 6.12 moved the header out of asm/ once
 * every architecture had been switched to the generic implementation.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
#include <linux/unaligned.h>
#else
#include <asm/unaligned.h>
#endif

/* the #ifndef tests below need the real definitions to be meaningful */
#include <linux/bitfield.h>
#include <linux/build_bug.h>
#include <linux/slab.h>

/*
 * FIELD_PREP_CONST(): 6.3. FIELD_PREP() cannot be used in a static initialiser
 * because its checks are not constant expressions. 6.3's definition, less the
 * "mask is contiguous" check, whose __BF_CHECK_POW2() is 6.3 as well.
 */
#ifndef FIELD_PREP_CONST
#define FIELD_PREP_CONST(_mask, _val)					\
	(								\
		/* mask must be non-zero */				\
		BUILD_BUG_ON_ZERO((_mask) == 0) +			\
		/* check if value fits */				\
		BUILD_BUG_ON_ZERO(~((_mask) >> __bf_shf(_mask)) & (_val)) + \
		/* and create the value */				\
		(((typeof(_mask))(_val) << __bf_shf(_mask)) & (_mask))	\
	)
#endif

/* kzalloc_obj(): 7.0. Only the one-object form the driver uses is shimmed. */
#ifndef kzalloc_obj
#define kzalloc_obj(p)		kzalloc(sizeof(p), GFP_KERNEL)
#endif

/*
 * min_queued_buffers: 6.8, renamed from min_buffers_needed. A textual rename is
 * safe -- the new name exists in no kernel old enough to take this branch.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 8, 0)
#define min_queued_buffers	min_buffers_needed
#endif

#endif /* _HD60S_COMPAT_H_ */
