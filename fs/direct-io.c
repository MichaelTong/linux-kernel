/*
 * fs/direct-io.c
 *
 * Copyright (C) 2002, Linus Torvalds.
 *
 * O_DIRECT
 *
 * 04Jul2002	Andrew Morton
 *		Initial version
 * 11Sep2002	janetinc@us.ibm.com
 * 		added readv/writev support.
 * 29Oct2002	Andrew Morton
 *		rewrote bio_add_page() support.
 * 30Oct2002	pbadari@us.ibm.com
 *		added support for non-aligned IO.
 * 06Nov2002	pbadari@us.ibm.com
 *		added asynchronous IO support.
 * 21Jul2003	nathans@sgi.com
 *		added IO completion notifier.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/highmem.h>
#include <linux/pagemap.h>
#include <linux/task_io_accounting_ops.h>
#include <linux/bio.h>
#include <linux/wait.h>
#include <linux/err.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/rwsem.h>
#include <linux/uio.h>
#include <linux/atomic.h>
#include <linux/prefetch.h>
#include <linux/aio.h>
#include <linux/kallsyms.h>
#include <linux/async_tx.h>
#include <linux/cpu.h>
#include <../drivers/md/raid5.h>
#include <../drivers/md/md.h>
#include <linux/flex_array.h>


/*
 * How many user pages to map in one call to get_user_pages().  This determines
 * the size of a structure in the slab cache
 */
#define DIO_PAGES	64


extern int ignoreR;
extern int reconRead;

struct compute_percpu{
    struct page *spare_page;
    struct flex_array *scribble;
};
/*
 * This code generally works in units of "dio_blocks".  A dio_block is
 * somewhere between the hard sector size and the filesystem block size.  it
 * is determined on a per-invocation basis.   When talking to the filesystem
 * we need to convert dio_blocks to fs_blocks by scaling the dio_block quantity
 * down by dio->blkfactor.  Similarly, fs-blocksize quantities are converted
 * to bio_block quantities by shifting left by blkfactor.
 *
 * If blkfactor is zero then the user's request was aligned to the filesystem's
 * blocksize.
 */

/* dio_state only used in the submission path */

struct dio_submit {
	struct bio *bio;		/* bio under assembly */
	unsigned blkbits;		/* doesn't change */
	unsigned blkfactor;		/* When we're using an alignment which
					   is finer than the filesystem's soft
					   blocksize, this specifies how much
					   finer.  blkfactor=2 means 1/4-block
					   alignment.  Does not change */
	unsigned start_zero_done;	/* flag: sub-blocksize zeroing has
					   been performed at the start of a
					   write */
	int pages_in_io;		/* approximate total IO pages */
	sector_t block_in_file;		/* Current offset into the underlying
					   file in dio_block units. */
	unsigned blocks_available;	/* At block_in_file.  changes */
	int reap_counter;		/* rate limit reaping */
	sector_t final_block_in_request;/* doesn't change */
	int boundary;			/* prev block is at a boundary */
	get_block_t *get_block;		/* block mapping function */
	dio_submit_t *submit_io;	/* IO submition function */

	loff_t logical_offset_in_bio;	/* current first logical block in bio */
	sector_t final_block_in_bio;	/* current final block in bio + 1 */
	sector_t next_block_for_io;	/* next block to be put under IO,
					   in dio_blocks units */

	/*
	 * Deferred addition of a page to the dio.  These variables are
	 * private to dio_send_cur_page(), submit_page_section() and
	 * dio_bio_add_page().
	 */
	struct page *cur_page;		/* The page */
	unsigned cur_page_offset;	/* Offset into it, in bytes */
	unsigned cur_page_len;		/* Nr of bytes at cur_page_offset */
	sector_t cur_page_block;	/* Where it starts */
	loff_t cur_page_fs_offset;	/* Offset in file */

	struct iov_iter *iter;
	/*
	 * Page queue.  These variables belong to dio_refill_pages() and
	 * dio_get_page().
	 */
	unsigned head;			/* next page to process */
	unsigned tail;			/* last valid page + 1 */
	size_t from, to;
};
struct bio_record{
    struct bio *bio;
    struct bio_record *next;
};
/* dio_state communicated between submission path and end_io */
struct dio {
	int flags;			/* doesn't change */
	int rw;
	struct inode *inode;
	loff_t i_size;			/* i_size when submitted */
	dio_iodone_t *end_io;		/* IO completion function */

	void *private;			/* copy from map_bh.b_private */

	/* BIO completion state */
	spinlock_t bio_lock;		/* protects BIO fields below */
	int page_errors;		/* errno from get_user_pages() */
	int is_async;			/* is IO async ? */
	bool defer_completion;		/* defer AIO completion to workqueue? */
	int io_error;			/* IO error in completion path */
	unsigned long refcount;		/* direct_io_worker() and bios */
	struct bio *bio_list;		/* singly linked via bi_private */
	struct bio_record *bio_record;
	struct bio_record *bio_record_tail;
	struct task_struct *waiter;	/* waiting task (NULL if none) */

	/* AIO related stuff */
	struct kiocb *iocb;		/* kiocb */
	ssize_t result;                 /* IO result */

	/*
	 * pages[] (and any fields placed after it) are not zeroed out at
	 * allocation time.  Don't add new fields after pages[] unless you
	 * wish that they not be zeroed.
	 */
	union {
		struct page *pages[DIO_PAGES];	/* page buffer */
		struct work_struct complete_work;/* deferred AIO completion */
	};
} ____cacheline_aligned_in_smp;

static struct kmem_cache *dio_cache __read_mostly;

/*
 * How many pages are in the queue?
 */
static inline unsigned dio_pages_present(struct dio_submit *sdio)
{
	return sdio->tail - sdio->head;
}

/*
 * Go grab and pin some userspace pages.   Typically we'll get 64 at a time.
 */
static inline int dio_refill_pages(struct dio *dio, struct dio_submit *sdio)
{
	ssize_t ret;

	ret = iov_iter_get_pages(sdio->iter, dio->pages, LONG_MAX, DIO_PAGES,
				&sdio->from);

	if (ret < 0 && sdio->blocks_available && (dio->rw & WRITE)) {
		struct page *page = ZERO_PAGE(0);
		/*
		 * A memory fault, but the filesystem has some outstanding
		 * mapped blocks.  We need to use those blocks up to avoid
		 * leaking stale data in the file.
		 */
		if (dio->page_errors == 0)
			dio->page_errors = ret;
		page_cache_get(page);
		dio->pages[0] = page;
		sdio->head = 0;
		sdio->tail = 1;
		sdio->from = 0;
		sdio->to = PAGE_SIZE;
		return 0;
	}

	if (ret >= 0) {
		iov_iter_advance(sdio->iter, ret);
		ret += sdio->from;
		sdio->head = 0;
		sdio->tail = (ret + PAGE_SIZE - 1) / PAGE_SIZE;
		sdio->to = ((ret - 1) & (PAGE_SIZE - 1)) + 1;
		return 0;
	}
	return ret;
}

/*
 * Get another userspace page.  Returns an ERR_PTR on error.  Pages are
 * buffered inside the dio so that we can call get_user_pages() against a
 * decent number of pages, less frequently.  To provide nicer use of the
 * L1 cache.
 */
static inline struct page *dio_get_page(struct dio *dio,
					struct dio_submit *sdio)
{
    //printk("MikeT: %s %s %d\n", __FILE__, __func__, __LINE__);
	if (dio_pages_present(sdio) == 0) {
		int ret;
		//printk("MikeT: %s %s %d\n", __FILE__, __func__, __LINE__);
		ret = dio_refill_pages(dio, sdio);
		//printk("MikeT: %s %s %d, page0 %p, page1 %p, page2 %p, page3 %p, page4 %p\n", __FILE__, __func__, __LINE__, dio->pages[0], dio->pages[1], dio->pages[2], dio->pages[3],dio->pages[4]);
		if (ret)
			return ERR_PTR(ret);
		BUG_ON(dio_pages_present(sdio) == 0);
	}
	return dio->pages[sdio->head];
}

/**
 * dio_complete() - called when all DIO BIO I/O has been completed
 * @offset: the byte offset in the file of the completed operation
 *
 * This drops i_dio_count, lets interested parties know that a DIO operation
 * has completed, and calculates the resulting return code for the operation.
 *
 * It lets the filesystem know if it registered an interest earlier via
 * get_block.  Pass the private field of the map buffer_head so that
 * filesystems can use it to hold additional state between get_block calls and
 * dio_complete.
 */
