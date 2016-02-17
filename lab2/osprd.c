#include <linux/version.h>
#include <linux/autoconf.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>

#include <linux/sched.h>
#include <linux/kernel.h>  /* printk() */
#include <linux/errno.h>   /* error codes */
#include <linux/types.h>   /* size_t */
#include <linux/vmalloc.h>
#include <linux/genhd.h>
#include <linux/blkdev.h>
#include <linux/wait.h>
#include <linux/file.h>

#include "spinlock.h"
#include "osprd.h"
#include "linkedlist.h"
#include "pidqueue.h"

/* The size of an OSPRD sector. */
#define SECTOR_SIZE	512

/* This flag is added to an OSPRD file's f_flags to indicate that the file
 * is locked. */
#define F_OSPRD_LOCKED	0x80000

/* eprintk() prints messages to the console.
 * (If working on a real Linux machine, change KERN_NOTICE to KERN_ALERT or
 * KERN_EMERG so that you are sure to see the messages.  By default, the
 * kernel does not print all messages to the console.  Levels like KERN_ALERT
 * and KERN_EMERG will make sure that you will see messages.) */
#define eprintk(format, ...) printk(KERN_NOTICE format, ## __VA_ARGS__)

MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("CS 111 RAM Disk");
MODULE_AUTHOR("Phil Crumm and Ivan Petkov");

#define OSPRD_MAJOR	222

/* This module parameter controls how big the disk will be.
 * You can specify module parameters when you load the module,
 * as an argument to insmod: "insmod osprd.ko nsectors=4096" */
static int nsectors = 32;
module_param(nsectors, int, 0);


/* The internal representation of our device. */
typedef struct osprd_info {
	uint8_t *data;                  // The data array. Its size is
	                                // (nsectors * SECTOR_SIZE) bytes.

	osp_spinlock_t mutex;           // Mutex for synchronizing access to
					// this block device

	unsigned ticket_head;		// Currently running ticket for
					// the device lock

	unsigned ticket_tail;		// Next available ticket for
					// the device lock

	wait_queue_head_t blockq;       // Wait queue for tasks blocked on
					// the device lock

	/* HINT: You may want to add additional fields to help
	         in detecting deadlock. */
	list_node_t *lock_holder_l;
	list_node_t *lock_waiter_l;

	// If a signal wakes a process in the queue, we re-queue everything for simplicity.
	int num_to_requeue;

  unsigned num_read_locks;
  unsigned num_write_locks;

	// The following elements are used internally; you don't need
	// to understand them.
	struct request_queue *queue;    // The device request queue.
	spinlock_t qlock;		// Used internally for mutual
	                                //   exclusion in the 'queue'.
	struct gendisk *gd;             // The generic disk.
} osprd_info_t;

#define NOSPRD 4
static osprd_info_t osprds[NOSPRD];

/**
 * Helper functions for dealing with deadlock handling and detection
 */

// Declare useful helper functions

/*
 * file2osprd(filp)
 *   Given an open file, check whether that file corresponds to an OSP ramdisk.
 *   If so, return a pointer to the ramdisk's osprd_info_t.
 *   If not, return NULL.
 */
static osprd_info_t *file2osprd(struct file *filp);

/**
 * Given a file struct, returns the drive ID of the "file"
 */
int drive_id_from_info (osprd_info_t *d)
{
	int i;
	for (i = 0; i < NOSPRD; i++)
	{
		if (&osprds[i] == d)
		{
			return i;
		}
	}

	// If we get here, we have a serious problem
	eprintk("Unrecognized device");
	return -1;

}

/*
 * for_each_open_file(task, callback, user_data)
 *   Given a task, call the function 'callback' once for each of 'task's open
 *   files.  'callback' is called as 'callback(filp, user_data)'; 'filp' is
 *   the open file, and 'user_data' is copied from for_each_open_file's third
 *   argument.
 */
