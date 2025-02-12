// SPDX-License-Identifier: GPL-2.0
/*
 * linux/drivers/staging/erofs/unzip_vle.c
 *
 * Copyright (C) 2018 HUAWEI, Inc.
 *             http://www.huawei.com/
 * Created by Gao Xiang <gaoxiang25@huawei.com>
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file COPYING in the main directory of the Linux
 * distribution for more details.
 */
#include "unzip_vle.h"
#include <linux/prefetch.h>
#include <linux/migrate.h>

#include <trace/events/erofs.h>

#define PAGE_MIGRATE_LOCKED	((void *)0x5F10C10C)

static struct workqueue_struct *z_erofs_workqueue __read_mostly;
static struct kmem_cache *z_erofs_workgroup_cachep __read_mostly;

void z_erofs_exit_zip_subsystem(void)
{
	BUG_ON(z_erofs_workqueue == NULL);
	BUG_ON(z_erofs_workgroup_cachep == NULL);

	destroy_workqueue(z_erofs_workqueue);
	kmem_cache_destroy(z_erofs_workgroup_cachep);
}

static inline int init_unzip_workqueue(void)
{
	const unsigned onlinecpus = num_possible_cpus();

	/*
	 * we don't need too many threads, limiting threads
	 * could improve scheduling performance.
	 */
	z_erofs_workqueue = alloc_workqueue("erofs_unzipd",
		WQ_UNBOUND | WQ_HIGHPRI | WQ_CPU_INTENSIVE,
		onlinecpus + onlinecpus / 4);

	return z_erofs_workqueue != NULL ? 0 : -ENOMEM;
}

int z_erofs_init_zip_subsystem(void)
{
	z_erofs_workgroup_cachep =
		kmem_cache_create("erofs_compress",
		Z_EROFS_WORKGROUP_SIZE, 0,
		SLAB_RECLAIM_ACCOUNT, NULL);

	if (z_erofs_workgroup_cachep != NULL) {
		if (!init_unzip_workqueue())
			return 0;

		kmem_cache_destroy(z_erofs_workgroup_cachep);
	}
	return -ENOMEM;
}

enum z_erofs_vle_work_role {
	Z_EROFS_VLE_WORK_SECONDARY,
	Z_EROFS_VLE_WORK_PRIMARY,
	Z_EROFS_VLE_WORK_PRIMARY_TERMINAL,

	/*
	 * The current work has at least been linked with the following
	 * processed chained works, which means if the processing page
	 * is the tail partial page of the work, the current work can
	 * safely use the whole page, as illustrated below:
	 * +--------------+-------------------------------------------+
	 * |  tail page   |      head page (of the previous work)     |
	 * +--------------+-------------------------------------------+
	 *   /\  which belongs to the current work
	 * [  (*) this page can be used for the current work itself.  ]
	 */
	Z_EROFS_VLE_WORK_PRIMARY_FOLLOWED,
	Z_EROFS_VLE_WORK_MAX
};

struct z_erofs_vle_work_builder {
	enum z_erofs_vle_work_role role;
	/*
	 * 'hosted = false' means that the current workgroup doesn't belong to
	 * the owned chained workgroups. In the other words, it is none of our
	 * business to submit this workgroup.
	 */
	bool hosted;

	struct z_erofs_vle_workgroup *grp;
	struct z_erofs_vle_work *work;
	struct z_erofs_pagevec_ctor vector;

	/* pages used for reading the compressed data */
	struct page **compressed_pages;
	unsigned compressed_deficit;
};

#define VLE_WORK_BUILDER_INIT()	\
	{ .work = NULL, .role = Z_EROFS_VLE_WORK_PRIMARY_FOLLOWED }

#ifdef EROFS_FS_HAS_MANAGED_CACHE
static void z_erofs_vle_scan_cachepages(struct z_erofs_vle_work_builder *bl,
					struct address_space *mapping,
					pgoff_t index,
					unsigned int clusterpages,
					bool reserve_allocation,
					struct list_head *pagepool)
{
	struct page **const compressed_pages = bl->compressed_pages;
	const unsigned int compressed_deficit = bl->compressed_deficit;
	bool standalone = true;
	gfp_t gfp = mapping_gfp_constraint(mapping, ~__GFP_DIRECT_RECLAIM);
	unsigned int i, j = 0;

	if (bl->role < Z_EROFS_VLE_WORK_PRIMARY_TERMINAL)
		return;

	index += clusterpages - compressed_deficit;

	/* TODO: optimize by introducing find_get_pages_range */
	for (i = 0; i < compressed_deficit; ++i) {
		struct page *page, *newpage = NULL;
		z_erofs_ctptr_t v;

		if (READ_ONCE(compressed_pages[i]) != NULL)
			continue;

		page = find_get_page(mapping, index + i);
		if (page != NULL)
			v = tagptr_fold(z_erofs_ctptr_t, page, 1);
		else if (reserve_allocation) {
#if 1
			if (!list_empty(pagepool)) {
				newpage = lru_to_page(pagepool);
				list_del(&newpage->lru);
			} else {
				newpage = alloc_pages(gfp | __GFP_NOMEMALLOC | __GFP_NORETRY | __GFP_NOWARN, 0);
			}
			if (!newpage)
				goto rrr;
			newpage->mapping = Z_EROFS_MAPPING_PREALLOCATED;
			v = tagptr_fold(z_erofs_ctptr_t, newpage, 1);
#else
			v = tagptr_init(z_erofs_ctptr_t,
					EROFS_UNALLOCATED_CACHED_PAGE);
#endif
		} else {
rrr:
			if (standalone)
				j = i;
			standalone = false;
			continue;
		}

		if (cmpxchg(&compressed_pages[i],
			    NULL, tagptr_cast_ptr(v)) == NULL)
			continue;

		if (page != NULL)
			put_page(page);
		else if (newpage) {
			newpage->mapping = NULL;
			/* someone just allocated this page, drop our attempt */
			list_add(&newpage->lru, pagepool);
		}
	}

	bl->compressed_pages += j;
	bl->compressed_deficit = compressed_deficit - j;
	if (standalone)
		bl->role = Z_EROFS_VLE_WORK_PRIMARY;
}

/* called by erofs_shrinker to get rid of all compressed_pages */
int erofs_try_to_free_all_cached_pages(struct erofs_sb_info *sbi,
				       struct erofs_workgroup *egrp)
{
	struct z_erofs_vle_workgroup *const grp =
		container_of(egrp, struct z_erofs_vle_workgroup, obj);
	struct z_erofs_vle_work *const primary_work =
		z_erofs_vle_grab_primary_work(grp);
	struct address_space *const mapping = MNGD_MAPPING(sbi);
	const int clusterpages = erofs_clusterpages(sbi);
	int i;

	/* refcount of workgroup is now freezed as 1, check if it's in migration */
	if (!mutex_trylock(&primary_work->lock))
		return -EBUSY;

	/*
	 * refcount of workgroup is now freezed as 1,
	 * therefore no need to worry about available decompression users.
	 */
	for (i = 0; i < clusterpages; ++i) {
		struct page *page = READ_ONCE(grp->compressed_pages[i]);

		if (page == NULL)
			continue;

#ifdef CONFIG_EROFS_FS_DEBUG
		if (unlikely(page == PAGE_MIGRATE_LOCKED)) {
			/* cannot be migrate locked */
			errln("%s: %d, mngd_mapping(%px) migrate_locked in grp %px",
			      __func__, __LINE__, mapping, grp);

			print_hex_dump(KERN_ERR, "grp data: ", DUMP_PREFIX_OFFSET,
				       16, 1, grp, sizeof(struct z_erofs_vle_workgroup), true);
			DBG_BUGON(1);
		}
#endif

		/* block other users from reclaiming or migrating the page */
		if (!trylock_page(page)) {
			mutex_unlock(&primary_work->lock);
			return -EBUSY;
		}

#ifdef CONFIG_EROFS_FS_DEBUG
		if (unlikely(page->mapping != mapping)) {
			errln("%s: %d, page->mapping != mngd_mapping(%px) compressed_page %px in grp %px",
				__func__, __LINE__, mapping, page, grp);

			print_hex_dump(KERN_ERR, "grp data: ", DUMP_PREFIX_OFFSET,
				16, 1, grp, sizeof(struct erofs_workgroup), true);

			print_hex_dump(KERN_ERR, "page data: ", DUMP_PREFIX_OFFSET,
				16, 1, page, sizeof(struct page), true);

			unlock_page(page);
			continue;
		}
#endif

		/* barrier is implied in the following 'unlock_page' */
		WRITE_ONCE(grp->compressed_pages[i], NULL);

		set_page_private(page, 0);
		ClearPagePrivate(page);

		unlock_page(page);
		put_page(page);
	}
	mutex_unlock(&primary_work->lock);
	return 0;
}