static ssize_t dio_complete(struct dio *dio, loff_t offset, ssize_t ret,
		bool is_async)
{
	ssize_t transferred = 0;
	struct bio_record *head, *next;

	/*
	 * AIO submission can race with bio completion to get here while
	 * expecting to have the last io completed by bio completion.
	 * In that case -EIOCBQUEUED is in fact not an error we want
	 * to preserve through this call.
	 */
	if (ret == -EIOCBQUEUED)
		ret = 0;

	if (dio->result) {
		transferred = dio->result;

		/* Check for short read case */
		if ((dio->rw == READ) && ((offset + transferred) > dio->i_size))
			transferred = dio->i_size - offset;
	}

	if (ret == 0)
		ret = dio->page_errors;
	if (ret == 0)
		ret = dio->io_error;
	if (ret == 0)
		ret = transferred;

	if (dio->end_io && dio->result)
	{
        printk("MikeT: %s %s %d, has end_io\n", __FILE__, __func__, __LINE__);
		dio->end_io(dio->iocb, offset, transferred, dio->private);
    }

	inode_dio_done(dio->inode);
	if (is_async) {
		if (dio->rw & WRITE) {
			int err;

			err = generic_write_sync(dio->iocb->ki_filp, offset,
						 transferred);
			if (err < 0 && ret > 0)
				ret = err;
		}

		aio_complete(dio->iocb, ret, 0);
	}

    head = dio->bio_record;
	while(head!=NULL)
	{
        next = head->next;
        kfree(head);
        head = next;
	}

	//printk("MikeT: %s %s %d, page: %08x, %08x, %08x\n", __FILE__, __func__, __LINE__, dio->pages[0], dio->pages[1], dio->pages[2]);

	kmem_cache_free(dio_cache, dio);
	return ret;
}

static void dio_aio_complete_work(struct work_struct *work)
{
	struct dio *dio = container_of(work, struct dio, complete_work);

	dio_complete(dio, dio->iocb->ki_pos, 0, true);
}

static int dio_bio_complete(struct dio *dio, struct bio *bio);

/*
 * Asynchronous IO callback.
 */
static void dio_bio_end_aio(struct bio *bio, int error)
{
	struct dio *dio = bio->bi_private;
	unsigned long remaining;
	unsigned long flags;

	/* cleanup the bio */
	dio_bio_complete(dio, bio);

	spin_lock_irqsave(&dio->bio_lock, flags);
	remaining = --dio->refcount;
	if (remaining == 1 && dio->waiter)
		wake_up_process(dio->waiter);
	spin_unlock_irqrestore(&dio->bio_lock, flags);

	if (remaining == 0) {
		if (dio->result && dio->defer_completion) {
			INIT_WORK(&dio->complete_work, dio_aio_complete_work);
			queue_work(dio->inode->i_sb->s_dio_done_wq,
				   &dio->complete_work);
		} else {
			dio_complete(dio, dio->iocb->ki_pos, 0, true);
		}
	}
}

/*
 * The BIO completion handler simply queues the BIO up for the process-context
 * handler.
 *
 * During I/O bi_private points at the dio.  After I/O, bi_private is used to
 * implement a singly-linked list of completed BIOs, at dio->bio_list.
 */
static void dio_bio_end_io(struct bio *bio, int error)
{
	struct dio *dio = bio->bi_private;
	unsigned long flags;

	spin_lock_irqsave(&dio->bio_lock, flags);
	bio->bi_private = dio->bio_list;
	dio->bio_list = bio;
	if (--dio->refcount == 1 + ignoreR && dio->waiter)
		wake_up_process(dio->waiter);
	spin_unlock_irqrestore(&dio->bio_lock, flags);

    bio->dio_comp=true;
}

static void dio_bio_end_pio(struct bio *bio, int error)
{
    struct dio *dio = bio->bi_private;
    unsigned long flags;

    printk("MikeT: %s %s %d, parity bio end.%s\n", __FILE__, __func__, __LINE__, (char *)bio_data(bio));
	spin_lock_irqsave(&dio->bio_lock, flags);
	bio->bi_private = dio->bio_list;
	dio->bio_list = bio;
	if (--dio->refcount == 1 + ignoreR && dio->waiter)
		wake_up_process(dio->waiter);
	spin_unlock_irqrestore(&dio->bio_lock, flags);
}

/**
 * dio_end_io - handle the end io action for the given bio
 * @bio: The direct io bio thats being completed
 * @error: Error if there was one
 *
 * This is meant to be called by any filesystem that uses their own dio_submit_t
 * so that the DIO specific endio actions are dealt with after the filesystem
 * has done it's completion work.
 */
void dio_end_io(struct bio *bio, int error)
{
	struct dio *dio = bio->bi_private;
    //printk("MikeT: %s %s %d\n",__FILE__,__func__,__LINE__);

	if (dio->is_async)
		dio_bio_end_aio(bio, error);
	else
		dio_bio_end_io(bio, error);
}
EXPORT_SYMBOL_GPL(dio_end_io);

static inline void
dio_bio_alloc(struct dio *dio, struct dio_submit *sdio,
	      struct block_device *bdev,
	      sector_t first_sector, int nr_vecs)
{
	struct bio *bio;

	/*
	 * bio_alloc() is guaranteed to return a bio when called with
	 * __GFP_WAIT and we request a valid number of vectors.
	 */
	bio = bio_alloc(GFP_KERNEL, nr_vecs);

	bio->bi_bdev = bdev;
	bio->bi_iter.bi_sector = first_sector;
	if (dio->is_async)
		bio->bi_end_io = dio_bio_end_aio;
	else
		bio->bi_end_io = dio_bio_end_io;

	sdio->bio = bio;
	sdio->logical_offset_in_bio = sdio->cur_page_fs_offset;
}

/*
 * In the AIO read case we speculatively dirty the pages before starting IO.
 * During IO completion, any of these pages which happen to have been written
 * back will be redirtied by bio_check_pages_dirty().
 *
 * bios hold a dio reference between submit_bio and ->end_io.
 */
static inline void dio_bio_submit(struct dio *dio, struct dio_submit *sdio)
{
	struct bio *bio = sdio->bio;
	unsigned long flags;

	bio->bi_private = dio;

	spin_lock_irqsave(&dio->bio_lock, flags);
	dio->refcount++;
	spin_unlock_irqrestore(&dio->bio_lock, flags);

	if (dio->is_async && dio->rw == READ)
		bio_set_pages_dirty(bio);
/*
    if(reconRead)
    {
        printk("MikeT: %s %s %d, "
               "bio %p, bi_next %p, bi_bdev %p, bi_flags %lx, bi_rw %lx; "
               "bi_iter: bi_sector %u, bi_size %u, bi_idx %u, bi_bvec_done %u; "
               "bi_phys_segments %u, bi_seg_front_size %u, bi_seg_back_size %u, bi_remaining %d; "
               "bi_vcnt %d, bi_max_vecs %d, bi_cnt %d, bi_io_vec %p, bi_pool %p\n ",
               __FILE__, __func__, __LINE__,
               bio, bio->bi_next, bio->bi_bdev, bio->bi_flags, bio->bi_rw,
               bio->bi_iter.bi_sector, bio->bi_iter.bi_size, bio->bi_iter.bi_idx, bio->bi_iter.bi_bvec_done,
               bio->bi_phys_segments, bio->bi_seg_front_size, bio->bi_seg_back_size, atomic_read(&bio->bi_remaining),
               bio->bi_vcnt, bio->bi_max_vecs, atomic_read(&bio->bi_cnt), bio->bi_io_vec, bio->bi_pool );

    }*/

	if (sdio->submit_io)
		sdio->submit_io(dio->rw, bio, dio->inode,
			       sdio->logical_offset_in_bio);
	else
	{
		if(dio->bio_record==NULL)
		{
            dio->bio_record = kmalloc(sizeof(struct bio_record), GFP_KERNEL);
            dio->bio_record->bio = bio;
            dio->bio_record->next = NULL;
            dio->bio_record_tail = dio->bio_record;
		}
		else
		{
            dio->bio_record_tail->next = kmalloc(sizeof(struct bio_record), GFP_KERNEL);
            dio->bio_record_tail->next->bio = bio;
            dio->bio_record_tail->next->next = NULL;
            dio->bio_record_tail = dio->bio_record_tail->next;
		}
		submit_bio(dio->rw, bio);
	}

	sdio->bio = NULL;
	sdio->boundary = 0;
	sdio->logical_offset_in_bio = 0;
}

/*
 * Release any resources in case of a failure
 */
static inline void dio_cleanup(struct dio *dio, struct dio_submit *sdio)
{
	while (sdio->head < sdio->tail)
		page_cache_release(dio->pages[sdio->head++]);
}

/*
 * Wait for the next BIO to complete.  Remove it and return it.  NULL is
 * returned once all BIOs have been completed.  This must only be called once
 * all bios have been issued so that dio->refcount can only decrease.  This
 * requires that that the caller hold a reference on the dio.
 */
static struct bio *dio_await_one(struct dio *dio)
{
	unsigned long flags;
	struct bio *bio = NULL;

