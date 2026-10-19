/**************************************************************************************************************
 * altobeam RTOS WSM host interface (HI) implementation
 *
 * Copyright (c) 2018, altobeam.inc   All rights reserved.
 *
 *  The source code contains proprietary information of AltoBeam, and shall not be distributed, 
 *  copied, reproduced, or disclosed in whole or in part without prior written permission of AltoBeam.
*****************************************************************************************************************/

#include "atbm_hal.h"


static void atbm_ThreadCallback(void *arg){
	atbm_thread_internal_t *thread = (atbm_thread_internal_t *)arg;
	pAtbm_thread_t self_thread;
	int ret;

	wifi_printk(WIFI_ALWAYS, "Thread %s %d start\n", thread->name, thread->wake_period);
	if(thread->wake_period){
		while (!thread->stop)
		{	
			do{
				if(atbm_atomic_read(&thread->count) != 0)
					break;
				ret = atbm_os_wait_event_timeout(&thread->wq, thread->wake_period);
			}while(0);
			atbm_atomic_set(&thread->count, 0);
			if(thread->cb){
				thread->cb(thread->p_arg, ret);
			}
		}
	}else{
		if(thread->cb){
			thread->cb(thread->p_arg, 0);
		}
	}
	self_thread = thread->thread;
	wifi_printk(WIFI_ALWAYS, "Thread %s stop\n", thread->name);
	/* Signal completion, then park until the stopping task deletes us. */
	atbm_os_wakeup_event(&thread->stopEv);
	atbm_ThreadStopEvent(self_thread);
}

atbm_thread_internal_t *atbm_createThreadInternal(char *name,
							thread_callback task,
							atbm_void *p_arg,
							int prio,
							int wake_period){
	atbm_thread_internal_t *thread = atbm_kmalloc(sizeof(atbm_thread_internal_t), GFP_KERNEL);
	if(!thread)
		return ATBM_NULL;
	
	thread->cb = task;
	thread->p_arg = p_arg;
	thread->stop = 0;
	thread->wake_period = wake_period;
	thread->name = name;
	atbm_atomic_set(&thread->count, 0);

	if(wake_period){
		atbm_os_init_waitevent(&thread->wq);
	}

	atbm_os_init_waitevent(&thread->stopEv);	
	thread->thread = atbm_createThread(name,atbm_ThreadCallback,(atbm_void*)thread,prio);

	if(!thread->thread){
		atbm_kfree(thread);
		return ATBM_NULL;
	}
	return thread;
}

int atbm_stopThreadInternal(atbm_thread_internal_t *thread){
	int wait_ret;

	wifi_printk(WIFI_ALWAYS,
		    "[ATBM_STOP_DBG] stop request name=%s ctrl=%p task=%p wake=%d\n",
		    thread->name, thread, thread->thread, thread->wake_period);
	thread->stop = 1;
	if(thread->wake_period){
		atbm_os_wakeup_event(&thread->wq);
	}
	wait_ret = atbm_os_wait_event_timeout(&thread->stopEv, 10*ATBM_HZ);
	wifi_printk(WIFI_ALWAYS,
		    "[ATBM_STOP_DBG] stop event name=%s ret=%d task=%p\n",
		    thread->name, wait_ret, thread->thread);

	/* The worker is parked after signaling stopEv. */
	wifi_printk(WIFI_ALWAYS,
		    "[ATBM_STOP_DBG] task delete begin name=%s task=%p\n",
		    thread->name, thread->thread);
	atbm_stopThread(thread->thread);
	wifi_printk(WIFI_ALWAYS,
		    "[ATBM_STOP_DBG] task delete done name=%s\n",
		    thread->name);
	wifi_printk(WIFI_ALWAYS,
		    "[ATBM_STOP_DBG] stopEv delete begin name=%s event=%p\n",
		    thread->name, thread->stopEv);
	atbm_os_delete_waitevent(&thread->stopEv);
	wifi_printk(WIFI_ALWAYS,
		    "[ATBM_STOP_DBG] stopEv delete done name=%s\n",
		    thread->name);
	if(thread->wake_period){
		wifi_printk(WIFI_ALWAYS,
			    "[ATBM_STOP_DBG] wq delete begin name=%s event=%p\n",
			    thread->name, thread->wq);
		atbm_os_delete_waitevent(&thread->wq);
		wifi_printk(WIFI_ALWAYS,
			    "[ATBM_STOP_DBG] wq delete done name=%s\n",
			    thread->name);
	}

	wifi_printk(WIFI_ALWAYS,
		    "[ATBM_STOP_DBG] ctrl free name=%s ctrl=%p\n",
		    thread->name, thread);
	atbm_kfree(thread);
	return 0;
};


void atbm_wakeupThreadInternal(atbm_thread_internal_t *thread){
	if(thread->wake_period == 0){
		wifi_printk(WIFI_ALWAYS, "wake up %d error\n", thread->name);
		return;
	}
	if(atbm_atomic_add_return(1, &thread->count) == 1){
		atbm_os_wakeup_event(&thread->wq);
	}
};
