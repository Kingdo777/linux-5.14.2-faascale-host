// SPDX-License-Identifier: GPL-2.0-only
/*
 * mm/balloon_compaction.c
 *
 * Common interface for making balloon pages movable by compaction.
 *
 * Copyright (C) 2012, Red Hat, Inc.  Rafael Aquini <aquini@redhat.com>
 */
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/export.h>
#include <linux/balloon_compaction.h>

static void balloon_page_enqueue_one(struct balloon_dev_info *b_dev_info,
				     struct page *page)
{
	/*
	 * Block others from accessing the 'page' when we get around to
	 * establishing additional references. We should be the only one
	 * holding a reference to the 'page' at this point. If we are not, then
	 * memory corruption is possible and we should stop execution.
	 */
	/// 我们在建立对page的引用时执行trylock_page，不过在此时该，page是没有任何人进行引用的，
	/// 因此在理论上trylock_page一定会成功，否则内存可能是损坏的，应该停止执行
	BUG_ON(!trylock_page(page));
	/// 将page添加到balloon-list中，同时将page设置为private
	balloon_page_insert(b_dev_info, page);
	unlock_page(page);
	/// 此函数记录执行 BALLOON_INFLATE 操作的次数，从而在vmstat中查看
	__count_vm_event(BALLOON_INFLATE);
}

/**
 * balloon_page_list_enqueue() - inserts a list of pages into the balloon page
 *				 list.
 * @b_dev_info: balloon device descriptor where we will insert a new page to
 * @pages: pages to enqueue - allocated using balloon_page_alloc.
 *
 * Driver must call this function to properly enqueue balloon pages before
 * definitively removing them from the guest system.
 *
 * Return: number of pages that were enqueued.
 */
 /**
  * 调用balloon_page_enqueue_one将一个pages列表中所有的page，都添加到balloon-pages中
  * 注意，要在操作之前给b_dev_info->pages_lock上锁
  * */
size_t balloon_page_list_enqueue(struct balloon_dev_info *b_dev_info,
				 struct list_head *pages)
{
	struct page *page, *tmp;
	unsigned long flags;
	size_t n_pages = 0;

	spin_lock_irqsave(&b_dev_info->pages_lock, flags);
	list_for_each_entry_safe(page, tmp, pages, lru) {
		list_del(&page->lru);
		balloon_page_enqueue_one(b_dev_info, page);
		n_pages++;
	}
	spin_unlock_irqrestore(&b_dev_info->pages_lock, flags);
	return n_pages;
}
EXPORT_SYMBOL_GPL(balloon_page_list_enqueue);

/**
 * balloon_page_list_dequeue() - removes pages from balloon's page list and
 *				 returns a list of the pages.
 * @b_dev_info: balloon device descriptor where we will grab a page from.
 * @pages: pointer to the list of pages that would be returned to the caller.
 * @n_req_pages: number of requested pages.
 *
 * Driver must call this function to properly de-allocate a previous enlisted
 * balloon pages before definitively releasing it back to the guest system.
 * This function tries to remove @n_req_pages from the ballooned pages and
 * return them to the caller in the @pages list.
 *
 * Note that this function may fail to dequeue some pages even if the balloon
 * isn't empty - since the page list can be temporarily empty due to compaction
 * of isolated pages.
 *
 * Return: number of pages that were added to the @pages list.
 */
 /**
  * 从balloon-pages列表中，拿走n_req_pages个page还给系统，上面过程的反操作
  * */
size_t balloon_page_list_dequeue(struct balloon_dev_info *b_dev_info,
				 struct list_head *pages, size_t n_req_pages)
{
	struct page *page, *tmp;
	unsigned long flags;
	size_t n_pages = 0;

	spin_lock_irqsave(&b_dev_info->pages_lock, flags);
	list_for_each_entry_safe(page, tmp, &b_dev_info->pages, lru) {
		if (n_pages == n_req_pages)
			break;

		/*
		 * Block others from accessing the 'page' while we get around to
		 * establishing additional references and preparing the 'page'
		 * to be released by the balloon driver.
		 */
		if (!trylock_page(page))
			continue;

		if (IS_ENABLED(CONFIG_BALLOON_COMPACTION) &&
		    PageIsolated(page)) {
			/* raced with isolation */
			unlock_page(page);
			continue;
		}
		balloon_page_delete(page);
		/// 统计计数，BALLOON_DEFLATE操作加1
		__count_vm_event(BALLOON_DEFLATE);
		list_add(&page->lru, pages);
		unlock_page(page);
		n_pages++;
	}
	spin_unlock_irqrestore(&b_dev_info->pages_lock, flags);
	/// 返回从balloon-pages中拿走的页面数
	return n_pages;
}
EXPORT_SYMBOL_GPL(balloon_page_list_dequeue);

/*
 * balloon_page_alloc - allocates a new page for insertion into the balloon
 *			page list.
 *
 * Driver must call this function to properly allocate a new balloon page.
 * Driver must call balloon_page_enqueue before definitively removing the page
 * from the guest system.
 *
 * Return: struct page for the allocated page or NULL on allocation failure.
 */
struct page *balloon_page_alloc(void)
{
	/// __GFP_NORETRY 是在发生破坏性回收之前失败，不会导致OMM killer
	/// GFP_NOWARN,具有合理的回退路径
	struct page *page = alloc_page(balloon_mapping_gfp_mask() |
				       __GFP_NOMEMALLOC | __GFP_NORETRY |
				       __GFP_NOWARN);
	return page;
}
EXPORT_SYMBOL_GPL(balloon_page_alloc);

