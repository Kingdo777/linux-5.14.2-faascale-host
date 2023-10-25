// SPDX-License-Identifier: GPL-2.0-only
/*
 *  mm/userfaultfd.c
 *
 *  Copyright (C) 2015  Red Hat, Inc.
 */

#include <linux/mm.h>
#include <linux/sched/signal.h>
#include <linux/pagemap.h>
#include <linux/rmap.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/userpopulatefd_k.h>
#include <linux/mmu_notifier.h>
#include <linux/hugetlb.h>
#include <linux/shmem_fs.h>
#include <asm/tlbflush.h>
#include <asm/tlb.h>
#include "internal.h"

/*
 * This function is called to print an error when a bad pte
 * is found. For example, we might have a PFN-mapped pte in
 * a region that doesn't allow it.
 *
 * The calling function must still handle the error.
 */
static void print_bad_pte(struct vm_area_struct *vma, unsigned long addr,
			  pte_t pte, struct page *page)
{
	pgd_t *pgd = pgd_offset(vma->vm_mm, addr);
	p4d_t *p4d = p4d_offset(pgd, addr);
	pud_t *pud = pud_offset(p4d, addr);
	pmd_t *pmd = pmd_offset(pud, addr);
	struct address_space *mapping;
	pgoff_t index;
	static unsigned long resume;
	static unsigned long nr_shown;
	static unsigned long nr_unshown;

	/*
	 * Allow a burst of 60 reports, then keep quiet for that minute;
	 * or allow a steady drip of one report per second.
	 */
	if (nr_shown == 60) {
		if (time_before(jiffies, resume)) {
			nr_unshown++;
			return;
		}
		if (nr_unshown) {
			pr_alert("BUG: Bad page map: %lu messages suppressed\n",
				 nr_unshown);
			nr_unshown = 0;
		}
		nr_shown = 0;
	}
	if (nr_shown++ == 0)
		resume = jiffies + 60 * HZ;

	mapping = vma->vm_file ? vma->vm_file->f_mapping : NULL;
	index = linear_page_index(vma, addr);

	pr_alert("BUG: Bad page map in process %s  pte:%08llx pmd:%08llx\n",
		 current->comm, (long long)pte_val(pte),
		 (long long)pmd_val(*pmd));
	if (page)
		dump_page(page, "bad pte");
	pr_alert("addr:%px vm_flags:%08lx anon_vma:%px mapping:%px index:%lx\n",
		 (void *)addr, vma->vm_flags, vma->anon_vma, mapping, index);
	pr_alert("file:%pD fault:%ps mmap:%ps readpage:%ps\n", vma->vm_file,
		 vma->vm_ops ? vma->vm_ops->fault : NULL,
		 vma->vm_file ? vma->vm_file->f_op->mmap : NULL,
		 mapping ? mapping->a_ops->readpage : NULL);
	dump_stack();
	add_taint(TAINT_BAD_PAGE, LOCKDEP_NOW_UNRELIABLE);
}

static inline void init_rss_vec(int *rss)
{
	memset(rss, 0, sizeof(int) * NR_MM_COUNTERS);
}

static inline void add_mm_rss_vec(struct mm_struct *mm, int *rss)
{
	int i;

	if (current->mm == mm)
		sync_mm_rss(mm);
	for (i = 0; i < NR_MM_COUNTERS; i++)
		if (rss[i])
			add_mm_counter(mm, i, rss[i]);
}

static unsigned long zap_pte_range(struct mmu_gather *tlb,
				   struct vm_area_struct *vma, pmd_t *pmd,
				   unsigned long addr, unsigned long end,
				   struct page **pages, ssize_t *pages_count)
{
	struct mm_struct *mm = tlb->mm;
	int force_flush = 0;
	int rss[NR_MM_COUNTERS];
	spinlock_t *ptl;
	pte_t *start_pte;
	pte_t *pte;