int erofs_try_to_free_cached_page(struct address_space *mapping,
				  struct page *page)
{
	struct erofs_sb_info *const sbi = EROFS_SB(mapping->host->i_sb);
	const unsigned int clusterpages = erofs_clusterpages(sbi);

	struct z_erofs_vle_workgroup *grp;
	int ret = 0;	/* 0 - busy */

	/* prevent the workgroup from being freed */
	rcu_read_lock();
	grp = (void *)page_private(page);

	if (erofs_workgroup_try_to_freeze(&grp->obj, 1)) {
		unsigned int i;

		for (i = 0; i < clusterpages; ++i) {
			if (grp->compressed_pages[i] == page) {
				WRITE_ONCE(grp->compressed_pages[i], NULL);
				ret = 1;
				break;
			}
		}

#ifdef CONFIG_EROFS_FS_DEBUG
		if (unlikely(!ret)) {
			errln("%s: %d, cannot found compressed_page %px in grp %px",
				__func__, __LINE__, page, grp);

			print_hex_dump(KERN_ERR, "grp data: ", DUMP_PREFIX_OFFSET,
				16, 1, grp, sizeof(struct erofs_workgroup), true);

			print_hex_dump(KERN_ERR, "page data: ", DUMP_PREFIX_OFFSET,
				16, 1, page, sizeof(struct page), true);
		}
#endif

		erofs_workgroup_unfreeze(&grp->obj, 1);
	}
	rcu_read_unlock();

	if (ret) {
		ClearPagePrivate(page);
		put_page(page);
	}
	return ret;
}
#ifdef CONFIG_MIGRATION
int erofs_migrate_cached_page(struct address_space *mapping,
			      struct page *newpage,
			      struct page *page,
			      enum migrate_mode mode)
{
	struct erofs_sb_info *const sbi = EROFS_SB(mapping->host->i_sb);
	const unsigned int clusterpages = erofs_clusterpages(sbi);

	struct z_erofs_vle_workgroup *grp;
	struct z_erofs_vle_work *primary_work;
	bool locking;
	int rc;
	unsigned int i;

	if (!PagePrivate(page))
		return migrate_page(mapping, newpage, page, mode);

	/* the workgroup will not be freed with compressed page locked */
	grp = (void *)READ_ONCE(page_private(page));
	DBG_BUGON(!grp);

	primary_work = z_erofs_vle_grab_primary_work(grp);

	if (!mutex_trylock(&primary_work->lock)) {
		if (mode == MIGRATE_ASYNC)
			return -EAGAIN;

		mutex_lock(&primary_work->lock);
	}

	/* drop this migration attempt if freezed to 1 (reclaiming) */
	if (atomic_read(&grp->obj.refcount) == EROFS_LOCKED_MAGIC) {
		mutex_unlock(&primary_work->lock);
		return -EBUSY;
	}

	rc = migrate_page_move_mapping(mapping, newpage, page, NULL, mode, 0);
	if (rc != MIGRATEPAGE_SUCCESS) {
		mutex_unlock(&primary_work->lock);
		return rc;
	}

	locking = false;
	for (i = 0; i < clusterpages; ++i) {
		const struct page *victim =
			cmpxchg(&grp->compressed_pages[i], page, newpage);

		if (victim == page) {
			get_page(newpage);
			set_page_private(newpage, (unsigned long)grp);
			__SetPagePrivate(newpage);
			break;
		}
		if (victim == PAGE_MIGRATE_LOCKED)
			locking = true;
	}

	if (i >= clusterpages)
		DBG_BUGON(!locking);
	else
		locking = false;

	ClearPagePrivate(page);
	set_page_private(page, 0);

	migrate_page_copy(newpage, page);
	mutex_unlock(&primary_work->lock);

	if (!locking)
		put_page(page);
	return MIGRATEPAGE_SUCCESS;
}
#endif
#endif

/* page_type must be Z_EROFS_PAGE_TYPE_EXCLUSIVE */
static inline bool try_to_reuse_as_compressed_page(
	struct z_erofs_vle_work_builder *b,
	struct page *page)
{
	while (b->compressed_deficit) {
		--b->compressed_deficit;
		if (NULL == cmpxchg(b->compressed_pages++, NULL, page))
			return true;
	}

	return false;
}

/* callers must be with work->lock held */
static int z_erofs_vle_work_add_page(
	struct z_erofs_vle_work_builder *builder,
	struct page *page,
	enum z_erofs_page_type type)
{
	int ret;
	bool occupied;

	/* give priority for the compressed data storage */
	if (builder->role >= Z_EROFS_VLE_WORK_PRIMARY &&
		type == Z_EROFS_PAGE_TYPE_EXCLUSIVE &&
		try_to_reuse_as_compressed_page(builder, page))
		return 0;

	ret = z_erofs_pagevec_ctor_enqueue(&builder->vector,
		page, type, &occupied);
	builder->work->vcnt += (unsigned)ret;

	return ret ? 0 : -EAGAIN;
}

static enum z_erofs_vle_work_role try_to_claim_workgroup(
	struct z_erofs_vle_workgroup *grp,
	z_erofs_vle_owned_workgrp_t *owned_head,
	bool *hosted)
{
	DBG_BUGON(*hosted == true);

	/* let's claim these following types of workgroup */
retry:
	if (grp->next == Z_EROFS_VLE_WORKGRP_NIL) {
		/* type 1, nil workgroup */
		if (Z_EROFS_VLE_WORKGRP_NIL != cmpxchg(&grp->next,
			Z_EROFS_VLE_WORKGRP_NIL, *owned_head))
			goto retry;

		*owned_head = grp;
		*hosted = true;

		/* lucky, I am the followee :) */
		return Z_EROFS_VLE_WORK_PRIMARY_FOLLOWED;
	} else if (grp->next == Z_EROFS_VLE_WORKGRP_TAIL) {
		/*
		 * type 2, link to the end of a existing open chain,
		 * be careful that its submission itself is governed
		 * by the original owned chain.
		 */
		if (Z_EROFS_VLE_WORKGRP_TAIL != cmpxchg(&grp->next,
			Z_EROFS_VLE_WORKGRP_TAIL, *owned_head))
			goto retry;

		*owned_head = Z_EROFS_VLE_WORKGRP_TAIL;
		return Z_EROFS_VLE_WORK_PRIMARY_TERMINAL;
	}

	/* :( better luck next time */
	return Z_EROFS_VLE_WORK_PRIMARY;
}

struct z_erofs_vle_work_finder {
	struct super_block *sb;
	pgoff_t idx;
	unsigned pageofs;

	struct z_erofs_vle_workgroup **grp_ret;
	enum z_erofs_vle_work_role *role;
	z_erofs_vle_owned_workgrp_t *owned_head;
	bool *hosted;
};

static struct z_erofs_vle_work *
z_erofs_vle_work_lookup(const struct z_erofs_vle_work_finder *f)
{
	bool tag, primary;
	struct erofs_workgroup *egrp;
	struct z_erofs_vle_workgroup *grp;
	struct z_erofs_vle_work *work;

	egrp = erofs_find_workgroup(f->sb, f->idx, &tag);
	if (egrp == NULL) {
		*f->grp_ret = NULL;
		return NULL;
	}

	grp = container_of(egrp, struct z_erofs_vle_workgroup, obj);
	*f->grp_ret = grp;

	work = z_erofs_vle_grab_work(grp, f->pageofs);
	/* if multiref is disabled, `primary' is always true */
	primary = true;

	DBG_BUGON(work->pageofs != f->pageofs);

	/*
	 * lock must be taken first to avoid grp->next == NIL between
	 * claiming workgroup and adding pages:
	 *                        grp->next != NIL
	 *   grp->next = NIL
	 *   mutex_unlock_all
	 *                        mutex_lock(&work->lock)
	 *                        add all pages to pagevec
	 *
	 * [correct locking case 1]:
	 *   mutex_lock(grp->work[a])
	 *   ...
	 *   mutex_lock(grp->work[b])     mutex_lock(grp->work[c])
	 *   ...                          *role = SECONDARY
	 *                                add all pages to pagevec
	 *                                ...
	 *                                mutex_unlock(grp->work[c])
	 *   mutex_lock(grp->work[c])
	 *   ...
	 *   grp->next = NIL
	 *   mutex_unlock_all
	 *
	 * [correct locking case 2]:
	 *   mutex_lock(grp->work[b])
	 *   ...
	 *   mutex_lock(grp->work[a])
	 *   ...
	 *   mutex_lock(grp->work[c])
	 *   ...
	 *   grp->next = NIL
	 *   mutex_unlock_all
	 *                                mutex_lock(grp->work[a])
	 *                                *role = PRIMARY_OWNER
	 *                                add all pages to pagevec
	 *                                ...
	 */
	mutex_lock(&work->lock);

	*f->hosted = false;
	*f->role = !primary ? Z_EROFS_VLE_WORK_SECONDARY :
			/* claim the workgroup if possible */
			try_to_claim_workgroup(grp,f->owned_head, f->hosted);
	return work;
}

