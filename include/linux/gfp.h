/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_GFP_H
#define __LINUX_GFP_H

#include <linux/mmdebug.h>
#include <linux/mmzone.h>
#include <linux/stddef.h>
#include <linux/linkage.h>
#include <linux/topology.h>

/* The typedef is in types.h but we want the documentation here */
#if 0
/**
 * typedef gfp_t - Memory allocation flags.
 *
 * GFP flags are commonly used throughout Linux to indicate how memory
 * should be allocated.  The GFP acronym stands for get_free_pages(),
 * the underlying memory allocation function.  Not every GFP flag is
 * supported by every function which may allocate memory.  Most users
 * will want to use a plain ``GFP_KERNEL``.
 */
typedef unsigned int __bitwise gfp_t;
#endif

struct vm_area_struct;

/*
 * In case of changes, please don't forget to update
 * include/trace/events/mmflags.h and tools/perf/builtin-kmem.c
 */

/* Plain integer GFP bitmasks. Do not use this directly. */
#define ___GFP_DMA		0x01u
#define ___GFP_HIGHMEM		0x02u
#define ___GFP_DMA32		0x04u
#define ___GFP_MOVABLE		0x08u
#define ___GFP_RECLAIMABLE	0x10u
#define ___GFP_HIGH		0x20u
#define ___GFP_IO		0x40u
#define ___GFP_FS		0x80u
#define ___GFP_ZERO		0x100u
#define ___GFP_ATOMIC		0x200u
#define ___GFP_DIRECT_RECLAIM	0x400u
#define ___GFP_KSWAPD_RECLAIM	0x800u
#define ___GFP_WRITE		0x1000u
#define ___GFP_NOWARN		0x2000u
#define ___GFP_RETRY_MAYFAIL	0x4000u
#define ___GFP_NOFAIL		0x8000u
#define ___GFP_NORETRY		0x10000u
#define ___GFP_MEMALLOC		0x20000u
#define ___GFP_COMP		0x40000u
#define ___GFP_NOMEMALLOC	0x80000u
#define ___GFP_HARDWALL		0x100000u
#define ___GFP_THISNODE		0x200000u
#define ___GFP_ACCOUNT		0x400000u
#define ___GFP_ZEROTAGS		0x800000u
#define ___GFP_SKIP_KASAN_POISON	0x1000000u
#ifdef CONFIG_LOCKDEP
#define ___GFP_NOLOCKDEP	0x2000000u
#else
#define ___GFP_NOLOCKDEP	0
#endif
/* If the above are modified, __GFP_BITS_SHIFT may need updating */
/// __GFP_BITS_SHIFT 用于记录当前一共有多少个独立的GFP标志位，用于生成 __GFP_BITS_MASK ，因此需要手动指定
/// GFP的个数，因此当以上的GFP标志发生变动时，需要调整__GFP_BITS_SHIFT

/*
 * Physical address zone modifiers (see linux/mmzone.h - low four bits)
 *
 * Do not put any conditional on these. If necessary modify the definitions
 * without the underscores and use them consistently. The definitions here may
 * be used in bit comparisons.
 */
/**
 * 低四位用于指定申请内存所在的zone，每个zone都有自己的一个伙伴系统. 这四个称之为 `zone modifiers`.
 * 这写标志可以用于位比较，但是不要单独的直接使用他们，而是使用不带下划线的标志，进行组合使用
 * */
#define __GFP_DMA	((__force gfp_t)___GFP_DMA)
#define __GFP_HIGHMEM	((__force gfp_t)___GFP_HIGHMEM)
#define __GFP_DMA32	((__force gfp_t)___GFP_DMA32)
#define __GFP_MOVABLE	((__force gfp_t)___GFP_MOVABLE)  /* ZONE_MOVABLE allowed */
#define GFP_ZONEMASK	(__GFP_DMA|__GFP_HIGHMEM|__GFP_DMA32|__GFP_MOVABLE)

/**
 * DOC: Page mobility and placement hints
 *
 * Page mobility and placement hints
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 * These flags provide hints about how mobile the page is. Pages with similar
 * mobility are placed within the same pageblocks to minimise problems due
 * to external fragmentation.
 *
 * %__GFP_MOVABLE (also a zone modifier) indicates that the page can be
 * moved by page migration during memory compaction or can be reclaimed.
 *
 * %__GFP_RECLAIMABLE is used for slab allocations that specify
 * SLAB_RECLAIM_ACCOUNT and whose pages can be freed via shrinkers.
 *
 * %__GFP_WRITE indicates the caller intends to dirty the page. Where possible,
 * these pages will be spread between local zones to avoid all the dirty
 * pages being in one zone (fair zone allocation policy).
 *
 * %__GFP_HARDWALL enforces the cpuset memory allocation policy.
 *
 * %__GFP_THISNODE forces the allocation to be satisfied from the requested
 * node with no fallbacks or placement policy enforcements.
 *
 * %__GFP_ACCOUNT causes the allocation to be accounted to kmemcg.
 */
/**
  * 页面移动性和放置提示
  *
  * 这些标志提供有关页面移动性的提示。 具有相似移动性的页面被放置在相同的页面块中，
  * 以最大限度地减少由于外部碎片引起的问题。
  *
  * %__GFP_MOVABLE (同时是一个zone标识符) 表示在内存压缩过程中可以通过页面迁移来移动该页面，也可以回收该页面。
  * %__GFP_RECLAIMABLE 通过指定SLAB_RECLAIM_ACCOUNT用于slab的内存分配，被指定该标志的page可以被shrinkers回收
  * %__GFP_WRITE 表示调用者可能会弄脏页面，在可能的情况下，将page分散在哥哥zone中，避免所有的脏页都在同一个zone(公平zone分配策略)
  * %__GFP_HARDWALL 强制执行 cpuset 内存分配策略。
  * %__GFP_THISNODE 强制从请求的节点满足分配，不执行fallback或放置策略。
  * %__GFP_ACCOUNT 分配被计数到kmemcg中，即内存属于kernel-memory，属于mem-cgrup的内容
  *
  * */
