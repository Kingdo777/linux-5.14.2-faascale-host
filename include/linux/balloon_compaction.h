/* SPDX-License-Identifier: GPL-2.0 */
/*
 * include/linux/balloon_compaction.h
 *
 * Common interface definitions for making balloon pages movable by compaction.
 *
 * Balloon page migration makes use of the general non-lru movable page
 * feature.
 *
 * 这里的 non-lru 可能是指 page.lru 仅仅用于链表的链接，而无lru算法牵扯其中
 *
 * page->private is used to reference the responsible balloon device.
 * page->mapping is used in context of non-lru page migration to reference
 * the address space operations for page isolation/migration/compaction.
 *
 * As the page isolation scanning step a compaction thread does is a lockless
 * procedure (from a page standpoint), it might bring some racy situations while
 * performing balloon page compaction. In order to sort out these racy scenarios
 * and safely perform balloon's page compaction and migration we must, always,
 * ensure following these simple rules:
 *
 *   i. when updating a balloon's page ->mapping element, strictly do it under
 *      the following lock order, independently of the far superior
 *      locking scheme (lru_lock, balloon_lock):
 *	    +-page_lock(page);
 *	      +--spin_lock_irq(&b_dev_info->pages_lock);
 *	            ... page->mapping updates here ...
 *
 *  ii. isolation or dequeueing procedure must remove the page from balloon
 *      device page list under b_dev_info->pages_lock.
 *
 * The functions provided by this interface are placed to help on coping with
 * the aforementioned balloon page corner case, as well as to ensure the simple
 * set of exposed rules are satisfied while we are dealing with balloon pages
 * compaction / migration.
 *
 * Copyright (C) 2012, Red Hat, Inc.  Rafael Aquini <aquini@redhat.com>
 */
#ifndef _LINUX_BALLOON_COMPACTION_H
#define _LINUX_BALLOON_COMPACTION_H
#include <linux/pagemap.h>
#include <linux/page-flags.h>
#include <linux/migrate.h>
#include <linux/gfp.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/list.h>

/*
 * Balloon device information descriptor.
 * This struct is used to allow the common balloon compaction interface
 * procedures to find the proper balloon device holding memory pages they'll
 * have to cope for page compaction / migration, as well as it serves the
 * balloon driver as a page book-keeper for its registered balloon devices.
 */
/**
 * 这是实现气球的核心结构，扩展的气球页面存放在次结构中，当需要从气球中释放页面时，同样从此结构中获取
 * 以上操作通过balloon_page_enqueue，balloon_page_dequeue实现，对应的实例是 virtio_balloon.vb_dev_info
 * 此外，从伙伴系统申请页面通过balloon_page_alloc函数进行封装，此三个函数都在本balloon_compaction.c文件中定义
 * 而归还页面调用的函数是put_page(), 这是mm.h中的函数
 * */
struct balloon_dev_info {
	unsigned long isolated_pages;	/* # of isolated pages for migration */
	spinlock_t pages_lock;		/* Protection to pages list */
	struct list_head pages;		/* Pages enqueued & handled to Host */
	int (*migratepage)(struct balloon_dev_info *, struct page *newpage,
			struct page *page, enum migrate_mode mode);
	struct inode *inode;
};

extern struct page *balloon_page_alloc(void);
extern void balloon_page_enqueue(struct balloon_dev_info *b_dev_info,
				 struct page *page);
extern struct page *balloon_page_dequeue(struct balloon_dev_info *b_dev_info);
extern size_t balloon_page_list_enqueue(struct balloon_dev_info *b_dev_info,
				      struct list_head *pages);
extern size_t balloon_page_list_dequeue(struct balloon_dev_info *b_dev_info,
				     struct list_head *pages, size_t n_req_pages);

static inline void balloon_devinfo_init(struct balloon_dev_info *balloon)
{
	balloon->isolated_pages = 0;
	spin_lock_init(&balloon->pages_lock);
	INIT_LIST_HEAD(&balloon->pages);
	balloon->migratepage = NULL;
	balloon->inode = NULL;
}