static struct z_erofs_vle_work *
z_erofs_vle_work_register(const struct z_erofs_vle_work_finder *f,
			  struct erofs_map_blocks *map)
{
	bool gnew = false;
	struct z_erofs_vle_workgroup *grp = *f->grp_ret;
	struct z_erofs_vle_work *work;

	/* if multiref is disabled, grp should never be nullptr */
	BUG_ON(grp != NULL);

	/* no available workgroup, let's allocate one */
	grp = kmem_cache_zalloc(z_erofs_workgroup_cachep, GFP_NOFS);
	if (unlikely(grp == NULL))
		return ERR_PTR(-ENOMEM);

	grp->obj.index = f->idx;
	grp->llen = map->m_llen;

	z_erofs_vle_set_workgrp_fmt(grp,
		(map->m_flags & EROFS_MAP_ZIPPED) ?
			Z_EROFS_VLE_WORKGRP_FMT_LZ4 :
			Z_EROFS_VLE_WORKGRP_FMT_PLAIN);
	atomic_set(&grp->obj.refcount, 1);

	/* new workgrps have been claimed as type 1 */
	WRITE_ONCE(grp->next, *f->owned_head);
	/* primary and followed work for all new workgrps */
	*f->role = Z_EROFS_VLE_WORK_PRIMARY_FOLLOWED;
	/* it should be submitted by ourselves */
	*f->hosted = true;

	gnew = true;
	work = z_erofs_vle_grab_primary_work(grp);
	work->pageofs = f->pageofs;

	mutex_init(&work->lock);

	/* lock all primary followed works before visible to others */
	if (unlikely(!mutex_trylock(&work->lock)))
		BUG();

	if (gnew) {
		int err = erofs_register_workgroup(f->sb, &grp->obj, 0);

		if (err) {
			mutex_unlock(&work->lock);
			kmem_cache_free(z_erofs_workgroup_cachep, grp);
			return ERR_PTR(-EAGAIN);
		}
	}

	*f->owned_head = *f->grp_ret = grp;
	return work;
}

#define builder_is_weak_followed(builder) \
	((builder)->role >= Z_EROFS_VLE_WORK_PRIMARY_TERMINAL)

#define builder_is_followed(builder) \
	((builder)->role >= Z_EROFS_VLE_WORK_PRIMARY_FOLLOWED)

static int z_erofs_vle_work_iter_begin(struct z_erofs_vle_work_builder *builder,
				       struct super_block *sb,
				       struct erofs_map_blocks *map,
				       z_erofs_vle_owned_workgrp_t *owned_head)
{
	const unsigned clusterpages = erofs_clusterpages(EROFS_SB(sb));
	struct z_erofs_vle_workgroup *grp;
	const struct z_erofs_vle_work_finder finder = {
		.sb = sb,
		.idx = erofs_blknr(map->m_pa),
		.pageofs = map->m_la & ~PAGE_MASK,
		.grp_ret = &grp,
		.role = &builder->role,
		.owned_head = owned_head,
		.hosted = &builder->hosted
	};
	struct z_erofs_vle_work *work;

	DBG_BUGON(builder->work != NULL);

	/* must be Z_EROFS_WORK_TAIL or the next chained work */
	DBG_BUGON(*owned_head == Z_EROFS_VLE_WORKGRP_NIL);
	DBG_BUGON(*owned_head == Z_EROFS_VLE_WORKGRP_TAIL_CLOSED);

	DBG_BUGON(erofs_blkoff(map->m_pa));

repeat:
	work = z_erofs_vle_work_lookup(&finder);
	if (work != NULL) {
		unsigned int orig_llen;

		/* increase workgroup `llen' if needed */
		while ((orig_llen = READ_ONCE(grp->llen)) < map->m_llen &&
		       orig_llen != cmpxchg_relaxed(&grp->llen,
						    orig_llen, map->m_llen))
			cpu_relax();
		goto got_it;
	}

	work = z_erofs_vle_work_register(&finder, map);
	if (unlikely(work == ERR_PTR(-EAGAIN)))
		goto repeat;

	if (unlikely(IS_ERR(work)))
		return PTR_ERR(work);
got_it:
	z_erofs_pagevec_ctor_init(&builder->vector,
		Z_EROFS_VLE_INLINE_PAGEVECS, work->pagevec, work->vcnt);

	if (builder->role >= Z_EROFS_VLE_WORK_PRIMARY) {
		/* enable possibly in-place decompression */
		builder->compressed_pages = grp->compressed_pages;
		builder->compressed_deficit = clusterpages;
	} else {
		builder->compressed_pages = NULL;
		builder->compressed_deficit = 0;
	}

	builder->grp = grp;
	builder->work = work;
	return 0;
}

/*
 * keep in mind that no referenced workgroups will be freed
 * only after a RCU grace period, so rcu_read_lock() could
 * prevent a workgroup from being freed.
 */
static void z_erofs_rcu_callback(struct rcu_head *head)
{
	struct z_erofs_vle_work *work =	container_of(head,
		struct z_erofs_vle_work, rcu);
	struct z_erofs_vle_workgroup *grp =
		z_erofs_vle_work_workgroup(work, true);

	kmem_cache_free(z_erofs_workgroup_cachep, grp);
}

void erofs_workgroup_free_rcu(struct erofs_workgroup *grp)
{
	struct z_erofs_vle_workgroup *const vgrp = container_of(grp,
		struct z_erofs_vle_workgroup, obj);
	struct z_erofs_vle_work *const work = &vgrp->work;

	call_rcu(&work->rcu, z_erofs_rcu_callback);
}

static void __z_erofs_vle_work_release(struct z_erofs_vle_workgroup *grp,
	struct z_erofs_vle_work *work __maybe_unused)
{
	erofs_workgroup_put(&grp->obj);
}

void z_erofs_vle_work_release(struct z_erofs_vle_work *work)
{
	struct z_erofs_vle_workgroup *grp =
		z_erofs_vle_work_workgroup(work, true);

	__z_erofs_vle_work_release(grp, work);
}

static inline bool
z_erofs_vle_work_iter_end(struct z_erofs_vle_work_builder *builder)
{
	struct z_erofs_vle_work *work = builder->work;

	if (work == NULL)
		return false;

	z_erofs_pagevec_ctor_exit(&builder->vector, false);
	mutex_unlock(&work->lock);

	/*
	 * if all pending pages are added, don't hold work reference
	 * any longer if the current work isn't hosted by ourselves.
	 */
	if (!builder->hosted)
		__z_erofs_vle_work_release(builder->grp, work);

	builder->work = NULL;
	builder->grp = NULL;
	return true;
}

static inline struct page *__stagingpage_alloc(struct list_head *pagepool,
					       gfp_t gfp)
{
	struct page *page = erofs_allocpage(pagepool, gfp);

	if (unlikely(page == NULL))
		return NULL;

	page->mapping = Z_EROFS_MAPPING_STAGING;
	return page;
}

struct z_erofs_vle_frontend {
	struct inode *const inode;

	struct z_erofs_vle_work_builder builder;
	struct erofs_map_blocks_iter m_iter;

	z_erofs_vle_owned_workgrp_t owned_head;

	bool initial;
#if (EROFS_FS_ZIP_CACHE_LVL >= 2)
	erofs_off_t cachedzone_la;
#endif
};

#define VLE_FRONTEND_INIT(__i) { \
	.inode = __i, \
	.m_iter = { \
		{ .m_llen = 0, .m_plen = 0 }, \
		.mpage = NULL \
	}, \
	.builder = VLE_WORK_BUILDER_INIT(), \
	.owned_head = Z_EROFS_VLE_WORKGRP_TAIL, \
	.initial = true, }

static int z_erofs_do_read_page(struct z_erofs_vle_frontend *fe,
				struct page *page,
				struct list_head *page_pool)
{
	struct super_block *const sb = fe->inode->i_sb;
	struct erofs_sb_info *const sbi __maybe_unused = EROFS_SB(sb);
	struct erofs_map_blocks_iter *const m = &fe->m_iter;
	struct erofs_map_blocks *const map = &m->map;
	struct z_erofs_vle_work_builder *const builder = &fe->builder;
	const loff_t offset = page_offset(page);

	bool tight = builder_is_weak_followed(builder);
	struct z_erofs_vle_work *work = builder->work;

	enum z_erofs_page_type page_type;
	unsigned cur, end, spiltted, index;
	int err = 0;

	/* register locked file pages as online pages in pack */
	z_erofs_onlinepage_init(page);

