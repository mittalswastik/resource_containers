//////////////////////////////////////////////////////////////////////
//                      North Carolina State University
//
//
//
//                             Copyright 2016
//
////////////////////////////////////////////////////////////////////////
//
// This program is free software; you can redistribute it and/or modify it
// under the terms and conditions of the GNU General Public License,
// version 2, as published by the Free Software Foundation.
//
// This program is distributed in the hope it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
// more details.
//
// You should have received a copy of the GNU General Public License along with
// this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
//
////////////////////////////////////////////////////////////////////////
//
//   Author:  Hung-Wei Tseng, Yu-Chia Liu
//
//   Description:
//     Core of Kernel Module for Processor Container
//
////////////////////////////////////////////////////////////////////////

#include "processor_container.h"

#include <asm/uaccess.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/poll.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/kthread.h>

// libraries added

#include <linux/list.h>
#include <linux/types.h>

/*

    Idea

    We need to implement a data structure to handle the contaianers and also the task within those containers

    We plan on creating a hash table with key entry as the container id and each entry having a list of tasks
    These task would be pointing to next task within the same container and last one points to first also each task has a pointer to its
    previous task and first one also points to the last (doubly circular linked list).
    We create hash table for container because it has continuos id - 0,1,2... which are not random hence we do not need to 
    worry aboout creating a hash function. Whereas for task within a id we have pid(process id) which is of random form hence
    difficult to manipulate hash key also when hash table would be dynamically increasing and decreasing

    Create

    Each create would either add a task to the list or resize the table to add a container
    As hash map searches in O(1) and link list adds as well in O(1) hence new task create for existing
    container id is O(1). For new container id we first need to resize the hash table

    [cid1]  -> task1 -> task2 ->task3 -> task4 ->task1
            also task4 <- task1 <- task2 <- task3 <- task4

    [cid2]
    
    [cid3]
    
    [cid4]    

    Here we are setting default containers to 10000, Will use krealloc to reallocate the array size to get new containers range

    Delete

    Every delete would mean to delete the currently running task of the container which is maintained by cur pointer hence delete
    is O(1).

    Switch

    Switch is called every 5ms as user library uses SIG system call to ask kernel to switch

    We have a current task in each resource container running. So whenever a switch is called We schedule next task of the same resource container
    hence we first need to identify the container.

*/

struct task_info {
    long long int cid;
    pid_t tid;
    struct task_info *next;
    struct task_info *prev;
    struct task_struct *taskinlist;         // pointing to its corresponding structure in kernel run queue
};

struct container_info {
    struct task_info *head;
    struct task_info *foot;
    struct task_info *cur;
};

struct container_info *containers[10000];

struct mutex lockproc; 

DEFINE_MUTEX(lockproc);

/**
 * Delete the task in the container.
 * 
 * external functions needed:
 * mutex_lock(), mutex_unlock(), wake_up_process(), 
 */

int processor_container_delete(struct processor_container_cmd __user *user_cmd)
{
    mutex_lock(&lockproc);

    struct processor_container_cmd *temp;

    temp = kmalloc(sizeof(struct processor_container_cmd), GFP_KERNEL);

    copy_from_user(temp,user_cmd, sizeof(struct processor_container_cmd));

    printk(KERN_INFO "to delete task with cid %llu", temp->cid);

    long long int x = temp->cid;

    if(containers[x]->cur == containers[x]->cur->next) {    // first and the last task
        printk(KERN_INFO "deleting the last task of container %llu",x);
        // only one task left within the container
        kfree(containers[x]->cur);
        containers[x]->head = NULL;
        containers[x]->foot = NULL;
        containers[x]->cur = NULL;
    }

    else if(containers[x]->cur == containers[x]->head) {    // first task but not the only
        printk(KERN_INFO "deleting task, the first, with tid %d from container %llu",containers[x]->cur->tid,x);

        containers[x]->head = containers[x]->head->next;
        containers[x]->foot->next = containers[x]->head;
        containers[x]->head->prev = containers[x]->foot;
        kfree(containers[x]->cur);
        containers[x]->cur = containers[x]->head;
        printk("waking up the next task %d", containers[x]->cur->tid);
        wake_up_process(containers[x]->cur->taskinlist);
    }

    else if(containers[x]->cur == containers[x]->foot) {
        // deleting the last task
        printk(KERN_INFO "deleting the rightmost task in the list, tid %d from container %llu",containers[x]->cur->tid,x);

        containers[x]->foot = containers[x]->foot->prev;
        containers[x]->head->prev = containers[x]->foot;
        containers[x]->foot->next = containers[x]->head;
        kfree(containers[x]->cur);
        containers[x]->cur = containers[x]->head;
        printk("waking up the next task %d", containers[x]->cur->tid);
        wake_up_process(containers[x]->cur->taskinlist);
    }

    else {
        // deleting any other task

        printk(KERN_INFO "deleting the task with tid %d from container %llu",containers[x]->cur->tid,x);

        containers[x]->cur->prev->next = containers[x]->cur->next;
        containers[x]->cur->next->prev = containers[x]->cur->prev;
        struct task_info *del;
        del = containers[x]->cur;
        containers[x]->cur = containers[x]->cur->next;
        kfree(del);
        printk("waking up the next task %d", containers[x]->cur->tid);
        wake_up_process(containers[x]->cur->taskinlist);
    }

    mutex_unlock(&lockproc);

    return 0;
}

