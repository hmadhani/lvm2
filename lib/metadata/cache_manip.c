/*
 * Copyright (C) 2014 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "lib.h"
#include "metadata.h"
#include "locking.h"
#include "pv_map.h"
#include "lvm-string.h"
#include "toolcontext.h"
#include "lv_alloc.h"
#include "pv_alloc.h"
#include "display.h"
#include "segtype.h"
#include "archiver.h"
#include "activate.h"
#include "str_list.h"
#include "defaults.h"
#include "lvm-exec.h"

/*
 * lv_cache_create
 * @pool
 * @origin
 *
 * Given a cache_pool and an origin, link the two and create a
 * cached LV.
 *
 * Returns: cache LV on success, NULL on failure
 */
struct logical_volume *lv_cache_create(struct logical_volume *pool,
				       struct logical_volume *origin)
{
	const struct segment_type *segtype;
	struct cmd_context *cmd = pool->vg->cmd;
	struct logical_volume *cache_lv;
	struct lv_segment *seg;

	if (!lv_is_cache_pool(pool)) {
		log_error(INTERNAL_ERROR
			  "%s is not a cache_pool LV", pool->name);
		return NULL;
	}

	if (lv_is_cache_type(origin)) {
		/*
		 * FIXME: We can layer caches, insert_layer_for_lv() would
		 * have to do a better job renaming the LVs in the stack
		 * first so that there isn't a name collision with <name>_corig.
		 * The origin under the origin would become *_corig_corig
		 * before renaming the origin above to *_corig.
		 */
		log_error(INTERNAL_ERROR
			  "The origin, %s, cannot be of cache type",
			  origin->name);
		return NULL;
	}

	if (!(segtype = get_segtype_from_string(cmd, "cache")))
		return_NULL;

	cache_lv = origin;
	if (!(origin = insert_layer_for_lv(cmd, cache_lv, CACHE, "_corig")))
		return_NULL;

	seg = first_seg(cache_lv);
	seg->segtype = segtype;

	if (!attach_pool_lv(seg, pool, NULL, NULL))
		return_0;

	return cache_lv;
}

/*
 * lv_cache_remove
 * @cache_lv
 *
 * Given a cache LV, remove the cache layer.  This will unlink
 * the origin and cache_pool, remove the cache LV layer, and promote
 * the origin to a usable non-cached LV of the same name as the
 * given cache_lv.
 *
 * Returns: 1 on success, 0 on failure
 */
int lv_cache_remove(struct logical_volume *cache_lv)
{
	struct cmd_context *cmd = cache_lv->vg->cmd;
	char *policy_name;
	uint64_t dirty_blocks;
	struct segment_type *segtype;
	struct lv_segment *cache_seg = first_seg(cache_lv);
	struct logical_volume *origin_lv;
	struct logical_volume *cache_pool_lv;

	if (!lv_is_cache(cache_lv))
		return_0;

	/*
	 * FIXME:
	 * Before the link can be broken, we must ensure that the
	 * cache has been flushed.  This may already be the case
	 * if the cache mode is writethrough (or the cleaner
	 * policy is in place from a previous half-finished attempt
	 * to remove the cache_pool).  It could take a long time to
	 * flush the cache - it should probably be done in the background.
	 *
	 * Also, if we do perform the flush in the background and we
	 * happen to also be removing the cache/origin LV, then we
	 * could check if the cleaner policy is in place and simply
	 * remove the cache_pool then without waiting for the flush to
	 * complete.
	 */
	if (!lv_cache_policy_info(cache_lv, &policy_name, NULL, NULL))
		return_0;

	if (strcmp(policy_name, "cleaner")) {
		/* We must swap in the cleaner to flush the cache */
		log_error("Flushing cache for %s", cache_lv->name);

		/*
		 * Is there are clean way to free the memory for the name
		 * and argv when changing the policy?
		 */
		cache_seg->policy_name = (char *)"cleaner";
		cache_seg->policy_argc = 0;
		cache_seg->policy_argv = NULL;

		/* update the kernel to put the cleaner policy in place */
		if (!vg_write(cache_lv->vg))
			return_0;
		if (!suspend_lv(cmd, cache_lv))
			return_0;
		if (!vg_commit(cache_lv->vg))
			return_0;
		if (!resume_lv(cmd, cache_lv))
			return_0;
	}

	//FIXME: use polling to do this...
	do {
		if (!lv_cache_block_info(cache_lv, NULL,
					 &dirty_blocks, NULL, NULL))
			return_0;
		log_error("%" PRIu64 " blocks must still be flushed.",
			  dirty_blocks);
		if (dirty_blocks)
			sleep(5);
	} while (dirty_blocks);

	cache_pool_lv = first_seg(cache_lv)->pool_lv;
	if (!detach_pool_lv(first_seg(cache_lv)))
		return_0;

	origin_lv = seg_lv(first_seg(cache_lv), 0);
	lv_set_visible(origin_lv);

//FIXME: We should be able to use 'remove_layer_from_lv', but
//       there is a call to 'lv_empty' in there that recursively
//       deletes everything down the tree - including the origin_lv
//       that we are trying to preserve!
//	if (!remove_layer_from_lv(cache_lv, origin_lv))
//		return_0;

	if (!remove_seg_from_segs_using_this_lv(origin_lv, first_seg(cache_lv)))
		return_0;
	if (!move_lv_segments(cache_lv, origin_lv, 0, 0))
		return_0;

	cache_lv->status &= ~CACHE;

	segtype = get_segtype_from_string(cmd, "error");
	if (!lv_add_virtual_segment(origin_lv, 0,
				    cache_lv->le_count, segtype, NULL))
		return_0;

	if (!vg_write(cache_lv->vg))
		return_0;

	/*
	 * suspend_lv on this cache LV will suspend all of the components:
	 * - the top-level cache LV
	 * - the origin
	 * - the cache_pool and all of its sub-LVs
	 */
	if (!suspend_lv(cmd, cache_lv))
		return_0;

	if (!vg_commit(cache_lv->vg))
		return_0;

	/*
	 * resume_lv on this (former) cache LV will resume all
	 * but the cache_pool LV.  It must be resumed seperately.
	 */
	if (!resume_lv(cmd, cache_lv))
		return_0;
	if (!resume_lv(cmd, cache_pool_lv))
		return_0;

	if (!activate_lv(cmd, origin_lv))
		return_0;
	if (!deactivate_lv(cmd, origin_lv))
		return_0;
	if (!lv_remove(origin_lv))
		return_0;

	return 1;
}