	spiltted = 0;
	end = PAGE_SIZE;
repeat:
	cur = end - 1;

	/* lucky, within the range of the current map_blocks */
	if (offset + cur >= map->m_la &&
		offset + cur < map->m_la + map->m_llen) {
		/* the work haven't exist (maybe due to allocation failure) */
		if (unlikely(!builder->work))
			goto rebegin_work;
		goto hitted;
	}

	/* go ahead the next map_blocks */
	debugln("%s: [out-of-range] pos %llu", __func__, offset + cur);

	if (z_erofs_vle_work_iter_end(builder))
		fe->initial = false;

	map->m_la = offset + cur;
	map->m_llen = 0;
	err = erofs_map_blocks_iter(fe->inode, map, &m->mpage, 0);
	if (unlikely(err))
		goto err_out;

rebegin_work:
	if (unlikely(!(map->m_flags & EROFS_MAP_MAPPED)))
		goto hitted;

	DBG_BUGON(map->m_plen != 1U << sbi->clusterbits);
	DBG_BUGON(erofs_blkoff(map->m_pa));

	err = z_erofs_vle_work_iter_begin(builder, sb, map, &fe->owned_head);
	if (unlikely(err))
		goto err_out;

#ifdef EROFS_FS_HAS_MANAGED_CACHE
	z_erofs_vle_scan_cachepages(builder, MNGD_MAPPING(sbi),
		erofs_blknr(map->m_pa),
		erofs_blknr(map->m_plen),
		/* compressed page caching selection strategy */
		fe->initial | (EROFS_FS_ZIP_CACHE_LVL >= 2 ?
			       map->m_la < fe->cachedzone_la : 0), page_pool);
#endif

	tight &= builder_is_weak_followed(builder);
	work = builder->work;
hitted:
	cur = end - min_t(unsigned, offset + end - map->m_la, end);
	if (unlikely(!(map->m_flags & EROFS_MAP_MAPPED))) {
		zero_user_segment(page, cur, end);
		goto next_part;
	}

	/* let's derive page type */
	page_type = cur ? Z_EROFS_VLE_PAGE_TYPE_HEAD :
		(!spiltted ? Z_EROFS_PAGE_TYPE_EXCLUSIVE :
			(tight ? Z_EROFS_PAGE_TYPE_EXCLUSIVE :
				Z_EROFS_VLE_PAGE_TYPE_TAIL_SHARED));

	if (cur)
		tight &= builder_is_followed(builder);
retry:
	err = z_erofs_vle_work_add_page(builder, page, page_type);
	/* should allocate an additional staging page for pagevec */
	if (err == -EAGAIN) {
		struct page *const newpage =
			__stagingpage_alloc(page_pool, GFP_NOFS);

		err = z_erofs_vle_work_add_page(builder,
			newpage, Z_EROFS_PAGE_TYPE_EXCLUSIVE);
		if (likely(!err))
			goto retry;
	}

	if (unlikely(err))
		goto err_out;

	index = page->index - map->m_la / PAGE_SIZE;

	/* FIXME! avoid the last relundant fixup & endio */
	z_erofs_onlinepage_fixup(page, index, true);

	/* bump up the number of spiltted parts of a page */
	++spiltted;

	if (unlikely(spiltted > 2)) {
		errln("%s, bad spiltted on page %px nid %llu index %lu",
			__func__, page, EROFS_V(fe->inode)->nid, page->index);
		BUG();
	}

	/* also update nr_pages */
	work->nr_pages = max_t(pgoff_t, work->nr_pages, index + 1);
next_part:
	/* can be used for verification */
	map->m_llen = offset + cur - map->m_la;

	end = cur;
	if (end > 0)
		goto repeat;

out:
	/* FIXME! avoid the last relundant fixup & endio */
	z_erofs_onlinepage_endio(page);

	debugln("%s, finish page: %pK spiltted: %u map->m_llen %llu",
		__func__, page, spiltted, map->m_llen);
	return err;

	/* if some error occurred while processing this page */
err_out:
	SetPageError(page);
	goto out;
}

static void z_erofs_vle_unzip_wq(struct work_struct *work);

static void z_erofs_vle_unzip_kickoff(void *ptr, int bios)
{
	tagptr1_t t = tagptr_init(tagptr1_t, ptr);
	struct z_erofs_vle_unzip_io *io = tagptr_unfold_ptr(t);
	bool background = tagptr_unfold_tags(t);

	if (!background) {
		unsigned long flags;

		spin_lock_irqsave(&io->u.wait.lock, flags);
		if (!atomic_add_return(bios, &io->pending_bios))
			wake_up_locked(&io->u.wait);
		spin_unlock_irqrestore(&io->u.wait.lock, flags);
		return;
	}

	if (!atomic_add_return(bios, &io->pending_bios)) {
#ifdef CONFIG_PREEMPT_COUNT
		if (in_atomic() || irqs_disabled())
			queue_work(z_erofs_workqueue, &io->u.work);
		else
			z_erofs_vle_unzip_wq(&io->u.work);
#else
		queue_work(z_erofs_workqueue, &io->u.work);
#endif
	}
}


#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 3, 0))
static inline void z_erofs_vle_read_endio(struct bio *bio, int err)
#else
static inline void z_erofs_vle_read_endio(struct bio *bio)
#endif
{
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 13, 0))
	const int err = bio->bi_status;
#elif (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 3, 0))
	const int err = bio->bi_error;
#endif
	unsigned i;
	struct bio_vec *bvec;

#ifdef EROFS_FS_HAS_MANAGED_CACHE
	struct address_space *mngda = NULL;
#endif

	bio_for_each_segment_all(bvec, bio, i) {
		struct page *page = bvec->bv_page;
		bool cachemngd = false;

		DBG_BUGON(PageUptodate(page));
		BUG_ON(page->mapping == NULL);

#ifdef EROFS_FS_HAS_MANAGED_CACHE
		if (unlikely(mngda == NULL && !z_erofs_is_stagingpage(page))) {
			struct inode *const inode = page->mapping->host;

			mngda = MNGD_MAPPING(EROFS_I_SB(inode));
		}

		/*
		 * If mngda has not gotten, it equals NULL,
		 * however, page->mapping never be NULL if working properly.
		 */
		cachemngd = (page->mapping == mngda);
#endif

		if (unlikely(err))
			SetPageError(page);
		else if (cachemngd)
			SetPageUptodate(page);

		if (cachemngd)
			unlock_page(page);
	}

	z_erofs_vle_unzip_kickoff(bio->bi_private, -1);
	bio_put(bio);
}

static struct page *z_pagemap_global[Z_EROFS_VLE_VMAP_GLOBAL_PAGES];
static DEFINE_MUTEX(z_pagemap_global_lock);

static int z_erofs_vle_unzip(struct super_block *sb,
	struct z_erofs_vle_workgroup *grp,
	struct list_head *page_pool)
{
	struct erofs_sb_info *const sbi = EROFS_SB(sb);
#ifdef EROFS_FS_HAS_MANAGED_CACHE
	struct address_space *const mngda = MNGD_MAPPING(sbi);
#endif
	const unsigned clusterpages = erofs_clusterpages(sbi);

	struct z_erofs_pagevec_ctor ctor;
	unsigned nr_pages;
	unsigned sparsemem_pages = 0;
	struct page *pages_onstack[Z_EROFS_VLE_VMAP_ONSTACK_PAGES];
	struct page **pages, **compressed_pages, *page;
	unsigned i, llen;

	enum z_erofs_page_type page_type;
	bool overlapped;
	struct z_erofs_vle_work *work;
	void *vout;
	int err;

	might_sleep();
	work = z_erofs_vle_grab_primary_work(grp);
	BUG_ON(!READ_ONCE(work->nr_pages));

	mutex_lock(&work->lock);
	nr_pages = work->nr_pages;

	if (likely(nr_pages <= Z_EROFS_VLE_VMAP_ONSTACK_PAGES))
		pages = pages_onstack;
	else if (nr_pages <= Z_EROFS_VLE_VMAP_GLOBAL_PAGES &&
		mutex_trylock(&z_pagemap_global_lock))
		pages = z_pagemap_global;
	else {
repeat:
		pages = kvmalloc_array(nr_pages,
			sizeof(struct page *), GFP_KERNEL);

		/* fallback to global pagemap for the lowmem scenario */
		if (unlikely(pages == NULL)) {
			if (nr_pages > Z_EROFS_VLE_VMAP_GLOBAL_PAGES)
				goto repeat;
			else {
				mutex_lock(&z_pagemap_global_lock);
				pages = z_pagemap_global;
			}
		}
	}

	for (i = 0; i < nr_pages; ++i)
		pages[i] = NULL;