	tlb_change_page_size(tlb, PAGE_SIZE);
again:
	init_rss_vec(rss);
	start_pte = pte_offset_map_lock(mm, pmd, addr, &ptl);
	pte = start_pte;
	flush_tlb_batched_pending(mm);
	arch_enter_lazy_mmu_mode();
	do {
		pte_t ptent = *pte;
		if (pte_none(ptent))
			goto err;

		// 如果使用该优化，那么久需要参考原代码，确保addr==end
		//		if (need_resched())
		//			goto err;

		if (pte_present(ptent)) {
			struct page *page;

			page = vm_normal_page(vma, addr, ptent);

			ptent = ptep_get_and_clear_full(mm, addr, pte,
							tlb->fullmm);
			tlb_remove_tlb_entry(tlb, pte, addr);
			if (unlikely(!page))
				continue;

			pages[*pages_count] = page;
			(*pages_count)++;
			get_page(page);

			if (!PageAnon(page)) {
				if (pte_dirty(ptent)) {
					force_flush = 1;
					set_page_dirty(page);
				}
				if (pte_young(ptent) &&
				    likely(!(vma->vm_flags & VM_SEQ_READ)))
					mark_page_accessed(page);
			}
			rss[mm_counter(page)]--;
			page_remove_rmap(page, false);
			if (unlikely(page_mapcount(page) < 0))
				print_bad_pte(vma, addr, ptent, page);
			if (unlikely(__tlb_remove_page(tlb, page))) {
				force_flush = 1;
				addr += PAGE_SIZE;
				break;
			}
		}

	} while (pte++, addr += PAGE_SIZE, addr != end);

	add_mm_rss_vec(mm, rss);
	arch_leave_lazy_mmu_mode();

	/* Do the actual TLB flush before dropping ptl */
	if (force_flush)
		tlb_flush_mmu_tlbonly(tlb);
	pte_unmap_unlock(start_pte, ptl);

	/*
	 * If we forced a TLB flush (either due to running out of
	 * batch buffers or because we needed to flush dirty TLB
	 * entries before releasing the ptl), free the batched
	 * memory too. Restart if we didn't do everything.
	 */
	if (force_flush) {
		force_flush = 0;
		tlb_flush_mmu(tlb);
	}

	if (addr != end) {
		goto again;
	}

	return addr;
err:
	pte_unmap_unlock(start_pte, ptl);
	return 0;
}

static inline unsigned long
zap_pmd_range(struct mmu_gather *tlb, struct vm_area_struct *vma, pud_t *pud,
	      unsigned long addr, unsigned long end, struct page **pages,
	      ssize_t *pages_count)
{
	pmd_t *pmd;
	unsigned long next;
	pmd = pmd_offset(pud, addr);
	do {
		next = pmd_addr_end(addr, end);
		next = zap_pte_range(tlb, vma, pmd, addr, next, pages,
				     pages_count);
		if (!next)
			return 0;

	} while (pmd++, addr = next, addr != end);

	return addr;
}

static inline unsigned long
zap_pud_range(struct mmu_gather *tlb, struct vm_area_struct *vma, p4d_t *p4d,
	      unsigned long addr, unsigned long end, struct page **pages,
	      ssize_t *pages_count)
{
	pud_t *pud;
	unsigned long next;

	pud = pud_offset(p4d, addr);
	do {
		next = pud_addr_end(addr, end);
		if (pud_none_or_clear_bad(pud))
			continue;
		next = zap_pmd_range(tlb, vma, pud, addr, next, pages,
				     pages_count);
		if (!next)
			return 0;
	} while (pud++, addr = next, addr != end);

	return addr;
}

