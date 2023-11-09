// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Virtio balloon implementation, inspired by Dor Laor and Marcelo
 * Tosatti's implementations.
 *
 *  Copyright 2008 Rusty Russell IBM Corporation
 */

#include <linux/virtio.h>
#include <linux/virtio_balloon.h>
#include <linux/swap.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/balloon_compaction.h>
#include <linux/oom.h>
#include <linux/wait.h>
#include <linux/mm.h>
#include <linux/mount.h>
#include <linux/magic.h>
#include <linux/pseudo_fs.h>
#include <linux/page_reporting.h>

/*
 * Balloon device works in 4K page units.  So each page is pointed to by
 * multiple balloon pages.  All memory counters in this driver are in balloon
 * page units.
 */
/**
 * 以下的理解是不正确的，是对大页的理解存在问题，并不是说采用大页，那么Page_Size就变了
 * Page_size是4KB这个依然是不变的。=，这个属于非常底层的内核配置，大页也要在其基础之上
 * */
/**
 * 内存气球的page大小是4KB，因此在内核中一个物理页可能对应于多个balloon-page，这是对于大页而言的，
 * 默认内核中使用的page大小也是4KB，因此基本上一个balloon-page对应一个物理内存page。
 * 因此在balloon设备驱动中存在很多页大小的转化代码，这都是为了处理大页的情况。
 * 只需要记住balloon是以4KB为单位进行缩放的！
 * */

/// 每个物理页page，包含多少个balloon-page，默认是1
#define VIRTIO_BALLOON_PAGES_PER_PAGE (unsigned)(PAGE_SIZE >> VIRTIO_BALLOON_PFN_SHIFT)
/// 用于和virtio后端进行通讯的存放pfn的数组的大小，即每次传送256*4K=1MB大小的内存
#define VIRTIO_BALLOON_ARRAY_PFNS_MAX 256
/* Maximum number of (4k) pages to deflate on OOM notifications. */
#define VIRTIO_BALLOON_OOM_NR_PAGES 256
#define VIRTIO_BALLOON_OOM_NOTIFY_PRIORITY 80

/// 向伙伴系统申请空闲页时，使用的falgs
#define VIRTIO_BALLOON_FREE_PAGE_ALLOC_FLAG (__GFP_NORETRY | __GFP_NOWARN | \
					     __GFP_NOMEMALLOC)
/* The order of free page blocks to report to host */
#define VIRTIO_BALLOON_HINT_BLOCK_ORDER (MAX_ORDER - 1)
/* The size of a free page block in bytes */
#define VIRTIO_BALLOON_HINT_BLOCK_BYTES \
	(1 << (VIRTIO_BALLOON_HINT_BLOCK_ORDER + PAGE_SHIFT))
#define VIRTIO_BALLOON_HINT_BLOCK_PAGES (1 << VIRTIO_BALLOON_HINT_BLOCK_ORDER)

#define ENABLE_BALLOON_STATS_TIMER 1

#ifdef CONFIG_BALLOON_COMPACTION
static struct vfsmount *balloon_mnt;
#endif

enum virtio_balloon_vq {
	VIRTIO_BALLOON_VQ_INFLATE,
	VIRTIO_BALLOON_VQ_DEFLATE,
	VIRTIO_BALLOON_VQ_STATS,
	VIRTIO_BALLOON_VQ_FREE_PAGE,
	VIRTIO_BALLOON_VQ_REPORTING,
	VIRTIO_BALLOON_VQ_MAX
};

enum virtio_balloon_config_read {
	VIRTIO_BALLOON_CONFIG_READ_CMD_ID = 0,
};

struct virtio_balloon {
	struct virtio_device *vdev;
	struct virtqueue *inflate_vq, *deflate_vq, *stats_vq, *free_page_vq;

	/* Balloon's own wq for cpu-intensive work items */
	struct workqueue_struct *balloon_wq;
	/* The free page reporting work item submitted to the balloon wq */
	struct work_struct report_free_page_work;

	/* The balloon servicing is delegated to a freezable workqueue. */
	struct work_struct update_balloon_stats_work;
	struct work_struct update_balloon_size_work;

	/* Prevent updating balloon when it is being canceled. */
	spinlock_t stop_update_lock;
	bool stop_update;
	/* Bitmap to indicate if reading the related config fields are needed */
	unsigned long config_read_bitmap;

	/* The list of allocated free pages, waiting to be given back to mm */
	struct list_head free_page_list;
	spinlock_t free_page_list_lock;
	/* The number of free page blocks on the above list */
	unsigned long num_free_page_blocks;
	/*
	 * The cmd id received from host.
	 * Read it via virtio_balloon_cmd_id_received to get the latest value
	 * sent from host.
	 */
	u32 cmd_id_received_cache;
	/* The cmd id that is actively in use */
	__virtio32 cmd_id_active;
	/* Buffer to store the stop sign */
	__virtio32 cmd_id_stop;

	/* Waiting for host to ack the pages we released. */
	wait_queue_head_t acked;

	/* Number of balloon pages we've told the Host we're not using. */
	unsigned int num_pages;
	/*
	 * The pages we've told the Host we're not using are enqueued
	 * at vb_dev_info->pages list.
	 * Each page on this list adds VIRTIO_BALLOON_PAGES_PER_PAGE
	 * to num_pages above.
	 */
	struct balloon_dev_info vb_dev_info;

	/* Synchronize access/update to this struct virtio_balloon elements */
	struct mutex balloon_lock;

	/* The array of pfns we tell the Host about. */
	unsigned int num_pfns;
	__virtio32 pfns[VIRTIO_BALLOON_ARRAY_PFNS_MAX];

	/* Memory statistics */
	struct virtio_balloon_stat stats[VIRTIO_BALLOON_S_NR];

	/* Shrinker to return free pages - VIRTIO_BALLOON_F_FREE_PAGE_HINT */
	struct shrinker shrinker;

	/* OOM notifier to deflate on OOM - VIRTIO_BALLOON_F_DEFLATE_ON_OOM */
	struct notifier_block oom_nb;

	/* Free page reporting device */
	struct virtqueue *reporting_vq;
	struct page_reporting_dev_info pr_dev_info;
};