	spin_lock_irqsave(&dio->bio_lock, flags);

	/*
	 * Wait as long as the list is empty and there are bios in flight.  bio
	 * completion drops the count, maybe adds to the list, and wakes while
	 * holding the bio_lock so we don't need set_current_state()'s barrier
	 * and can call it after testing our condition.
	 */
	while (dio->refcount > 1 && dio->bio_list == NULL) {
		__set_current_state(TASK_UNINTERRUPTIBLE);
		dio->waiter = current;
		spin_unlock_irqrestore(&dio->bio_lock, flags);
		io_schedule();
		/* wake up sets us TASK_RUNNING */
		spin_lock_irqsave(&dio->bio_lock, flags);
		dio->waiter = NULL;
	}
	if (dio->bio_list) {
		bio = dio->bio_list;
		dio->bio_list = bio->bi_private;
	}
	spin_unlock_irqrestore(&dio->bio_lock, flags);
	return bio;
}

/*
 * Process one completed BIO.  No locks are held.
 */
static int dio_bio_complete(struct dio *dio, struct bio *bio)
{
	const int uptodate = test_bit(BIO_UPTODATE, &bio->bi_flags);
	struct bio_vec *bvec;
	unsigned i;

	if (!uptodate)
		dio->io_error = -EIO;

	if (dio->is_async && dio->rw == READ) {
		bio_check_pages_dirty(bio);	/* transfers ownership */
	} else {
		bio_for_each_segment_all(bvec, bio, i) {
			struct page *page = bvec->bv_page;

			if (dio->rw == READ && !PageCompound(page))
				set_page_dirty_lock(page);
			page_cache_release(page);
            //printk("MikeT: %s %s %d, in for each,  i: %d\n", __FILE__, __func__, __LINE__, i);
		}
		//printk("MikeT: %s %s %d, bi_cnt before bio_put: %d\n", __FILE__, __func__, __LINE__, atomic_read(&bio->bi_cnt));
		bio_put(bio);
	}
	return uptodate ? 0 : -EIO;
}

/*
 * Wait on and process all in-flight BIOs.  This must only be called once
 * all bios have been issued so that the refcount can only decrease.
 * This just waits for all bios to make it through dio_bio_complete.  IO
 * errors are propagated through dio->io_error and should be propagated via
 * dio_complete().
 */
static void dio_await_completion(struct dio *dio)
{
	struct bio *bio;
	//int num_bio=0;
	do {
		bio = dio_await_one(dio);
		if (bio)
		{
            //num_bio++;
            //printk("MikeT: %s %s %d, num_bio: %d\n", __FILE__, __func__, __LINE__, num_bio);
			dio_bio_complete(dio, bio);
        }
	} while (bio);
}

/*
 * Process one completed BIO.  No locks are held.
 */
static int dio_bio_complete_MikeT(struct dio *dio, struct bio *bio)
{
	const int uptodate = test_bit(BIO_UPTODATE, &bio->bi_flags);
	struct bio_vec *bvec;
	unsigned i;

	if (!uptodate)
		dio->io_error = -EIO;

	if (dio->is_async && dio->rw == READ) {
		bio_check_pages_dirty(bio);	/* transfers ownership */
	} else {
		bio_for_each_segment_all(bvec, bio, i) {
			struct page *page = bvec->bv_page;

			if (dio->rw == READ && !PageCompound(page))
				set_page_dirty_lock(page);
			page_cache_release(page);
            //printk("MikeT: %s %s %d, Before put bio, i: %d\n", __FILE__, __func__, __LINE__, i);
		}
		//bio_put(bio);
	}
	return uptodate ? 0 : -EIO;
}
static int dio_alloc_percpu(struct compute_percpu *percpu, unsigned long cpu)
{
    int err=0;
    struct compute_percpu *pc;
    percpu = alloc_percpu(struct compute_percpu);
    printk("MikeT: %s %s %d\n", __FILE__, __func__, __LINE__);
    if(!percpu)
        return -ENOMEM;
    printk("MikeT: %s %s %d\n", __FILE__, __func__, __LINE__);
    pc = per_cpu_ptr(percpu, cpu);
    printk("MikeT: %s %s %d, %p\n", __FILE__, __func__, __LINE__, pc);
    if(!pc->scribble)
        pc->scribble = kmalloc(sizeof(struct page *) * (4+2) + sizeof(addr_conv_t)*(4+2), GFP_KERNEL);
    if(!pc->scribble)
    {
        return -ENOMEM;
    }
    if(err)
    {
        pr_err("%s, failed memory allocation for cpu %ld\n", __func__, cpu);
    }
    printk("MikeT: %s %s %d, %p\n", __FILE__, __func__, __LINE__,percpu);
    return err;
}

static void dio_free_percpu(struct compute_percpu *percpu, unsigned long cpu)
{
    struct compute_percpu *pc;
    pc = per_cpu_ptr(percpu, cpu);
    if(!pc)
        return;
    safe_put_page(pc->spare_page);
    if(pc->scribble)
        flex_array_free(pc->scribble);
    pc->spare_page=NULL;
    pc->scribble=NULL;
    free_percpu(percpu);
}

static int dio_alloc_percpu_all(struct compute_percpu *percpu)
{
    unsigned long cpu;
    int err=0;

    percpu = alloc_percpu(struct compute_percpu);
    if(!percpu)
        return -ENOMEM;

    get_online_cpus();
    for_each_present_cpu(cpu){
        struct compute_percpu *per;
        per = per_cpu_ptr(percpu, cpu);
        if(!per->scribble)
            per->scribble = kmalloc(sizeof(struct page *) * (4+2) + sizeof(addr_conv_t)*(4+2), GFP_KERNEL);
        if(!per->scribble)
        {
            return -ENOMEM;
        }
        if(err)
        {
            pr_err("%s, failed memory allocation for cpu %ld\n", __func__, cpu);
        }
        printk("MikeT: %s %s %d, cpu %lu, scribble: %p\n ", __FILE__, __func__, __LINE__, cpu, per->scribble);
    }
    put_online_cpus();

    return err;
}

static void dio_free_percpu_all(struct compute_percpu *percpu)
{
    unsigned long cpu;
    if(!percpu)
        return;
    get_online_cpus();
    for_each_possible_cpu(cpu)
    {
        struct compute_percpu *per;
        per = per_cpu_ptr(percpu, cpu);
        safe_put_page(per->spare_page);
        if(per->scribble)
            flex_array_free(per->scribble);
        per->spare_page=NULL;
        per->scribble=NULL;

    }
    put_online_cpus();
    free_percpu(percpu);
}
static void do_sync_xor(struct page *dest, struct page **src_list, unsigned int offset,
                        int src_cnt, size_t len, struct async_submit_ctl *submit)
{
    int i;
    int xor_src_cnt = 0;
    int src_off=0;
    void *dest_buf;
    void **srcs;

    if(submit->scribble)
        srcs=submit->scribble;
    else
        srcs=(void**)src_list;

    for(i=0; i<src_cnt; i++)
        if(src_list[i])
            srcs[xor_src_cnt++] = page_address(src_list[i])+offset;
    src_cnt = xor_src_cnt;
    dest_buf = page_address(dest) + offset;

    memset(dest_buf, 0, len);

    while(src_cnt>0)
    {
        xor_src_cnt = min(src_cnt, MAX_XOR_BLOCKS);
        xor_blocks(xor_src_cnt, len, dest_buf, &srcs[src_off]);

        src_cnt -= xor_src_cnt;
        src_off += xor_src_cnt;
    }
}
static void dio_bio_do_compute5(struct dio *dio, struct page *parity, int target)
{
    struct page **xor_srcs;
    struct page *xor_dest;
    struct async_submit_ctl submit;
    int i,count=0;
    char *tmp=kmalloc(4096, GFP_KERNEL);

    struct compute_percpu __percpu *percpu=NULL, *pc=NULL;
	int ret;
	unsigned long cpu;

    printk("MikeT: %s %s %d\n", __FILE__, __func__, __LINE__);

    if((ret = dio_alloc_percpu_all(percpu))!=0)
    {
        printk("MikeT: %s %s %d, can't alloc percpu: %d\n", __FILE__, __func__, __LINE__, ret);
    }
    cpu = get_cpu();
    pc = per_cpu_ptr(percpu, cpu);

    printk("MikeT: %s %s %d, cpu %lu, pc %p\n", __FILE__, __func__, __LINE__, cpu, pc);
    printk("MikeT: %s %s %d, dio %p\n", __FILE__, __func__, __LINE__, dio);
    xor_srcs = kmalloc(sizeof(struct page *) * (4+2) + sizeof(addr_conv_t)*(4+2), GFP_KERNEL);//pc->scribble;
    printk("MikeT: %s %s %d, srcs: %p\n", __FILE__, __func__, __LINE__, xor_srcs);

    xor_dest = virt_to_page(tmp);
    xor_srcs[count++]=parity;
    for(i=0;i<3;i++)
    {
        if(i!=target)
            xor_srcs[count++]=dio->pages[i];
    }
    printk("MikeT: %s %s %d\n", __FILE__, __func__, __LINE__);
    submit.scribble = (struct addr_conv_t*)(xor_srcs + sizeof(struct page *) *(4+2));
    do_sync_xor(xor_dest, xor_srcs, 0, count, PAGE_SIZE, &submit);

    put_cpu();
    dio_free_percpu_all(percpu);

    printk("MikeT: %s %s %d\n", __FILE__, __func__, __LINE__);
    printk("MikeT: %s %s %d, data: %s\n", __FILE__, __func__, __LINE__, (char *)page_address(xor_dest));
    memcpy(page_address(dio->pages[target]), tmp, PAGE_SIZE);

    //clean up
    //page_cache_release(xor_dest);
    //free_page(xor_dest);
    if (!PageCompound(dio->pages[target]))
        set_page_dirty_lock(dio->pages[target]);
    page_cache_release(dio->pages[target]);

    xor_dest = NULL;
    kfree(tmp);
    kfree(xor_srcs);
    xor_srcs = NULL;

}



