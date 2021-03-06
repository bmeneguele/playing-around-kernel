/*
 * Copyright (c) 2017 Bruno E. O. Meneguele <bmeneguele@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License version 2 as published by the
 * Free Software Foundation.
 */

/* __init/exit, macros (MODULE_*) that initializes the module itself */
#include <linux/module.h>
/* Printing function definitions */
#include <linux/kernel.h>
/* RCU enabled linked list */
#include <linux/rculist.h>
/* Things related to memory allocation, e.g. kmalloc */
#include <linux/slab.h>
/* Kobject related stuff, here used to create sysfs interface */
#include <linux/kobject.h>
/* Kernel specific string manipulation library */
#include <linux/string.h>
/* Spinlocks related stuff for synchronization */
#include <linux/spinlock.h>
/* Timer related stuff for linked list node removal simulation */
#include <linux/timer.h>

/* Utilities file. For now there are only printing helper functions */
#include "utils.h"

/* A way to avoid bufferoverflow is using predefined array sizes */
#define DOG_ENTRY_NBYTES 64

/* Dummy structure to examplify the linked list and rcu behaviour */
struct dog {
	/* Field used as link point between struct dog and the linked list
	 * itself */
	struct list_head list;
	/* Field used by RCU mechanism to track structures awaiting grace
	 * periods */
	struct rcu_head rh;
	char *breed;
	int age; /* in months */
	bool training_easy;
};

/*
 * Threads that updates the linked-list somehow (inserting/removing nodes) need
 * be controlled with locks, to avoid overruns and race conditions on the linked
 * list between these threads (updaters). This is a different situation when
 * compared to updaters vs readers race condition, which was solved using a
 * lockless approach, named RCU (read-copy-update).
 *
 * Spin locks have less overhead than semaphores/mutexes, considering that the
 * thread won't sleep while trying to acquire the lock, because of that it was
 * used here. But as they are named, spinlocks busy-waits for the critical
 * section, so it should be used only in situations where the critical section
 * is released really fast.
 */
DEFINE_SPINLOCK(list_update_lock);

/* The node that represents the head of the linked list used in this module */
LIST_HEAD(dog_list);

/* A simple way to maintain the number of elements in the linked list. It must
 * be manually updated in every insertion/removal */
static size_t dog_list_size;

/*
 * Function called everytime the sysfs attribute file is read.
 * Example: cat /sys/rcu-linked-list/dog
 */
static ssize_t dog_attr_show(struct kobject *kobj, struct kobj_attribute *attr,
			     char *buf)
{
	struct dog *entry;
	size_t nbytes = 0;

	PR_DEBUG("show requested\n");
	/* Where RCU read-side critical section starts */
	rcu_read_lock();
	/* Copy directly to *buf, which is the output buffer */
	list_for_each_entry_rcu(entry, &dog_list, list) {
		nbytes += snprintf(&buf[nbytes], DOG_ENTRY_NBYTES,
				   "%s %d %s\n", entry->breed, entry->age,
				   entry->training_easy ? "true" : "false");
	}
	/* Where RCU read-side critical section ends */
	rcu_read_unlock();
	return nbytes;
}

/*
 * Function called everytime the sysfs attribute file is written.
 * Example: echo Golden,3,0 > /sys/rcu-linked-list/dog
 */