#ifdef CONFIG_BALLOON_COMPACTION
extern const struct address_space_operations balloon_aops;
extern bool balloon_page_isolate(struct page *page,
				isolate_mode_t mode);
extern void balloon_page_putback(struct page *page);
extern int balloon_page_migrate(struct address_space *mapping,
				struct page *newpage,
				struct page *page, enum migrate_mode mode);

/*
 * balloon_page_insert - insert a page into the balloon's page list and make
 *			 the page->private assignment accordingly.
 * @balloon : pointer to balloon device
 * @page    : page to be assigned as a 'balloon page'
 *
 * Caller must ensure the page is locked and the spin_lock protecting balloon
 * pages list is held before inserting a page into the balloon device.
 */
/**
 *
 * 在将页面插入气球设备之前，调用者必须确保页面被锁定，并且拿到了balloon_dev_info.pages_lock这个
 * 用于保护balloon_dev_info.pages 列表的自旋锁
 * */
static inline void balloon_page_insert(struct balloon_dev_info *balloon,
				       struct page *page)
{
	/// 将页面标记为“脱机”状态，即此page不应该被所有者 touche (read/write/dump/save)
	__SetPageOffline(page);
	__SetPageMovable(page, balloon->inode->i_mapping);
	set_page_private(page, (unsigned long)balloon);
	list_add(&page->lru, &balloon->pages);
}

/*
 * balloon_page_delete - delete a page from balloon's page list and clear
 *			 the page->private assignement accordingly.
 * @page    : page to be released from balloon's page list
 *
 * Caller must ensure the page is locked and the spin_lock protecting balloon
 * pages list is held before deleting a page from the balloon device.
 */
static inline void balloon_page_delete(struct page *page)
{
	__ClearPageOffline(page);
	__ClearPageMovable(page);
	set_page_private(page, 0);
	/*
	 * No touch page.lru field once @page has been isolated
	 * because VM is using the field.
	 */
	if (!PageIsolated(page))
		list_del(&page->lru);
}

/*
 * balloon_page_device - get the b_dev_info descriptor for the balloon device
 *			 that enqueues the given page.
 */
static inline struct balloon_dev_info *balloon_page_device(struct page *page)
{
	return (struct balloon_dev_info *)page_private(page);
}

/// 根据https://www.kernel.org/doc/html/v5.14/core-api/memory-allocation.html
/// 此标志的限制非常低，即不必是直接映射的内存区域，不必是不可移动的内存区域
static inline gfp_t balloon_mapping_gfp_mask(void)
{
	return GFP_HIGHUSER_MOVABLE;
}

#else /* !CONFIG_BALLOON_COMPACTION */

static inline void balloon_page_insert(struct balloon_dev_info *balloon,
				       struct page *page)
{
	__SetPageOffline(page);
	list_add(&page->lru, &balloon->pages);
}

static inline void balloon_page_delete(struct page *page)
{
	__ClearPageOffline(page);
	list_del(&page->lru);
}

static inline bool balloon_page_isolate(struct page *page)
{
	return false;
}

static inline void balloon_page_putback(struct page *page)
{
	return;
}

static inline int balloon_page_migrate(struct page *newpage,
				struct page *page, enum migrate_mode mode)
{
	return 0;
}

static inline gfp_t balloon_mapping_gfp_mask(void)
{
	return GFP_HIGHUSER;
}

#endif /* CONFIG_BALLOON_COMPACTION */

/*
 * balloon_page_push - insert a page into a page list.
 * @head : pointer to list
 * @page : page to be added
 *
 * Caller must ensure the page is private and protect the list.
 */
static inline void balloon_page_push(struct list_head *pages, struct page *page)
{
	list_add(&page->lru, pages);
}

/*
 * balloon_page_pop - remove a page from a page list.
 * @head : pointer to list
 * @page : page to be added
 *
 * Caller must ensure the page is private and protect the list.
 */
static inline struct page *balloon_page_pop(struct list_head *pages)
{
	struct page *page = list_first_entry_or_null(pages, struct page, lru);

	if (!page)
		return NULL;

	list_del(&page->lru);
	return page;
}
#endif /* _LINUX_BALLOON_COMPACTION_H */