static inline unsigned long
zap_p4d_range(struct mmu_gather *tlb, struct vm_area_struct *vma, pgd_t *pgd,
	      unsigned long addr, unsigned long end, struct page **pages,
	      ssize_t *pages_count)
{
	p4d_t *p4d;
	unsigned long next;

	p4d = p4d_offset(pgd, addr);
	do {
		next = p4d_addr_end(addr, end);
		if (p4d_none_or_clear_bad(p4d))
			continue;
		next = zap_pud_range(tlb, vma, p4d, addr, next, pages,
				     pages_count);
		if (!next)
			return 0;
	} while (p4d++, addr = next, addr != end);

	return addr;
}

static ssize_t unmap_page_range_userpopulatefd(struct mmu_gather *tlb,
					       struct vm_area_struct *vma,
					       unsigned long addr,
					       unsigned long end,
					       struct page **pages)
{
	pgd_t *pgd;
	unsigned long next;
	ssize_t pages_count = 0;

	BUG_ON(addr >= end);
	tlb_start_vma(tlb, vma);
	pgd = pgd_offset(vma->vm_mm, addr);
	do {
		next = pgd_addr_end(addr, end);
		if (pgd_none_or_clear_bad(pgd))
			continue;
		next = zap_p4d_range(tlb, vma, pgd, addr, next, pages,
				     &pages_count);
		if (!next)
			break;
	} while (pgd++, addr = next, addr != end);
	tlb_end_vma(tlb, vma);
	return pages_count;
}

static ssize_t zap_page_range_userpopulatefd(struct vm_area_struct *vma,
					     unsigned long start,
					     unsigned long size,
					     struct page **pages)
{
	struct mmu_notifier_range range;
	struct mmu_gather tlb;
	ssize_t pages_count;

	lru_add_drain();
	mmu_notifier_range_init(&range, MMU_NOTIFY_CLEAR, 0, vma, vma->vm_mm,
				start, start + size);
	tlb_gather_mmu(&tlb, vma->vm_mm);
	update_hiwater_rss(vma->vm_mm);
	mmu_notifier_invalidate_range_start(&range);
	pages_count = unmap_page_range_userpopulatefd(&tlb, vma, start,
						      range.end, pages);
	mmu_notifier_invalidate_range_end(&range);
	tlb_finish_mmu(&tlb);
	return pages_count;
}

static __always_inline struct vm_area_struct *
find_dst_vma(struct mm_struct *dst_mm, unsigned long dst_start,
	     unsigned long len)
{
	/*
	 * Make sure that the dst range is both valid and fully within a
	 * single existing vma.
	 */
	struct vm_area_struct *dst_vma;

	dst_vma = find_vma(dst_mm, dst_start);
	if (!dst_vma)
		return NULL;

	if (dst_start < dst_vma->vm_start || dst_start + len > dst_vma->vm_end)
		return NULL;

	return dst_vma;
}
static __always_inline struct vm_area_struct *
find_src_vma(struct mm_struct *src_mm, unsigned long src_start,
	     unsigned long len)
{
	return find_dst_vma(src_mm, src_start, len);
}

static pmd_t *mm_alloc_pmd(struct mm_struct *mm, unsigned long address)
{
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;

	pgd = pgd_offset(mm, address);
	p4d = p4d_alloc(mm, pgd, address);
	if (!p4d)
		return NULL;
	pud = pud_alloc(mm, p4d, address);
	if (!pud)
		return NULL;
	/*
	 * Note that we didn't run this because the pmd was
	 * missing, the *pmd may be already established and in
	 * turn it may also be a trans_huge_pmd.
	 */
	return pmd_alloc(mm, pud, address);
}

#ifdef CONFIG_HUGETLB_PAGE
/*
 * __mcopy_atomic processing for HUGETLB vmas.  Note that this routine is
 * called with mmap_lock held, it will release mmap_lock before returning.
 */