static void for_each_open_file(struct task_struct *task,
			       void (*callback)(struct file *filp,
						osprd_info_t *user_data),
			       osprd_info_t *user_data);

/**
 * Releases any read/write lock for a given file
 * and modifies the relevant osprd_info_t as appropriate and clears
 * F_OSPRD_LOCKED from filp->f_flags. If the file was locked, the entire
 * wait queue is woken up after the lock is removed.
 *
 * returns 0 on success
 * return -EINVAL if filp is NULL or is not locked
 */
static int release_file_lock(struct file *filp);

/**
 * Tries to acquire a lock for a file given that there are no locking
 * conflicts with the disk.
 *
 * returns 0 on success
 * returns -EBUSY if another lock is already in place
 * returns -EDEADLK if the process already has a lock
 */
static int try_acquire_file_lock(struct file *filp);

/*
 * osprd_process_request(d, req)
 *   Called when the user reads or writes a sector.
 *   Should perform the read or write, as appropriate.
 */
static void osprd_process_request(osprd_info_t *d, struct request *req)
{
  size_t offset;
  size_t num_bytes;

	if (!blk_fs_request(req)) {
		end_request(req, 0);
		return;
	}

	// EXERCISE: Perform the read or write request by copying data between
	// our data array and the request's buffer.
	// Hint: The 'struct request' argument tells you what kind of request
	// this is, and which sectors are being read or written.
	// Read about 'struct request' in <linux/blkdev.h>.
	// Consider the 'req->sector', 'req->current_nr_sectors', and
	// 'req->buffer' members, and the rq_data_dir() function.

  if (req->sector < 0 || req->sector >= nsectors)
  {
    // sector_t is defined as an unsigned long in <linux/types.h>
    eprintk("Invalid sector requested: [%lu]. max sectors: [%i]\n", (unsigned long)req->sector, nsectors);
    end_request(req, 0);
  }

  offset = req->sector * SECTOR_SIZE;

  // If the number of requested sectors would reach the end of the disk
  // use as many sectors as possible until the end is reached
  if(req->sector + req->current_nr_sectors > nsectors)
  {
    num_bytes = (nsectors - req->sector) * SECTOR_SIZE;
    eprintk("Requested sector [%lu] with [%u] additional sectors.\n", (unsigned long)req->sector, req->current_nr_sectors);
    eprintk("Using [%u] additional sectors instead.\n", num_bytes / SECTOR_SIZE);
  }
  else
  {
    num_bytes = req->current_nr_sectors * SECTOR_SIZE;
  }

  // According to http://www.makelinux.net/ldd3/chp-16-sect-3
  // it is save to dereference req->buffer and write to it.

  // Note from @ipetkov: I'm not sure if req->buffer needs to
  // be resized at all, I'm assuming linux will allocate the
  // memory before the request is sent. No issues are apparent
  // from the first 8 default test cases.
  spin_lock(&d->mutex);

  if(rq_data_dir(req) == READ)
    memcpy(req->buffer, d->data + offset, num_bytes);
  else // WRITE
    memcpy(d->data + offset, req->buffer, num_bytes);

  spin_unlock(&d->mutex);

	end_request(req, 1);
}


// This function is called when a /dev/osprdX file is opened.
// You aren't likely to need to change this.
static int osprd_open(struct inode *inode, struct file *filp)
{
	// Always set the O_SYNC flag. That way, we will get writes immediately
	// instead of waiting for them to get through write-back caches.
	filp->f_flags |= O_SYNC;
	return 0;
}


// This function is called when a /dev/osprdX file is finally closed.
// (If the file descriptor was dup2ed, this function is called only when the
// last copy is closed.)
static int osprd_close_last(struct inode *inode, struct file *filp)
{
	// EXERCISE: If the user closes a ramdisk file that holds
	// a lock, release the lock.  Also wake up blocked processes
	// as appropriate.

	release_file_lock(filp);

	return 0;
}


/*
 * osprd_lock
 */