	z_erofs_pagevec_ctor_init(&ctor,
		Z_EROFS_VLE_INLINE_PAGEVECS, work->pagevec, 0);

	for (i = 0; i < work->vcnt; ++i) {
		unsigned pagenr;

		page = z_erofs_pagevec_ctor_dequeue(&ctor, &page_type);

		/* all pages in pagevec ought to be valid */
		DBG_BUGON(page == NULL);
		DBG_BUGON(page->mapping == NULL);

		if (z_erofs_gather_if_stagingpage(page_pool, page))
			continue;

		if (page_type == Z_EROFS_VLE_PAGE_TYPE_HEAD)
			pagenr = 0;
		else
			pagenr = z_erofs_onlinepage_index(page);

		BUG_ON(pagenr >= nr_pages);
		BUG_ON(pages[pagenr] != NULL);

		pages[pagenr] = page;
	}
	sparsemem_pages = i;

	z_erofs_pagevec_ctor_exit(&ctor, true);

	overlapped = false;
	compressed_pages = grp->compressed_pages;

	err = 0;
	for (i = 0; i < clusterpages; ++i) {
		unsigned pagenr;

		page = compressed_pages[i];

		/* all compressed pages ought to be valid */
		DBG_BUGON(page == NULL);
		DBG_BUGON(page->mapping == NULL);

		if (z_erofs_is_stagingpage(page))
			continue;
#ifdef EROFS_FS_HAS_MANAGED_CACHE
		else if (page->mapping == mngda) {
			if (unlikely(!PageUptodate(page))) {
				/* PageError should be set in z_erofs_vle_read_endio */
				DBG_BUGON(!PageError(page));
				err = -EIO;
			}
			continue;
		}
#endif

		/* only non-head page could be reused as a compressed page */
		pagenr = z_erofs_onlinepage_index2(page);

		BUG_ON(pagenr >= nr_pages);
		BUG_ON(pages[pagenr] != NULL);
		++sparsemem_pages;
		pages[pagenr] = page;

		overlapped = true;
	}

	if (err)
		goto out;

	llen = (nr_pages << PAGE_SHIFT) - work->pageofs;

	if (z_erofs_vle_workgrp_fmt(grp) == Z_EROFS_VLE_WORKGRP_FMT_PLAIN) {
		/* FIXME! this should be fixed in the future */
		BUG_ON(grp->llen != llen);

		err = z_erofs_vle_plain_copy(compressed_pages, clusterpages,
			pages, nr_pages, work->pageofs);
		goto out;
	}

	if (llen > grp->llen)
		llen = grp->llen;

	err = z_erofs_vle_unzip_fast_percpu(compressed_pages,
		clusterpages, pages, llen, work->pageofs,
		test_opt(sbi, LZ4ASM));
	if (err != -ENOTSUPP)
		goto out;

	if (sparsemem_pages >= nr_pages) {
		BUG_ON(sparsemem_pages > nr_pages);
		goto skip_allocpage;
	}

	for (i = 0; i < nr_pages; ++i) {
		if (pages[i] != NULL)
			continue;

		pages[i] = __stagingpage_alloc(page_pool, GFP_NOFS
//#if defined(CONFIG_CMA) && defined(___GFP_CMA)
//			     | ___GFP_CMA
//#endif
		);
	}

skip_allocpage:
	vout = erofs_vmap(pages, nr_pages);
	if (!vout) {
		err = -ENOMEM;
		goto out;
	}

	err = z_erofs_vle_unzip_vmap(compressed_pages,
		clusterpages, vout, llen,
		work->pageofs, overlapped, test_opt(sbi, LZ4ASM));

	erofs_vunmap(vout, nr_pages);

out:
	/* must handle all compressed pages before endding pages */
	for (i = 0; i < clusterpages; ++i) {
		page = compressed_pages[i];

#ifdef EROFS_FS_HAS_MANAGED_CACHE
		if (page->mapping == mngda)
			continue;
#endif
		/* recycle all individual staging pages */
		(void)z_erofs_gather_if_stagingpage(page_pool, page);

		WRITE_ONCE(compressed_pages[i], NULL);
	}

	for (i = 0; i < nr_pages; ++i) {
		page = pages[i];

		if (!page)
			continue;

		DBG_BUGON(page->mapping == NULL);

		/* recycle all individual staging pages */
		if (z_erofs_gather_if_stagingpage(page_pool, page))
			continue;

		if (unlikely(err < 0))
			SetPageError(page);

		z_erofs_onlinepage_endio(page);
	}

	if (pages == z_pagemap_global)
		mutex_unlock(&z_pagemap_global_lock);
	else if (unlikely(pages != pages_onstack))
		kvfree(pages);

	work->nr_pages = 0;
	work->vcnt = 0;

	/* all work locks MUST be taken before the following line */

	WRITE_ONCE(grp->next, Z_EROFS_VLE_WORKGRP_NIL);

	/* all work locks SHOULD be released right now */
	mutex_unlock(&work->lock);

	z_erofs_vle_work_release(work);
	return err;
}

static void z_erofs_vle_unzip_all(struct super_block *sb,
				  struct z_erofs_vle_unzip_io *io,
				  struct list_head *page_pool)
{
	z_erofs_vle_owned_workgrp_t owned = io->head;

	while (owned != Z_EROFS_VLE_WORKGRP_TAIL_CLOSED) {
		struct z_erofs_vle_workgroup *grp;

		/* no possible that 'owned' equals Z_EROFS_WORK_TPTR_TAIL */
		DBG_BUGON(owned == Z_EROFS_VLE_WORKGRP_TAIL);

		/* no possible that 'owned' equals NULL */
		DBG_BUGON(owned == Z_EROFS_VLE_WORKGRP_NIL);

		grp = owned;
		owned = READ_ONCE(grp->next);

		z_erofs_vle_unzip(sb, grp, page_pool);
	}
}

static void z_erofs_vle_unzip_wq(struct work_struct *work)
{
	struct z_erofs_vle_unzip_io_sb *iosb = container_of(work,
		struct z_erofs_vle_unzip_io_sb, io.u.work);
	LIST_HEAD(page_pool);

	BUG_ON(iosb->io.head == Z_EROFS_VLE_WORKGRP_TAIL_CLOSED);
	z_erofs_vle_unzip_all(iosb->sb, &iosb->io, &page_pool);

	put_pages_list(&page_pool);
	kvfree(iosb);
}

static inline struct z_erofs_vle_unzip_io *
prepare_io_handler(struct super_block *sb,
		   struct z_erofs_vle_unzip_io *io,
		   bool background)
{
	struct z_erofs_vle_unzip_io_sb *iosb;

	if (!background) {
		/* waitqueue available for foreground io */
		BUG_ON(io == NULL);

		init_waitqueue_head(&io->u.wait);
		atomic_set(&io->pending_bios, 0);
		goto out;
	}

	if (io != NULL)
		BUG();
	else {
		/* allocate extra io descriptor for background io */
		iosb = kvzalloc(sizeof(struct z_erofs_vle_unzip_io_sb),
			GFP_KERNEL | __GFP_NOFAIL);
		BUG_ON(iosb == NULL);

		io = &iosb->io;
	}

	iosb->sb = sb;
	INIT_WORK(&io->u.work, z_erofs_vle_unzip_wq);
out:
	io->head = Z_EROFS_VLE_WORKGRP_TAIL_CLOSED;
	return io;
}

static struct page *
z_erofs_workgrp_grab_page_for_submission(struct z_erofs_vle_workgroup *grp,
					 pgoff_t first_index,
					 unsigned int nr,
					 struct list_head *pagepool,
					 gfp_t gfp,
					 struct address_space *mc)
{
	struct address_space *mapping;
	struct page *oldpage, *page;
	bool tocache = false;
	z_erofs_ctptr_t t;
	int justfound;

repeat:
	page = xchg(&grp->compressed_pages[nr], PAGE_MIGRATE_LOCKED);
	oldpage = PAGE_MIGRATE_LOCKED;

	if (page == NULL)
		goto out_allocpage;

#ifdef EROFS_FS_HAS_MANAGED_CACHE
	if (page == EROFS_UNALLOCATED_CACHED_PAGE) {
		tocache = true;
		goto out_allocpage;
	}
#endif

	/* parse the compressed tagged pointer */
	t = tagptr_init(z_erofs_ctptr_t, page);
	justfound = tagptr_unfold_tags(t);
	page = tagptr_unfold_ptr(t);

	mapping = READ_ONCE(page->mapping);

#ifndef EROFS_FS_HAS_MANAGED_CACHE
	/* if managed cache is disabled, it is impossible `justfound' */
	DBG_BUGON(justfound);

	/* and it should be locked, not uptodate, and not truncated */
	DBG_BUGON(!PageLocked(page));
	DBG_BUGON(PageUptodate(page));
	DBG_BUGON(mapping == NULL);

