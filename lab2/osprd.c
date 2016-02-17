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
// DONE EXERCISE: Pass your names into the kernel as the module's authors.
MODULE_AUTHOR("Cristen Anderson and Sunnie So");
#define OSPRD_MAJOR	222

/* This module parameter controls how big the disk will be.
 * You can specify module parameters when you load the module,
 * as an argument to insmod: "insmod osprd.ko nsectors=4096" */
static int nsectors = 32;
module_param(nsectors, int, 0);

/* linked list nodes to hold processes that have read/write locks, and invalid ticket numbers*/
struct pid_list {
	pid_t pid;
	struct pid_list* next;
};
struct invalid_list {
	unsigned num;
	struct invalid_list* next;
};

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

	wait_queue_head_t	 blockq;       // Wait queue for tasks blocked on
					// the device lock

	/* HINT: You may want to add additional fields to help
	         in detecting deadlock. */


	// number of processes that currently have the write lock
	int write_lock;
	// number of processes that currently have the read lock
	int read_locks;

	// list of processes with read/write locks
	struct pid_list read_lock_procs;
	pid_t write_lock_proc;

	// list of invalid ticket numbers
	struct invalid_list invalid_tickets;


	// The following elements are used internally; you don't need
	// to understand them.
	struct request_queue *queue;    // The device request queue.
	spinlock_t qlock;		// Used internally for mutual
	                                //   exclusion in the 'queue'.
	struct gendisk *gd;             // The generic disk.
} osprd_info_t;

#define NOSPRD 4
static osprd_info_t osprds[NOSPRD];


// Declare useful helper functions

/* 
* next_valid_ticket(invalid, ticket_head)
* Returns the next valid executable ticket
*/
unsigned next_valid_ticket(struct invalid_list* invalid, unsigned ticket_head) {
	// If the num is -1, it means there are no invalid tickets
	// ticket_head must be valid
	if (invalid->num == -1) 
		return ticket_head;
	// set pointer to invalid list
	struct invalid_list* list = invalid;
	// If the ticket_head is invalid, increment and check again 
		// from beginning of list
	while (1) {
		// eprintk("Infinite loop?\n");
		if (ticket_head == list->num) {
			ticket_head++;
			list = invalid;
			continue;
		}
		// if this condition is met, ticket_head couldn't be found
			// therefore ticket_head is valid
		if (list->next == NULL) {
			return ticket_head;
		}
		// Iterate to next node
		list = list->next;
	}
}

/*
* add_to_invalid(list, ticket)
* add ticket to the list of invalid tickets
*/
void add_to_invalid(struct invalid_list* list, unsigned ticket) {
	// If the num is -1, it means no nodes have been added yet
	// Use this first node for the first invalid ticket
	if (list->num == -1) {
		list->num = ticket;
		return;
	}
	// Find the last used node in the list
	while (list->next != NULL)
		list = list->next;
	// Create a new node to add to the list
	struct invalid_list inval;
	inval.num = ticket;
	inval.next = NULL;
	// Add the node to the list
	list->next = &inval;
	return;
}

/*
* add_to_read(list, pid)
* add pid to the list of procedures with read lock
*/
void add_to_read(struct pid_list* list, pid_t read_pid) {
	// If the pid is -1, it means no nodes have been added yet
	// Use this first node for the first read_pid
	if (list->pid == -1) {
		list->pid = read_pid;
		return;
	}
	// Find the last used node in the list
	while (list->next != NULL)
		list = list->next;
	// Create a new node to add to the list
	struct pid_list p;
	p.pid = read_pid;
	p.next = NULL;
	// Add the node to the list
	list->next = &p;
	return;
}
/*
 * file2osprd(filp)
 *   Given an open file, check whether that file corresponds to an OSP ramdisk.
 *   If so, return a pointer to the ramdisk's osprd_info_t.
 *   If not, return NULL.
 */
static osprd_info_t *file2osprd(struct file *filp);


/*
* remove_from_read(list, ticket)
* remove pid from the list of procedures with read lock
*/
void remove_from_read(struct pid_list* list, pid_t read_pid) {
	// If next is null, read_pid must be the only node in the list
	// Invalidate by setting pid to -1. 
	if (list->next == NULL) {
		if (list->pid != read_pid) {
			eprintk("Invalid list structure: first node, read_pid == %d\n", read_pid);
			return;
		}
		list->pid = -1;
		return;
	}
	// Stop at the node before the node we want to remove
	while (list->next != NULL && list->next->pid != read_pid)
		list = list->next;
	// If it's null it means read_pid couldn't be found in the list
	if (list->next == NULL) {
		eprintk("Invalid list structure: in the middle\n");
		return;
	}
	// Set the next pointer to the next pointer of the node we want to remove
	// This effectively skips over the node we want to remove
	list->next = list->next->next;
	return;
}