static int release_file_lock(struct file *filp)
{
  if (filp) {
    osprd_info_t *d = file2osprd(filp);
    int filp_writable = filp->f_mode & FMODE_WRITE;
    int filp_locked = filp->f_flags & F_OSPRD_LOCKED;

    if(filp_locked)
    {
      spin_lock(&d->mutex);

      if(filp_writable)
        d->num_write_locks--;
      else // file open for reading
        d->num_read_locks--;

      // Set the head to the next ticket so that some process may respond
      // Check for overflows, if the tail wraps around to 0, move the head to 0 as well.
      if(d->ticket_head < d->ticket_tail)
        d->ticket_head++;
      else if(d->ticket_head > d->ticket_tail)
        d->ticket_head = 0;

      d->lock_holder_l = list_remove_element(d->lock_holder_l, current->pid);
      spin_unlock(&d->mutex);

      // Clear the file's locked bit
      filp->f_flags &= ~F_OSPRD_LOCKED;

      wake_up_all(&d->blockq);
      return 0;
    }
  }

  return -EINVAL;
}

static int try_acquire_file_lock(struct file *filp)
{
  osprd_info_t *d = file2osprd(filp);
  int filp_writable = filp->f_mode & FMODE_WRITE;

  // Check that this flag doesn't already have a lock
  if(filp->f_flags & F_OSPRD_LOCKED)
    return -EDEADLK;

  spin_lock(&d->mutex);

  // We cannot grab the disk if there is any write lock
  // or if the caller wishes to write and someone else is reading
  if(d->num_write_locks > 0 || (filp_writable && d->num_read_locks > 0))
  {
    spin_unlock(&d->mutex);
    return -EBUSY;
  }

  // Grab the lock!
  if(filp_writable)
    d->num_write_locks++;
  else // readable
    d->num_read_locks++;

  // Add the process to the holder's list
  d->lock_holder_l = list_add_to_front(d->lock_holder_l, current->pid);

  spin_unlock(&d->mutex);
  filp->f_flags |= F_OSPRD_LOCKED;
  return 0;
}

static bool current_has_lock_on_disk (int d_id)
{
	list_node_t *result;
	osprd_info_t *d = &osprds[d_id];

	spin_lock(&d->mutex);
	result = list_contains(d->lock_holder_l, current->pid);
	spin_unlock(&d->mutex);

	return result ? true : false;
}

static int find_drive_id_for_waiter (pid_t p)
{
	int i;
	list_node_t *elem = NULL;
	for(i = 0; i < NOSPRD; i++)
	{
		spin_lock(&(osprds[i].mutex));
		elem = list_contains(osprds[i].lock_waiter_l, p);
		spin_unlock(&(osprds[i].mutex));

		if(elem && elem->visited == false)
		{
			elem->visited = true;
			return i;
		}
	}

	// If we get here, we've visited the node before
	return -1;
}

static bool check_deadlock (osprd_info_t *d)
{
	pid_queue_t *q;
	int i, d_id = drive_id_from_info(d);
	bool ret = false;
	pid_t pid = -1;

	q = pid_queue_init();

	for(;;)
	{
		if(d_id > -1)
		{
			// Cannot lock the same disk twice
			if(current_has_lock_on_disk(d_id))
			{
				ret = true;
				break;
			}

			spin_lock(&d->mutex);
			pid_queue_add_elements_from_list(q, osprds[d_id].lock_holder_l);
			spin_unlock(&d->mutex);
		}

		if(pid_queue_empty(q))
			break;

		pid = pid_queue_pop(q);
		d_id = find_drive_id_for_waiter(pid);
	}

	// Clean up after checks
	for(i = 0; i < NOSPRD; i++)
	{
		spin_lock(&(osprds[i].mutex));
		list_mark_visited(osprds[i].lock_waiter_l, false);
		spin_unlock(&(osprds[i].mutex));
	}

	pid_queue_remove_all(q);
	kfree(q);

	return ret;
}