#define __GFP_RECLAIMABLE ((__force gfp_t)___GFP_RECLAIMABLE)
#define __GFP_WRITE	((__force gfp_t)___GFP_WRITE)
#define __GFP_HARDWALL   ((__force gfp_t)___GFP_HARDWALL)
#define __GFP_THISNODE	((__force gfp_t)___GFP_THISNODE)
#define __GFP_ACCOUNT	((__force gfp_t)___GFP_ACCOUNT)

/**
 * DOC: Watermark modifiers
 *
 * Watermark modifiers -- controls access to emergency reserves
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 * %__GFP_HIGH indicates that the caller is high-priority and that granting
 * the request is necessary before the system can make forward progress.
 * For example, creating an IO context to clean pages.
 *
 * %__GFP_ATOMIC indicates that the caller cannot reclaim or sleep and is
 * high priority. Users are typically interrupt handlers. This may be
 * used in conjunction with %__GFP_HIGH
 *
 * %__GFP_MEMALLOC allows access to all memory. This should only be used when
 * the caller guarantees the allocation will allow more memory to be freed
 * very shortly e.g. process exiting or swapping. Users either should
 * be the MM or co-ordinating closely with the VM (e.g. swap over NFS).
 * Users of this flag have to be extremely careful to not deplete the reserve
 * completely and implement a throttling mechanism which controls the
 * consumption of the reserve based on the amount of freed memory.
 * Usage of a pre-allocated pool (e.g. mempool) should be always considered
 * before using this flag.
 *
 * %__GFP_NOMEMALLOC is used to explicitly forbid access to emergency reserves.
 * This takes precedence over the %__GFP_MEMALLOC flag if both are set.
 */
/**
  * 水位标志符 ： 控制对于应急贮备内存的访问
  *
  * %__GFP_HIGH 表示调用是高优先级的，在系统可以make forward progress之前，批准请求是必要的。如创建I/O context 来清理回收page
  *
  * %__GFP_ATOMIC 表示调用者无法恢复或睡眠，且具有高优先级。用户通常是中断处理程序。这可能与%_GFP_HIGH一起使用
  *
  * %__GFP_MEMALLOC 允许访问所有内存。 这应该只在调用者保证分配之后将很快释放更多内存时使用，例如 进程退出或交换。
  * 		    用户应该是 MM or co-ordinating closely with the VM (e.g. swap over NFS)。
  * 		    此标志的用户必须非常小心，不要完全耗尽储备，并实施一种节流机制，根据释放的内存量控制储备的消耗。
  * 		    在使用此标志之前，应始终考虑使用预分配池（例如 mempool）。
  *
  * %__GFP_NOMEMALLOC 用于明确禁止使用应急储备。如果同时设置了__GFP_MEMALLOC和__GFP_NOMEMALLOC，则前者失效
  *
  * */
#define __GFP_ATOMIC	((__force gfp_t)___GFP_ATOMIC)
#define __GFP_HIGH	((__force gfp_t)___GFP_HIGH)
#define __GFP_MEMALLOC	((__force gfp_t)___GFP_MEMALLOC)
#define __GFP_NOMEMALLOC ((__force gfp_t)___GFP_NOMEMALLOC)

/**
 * DOC: Reclaim modifiers
 *
 * Reclaim modifiers
 * ~~~~~~~~~~~~~~~~~
 * Please note that all the following flags are only applicable to sleepable
 * allocations (e.g. %GFP_NOWAIT and %GFP_ATOMIC will ignore them).
 *
 * %__GFP_IO can start physical IO.
 *
 * %__GFP_FS can call down to the low-level FS. Clearing the flag avoids the
 * allocator recursing into the filesystem which might already be holding
 * locks.
 *
 * %__GFP_DIRECT_RECLAIM indicates that the caller may enter direct reclaim.
 * This flag can be cleared to avoid unnecessary delays when a fallback
 * option is available.
 *
 * %__GFP_KSWAPD_RECLAIM indicates that the caller wants to wake kswapd when
 * the low watermark is reached and have it reclaim pages until the high
 * watermark is reached. A caller may wish to clear this flag when fallback
 * options are available and the reclaim is likely to disrupt the system. The
 * canonical example is THP allocation where a fallback is cheap but
 * reclaim/compaction may cause indirect stalls.
 *
 * %__GFP_RECLAIM is shorthand to allow/forbid both direct and kswapd reclaim.
 *
 * The default allocator behavior depends on the request size. We have a concept
 * of so called costly allocations (with order > %PAGE_ALLOC_COSTLY_ORDER).
 * !costly allocations are too essential to fail so they are implicitly
 * non-failing by default (with some exceptions like OOM victims might fail so
 * the caller still has to check for failures) while costly requests try to be
 * not disruptive and back off even without invoking the OOM killer.
 * The following three modifiers might be used to override some of these
 * implicit rules
 *
 * %__GFP_NORETRY: The VM implementation will try only very lightweight
 * memory direct reclaim to get some memory under memory pressure (thus
 * it can sleep). It will avoid disruptive actions like OOM killer. The
 * caller must handle the failure which is quite likely to happen under
 * heavy memory pressure. The flag is suitable when failure can easily be
 * handled at small cost, such as reduced throughput
 *
 * %__GFP_RETRY_MAYFAIL: The VM implementation will retry memory reclaim
 * procedures that have previously failed if there is some indication
 * that progress has been made else where.  It can wait for other
 * tasks to attempt high level approaches to freeing memory such as
 * compaction (which removes fragmentation) and page-out.
 * There is still a definite limit to the number of retries, but it is
 * a larger limit than with %__GFP_NORETRY.
 * Allocations with this flag may fail, but only when there is
 * genuinely little unused memory. While these allocations do not
 * directly trigger the OOM killer, their failure indicates that
 * the system is likely to need to use the OOM killer soon.  The
 * caller must handle failure, but can reasonably do so by failing
 * a higher-level request, or completing it only in a much less
 * efficient manner.
 * If the allocation does fail, and the caller is in a position to
 * free some non-essential memory, doing so could benefit the system
 * as a whole.
 *
 * %__GFP_NOFAIL: The VM implementation _must_ retry infinitely: the caller
 * cannot handle allocation failures. The allocation could block
 * indefinitely but will never return with failure. Testing for
 * failure is pointless.
 * New users should be evaluated carefully (and the flag should be
 * used only when there is no reasonable failure policy) but it is
 * definitely preferable to use the flag rather than opencode endless
 * loop around allocator.
 * Using this flag for costly allocations is _highly_ discouraged.
 */
