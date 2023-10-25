
/* SPDX-License-Identifier: GPL-2.0 */
/*
 *  include/linux/userfaultfd_k.h
 *
 *  Copyright (C) 2015  Red Hat, Inc.
 *
 */

#ifndef _LINUX_USERPOPULATEFD_K_H
#define _LINUX_USERPOPULATEFD_K_H

#include <linux/userpopulatefd.h> /* linux/include/uapi/linux/userpopulatefd.h */

enum mpopulate_atomic_mode {
	MPOPULATE_ATOMIC_COPY,
	MPOPULATE_ATOMIC_GRATE,
};

extern ssize_t mpopulate_copy_atomic(struct mm_struct *mm,
				     unsigned long dst_start,
				     unsigned long src_start,
				     unsigned long len);

extern ssize_t mpopulat_graft_atomic(struct mm_struct *mm,
				     unsigned long dst_start,
				     unsigned long src_start,
				     unsigned long len);

#endif /* _LINUX_USERPOPULATEFD_K_H */
