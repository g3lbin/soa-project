/**
 * @file mdf.h
 * @brief This header file defines the macros and data structures used by
 *        the multi-flow device driver. Some helper functions are also defined.
 *
 * @author Cristiano Cuffaro
 * 
 * @date March 12, 2022
 */

#ifndef _MFD_H
#define _MFD_H

#define DEV_ENABLED     (1)     /* operating status of the enabled device */

#define WQ_NAME_LENGTH  (24)    /* same as WQ_NAME_LEN which is not exported */

#define STREAMS_NUM     (2)     /* number of different data flows */
#define LOW_PRIORITY    (0)     /* index for low priority stream */
#define HIGH_PRIORITY   (1)     /* index for high priority stream */

#define MAX_STREAM_SIZE         (4 * PAGE_SIZE) /* max size of each stream */
#define MAX_WAIT_TIMEINT        (LONG_MAX / HZ) /* infinite time in seconds */

#define CHARP_ENTRY_SIZE        (32)    /* size of each entry of an array */
                                        /* of charp parameters            */

/*
 * The following are macros that make it easier to access or modify
 * some fields of structures.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 0, 0)
#define get_major(session) MAJOR(session->f_inode->i_rdev)
#define get_minor(session) MINOR(session->f_inode->i_rdev)
#else
#define get_major(session) MAJOR(session->f_dentry->d_inode->i_rdev)
#define get_minor(session) MINOR(session->f_dentry->d_inode->i_rdev)
#endif

#define get_stream_len(x, p)            \
        atomic_read((atomic_t *)&(x)->valid_b[p])
#define add_stream_len(n, x, p)         \
        atomic_add(n, (atomic_t *)&(x)->valid_b[p])
#define sub_stream_len(n, x, p)         \
        atomic_sub(n, (atomic_t *)&(x)->valid_b[p])

#define get_free_space(x, p)            \
        atomic_read((atomic_t *)&(x)->free_b[p])
#define add_free_space(n, x, p)         \
        atomic_add(n, (atomic_t *)&(x)->free_b[p])
#define sub_free_space(n, x, p)         \
        atomic_sub(n, (atomic_t *)&(x)->free_b[p])

#define get_waiting_threads(x, p)       \
        atomic_read(&(x)->waiting_for_data[p])
#define inc_waiting_threads(x, p)       \
        atomic_inc(&(x)->waiting_for_data[p])
#define dec_waiting_threads(x, p)       \
        atomic_dec(&(x)->waiting_for_data[p])

/**
 * remove_list_item - removes an item from list_head's list
 * @item: the actual item
 */
#define remove_list_item(item)                  \
        do {                                    \
                item.prev->next = item.next;    \
                item.next->prev = item.prev;    \
        } while(0)

/**
 * lock_and_check - tries to take the lock and checks the condition
 * @condition: a C expression for the event to wait for
 * @mutexp: the pointer to the mutex to be locked
 * 
 * Returns:
 * 0 if it fails to take the lock or @condition is evaluated as %false,
 * 1 if the @condition is evaluated as %true.
 * In the second case, the mutex remains acquired.
 */
#define lock_and_check(condition, mutexp)       \
({                                              \
        int __ret = 0;                          \
        if (mutex_trylock(mutexp)) {            \
                if (condition)                  \
                        __ret = 1;              \
                else                            \
                        mutex_unlock(mutexp);   \
        }                                       \
        __ret;                                  \
})

/**
 * mfd_module_param_array_named - renamed parameter which is an array of some type
 * @name: a valid C identifier which is the parameter name
 * @array: the name of the array variable
 * @type: the type of each entry
 * @nump: optional pointer filled in with the number written
 * @perm: visibility in sysfs
 *
 * This is a specific reimplementation of the module_param_array_named() which
 * permits to re-define the param_ops_##type and the param_array_ops, in order to
 * satisfy the project' specification.
 */