/**
  * 回收标志符
  * 首先以下所有标志，都仅仅适用于可阻塞(sleep)的内存分配，因此 %GFP_NOWAIT 和 %GFP_ATOMIC 将忽略它们
  *
  * %__GFP_IO 可以启动物理 IO。
  *
  * %__GFP_FS 可以向下调用 low-level FS。清除标志可以避免分配器递归到可能已经持有锁的文件系统中。
  *
  * %__GFP_DIRECT_RECLAIM 表示调用者可以进入直接回收。 当 fallback option 可用时，可以清除此标志以避免不必要的延迟。
  *
  * %__GFP_KSWAPD_RECLAIM 表示调用者希望在达到低水位线时唤醒 kswapd 并让它回收页面直到达到高水位线。
  *                       当 fallback option 可并且回收可能会破坏系统时，调用者可能希望清除此标志。
  *                       典型的例子是 THP(透明大页) 分配，其fallback很`便宜`，但回收/压缩可能会导致间接停顿。
  *
  * %__GFP_RECLAIM 是允许/禁止 DIRECT和 KSWAPD 回收的简写。即同时允许或禁止 %__GFP_DIRECT_RECLAIM和%__GFP_KSWAPD_RECLAIM
  *
  * 默认分配器行为会依赖于请求大小。
  * 我们有一个所谓的高成本分配（order > %PAGE_ALLOC_COSTLY_ORDER）的概念。(PAGE_ALLOC_COSTLY_ORDER默认是3，即当一次申请页面超过8(>=16,n=4,5,6..)个page时，被认为高成本)
  * !costly的分配(小page) 太重要而不能失败，因此默认情况下它们是隐式不失败的（有一些例外，比如 OOM `受害者`可能会失败，所以调用者仍然必须检查失败），
  * 而代价高昂的请求即使不调用 OOM killer, 也会尝试尽量不造成破坏并后退。
  *
  * 以下三个修饰符可用于覆盖其中一些隐式规则:
  *
  * %__GFP_NORETRY：VM 实现只会尝试非常轻量级的内存直接回收，以在内存压力下获取一些内存（因此它可以休眠）。
  * 		   它将避免像OOM杀手这样的破坏性行为。 调用者必须处理在沉重的内存压力下很可能发生的故障。
  * 		   该标志适用于可以以较低成本轻松处理故障的情况，例如 reduced throughput
  *
  * %__GFP_RETRY_MAYFAIL：如果有迹象表明其他地方已经取得进展，VM 实现将重试先前失败的内存回收过程。
  * 			 它可以等待其他任务尝试释放内存的高级方法，例如压缩（消除碎片）和分页。
  * 			 重试次数仍有明确限制，但比 %__GFP_NORETRY 的限制更大。 使用此标志的分配可能会失败，
  * 			 但只有在真正没有未使用的内存很少时才会失败。 虽然这些分配不会直接触发 OOM 杀手，
  * 			 但它们的失败表明系统可能很快需要使用 OOM 杀手。 调用者必须处理失败，
  * 			 但可以通过失败更高级别的请求或仅以效率低得多的方式完成它来合理地做到这一点。
  * 			 如果分配确实失败，并且调用者能够释放一些非必要的内存，那么这样做可能会使整个系统受益。
  *
  * %__GFP_NOFAIL：VM 实现_必须_无限重试：被调用者无法处理分配失败。 分配可以无限期地阻塞，但永远不会返回失败。
  *  		  测试失败是没有意义的。 应该仔细评估新用户（并且只有在没有合理的故障策略时才应该使用该标志），
  *  		  但绝对最好使用该标志而不是围绕分配器开放代码无限循环。 _highly_ 不鼓励使用此标志进行昂贵的分配。
  *
  * */
#define __GFP_IO	((__force gfp_t)___GFP_IO)
#define __GFP_FS	((__force gfp_t)___GFP_FS)
#define __GFP_DIRECT_RECLAIM	((__force gfp_t)___GFP_DIRECT_RECLAIM) /* Caller can reclaim */
#define __GFP_KSWAPD_RECLAIM	((__force gfp_t)___GFP_KSWAPD_RECLAIM) /* kswapd can wake */
#define __GFP_RECLAIM ((__force gfp_t)(___GFP_DIRECT_RECLAIM|___GFP_KSWAPD_RECLAIM))
#define __GFP_RETRY_MAYFAIL	((__force gfp_t)___GFP_RETRY_MAYFAIL)
#define __GFP_NOFAIL	((__force gfp_t)___GFP_NOFAIL)
#define __GFP_NORETRY	((__force gfp_t)___GFP_NORETRY)

/**
 * DOC: Action modifiers
 *
 * Action modifiers
 * ~~~~~~~~~~~~~~~~
 *
 * %__GFP_NOWARN suppresses allocation failure reports.
 *
 * %__GFP_COMP address compound page metadata.
 *
 * %__GFP_ZERO returns a zeroed page on success.
 *
 * %__GFP_ZEROTAGS returns a page with zeroed memory tags on success, if
 * __GFP_ZERO is set.
 *
 * %__GFP_SKIP_KASAN_POISON returns a page which does not need to be poisoned
 * on deallocation. Typically used for userspace pages. Currently only has an
 * effect in HW tags mode.
 */
/**
  * action 修饰符
  *
  * %__GFP_NOWARN 抑制分配失败报告。
  *
  * %__GFP_COMP 地址复合页元数据。
  *
  * %__GFP_ZERO 成功时返回一个归零的页面。
  *
  *  %__GFP_ZEROTAGS 如果设置了 __GFP_ZERO，则 %__GFP_ZEROTAGS 在成功时返回一个内存标记为零的页面。
  *
  * %__GFP_SKIP_KASAN_POISON 返回一个不需要在释放时中毒的页面。 通常用于用户空间页面。 目前仅在 HW 标签模式下有效。
  * */