/*
* deadlock_check(file*)
* return -1 when there is a deadlock
* return 0 when there is no deadlock
*/
int deadlock_check(struct file* filp){
	
	osprd_info_t *d = file2osprd(filp);	// device info
	int r = 0;			// return value: initially 0

	// is file open for writing?
	int filp_writable = (filp->f_mode & FMODE_WRITE) != 0;

	// This line avoids compiler warnings; you may remove it.
	(void) filp_writable, (void) d;

	// Check that the flag doesn't already have a lock
  	if(filp->f_flags & F_OSPRD_LOCKED)
    		return -1;
	if(current->pid == d->write_lock_proc){
		eprintk("deadlock catch: current pid has a lock already.\n");		
		return -1;
	}
	
	struct pid_list * li = &(d->read_lock_procs);
	if(li->next == NULL)
		eprintk("%d -> ", li->pid);
	while(li->next !=NULL){
		eprintk("%d -> ", li->pid);
		if(li->pid == current->pid){
			eprintk("deadlock catch: current pid has a read lock\n");
			return -EDEADLK;
		}
		li = li->next;
	}
	eprintk("\n");
	return 0;
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


/*
 * osprd_process_request(d, req)
 *   Called when the user reads or writes a sector.
 *   Should perform the read or write, as appropriate.
 */
static void osprd_process_request(osprd_info_t *d, struct request *req)
{
	if (!blk_fs_request(req)) {
		end_request(req, 0);
		return;
	}


	// DONE EXERCISE: Perform the read or write request by copying data between
	// our data array and the request's buffer.
	// Hint: The 'struct request' argument tells you what kind of request
	// this is, and which sectors are being read or written.
	// Read about 'struct request' in <linux/blkdev.h>.
	// Consider the 'req->sector', 'req->current_nr_sectors', and
	// 'req->buffer' members, and the rq_data_dir() function.

	// Your code here.

	// compute the offset, set pointer to correct region
	uint8_t *dataPtr = d->data + (req->sector) * SECTOR_SIZE;
	
	// check if it's read or write and copy data
	unsigned int requestType = rq_data_dir(req);

	if(requestType == READ)
		memcpy((void*) req->buffer, (void*)dataPtr, req->current_nr_sectors * SECTOR_SIZE);
	else if (requestType == WRITE)
		memcpy((void*)dataPtr, (void*) req->buffer, req->current_nr_sectors * SECTOR_SIZE);

	//eprintk("Should process request...\n");
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
	eprintk("entering close_last... \n");
	if (filp) {
		osprd_info_t *d = file2osprd(filp);
		int filp_writable = (filp->f_mode & FMODE_WRITE) != 0;
		// DONE EXERCISE: If the user closes a ramdisk file that holds
		// a lock, release the lock.  Also wake up blocked processes
		// as appropriate.

		// Your code here.
		//if (filp->f_flags == (filp->f_flags | F_OSPRD_LOCKED)) {
		if (filp->f_flags & F_OSPRD_LOCKED){
			filp->f_flags &= ~F_OSPRD_LOCKED;
			osp_spin_lock(&(d->mutex));
			if(filp_writable) {
				d->write_lock_proc = -1;
				d->write_lock = 0;
			}
			else {
				eprintk("from osprd_close_last...\n");
				
				remove_from_read(&(d->read_lock_procs), current->pid);
				d->read_locks--;
				
				
			}
			// wake up tasks in wait queue:
			wake_up_all(&(d->blockq));
			osp_spin_unlock(&(d->mutex));
		}
		// This line avoids compiler warnings; you may remove it.
		// (void) filp_writable, (void) d;

	}
	return 0;
}


/*
 * osprd_lock
 */

/*
 * osprd_ioctl(inode, filp, cmd, arg)
 *   Called to perform an ioctl on the named file.
 */
int osprd_ioctl(struct inode *inode, struct file *filp,
		unsigned int cmd, unsigned long arg)
{
	osprd_info_t *d = file2osprd(filp);	// device info
	int r = 0;			// return value: initially 0

	// is file open for writing?
	int filp_writable = (filp->f_mode & FMODE_WRITE) != 0;

	// This line avoids compiler warnings; you may remove it.
	(void) filp_writable, (void) d;




	// Set 'r' to the ioctl's return value: 0 on success, negative on error

	if (cmd == OSPRDIOCACQUIRE) {

		// DONE EXERCISE: Lock the ramdisk.
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

		// Your code here (instead of the next two lines).

		// eprintk("Attempting to acquire\n");
		//r = -ENOTTY;
	
		eprintk("entering ACQUIRE case...pid == %d\n",current->pid);
		// DEADLOCK check

		if (deadlock_check(filp))
			return -EDEADLK;
		
		eprintk("ACQUIRE: grabbing ticket... pid == %d\n",current->pid);
		// get the ticket:
		osp_spin_lock(&(d->mutex));
		unsigned my_ticket = d->ticket_tail;
		d->ticket_tail++;
		eprintk("ACQUIRE: in ticket spin lock... pid == %d\n",current->pid);
		osp_spin_unlock(&(d->mutex));
		eprintk("ACQUIRE: grabbed ticket... pid == %d\n",current->pid);
		
		if (filp_writable) {
			if(wait_event_interruptible(d->blockq, (my_ticket == d->ticket_head) 
							&& (d->write_lock == 0) && (d->read_locks == 0)) == -ERESTARTSYS){
				// we enter here when the current task receives a signal before
				// CONDITION becomes true, and the macro returns -ERESTARTSYS.
				// if on the ticket_head, set to the next valid ticket, 
				eprintk("Signal interrupt\n");
				osp_spin_lock(&(d->mutex));

				// If current ticket is ticket_head, simply set to next ticket,
					// otherwise ticket is invalidated
				if (d->ticket_head == my_ticket)
					d->ticket_head = next_valid_ticket(&(d->invalid_tickets), d->ticket_head+1);
				else add_to_invalid(&(d->invalid_tickets), my_ticket);

				// wake up tasks in wait queue:
				wake_up_all(&(d->blockq));
				osp_spin_unlock(&(d->mutex));
				r = -ERESTARTSYS;
			} else {
				// We can get the lock!
				eprintk("OSPRDIOCACQUIRE w, %d,  attempt to acquire spin lock...\n", current->pid);
				osp_spin_lock(&(d->mutex));

				eprintk("OSPRDIOCACQUIRE w, %d,  got spin lock!\n",current->pid);
				// add ourselves to the write list
				d->write_lock = 1; 
				d->write_lock_proc = current->pid;

				// final settings(we acquired lock): 
				filp->f_flags |= F_OSPRD_LOCKED;
				d->ticket_head = next_valid_ticket(&(d->invalid_tickets), d->ticket_head+1);
				osp_spin_unlock(&(d->mutex));
				r = 0;
			}
		} else {
			if (wait_event_interruptible(d->blockq, (my_ticket == d->ticket_head) 
							&& (d->write_lock == 0)) == -ERESTARTSYS) {
				// we enter here when the current task receives a signal before
				// CONDITION becomes true, and the macro returns -ERESTARTSYS.
				// if on the ticket_head, set to the next valid ticket, 
				eprintk("Signal interrupt\n");
				osp_spin_lock(&(d->mutex));

				// If current ticket is ticket_head, simply set to next ticket,
					// otherwise ticket is invalidated
				if (d->ticket_head == my_ticket)
					d->ticket_head = next_valid_ticket(&(d->invalid_tickets), d->ticket_head+1);
				else 
					add_to_invalid(&(d->invalid_tickets), my_ticket);

				// wake up tasks in wait queue:
				wake_up_all(&(d->blockq));
				osp_spin_unlock(&(d->mutex));
				r = -ERESTARTSYS;
			} else {
				// We can get the lock!
				eprintk("OSPRDIOCACQUIRE r, %d,  attempt to acquire spin lock...\n", current->pid);
				
				osp_spin_lock(&(d->mutex));
				eprintk("OSPRDIOCACQUIRE r , %d, got spin lock...\n", current->pid);
				
				// add ourselves to the read list
				d->read_locks++; 
				add_to_read(&(d->read_lock_procs), current->pid);

				// final settings(we acquired lock): 
				filp->f_flags |= F_OSPRD_LOCKED;
				d->ticket_head = next_valid_ticket(&(d->invalid_tickets), d->ticket_head+1);
				osp_spin_unlock(&(d->mutex));
				r = 0;
			}
		}
		
		
	} else if (cmd == OSPRDIOCTRYACQUIRE) {

		// DONE EXERCISE: ATTEMPT to lock the ramdisk.
		//
		// This is just like OSPRDIOCACQUIRE, except it should never
		// block.  If OSPRDIOCACQUIRE would block or return deadlock,
		// OSPRDIOCTRYACQUIRE should return -EBUSY.
		// Otherwise, if we can grant the lock request, return 0.

		// Your code here (instead of the next two lines).
		// eprintk("Attempting to try acquire\n");
		//r = -ENOTTY;

		if(filp->f_flags & F_OSPRD_LOCKED)
	    		return -EBUSY;
		// get the ticket:
		//osp_spin_lock(&(d->mutex));
	//	unsigned my_ticket = d->ticket_tail;
	//	d->ticket_tail++;
	//	osp_spin_unlock(&(d->mutex));

		if (filp_writable) {
			// We can get the lock!
			eprintk("OSPRDIOCTRYACQUIRE L w, %d,  attempt to acquire spin lock...\n", current->pid);
			osp_spin_lock(&(d->mutex));

			eprintk("OSPRDIOCTRYACQUIRE L w, %d,  got spin lock..., write_lock = %d\n", current->pid,d->write_lock );
			if ((d->write_lock == 0) && (d->read_locks == 0)) {

				eprintk("tryacquire CONDITIOn is true, %d\n", current->pid);
				// add ourselves to the write list
				d->write_lock = 1; 
				d->write_lock_proc = current->pid;

				// final settings(we acquired lock): 
				//d->ticket_head = next_valid_ticket(&(d->invalid_tickets), d->ticket_head+1);
				osp_spin_unlock(&(d->mutex));
				filp->f_flags |= F_OSPRD_LOCKED;
				r = 0;
			} 
			else {
				eprintk("L write return EBUSY, %d\n", current->pid);
				r = -EBUSY;
				osp_spin_unlock(&(d->mutex));
			}				
		}
		else {
			// We can get the lock!
			eprintk("OSPRDIOCACQUIRE L r, %d,  attempt to acquire spin lock...\n", current->pid);
			osp_spin_lock(&(d->mutex));

			eprintk("OSPRDIOCACQUIRE L r, %d,  got spin lock...write_lock = %d\n", current->pid,d->write_lock );
		
			if (d->write_lock == 0) {
					
				// add ourselves to the read list
				d->read_locks++; 
				eprintk("BEFORE read_list:\n");
				
				struct pid_list *list = &(d->read_lock_procs);
				while( list != NULL){
					eprintk("%d - > ", list->pid);
					list = list->next;
				}
				add_to_read(&(d->read_lock_procs), current->pid);

				// final settings(we acquired lock): 
				//d->ticket_head = next_valid_ticket(&(d->invalid_tickets), d->ticket_head+1);
				
				filp->f_flags |= F_OSPRD_LOCKED;
				osp_spin_unlock(&(d->mutex));				
				r = 0;
			}
			else {
				eprintk("L read return EBUSY, %d\n", current->pid);
				
				osp_spin_unlock(&(d->mutex));
				if ( filp->f_flags & F_OSPRD_LOCKED)
					eprintk("ebusy is locked\n");
				r = -EBUSY;
			}		
		}


	} else if (cmd == OSPRDIOCRELEASE) {

		// DONE EXERCISE: Unlock the ramdisk.
		//
		// If the file hasn't locked the ramdisk, return -EINVAL.
		// Otherwise, clear the lock from filp->f_flags, wake up
		// the wait queue, perform any additional accounting steps
		// you need, and return 0.

		// Your code here (instead of the next line).
		// r = -ENOTTY;
		//eprintk("Releasing...ioctl\n");
		eprintk("entering RELEASE case... pid == %d\n",current->pid);
		//if (filp->f_flags != (filp->f_flags | F_OSPRD_LOCKED))
		if (!(filp->f_flags & F_OSPRD_LOCKED))
			r = -EINVAL;
		else {
			//clear the lock bit
			filp->f_flags &= ~F_OSPRD_LOCKED;
			osp_spin_lock(&(d->mutex));
			if(filp_writable) {
				d->write_lock_proc = -1;
				d->write_lock = 0;
			}
			else {

				eprintk("from OSPRDIOCRELEASE..., before\n");
				struct pid_list * li = &(d->read_lock_procs);
				while(li->next !=NULL){
					eprintk("%d -> ", li->pid);
					li = li->next;
				}
				remove_from_read(&(d->read_lock_procs), current->pid);
				d->read_locks--;
				
				li = &(d->read_lock_procs);
				while(li->next !=NULL){
					eprintk("%d -> ", li->pid);
					li = li->next;
				}
			}
			// wake up tasks in wait queue:
			wake_up_all(&(d->blockq));
			osp_spin_unlock(&(d->mutex));
			r = 0;
		}

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
	d->read_locks = 0;
	d->write_lock = 0;
	d->invalid_tickets.next = NULL;
	d->invalid_tickets.num = -1;
	d->read_lock_procs.next = NULL;
	d->read_lock_procs.pid = -1;
	d->write_lock_proc = -1;
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