static __always_inline ssize_t
__mpopulate_atomic_hugetlb(struct mm_struct *mm, struct vm_area_struct *dst_vma,
			   unsigned long dst_start, unsigned long src_start,
			   unsigned long len, enum mpopulate_atomic_mode mode)
{
	ssize_t err;
	pte_t *dst_pte;
	unsigned long src_addr, dst_addr;
	long copied;
	struct page *page;
	unsigned long vma_hpagesize;
	pgoff_t idx;
	u32 hash;
	struct address_space *mapping;

	BUG_ON(dst_vma->vm_flags & VM_SHARED);
	BUG_ON(anon_vma_prepare(dst_vma));

	src_addr = src_start;
	dst_addr = dst_start;
	copied = 0;
	page = NULL;
	vma_hpagesize = vma_kernel_pagesize(dst_vma);

	/*
	 * Validate alignment based on huge page size
	 */
	err = -EINVAL;
	if (dst_start & (vma_hpagesize - 1) || len & (vma_hpagesize - 1))
		goto out_unlock;

retry:
	/*
	 * On routine entry dst_vma is set.  If we had to drop mmap_lock and
	 * retry, dst_vma will be set to NULL and we must lookup again.
	 */
	if (!dst_vma) {
		err = -ENOENT;
		dst_vma = find_dst_vma(mm, dst_start, len);
		if (!dst_vma || !is_vm_hugetlb_page(dst_vma))
			goto out_unlock;

		err = -EINVAL;
		if (vma_hpagesize != vma_kernel_pagesize(dst_vma))
			goto out_unlock;
	}

	while (src_addr < src_start + len) {
		BUG_ON(dst_addr >= dst_start + len);

		/*
		 * Serialize via i_mmap_rwsem and hugetlb_fault_mutex.
		 * i_mmap_rwsem ensures the dst_pte remains valid even
		 * in the case of shared pmds.  fault mutex prevents
		 * races with other faulting threads.
		 */
		mapping = dst_vma->vm_file->f_mapping;
		i_mmap_lock_read(mapping);
		idx = linear_page_index(dst_vma, dst_addr);
		hash = hugetlb_fault_mutex_hash(mapping, idx);
		mutex_lock(&hugetlb_fault_mutex_table[hash]);

		err = -ENOMEM;
		dst_pte = huge_pte_alloc(mm, dst_vma, dst_addr, vma_hpagesize);
		if (!dst_pte) {
			mutex_unlock(&hugetlb_fault_mutex_table[hash]);
			i_mmap_unlock_read(mapping);
			goto out_unlock;
		}

		if (!huge_pte_none(huge_ptep_get(dst_pte))) {
			err = -EEXIST;
			mutex_unlock(&hugetlb_fault_mutex_table[hash]);
			i_mmap_unlock_read(mapping);
			goto out_unlock;
		}

		err = hugetlb_mcopy_atomic_pte(mm, dst_pte, dst_vma, dst_addr,
					       src_addr, mode, &page);

		mutex_unlock(&hugetlb_fault_mutex_table[hash]);
		i_mmap_unlock_read(mapping);

		cond_resched();

		if (unlikely(err == -ENOENT)) {
			mmap_read_unlock(mm);
			BUG_ON(!page);

			err = copy_huge_page_from_user(
				page, (const void __user *)src_addr,
				vma_hpagesize / PAGE_SIZE, true);
			if (unlikely(err)) {
				err = -EFAULT;
				goto out;
			}
			mmap_read_lock(mm);

			dst_vma = NULL;
			goto retry;
		} else
			BUG_ON(page);

		if (!err) {
			dst_addr += vma_hpagesize;
			src_addr += vma_hpagesize;
			copied += vma_hpagesize;

			if (fatal_signal_pending(current))
				err = -EINTR;
		}
		if (err)
			break;
	}

out_unlock:
	mmap_read_unlock(mm);
out:
	if (page)
		put_page(page);
	BUG_ON(copied < 0);
	BUG_ON(err > 0);
	BUG_ON(!copied && !err);
	return copied ? copied : err;
}
#else /* !CONFIG_HUGETLB_PAGE */
/* fail at build time if gcc attempts to use this */
extern ssize_t
__mcopy_atomic_hugetlb(struct mm_struct *dst_mm, struct vm_area_struct *dst_vma,
		       unsigned long dst_start, unsigned long src_start,
		       unsigned long len, enum mcopy_atomic_mode mode);