#define __GFP_NOWARN	((__force gfp_t)___GFP_NOWARN)
#define __GFP_COMP	((__force gfp_t)___GFP_COMP)
#define __GFP_ZERO	((__force gfp_t)___GFP_ZERO)
#define __GFP_ZEROTAGS	((__force gfp_t)___GFP_ZEROTAGS)
#define __GFP_SKIP_KASAN_POISON	((__force gfp_t)___GFP_SKIP_KASAN_POISON)

/* Disable lockdep for GFP context tracking */
#define __GFP_NOLOCKDEP ((__force gfp_t)___GFP_NOLOCKDEP)

/* Room for N __GFP_FOO bits */
#define __GFP_BITS_SHIFT (25 + IS_ENABLED(CONFIG_LOCKDEP))
#define __GFP_BITS_MASK ((__force gfp_t)((1 << __GFP_BITS_SHIFT) - 1))

/**
 * DOC: Useful GFP flag combinations
 *
 * Useful GFP flag combinations
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 * Useful GFP flag combinations that are commonly used. It is recommended
 * that subsystems start with one of these combinations and then set/clear
 * %__GFP_FOO flags as necessary.
 *
 * %GFP_ATOMIC users can not sleep and need the allocation to succeed. A lower
 * watermark is applied to allow access to "atomic reserves".
 * The current implementation doesn't support NMI and few other strict
 * non-preemptive contexts (e.g. raw_spin_lock). The same applies to %GFP_NOWAIT.
 *
 * %GFP_KERNEL is typical for kernel-internal allocations. The caller requires
 * %ZONE_NORMAL or a lower zone for direct access but can direct reclaim.
 *
 * %GFP_KERNEL_ACCOUNT is the same as GFP_KERNEL, except the allocation is
 * accounted to kmemcg.
 *
 * %GFP_NOWAIT is for kernel allocations that should not stall for direct
 * reclaim, start physical IO or use any filesystem callback.
 *
 * %GFP_NOIO will use direct reclaim to discard clean pages or slab pages
 * that do not require the starting of any physical IO.
 * Please try to avoid using this flag directly and instead use
 * memalloc_noio_{save,restore} to mark the whole scope which cannot
 * perform any IO with a short explanation why. All allocation requests
 * will inherit GFP_NOIO implicitly.
 *
 * %GFP_NOFS will use direct reclaim but will not use any filesystem interfaces.
 * Please try to avoid using this flag directly and instead use
 * memalloc_nofs_{save,restore} to mark the whole scope which cannot/shouldn't
 * recurse into the FS layer with a short explanation why. All allocation
 * requests will inherit GFP_NOFS implicitly.
 *
 * %GFP_USER is for userspace allocations that also need to be directly
 * accessibly by the kernel or hardware. It is typically used by hardware
 * for buffers that are mapped to userspace (e.g. graphics) that hardware
 * still must DMA to. cpuset limits are enforced for these allocations.
 *
 * %GFP_DMA exists for historical reasons and should be avoided where possible.
 * The flags indicates that the caller requires that the lowest zone be
 * used (%ZONE_DMA or 16M on x86-64). Ideally, this would be removed but
 * it would require careful auditing as some users really require it and
 * others use the flag to avoid lowmem reserves in %ZONE_DMA and treat the
 * lowest zone as a type of emergency reserve.
 *
 * %GFP_DMA32 is similar to %GFP_DMA except that the caller requires a 32-bit
 * address.
 *
 * %GFP_HIGHUSER is for userspace allocations that may be mapped to userspace,
 * do not need to be directly accessible by the kernel but that cannot
 * move once in use. An example may be a hardware allocation that maps
 * data directly into userspace but has no addressing limitations.
 *
 * %GFP_HIGHUSER_MOVABLE is for userspace allocations that the kernel does not
 * need direct access to but can use kmap() when access is required. They
 * are expected to be movable via page reclaim or page migration. Typically,
 * pages on the LRU would also be allocated with %GFP_HIGHUSER_MOVABLE.
 *
 * %GFP_TRANSHUGE and %GFP_TRANSHUGE_LIGHT are used for THP allocations. They
 * are compound allocations that will generally fail quickly if memory is not
 * available and will not wake kswapd/kcompactd on failure. The _LIGHT
 * version does not attempt reclaim/compaction at all and is by default used
 * in page fault path, while the non-light is used by khugepaged.
 */