#define mfd_module_param_array_named(name, array, type, nump, perm)     \
	param_check_##type(name, &((char **)array)[0]);                 \
	static const struct kparam_array __param_arr_##name             \
	= {     .max = ARRAY_SIZE(array), .num = nump,                  \
	        .ops = &mfd_param_ops_##type,                           \
	        .elemsize = sizeof(array[0]), .elem = array };          \
	__module_param_call(MODULE_PARAM_PREFIX, name,                  \
			    &mfd_param_array_ops,                       \
			    .arr = &__param_arr_##name,                 \
			    perm, -1, 0);                               \
	__MODULE_PARM_TYPE(name, "array of " #type)

extern const struct kernel_param_ops mfd_param_ops_charp;
extern const struct kernel_param_ops mfd_param_array_ops;

/**
 * data_segment - represents a data segment
 * @data: the actual segment data
 * @size: the size of the data
 * @pos: the position of the next byte to read
 * @list: head for the list of data segments
 */
struct data_segment {
        char *data;
        unsigned long size;
        unsigned long pos;
        struct list_head list;
} __attribute__((packed));

/**
 * segment_list - a list of data segments that constitutes a data stream
 * @head: dummy element that constitutes the head of the list
 * @tail: dummy element that constitutes the tail of the list
 * 
 * To fill the list just connect the list_head structures contained
 * in the data segments as predecessors of @tail.
 */
struct segment_list {
        struct list_head head;
        struct list_head tail;
};

/**
 * device_struct - keeps the multi-flow device file informations
 * @busy: mutex used for single session operative mode
 * @waitq: waitqueues for blocking read and write operations
 * @wr_workq: workqueues for asynchronous execution of low priority writes
 * @sync: operation synchronizer of each stream
 * @streams: device data streams
 * @valid_b: valid bytes (readables) of each stream
 * @free_b: free space to execute write operations
 * @waiting_for_data: number of threads currently waiting for data along the streams
 */
struct device_struct {
#ifdef SINGLE_SESSION_OBJECT
        struct mutex            busy;
#endif
        wait_queue_head_t       waitq[STREAMS_NUM];
        struct workqueue_struct *wr_workq;
        struct mutex            sync[STREAMS_NUM];
        struct segment_list     streams[STREAMS_NUM];
        int                     valid_b[STREAMS_NUM];
        int                     free_b[STREAMS_NUM];
        atomic_t                waiting_for_data[STREAMS_NUM];
} __randomize_layout;

/**
 * session_data - the data associated with the I/O session
 * @current_priority: priority level (high or low) for the operations
 * @timeout: timeout interval (in jiffies) to break the wait of blocking ops
 */
struct session_data {
        short   current_priority;
        long    timeout;
};

/**
 * packed_write - keeps the information observable from within the deferred work
 * @the_work: the deferred work
 * @minor: minor number of the multi-flow device file
 * @buf: the kernel buffer which contains the source data
 * @count: number of bytes to write
 * @real_write: pointer to actual write function
 */
struct packed_write {
        struct work_struct      the_work;
        int                     minor;
        struct data_segment     *seg;
        void                    (*real_write)(short prio, int minor,
                                                struct data_segment *new);
};

/**
 * to_packed_write - retrieves the external struct that embeds the work_struct
 * @work: workqueue item
 * 
 * Returns a pointer to packed_write structure.
 */
static inline struct packed_write *to_packed_write(struct work_struct *work)
{
	return container_of(work, struct packed_write, the_work);
}

/**
 * alloc_data_segment - allocates memory for a data segment
 * @segp: the pointer to the address of the data segment
 * @data_size: the size of the segment data
 * @mask: GFP flag combination
 * 
 * Returns 0 if the operation is successful, otherwise a negative value.
 */
static inline int alloc_data_segment(struct data_segment **segp, long data_size, gfp_t mask)
{
        *segp = kzalloc(sizeof(struct data_segment) + data_size, mask);
        if (*segp == NULL)
                return -ENOMEM;
        (*segp)->data = (char *)*segp + sizeof(struct data_segment);
        (*segp)->size = data_size;
        return 0;
}

/**
 * free_data_segment - frees the memory occupied by a data segment
 * @seg: the address of the data segment
 */
static __always_inline void free_data_segment(struct data_segment *seg)
{
        kfree(seg);
}

/**
 * alloc_packed_write - allocates memory for a struct packed_write
 * @pck_wrp: the pointer to the address of the struct
 * @mask: GFP flag combination
 * 
 * Returns 0 if the operation is successful, otherwise a negative value.
 */
static inline int alloc_packed_write(struct packed_write **pck_wrp, gfp_t mask)
{
        *pck_wrp = kzalloc(sizeof(struct packed_write), mask);
        if (!(*pck_wrp))
                return -ENOMEM;
        return 0;
}

/**
 * free_packed_write - frees the memory occupied by a struct packed_write
 * @pck_wr: the address of the struct packed_write
 */
static __always_inline void free_packed_write(struct packed_write *pck_wr)
{
        kfree(pck_wr);
}

/**
 * get_priority_str - provides the string corresponding to the priority level
 * 
 * This function is useful for debug prints.
 * 
 * Returns the actual string.
 */
static inline const char *get_priority_str(short prio)
{
        return (prio == LOW_PRIORITY) ? "low priority" : "high priority";
}

#endif