	goto out;
#else
	if (mapping == Z_EROFS_MAPPING_PREALLOCATED) {
		WRITE_ONCE(grp->compressed_pages[nr], page);
		goto out_add_to_managed_cache;
	}


	/* all unmanaged pages are locked, so it's impossible to be NULL */
	if (mapping != NULL && mapping != mc) {
		WRITE_ONCE(grp->compressed_pages[nr], page);
		/* ought to be unmanaged pages */
		goto out;
	}

	lock_page(page);
#ifdef CONFIG_EROFS_FS_DEBUG
	/* page reclaim went wrong, should never happen */
	if (unlikely(justfound && PagePrivate(page))) {
		struct erofs_workgroup *ogrp;

		errln("%s: %d: page %px refcount %d grp %px (index %lu count %d) "
			"page_private %lx",
			__func__, __LINE__, page, page_count(page), grp, grp->obj.index,
			atomic_read(&grp->obj.refcount), page_private(page));

		print_hex_dump(KERN_ERR, "grp data: ", DUMP_PREFIX_OFFSET,
			16, 1, grp, sizeof(struct erofs_workgroup), true);

		rcu_read_lock();
		ogrp = (void *)page_private(page);
		errln("%s: %d: page %px page_private %px", __func__, __LINE__, page, ogrp);

		print_hex_dump(KERN_ERR, "ogrp data: ", DUMP_PREFIX_OFFSET,
			16, 1, ogrp, sizeof(struct erofs_workgroup), true);
		rcu_read_unlock();
		BUG();
	}
#endif

	if (page->mapping == mc) {
		WRITE_ONCE(grp->compressed_pages[nr], page);

		if (!PagePrivate(page)) {
			if (!justfound)
				get_page(page);
			justfound = 0;
			set_page_private(page, (unsigned long)grp);
			SetPagePrivate(page);
		}

		if (PageUptodate(page)) {
			unlock_page(page);
			page = NULL;
		}
		goto out;
	}

	/* for the truncation case (page locked) */
	DBG_BUGON(page->mapping != NULL);
	DBG_BUGON(PagePrivate(page));

	tocache = true;
#ifdef CONFIG_EROFS_FS_DEBUG
	errln("%s: %d truncated page %px (count %d) grp %px (count %d)",
	      __func__, __LINE__, page, page_count(page), grp, atomic_read(&grp->obj.refcount));
#endif
	unlock_page(page);
	put_page(page);
	/* fallthrough */
#endif
out_allocpage:
	if (tocache)
		gfp |= __GFP_MOVABLE
#if defined(CONFIG_CMA) && defined(___GFP_CMA)
			     | ___GFP_CMA
#endif
		;

	page = __stagingpage_alloc(pagepool, gfp);
	if (oldpage != cmpxchg(&grp->compressed_pages[nr], oldpage, page)) {
		list_add(&page->lru, pagepool);
		cpu_relax();
		goto repeat;
	}
#ifdef EROFS_FS_HAS_MANAGED_CACHE
	if (!tocache)
		goto out;
out_add_to_managed_cache:
	if (add_to_page_cache_lru(page, mc, first_index + nr, gfp)) {
#ifdef CONFIG_EROFS_FS_DEBUG
		errln("%s: %d add_to_page_cache_lru failed page %px (count %d) grp %px (count %d)",
			__func__, __LINE__, page, page_count(page), grp, atomic_read(&grp->obj.refcount));
#endif
		page->mapping = Z_EROFS_MAPPING_STAGING;
		goto out;
	}

	set_page_private(page, (unsigned long)grp);
	SetPagePrivate(page);
#endif
out:	/* the only exit (for tracing and debugging) */
	return page;
}

#ifdef EROFS_FS_HAS_MANAGED_CACHE
#define __FSIO_1 1
#else
#define __FSIO_1 0
#endif

static bool z_erofs_vle_submit_all(struct super_block *sb,
				   z_erofs_vle_owned_workgrp_t owned_head,
				   struct list_head *pagepool,
				   struct z_erofs_vle_unzip_io *fg_io,
				   bool force_fg,
				   unsigned *io_submitted)
{
	struct erofs_sb_info *const sbi = EROFS_SB(sb);
	const unsigned clusterpages = erofs_clusterpages(sbi);
	const gfp_t gfp = GFP_NOFS;
#ifdef EROFS_FS_HAS_MANAGED_CACHE
	struct address_space *const mc = MNGD_MAPPING(sbi);
	struct z_erofs_vle_workgroup *lstgrp_noio = NULL, *lstgrp_io = NULL;
#endif
	struct z_erofs_vle_unzip_io *ios[1 + __FSIO_1];
	struct bio *bio;
	tagptr1_t bi_private;
	/* since bio will be NULL, no need to initialize last_index */
	pgoff_t uninitialized_var(last_index);
	bool force_submit = false;
	unsigned nr_bios;

	if (unlikely(owned_head == Z_EROFS_VLE_WORKGRP_TAIL))
		return false;

	/*
	 * force_fg == 1, (io, fg_io[0]) no io, (io, fg_io[1]) need submit io
	 * force_fg == 0, (io, fg_io[0]) no io; (io[1], bg_io) need submit io
	 */
#ifdef EROFS_FS_HAS_MANAGED_CACHE
	ios[0] = prepare_io_handler(sb, fg_io + 0, false);
#endif

	if (force_fg) {
		ios[__FSIO_1] = prepare_io_handler(sb, fg_io + __FSIO_1, false);
		bi_private = tagptr_fold(tagptr1_t, ios[__FSIO_1], 0);
	} else {
		ios[__FSIO_1] = prepare_io_handler(sb, NULL, true);
		bi_private = tagptr_fold(tagptr1_t, ios[__FSIO_1], 1);
	}

	nr_bios = 0;
	force_submit = false;
	bio = NULL;

	/* by default, all need io submission */
	ios[__FSIO_1]->head = owned_head;

	do {
		struct z_erofs_vle_workgroup *grp;
		pgoff_t first_index;
		struct page *page;
		unsigned i = 0;
		unsigned int noio = 0;
		int err;

		/* no possible 'owned_head' equals the following */
		DBG_BUGON(owned_head == Z_EROFS_VLE_WORKGRP_TAIL_CLOSED);
		DBG_BUGON(owned_head == Z_EROFS_VLE_WORKGRP_NIL);

		grp = owned_head;

		/* close the main owned chain at first */
		owned_head = cmpxchg(&grp->next, Z_EROFS_VLE_WORKGRP_TAIL,
			Z_EROFS_VLE_WORKGRP_TAIL_CLOSED);

		first_index = grp->obj.index;
		force_submit |= (first_index != last_index + 1);

		/* fulfill all compressed pages */
repeat:
		page = z_erofs_workgrp_grab_page_for_submission(grp,
			first_index, i, pagepool, gfp, mc);

		if (page == NULL) {
			force_submit = true;
			++noio;
			goto skippage;
		}

		if (bio != NULL && force_submit) {
submit_bio_retry:
			__submit_bio(bio, REQ_OP_READ, 0);
			bio = NULL;
		}

		if (bio == NULL) {
			bio = erofs_grab_bio(sb, first_index + i,
				BIO_MAX_PAGES, z_erofs_vle_read_endio, true);
			bio->bi_private = tagptr_cast_ptr(bi_private);

			++nr_bios;
		}

		err = bio_add_page(bio, page, PAGE_SIZE, 0);
		if (err < PAGE_SIZE)
			goto submit_bio_retry;

		force_submit = false;
		last_index = first_index + i;
skippage:
		if (++i < clusterpages)
			goto repeat;

#ifdef EROFS_FS_HAS_MANAGED_CACHE
		if (noio < clusterpages) {
			lstgrp_io = grp;
		} else {
			z_erofs_vle_owned_workgrp_t iogrp_next =
				owned_head == Z_EROFS_VLE_WORKGRP_TAIL ?
				Z_EROFS_VLE_WORKGRP_TAIL_CLOSED :
				owned_head;

			if (lstgrp_io == NULL)
				ios[1]->head = iogrp_next;
			else
				WRITE_ONCE(lstgrp_io->next, iogrp_next);

			if (lstgrp_noio == NULL)
				ios[0]->head = grp;
			else
				WRITE_ONCE(lstgrp_noio->next, grp);

			lstgrp_noio = grp;
		}
#endif
	} while (owned_head != Z_EROFS_VLE_WORKGRP_TAIL);

	if (bio != NULL)
		__submit_bio(bio, REQ_OP_READ, 0);

	if (io_submitted)
		*io_submitted = nr_bios;

#ifndef EROFS_FS_HAS_MANAGED_CACHE
	BUG_ON(!nr_bios);
#else
	if (lstgrp_noio != NULL)
		WRITE_ONCE(lstgrp_noio->next, Z_EROFS_VLE_WORKGRP_TAIL_CLOSED);

	if (!force_fg && !nr_bios) {
		kvfree(container_of(ios[1],
			struct z_erofs_vle_unzip_io_sb, io));
		return true;
	}
#endif

	z_erofs_vle_unzip_kickoff(tagptr_cast_ptr(bi_private), nr_bios);
	return true;
}