/**
 * 可用的标志组合
 * 常用的有用 GFP 标志组合。 建议子系统从这些组合之一开始，然后根据需要再设置/清除某些标志。
 *
 * %GFP_ATOMIC 调用者不能阻塞，需要分配成功。应用较低的水印以允许访问“原子储备”。 当前的实现不支持 NMI 和其他一些严格的非抢占式上下文（例如 raw_spin_lock）。这同样适用于 %GFP_NOWAIT。
 *
 * %GFP_KERNEL 是典型的内核内部分配。调用者需要 %ZONE_NORMAL 或更低的区域才能直接访问，但可以直接回收。
 *
 * %GFP_KERNEL_ACCOUNT 与 GFP_KERNEL 相同，只是分配计入 kmemcg。
 *
 * %GFP_NOWAIT 用于不应因直接回收、启动物理 IO 或使用任何文件系统回调而停止的内核分配。
 *
 * %GFP_NOIO 将使用直接回收来丢弃不需要启动任何物理 IO 的干净页面或平板页面。请尽量避免直接使用此标志，而是使用 memalloc_noio_{save,restore} 来标记无法执行任何 IO 的整个范围，并简要说明原因。所有分配请求都将隐式继承 GFP_NOIO。
 *
 * %GFP_NOFS 将使用直接回收但不会使用任何文件系统接口。请尽量避免直接使用此标志，而是使用 memalloc_nofs_{save,restore} 来标记不能/不应该递归到 FS 层的整个范围，并简要说明原因。所有分配请求都将隐式继承 GFP_NOFS。
 *
 * %GFP_USER 用于用户空间分配，也需要内核或硬件直接访问。它通常由硬件用于映射到硬件仍然必须 DMA 到的用户空间（例如图形）的缓冲区。对这些分配强制执行 cpuset 限制。
 *
 * %GFP_DMA 由于历史原因而存在，应尽可能避免。标志指示调用者要求使用最低区域（%ZONE_DMA 或 x86-64 上的 16M）。理想情况下，这将被删除，但需要仔细审核，因为一些用户确实需要它，而其他用户使用该标志来避免 %ZONE_DMA 中的低内存储备，并将最低区域视为一种紧急储备。
 *
 * %GFP_DMA32 与 %GFP_DMA 类似，只是调用者需要 32 位地址。
 *
 * %GFP_HIGHUSER 用于可以映射到用户空间的用户空间分配，不需要内核直接访问，但一旦使用就不能移动。一个示例可能是将数据直接映射到用户空间但没有寻址限制的硬件分配
 *
 * %GFP_HIGHUSER_MOVABLE 用于内核不需要直接访问但可以在需要访问时使用 kmap() 的用户空间分配。预计它们可以通过页面回收或页面迁移来移动。通常，LRU 上的页面也将使用 %GFP_HIGHUSER_MOVABLE 进行分配。
 *
 * %GFP_TRANSHUGE 和 %GFP_TRANSHUGE_LIGHT 用于 THP 分配。它们是复合分配，如果内存不可用，通常会很快失败，并且不会在失败时唤醒 kswapd/kcompactd。 _LIGHT 版本根本不尝试回收/压缩，默认情况下用于页面错误路径，而非轻版本由 khugepaged 使用。
 * */
#define GFP_ATOMIC	(__GFP_HIGH|__GFP_ATOMIC|__GFP_KSWAPD_RECLAIM)
#define GFP_KERNEL	(__GFP_RECLAIM | __GFP_IO | __GFP_FS)
#define GFP_KERNEL_ACCOUNT (GFP_KERNEL | __GFP_ACCOUNT)
#define GFP_NOWAIT	(__GFP_KSWAPD_RECLAIM)
#define GFP_NOIO	(__GFP_RECLAIM)
#define GFP_NOFS	(__GFP_RECLAIM | __GFP_IO)
#define GFP_USER	(__GFP_RECLAIM | __GFP_IO | __GFP_FS | __GFP_HARDWALL)
#define GFP_DMA		__GFP_DMA
#define GFP_DMA32	__GFP_DMA32
#define GFP_HIGHUSER	(GFP_USER | __GFP_HIGHMEM)
#define GFP_HIGHUSER_MOVABLE	(GFP_HIGHUSER | __GFP_MOVABLE | \
			 __GFP_SKIP_KASAN_POISON)
#define GFP_TRANSHUGE_LIGHT	((GFP_HIGHUSER_MOVABLE | __GFP_COMP | \
			 __GFP_NOMEMALLOC | __GFP_NOWARN) & ~__GFP_RECLAIM)
#define GFP_TRANSHUGE	(GFP_TRANSHUGE_LIGHT | __GFP_DIRECT_RECLAIM)

/* Convert GFP flags to their corresponding migrate type */
#define GFP_MOVABLE_MASK (__GFP_RECLAIMABLE|__GFP_MOVABLE)
#define GFP_MOVABLE_SHIFT 3

static inline int gfp_migratetype(const gfp_t gfp_flags)
{
	VM_WARN_ON((gfp_flags & GFP_MOVABLE_MASK) == GFP_MOVABLE_MASK);
	BUILD_BUG_ON((1UL << GFP_MOVABLE_SHIFT) != ___GFP_MOVABLE);
	BUILD_BUG_ON((___GFP_MOVABLE >> GFP_MOVABLE_SHIFT) != MIGRATE_MOVABLE);

	if (unlikely(page_group_by_mobility_disabled))
		return MIGRATE_UNMOVABLE;

	/* Group based on mobility */
	return (gfp_flags & GFP_MOVABLE_MASK) >> GFP_MOVABLE_SHIFT;
}
#undef GFP_MOVABLE_MASK
#undef GFP_MOVABLE_SHIFT

static inline bool gfpflags_allow_blocking(const gfp_t gfp_flags)
{
	return !!(gfp_flags & __GFP_DIRECT_RECLAIM);
}

/**
 * gfpflags_normal_context - is gfp_flags a normal sleepable context?
 * @gfp_flags: gfp_flags to test
 *
 * Test whether @gfp_flags indicates that the allocation is from the
 * %current context and allowed to sleep.
 *
 * An allocation being allowed to block doesn't mean it owns the %current
 * context.  When direct reclaim path tries to allocate memory, the
 * allocation context is nested inside whatever %current was doing at the
 * time of the original allocation.  The nested allocation may be allowed
 * to block but modifying anything %current owns can corrupt the outer
 * context's expectations.
 *
 * %true result from this function indicates that the allocation context
 * can sleep and use anything that's associated with %current.
 */
static inline bool gfpflags_normal_context(const gfp_t gfp_flags)
{
	return (gfp_flags & (__GFP_DIRECT_RECLAIM | __GFP_MEMALLOC)) ==
		__GFP_DIRECT_RECLAIM;
}

#ifdef CONFIG_HIGHMEM
#define OPT_ZONE_HIGHMEM ZONE_HIGHMEM
#else
#define OPT_ZONE_HIGHMEM ZONE_NORMAL
#endif

#ifdef CONFIG_ZONE_DMA
#define OPT_ZONE_DMA ZONE_DMA
#else
#define OPT_ZONE_DMA ZONE_NORMAL
#endif

#ifdef CONFIG_ZONE_DMA32
#define OPT_ZONE_DMA32 ZONE_DMA32
#else
#define OPT_ZONE_DMA32 ZONE_NORMAL
#endif