/*
 * balloon_page_enqueue - inserts a new page into the balloon page list.
 *
 * @b_dev_info: balloon device descriptor where we will insert a new page
 * @page: new page to enqueue - allocated using balloon_page_alloc.
 *
 * Drivers must call this function to properly enqueue a new allocated balloon
 * page before definitively removing the page from the guest system.
 *
 * Drivers must not call balloon_page_enqueue on pages that have been pushed to
 * a list with balloon_page_push before removing them with balloon_page_pop. To
 * enqueue a list of pages, use balloon_page_list_enqueue instead.
 */
/**
 * 这是 virtio-balloon调用的接口
 * 如果page被balloon_page_push函数查到了list上，那么就不能再执行balloon_page_enqueue了，
 * 而是应该先执行balloon_page_pop
 * */
void balloon_page_enqueue(struct balloon_dev_info *b_dev_info,
			  struct page *page)
{
	unsigned long flags;

	spin_lock_irqsave(&b_dev_info->pages_lock, flags);
	balloon_page_enqueue_one(b_dev_info, page);
	spin_unlock_irqrestore(&b_dev_info->pages_lock, flags);
}
EXPORT_SYMBOL_GPL(balloon_page_enqueue);

/*
 * balloon_page_dequeue - removes a page from balloon's page list and returns
 *			  its address to allow the driver to release the page.
 * @b_dev_info: balloon device descriptor where we will grab a page from.
 *
 * Driver must call this function to properly dequeue a previously enqueued page
 * before definitively releasing it back to the guest system.
 *
 * Caller must perform its own accounting to ensure that this
 * function is called only if some pages are actually enqueued.
 *
 * Note that this function may fail to dequeue some pages even if there are
 * some enqueued pages - since the page list can be temporarily empty due to
 * the compaction of isolated pages.
 *
 * TODO: remove the caller accounting requirements, and allow caller to wait
 * until all pages can be dequeued.
 *
 * Return: struct page for the dequeued page, or NULL if no page was dequeued.
 */

/**
 * 驱动程序必须调用此函数以正确地将先前加入到balloon-pages的页面出列，然后才能将其最终释放回客户系统。
 * 调用者必须执行自己确保仅当某些页面实际被加入到balloon-pages时才调用此函数，即如果没有执行balloon_page_enqueue，那么不应该执行此函数。
 * 请注意，即使有一些已入队的页面，此功能也可能无法使某些页面出队 - 因为由于the compaction of isolated pages，页面列表可能暂时为空。
 * TODO：移除调用者计费要求，并允许调用者等待，直到所有页面都可以出列。
 * */

struct page *balloon_page_dequeue(struct balloon_dev_info *b_dev_info)
{
	unsigned long flags;
	LIST_HEAD(pages);
	int n_pages;

	n_pages = balloon_page_list_dequeue(b_dev_info, &pages, 1);

	if (n_pages != 1) {
		/*
		 * If we are unable to dequeue a balloon page because the page
		 * list is empty and there are no isolated pages, then something
		 * went out of track and some balloon pages are lost.
		 * BUG() here, otherwise the balloon driver may get stuck in
		 * an infinite loop while attempting to release all its pages.
		 */
		spin_lock_irqsave(&b_dev_info->pages_lock, flags);
		if (unlikely(list_empty(&b_dev_info->pages) &&
			     !b_dev_info->isolated_pages))
			BUG();
		spin_unlock_irqrestore(&b_dev_info->pages_lock, flags);
		return NULL;
	}
	return list_first_entry(&pages, struct page, lru);
}
EXPORT_SYMBOL_GPL(balloon_page_dequeue);

#ifdef CONFIG_BALLOON_COMPACTION

bool balloon_page_isolate(struct page *page, isolate_mode_t mode)

{
	struct balloon_dev_info *b_dev_info = balloon_page_device(page);
	unsigned long flags;

	spin_lock_irqsave(&b_dev_info->pages_lock, flags);
	list_del(&page->lru);
	b_dev_info->isolated_pages++;
	spin_unlock_irqrestore(&b_dev_info->pages_lock, flags);

	return true;
}

void balloon_page_putback(struct page *page)
{
	struct balloon_dev_info *b_dev_info = balloon_page_device(page);
	unsigned long flags;

	spin_lock_irqsave(&b_dev_info->pages_lock, flags);
	list_add(&page->lru, &b_dev_info->pages);
	b_dev_info->isolated_pages--;
	spin_unlock_irqrestore(&b_dev_info->pages_lock, flags);
}


/* move_to_new_page() counterpart for a ballooned page */
int balloon_page_migrate(struct address_space *mapping,
		struct page *newpage, struct page *page,
		enum migrate_mode mode)
{
	struct balloon_dev_info *balloon = balloon_page_device(page);

	/*
	 * We can not easily support the no copy case here so ignore it as it
	 * is unlikely to be used with balloon pages. See include/linux/hmm.h
	 * for a user of the MIGRATE_SYNC_NO_COPY mode.
	 */
	if (mode == MIGRATE_SYNC_NO_COPY)
		return -EINVAL;

	VM_BUG_ON_PAGE(!PageLocked(page), page);
	VM_BUG_ON_PAGE(!PageLocked(newpage), newpage);

	return balloon->migratepage(balloon, newpage, page, mode);
}

const struct address_space_operations balloon_aops = {
	.migratepage = balloon_page_migrate,
	.isolate_page = balloon_page_isolate,
	.putback_page = balloon_page_putback,
};
EXPORT_SYMBOL_GPL(balloon_aops);

#endif /* CONFIG_BALLOON_COMPACTION */