/*
 * osprd_ioctl(inode, filp, cmd, arg)
 *   Called to perform an ioctl on the named file.
 */
int osprd_ioctl(struct inode *inode, struct file *filp,
		unsigned int cmd, unsigned long arg)
{
	osprd_info_t *d = file2osprd(filp);	// device info
	int r = 0;			// return value: initially 0

	// Set 'r' to the ioctl's return value: 0 on success, negative on error

	if (cmd == OSPRDIOCACQUIRE) {

		// EXERCISE: Lock the ramdisk.
		//
		// If *filp is open for writing (filp_writable), then attempt
		// to write-lock the ramdisk; otherwise attempt to read-lock
		// the ramdisk.
		//
        // This lock request must block using 'd->blockq' until:
		// 1) no other process holds a write lock;
		// 2) either the request is for a read lock, or no other process
		//    holds a read lock; and
		// 3) lock requests should be serviced in order, so no process
		//    that blocked earlier is still blocked waiting for the
		//    lock.
		//
		// If a process acquires a lock, mark this fact by setting
		// 'filp->f_flags |= F_OSPRD_LOCKED'.  You also need to
		// keep track of how many read and write locks are held:
		// change the 'osprd_info_t' structure to do this.
		//
		// Also wake up processes waiting on 'd->blockq' as needed.
		//
		// If the lock request would cause a deadlock, return -EDEADLK.
		// If the lock request blocks and is awoken by a signal, then
		// return -ERESTARTSYS.
		// Otherwise, if we can grant the lock request, return 0.

		// 'd->ticket_head' and 'd->ticket_tail' should help you
		// service lock requests in order.  These implement a ticket
		// order: 'ticket_tail' is the next ticket, and 'ticket_head'
		// is the ticket currently being served.  You should set a local
		// variable to 'd->ticket_head' and increment 'd->ticket_head'.
		// Then, block at least until 'd->ticket_tail == local_ticket'.
		// (Some of these operations are in a critical section and must
		// be protected by a spinlock; which ones?)

    // Used to track the current request
    unsigned local_ticket;

    if(check_deadlock(d))
		return -EDEADLK;

    // If we can't acquire the lock we put ourselves in the back of the queue
    // when the lock is released ticket_head will be incremented and we'll be
    // woken up
    while(try_acquire_file_lock(filp) != 0)
    {
      spin_lock(&d->mutex);
      d->ticket_tail++;
      local_ticket = d->ticket_tail;
      d->lock_waiter_l = list_add_to_front(d->lock_waiter_l, current->pid);
      spin_unlock(&d->mutex);

      wait_event_interruptible(d->blockq, d->ticket_head == local_ticket || d->num_to_requeue > 0);

      spin_lock(&d->mutex);
      d->lock_waiter_l = list_remove_element(d->lock_waiter_l, current->pid);
      spin_unlock(&d->mutex);

      // process any pending signals by re-queueing everything
      if(d->num_to_requeue > 0)
      {
        // Note: do NOT wake threads here, each thread should requeue only ONCE
        spin_lock(&d->mutex);
        d->num_to_requeue--;
        spin_unlock(&d->mutex);

        // If we find another pending signal, dispatch that too
        if(signal_pending(current))
          return -ERESTARTSYS;
      }
      else if(signal_pending(current)) // See if we were woken up by a signal
      {
        // For simplicity, we requeue all waiting tasks (-1 which
        // is the process) being "popped" off the wait queue
        spin_lock(&d->mutex);
        d->num_to_requeue = (d->ticket_tail - d->ticket_head - 1);
        d->ticket_head = 0;
        d->ticket_tail = 0;

        // All threads are woken to notify them of requeuing
        // meanwhile, we wait until that finishes (no need to check
        // for more interrupts, the process will exit anyway).
        wake_up_all(&d->blockq);
        spin_unlock(&d->mutex);

        // Sanity check
        if(d->num_to_requeue > 0)
        {
          wait_event(d->blockq, d->num_to_requeue == 0);
          wake_up_all(&d->blockq); // Wake everyone up again to check for other pending signals
        }
        return -ERESTARTSYS;
      }
    }

    r = 0;

	} else if (cmd == OSPRDIOCTRYACQUIRE) {

		// EXERCISE: ATTEMPT to lock the ramdisk.
		//
		// This is just like OSPRDIOCACQUIRE, except it should never
		// block.  If OSPRDIOCACQUIRE would block or return deadlock,
		// OSPRDIOCTRYACQUIRE should return -EBUSY.
		// Otherwise, if we can grant the lock request, return 0.

    r = try_acquire_file_lock(filp);

    if(r == -EDEADLK)
		r = -EBUSY;

	} else if (cmd == OSPRDIOCRELEASE) {

		// EXERCISE: Unlock the ramdisk.
		//
		// If the file hasn't locked the ramdisk, return -EINVAL.
		// Otherwise, clear the lock from filp->f_flags, wake up
		// the wait queue, perform any additional accounting steps
		// you need, and return 0.

		r = release_file_lock(filp);

	} else
		r = -ENOTTY; /* unknown command */
	return r;
}