/*
 * GFP_ZONE_TABLE is a word size bitstring that is used for looking up the
 * zone to use given the lowest 4 bits of gfp_t. Entries are GFP_ZONES_SHIFT
 * bits long and there are 16 of them to cover all possible combinations of
 * __GFP_DMA, __GFP_DMA32, __GFP_MOVABLE and __GFP_HIGHMEM.
 *
 * The zone fallback order is MOVABLE=>HIGHMEM=>NORMAL=>DMA32=>DMA.
 * But GFP_MOVABLE is not only a zone specifier but also an allocation
 * policy. Therefore __GFP_MOVABLE plus another zone selector is valid.
 * Only 1 bit of the lowest 3 bits (DMA,DMA32,HIGHMEM) can be set to "1".
 *
 *       bit       result
 *       =================
 *       0x0    => NORMAL
 *       0x1    => DMA or NORMAL
 *       0x2    => HIGHMEM or NORMAL
 *       0x3    => BAD (DMA+HIGHMEM)
 *       0x4    => DMA32 or NORMAL
 *       0x5    => BAD (DMA+DMA32)
 *       0x6    => BAD (HIGHMEM+DMA32)
 *       0x7    => BAD (HIGHMEM+DMA32+DMA)
 *       0x8    => NORMAL (MOVABLE+0)
 *       0x9    => DMA or NORMAL (MOVABLE+DMA)
 *       0xa    => MOVABLE (Movable is valid only if HIGHMEM is set too)
 *       0xb    => BAD (MOVABLE+HIGHMEM+DMA)
 *       0xc    => DMA32 or NORMAL (MOVABLE+DMA32)
 *       0xd    => BAD (MOVABLE+DMA32+DMA)
 *       0xe    => BAD (MOVABLE+DMA32+HIGHMEM)
 *       0xf    => BAD (MOVABLE+DMA32+HIGHMEM+DMA)
 *
 * GFP_ZONES_SHIFT must be <= 2 on 32 bit platforms.
 */

/**
 * GFP_ZONE_TABLE 是一个Word大小(2字节)的位串，用于在给定 gfp_t 的最低 4 位的情况下查找要使用的区域。
 * 条目的长度为 GFP_ZONES_SHIFT 位，其中有 16 个涵盖 __GFP_DMA、__GFP_DMA32、__GFP_MOVABLE 和 __GFP_HIGHMEM 的所有可能组合。
 * 即，根据zone标志符，决定从哪些zone中分配内存
 *
 * zone 的后背选项(fallback)循序是：
 * MOVABLE=>HIGHMEM=>NORMAL=>DMA32=>DMA.
 * 即当指定为HIGHMEM，但是内存不足时，则可以从NORMAL获取内存
 *
 * NORMAL 是指那些可以被内核直接通过直接映射的方式访问的内存(不需要通过页表转义，而是直接加一个偏移量)，在X86中，内核只有1G的虚拟地址空间，因此normal大概是800+MB
 * 而剩下的物理内存则属于__GFP_HIGHMEM，但是在X86-64中，内核地址空间无敌大，有64TB，因此可以认为在x86_64中几乎所有的内存都是normal
 *
 * 在我的机器上，zone包括：
 *
 * DMA 0~16MB
 * DMA32 16MB~4GB
 * Normal 4GB 以上所有
 * Movable 虚拟区域
 * Device 虚拟区域
 *
 * GFP_MOVABLE 不仅是一个区域说明符，还是一个分配策略。 因此必须是 __GFP_MOVABLE 加上另一个区域选择器是有效的。
 * 最低 3 位（DMA、DMA32、HIGHMEM）中只有 1 位可以设置为“1”。
 * 说白了NORMAL是一直可以用的，normal是可以被内核直接映射使用的内存，在
 * 0000      	None        			NORMAL
 * 0001		__GFP_DMA			DMA / NORMAL
 * 0010		__GFP_HIGHMEM			HIGHMEM / NORMAL
 * 0100		__GFP_DMA32			DMA32 / NORMAL
 *
 * 1000      	__GFP_MOVABLE 			NORMAL
 * 1001		__GFP_MOVABLE & __GFP_DMA	DMA / NORMAL
 * 1010		__GFP_MOVABLE & __GFP_HIGHMEM	HIGHMEM / NORMAL
 * 1100		__GFP_MOVABLE & __GFP_DMA32	DMA32 / NORMAL
 *
 * bit	comb     				result			Actual(In x64 System, No ZONE_HIGHMEM)
 * ==========================================================================================================================
 * 0x0    None					=> NORMAL		ZONE_NORMAL
 * 0x1    __GFP_DMA				=> DMA or NORMAL	ZONE_DMA(有DMA就是DMA，没有则是Normal)
 * 0x2    __GFP_HIGHMEM				=> HIGHMEM or NORMAL	ZONE_NORMAL(没有HIGHMEM则是Normal)
 * 0x3    __GFP_(DMA+HIGHMEM)			=> BAD 			BAD
 * 0x4    __GFP_DMA32				=> DMA32 or NORMAL	ZONE_DMA32(有DMA32就是DMA32)
 * 0x5    __GFP_(DMA+DMA32)			=> BAD 			BAD
 * 0x6    __GFP_HIGHMEM+DMA32)			=> BAD			BAD
 * 0x7    __GFP_(HIGHMEM+DMA32+DMA)		=> BAD			BAD
 * 0x8    __GFP_MOVABLE				=> NORMAL		ZONE_NORMAL(单独使用MOVABLE无效)
 * 0x9    __GFP_(MOVABLE+DMA)			=> DMA or NORMAL	ZONE_DMA
 * 0xa    __GFP_(MOVABLE+HIGHMEM)		=> MOVABLE 		ZONE_MOVABLE(__GFP_MOVABLE必须要和__GFP_HIGHMEM一起使用)
 * 0xb    __GFP_(OVABLE+HIGHMEM+DMA)		=> BAD			BAD
 * 0xc    __GFP_(MOVABLE+DMA32)			=> DMA32 or NORMAL	ZONE_DMA32
 * 0xd    __GFP_(MOVABLE+DMA32+DMA)		=> BAD			BAD
 * 0xe    __GFP_(MOVABLE+DMA32+HIGHMEM)		=> BAD			BAD
 * 0xf    __GFP_(MOVABLE+DMA32+HIGHMEM+DMA)	=> BAD			BAD
 *
 * */