static void z_erofs_submit_and_unzip(struct z_erofs_vle_frontend *f,
				     struct list_head *pagepool,
				     bool force_fg,
				     unsigned *io_submitted)
{
	struct super_block *sb = f->inode->i_sb;
	struct z_erofs_vle_unzip_io io[1 + __FSIO_1];

	if (!z_erofs_vle_submit_all(sb, f->owned_head, pagepool, io, force_fg, io_submitted))
		return;

#ifdef EROFS_FS_HAS_MANAGED_CACHE
	z_erofs_vle_unzip_all(sb, &io[0], pagepool);
#endif
	if (!force_fg)
		return;

	/* wait until all bios are completed */
	io_wait_event(io[__FSIO_1].u.wait,
		      !atomic_read(&io[__FSIO_1].pending_bios));

	/* let's synchronous decompression */
	z_erofs_vle_unzip_all(sb, &io[__FSIO_1], pagepool);
}

static int z_erofs_vle_normalaccess_readpage(struct file *file,
					     struct page *page)
{
	struct inode *const inode = page->mapping->host;
	struct z_erofs_vle_frontend f = VLE_FRONTEND_INIT(inode);
	int err;
	LIST_HEAD(pagepool);

	trace_erofs_readpage(page, false);

#if (EROFS_FS_ZIP_CACHE_LVL >= 2)
	f.cachedzone_la = page->index << PAGE_SHIFT;
#endif
	err = z_erofs_do_read_page(&f, page, &pagepool);
	(void)z_erofs_vle_work_iter_end(&f.builder);

	if (err) {
		errln("%s, failed to read, err [%d]", __func__, err);
		goto out;
	}

	z_erofs_submit_and_unzip(&f, &pagepool, true, NULL);
out:
	if (f.m_iter.mpage != NULL)
		put_page(f.m_iter.mpage);

	/* clean up the remaining free pages */
	put_pages_list(&pagepool);
	return 0;
}

static int z_erofs_vle_normalaccess_readpages(struct file *filp,
					      struct address_space *mapping,
					      struct list_head *pages,
					      unsigned int nr_pages)
{
	struct inode *const inode = mapping->host;
	struct block_device *bdev = inode->i_sb->s_bdev;
	struct erofs_sb_info *const sbi = EROFS_I_SB(inode);

	bool sync = __should_decompress_synchronously(sbi, nr_pages);
	struct z_erofs_vle_frontend f = VLE_FRONTEND_INIT(inode);
	gfp_t gfp = mapping_gfp_constraint(mapping, GFP_KERNEL);
	struct page *head = NULL;
	LIST_HEAD(pagepool);
	unsigned io_submitted = 0;

	trace_erofs_readpages(mapping->host, lru_to_page(pages), nr_pages, false);

#ifdef CONFIG_BLK_DEV_THROTTLING
	if (pages) {
		/*
		 * Get one quota before read pages, when this ends,
		 * get the rest of quotas according to how many bios
		 * we submited in this routine.
		 */
		blk_throtl_get_quota(bdev, PAGE_SIZE,
				     msecs_to_jiffies(100), true);
	}
#endif

#if (EROFS_FS_ZIP_CACHE_LVL >= 2)
	f.cachedzone_la = lru_to_page(pages)->index << PAGE_SHIFT;
#endif
	for (; nr_pages; --nr_pages) {
		struct page *page = lru_to_page(pages);

		prefetchw(&page->flags);
		list_del(&page->lru);

		/*
		 * A pure asynchronous readahead is indicated if
		 * a PG_readahead marked page is hitted at first.
		 * Let's also do asynchronous decompression for this case.
		 */
		sync &= !(PageReadahead(page) && !head);	/*lint !e514*/

		if (add_to_page_cache_lru(page, mapping, page->index, gfp)) {
			list_add(&page->lru, &pagepool);
			continue;
		}

		BUG_ON(PagePrivate(page));
		set_page_private(page, (unsigned long)head);
		head = page;
	}

	while (head != NULL) {
		struct page *page = head;
		int err;

		/* traversal in reverse order */
		head = (void *)page_private(page);

		err = z_erofs_do_read_page(&f, page, &pagepool);
		if (err) {
			struct erofs_vnode *vi = EROFS_V(inode);

			errln("%s, readahead error at page %lu of nid %llu",
				__func__, page->index, vi->nid);
		}

		put_page(page);
	}

	(void)z_erofs_vle_work_iter_end(&f.builder);

	z_erofs_submit_and_unzip(&f, &pagepool, sync, &io_submitted);

	if (f.m_iter.mpage != NULL)
		put_page(f.m_iter.mpage);

	/* clean up the remaining free pages */
	put_pages_list(&pagepool);

#ifdef CONFIG_BLK_DEV_THROTTLING
	if (io_submitted)
		while (--io_submitted)
			blk_throtl_get_quota(bdev, PAGE_SIZE,
					     msecs_to_jiffies(100), true);
#endif
	return 0;
}

const struct address_space_operations z_erofs_vle_normalaccess_aops = {
	.readpage = z_erofs_vle_normalaccess_readpage,
	.readpages = z_erofs_vle_normalaccess_readpages,
};

/*
 * Variable-sized Logical Extent (Fixed Physical Cluster) Compression Mode
 * ---
 * VLE compression mode attempts to compress a number of logical data into
 * a physical cluster with a fixed size.
 * VLE compression mode uses "struct z_erofs_vle_decompressed_index".
 */
#define __vle_cluster_advise(x, bit, bits) \
	((le16_to_cpu(x) >> (bit)) & ((1 << (bits)) - 1))

#define __vle_cluster_type(advise) __vle_cluster_advise(advise, \
	Z_EROFS_VLE_DI_CLUSTER_TYPE_BIT, Z_EROFS_VLE_DI_CLUSTER_TYPE_BITS)

#define vle_cluster_type(di)	\
	__vle_cluster_type((di)->di_advise)

#ifdef CONFIG_EROFS_FS_HUAWEI_EXTENSION

#define vle_huawei_compat_previous_clusters(clustersize, di)	(\
	(le16_to_cpu((di)->di_clusterofs) / clustersize) | \
(__vle_cluster_advise((di)->di_advise, 4, 4) << 4))

#endif

static int
vle_decompressed_index_clusterofs(unsigned int *clusterofs,
				  unsigned int clustersize,
				  struct z_erofs_vle_decompressed_index *di)
{
	switch (vle_cluster_type(di)) {
	case Z_EROFS_VLE_CLUSTER_TYPE_NONHEAD:
		*clusterofs = clustersize;
		break;
#ifdef CONFIG_EROFS_FS_HUAWEI_EXTENSION
	case Z_EROFS_VLE_CLUSTER_TYPE_HUAWEI_COMPAT:
		if (vle_huawei_compat_previous_clusters(clustersize, di)) {
			*clusterofs = clustersize;
			break;
		}
		/* fallthrough */
#endif
	case Z_EROFS_VLE_CLUSTER_TYPE_PLAIN:
	case Z_EROFS_VLE_CLUSTER_TYPE_HEAD:
		*clusterofs = le16_to_cpu(di->di_clusterofs);
		break;
	default:
		DBG_BUGON(1);
		return -EIO;
	}
	return 0;
}

static inline erofs_blk_t
vle_extent_blkaddr(struct inode *inode, pgoff_t index)
{
	struct erofs_sb_info *sbi = EROFS_I_SB(inode);
	struct erofs_vnode *vi = EROFS_V(inode);

	unsigned ofs = Z_EROFS_VLE_EXTENT_ALIGN(vi->inode_isize +
		vi->xattr_isize) + sizeof(struct erofs_extent_header) +
		index * sizeof(struct z_erofs_vle_decompressed_index);

	return erofs_blknr(iloc(sbi, vi->nid) + ofs);
}

static inline unsigned int
vle_extent_blkoff(struct inode *inode, pgoff_t index)
{
	struct erofs_sb_info *sbi = EROFS_I_SB(inode);
	struct erofs_vnode *vi = EROFS_V(inode);

	unsigned ofs = Z_EROFS_VLE_EXTENT_ALIGN(vi->inode_isize +
		vi->xattr_isize) + sizeof(struct erofs_extent_header) +
		index * sizeof(struct z_erofs_vle_decompressed_index);

	return erofs_blkoff(iloc(sbi, vi->nid) + ofs);
}