static const struct virtio_device_id id_table[] = {
	{ VIRTIO_ID_BALLOON, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

static u32 page_to_balloon_pfn(struct page *page)
{
	unsigned long pfn = page_to_pfn(page);

	BUILD_BUG_ON(PAGE_SHIFT < VIRTIO_BALLOON_PFN_SHIFT);
	/* Convert pfn from Linux page size to balloon page size. */
	return pfn * VIRTIO_BALLOON_PAGES_PER_PAGE;
}

/// 一个回调函数，当hypervisor执行完缩放气球的操作后，会通知内核，而内核会调用该回调，从而唤醒阻塞在vb->acked
/// 上的线程，即fill-balloon和leak-balloon操作
static void balloon_ack(struct virtqueue *vq)
{
	struct virtio_balloon *vb = vq->vdev->priv;

	wake_up(&vb->acked);
}

/** 在扩展或缩小气球大小后，都要通知并等待hypervisor释放或者重新映射页面
 * 当fill-balloon时，vq 是 inflate_vq，此时vb->pfns内容使我们刚刚从伙伴系统申请的，需要被释放的page，
 * 页面发送到hypervisor后，将会取消对这些页面的映射(滞后执行)，然后这写页面就可以重分配给其他的VM了
 *
 * 相反，当leak-balloon时，vq 是 deflate_vq，此时vb->pfns是我们需要需要回收到伙伴系统的页面，此时hypervisor
 * 需要为我们重新建立这写页面的映射
*/
static void tell_host(struct virtio_balloon *vb, struct virtqueue *vq)
{
	/// 包含地址和大小的一个数据结构
	struct scatterlist sg;
	unsigned int len;

	sg_init_one(&sg, vb->pfns, sizeof(vb->pfns[0]) * vb->num_pfns);

	/* We should always be able to add one buffer to an empty queue. */
	virtqueue_add_outbuf(vq, &sg, 1, vb, GFP_KERNEL);
	virtqueue_kick(vq);

	/* When host has read buffer, this completes via balloon_ack */
	/// 阻塞等待，hypervisor返回，即等待balloon_ack函数的执行
	wait_event(vb->acked, virtqueue_get_buf(vq, &len));

}

static int virtballoon_free_page_report(struct page_reporting_dev_info *pr_dev_info,
				   struct scatterlist *sg, unsigned int nents)
{
	struct virtio_balloon *vb =
		container_of(pr_dev_info, struct virtio_balloon, pr_dev_info);
	struct virtqueue *vq = vb->reporting_vq;
	unsigned int unused, err;

	/* We should always be able to add these buffers to an empty queue. */
	err = virtqueue_add_inbuf(vq, sg, nents, vb, GFP_NOWAIT | __GFP_NOWARN);

	/*
	 * In the extremely unlikely case that something has occurred and we
	 * are able to trigger an error we will simply display a warning
	 * and exit without actually processing the pages.
	 */
	if (WARN_ON_ONCE(err))
		return err;

	virtqueue_kick(vq);

	/* When host has read buffer, this completes via balloon_ack */
	wait_event(vb->acked, virtqueue_get_buf(vq, &unused));

	return 0;
}

static void set_page_pfns(struct virtio_balloon *vb,
			  __virtio32 pfns[], struct page *page)
{
	unsigned int i;

	BUILD_BUG_ON(VIRTIO_BALLOON_PAGES_PER_PAGE > VIRTIO_BALLOON_ARRAY_PFNS_MAX);

	/*
	 * Set balloon pfns pointing at this page.
	 * Note that the first pfn points at start of the page.
	 */
	for (i = 0; i < VIRTIO_BALLOON_PAGES_PER_PAGE; i++)
		pfns[i] = cpu_to_virtio32(vb->vdev,
					  page_to_balloon_pfn(page) + i);
}

static unsigned fill_balloon(struct virtio_balloon *vb, size_t num)
{
	unsigned num_allocated_pages;
	unsigned num_pfns;
	struct page *page;
#ifdef ENABLE_BALLOON_STATS_TIMER
	ktime_t start_time;
#endif
	/// 申请的扩展到balloon的物理页page的组成的链表
	LIST_HEAD(pages);
#ifdef ENABLE_BALLOON_STATS_TIMER
	start_time = ktime_get();
#endif

	/* We can only do one array worth at a time. */
	/// vb->pfns是记录本次扩缩操作那些物理页将被扩展到气球当中，数值即pfn，此数组的最大值是 VIRTIO_BALLOON_ARRAY_PFNS_MAX = 256
	/// 也就是说每次最多缩放1MB大小的内存，因此这里的num就是本次扩展需要从伙伴系统拿走多少个balloon-page
	num = min(num, ARRAY_SIZE(vb->pfns));

	/// VIRTIO_BALLOON_PAGES_PER_PAGE 是每个物理page包含多少个balloon-page，因为balloon-page大小是4KB
	/// 如果系统内存页采用大小，那么一个物理页将包含多个balloon-page，但是一般是物理页等于气球页都是4KB
	for (num_pfns = 0; num_pfns < num;
	     num_pfns += VIRTIO_BALLOON_PAGES_PER_PAGE) {
		/// balloon_page_alloc 在mm/balloon_compaction.c中实现，这是kernel给实现balloon留下的接口，因为实现内存气球
		/// 的技术有很多，比如xen-balloon，因此kernel留了统一的关于内存的申请和释放的接口
		struct page *page = balloon_page_alloc();

		if (!page) {
			dev_info_ratelimited(&vb->vdev->dev,
					     "Out of puff! Can't get %u pages\n",
					     VIRTIO_BALLOON_PAGES_PER_PAGE);
			/* Sleep for at least 1/5 of a second before retry. */
			/// 注意，此代码是在workqueue中执行的，因此可以执行sleep
			msleep(200);
			break;
		}

		balloon_page_push(&pages, page);
	}

	mutex_lock(&vb->balloon_lock);

	vb->num_pfns = 0;

	while ((page = balloon_page_pop(&pages))) {
		/// vb->vb_dev_info 类型是 balloon_dev_info，同样在mm/balloon_compaction.h中定义，
		/// 用于记录那些page被添加到了balloon中，balloon_page_enqueue 即入队操作
		balloon_page_enqueue(&vb->vb_dev_info, page);
		/// 将page写入vb->pfns数组，vb->num_pfns 暂作为数组索引
		set_page_pfns(vb, vb->pfns + vb->num_pfns, page);
		/// 记录气球的实际大小
		vb->num_pages += VIRTIO_BALLOON_PAGES_PER_PAGE;
		/// 这一步，应该修改可用内存的大小，即通过free调用查看到的可用内存数目
		/// 最终修改的是全局变量 _totalram_pages
		if (!virtio_has_feature(vb->vdev,
					VIRTIO_BALLOON_F_DEFLATE_ON_OOM))
			adjust_managed_page_count(page, -1);
		vb->num_pfns += VIRTIO_BALLOON_PAGES_PER_PAGE;
	}
	/// 用于返回，实际扩展了多少page
	num_allocated_pages = vb->num_pfns;
#ifdef ENABLE_BALLOON_STATS_TIMER
	pr_info("fill_balloon uses %lld us\n", ktime_to_us(ktime_sub(ktime_get(), start_time)));
	start_time = ktime_get();
#endif
	/* Did we get any? */
	/// 通知hypervisor的后端驱动，回收我们收集到气球中的页面
	if (vb->num_pfns != 0)
		tell_host(vb, vb->inflate_vq);
	mutex_unlock(&vb->balloon_lock);
#ifdef ENABLE_BALLOON_STATS_TIMER
	pr_info("fill_tell_host uses %lld us\n", ktime_to_us(ktime_sub(ktime_get(), start_time)));
#endif
	return num_allocated_pages;
}

static void release_pages_balloon(struct virtio_balloon *vb,
				 struct list_head *pages)
{
	struct page *page, *next;

	list_for_each_entry_safe(page, next, pages, lru) {
		if (!virtio_has_feature(vb->vdev,
					VIRTIO_BALLOON_F_DEFLATE_ON_OOM))
			/// 增加全局变量 _totalram_pages 数目
			adjust_managed_page_count(page, 1);
		list_del(&page->lru);
		/// 归还页面给伙伴系统，注意，这里的操作仅仅是减少一次引用，因为理论上被balloon拿到的page的
		/// 引用大小是1，减少一次之后就变成了0，那么就会被自动回收，而不是直接调用伙伴系统
		put_page(page); /* balloon reference */
	}
}

static unsigned leak_balloon(struct virtio_balloon *vb, size_t num)
{
	unsigned num_freed_pages;
	struct page *page;
#ifdef ENABLE_BALLOON_STATS_TIMER
	ktime_t start_time;
	int used_time;
#endif
	struct balloon_dev_info *vb_dev_info = &vb->vb_dev_info;
	LIST_HEAD(pages);
#ifdef ENABLE_BALLOON_STATS_TIMER
	start_time = ktime_get();
#endif

	/* We can only do one array worth at a time. */
	num = min(num, ARRAY_SIZE(vb->pfns));

	mutex_lock(&vb->balloon_lock);
	/* We can't release more pages than taken */
	num = min(num, (size_t)vb->num_pages);
	for (vb->num_pfns = 0; vb->num_pfns < num;
	     vb->num_pfns += VIRTIO_BALLOON_PAGES_PER_PAGE) {
		/// 与前面fill-balloon操作相反，此时需要从vb_dev_info中获取要被回收给伙伴系统的page
		page = balloon_page_dequeue(vb_dev_info);
		if (!page)
			break;
		/// 将这写应该从气球释放的页面写入vb->pfns，和pages列表，前者用于和hypervisor通讯，后者用于归还给伙伴系统
		set_page_pfns(vb, vb->pfns + vb->num_pfns, page);
		list_add(&page->lru, &pages);
		vb->num_pages -= VIRTIO_BALLOON_PAGES_PER_PAGE;
	}

	num_freed_pages = vb->num_pfns;
#ifdef ENABLE_BALLOON_STATS_TIMER
	used_time = ktime_to_us(ktime_sub(ktime_get(), start_time));
	start_time = ktime_get();
#endif
	/*
	 * Note that if
	 * virtio_has_feature(vdev, VIRTIO_BALLOON_F_MUST_TELL_HOST);
	 * is true, we *have* to do it in this order
	 */
	/// 通知hypervisor重新建立映射
	if (vb->num_pfns != 0)
		tell_host(vb, vb->deflate_vq);
#ifdef ENABLE_BALLOON_STATS_TIMER
	pr_info("leak_tell_host uses %lld us\n",  ktime_to_us(ktime_sub(ktime_get(), start_time)));
	start_time = ktime_get();
#endif
	/// 回收页面给伙伴系统，同时增加 _totalram_pages，对应fill-balloon减小_totalram_pages的操作
	release_pages_balloon(vb, &pages);
	mutex_unlock(&vb->balloon_lock);
#ifdef ENABLE_BALLOON_STATS_TIMER
	used_time += ktime_to_us(ktime_sub(ktime_get(), start_time));
	pr_info("leak_balloon uses %u us\n", used_time);
#endif
	return num_freed_pages;
}

static inline void update_stat(struct virtio_balloon *vb, int idx,
			       u16 tag, u64 val)
{
	BUG_ON(idx >= VIRTIO_BALLOON_S_NR);
	vb->stats[idx].tag = cpu_to_virtio16(vb->vdev, tag);
	vb->stats[idx].val = cpu_to_virtio64(vb->vdev, val);
}

#define pages_to_bytes(x) ((u64)(x) << PAGE_SHIFT)

static unsigned int update_balloon_stats(struct virtio_balloon *vb)
{
	unsigned long events[NR_VM_EVENT_ITEMS];
	struct sysinfo i;
	unsigned int idx = 0;
	long available;
	unsigned long caches;

	all_vm_events(events);
	si_meminfo(&i);

	available = si_mem_available();
	caches = global_node_page_state(NR_FILE_PAGES);

#ifdef CONFIG_VM_EVENT_COUNTERS
	update_stat(vb, idx++, VIRTIO_BALLOON_S_SWAP_IN,
				pages_to_bytes(events[PSWPIN]));
	update_stat(vb, idx++, VIRTIO_BALLOON_S_SWAP_OUT,
				pages_to_bytes(events[PSWPOUT]));
	update_stat(vb, idx++, VIRTIO_BALLOON_S_MAJFLT, events[PGMAJFAULT]);
	update_stat(vb, idx++, VIRTIO_BALLOON_S_MINFLT, events[PGFAULT]);
#ifdef CONFIG_HUGETLB_PAGE
	update_stat(vb, idx++, VIRTIO_BALLOON_S_HTLB_PGALLOC,
		    events[HTLB_BUDDY_PGALLOC]);
	update_stat(vb, idx++, VIRTIO_BALLOON_S_HTLB_PGFAIL,
		    events[HTLB_BUDDY_PGALLOC_FAIL]);
#endif
#endif
	update_stat(vb, idx++, VIRTIO_BALLOON_S_MEMFREE,
				pages_to_bytes(i.freeram));
	update_stat(vb, idx++, VIRTIO_BALLOON_S_MEMTOT,
				pages_to_bytes(i.totalram));
	update_stat(vb, idx++, VIRTIO_BALLOON_S_AVAIL,
				pages_to_bytes(available));
	update_stat(vb, idx++, VIRTIO_BALLOON_S_CACHES,
				pages_to_bytes(caches));

	return idx;
}

/*
 * While most virtqueues communicate guest-initiated requests to the hypervisor,
 * the stats queue operates in reverse.  The driver initializes the virtqueue
 * with a single buffer.  From that point forward, all conversations consist of
 * a hypervisor request (a call to this function) which directs us to refill
 * the virtqueue with a fresh stats buffer.  Since stats collection can sleep,
 * we delegate the job to a freezable workqueue that will do the actual work via
 * stats_handle_request().
 */
static void stats_request(struct virtqueue *vq)
{
	struct virtio_balloon *vb = vq->vdev->priv;

	spin_lock(&vb->stop_update_lock);
	if (!vb->stop_update)
		queue_work(system_freezable_wq, &vb->update_balloon_stats_work);
	spin_unlock(&vb->stop_update_lock);
}

static void stats_handle_request(struct virtio_balloon *vb)
{
	struct virtqueue *vq;
	struct scatterlist sg;
	unsigned int len, num_stats;

	num_stats = update_balloon_stats(vb);

	vq = vb->stats_vq;
	if (!virtqueue_get_buf(vq, &len))
		return;
	sg_init_one(&sg, vb->stats, sizeof(vb->stats[0]) * num_stats);
	virtqueue_add_outbuf(vq, &sg, 1, vb, GFP_KERNEL);
	virtqueue_kick(vq);
}

/**
 * 返回balloon需要缩放的数值
 * 如，要气球缩小1MB(先`balloon 1023` -> 再`balloon 1024`),则target值为-256
 * 如，要气球扩大1MB(初始内存为1024，然后执行`balloon 1023`)，则target为256
 * */
static inline s64 towards_target(struct virtio_balloon *vb)
{
	s64 target;
	u32 num_pages;

	/**
	 * vb->vdev->config 类型是 virtio_config_ops
	 * 以下操作为，调用virtio_config_ops中的get勾子函数来获取 virtio_balloon_config.num_pages
	 * 然后将其赋值给num_pages，virtio_balloon_config.num_pages记录了每次请求hypervisor希望
	 * 气球变成多大，是一个无符号整数
	 * */
	/* Legacy balloon config space is LE, unlike all other devices. */
	virtio_cread_le(vb->vdev, struct virtio_balloon_config, num_pages,
			&num_pages);

	target = num_pages;
	/**
	 * vb->num_pages 记录的是气球当前的大小，当缩放完成后会修改此值，然后赋值给 virtio_balloon_config.actual
	 * */
	return target - vb->num_pages;
}

/* Gives back @num_to_return blocks of free pages to mm. */
static unsigned long return_free_pages_to_mm(struct virtio_balloon *vb,
					     unsigned long num_to_return)
{
	struct page *page;
	unsigned long num_returned;

	spin_lock_irq(&vb->free_page_list_lock);
	for (num_returned = 0; num_returned < num_to_return; num_returned++) {
		page = balloon_page_pop(&vb->free_page_list);
		if (!page)
			break;
		free_pages((unsigned long)page_address(page),
			   VIRTIO_BALLOON_HINT_BLOCK_ORDER);
	}
	vb->num_free_page_blocks -= num_returned;
	spin_unlock_irq(&vb->free_page_list_lock);

	return num_returned;
}

static void virtio_balloon_queue_free_page_work(struct virtio_balloon *vb)
{
	if (!virtio_has_feature(vb->vdev, VIRTIO_BALLOON_F_FREE_PAGE_HINT))
		return;

	/* No need to queue the work if the bit was already set. */
	if (test_and_set_bit(VIRTIO_BALLOON_CONFIG_READ_CMD_ID,
			     &vb->config_read_bitmap))
		return;

	queue_work(vb->balloon_wq, &vb->report_free_page_work);
}

/**
 * virtballoon_changed 是 balloon前端驱动 virtio_balloon_driver 结构中 config_changed的回调函数
 * 即，当balloon的配置发生改变时将调用此函数，对应了baloon的fill和leak操作
 * */

static void virtballoon_changed(struct virtio_device *vdev)
{
	struct virtio_balloon *vb = vdev->priv;
	unsigned long flags;

	spin_lock_irqsave(&vb->stop_update_lock, flags);
	if (!vb->stop_update) {
		/// balloon的size更新操作是由workqueue执行的，update_balloon_size_work在probe函数中初始化
		/// update_balloon_size_work将调用update_balloon_size_func执行具体的修改过程
		/// workqueue的使用使得整个过程是异步的，因此qemu在执行`balloon 1023`调节内存大小后会立即返回
		queue_work(system_freezable_wq,
			   &vb->update_balloon_size_work);
		virtio_balloon_queue_free_page_work(vb);
	}
	spin_unlock_irqrestore(&vb->stop_update_lock, flags);
}
/// 修改virtio_balloon_config.actual的值，使之等于virtio_balloon_config.num_pages
static void update_balloon_size(struct virtio_balloon *vb)
{
	u32 actual = vb->num_pages;

	/* Legacy balloon config space is LE, unlike all other devices. */
	virtio_cwrite_le(vb->vdev, struct virtio_balloon_config, actual,
			 &actual);
}

static void update_balloon_stats_func(struct work_struct *work)
{
	struct virtio_balloon *vb;

	vb = container_of(work, struct virtio_balloon,
			  update_balloon_stats_work);
	stats_handle_request(vb);
}
/**
 * fill/leak balloon的具体执行的函数，是workqueue update_balloon_size_work的入口函数
 *
 * */
static void update_balloon_size_func(struct work_struct *work)
{
	struct virtio_balloon *vb;
	s64 diff;

	vb = container_of(work, struct virtio_balloon,
			  update_balloon_size_work);
	/// 获取要缩放气球的大小，整数表示扩大气球，负数表示缩小气球
	/// 扩大气球等价于减少guest VM的可用内存
	diff = towards_target(vb);

	if (!diff)
		return;
	/// 根据diff的符号，选择指定的函数，需要注意，每次只能缩放1MB
	if (diff > 0)
		diff -= fill_balloon(vb, diff);
	else
		diff += leak_balloon(vb, -diff);
	/// 修改virtio_balloon_config.actual的值，使之等于virtio_balloon_config.num_pages
	update_balloon_size(vb);

	if (diff)
		queue_work(system_freezable_wq, work);
}

static int init_vqs(struct virtio_balloon *vb)
{
	struct virtqueue *vqs[VIRTIO_BALLOON_VQ_MAX];
	vq_callback_t *callbacks[VIRTIO_BALLOON_VQ_MAX];
	const char *names[VIRTIO_BALLOON_VQ_MAX];
	int err;

	/*
	 * Inflateq and deflateq are used unconditionally. The names[]
	 * will be NULL if the related feature is not enabled, which will
	 * cause no allocation for the corresponding virtqueue in find_vqs.
	 */
	callbacks[VIRTIO_BALLOON_VQ_INFLATE] = balloon_ack;
	names[VIRTIO_BALLOON_VQ_INFLATE] = "inflate";
	callbacks[VIRTIO_BALLOON_VQ_DEFLATE] = balloon_ack;
	names[VIRTIO_BALLOON_VQ_DEFLATE] = "deflate";
	callbacks[VIRTIO_BALLOON_VQ_STATS] = NULL;
	names[VIRTIO_BALLOON_VQ_STATS] = NULL;
	callbacks[VIRTIO_BALLOON_VQ_FREE_PAGE] = NULL;
	names[VIRTIO_BALLOON_VQ_FREE_PAGE] = NULL;
	names[VIRTIO_BALLOON_VQ_REPORTING] = NULL;

	if (virtio_has_feature(vb->vdev, VIRTIO_BALLOON_F_STATS_VQ)) {
		names[VIRTIO_BALLOON_VQ_STATS] = "stats";
		callbacks[VIRTIO_BALLOON_VQ_STATS] = stats_request;
	}

	if (virtio_has_feature(vb->vdev, VIRTIO_BALLOON_F_FREE_PAGE_HINT)) {
		names[VIRTIO_BALLOON_VQ_FREE_PAGE] = "free_page_vq";
		callbacks[VIRTIO_BALLOON_VQ_FREE_PAGE] = NULL;
	}

	if (virtio_has_feature(vb->vdev, VIRTIO_BALLOON_F_REPORTING)) {
		names[VIRTIO_BALLOON_VQ_REPORTING] = "reporting_vq";
		callbacks[VIRTIO_BALLOON_VQ_REPORTING] = balloon_ack;
	}

	err = vb->vdev->config->find_vqs(vb->vdev, VIRTIO_BALLOON_VQ_MAX,
					 vqs, callbacks, names, NULL, NULL);
	if (err)
		return err;

	vb->inflate_vq = vqs[VIRTIO_BALLOON_VQ_INFLATE];
	vb->deflate_vq = vqs[VIRTIO_BALLOON_VQ_DEFLATE];
	if (virtio_has_feature(vb->vdev, VIRTIO_BALLOON_F_STATS_VQ)) {
		struct scatterlist sg;
		unsigned int num_stats;
		vb->stats_vq = vqs[VIRTIO_BALLOON_VQ_STATS];

		/*
		 * Prime this virtqueue with one buffer so the hypervisor can
		 * use it to signal us later (it can't be broken yet!).
		 */
		num_stats = update_balloon_stats(vb);

		sg_init_one(&sg, vb->stats, sizeof(vb->stats[0]) * num_stats);
		err = virtqueue_add_outbuf(vb->stats_vq, &sg, 1, vb,
					   GFP_KERNEL);
		if (err) {
			dev_warn(&vb->vdev->dev, "%s: add stat_vq failed\n",
				 __func__);
			return err;
		}
		virtqueue_kick(vb->stats_vq);
	}

	if (virtio_has_feature(vb->vdev, VIRTIO_BALLOON_F_FREE_PAGE_HINT))
		vb->free_page_vq = vqs[VIRTIO_BALLOON_VQ_FREE_PAGE];

	if (virtio_has_feature(vb->vdev, VIRTIO_BALLOON_F_REPORTING))
		vb->reporting_vq = vqs[VIRTIO_BALLOON_VQ_REPORTING];

	return 0;
}

static u32 virtio_balloon_cmd_id_received(struct virtio_balloon *vb)
{
	if (test_and_clear_bit(VIRTIO_BALLOON_CONFIG_READ_CMD_ID,
			       &vb->config_read_bitmap)) {
		/* Legacy balloon config space is LE, unlike all other devices. */
		virtio_cread_le(vb->vdev, struct virtio_balloon_config,
				free_page_hint_cmd_id,
				&vb->cmd_id_received_cache);
	}

	return vb->cmd_id_received_cache;
}

static int send_cmd_id_start(struct virtio_balloon *vb)
{
	struct scatterlist sg;
	struct virtqueue *vq = vb->free_page_vq;
	int err, unused;

	/* Detach all the used buffers from the vq */
	while (virtqueue_get_buf(vq, &unused))
		;

	vb->cmd_id_active = cpu_to_virtio32(vb->vdev,
					virtio_balloon_cmd_id_received(vb));
	sg_init_one(&sg, &vb->cmd_id_active, sizeof(vb->cmd_id_active));
	err = virtqueue_add_outbuf(vq, &sg, 1, &vb->cmd_id_active, GFP_KERNEL);
	if (!err)
		virtqueue_kick(vq);
	return err;
}

static int send_cmd_id_stop(struct virtio_balloon *vb)
{
	struct scatterlist sg;
	struct virtqueue *vq = vb->free_page_vq;
	int err, unused;

	/* Detach all the used buffers from the vq */
	while (virtqueue_get_buf(vq, &unused))
		;

	sg_init_one(&sg, &vb->cmd_id_stop, sizeof(vb->cmd_id_stop));
	err = virtqueue_add_outbuf(vq, &sg, 1, &vb->cmd_id_stop, GFP_KERNEL);
	if (!err)
		virtqueue_kick(vq);
	return err;
}

static int get_free_page_and_send(struct virtio_balloon *vb)
{
	struct virtqueue *vq = vb->free_page_vq;
	struct page *page;
	struct scatterlist sg;
	int err, unused;
	void *p;

	/* Detach all the used buffers from the vq */
	while (virtqueue_get_buf(vq, &unused))
		;

	page = alloc_pages(VIRTIO_BALLOON_FREE_PAGE_ALLOC_FLAG,
			   VIRTIO_BALLOON_HINT_BLOCK_ORDER);
	/*
	 * When the allocation returns NULL, it indicates that we have got all
	 * the possible free pages, so return -EINTR to stop.
	 */
	if (!page)
		return -EINTR;

	p = page_address(page);
	sg_init_one(&sg, p, VIRTIO_BALLOON_HINT_BLOCK_BYTES);
	/* There is always 1 entry reserved for the cmd id to use. */
	if (vq->num_free > 1) {
		err = virtqueue_add_inbuf(vq, &sg, 1, p, GFP_KERNEL);
		if (unlikely(err)) {
			free_pages((unsigned long)p,
				   VIRTIO_BALLOON_HINT_BLOCK_ORDER);
			return err;
		}
		virtqueue_kick(vq);
		spin_lock_irq(&vb->free_page_list_lock);
		balloon_page_push(&vb->free_page_list, page);
		vb->num_free_page_blocks++;
		spin_unlock_irq(&vb->free_page_list_lock);
	} else {
		/*
		 * The vq has no available entry to add this page block, so
		 * just free it.
		 */
		free_pages((unsigned long)p, VIRTIO_BALLOON_HINT_BLOCK_ORDER);
	}

	return 0;
}

static int send_free_pages(struct virtio_balloon *vb)
{
	int err;
	u32 cmd_id_active;

	while (1) {
		/*
		 * If a stop id or a new cmd id was just received from host,
		 * stop the reporting.
		 */
		cmd_id_active = virtio32_to_cpu(vb->vdev, vb->cmd_id_active);
		if (unlikely(cmd_id_active !=
			     virtio_balloon_cmd_id_received(vb)))
			break;

		/*
		 * The free page blocks are allocated and sent to host one by
		 * one.
		 */
		err = get_free_page_and_send(vb);
		if (err == -EINTR)
			break;
		else if (unlikely(err))
			return err;
	}

	return 0;
}

static void virtio_balloon_report_free_page(struct virtio_balloon *vb)
{
	int err;
	struct device *dev = &vb->vdev->dev;

	/* Start by sending the received cmd id to host with an outbuf. */
	err = send_cmd_id_start(vb);
	if (unlikely(err))
		dev_err(dev, "Failed to send a start id, err = %d\n", err);

	err = send_free_pages(vb);
	if (unlikely(err))
		dev_err(dev, "Failed to send a free page, err = %d\n", err);

	/* End by sending a stop id to host with an outbuf. */
	err = send_cmd_id_stop(vb);
	if (unlikely(err))
		dev_err(dev, "Failed to send a stop id, err = %d\n", err);
}

static void report_free_page_func(struct work_struct *work)
{
	struct virtio_balloon *vb = container_of(work, struct virtio_balloon,
						 report_free_page_work);
	u32 cmd_id_received;

	cmd_id_received = virtio_balloon_cmd_id_received(vb);
	if (cmd_id_received == VIRTIO_BALLOON_CMD_ID_DONE) {
		/* Pass ULONG_MAX to give back all the free pages */
		return_free_pages_to_mm(vb, ULONG_MAX);
	} else if (cmd_id_received != VIRTIO_BALLOON_CMD_ID_STOP &&
		   cmd_id_received !=
		   virtio32_to_cpu(vb->vdev, vb->cmd_id_active)) {
		virtio_balloon_report_free_page(vb);
	}
}

#ifdef CONFIG_BALLOON_COMPACTION
/*
 * virtballoon_migratepage - perform the balloon page migration on behalf of
 *			     a compaction thread.     (called under page lock)
 * @vb_dev_info: the balloon device
 * @newpage: page that will replace the isolated page after migration finishes.
 * @page   : the isolated (old) page that is about to be migrated to newpage.
 * @mode   : compaction mode -- not used for balloon page migration.
 *
 * After a ballooned page gets isolated by compaction procedures, this is the
 * function that performs the page migration on behalf of a compaction thread
 * The page migration for virtio balloon is done in a simple swap fashion which
 * follows these two macro steps:
 *  1) insert newpage into vb->pages list and update the host about it;
 *  2) update the host about the old page removed from vb->pages list;
 *
 * This function preforms the balloon page migration task.
 * Called through balloon_mapping->a_ops->migratepage
 */
static int virtballoon_migratepage(struct balloon_dev_info *vb_dev_info,
		struct page *newpage, struct page *page, enum migrate_mode mode)
{
	struct virtio_balloon *vb = container_of(vb_dev_info,
			struct virtio_balloon, vb_dev_info);
	unsigned long flags;

	/*
	 * In order to avoid lock contention while migrating pages concurrently
	 * to leak_balloon() or fill_balloon() we just give up the balloon_lock
	 * this turn, as it is easier to retry the page migration later.
	 * This also prevents fill_balloon() getting stuck into a mutex
	 * recursion in the case it ends up triggering memory compaction
	 * while it is attempting to inflate the ballon.
	 */
	if (!mutex_trylock(&vb->balloon_lock))
		return -EAGAIN;

	get_page(newpage); /* balloon reference */

	/*
	  * When we migrate a page to a different zone and adjusted the
	  * managed page count when inflating, we have to fixup the count of
	  * both involved zones.
	  */
	if (!virtio_has_feature(vb->vdev, VIRTIO_BALLOON_F_DEFLATE_ON_OOM) &&
	    page_zone(page) != page_zone(newpage)) {
		adjust_managed_page_count(page, 1);
		adjust_managed_page_count(newpage, -1);
	}

	/* balloon's page migration 1st step  -- inflate "newpage" */
	spin_lock_irqsave(&vb_dev_info->pages_lock, flags);
	balloon_page_insert(vb_dev_info, newpage);
	vb_dev_info->isolated_pages--;
	__count_vm_event(BALLOON_MIGRATE);
	spin_unlock_irqrestore(&vb_dev_info->pages_lock, flags);
	vb->num_pfns = VIRTIO_BALLOON_PAGES_PER_PAGE;
	set_page_pfns(vb, vb->pfns, newpage);
	tell_host(vb, vb->inflate_vq);

	/* balloon's page migration 2nd step -- deflate "page" */
	spin_lock_irqsave(&vb_dev_info->pages_lock, flags);
	balloon_page_delete(page);
	spin_unlock_irqrestore(&vb_dev_info->pages_lock, flags);
	vb->num_pfns = VIRTIO_BALLOON_PAGES_PER_PAGE;
	set_page_pfns(vb, vb->pfns, page);
	tell_host(vb, vb->deflate_vq);

	mutex_unlock(&vb->balloon_lock);

	put_page(page); /* balloon reference */

	return MIGRATEPAGE_SUCCESS;
}

static int balloon_init_fs_context(struct fs_context *fc)
{
	return init_pseudo(fc, BALLOON_KVM_MAGIC) ? 0 : -ENOMEM;
}

static struct file_system_type balloon_fs = {
	.name           = "balloon-kvm",
	.init_fs_context = balloon_init_fs_context,
	.kill_sb        = kill_anon_super,
};

#endif /* CONFIG_BALLOON_COMPACTION */

static unsigned long shrink_free_pages(struct virtio_balloon *vb,
				       unsigned long pages_to_free)
{
	unsigned long blocks_to_free, blocks_freed;

	pages_to_free = round_up(pages_to_free,
				 VIRTIO_BALLOON_HINT_BLOCK_PAGES);
	blocks_to_free = pages_to_free / VIRTIO_BALLOON_HINT_BLOCK_PAGES;
	blocks_freed = return_free_pages_to_mm(vb, blocks_to_free);

	return blocks_freed * VIRTIO_BALLOON_HINT_BLOCK_PAGES;
}

static unsigned long virtio_balloon_shrinker_scan(struct shrinker *shrinker,
						  struct shrink_control *sc)
{
	struct virtio_balloon *vb = container_of(shrinker,
					struct virtio_balloon, shrinker);

	return shrink_free_pages(vb, sc->nr_to_scan);
}

static unsigned long virtio_balloon_shrinker_count(struct shrinker *shrinker,
						   struct shrink_control *sc)
{
	struct virtio_balloon *vb = container_of(shrinker,
					struct virtio_balloon, shrinker);

	return vb->num_free_page_blocks * VIRTIO_BALLOON_HINT_BLOCK_PAGES;
}

static int virtio_balloon_oom_notify(struct notifier_block *nb,
				     unsigned long dummy, void *parm)
{
	struct virtio_balloon *vb = container_of(nb,
						 struct virtio_balloon, oom_nb);
	unsigned long *freed = parm;

	*freed += leak_balloon(vb, VIRTIO_BALLOON_OOM_NR_PAGES) /
		  VIRTIO_BALLOON_PAGES_PER_PAGE;
	update_balloon_size(vb);

	return NOTIFY_OK;
}

static void virtio_balloon_unregister_shrinker(struct virtio_balloon *vb)
{
	unregister_shrinker(&vb->shrinker);
}

static int virtio_balloon_register_shrinker(struct virtio_balloon *vb)
{
	vb->shrinker.scan_objects = virtio_balloon_shrinker_scan;
	vb->shrinker.count_objects = virtio_balloon_shrinker_count;
	vb->shrinker.seeks = DEFAULT_SEEKS;

	return register_shrinker(&vb->shrinker);
}

static int virtballoon_probe(struct virtio_device *vdev)
{
	struct virtio_balloon *vb;
	int err;

	if (!vdev->config->get) {
		dev_err(&vdev->dev, "%s failure: config access disabled\n",
			__func__);
		return -EINVAL;
	}

	vdev->priv = vb = kzalloc(sizeof(*vb), GFP_KERNEL);
	if (!vb) {
		err = -ENOMEM;
		goto out;
	}

	INIT_WORK(&vb->update_balloon_stats_work, update_balloon_stats_func);
	INIT_WORK(&vb->update_balloon_size_work, update_balloon_size_func);
	spin_lock_init(&vb->stop_update_lock);
	mutex_init(&vb->balloon_lock);
	init_waitqueue_head(&vb->acked);
	vb->vdev = vdev;

	balloon_devinfo_init(&vb->vb_dev_info);

	err = init_vqs(vb);
	if (err)
		goto out_free_vb;

#ifdef CONFIG_BALLOON_COMPACTION
	balloon_mnt = kern_mount(&balloon_fs);
	if (IS_ERR(balloon_mnt)) {
		err = PTR_ERR(balloon_mnt);
		goto out_del_vqs;
	}

	vb->vb_dev_info.migratepage = virtballoon_migratepage;
	vb->vb_dev_info.inode = alloc_anon_inode(balloon_mnt->mnt_sb);
	if (IS_ERR(vb->vb_dev_info.inode)) {
		err = PTR_ERR(vb->vb_dev_info.inode);
		goto out_kern_unmount;
	}
	vb->vb_dev_info.inode->i_mapping->a_ops = &balloon_aops;
#endif
	if (virtio_has_feature(vdev, VIRTIO_BALLOON_F_FREE_PAGE_HINT)) {
		/*
		 * There is always one entry reserved for cmd id, so the ring
		 * size needs to be at least two to report free page hints.
		 */
		if (virtqueue_get_vring_size(vb->free_page_vq) < 2) {
			err = -ENOSPC;
			goto out_iput;
		}
		vb->balloon_wq = alloc_workqueue("balloon-wq",
					WQ_FREEZABLE | WQ_CPU_INTENSIVE, 0);
		if (!vb->balloon_wq) {
			err = -ENOMEM;
			goto out_iput;
		}
		INIT_WORK(&vb->report_free_page_work, report_free_page_func);
		vb->cmd_id_received_cache = VIRTIO_BALLOON_CMD_ID_STOP;
		vb->cmd_id_active = cpu_to_virtio32(vb->vdev,
						  VIRTIO_BALLOON_CMD_ID_STOP);
		vb->cmd_id_stop = cpu_to_virtio32(vb->vdev,
						  VIRTIO_BALLOON_CMD_ID_STOP);
		spin_lock_init(&vb->free_page_list_lock);
		INIT_LIST_HEAD(&vb->free_page_list);
		/*
		 * We're allowed to reuse any free pages, even if they are
		 * still to be processed by the host.
		 */
		err = virtio_balloon_register_shrinker(vb);
		if (err)
			goto out_del_balloon_wq;
	}

	if (virtio_has_feature(vb->vdev, VIRTIO_BALLOON_F_DEFLATE_ON_OOM)) {
		vb->oom_nb.notifier_call = virtio_balloon_oom_notify;
		vb->oom_nb.priority = VIRTIO_BALLOON_OOM_NOTIFY_PRIORITY;
		err = register_oom_notifier(&vb->oom_nb);
		if (err < 0)
			goto out_unregister_shrinker;
	}

	if (virtio_has_feature(vdev, VIRTIO_BALLOON_F_PAGE_POISON)) {
		/* Start with poison val of 0 representing general init */
		__u32 poison_val = 0;

		/*
		 * Let the hypervisor know that we are expecting a
		 * specific value to be written back in balloon pages.
		 *
		 * If the PAGE_POISON value was larger than a byte we would
		 * need to byte swap poison_val here to guarantee it is
		 * little-endian. However for now it is a single byte so we
		 * can pass it as-is.
		 */
		if (!want_init_on_free())
			memset(&poison_val, PAGE_POISON, sizeof(poison_val));

		virtio_cwrite_le(vb->vdev, struct virtio_balloon_config,
				 poison_val, &poison_val);
	}

	vb->pr_dev_info.report = virtballoon_free_page_report;
	if (virtio_has_feature(vb->vdev, VIRTIO_BALLOON_F_REPORTING)) {
		unsigned int capacity;

		capacity = virtqueue_get_vring_size(vb->reporting_vq);
		if (capacity < PAGE_REPORTING_CAPACITY) {
			err = -ENOSPC;
			goto out_unregister_oom;
		}

		/*
		 * The default page reporting order is @pageblock_order, which
		 * corresponds to 512MB in size on ARM64 when 64KB base page
		 * size is used. The page reporting won't be triggered if the
		 * freeing page can't come up with a free area like that huge.
		 * So we specify the page reporting order to 5, corresponding
		 * to 2MB. It helps to avoid THP splitting if 4KB base page
		 * size is used by host.
		 *
		 * Ideally, the page reporting order is selected based on the
		 * host's base page size. However, it needs more work to report
		 * that value. The hard-coded order would be fine currently.
		 */
#if defined(CONFIG_ARM64) && defined(CONFIG_ARM64_64K_PAGES)
		vb->pr_dev_info.order = 5;
#endif

		err = page_reporting_register(&vb->pr_dev_info);
		if (err)
			goto out_unregister_oom;
	}

	virtio_device_ready(vdev);
	if (towards_target(vb))
		virtballoon_changed(vdev);
	return 0;

out_unregister_oom:
	if (virtio_has_feature(vb->vdev, VIRTIO_BALLOON_F_DEFLATE_ON_OOM))
		unregister_oom_notifier(&vb->oom_nb);
out_unregister_shrinker:
	if (virtio_has_feature(vb->vdev, VIRTIO_BALLOON_F_FREE_PAGE_HINT))
		virtio_balloon_unregister_shrinker(vb);
out_del_balloon_wq:
	if (virtio_has_feature(vdev, VIRTIO_BALLOON_F_FREE_PAGE_HINT))
		destroy_workqueue(vb->balloon_wq);
out_iput:
#ifdef CONFIG_BALLOON_COMPACTION
	iput(vb->vb_dev_info.inode);
out_kern_unmount:
	kern_unmount(balloon_mnt);
out_del_vqs:
#endif
	vdev->config->del_vqs(vdev);
out_free_vb:
	kfree(vb);
out:
	return err;
}

static void remove_common(struct virtio_balloon *vb)
{
	/* There might be pages left in the balloon: free them. */
	while (vb->num_pages)
		leak_balloon(vb, vb->num_pages);
	update_balloon_size(vb);

	/* There might be free pages that are being reported: release them. */
	if (virtio_has_feature(vb->vdev, VIRTIO_BALLOON_F_FREE_PAGE_HINT))
		return_free_pages_to_mm(vb, ULONG_MAX);

	/* Now we reset the device so we can clean up the queues. */
	vb->vdev->config->reset(vb->vdev);

	vb->vdev->config->del_vqs(vb->vdev);
}

static void virtballoon_remove(struct virtio_device *vdev)
{
	struct virtio_balloon *vb = vdev->priv;

	if (virtio_has_feature(vb->vdev, VIRTIO_BALLOON_F_REPORTING))
		page_reporting_unregister(&vb->pr_dev_info);
	if (virtio_has_feature(vb->vdev, VIRTIO_BALLOON_F_DEFLATE_ON_OOM))
		unregister_oom_notifier(&vb->oom_nb);
	if (virtio_has_feature(vb->vdev, VIRTIO_BALLOON_F_FREE_PAGE_HINT))
		virtio_balloon_unregister_shrinker(vb);
	spin_lock_irq(&vb->stop_update_lock);
	vb->stop_update = true;
	spin_unlock_irq(&vb->stop_update_lock);
	cancel_work_sync(&vb->update_balloon_size_work);
	cancel_work_sync(&vb->update_balloon_stats_work);

	if (virtio_has_feature(vdev, VIRTIO_BALLOON_F_FREE_PAGE_HINT)) {
		cancel_work_sync(&vb->report_free_page_work);
		destroy_workqueue(vb->balloon_wq);
	}

	remove_common(vb);
#ifdef CONFIG_BALLOON_COMPACTION
	if (vb->vb_dev_info.inode)
		iput(vb->vb_dev_info.inode);

	kern_unmount(balloon_mnt);
#endif
	kfree(vb);
}

#ifdef CONFIG_PM_SLEEP
static int virtballoon_freeze(struct virtio_device *vdev)
{
	struct virtio_balloon *vb = vdev->priv;

	/*
	 * The workqueue is already frozen by the PM core before this
	 * function is called.
	 */
	remove_common(vb);
	return 0;
}

static int virtballoon_restore(struct virtio_device *vdev)
{
	struct virtio_balloon *vb = vdev->priv;
	int ret;
	ret = init_vqs(vdev->priv);
	if (ret)
		return ret;

	virtio_device_ready(vdev);

	if (towards_target(vb))
		virtballoon_changed(vdev);
	update_balloon_size(vb);
	return 0;
}
#endif

static int virtballoon_validate(struct virtio_device *vdev)
{
	/*
	 * Inform the hypervisor that our pages are poisoned or
	 * initialized. If we cannot do that then we should disable
	 * page reporting as it could potentially change the contents
	 * of our free pages.
	 */
	if (!want_init_on_free() && !page_poisoning_enabled_static())
		__virtio_clear_bit(vdev, VIRTIO_BALLOON_F_PAGE_POISON);
	else if (!virtio_has_feature(vdev, VIRTIO_BALLOON_F_PAGE_POISON))
		__virtio_clear_bit(vdev, VIRTIO_BALLOON_F_REPORTING);

	__virtio_clear_bit(vdev, VIRTIO_F_ACCESS_PLATFORM);
	return 0;
}

static unsigned int features[] = {
	VIRTIO_BALLOON_F_MUST_TELL_HOST,
	VIRTIO_BALLOON_F_STATS_VQ,
	VIRTIO_BALLOON_F_DEFLATE_ON_OOM,
	VIRTIO_BALLOON_F_FREE_PAGE_HINT,
	VIRTIO_BALLOON_F_PAGE_POISON,
	VIRTIO_BALLOON_F_REPORTING,
};

static struct virtio_driver virtio_balloon_driver = {
	.feature_table = features,
	.feature_table_size = ARRAY_SIZE(features),
	.driver.name =	KBUILD_MODNAME,
	.driver.owner =	THIS_MODULE,
	.id_table =	id_table,
	.validate =	virtballoon_validate,
	.probe =	virtballoon_probe,
	.remove =	virtballoon_remove,
	.config_changed = virtballoon_changed,
#ifdef CONFIG_PM_SLEEP
	.freeze	=	virtballoon_freeze,
	.restore =	virtballoon_restore,
#endif
};

module_virtio_driver(virtio_balloon_driver);
MODULE_DEVICE_TABLE(virtio, id_table);
MODULE_DESCRIPTION("Virtio balloon driver");
MODULE_LICENSE("GPL");