#endif /* CONFIG_HUGETLB_PAGE */

/*
* Install PTEs, to map dst_addr (within dst_vma) to page.
*
* This function handles both MCOPY_ATOMIC_NORMAL and _CONTINUE for both shmem
* and anon, and for both shared and private VMAs.
*/
static int mpopulate_atomic_install_pte(struct mm_struct *dst_mm,
					pmd_t *dst_pmd,
					struct vm_area_struct *dst_vma,
					unsigned long dst_addr,
					struct page *page, bool newly_allocated)
{
	int ret;
	pte_t _dst_pte, *dst_pte;
	bool writable = dst_vma->vm_flags & VM_WRITE;
	spinlock_t *ptl;
	//	bool page_in_cache = page->mapping;

	BUG_ON(dst_vma->vm_flags & VM_SHARED);
	//	BUG_ON(page_in_cache);

	_dst_pte = mk_pte(page, dst_vma->vm_page_prot);
	if (writable) {
		_dst_pte = pte_mkdirty(_dst_pte);
		_dst_pte = pte_mkwrite(_dst_pte);
	}

	dst_pte = pte_offset_map_lock(dst_mm, dst_pmd, dst_addr, &ptl);

	ret = -EEXIST;
	if (!pte_none(*dst_pte))
		goto out_unlock;
	//	if (page_in_cache)
	//		page_add_anon_rmap(page, dst_vma, dst_addr, 0);
	//	else
	page_add_new_anon_rmap(page, dst_vma, dst_addr, false);

	/*
	* Must happen after rmap, as mm_counter() checks mapping (via
	* PageAnon()), which is set by __page_set_anon_rmap().
	*/
	inc_mm_counter(dst_mm, mm_counter(page));

	if (newly_allocated)
		lru_cache_add_inactive_or_unevictable(page, dst_vma);

	set_pte_at(dst_mm, dst_addr, dst_pte, _dst_pte);

	/* No need to invalidate - it was non-present before */
	update_mmu_cache(dst_vma, dst_addr, dst_pte);
	ret = 0;
out_unlock:
	pte_unmap_unlock(dst_pte, ptl);
	return ret;
}

static int mpopulate_copy_atomic_pte(struct mm_struct *dst_mm, pmd_t *dst_pmd,
				     struct vm_area_struct *dst_vma,
				     unsigned long dst_addr,
				     unsigned long src_addr,
				     struct page **pagep)
{
	void *page_kaddr;
	int ret;
	struct page *page;

	if (!*pagep) {
		ret = -ENOMEM;
		page = alloc_page_vma(GFP_HIGHUSER_MOVABLE, dst_vma, dst_addr);
		if (!page)
			goto out;

		page_kaddr = kmap_atomic(page);
		ret = copy_from_user(page_kaddr, (const void __user *)src_addr,
				     PAGE_SIZE);
		kunmap_atomic(page_kaddr);

		/* fallback to copy_from_user outside mmap_lock */
		if (unlikely(ret)) {
			ret = -ENOENT;
			*pagep = page;
			/* don't free the page */
			goto out;
		}
	} else {
		page = *pagep;
		*pagep = NULL;
	}

	/*
	 * The memory barrier inside __SetPageUptodate makes sure that
	 * preceding stores to the page contents become visible before
	 * the set_pte_at() write.
	 */
	__SetPageUptodate(page);

	ret = -ENOMEM;
	if (mem_cgroup_charge(page, dst_mm, GFP_KERNEL))
		goto out_release;

	ret = mpopulate_atomic_install_pte(dst_mm, dst_pmd, dst_vma, dst_addr,
					   page, true);
	if (ret)
		goto out_release;
out:
	return ret;
out_release:
	put_page(page);
	goto out;
}