#if defined(CONFIG_ZONE_DEVICE) && (MAX_NR_ZONES-1) <= 4
/* ZONE_DEVICE is not a valid GFP zone specifier */
#define GFP_ZONES_SHIFT 2
#else
#define GFP_ZONES_SHIFT ZONES_SHIFT
#endif

#if 16 * GFP_ZONES_SHIFT > BITS_PER_LONG
#error GFP_ZONES_SHIFT too large to create GFP_ZONE_TABLE integer
#endif

#define GFP_ZONE_TABLE ( \
	(ZONE_NORMAL << 0 * GFP_ZONES_SHIFT)				       \
	| (OPT_ZONE_DMA << ___GFP_DMA * GFP_ZONES_SHIFT)		       \
	| (OPT_ZONE_HIGHMEM << ___GFP_HIGHMEM * GFP_ZONES_SHIFT)	       \
	| (OPT_ZONE_DMA32 << ___GFP_DMA32 * GFP_ZONES_SHIFT)		       \
	| (ZONE_NORMAL << ___GFP_MOVABLE * GFP_ZONES_SHIFT)		       \
	| (OPT_ZONE_DMA << (___GFP_MOVABLE | ___GFP_DMA) * GFP_ZONES_SHIFT)    \
	| (ZONE_MOVABLE << (___GFP_MOVABLE | ___GFP_HIGHMEM) * GFP_ZONES_SHIFT)\
	| (OPT_ZONE_DMA32 << (___GFP_MOVABLE | ___GFP_DMA32) * GFP_ZONES_SHIFT)\
)

/*
 * GFP_ZONE_BAD is a bitmap for all combinations of __GFP_DMA, __GFP_DMA32
 * __GFP_HIGHMEM and __GFP_MOVABLE that are not permitted. One flag per
 * entry starting with bit 0. Bit is set if the combination is not
 * allowed.
 */
#define GFP_ZONE_BAD ( \
	1 << (___GFP_DMA | ___GFP_HIGHMEM)				      \
	| 1 << (___GFP_DMA | ___GFP_DMA32)				      \
	| 1 << (___GFP_DMA32 | ___GFP_HIGHMEM)				      \
	| 1 << (___GFP_DMA | ___GFP_DMA32 | ___GFP_HIGHMEM)		      \
	| 1 << (___GFP_MOVABLE | ___GFP_HIGHMEM | ___GFP_DMA)		      \
	| 1 << (___GFP_MOVABLE | ___GFP_DMA32 | ___GFP_DMA)		      \
	| 1 << (___GFP_MOVABLE | ___GFP_DMA32 | ___GFP_HIGHMEM)		      \
	| 1 << (___GFP_MOVABLE | ___GFP_DMA32 | ___GFP_DMA | ___GFP_HIGHMEM)  \
)

static inline enum zone_type gfp_zone(gfp_t flags)
{
	enum zone_type z;
	int bit = (__force int) (flags & GFP_ZONEMASK);

	z = (GFP_ZONE_TABLE >> (bit * GFP_ZONES_SHIFT)) &
					 ((1 << GFP_ZONES_SHIFT) - 1);
	VM_BUG_ON((GFP_ZONE_BAD >> bit) & 1);
	return z;
}

/*
 * There is only one page-allocator function, and two main namespaces to
 * it. The alloc_page*() variants return 'struct page *' and as such
 * can allocate highmem pages, the *get*page*() variants return
 * virtual kernel addresses to the allocated page(s).
 */
/**
 * 选择node的zonelist，每个zone都有1个(UMA)或2个(NUMA) zonelist，
 * 其中一个zonelist仅仅包括了该node所有的zone即ZONELIST_NOFALLBACK；
 * 而另一个zonelist，还包括了其后备节点(fillback)上的所有的zone
 *
 * 如果在分配page时指定了__GFP_THISNODE，即表示，仅仅从本节节点上分配，则函数返回ZONELIST_NOFALLBACK
 * 否则返回ZONELIST_FALLBACK，这是目前唯一的需求，但是需要注意，UMA仅仅有的list也是ZONELIST_FALLBACK
 * */
static inline int gfp_zonelist(gfp_t flags)
{
#ifdef CONFIG_NUMA
	if (unlikely(flags & __GFP_THISNODE))
		return ZONELIST_NOFALLBACK;
#endif
	return ZONELIST_FALLBACK;
}

/*
 * We get the zone list from the current node and the gfp_mask.
 * This zone list contains a maximum of MAX_NUMNODES*MAX_NR_ZONES zones.
 * There are two zonelists per node, one for all zones with memory and
 * one containing just zones from the node the zonelist belongs to.
 *
 * For the case of non-NUMA systems the NODE_DATA() gets optimized to
 * &contig_page_data at compile-time.
 */
static inline struct zonelist *node_zonelist(int nid, gfp_t flags)
{
	return NODE_DATA(nid)->node_zonelists + gfp_zonelist(flags);
}

#ifndef HAVE_ARCH_FREE_PAGE
static inline void arch_free_page(struct page *page, int order) { }
#endif
#ifndef HAVE_ARCH_ALLOC_PAGE
static inline void arch_alloc_page(struct page *page, int order) { }
#endif
#ifndef HAVE_ARCH_MAKE_PAGE_ACCESSIBLE
static inline int arch_make_page_accessible(struct page *page)
{
	return 0;
}
#endif

struct page *__alloc_pages(gfp_t gfp, unsigned int order, int preferred_nid,
		nodemask_t *nodemask);

unsigned long __alloc_pages_bulk(gfp_t gfp, int preferred_nid,
				nodemask_t *nodemask, int nr_pages,
				struct list_head *page_list,
				struct page **page_array);