// Initialize internal fields for an osprd_info_t.

static void osprd_setup(osprd_info_t *d)
{
	/* Initialize the wait queue. */
	init_waitqueue_head(&d->blockq);
	osp_spin_lock_init(&d->mutex);
	d->ticket_head = d->ticket_tail = 0;
	/* Add code here if you add fields to osprd_info_t. */

  d->num_read_locks = 0;
  d->num_write_locks = 0;
  d->num_to_requeue = 0;

  d->lock_holder_l = NULL;
	d->lock_waiter_l = NULL;
}


/*****************************************************************************/
/*         THERE IS NO NEED TO UNDERSTAND ANY CODE BELOW THIS LINE!          */
/*                                                                           */
/*****************************************************************************/

// Process a list of requests for a osprd_info_t.
// Calls osprd_process_request for each element of the queue.

static void osprd_process_request_queue(request_queue_t *q)
{
	osprd_info_t *d = (osprd_info_t *) q->queuedata;
	struct request *req;

	while ((req = elv_next_request(q)) != NULL)
		osprd_process_request(d, req);
}


// Some particularly horrible stuff to get around some Linux issues:
// the Linux block device interface doesn't let a block device find out
// which file has been closed.  We need this information.

static struct file_operations osprd_blk_fops;
static int (*blkdev_release)(struct inode *, struct file *);

static int _osprd_release(struct inode *inode, struct file *filp)
{
	if (file2osprd(filp))
		osprd_close_last(inode, filp);
	return (*blkdev_release)(inode, filp);
}

static int _osprd_open(struct inode *inode, struct file *filp)
{
	if (!osprd_blk_fops.open) {
		memcpy(&osprd_blk_fops, filp->f_op, sizeof(osprd_blk_fops));
		blkdev_release = osprd_blk_fops.release;
		osprd_blk_fops.release = _osprd_release;
	}
	filp->f_op = &osprd_blk_fops;
	return osprd_open(inode, filp);
}


// The device operations structure.

static struct block_device_operations osprd_ops = {
	.owner = THIS_MODULE,
	.open = _osprd_open,
	// .release = osprd_release, // we must call our own release
	.ioctl = osprd_ioctl
};


// Given an open file, check whether that file corresponds to an OSP ramdisk.
// If so, return a pointer to the ramdisk's osprd_info_t.
// If not, return NULL.

static osprd_info_t *file2osprd(struct file *filp)
{
	if (filp) {
		struct inode *ino = filp->f_dentry->d_inode;
		if (ino->i_bdev
		    && ino->i_bdev->bd_disk
		    && ino->i_bdev->bd_disk->major == OSPRD_MAJOR
		    && ino->i_bdev->bd_disk->fops == &osprd_ops)
			return (osprd_info_t *) ino->i_bdev->bd_disk->private_data;
	}
	return NULL;
}