static int dio_bio_complete_parity_MikeT(struct dio *dio, struct bio *bio)
{
	const int uptodate = test_bit(BIO_UPTODATE, &bio->bi_flags);

	if (!uptodate)
		dio->io_error = -EIO;

	if (dio->is_async && dio->rw == READ) {
		bio_check_pages_dirty(bio);	/* transfers ownership */
	} else {
	    /*
		bio_for_each_segment_all(bvec, bio, i) {
			struct page *page = bvec->bv_page;

			if (dio->rw == READ && !PageCompound(page))
				set_page_dirty_lock(page);
			page_cache_release(page);
            printk("MikeT: %s %s %d, Before put bio, i: %d\n", __FILE__, __func__, __LINE__, i);
		}
		bio_put(bio);*/
		/***reconstruct***/
		printk("MikeT: %s %s %d, will reconstruct\n", __FILE__, __func__, __LINE__);

		dio_bio_do_compute5(dio, bio_page(bio), 1);
		//page_cache_release(bio_page(bio));
		kfree(bio_data(bio));
        //free_page(bio_page(bio));
        bio_put(bio);

	}
	return uptodate ? 0 : -EIO;
}

static void dio_await_completion_MikeT(struct dio *dio)
{
	struct bio *bio;
	struct bio_record *head,*next;

	do {
        if(dio->refcount == 1 + ignoreR)
            break;
		bio = dio_await_one(dio);
		if (bio)
			dio_bio_complete_MikeT(dio, bio);
	} while (bio);

    head = dio->bio_record;
	while(head!=NULL)
    {
        next = head->next;
        head->bio->dio_comp = !(head->bio->dio_comp);
        head = next;
    }
}
static void dio_await_completion_parity_MikeT(struct dio *dio)
{
    struct bio *bio;
    printk("MikeT: %s %s %d\n", __FILE__, __func__, __LINE__);
    do{
        bio = dio_await_one(dio);
        if(bio)
        {
            printk("MikeT: %s %s %d, bio %p\n", __FILE__, __func__, __LINE__, bio);
            if(bio->need_parity)
            {
                dio_bio_complete_parity_MikeT(dio, bio);
                break;
            }
            else
                dio_bio_complete(dio, bio);
        }
    }while(bio);

}
static inline int drop_refcount(struct dio *dio)
{
	int ret2;
	unsigned long flags;

	/*
	 * Sync will always be dropping the final ref and completing the
	 * operation.  AIO can if it was a broken operation described above or
	 * in fact if all the bios race to complete before we get here.  In
	 * that case dio_complete() translates the EIOCBQUEUED into the proper
	 * return code that the caller will hand to aio_complete().
	 *
	 * This is managed by the bio_lock instead of being an atomic_t so that
	 * completion paths can drop their ref and use the remaining count to
	 * decide to wake the submission path atomically.
	 */
	spin_lock_irqsave(&dio->bio_lock, flags);
	ret2 = --dio->refcount;
	spin_unlock_irqrestore(&dio->bio_lock, flags);
	return ret2;
}

/**
 * dio_complete() - called when all DIO BIO I/O has been completed
 * @offset: the byte offset in the file of the completed operation
 *
 * This drops i_dio_count, lets interested parties know that a DIO operation
 * has completed, and calculates the resulting return code for the operation.
 *
 * It lets the filesystem know if it registered an interest earlier via
 * get_block.  Pass the private field of the map buffer_head so that
 * filesystems can use it to hold additional state between get_block calls and
 * dio_complete.
 */
static ssize_t dio_complete_MikeT(struct dio *dio, loff_t offset, ssize_t ret,
		bool is_async)
{
	ssize_t transferred = 0;
	struct bio_record *head, *next;
	if(dio->refcount!=0)
        drop_refcount(dio);
	/*
	 * AIO submission can race with bio completion to get here while
	 * expecting to have the last io completed by bio completion.
	 * In that case -EIOCBQUEUED is in fact not an error we want
	 * to preserve through this call.
	 */
	if (ret == -EIOCBQUEUED)
		ret = 0;

	if (dio->result) {
		transferred = dio->result;

		/* Check for short read case */
		if ((dio->rw == READ) && ((offset + transferred) > dio->i_size))
			transferred = dio->i_size - offset;
	}

	if (ret == 0)
		ret = dio->page_errors;
	if (ret == 0)
		ret = dio->io_error;
	if (ret == 0)
		ret = transferred;

	if (dio->end_io && dio->result)
		dio->end_io(dio->iocb, offset, transferred, dio->private);

	inode_dio_done(dio->inode);
	if (is_async) {
		if (dio->rw & WRITE) {
			int err;

			err = generic_write_sync(dio->iocb->ki_filp, offset,
						 transferred);
			if (err < 0 && ret > 0)
				ret = err;
		}

		aio_complete(dio->iocb, ret, 0);
	}
    head = dio->bio_record;
	while(head!=NULL)
	{
        next = head->next;
        if(!head->bio->dio_comp)
            kfree(head);
        head = next;
	}

	kmem_cache_free(dio_cache, dio);

	return ret;
}

/*
 * A really large O_DIRECT read or write can generate a lot of BIOs.  So
 * to keep the memory consumption sane we periodically reap any completed BIOs
 * during the BIO generation phase.
 *
 * This also helps to limit the peak amount of pinned userspace memory.
 */
static inline int dio_bio_reap(struct dio *dio, struct dio_submit *sdio)
{
	int ret = 0;

	if (sdio->reap_counter++ >= 64) {
		while (dio->bio_list) {
			unsigned long flags;
			struct bio *bio;
			int ret2;

			spin_lock_irqsave(&dio->bio_lock, flags);
			bio = dio->bio_list;
			dio->bio_list = bio->bi_private;
			spin_unlock_irqrestore(&dio->bio_lock, flags);
			ret2 = dio_bio_complete(dio, bio);
			if (ret == 0)
				ret = ret2;
		}
		sdio->reap_counter = 0;
	}
	return ret;
}

/*
 * Create workqueue for deferred direct IO completions. We allocate the
 * workqueue when it's first needed. This avoids creating workqueue for
 * filesystems that don't need it and also allows us to create the workqueue
 * late enough so the we can include s_id in the name of the workqueue.
 */
static int sb_init_dio_done_wq(struct super_block *sb)
{
	struct workqueue_struct *old;
	struct workqueue_struct *wq = alloc_workqueue("dio/%s",
						      WQ_MEM_RECLAIM, 0,
						      sb->s_id);
	if (!wq)
		return -ENOMEM;
	/*
	 * This has to be atomic as more DIOs can race to create the workqueue
	 */
	old = cmpxchg(&sb->s_dio_done_wq, NULL, wq);
	/* Someone created workqueue before us? Free ours... */
	if (old)
		destroy_workqueue(wq);
	return 0;
}

static int dio_set_defer_completion(struct dio *dio)
{
	struct super_block *sb = dio->inode->i_sb;

	if (dio->defer_completion)
		return 0;
	dio->defer_completion = true;
	if (!sb->s_dio_done_wq)
		return sb_init_dio_done_wq(sb);
	return 0;
}

