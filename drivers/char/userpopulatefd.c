//
// Created by kingdo on 23-10-11.
//
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/ioctl.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/miscdevice.h>
#include <linux/major.h>
#include <linux/ioport.h>
#include <linux/fcntl.h>
#include <linux/init.h>
#include <linux/poll.h>
#include <linux/mm.h>
#include <linux/proc_fs.h>
#include <linux/spinlock.h>
#include <linux/sysctl.h>
#include <linux/wait.h>
#include <linux/sched/signal.h>
#include <linux/bcd.h>
#include <linux/seq_file.h>
#include <linux/bitops.h>
#include <linux/compat.h>
#include <linux/clocksource.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/acpi.h>
#include <linux/hpet.h>
#include <asm/current.h>
#include <asm/irq.h>
#include <asm/div64.h>
#include <linux/device.h>
#include <linux/userpopulatefd_k.h>
#include <linux/security.h>

// close all optimization for this file
//#pragma GCC optimize("O0")

#define DEVICE_NAME "userpopulatefd"

static struct cdev cdev;
static dev_t dev;
static struct class *userpopulatefd_class;
static struct device *userpopulatefd_device;
static char kernel_buffer[PAGE_SIZE] = "Hello, I am Kingdo!";

struct userfaultfd_ctx {};

static __always_inline int validate_range(struct mm_struct *mm, __u64 start,
					  __u64 len)
{
	__u64 task_size = mm->task_size;

	if (start & ~PAGE_MASK)
		return -EINVAL;
	if (len & ~PAGE_MASK)
		return -EINVAL;
	if (!len)
		return -EINVAL;
	if (start < mmap_min_addr)
		return -EINVAL;
	if (start >= task_size)
		return -EINVAL;
	if (len > task_size - start)
		return -EINVAL;
	return 0;
}

static int userpopulatefd_ioctl_graft(struct userfaultfd_ctx *ctx,
				      unsigned long arg)
{
	__s64 ret;
	struct upfdio_graft upfdio_graft;
	struct upfdio_graft __user *user_upfdio_graft;

	user_upfdio_graft = (struct upfdio_graft __user *)arg;

	ret = -EFAULT;
	if (copy_from_user(&upfdio_graft, user_upfdio_graft,
			   sizeof(struct upfdio_graft)))
		goto out;

	ret = validate_range(current->mm, upfdio_graft.dst, upfdio_graft.len);
	if (ret)
		goto out;

	ret = -EINVAL;
	if (upfdio_graft.src + upfdio_graft.len <= upfdio_graft.src)
		goto out;

	ret = mpopulat_graft_atomic(current->mm, upfdio_graft.dst,
				    upfdio_graft.src, upfdio_graft.len);

out:
	return ret;
}

static int userpopulatefd_ioctl_copy(struct userfaultfd_ctx *ctx,
				     unsigned long arg)
{
	__s64 ret;
	struct upfdio_copy upfdio_copy;
	struct upfdio_copy __user *user_upfdio_copy;

	user_upfdio_copy = (struct upfdio_copy __user *)arg;

	ret = -EFAULT;
	if (copy_from_user(&upfdio_copy, user_upfdio_copy,
			   sizeof(struct upfdio_copy)))
		goto out;

	ret = validate_range(current->mm, upfdio_copy.dst, upfdio_copy.len);
	if (ret)
		goto out;
	ret = -EINVAL;
	if (upfdio_copy.src + upfdio_copy.len <= upfdio_copy.src)
		goto out;
	ret = mpopulate_copy_atomic(current->mm, upfdio_copy.dst,
				    upfdio_copy.src, upfdio_copy.len);

out:
	return ret;
}

static int userpopulatefd_ioctl_read(struct userfaultfd_ctx *ctx,
				     unsigned long arg)
{
	__s64 ret, page_count;
	int i;
	struct upfdio_rw upfdio_rw;
	struct upfdio_rw __user *user_upfdio_rw;

	user_upfdio_rw = (struct upfdio_rw __user *)arg;

	ret = -EFAULT;
	if (copy_from_user(&upfdio_rw, user_upfdio_rw,
			   /* don't copy "rw" last field */
			   sizeof(struct upfdio_rw) - sizeof(__s64)))
		goto out;

	ret = validate_range(current->mm, upfdio_rw.addr, upfdio_rw.len);
	if (ret)
		goto out;

	page_count = (__s64)(upfdio_rw.len / PAGE_SIZE);

	ret = -EFAULT;
	for (i = 0; i < page_count; i++) {
		if (copy_to_user((void __user *)(upfdio_rw.addr + i * PAGE_SIZE),
				 kernel_buffer, PAGE_SIZE))
			goto out;
	}

	if (unlikely(put_user(page_count * PAGE_SIZE, &user_upfdio_rw->ret)))
		return -EFAULT;

	ret = (page_count * PAGE_SIZE == upfdio_rw.len) ? 0 : -EFAULT;
out:
	return ret;
}

