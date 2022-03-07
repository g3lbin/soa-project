/*
 * ioctl.h defines the command codes used to make IOCTL requests
 *
 * The IOCTL magic number and the command numbers were chosen according
 * to the following link:
 * https://www.kernel.org/doc/Documentation/ioctl/ioctl-number.txt
 * 
 * Note: to modify the O_NONBLOCK flag in filp->f_flags can also be used
 * the predefined command FIONBIO, but the IOC_SWITCH_BLOCKING is useful
 * for printk prints and makes the driver complete and independent.
 */
#ifndef _MFD_IOCTL_H
#define _MFD_IOCTL_H

#define IOC_MAGIC 'r'
#define IOC_SWITCH_PRIORITY     _IO(IOC_MAGIC, 0x20)            // switch the priority level (high or low) for the operations
#define IOC_SWITCH_BLOCKING     _IO(IOC_MAGIC, 0x21)            // switch to blocking or non-blocking read and write operations
#define IOC_SET_WAIT_TIMEINT    _IOW(IOC_MAGIC, 0x22, long *)   // set the waiting time interval of blocking operations

#endif