/*
 * Call into the fs to map some more disk blocks.  We record the current number
 * of available blocks at sdio->blocks_available.  These are in units of the
 * fs blocksize, (1 << inode->i_blkbits).
 *
 * The fs is allowed to map lots of blocks at once.  If it wants to do that,
 * it uses the passed inode-relative block number as the file offset, as usual.
 *
 * get_block() is passed the number of i_blkbits-sized blocks which direct_io
 * has remaining to do.  The fs should not map more than this number of blocks.
 *
 * If the fs has mapped a lot of blocks, it should populate bh->b_size to
 * indicate how much contiguous disk space has been made available at
 * bh->b_blocknr.
 *
 * If *any* of the mapped blocks are new, then the fs must set buffer_new().
 * This isn't very efficient...
 *
 * In the case of filesystem holes: the fs may return an arbitrarily-large
 * hole by returning an appropriate value in b_size and by clearing
 * buffer_mapped().  However the direct-io code will only process holes one
 * block at a time - it will repeatedly call get_block() as it walks the hole.
 */
static int get_more_blocks(struct dio *dio, struct dio_submit *sdio,
			   struct buffer_head *map_bh)
{
	int ret;
	sector_t fs_startblk;	/* Into file, in filesystem-sized blocks */
	sector_t fs_endblk;	/* Into file, in filesystem-sized blocks */
	unsigned long fs_count;	/* Number of filesystem-sized blocks */
	int create;
	unsigned int i_blkbits = sdio->blkbits + sdio->blkfactor;

	/*
	 * If there was a memory error and we've overwritten all the
	 * mapped blocks then we can now return that memory error
	 */
	ret = dio->page_errors;
	if (ret == 0) {
        printk("MikeT: %s %s %d\n", __FILE__, __func__, __LINE__);
		BUG_ON(sdio->block_in_file >= sdio->final_block_in_request);
		fs_startblk = sdio->block_in_file >> sdio->blkfactor;
		fs_endblk = (sdio->final_block_in_request - 1) >>
					sdio->blkfactor;
		fs_count = fs_endblk - fs_startblk + 1;

		map_bh->b_state = 0;
		map_bh->b_size = fs_count << i_blkbits;

		/*
		 * For writes inside i_size on a DIO_SKIP_HOLES filesystem we
		 * forbid block creations: only overwrites are permitted.
		 * We will return early to the caller once we see an
		 * unmapped buffer head returned, and the caller will fall
		 * back to buffered I/O.
		 *
		 * Otherwise the decision is left to the get_blocks method,
		 * which may decide to handle it or also return an unmapped
		 * buffer head.
		 */
		create = dio->rw & WRITE;
		if (dio->flags & DIO_SKIP_HOLES) {
			if (sdio->block_in_file < (i_size_read(dio->inode) >>
							sdio->blkbits))
				create = 0;
		}

		ret = (*sdio->get_block)(dio->inode, fs_startblk,
						map_bh, create);

		/* Store for completion */
		dio->private = map_bh->b_private;

		if (ret == 0 && buffer_defer_completion(map_bh))
			ret = dio_set_defer_completion(dio);
	}
	return ret;
}

/*
 * There is no bio.  Make one now.
 */
static inline int dio_new_bio(struct dio *dio, struct dio_submit *sdio,
		sector_t start_sector, struct buffer_head *map_bh)
{
	sector_t sector;
	int ret, nr_pages;

	ret = dio_bio_reap(dio, sdio);
	if (ret)
		goto out;
	sector = start_sector << (sdio->blkbits - 9);
	nr_pages = min(sdio->pages_in_io, bio_get_nr_vecs(map_bh->b_bdev));
	BUG_ON(nr_pages <= 0);
	dio_bio_alloc(dio, sdio, map_bh->b_bdev, sector, nr_pages);
	sdio->boundary = 0;
out:
	return ret;
}

/*
 * Attempt to put the current chunk of 'cur_page' into the current BIO.  If
 * that was successful then update final_block_in_bio and take a ref against
 * the just-added page.
 *
 * Return zero on success.  Non-zero means the caller needs to start a new BIO.
 */
static inline int dio_bio_add_page(struct dio_submit *sdio)
{
	int ret;

	ret = bio_add_page(sdio->bio, sdio->cur_page,
			sdio->cur_page_len, sdio->cur_page_offset);
	if (ret == sdio->cur_page_len) {
		/*
		 * Decrement count only, if we are done with this page
		 */
		if ((sdio->cur_page_len + sdio->cur_page_offset) == PAGE_SIZE)
			sdio->pages_in_io--;
		page_cache_get(sdio->cur_page);
		sdio->final_block_in_bio = sdio->cur_page_block +
			(sdio->cur_page_len >> sdio->blkbits);
		ret = 0;
	} else {
		ret = 1;
	}
	return ret;
}

/*
 * Put cur_page under IO.  The section of cur_page which is described by
 * cur_page_offset,cur_page_len is put into a BIO.  The section of cur_page
 * starts on-disk at cur_page_block.
 *
 * We take a ref against the page here (on behalf of its presence in the bio).
 *
 * The caller of this function is responsible for removing cur_page from the
 * dio, and for dropping the refcount which came from that presence.
 */
static inline int dio_send_cur_page(struct dio *dio, struct dio_submit *sdio,
		struct buffer_head *map_bh)
{
	int ret = 0;

	if (sdio->bio) {
		loff_t cur_offset = sdio->cur_page_fs_offset;
		loff_t bio_next_offset = sdio->logical_offset_in_bio +
			sdio->bio->bi_iter.bi_size;

		/*
		 * See whether this new request is contiguous with the old.
		 *
		 * Btrfs cannot handle having logically non-contiguous requests
		 * submitted.  For example if you have
		 *
		 * Logical:  [0-4095][HOLE][8192-12287]
		 * Physical: [0-4095]      [4096-8191]
		 *
		 * We cannot submit those pages together as one BIO.  So if our
		 * current logical offset in the file does not equal what would
		 * be the next logical offset in the bio, submit the bio we
		 * have.
		 */
		if (sdio->final_block_in_bio != sdio->cur_page_block ||
		    cur_offset != bio_next_offset)
		{
            //printk("MikeT: %s %s %d, before dio_bio_submit\n",__FILE__,__func__,__LINE__);
			dio_bio_submit(dio, sdio);
        }
	}

	if (sdio->bio == NULL) {
        //printk("MikeT: %s %s %d, sdio->bio == NULL\n",__FILE__,__func__,__LINE__);
		ret = dio_new_bio(dio, sdio, sdio->cur_page_block, map_bh);
		if (ret)
			goto out;
	}

	if (dio_bio_add_page(sdio) != 0) {
        //printk("MikeT: %s %s %d, before dio_bio_submit\n",__FILE__,__func__,__LINE__);
		dio_bio_submit(dio, sdio);
		ret = dio_new_bio(dio, sdio, sdio->cur_page_block, map_bh);
		if (ret == 0) {
			ret = dio_bio_add_page(sdio);
			BUG_ON(ret != 0);
		}
	}
out:
    //printk("MikeT: %s %s %d, ret: %d\n",__FILE__,__func__,__LINE__, ret);
	return ret;
}

/*
 * An autonomous function to put a chunk of a page under deferred IO.
 *
 * The caller doesn't actually know (or care) whether this piece of page is in
 * a BIO, or is under IO or whatever.  We just take care of all possible
 * situations here.  The separation between the logic of do_direct_IO() and
 * that of submit_page_section() is important for clarity.  Please don't break.
 *
 * The chunk of page starts on-disk at blocknr.
 *
 * We perform deferred IO, by recording the last-submitted page inside our
 * private part of the dio structure.  If possible, we just expand the IO
 * across that page here.
 *
 * If that doesn't work out then we put the old page into the bio and add this
 * page to the dio instead.
 */
static inline int
submit_page_section(struct dio *dio, struct dio_submit *sdio, struct page *page,
		    unsigned offset, unsigned len, sector_t blocknr,
		    struct buffer_head *map_bh)
{
	int ret = 0;

	if (dio->rw & WRITE) {
		/*
		 * Read accounting is performed in submit_bio()
		 */
		//printk("MikeT: %s %s %d\n",__FILE__,__func__,__LINE__);
		task_io_account_write(len);
	}

	/*
	 * Can we just grow the current page's presence in the dio?
	 */
	if (sdio->cur_page == page &&
	    sdio->cur_page_offset + sdio->cur_page_len == offset &&
	    sdio->cur_page_block +
	    (sdio->cur_page_len >> sdio->blkbits) == blocknr) {
		//printk("MikeT: %s %s %d, ret %d\n",__FILE__,__func__,__LINE__,ret);
		sdio->cur_page_len += len;
		goto out;
	}

	/*
	 * If there's a deferred page already there then send it.
	 */
	if (sdio->cur_page) {
		ret = dio_send_cur_page(dio, sdio, map_bh);
		//printk("MikeT: %s %s %d, ret %d\n",__FILE__,__func__,__LINE__,ret);
		page_cache_release(sdio->cur_page);
		sdio->cur_page = NULL;
		if (ret)
			return ret;
	}