/**
 * Create a task in the corresponding container.
 * external functions needed:
 * copy_from_user(), mutex_lock(), mutex_unlock(), set_current_state(), schedule()
 * 
 * external variables needed:
 * struct task_struct* current  
 */

int processor_container_create(struct processor_container_cmd __user *user_cmd)
{

    mutex_lock(&lockproc);

    struct processor_container_cmd *temp;    

    temp = kmalloc(sizeof(struct processor_container_cmd), GFP_KERNEL);

    copy_from_user(temp,user_cmd, sizeof(struct processor_container_cmd));

    printk(KERN_INFO "to add task with cid %llu", temp->cid);

    struct task_info *task = kmalloc(sizeof(struct task_info), GFP_KERNEL);

    struct task_struct *currenttask = current;     // can directly use current->pid though.

    long long int x = temp->cid;    // check if _u64 = long long int

    if(containers[x] == NULL) {

            printk(KERN_INFO "new container");
            printk(KERN_INFO "creating task with id %d", currenttask->pid);
            task->cid = x;
            task->tid = currenttask->pid;
            task->taskinlist = currenttask;

            struct container_info *container = kmalloc(sizeof(struct container_info), GFP_KERNEL);

            containers[x] = container;

            containers[x]->head = task;
            containers[x]->foot = containers[x]->head;
            containers[x]->foot->next = containers[x]->head;
            containers[x]->head->prev = containers[x]->foot;
            containers[x]->cur = containers[x]->head;

            mutex_unlock(&lockproc);
        }

    else if (containers[x]->head == NULL) {
        // this is when a container has deleted all task which were created, then next task added as 1st task
        printk(KERN_INFO "container already here");
        printk(KERN_INFO "new first task of container, task id %d", currenttask->pid);
        task->cid = x;
        task->tid = currenttask->pid;
        task->taskinlist = currenttask;

        containers[x]->head = task;
        containers[x]->foot = containers[x]->head;
        containers[x]->foot->next = containers[x]->head;
        containers[x]->head->prev = containers[x]->head;
        containers[x]->cur = containers[x]->head;

        mutex_unlock(&lockproc);

    }    

    else {

        printk(KERN_INFO "container already here");
        printk(KERN_INFO "creating task with id %d", currenttask->pid);
        task->cid = x;
        task->tid = currenttask->pid;
        task->taskinlist = currenttask;

        containers[x]->foot->next = task;
        containers[x]->foot->next->prev = containers[x]->foot;
        containers[x]->foot = task;
        containers[x]->foot->next = containers[x]->head;
        containers[x]->head->prev = containers[x]->foot;

        mutex_unlock(&lockproc);
        set_current_state(TASK_INTERRUPTIBLE);
        schedule();
    }
    
    return 0;
}

/**
 * switch to the next task in the next container
 * 
 * external functions needed:
 * mutex_lock(), mutex_unlock(), wake_up_process(), set_current_state(), schedule()
 */
int processor_container_switch(struct processor_container_cmd __user *user_cmd)
{

    mutex_lock(&lockproc);

    long long int counter;
    long long int i;

    for(i = 0 ; i < 10000 ; i++) {
        if(containers[i] != NULL) {

            if(containers[i]->cur != NULL) {
                if(containers[i]->cur->tid == current->pid) {
                    counter = i;
                    break;
                }
            }
        }
    }

    // there will be a counter value in the end for sure because there has to be some container running that task

    printk(KERN_INFO "task provided by container %llu ",counter);

    if(containers[counter]->cur == containers[counter]->cur->next){
        printk(KERN_INFO "scheduling %d the same, not putting process id %d in container %llu to sleep",containers[counter]->cur->next->tid,containers[counter]->cur->tid,counter);
        mutex_unlock(&lockproc); 
    }

    else {

        printk(KERN_INFO "scheduling %d and putting process id %d in container %llu to sleep",containers[counter]->cur->next->tid,containers[counter]->cur->tid,counter);
        
        containers[counter]->cur = containers[counter]->cur->next;
    
        wake_up_process(containers[counter]->cur->taskinlist);

        mutex_unlock(&lockproc);

        set_current_state(TASK_INTERRUPTIBLE);

        schedule();
    }

    return 0;
}

/**
 * control function that receive the command in user space and pass arguments to
 * corresponding functions.
 */
int processor_container_ioctl(struct file *filp, unsigned int cmd,
                              unsigned long arg)
{
    switch (cmd)
    {
    case PCONTAINER_IOCTL_CSWITCH:
        return processor_container_switch((void __user *)arg);
    case PCONTAINER_IOCTL_CREATE:
        return processor_container_create((void __user *)arg);
    case PCONTAINER_IOCTL_DELETE:
        return processor_container_delete((void __user *)arg);
    default:
        return -ENOTTY;
    }
}


// realloc

// (void*) tasks = krealloc((void *) tasks, (sizeof(tasks)+sizeof(struct task_info)), GFP_KERNEL);