//
// Created by kingdo on 23-10-16.
//

#ifndef LINUX_KERNEL_5_14_2_USER_POPULATE_H
#define LINUX_KERNEL_5_14_2_USER_POPULATE_H

#include <linux/types.h>

#define _UPFDIO_GRAFT (0x00)
#define _UPFDIO_COPY (0x01)
#define _UPFDIO_READ (0x02)
#define _UPFDIO_WRITE (0x03)

/* userpopulatefd ioctl ids */
#define UPFDIO 0xAA
#define UPFDIO_GRAFT _IOWR(UPFDIO, _UPFDIO_GRAFT, struct upfdio_graft)
#define UPFDIO_COPY _IOWR(UPFDIO, _UPFDIO_COPY, struct upfdio_copy)
#define UPFDIO_READ _IOWR(UPFDIO, _UPFDIO_READ, struct upfdio_rw)
#define UPFDIO_WRITE _IOWR(UPFDIO, _UPFDIO_WRITE, struct upfdio_rw)

//struct upfdio_range {
//	__u64 start;
//	__u64 len;
//};

struct upfdio_graft {
	__u64 src;
	__u64 dst;
	__u64 len;
};

struct upfdio_copy {
	__u64 src;
	__u64 dst;
	__u64 len;
};

struct upfdio_rw {
	__u64 addr;
	__u64 len;
	__u64 ret;
};

#endif //LINUX_KERNEL_5_14_2_USER_POPULATE_H