	page_cache_get(page);		/* It is in dio */
	sdio->cur_page = page;
	sdio->cur_page_offset = offset;
	sdio->cur_page_len = len;
	sdio->cur_page_block = blocknr;
	sdio->cur_page_fs_offset = sdio->block_in_file << sdio->blkbits;
out:
	/*
	 * If sdio->boundary then we want to schedule the IO now to
	 * avoid metadata seeks.
	 */
	if (sdio->boundary) {

		//printk("MikeT: %s %s %d\n",__FILE__,__func__,__LINE__);
		ret = dio_send_cur_page(dio, sdio, map_bh);
		//printk("MikeT: %s %s %d, ret %d, before dio_bio_submit\n",__FILE__,__func__,__LINE__,ret);
		dio_bio_submit(dio, sdio);
		page_cache_release(sdio->cur_page);
		sdio->cur_page = NULL;
	}
	return ret;
}

/*
 * Clean any dirty buffers in the blockdev mapping which alias newly-created
 * file blocks.  Only called for S_ISREG files - blockdevs do not set
 * buffer_new
 */
static void clean_blockdev_aliases(struct dio *dio, struct buffer_head *map_bh)
{
	unsigned i;
	unsigned nblocks;

	nblocks = map_bh->b_size >> dio->inode->i_blkbits;

	for (i = 0; i < nblocks; i++) {
		unmap_underlying_metadata(map_bh->b_bdev,
					  map_bh->b_blocknr + i);
	}
}

/*
 * If we are not writing the entire block and get_block() allocated
 * the block for us, we need to fill-in the unused portion of the
 * block with zeros. This happens only if user-buffer, fileoffset or
 * io length is not filesystem block-size multiple.
 *
 * `end' is zero if we're doing the start of the IO, 1 at the end of the
 * IO.
 */
static inline void dio_zero_block(struct dio *dio, struct dio_submit *sdio,
		int end, struct buffer_head *map_bh)
{
	unsigned dio_blocks_per_fs_block;
	unsigned this_chunk_blocks;	/* In dio_blocks */
	unsigned this_chunk_bytes;
	struct page *page;

	sdio->start_zero_done = 1;
	if (!sdio->blkfactor || !buffer_new(map_bh))
		return;

	dio_blocks_per_fs_block = 1 << sdio->blkfactor;
	this_chunk_blocks = sdio->block_in_file & (dio_blocks_per_fs_block - 1);

	if (!this_chunk_blocks)
		return;

	/*
	 * We need to zero out part of an fs block.  It is either at the
	 * beginning or the end of the fs block.
	 */
	if (end)
		this_chunk_blocks = dio_blocks_per_fs_block - this_chunk_blocks;

	this_chunk_bytes = this_chunk_blocks << sdio->blkbits;

	page = ZERO_PAGE(0);
	if (submit_page_section(dio, sdio, page, 0, this_chunk_bytes,
				sdio->next_block_for_io, map_bh))
		return;

	sdio->next_block_for_io += this_chunk_blocks;
}

/*
 * Walk the user pages, and the file, mapping blocks to disk and generating
 * a sequence of (page,offset,len,block) mappings.  These mappings are injected
 * into submit_page_section(), which takes care of the next stage of submission
 *
 * Direct IO against a blockdev is different from a file.  Because we can
 * happily perform page-sized but 512-byte aligned IOs.  It is important that
 * blockdev IO be able to have fine alignment and large sizes.
 *
 * So what we do is to permit the ->get_block function to populate bh.b_size
 * with the size of IO which is permitted at this offset and this i_blkbits.
 *
 * For best results, the blockdev should be set up with 512-byte i_blkbits and
 * it should set b_size to PAGE_SIZE or more inside get_block().  This gives
 * fine alignment but still allows this function to work in PAGE_SIZE units.
 */
static int do_direct_IO(struct dio *dio, struct dio_submit *sdio,
			struct buffer_head *map_bh)
{
	const unsigned blkbits = sdio->blkbits;
	int ret = 0;
    //int x=0,y=0;
	while (sdio->block_in_file < sdio->final_block_in_request) {
		struct page *page;
		size_t from, to;
        //x++;

		page = dio_get_page(dio, sdio);
		if (IS_ERR(page)) {
			ret = PTR_ERR(page);
			goto out;
		}
		from = sdio->head ? 0 : sdio->from;
		to = (sdio->head == sdio->tail - 1) ? sdio->to : PAGE_SIZE;
		sdio->head++;

		while (from < to) {
			unsigned this_chunk_bytes;	/* # of bytes mapped */
			unsigned this_chunk_blocks;	/* # of blocks */
			unsigned u;

            //y++;
			if (sdio->blocks_available == 0) {
				/*
				 * Need to go and map some more disk
				 */
				unsigned long blkmask;
				unsigned long dio_remainder;
                printk("MikeT: %s %s %d\n", __FILE__, __func__, __LINE__);
				ret = get_more_blocks(dio, sdio, map_bh);
				if (ret) {
					page_cache_release(page);
					goto out;
				}
				if (!buffer_mapped(map_bh))
					goto do_holes;

				sdio->blocks_available =
						map_bh->b_size >> sdio->blkbits;
				sdio->next_block_for_io =
					map_bh->b_blocknr << sdio->blkfactor;
				if (buffer_new(map_bh))
					clean_blockdev_aliases(dio, map_bh);

				if (!sdio->blkfactor)
					goto do_holes;

				blkmask = (1 << sdio->blkfactor) - 1;
				dio_remainder = (sdio->block_in_file & blkmask);

				/*
				 * If we are at the start of IO and that IO
				 * starts partway into a fs-block,
				 * dio_remainder will be non-zero.  If the IO
				 * is a read then we can simply advance the IO
				 * cursor to the first block which is to be
				 * read.  But if the IO is a write and the
				 * block was newly allocated we cannot do that;
				 * the start of the fs block must be zeroed out
				 * on-disk
				 */
				if (!buffer_new(map_bh))
					sdio->next_block_for_io += dio_remainder;
				sdio->blocks_available -= dio_remainder;
			}
do_holes:
			/* Handle holes */
			if (!buffer_mapped(map_bh)) {
				loff_t i_size_aligned;

				/* AKPM: eargh, -ENOTBLK is a hack */
				if (dio->rw & WRITE) {
					page_cache_release(page);
					return -ENOTBLK;
				}

				/*
				 * Be sure to account for a partial block as the
				 * last block in the file
				 */
				i_size_aligned = ALIGN(i_size_read(dio->inode),
							1 << blkbits);
				if (sdio->block_in_file >=
						i_size_aligned >> blkbits) {
					/* We hit eof */
					page_cache_release(page);
					goto out;
				}
				printk("MikeT: %s %s %d\n", __FILE__, __func__, __LINE__);
				zero_user(page, from, 1 << blkbits);
				sdio->block_in_file++;
				from += 1 << blkbits;
				dio->result += 1 << blkbits;
				goto next_block;
			}

			/*
			 * If we're performing IO which has an alignment which
			 * is finer than the underlying fs, go check to see if
			 * we must zero out the start of this block.
			 */
			if (unlikely(sdio->blkfactor && !sdio->start_zero_done))
				dio_zero_block(dio, sdio, 0, map_bh);

			/*
			 * Work out, in this_chunk_blocks, how much disk we
			 * can add to this page
			 */
			this_chunk_blocks = sdio->blocks_available;
			u = (to - from) >> blkbits;
			if (this_chunk_blocks > u)
				this_chunk_blocks = u;
			u = sdio->final_block_in_request - sdio->block_in_file;
			if (this_chunk_blocks > u)
				this_chunk_blocks = u;
			this_chunk_bytes = this_chunk_blocks << blkbits;
			BUG_ON(this_chunk_bytes == 0);

			if (this_chunk_blocks == sdio->blocks_available)
				sdio->boundary = buffer_boundary(map_bh);
            //printk("MikeT: %s %s %d, %d %d, before submit_page_section\n", __FILE__,__func__, __LINE__,x,y);
			ret = submit_page_section(dio, sdio, page,
						  from,
						  this_chunk_bytes,
						  sdio->next_block_for_io,
						  map_bh);
            //printk("MikeT: %s %s %d, %d %d, ret %d, after submit_page_section\n", __FILE__,__func__, __LINE__,x,y, ret);
			if (ret) {
				page_cache_release(page);
				//printk("MikeT: %s %s %d, out\n", __FILE__,__func__, __LINE__);
				goto out;
			}
			sdio->next_block_for_io += this_chunk_blocks;

			sdio->block_in_file += this_chunk_blocks;
			from += this_chunk_bytes;
			dio->result += this_chunk_bytes;
			sdio->blocks_available -= this_chunk_blocks;
next_block:
			BUG_ON(sdio->block_in_file > sdio->final_block_in_request);
			if (sdio->block_in_file == sdio->final_block_in_request)
				break;
		}

		/* Drop the ref which was taken in get_user_pages() */
		page_cache_release(page);
	}
out:
	return ret;
}