static ssize_t mpopulate_graft_atomic_pte(struct mm_struct *dst_mm,
					  pmd_t *dst_pmd,
					  struct vm_area_struct *dst_vma,
					  unsigned long dst_addr,
					  unsigned long src_addr,
					  struct page **pagep)
{
	int ret;
	struct page *page;
	page = *pagep;
	*pagep = NULL;

	__SetPageUptodate(page);

	//	ret = -ENOMEM;
	//	if (mem_cgroup_charge(page, dst_mm, GFP_KERNEL))
	//		goto out_release;

	ret = mpopulate_atomic_install_pte(dst_mm, dst_pmd, dst_vma, dst_addr,
					   page, false);
	if (ret)
		goto out_release;
out:
	return ret;
out_release:
	put_page(page);
	goto out;
}

static __always_inline ssize_t mfill_atomic_pte(struct mm_struct *dst_mm,
						pmd_t *dst_pmd,
						struct vm_area_struct *dst_vma,
						unsigned long dst_addr,
						unsigned long src_addr,
						struct page **page,
						enum mpopulate_atomic_mode mode)
{
	ssize_t err;

	switch (mode) {
	case MPOPULATE_ATOMIC_COPY:
		err = mpopulate_copy_atomic_pte(dst_mm, dst_pmd, dst_vma,
						dst_addr, src_addr, page);
		break;
	case MPOPULATE_ATOMIC_GRATE:
		err = mpopulate_graft_atomic_pte(dst_mm, dst_pmd, dst_vma,
						 dst_addr, src_addr, page);
		break;
	default:
		unreachable();
	}

	return err;
}

//__always_inline
static ssize_t __mpopulate_atomic(struct mm_struct *mm, unsigned long dst_start,
				  unsigned long src_start, unsigned long len,
				  enum mpopulate_atomic_mode mode)
{
	struct vm_area_struct *dst_vma, *src_vma;
	ssize_t err;
	pmd_t *dst_pmd;
	unsigned long src_addr, dst_addr;
	long copied;
	struct page *page;
	struct page **pages;
	unsigned long pages_addr, pages_count, zap_pages_count;

	/*
	 * Sanitize the command parameters:
	 */
	BUG_ON(dst_start & ~PAGE_MASK);
	BUG_ON(len & ~PAGE_MASK);

	/* Does the address range wrap, or is the span zero-sized? */
	BUG_ON(src_start + len <= src_start);
	BUG_ON(dst_start + len <= dst_start);

	src_addr = src_start;
	dst_addr = dst_start;
	copied = 0;
	page = NULL;
	pages = NULL;
retry:
	mmap_read_lock(mm);

	/*
	 * Make sure the vma is not shared and is anonymous, that the dst range is
	 * both valid and fully within a single existing vma.
	 */
	err = -ENOENT;
	dst_vma = find_dst_vma(mm, dst_start, len);
	if (!dst_vma)
		goto out_unlock;
	src_vma = find_src_vma(mm, src_start, len);
	if (!src_vma)
		goto out_unlock;

	err = -EINVAL;
	if (!vma_is_anonymous(dst_vma) || dst_vma->vm_flags & VM_SHARED)
		goto out_unlock;
	if (!vma_is_anonymous(src_vma) || src_vma->vm_flags & VM_SHARED)
		goto out_unlock;

	/*
	 * If this is a HUGETLB vma, pass off to appropriate routine
	 */
	if (is_vm_hugetlb_page(dst_vma))
		return __mpopulate_atomic_hugetlb(mm, dst_vma, dst_start,
						  src_start, len, mode);

	/*
	 * Ensure the dst_vma has a anon_vma or this page
	 * would get a NULL anon_vma when moved in the
	 * dst_vma.
	 */
	err = -ENOMEM;
	if (!(dst_vma->vm_flags & VM_SHARED) &&
	    unlikely(anon_vma_prepare(dst_vma)))
		goto out_unlock;