static ssize_t dog_attr_store(struct kobject *kobj, struct kobj_attribute *attr,
			      const char *buf, size_t count)
{
	struct dog *entry;
	char *str_token, *ibuf, *dog_attr[3];
	unsigned int idx = 0;
	int dog_age, dog_training;
	int err;
	unsigned long irq_flags;

	PR_DEBUG("store requested\n");

	/* User input handling code (basicaly string manipulation) */
	ibuf = kstrndup(buf, DOG_ENTRY_NBYTES, GFP_KERNEL);
	if (!ibuf)
		return -ENOMEM;
	while ((str_token = strsep(&ibuf, ",")) != NULL)
		dog_attr[idx++] = str_token;
	if (idx < 3)
		return -EINVAL;

	err = kstrtoint(dog_attr[1], 10, &dog_age);
	if (err)
		return err;
	err = kstrtoint(dog_attr[2], 2, &dog_training);
	if (err)
		return err;

	/* New dog_list entry being created and assigned */
	entry = (struct dog *) kmalloc(sizeof(struct dog), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;

	entry->breed = dog_attr[0];
	entry->age = dog_age;
	entry->training_easy = dog_training ? true : false;
	/* Spinlock to allow only one updater thread a time. But pay attention,
	 * the deletion code to this same data structure is running concurrently
	 * in interrupt context (from a timer callback funtions), hence the
	 * spinlock should disable IRQs in this CPU specifically, avoiding a
	 * deadlock. Locks, in general, acts in a per-CPU manner, since there is
	 * no problem a task (IRQ or not) to be locked in a different CPU
	 * waiting a lock to be released, it doesn't feature a _deadlock_.
	 * All that said, a call to spin_lock_irqsave()/spin_unlock_irqrestore()
	 * will be made, instead the normal spin_lock()/spin_unlock() function
	 */
	spin_lock_irqsave(&list_update_lock, irq_flags);
	list_add_tail_rcu(&entry->list, &dog_list);
	dog_list_size++;
	spin_unlock_irqrestore(&list_update_lock, irq_flags);

	PR_DEBUG("%s %d %s\n", dog_attr[0], dog_age,
		 dog_training ? "true" : "false");
	return count;
}

/* Top-level kobject, which represents a directory within sysfs */
static struct kobject *dog_kobj;

/* sysfs attribute file definition: name, permition, read and write callbacks */
static struct kobj_attribute dog_attribute = __ATTR(dog, 0664, &dog_attr_show,
						    &dog_attr_store);

/* List of all sysfs attribute files that should be created on module init */
static struct attribute *attrs[] = {
	&dog_attribute.attr,
	NULL,
};

/* sysfs arrange attributes (files) in groups */
static struct attribute_group attr_group = {
	.attrs = attrs,
};

/* Timer structure that will handle linked list nodes remocal */
static struct timer_list removal_timer;

/*
 * This function is called in interrupt context, thus this need to be fast and
 * must not sleep. If needed, spinlocks is OK for this situation since they
 * don't sleep.
 */
static void timer_remove_dog(unsigned long data)
{
	struct dog *entry;

	/* It isn't necessary to control the removal process because there isn't
	 * any other thread executing this function. It'll be reexecuted just
	 * after mod_timer() is called and than reenabling the timer for the
	 * next interruption. */
	if (!list_empty(&dog_list)) {
		/* Any update (insertion/deletion) must be controlled in such a
		 * way that just one thread traverses the list at a time. And
		 * considering the control of IRQ vs process context lock
		 * sharing issue was handled in reader's code,
		 * spin_lock()/spin_unlock() calls can be made here. */
		spin_lock(&list_update_lock);
		entry = list_first_entry(&dog_list, struct dog, list);
		/* Delete dog entry following RCU mechanism */
		list_del_rcu(&entry->list);
		PR_DEBUG("entry deleted: %s,%d,%s\n", entry->breed, entry->age,
			 entry->training_easy ? "true" : "false");
		/* Wait all RCU readers left their read-side critical sections
		 * and free the old entry memory. This function abrevs the use
		 * of call_rcu(), a dummy callback just for kfree() the element
		 * and a rcu_barrier() in module's __exit for any additional
		 * callbacks */
		kfree_rcu(entry, rh);
		spin_unlock(&list_update_lock);
	}
	/* Reassign timer's expiration time */
	mod_timer(&removal_timer, jiffies + msecs_to_jiffies(5000));
}

static int __init rcu_linked_list_init(void)
{
	int err;

	spin_lock_init(&list_update_lock);

	/* Create and add a kobject dentry (directory entry) in sysfs */
	dog_kobj = kobject_create_and_add("rcu-linked-list", NULL);
	if (!dog_kobj) {
		 err = -ENOMEM;
		 goto timer_cleanup;
	}
	/* Add all attributes (files) inside the dentry previously created */
	err = sysfs_create_group(dog_kobj, &attr_group);
	if (err)
		goto sysfs_cleanup;

	/* Removal timer setup and initialization */
	setup_timer(&removal_timer, timer_remove_dog, 0);
	/* Set expiration time for 5 seconds from now */
	mod_timer(&removal_timer, jiffies + msecs_to_jiffies(5000));

	PR_DEBUG("module loaded\n");
	return 0;

sysfs_cleanup:
	/* Decrement dentry reference counter in case of error */
	kobject_put(dog_kobj);
timer_cleanup:
	/* Destroy timer and spins until its handler finishes (case running) */
	del_timer_sync(&removal_timer);
	return err;
}

static void __exit rcu_linked_list_exit(void)
{
	/* Destroy timer and spins until its handler finishes (case running) */
	del_timer_sync(&removal_timer);

	/* Decrement kobject dentry reference counter when exiting the module.
	 * In this way the kernel can safely free the memory used by the
	 * kobject. */
	kobject_put(dog_kobj);
	PR_DEBUG("module unloaded\n");
}

module_init(rcu_linked_list_init);
module_exit(rcu_linked_list_exit);

MODULE_AUTHOR("Bruno E. O. Meneguele <bmeneguele@gmail.com>");
MODULE_DESCRIPTION("RCU mechanism over linked lists");
MODULE_LICENSE("GPL");