/* Bulk allocate order-0 pages */
static inline unsigned long
alloc_pages_bulk_list(gfp_t gfp, unsigned long nr_pages, struct list_head *list)
{
	return __alloc_pages_bulk(gfp, numa_mem_id(), NULL, nr_pages, list, NULL);
}

static inline unsigned long
alloc_pages_bulk_array(gfp_t gfp, unsigned long nr_pages, struct page **page_array)
{
	return __alloc_pages_bulk(gfp, numa_mem_id(), NULL, nr_pages, NULL, page_array);
}

static inline unsigned long
alloc_pages_bulk_array_node(gfp_t gfp, int nid, unsigned long nr_pages, struct page **page_array)
{
	if (nid == NUMA_NO_NODE)
		nid = numa_mem_id();

	return __alloc_pages_bulk(gfp, nid, NULL, nr_pages, NULL, page_array);
}

/*
 * Allocate pages, preferring the node given as nid. The node must be valid and
 * online. For more general interface, see alloc_pages_node().
 */
static inline struct page *
__alloc_pages_node(int nid, gfp_t gfp_mask, unsigned int order)
{
	VM_BUG_ON(nid < 0 || nid >= MAX_NUMNODES);
	VM_WARN_ON((gfp_mask & __GFP_THISNODE) && !node_online(nid));

	return __alloc_pages(gfp_mask, order, nid, NULL);
}

/*
 * Allocate pages, preferring the node given as nid. When nid == NUMA_NO_NODE,
 * prefer the current CPU's closest node. Otherwise node must be valid and
 * online.
 */
static inline struct page *alloc_pages_node(int nid, gfp_t gfp_mask,
						unsigned int order)
{
	if (nid == NUMA_NO_NODE)
		nid = numa_mem_id();

	return __alloc_pages_node(nid, gfp_mask, order);
}

#ifdef CONFIG_NUMA
struct page *alloc_pages(gfp_t gfp, unsigned int order);
extern struct page *alloc_pages_vma(gfp_t gfp_mask, int order,
			struct vm_area_struct *vma, unsigned long addr,
			int node, bool hugepage);
#define alloc_hugepage_vma(gfp_mask, vma, addr, order) \
	alloc_pages_vma(gfp_mask, order, vma, addr, numa_node_id(), true)
#else
static inline struct page *alloc_pages(gfp_t gfp_mask, unsigned int order)
{
	return alloc_pages_node(numa_node_id(), gfp_mask, order);
}
#define alloc_pages_vma(gfp_mask, order, vma, addr, node, false)\
	alloc_pages(gfp_mask, order)
#define alloc_hugepage_vma(gfp_mask, vma, addr, order) \
	alloc_pages(gfp_mask, order)
#endif
#define alloc_page(gfp_mask) alloc_pages(gfp_mask, 0)
#define alloc_page_vma(gfp_mask, vma, addr)			\
	alloc_pages_vma(gfp_mask, 0, vma, addr, numa_node_id(), false)

extern unsigned long __get_free_pages(gfp_t gfp_mask, unsigned int order);
extern unsigned long get_zeroed_page(gfp_t gfp_mask);

void *alloc_pages_exact(size_t size, gfp_t gfp_mask);
void free_pages_exact(void *virt, size_t size);
void * __meminit alloc_pages_exact_nid(int nid, size_t size, gfp_t gfp_mask);

#define __get_free_page(gfp_mask) \
		__get_free_pages((gfp_mask), 0)

#define __get_dma_pages(gfp_mask, order) \
		__get_free_pages((gfp_mask) | GFP_DMA, (order))

extern void __free_pages(struct page *page, unsigned int order);
extern void free_pages(unsigned long addr, unsigned int order);

struct page_frag_cache;
extern void __page_frag_cache_drain(struct page *page, unsigned int count);
extern void *page_frag_alloc_align(struct page_frag_cache *nc,
				   unsigned int fragsz, gfp_t gfp_mask,
				   unsigned int align_mask);

static inline void *page_frag_alloc(struct page_frag_cache *nc,
			     unsigned int fragsz, gfp_t gfp_mask)
{
	return page_frag_alloc_align(nc, fragsz, gfp_mask, ~0u);
}

extern void page_frag_free(void *addr);

#define __free_page(page) __free_pages((page), 0)
#define free_page(addr) free_pages((addr), 0)

void page_alloc_init(void);
void drain_zone_pages(struct zone *zone, struct per_cpu_pages *pcp);
void drain_all_pages(struct zone *zone);
void drain_local_pages(struct zone *zone);

void page_alloc_init_late(void);

/*
 * gfp_allowed_mask is set to GFP_BOOT_MASK during early boot to restrict what
 * GFP flags are used before interrupts are enabled. Once interrupts are
 * enabled, it is set to __GFP_BITS_MASK while the system is running. During
 * hibernation, it is used by PM to avoid I/O during memory allocation while
 * devices are suspended.
 */
extern gfp_t gfp_allowed_mask;

/* Returns true if the gfp_mask allows use of ALLOC_NO_WATERMARK */
bool gfp_pfmemalloc_allowed(gfp_t gfp_mask);

extern void pm_restrict_gfp_mask(void);
extern void pm_restore_gfp_mask(void);

extern gfp_t vma_thp_gfp_mask(struct vm_area_struct *vma);

#ifdef CONFIG_PM_SLEEP
extern bool pm_suspended_storage(void);
#else
static inline bool pm_suspended_storage(void)
{
	return false;
}
#endif /* CONFIG_PM_SLEEP */

#ifdef CONFIG_CONTIG_ALLOC
/* The below functions must be run on a range from a single zone. */
extern int alloc_contig_range(unsigned long start, unsigned long end,
			      unsigned migratetype, gfp_t gfp_mask);
extern struct page *alloc_contig_pages(unsigned long nr_pages, gfp_t gfp_mask,
				       int nid, nodemask_t *nodemask);
#endif
void free_contig_range(unsigned long pfn, unsigned long nr_pages);

#ifdef CONFIG_CMA
/* CMA stuff */
extern void init_cma_reserved_pageblock(struct page *page);
#endif

#endif /* __LINUX_GFP_H */