	if (mode == MPOPULATE_ATOMIC_GRATE) {
		pages_count = len / PAGE_SIZE;
		pages = kmalloc(sizeof(struct page *) * pages_count,
				GFP_KERNEL);
		if (!pages) {
			err = -ENOMEM;
			goto out_unlock;
		}
		zap_pages_count = zap_page_range_userpopulatefd(
			src_vma, src_start, len, pages);
		if (pages_count != zap_pages_count) {
			while (zap_pages_count > 0) {
				put_page(pages[zap_pages_count - 1]);
				zap_pages_count--;
			}
			err = -EINVAL;
			goto out_unlock;
		}
		pages_addr = (unsigned long)pages;
	}

	while (src_addr < src_start + len) {
		pmd_t dst_pmdval;

		BUG_ON(dst_addr >= dst_start + len);

		dst_pmd = mm_alloc_pmd(mm, dst_addr);
		if (unlikely(!dst_pmd)) {
			err = -ENOMEM;
			break;
		}

		dst_pmdval = pmd_read_atomic(dst_pmd);
		/*
		 * If the dst_pmd is mapped as THP don't
		 * override it and just be strict.
		 */
		if (unlikely(pmd_trans_huge(dst_pmdval))) {
			err = -EEXIST;
			break;
		}
		if (unlikely(pmd_none(dst_pmdval)) &&
		    unlikely(__pte_alloc(mm, dst_pmd))) {
			err = -ENOMEM;
			break;
		}
		/* If an huge pmd materialized from under us fail */
		if (unlikely(pmd_trans_huge(*dst_pmd))) {
			err = -EFAULT;
			break;
		}

		BUG_ON(pmd_none(*dst_pmd));
		BUG_ON(pmd_trans_huge(*dst_pmd));

		if (mode == MPOPULATE_ATOMIC_GRATE) {
			page = *(struct page **)pages_addr;
			page->mapping = NULL;
			pages_addr += sizeof(struct page **);
		}

		err = mfill_atomic_pte(mm, dst_pmd, dst_vma, dst_addr, src_addr,
				       &page, mode);
		cond_resched();

		if (mode == MPOPULATE_ATOMIC_COPY && unlikely(err == -ENOENT)) {
			void *page_kaddr;

			mmap_read_unlock(mm);
			BUG_ON(!page);

			page_kaddr = kmap(page);
			err = copy_from_user(page_kaddr,
					     (const void __user *)src_addr,
					     PAGE_SIZE);
			kunmap(page);
			if (unlikely(err)) {
				err = -EFAULT;
				goto out;
			}
			goto retry;
		} else
			BUG_ON(page);

		if (!err) {
			dst_addr += PAGE_SIZE;
			src_addr += PAGE_SIZE;
			copied += PAGE_SIZE;
			if (fatal_signal_pending(current))
				err = -EINTR;
		}
		if (err)
			break;
	}

out_unlock:
	mmap_read_unlock(mm);
out:
	if (page)
		put_page(page);
	if (mode == MPOPULATE_ATOMIC_GRATE && pages)
		kfree(pages);
	BUG_ON(copied < 0);
	BUG_ON(err > 0);
	BUG_ON(!copied && !err);
	return copied ? copied : err;
}

extern ssize_t mpopulate_copy_atomic(struct mm_struct *mm,
				     unsigned long dst_start,
				     unsigned long src_start, unsigned long len)
{
	return __mpopulate_atomic(mm, dst_start, src_start, len,
				  MPOPULATE_ATOMIC_COPY);
}

extern ssize_t mpopulat_graft_atomic(struct mm_struct *mm,
				     unsigned long dst_start,
				     unsigned long src_start, unsigned long len)
{
	return __mpopulate_atomic(mm, dst_start, src_start, len,
				  MPOPULATE_ATOMIC_GRATE);
}