static int userpopulatefd_ioctl_write(struct userfaultfd_ctx *ctx,
				      unsigned long arg)
{
	__s64 ret;

	struct upfdio_rw upfdio_rw;
	struct upfdio_rw __user *user_upfdio_rw;

	user_upfdio_rw = (struct upfdio_rw __user *)arg;

	ret = -EFAULT;
	if (copy_from_user(&upfdio_rw, user_upfdio_rw,
			   /* don't copy "rw" last field */
			   sizeof(struct upfdio_rw) - sizeof(__s64)))
		goto out;

	if (upfdio_rw.len != PAGE_SIZE)
		return -EINVAL;

	if (copy_from_user(kernel_buffer, (void __user *)upfdio_rw.addr,
			   upfdio_rw.len))
		goto out;
	ret = 0;
out:
	return ret;
}

static long userpopulatefd_ioctl(struct file *file, unsigned int cmd,
				 unsigned long arg)
{
	int ret = -EINVAL;
	switch (cmd) {
	case UPFDIO_GRAFT:
		ret = userpopulatefd_ioctl_graft(NULL, arg);
		break;
	case UPFDIO_COPY:
		ret = userpopulatefd_ioctl_copy(NULL, arg);
		break;
	case UPFDIO_READ:
		ret = userpopulatefd_ioctl_read(NULL, arg);
		break;
	case UPFDIO_WRITE:
		ret = userpopulatefd_ioctl_write(NULL, arg);
		break;
	default:
		ret = -ENOTTY; // Unsupported command
	}
	return ret;
}

static int userpopulatefd_open(struct inode *inode, struct file *file)
{
	return 0;
}

static int userpopulatefd_release(struct inode *inode, struct file *file)
{
	return 0;
}

static ssize_t userpopulatefd_read(struct file *file, char __user *user_buf,
				   size_t count, loff_t *ppos)
{
	int bytes_to_copy = min(strlen(kernel_buffer), count);

	if (copy_to_user(user_buf, kernel_buffer, bytes_to_copy)) {
		return -EFAULT;
	}
	return bytes_to_copy;
}

static ssize_t userpopulatefd_write(struct file *file,
				    const char __user *user_buf, size_t count,
				    loff_t *ppos)
{
	int bytes_to_copy = min(sizeof(kernel_buffer) - 1, count);

	if (copy_from_user(kernel_buffer, user_buf, bytes_to_copy)) {
		return -EFAULT;
	}
	kernel_buffer[bytes_to_copy] = '\0';
	return bytes_to_copy;
}

static const struct file_operations userpopulatefd_fops = {
	.owner = THIS_MODULE,
	.open = userpopulatefd_open,
	.release = userpopulatefd_release,
	.read = userpopulatefd_read,
	.write = userpopulatefd_write,
	.unlocked_ioctl = userpopulatefd_ioctl,
};

static int __init userpopulatefd_init(void)
{
	int ret;

	if (alloc_chrdev_region(&dev, 0, 1, DEVICE_NAME) < 0) {
		pr_alert("Failed to allocate character device region\n");
		return -1;
	}

	cdev_init(&cdev, &userpopulatefd_fops);
	if ((ret = cdev_add(&cdev, dev, 1)) < 0) {
		unregister_chrdev_region(dev, 1);
		pr_alert("Failed to add character device\n");
		return ret;
	}

	userpopulatefd_class = class_create(THIS_MODULE, DEVICE_NAME);
	if (IS_ERR(userpopulatefd_class)) {
		cdev_del(&cdev);
		unregister_chrdev_region(dev, 1);
		pr_alert("Failed to create class\n");
		return PTR_ERR(userpopulatefd_class);
	}

	userpopulatefd_device = device_create(userpopulatefd_class, NULL, dev,
					      NULL, DEVICE_NAME);
	if (IS_ERR(userpopulatefd_device)) {
		class_destroy(userpopulatefd_class);
		cdev_del(&cdev);
		unregister_chrdev_region(dev, 1);
		pr_alert("Failed to create device\n");
		return PTR_ERR(userpopulatefd_device);
	}

	pr_info("userpopulatefd module loaded\n");
	return 0;
}

static void __exit userpopulatefd_exit(void)
{
	device_destroy(userpopulatefd_class, dev);
	class_destroy(userpopulatefd_class);
	cdev_del(&cdev);
	unregister_chrdev_region(dev, 1);
}

module_init(userpopulatefd_init);
module_exit(userpopulatefd_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Kingdo");
MODULE_DESCRIPTION("A simple character device driver");