static inline int send_RAID_bio_MikeT(struct dio *dio, struct dio_submit *sdio, struct bio *rbio)
{
    struct page *page;
    struct bio *bio;
    //struct bio_vec *bvec;
    char *tmp = kmalloc(4096, GFP_KERNEL);

    page = virt_to_page(tmp);


    //page = alloc_page(GFP_KERNEL);//rbio->bi_io_vec[0].bv_page;//__get_free_page(__GFP_IO);
    //page = kmalloc(sizeof(struct page), GFP_USER);
    //page1 = kmalloc(sizeof(struct page), GFP_KERNEL);
    //page2 = alloc_page(GFP_KERNEL);
    //page3 = alloc_page(GFP_USER);

    printk("MikeT: %s %s %d, page %p, tmp:%p\n", __FILE__, __func__, __LINE__,kmap(page),tmp);
    //memset(page_address(page), 0, PAGE_SIZE);
    if(IS_ERR(page))
    {
            printk("MikeT: %s %s %d, error page\n", __FILE__, __func__, __LINE__);
            kfree(tmp);
            return -1;
    }
    //page_cache_get(page);
     /*
     printk("MikeT: %s %s %d, "
               "bio %p, bi_next %p, bi_bdev %p, bi_flags %lx, bi_rw %lx; "
               "bi_iter: bi_sector %u, bi_size %u, bi_idx %u, bi_bvec_done %u; "
               "bi_phys_segments %u, bi_seg_front_size %u, bi_seg_back_size %u, bi_remaining %d; "
               "bi_vcnt %d, bi_max_vecs %d, bi_cnt %d, bi_io_vec %p, bi_pool %p\n ",
               __FILE__, __func__, __LINE__,
               rbio, rbio->bi_next, rbio->bi_bdev, rbio->bi_flags, rbio->bi_rw,
               rbio->bi_iter.bi_sector, rbio->bi_iter.bi_size, rbio->bi_iter.bi_idx, rbio->bi_iter.bi_bvec_done,
               rbio->bi_phys_segments, rbio->bi_seg_front_size, rbio->bi_seg_back_size, atomic_read(&rbio->bi_remaining),
               rbio->bi_vcnt, rbio->bi_max_vecs, atomic_read(&rbio->bi_cnt), rbio->bi_io_vec, rbio->bi_pool );
               */
    printk("MikeT: %s %s %d, get page: %p\n", __FILE__, __func__, __LINE__,page);
    bio = bio_kmalloc(GFP_KERNEL, 1);
    //bio_reset(bio);
    bio->bi_bdev = rbio->bi_bdev;
    //bio->bi_flags = rbio->bi_flags;
    bio->bi_rw = READ;

    bio->bi_iter.bi_sector = rbio->bi_iter.bi_sector;
    bio->bi_iter.bi_size = rbio->bi_iter.bi_size;

    //bio->bi_seg_front_size = rbio->bi_seg_front_size;
    //bio->bi_seg_back_size = rbio->bi_seg_back_size;
    bio->bi_private = dio;
    bio->bi_end_io = dio_bio_end_pio;
    bio->need_parity = true;

    //bio_add_page(bio, page, PAGE_SIZE, 0);
    bio->bi_io_vec[0].bv_page = page;
    bio->bi_io_vec[0].bv_len = PAGE_SIZE;
    bio->bi_io_vec[0].bv_offset = 0;
    //bio->bi_phys_segments++;
    bio->bi_vcnt++;

    //bvec = &rbio->bi_io_vec[rbio->bi_vcnt];

    //bio_add_page(bio, page, bvec->bv_len, bvec->bv_offset);
    //bio->bi_flags |= 1<<BIO_SEG_VALID;
    //printk("MikeT: %s %s %d\n", __FILE__, __func__, __LINE__);
    //kfree(page);
    //bio_put(bio);
    //printk("MikeT: %s %s %d, rw: %d, sectoraddr: %llu, sectorsize %d, deviceaddr: %p, rbio addr: %p,  bi_idx: %d,  bi_done %d, bi_vcnt %d\n",
	//__FILE__,__func__, __LINE__, rbio->bi_rw, (unsigned long long)rbio->bi_iter.bi_sector, rbio->bi_iter.bi_size, rbio->bi_bdev, rbio, rbio->bi_iter.bi_idx, rbio->bi_iter.bi_bvec_done, rbio->bi_vcnt);

    //printk("MikeT: %s %s %d, rw: %d, sectoraddr: %llu, sectorsize %d, deviceaddr: %p, bio addr: %p,  bi_idx: %d,  bi_done %d, bi_vcnt %d\n",
	//__FILE__,__func__, __LINE__, bio->bi_rw, (unsigned long long)bio->bi_iter.bi_sector, bio->bi_iter.bi_size, bio->bi_bdev, bio, bio->bi_iter.bi_idx, bio->bi_iter.bi_bvec_done, bio->bi_vcnt);
    dio->refcount++;
/*
    printk("MikeT: %s %s %d, "
               "bio %p, bi_next %p, bi_bdev %p, bi_flags %lx, bi_rw %lx; "
               "bi_iter: bi_sector %u, bi_size %u, bi_idx %u, bi_bvec_done %u; "
               "bi_phys_segments %u, bi_seg_front_size %u, bi_seg_back_size %u, bi_remaining %d; "
               "bi_vcnt %d, bi_max_vecs %d, bi_cnt %d, bi_io_vec %p, bi_pool %p\n ",
               __FILE__, __func__, __LINE__,
               bio, bio->bi_next, bio->bi_bdev, bio->bi_flags, bio->bi_rw,
               bio->bi_iter.bi_sector, bio->bi_iter.bi_size, bio->bi_iter.bi_idx, bio->bi_iter.bi_bvec_done,
               bio->bi_phys_segments, bio->bi_seg_front_size, bio->bi_seg_back_size, atomic_read(&bio->bi_remaining),
               bio->bi_vcnt, bio->bi_max_vecs, atomic_read(&bio->bi_cnt), bio->bi_io_vec, bio->bi_pool );
*/
    submit_bio(dio->rw, bio);
    //page_cache_release(page);
    return 0;
}

static inline bool isRaid(struct bio *bi, bool rw)
{
    struct request_queue *q = bdev_get_queue(bi->bi_bdev);
    char *modname=NULL;
    const char *name =NULL;
    unsigned long kaoffset, kasize;
    char namebuff[500];
    name = kallsyms_lookup((unsigned long)(q->make_request_fn), &kasize, &kaoffset, &modname, namebuff);
    printk("MikeT: %s %s %d, %s\n",__FILE__,__func__,__LINE__, name);
    if(strcmp(name, "md_make_request")==0&&rw==READ)
        return true;
    else
        return false;
}

/*
 * This is a library function for use by filesystem drivers.
 *
 * The locking rules are governed by the flags parameter:
 *  - if the flags value contains DIO_LOCKING we use a fancy locking
 *    scheme for dumb filesystems.
 *    For writes this function is called under i_mutex and returns with
 *    i_mutex held, for reads, i_mutex is not held on entry, but it is
 *    taken and dropped again before returning.
 *  - if the flags value does NOT contain DIO_LOCKING we don't use any
 *    internal locking but rather rely on the filesystem to synchronize
 *    direct I/O reads/writes versus each other and truncate.
 *
 * To help with locking against truncate we incremented the i_dio_count
 * counter before starting direct I/O, and decrement it once we are done.
 * Truncate can wait for it to reach zero to provide exclusion.  It is
 * expected that filesystem provide exclusion between new direct I/O
 * and truncates.  For DIO_LOCKING filesystems this is done by i_mutex,
 * but other filesystems need to take care of this on their own.
 *
 * NOTE: if you pass "sdio" to anything by pointer make sure that function
 * is always inlined. Otherwise gcc is unable to split the structure into
 * individual fields and will generate much worse code. This is important
 * for the whole file.
 */