// Call the function 'callback' with data 'user_data' for each of 'task's
// open files.

static void for_each_open_file(struct task_struct *task,
		  void (*callback)(struct file *filp, osprd_info_t *user_data),
		  osprd_info_t *user_data)
{
	int fd;
	task_lock(task);
	spin_lock(&task->files->file_lock);
	{
#if LINUX_VERSION_CODE <= KERNEL_VERSION(2, 6, 13)
		struct files_struct *f = task->files;
#else
		struct fdtable *f = task->files->fdt;
#endif
		for (fd = 0; fd < f->max_fds; fd++)
			if (f->fd[fd])
				(*callback)(f->fd[fd], user_data);
	}
	spin_unlock(&task->files->file_lock);
	task_unlock(task);
}


// Destroy a osprd_info_t.

static void cleanup_device(osprd_info_t *d)
{
	wake_up_all(&d->blockq);
	if (d->gd) {
		del_gendisk(d->gd);
		put_disk(d->gd);
	}
	if (d->queue)
		blk_cleanup_queue(d->queue);
	if (d->data)
		vfree(d->data);

	list_free_all(d->lock_holder_l);
	list_free_all(d->lock_waiter_l);
	d->lock_holder_l = NULL;
	d->lock_waiter_l = NULL;
}


// Initialize a osprd_info_t.

static int setup_device(osprd_info_t *d, int which)
{
	memset(d, 0, sizeof(osprd_info_t));

	/* Get memory to store the actual block data. */
	if (!(d->data = vmalloc(nsectors * SECTOR_SIZE)))
		return -1;
	memset(d->data, 0, nsectors * SECTOR_SIZE);

	/* Set up the I/O queue. */
	spin_lock_init(&d->qlock);
	if (!(d->queue = blk_init_queue(osprd_process_request_queue, &d->qlock)))
		return -1;
	blk_queue_hardsect_size(d->queue, SECTOR_SIZE);
	d->queue->queuedata = d;

	/* The gendisk structure. */
	if (!(d->gd = alloc_disk(1)))
		return -1;
	d->gd->major = OSPRD_MAJOR;
	d->gd->first_minor = which;
	d->gd->fops = &osprd_ops;
	d->gd->queue = d->queue;
	d->gd->private_data = d;
	snprintf(d->gd->disk_name, 32, "osprd%c", which + 'a');
	set_capacity(d->gd, nsectors);
	add_disk(d->gd);

	/* Call the setup function. */
	osprd_setup(d);

	return 0;
}

static void osprd_exit(void);


// The kernel calls this function when the module is loaded.
// It initializes the 4 osprd block devices.

static int __init osprd_init(void)
{
	int i, r;

	// shut up the compiler
	(void) for_each_open_file;
#ifndef osp_spin_lock
	(void) osp_spin_lock;
	(void) osp_spin_unlock;
#endif

	/* Register the block device name. */
	if (register_blkdev(OSPRD_MAJOR, "osprd") < 0) {
		printk(KERN_WARNING "osprd: unable to get major number\n");
		return -EBUSY;
	}

	/* Initialize the device structures. */
	for (i = r = 0; i < NOSPRD; i++)
		if (setup_device(&osprds[i], i) < 0)
			r = -EINVAL;

	if (r < 0) {
		printk(KERN_EMERG "osprd: can't set up device structures\n");
		osprd_exit();
		return -EBUSY;
	} else
		return 0;
}


// The kernel calls this function to unload the osprd module.
// It destroys the osprd devices.

static void osprd_exit(void)
{
	int i;
	for (i = 0; i < NOSPRD; i++)
		cleanup_device(&osprds[i]);
	unregister_blkdev(OSPRD_MAJOR, "osprd");
}


// Tell Linux to call those functions at init and exit time.
module_init(osprd_init);
module_exit(osprd_exit);