struct vle_map_blocks_iter_ctx {
	struct inode *inode;
	struct super_block *sb;
	unsigned int clusterbits;

	struct page **mpage_ret;
	void **kaddr_ret;
};

static int
vle_get_logical_extent_head(const struct vle_map_blocks_iter_ctx *ctx,
			    unsigned int lcn,	/* logical cluster number */
			    unsigned long long *ofs,
			    erofs_blk_t *pblk,
			    unsigned int *flags)
{
	const unsigned int clustersize = 1 << ctx->clusterbits;
	const erofs_blk_t mblk = vle_extent_blkaddr(ctx->inode, lcn);
	struct page *mpage = *ctx->mpage_ret;	/* extent metapage */

	struct z_erofs_vle_decompressed_index *di;
	unsigned int cluster_type, delta0;

	if (mpage->index != mblk) {
		kunmap_atomic(*ctx->kaddr_ret);
		unlock_page(mpage);
		put_page(mpage);

		mpage = erofs_get_meta_page(ctx->sb, mblk, false);
		if (IS_ERR(mpage)) {
			*ctx->mpage_ret = NULL;
			return PTR_ERR(mpage);
		}
		*ctx->mpage_ret = mpage;
		*ctx->kaddr_ret = kmap_atomic(mpage);
	}

	di = *ctx->kaddr_ret + vle_extent_blkoff(ctx->inode, lcn);

	cluster_type = vle_cluster_type(di);
	switch (cluster_type) {
	case Z_EROFS_VLE_CLUSTER_TYPE_NONHEAD:
		delta0 = le16_to_cpu(di->di_u.delta[0]);
		if (unlikely(!delta0 || delta0 > lcn)) {
			errln("invalid NONHEAD dl0 %u at lcn %u of nid %llu",
			      delta0, lcn, EROFS_V(ctx->inode)->nid);
			DBG_BUGON(1);
			return -EIO;
		}
		return vle_get_logical_extent_head(ctx,
			lcn - delta0, ofs, pblk, flags);
	case Z_EROFS_VLE_CLUSTER_TYPE_PLAIN:
		*flags ^= EROFS_MAP_ZIPPED;
		/* fallthrough */
#ifdef CONFIG_EROFS_FS_HUAWEI_EXTENSION
	case Z_EROFS_VLE_CLUSTER_TYPE_HUAWEI_COMPAT:
		lcn -= vle_huawei_compat_previous_clusters(clustersize, di);
		/* fallthrough */
#endif
	case Z_EROFS_VLE_CLUSTER_TYPE_HEAD:
		/* clustersize should be a power of two */
		*ofs = ((u64)lcn << ctx->clusterbits) +
			(le16_to_cpu(di->di_clusterofs) & (clustersize - 1));
		*pblk = le32_to_cpu(di->di_u.blkaddr);
		break;
	default:
		errln("unknown cluster type %u at lcn %u of nid %llu",
		      cluster_type, lcn, EROFS_V(ctx->inode)->nid);
		DBG_BUGON(1);
		return -EIO;
	}
	return 0;
}

int z_erofs_map_blocks_iter(struct inode *inode,
	struct erofs_map_blocks *map,
	struct page **mpage_ret, int flags)
{
	void *kaddr;
	const struct vle_map_blocks_iter_ctx ctx = {
		.inode = inode,
		.sb = inode->i_sb,
		.clusterbits = EROFS_I_SB(inode)->clusterbits,
		.mpage_ret = mpage_ret,
		.kaddr_ret = &kaddr
	};
	const unsigned int clustersize = 1 << ctx.clusterbits;
	/* if both m_(l,p)len are 0, regularize l_lblk, l_lofs, etc... */
	const bool initial = !map->m_llen;

	/* logicial extent (start, end) offset */
	unsigned long long ofs, end;
	unsigned int lcn;
	u32 ofs_rem;

	erofs_blk_t mblk, pblk;
	struct page *mpage = *mpage_ret;
	struct z_erofs_vle_decompressed_index *di;
	unsigned int cluster_type, logical_cluster_ofs;
	int err = 0;

	trace_z_erofs_map_blocks_iter_enter(inode, map, flags);

	/* when trying to read beyond EOF, leave it unmapped */
	if (unlikely(map->m_la >= inode->i_size)) {
		DBG_BUGON(!initial);
		map->m_llen = map->m_la + 1 - inode->i_size;
		map->m_la = inode->i_size;
		map->m_flags = 0;
		goto out;
	}

	debugln("%s, m_la %llu m_llen %llu --- start", __func__,
		map->m_la, map->m_llen);

	ofs = map->m_la + map->m_llen;

	/* clustersize should be power of two */
	lcn = ofs >> ctx.clusterbits;
	ofs_rem = ofs & (clustersize - 1);

	mblk = vle_extent_blkaddr(inode, lcn);

	if (mpage == NULL || mpage->index != mblk) {
		if (mpage != NULL)
			put_page(mpage);

		mpage = erofs_get_meta_page(ctx.sb, mblk, false);
		if (IS_ERR(mpage)) {
			err = PTR_ERR(mpage);
			goto out;
		}
		*mpage_ret = mpage;
	} else {
		lock_page(mpage);
		DBG_BUGON(!PageUptodate(mpage));
	}

	kaddr = kmap_atomic(mpage);
	di = kaddr + vle_extent_blkoff(inode, lcn);

	debugln("%s, lcn %u mblk %u e_blkoff %u", __func__, lcn,
		mblk, vle_extent_blkoff(inode, lcn));

	err = vle_decompressed_index_clusterofs(&logical_cluster_ofs,
						clustersize, di);
	if (unlikely(err))
		goto unmap_out;

	if (!initial) {
		/* [walking mode] 'map' has been already initialized */
		map->m_llen += logical_cluster_ofs;
		goto unmap_out;
	}

	/* by default, compressed */
	map->m_flags |= EROFS_MAP_ZIPPED;

	end = ((u64)lcn + 1) * clustersize;

	cluster_type = vle_cluster_type(di);

	switch (cluster_type) {
	case Z_EROFS_VLE_CLUSTER_TYPE_PLAIN:
		if (ofs_rem >= logical_cluster_ofs)
			map->m_flags ^= EROFS_MAP_ZIPPED;
		/* fallthrough */
#ifdef CONFIG_EROFS_FS_HUAWEI_EXTENSION
	case Z_EROFS_VLE_CLUSTER_TYPE_HUAWEI_COMPAT:
		if (vle_huawei_compat_previous_clusters(clustersize, di)) {
			end = (lcn-- + 1ULL) * clustersize;
			goto nonhead;
		}
		/* fallthrough */
#endif
	case Z_EROFS_VLE_CLUSTER_TYPE_HEAD:
		if (ofs_rem == logical_cluster_ofs) {
			pblk = le32_to_cpu(di->di_u.blkaddr);
			goto exact_hitted;
		}

		if (ofs_rem > logical_cluster_ofs) {
			ofs = (u64)lcn * clustersize | logical_cluster_ofs;
			pblk = le32_to_cpu(di->di_u.blkaddr);
			break;
		}

		/* logical cluster number should be >= 1 */
		if (unlikely(!lcn)) {
			errln("invalid logical cluster 0 at nid %llu",
				EROFS_V(inode)->nid);
			err = -EIO;
			goto unmap_out;
		}
		end = ((u64)lcn-- * clustersize) | logical_cluster_ofs;
		/* fallthrough */
	case Z_EROFS_VLE_CLUSTER_TYPE_NONHEAD:
nonhead:	/* get the correspoinding first chunk */
		err = vle_get_logical_extent_head(&ctx, lcn, &ofs,
						  &pblk, &map->m_flags);
		mpage = *mpage_ret;

		if (unlikely(err)) {
			if (mpage != NULL)
				goto unmap_out;
			goto out;
		}
		break;
	default:
		errln("unknown cluster type %u at offset %llu of nid %llu",
			cluster_type, ofs, EROFS_V(inode)->nid);
		err = -EIO;
		goto unmap_out;
	}

	map->m_la = ofs;
exact_hitted:
	map->m_llen = end - ofs;
	map->m_plen = clustersize;
	map->m_pa = blknr_to_addr(pblk);
	map->m_flags |= EROFS_MAP_MAPPED;
unmap_out:
	kunmap_atomic(kaddr);
	unlock_page(mpage);
out:
	debugln("%s, m_la %llu m_pa %llu m_llen %llu m_plen %llu m_flags 0%o",
		__func__, map->m_la, map->m_pa,
		map->m_llen, map->m_plen, map->m_flags);

	trace_z_erofs_map_blocks_iter_exit(inode, map, flags, err);

	/* aggressively BUG_ON iff CONFIG_EROFS_FS_DEBUG is on */
	DBG_BUGON(err < 0 && err != -ENOMEM);
	return err;
}