static inline ssize_t
do_blockdev_direct_IO(int rw, struct kiocb *iocb, struct inode *inode,
	struct block_device *bdev, struct iov_iter *iter, loff_t offset,
	get_block_t get_block, dio_iodone_t end_io,
	dio_submit_t submit_io,	int flags)
{
	unsigned i_blkbits = ACCESS_ONCE(inode->i_blkbits);
	unsigned blkbits = i_blkbits;
	unsigned blocksize_mask = (1 << blkbits) - 1;
	ssize_t retval = -EINVAL;
	size_t count = iov_iter_count(iter);
	loff_t end = offset + count;
	struct dio *dio;
	struct dio_submit sdio = { 0, };
	struct buffer_head map_bh = { 0, };
	struct blk_plug plug;
	unsigned long align = offset | iov_iter_alignment(iter);
	bool isRAID=false;

	if (rw & WRITE)
		rw = WRITE_ODIRECT;

	/*
	 * Avoid references to bdev if not absolutely needed to give
	 * the early prefetch in the caller enough time.
	 */
	if (align & blocksize_mask) {
		if (bdev)
			blkbits = blksize_bits(bdev_logical_block_size(bdev));
		blocksize_mask = (1 << blkbits) - 1;
		if (align & blocksize_mask)
			goto out;
	}

	/* watch out for a 0 len io from a tricksy fs */
	if (rw == READ && !iov_iter_count(iter))
		return 0;

	dio = kmem_cache_alloc(dio_cache, GFP_KERNEL);
	retval = -ENOMEM;
	if (!dio)
		goto out;
	/*
	 * Believe it or not, zeroing out the page array caused a .5%
	 * performance regression in a database benchmark.  So, we take
	 * care to only zero out what's needed.
	 */
	memset(dio, 0, offsetof(struct dio, pages));

	dio->flags = flags;
	if (dio->flags & DIO_LOCKING) {
		if (rw == READ) {
			struct address_space *mapping =
					iocb->ki_filp->f_mapping;

			/* will be released by direct_io_worker */
			mutex_lock(&inode->i_mutex);

			retval = filemap_write_and_wait_range(mapping, offset,
							      end - 1);
			if (retval) {
				mutex_unlock(&inode->i_mutex);
				kmem_cache_free(dio_cache, dio);
				goto out;
			}
		}
	}

	/*
	 * For file extending writes updating i_size before data writeouts
	 * complete can expose uninitialized blocks in dumb filesystems.
	 * In that case we need to wait for I/O completion even if asked
	 * for an asynchronous write.
	 */
	if (is_sync_kiocb(iocb))
		dio->is_async = false;
	else if (!(dio->flags & DIO_ASYNC_EXTEND) &&
            (rw & WRITE) && end > i_size_read(inode))
		dio->is_async = false;
	else
		dio->is_async = true;

	dio->inode = inode;
	dio->rw = rw;

	/*
	 * For AIO O_(D)SYNC writes we need to defer completions to a workqueue
	 * so that we can call ->fsync.
	 */
	if (dio->is_async && (rw & WRITE) &&
	    ((iocb->ki_filp->f_flags & O_DSYNC) ||
	     IS_SYNC(iocb->ki_filp->f_mapping->host))) {
		retval = dio_set_defer_completion(dio);
		if (retval) {
			/*
			 * We grab i_mutex only for reads so we don't have
			 * to release it here
			 */
			kmem_cache_free(dio_cache, dio);
			goto out;
		}
	}

	/*
	 * Will be decremented at I/O completion time.
	 */
	atomic_inc(&inode->i_dio_count);

	retval = 0;
	sdio.blkbits = blkbits;
	sdio.blkfactor = i_blkbits - blkbits;
	sdio.block_in_file = offset >> blkbits;

	sdio.get_block = get_block;
	dio->end_io = end_io;
	sdio.submit_io = submit_io;
	sdio.final_block_in_bio = -1;
	sdio.next_block_for_io = -1;

	dio->iocb = iocb;
	dio->i_size = i_size_read(inode);

	spin_lock_init(&dio->bio_lock);
	dio->refcount = 1;

	sdio.iter = iter;
	sdio.final_block_in_request =
		(offset + iov_iter_count(iter)) >> blkbits;
    //printk("MikeT: %s %s %d, ITER_BVEC? %s\n", __FILE__, __func__, __LINE__, iter->type & 4?"yes":"no");
	/*
	 * In case of non-aligned buffers, we may need 2 more
	 * pages since we need to zero out first and last block.
	 */
	if (unlikely(sdio.blkfactor))
		sdio.pages_in_io = 2;

	sdio.pages_in_io += iov_iter_npages(iter, INT_MAX);

	blk_start_plug(&plug);

	retval = do_direct_IO(dio, &sdio, &map_bh);
	if (retval)
		dio_cleanup(dio, &sdio);

	if (retval == -ENOTBLK) {
		/*
		 * The remaining part of the request will be
		 * be handled by buffered I/O when we return
		 */
		retval = 0;
	}
	/*
	 * There may be some unwritten disk at the end of a part-written
	 * fs-block-sized block.  Go zero that now.
	 */
	dio_zero_block(dio, &sdio, 1, &map_bh);

	if (sdio.cur_page) {
		ssize_t ret2;

		ret2 = dio_send_cur_page(dio, &sdio, &map_bh);
		if (retval == 0)
			retval = ret2;
		page_cache_release(sdio.cur_page);
		sdio.cur_page = NULL;
	}
	if (sdio.bio)
	{
        isRAID = isRaid(sdio.bio, rw);
        printk("MikeT: %s %s %d, %s\n",__FILE__,__func__,__LINE__, isRAID?"is RAID":"not RAID");
		dio_bio_submit(dio, &sdio);
    }
	blk_finish_plug(&plug);
	/*
	 * It is possible that, we return short IO due to end of file.
	 * In that case, we need to release all the pages we got hold on.
	 */
    dio_cleanup(dio, &sdio);

	/*
	 * All block lookups have been performed. For READ requests
	 * we can let i_mutex go now that its achieved its purpose
	 * of protecting us from looking up uninitialized blocks.
	 */
	if (rw == READ && (dio->flags & DIO_LOCKING))
		mutex_unlock(&dio->inode->i_mutex);

	/*
	 * The only time we want to leave bios in flight is when a successful
	 * partial aio read or full aio write have been setup.  In that case
	 * bio completion will call aio_complete.  The only time it's safe to
	 * call aio_complete is when we return -EIOCBQUEUED, so we key on that.
	 * This had *better* be the only place that raises -EIOCBQUEUED.
	 */
	BUG_ON(retval == -EIOCBQUEUED);
	if (dio->is_async && retval == 0 && dio->result &&
	    (rw == READ || dio->result == count))
		retval = -EIOCBQUEUED;
	else
	{
		if(isRAID&&ignoreR!=0)
		{
            printk("MikeT: %s %s %d, is RAID\n",__FILE__,__func__,__LINE__);
            dio_await_completion_MikeT(dio);
        }
        else
        {
            printk("MikeT: %s %s %d, not RAID\n",__FILE__,__func__,__LINE__);
            dio_await_completion(dio);
        }

    }

    if (isRAID && reconRead!=0 && ignoreR!=0)
    {
        struct bio_record *head,*next;
        struct bio *rbio=NULL;
        head = dio->bio_record;
        while(head!=NULL)
        {
            next = head->next;
            if(head->bio->dio_comp)
            {
                rbio = head->bio;
                break;
            }
            head = next;
        }
        if(rbio)
        {
            send_RAID_bio_MikeT(dio, &sdio, rbio);
            printk("MikeT: %s %s %d, wait parity\n", __FILE__, __func__, __LINE__);
            dio_await_completion_parity_MikeT(dio);
            printk("MikeT: %s %s %d, after wait parity\n", __FILE__, __func__, __LINE__);
        }
    }

    if (isRAID && ignoreR!=0)
    {
        printk("MikeT: %s %s %d, drop_refcount: %d, is RAID\n",__FILE__,__func__,__LINE__, drop_refcount(dio));
        retval = dio_complete_MikeT(dio,offset,retval,false);
    }
    else if ((!isRAID || ignoreR==0) && drop_refcount(dio) == 0 ) {
        printk("MikeT: %s %s %d, %s\n",__FILE__,__func__,__LINE__, isRAID?"is RAID, ignoreR: 0":"not RAID");
		retval = dio_complete(dio, offset, retval, false);
	} else
		BUG_ON(retval != -EIOCBQUEUED);

out:
	return retval;
}

ssize_t
__blockdev_direct_IO(int rw, struct kiocb *iocb, struct inode *inode,
	struct block_device *bdev, struct iov_iter *iter, loff_t offset,
	get_block_t get_block, dio_iodone_t end_io,
	dio_submit_t submit_io,	int flags)
{
	/*
	 * The block device state is needed in the end to finally
	 * submit everything.  Since it's likely to be cache cold
	 * prefetch it here as first thing to hide some of the
	 * latency.
	 *
	 * Attempt to prefetch the pieces we likely need later.
	 */
	prefetch(&bdev->bd_disk->part_tbl);
	prefetch(bdev->bd_queue);
	prefetch((char *)bdev->bd_queue + SMP_CACHE_BYTES);

	return do_blockdev_direct_IO(rw, iocb, inode, bdev, iter, offset,
				     get_block, end_io, submit_io, flags);
}

EXPORT_SYMBOL(__blockdev_direct_IO);

static __init int dio_init(void)
{
	dio_cache = KMEM_CACHE(dio, SLAB_PANIC);
	return 0;
}
module_init(dio_init)