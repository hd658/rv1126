/*
 * The input core
 *
 * Copyright (c) 1999-2002 Vojtech Pavlik
 */

/*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 */

#define pr_fmt(fmt) KBUILD_BASENAME ": " fmt

#include <linux/init.h>
#include <linux/types.h>
#include <linux/idr.h>
#include <linux/input/mt.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/random.h>
#include <linux/major.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/poll.h>
#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include "input-compat.h"

MODULE_AUTHOR("Vojtech Pavlik <vojtech@suse.cz>");
MODULE_DESCRIPTION("Input core");
MODULE_LICENSE("GPL");

#define INPUT_MAX_CHAR_DEVICES		1024
#define INPUT_FIRST_DYNAMIC_DEV		256
static DEFINE_IDA(input_ida);

static LIST_HEAD(input_dev_list);
static LIST_HEAD(input_handler_list);

/*
 * input_mutex protects access to both input_dev_list and input_handler_list.
 * This also causes input_[un]register_device and input_[un]register_handler
 * be mutually exclusive which simplifies locking in drivers implementing
 * input handlers.
 * 
 *	input_mutex 保护对 input_dev_list 和 input_handler_list 的访问。
* 	这也使得 input_[un]register_device 和 input_[un]register_handler
* 	互斥，从而简化了实现输入处理程序的驱动程序中的锁逻辑。
 */
static DEFINE_MUTEX(input_mutex);

static const struct input_value input_value_sync = { EV_SYN, SYN_REPORT, 1 };

static inline int is_event_supported(unsigned int code,
				     unsigned long *bm, unsigned int max)
{
	return code <= max && test_bit(code, bm);
}

static int input_defuzz_abs_event(int value, int old_val, int fuzz)
{
	if (fuzz) {
		if (value > old_val - fuzz / 2 && value < old_val + fuzz / 2)
			return old_val;

		if (value > old_val - fuzz && value < old_val + fuzz)
			return (old_val * 3 + value) / 4;

		if (value > old_val - fuzz * 2 && value < old_val + fuzz * 2)
			return (old_val + value) / 2;
	}

	return value;
}

static void input_start_autorepeat(struct input_dev *dev, int code)
{
	if (test_bit(EV_REP, dev->evbit) &&
	    dev->rep[REP_PERIOD] && dev->rep[REP_DELAY] &&
	    dev->timer.function) {
		dev->repeat_key = code;
		mod_timer(&dev->timer,
			  jiffies + msecs_to_jiffies(dev->rep[REP_DELAY]));
	}
}

static void input_stop_autorepeat(struct input_dev *dev)
{
	del_timer(&dev->timer);
}

/*
 * Pass event first through all filters and then, if event has not been
 * filtered out, through all open handles. This function is called with
 * dev->event_lock held and interrupts disabled.
 * 
 * 
 * * 首先将事件通过所有过滤器传递，然后，如果事件未被过滤掉，
* 则通过所有打开的句柄传递。调用此函数时持有 dev->event_lock
* 并且中断已禁用。
 */
static unsigned int input_to_handler(struct input_handle *handle,
			struct input_value *vals, unsigned int count)
{
	struct input_handler *handler = handle->handler;
	struct input_value *end = vals;
	struct input_value *v;

	if (handler->filter) {
		for (v = vals; v != vals + count; v++) {
			if (handler->filter(handle, v->type, v->code, v->value))
				continue;
			if (end != v)
				*end = *v;
			end++;
		}
		count = end - vals;
	}

	if (!count)
		return 0;

	if (handler->events)
		handler->events(handle, vals, count);
	else if (handler->event)
		for (v = vals; v != vals + count; v++)
			handler->event(handle, v->type, v->code, v->value);

	return count;
}

/*
 * Pass values first through all filters and then, if event has not been
 * filtered out, through all open handles. This function is called with
 * dev->event_lock held and interrupts disabled.
 * 
 * * 首先将值通过所有过滤器传递，如果事件未被过滤掉，再通过所有打开的句柄传递。
*    调用此函数时持有 dev->event_lock 且中断已禁用。
 */
static void input_pass_values(struct input_dev *dev,
			      struct input_value *vals, unsigned int count)
{
	struct input_handle *handle;
	struct input_value *v;

	if (!count)
		return;

	rcu_read_lock();

	handle = rcu_dereference(dev->grab);
	if (handle) {
		count = input_to_handler(handle, vals, count);
	} else {
		list_for_each_entry_rcu(handle, &dev->h_list, d_node)
			if (handle->open) {
				count = input_to_handler(handle, vals, count);
				if (!count)
					break;
			}
	}

	rcu_read_unlock();

	/* trigger auto repeat for key events */
	if (test_bit(EV_REP, dev->evbit) && test_bit(EV_KEY, dev->evbit)) {
		for (v = vals; v != vals + count; v++) {
			if (v->type == EV_KEY && v->value != 2) {
				if (v->value)
					input_start_autorepeat(dev, v->code);
				else
					input_stop_autorepeat(dev);
			}
		}
	}
}

static void input_pass_event(struct input_dev *dev,
			     unsigned int type, unsigned int code, int value)
{
	struct input_value vals[] = { { type, code, value } };

	input_pass_values(dev, vals, ARRAY_SIZE(vals));
}

/*
 * Generate software autorepeat event. Note that we take
 * dev->event_lock here to avoid racing with input_event
 * which may cause keys get "stuck".
 */
static void input_repeat_key(struct timer_list *t)
{
	struct input_dev *dev = from_timer(dev, t, timer);
	unsigned long flags;

	spin_lock_irqsave(&dev->event_lock, flags);

	if (test_bit(dev->repeat_key, dev->key) &&
	    is_event_supported(dev->repeat_key, dev->keybit, KEY_MAX)) {
		struct input_value vals[] =  {
			{ EV_KEY, dev->repeat_key, 2 },
			input_value_sync
		};

		input_pass_values(dev, vals, ARRAY_SIZE(vals));

		if (dev->rep[REP_PERIOD])
			mod_timer(&dev->timer, jiffies +
					msecs_to_jiffies(dev->rep[REP_PERIOD]));
	}

	spin_unlock_irqrestore(&dev->event_lock, flags);
}

#define INPUT_IGNORE_EVENT	0
#define INPUT_PASS_TO_HANDLERS	1
#define INPUT_PASS_TO_DEVICE	2
#define INPUT_SLOT		4
#define INPUT_FLUSH		8
#define INPUT_PASS_TO_ALL	(INPUT_PASS_TO_HANDLERS | INPUT_PASS_TO_DEVICE)

static int input_handle_abs_event(struct input_dev *dev,
				  unsigned int code, int *pval)
{
	struct input_mt *mt = dev->mt;
	bool is_mt_event;
	int *pold;

	if (code == ABS_MT_SLOT) {
		/*
		 * "Stage" the event; we'll flush it later, when we
		 * get actual touch data.
		 */
		if (mt && *pval >= 0 && *pval < mt->num_slots)
			mt->slot = *pval;

		return INPUT_IGNORE_EVENT;
	}

	is_mt_event = input_is_mt_value(code);

	if (!is_mt_event) {
		pold = &dev->absinfo[code].value;
	} else if (mt) {
		pold = &mt->slots[mt->slot].abs[code - ABS_MT_FIRST];
	} else {
		/*
		 * Bypass filtering for multi-touch events when
		 * not employing slots.
		 */
		pold = NULL;
	}

	if (pold) {
		*pval = input_defuzz_abs_event(*pval, *pold,
						dev->absinfo[code].fuzz);
		if (*pold == *pval)
			return INPUT_IGNORE_EVENT;

		*pold = *pval;
	}

	/* Flush pending "slot" event */
	if (is_mt_event && mt && mt->slot != input_abs_get_val(dev, ABS_MT_SLOT)) {
		input_abs_set_val(dev, ABS_MT_SLOT, mt->slot);
		return INPUT_PASS_TO_HANDLERS | INPUT_SLOT;
	}

	return INPUT_PASS_TO_HANDLERS;
}

static int input_get_disposition(struct input_dev *dev,
			  unsigned int type, unsigned int code, int *pval)
{
	int disposition = INPUT_IGNORE_EVENT;
	int value = *pval;

	switch (type) {

	case EV_SYN:
		switch (code) {
		case SYN_CONFIG:
			disposition = INPUT_PASS_TO_ALL;
			break;

		case SYN_REPORT:
			disposition = INPUT_PASS_TO_HANDLERS | INPUT_FLUSH;
			break;
		case SYN_MT_REPORT:
			disposition = INPUT_PASS_TO_HANDLERS;
			break;
		}
		break;

	case EV_KEY:
		if (is_event_supported(code, dev->keybit, KEY_MAX)) {

			/* auto-repeat bypasses state updates */
			if (value == 2) {
				disposition = INPUT_PASS_TO_HANDLERS;
				break;
			}

			if (!!test_bit(code, dev->key) != !!value) {

				__change_bit(code, dev->key);
				disposition = INPUT_PASS_TO_HANDLERS;
			}
		}
		break;

	case EV_SW:
		if (is_event_supported(code, dev->swbit, SW_MAX) &&
		    !!test_bit(code, dev->sw) != !!value) {

			__change_bit(code, dev->sw);
			disposition = INPUT_PASS_TO_HANDLERS;
		}
		break;

	case EV_ABS:
		if (is_event_supported(code, dev->absbit, ABS_MAX))
			disposition = input_handle_abs_event(dev, code, &value);

		break;

	case EV_REL:
		if (is_event_supported(code, dev->relbit, REL_MAX) && value)
			disposition = INPUT_PASS_TO_HANDLERS;

		break;

	case EV_MSC:
		if (is_event_supported(code, dev->mscbit, MSC_MAX))
			disposition = INPUT_PASS_TO_ALL;

		break;

	case EV_LED:
		if (is_event_supported(code, dev->ledbit, LED_MAX) &&
		    !!test_bit(code, dev->led) != !!value) {

			__change_bit(code, dev->led);
			disposition = INPUT_PASS_TO_ALL;
		}
		break;

	case EV_SND:
		if (is_event_supported(code, dev->sndbit, SND_MAX)) {

			if (!!test_bit(code, dev->snd) != !!value)
				__change_bit(code, dev->snd);
			disposition = INPUT_PASS_TO_ALL;
		}
		break;

	case EV_REP:
		if (code <= REP_MAX && value >= 0 && dev->rep[code] != value) {
			dev->rep[code] = value;
			disposition = INPUT_PASS_TO_ALL;
		}
		break;

	case EV_FF:
		if (value >= 0)
			disposition = INPUT_PASS_TO_ALL;
		break;

	case EV_PWR:
		disposition = INPUT_PASS_TO_ALL;
		break;
	}

	*pval = value;
	return disposition;
}

static void input_handle_event(struct input_dev *dev,
			       unsigned int type, unsigned int code, int value)
{
	int disposition = input_get_disposition(dev, type, code, &value);

	if (disposition != INPUT_IGNORE_EVENT && type != EV_SYN)
		add_input_randomness(type, code, value);

	if ((disposition & INPUT_PASS_TO_DEVICE) && dev->event)
		dev->event(dev, type, code, value);

	if (!dev->vals)
		return;

	if (disposition & INPUT_PASS_TO_HANDLERS) {
		struct input_value *v;

		if (disposition & INPUT_SLOT) {
			v = &dev->vals[dev->num_vals++];
			v->type = EV_ABS;
			v->code = ABS_MT_SLOT;
			v->value = dev->mt->slot;
		}

		v = &dev->vals[dev->num_vals++];
		v->type = type;
		v->code = code;
		v->value = value;
	}

	if (disposition & INPUT_FLUSH) {
		if (dev->num_vals >= 2)
			input_pass_values(dev, dev->vals, dev->num_vals);
		dev->num_vals = 0;
		/*
		 * Reset the timestamp on flush so we won't end up
		 * with a stale one. Note we only need to reset the
		 * monolithic one as we use its presence when deciding
		 * whether to generate a synthetic timestamp.
		 */
		dev->timestamp[INPUT_CLK_MONO] = ktime_set(0, 0);
	} else if (dev->num_vals >= dev->max_vals - 2) {
		dev->vals[dev->num_vals++] = input_value_sync;
		input_pass_values(dev, dev->vals, dev->num_vals);
		dev->num_vals = 0;
	}

}

/**
 * input_event() - report new input event
 * @dev: device that generated the event
 * @type: type of the event
 * @code: event code
 * @value: value of the event
 *
 * This function should be used by drivers implementing various input
 * devices to report input events. See also input_inject_event().
 *
 * NOTE: input_event() may be safely used right after input device was
 * allocated with input_allocate_device(), even before it is registered
 * with input_register_device(), but the event will not reach any of the
 * input handlers. Such early invocation of input_event() may be used
 * to 'seed' initial state of a switch or initial position of absolute
 * axis, etc.
   input_event() - 上报新的输入事件
 * @dev: 产生该事件的设备
 * @type: 事件类型
 * @code: 事件代码
 * @value: 事件的值
 *
 * 实现各种输入设备的驱动程序应使用此函数来上报输入事件。
 * 另请参见 input_inject_event()。
 *
 * 注意：input_event() 可以在 input_allocate_device() 分配输入设备后
 * 立即安全使用，甚至可以在 input_register_device() 注册该设备之前
 * 使用，但这样的事件不会到达任何输入处理程序。这种对 input_event()
 * 的早期调用可用于“播种”某个开关的初始状态，或某个绝对轴的初始位置等。 * 
 */
void input_event(struct input_dev *dev,
		 unsigned int type, unsigned int code, int value)
{
	unsigned long flags;

	if (is_event_supported(type, dev->evbit, EV_MAX)) {

		spin_lock_irqsave(&dev->event_lock, flags);
		input_handle_event(dev, type, code, value);
		spin_unlock_irqrestore(&dev->event_lock, flags);
	}
}
EXPORT_SYMBOL(input_event);

/**
 * input_inject_event() - send input event from input handler
 * @handle: input handle to send event through
 * @type: type of the event
 * @code: event code
 * @value: value of the event
 *
 * Similar to input_event() but will ignore event if device is
 * "grabbed" and handle injecting event is not the one that owns
 * the device.
 * 
 * input_inject_event() - 从输入处理程序发送输入事件
 * @handle: 用于发送该事件的输入句柄
 * @type: 事件类型
 * @code: 事件代码
 * @value: 事件值
 *
 * 与 input_event() 类似，但如果设备已被“抓取”，且正在注入事件的句柄
 * 不是拥有该设备的句柄时，此函数将忽略该事件。 * 
 */
void input_inject_event(struct input_handle *handle,
			unsigned int type, unsigned int code, int value)
{
	struct input_dev *dev = handle->dev;
	struct input_handle *grab;
	unsigned long flags;

	if (is_event_supported(type, dev->evbit, EV_MAX)) {
		spin_lock_irqsave(&dev->event_lock, flags);

		rcu_read_lock();
		grab = rcu_dereference(dev->grab);
		if (!grab || grab == handle)
			input_handle_event(dev, type, code, value);
		rcu_read_unlock();

		spin_unlock_irqrestore(&dev->event_lock, flags);
	}
}
EXPORT_SYMBOL(input_inject_event);

/**
 * input_alloc_absinfo - allocates array of input_absinfo structs
 * @dev: the input device emitting absolute events
 *
 * If the absinfo struct the caller asked for is already allocated, this
 * functions will not do anything.
 * 
 * input_alloc_absinfo - 分配 input_absinfo 结构体数组
 * @dev: 发送绝对事件的输入设备
 *
 * 如果调用者请求的 absinfo 结构体已经分配过，本函数将不做任何操作。 * 
 */
void input_alloc_absinfo(struct input_dev *dev)
{
	if (dev->absinfo)
		return;

	dev->absinfo = kcalloc(ABS_CNT, sizeof(*dev->absinfo), GFP_KERNEL);
	if (!dev->absinfo) {
		dev_err(dev->dev.parent ?: &dev->dev,
			"%s: unable to allocate memory\n", __func__);
		/*
		 * We will handle this allocation failure in
		 * input_register_device() when we refuse to register input
		 * device with ABS bits but without absinfo.
		 */
	}
}
EXPORT_SYMBOL(input_alloc_absinfo);

void input_set_abs_params(struct input_dev *dev, unsigned int axis,
			  int min, int max, int fuzz, int flat)
{
	struct input_absinfo *absinfo;

	input_alloc_absinfo(dev);
	if (!dev->absinfo)
		return;

	absinfo = &dev->absinfo[axis];
	absinfo->minimum = min;
	absinfo->maximum = max;
	absinfo->fuzz = fuzz;
	absinfo->flat = flat;

	__set_bit(EV_ABS, dev->evbit);
	__set_bit(axis, dev->absbit);
}
EXPORT_SYMBOL(input_set_abs_params);


/**
 * input_grab_device - grabs device for exclusive use
 * @handle: input handle that wants to own the device
 *
 * When a device is grabbed by an input handle all events generated by
 * the device are delivered only to this handle. Also events injected
 * by other input handles are ignored while device is grabbed.
 * 
 * input_grab_device - 抓取设备以供独占使用
 * @handle: 想要拥有该设备的输入句柄
 *
 * 当设备被某个输入句柄抓取后，该设备产生的所有事件将仅传递给此句柄。
 * 同时，在设备被抓取期间，其他输入句柄注入的事件会被忽略。 * 
 * 
 * man:这个函数要和input_releaase_device配合使用
 */
int input_grab_device(struct input_handle *handle)
{
	struct input_dev *dev = handle->dev;
	int retval;

	retval = mutex_lock_interruptible(&dev->mutex);
	if (retval)
		return retval;

	if (dev->grab) {
		retval = -EBUSY;
		goto out;
	}

	rcu_assign_pointer(dev->grab, handle);

 out:
	mutex_unlock(&dev->mutex);
	return retval;
}
EXPORT_SYMBOL(input_grab_device);

static void __input_release_device(struct input_handle *handle)
{
	struct input_dev *dev = handle->dev;
	struct input_handle *grabber;

	grabber = rcu_dereference_protected(dev->grab,
					    lockdep_is_held(&dev->mutex));
	if (grabber == handle) {
		rcu_assign_pointer(dev->grab, NULL);
		/* Make sure input_pass_event() notices that grab is gone */
		synchronize_rcu();

		list_for_each_entry(handle, &dev->h_list, d_node)
			if (handle->open && handle->handler->start)
				handle->handler->start(handle);
	}
}

/**
 * input_release_device - release previously grabbed device
 * @handle: input handle that owns the device
 *
 * Releases previously grabbed device so that other input handles can
 * start receiving input events. Upon release all handlers attached
 * to the device have their start() method called so they have a change
 * to synchronize device state with the rest of the system.
 * 
 * input_release_device - 释放之前抓取的设备
 * @handle: 拥有该设备的输入句柄
 *
 * 释放之前抓取的设备，以便其他输入句柄可以开始接收输入事件。
 * 释放时，所有挂载到该设备上的处理程序都会调用其 start() 方法，
 * 这样它们就有机会将设备状态与系统其余部分同步。
 * 
 * man:这个函数要和input_grab_device配合使用
 *  * 
 */
void input_release_device(struct input_handle *handle)
{
	struct input_dev *dev = handle->dev;

	mutex_lock(&dev->mutex);
	__input_release_device(handle);
	mutex_unlock(&dev->mutex);
}
EXPORT_SYMBOL(input_release_device);

/**
 * input_open_device - open input device
 * @handle: handle through which device is being accessed
 *
 * This function should be called by input handlers when they
 * want to start receive events from given input device.
 * 
 * input_open_device - 打开输入设备
 * @handle: 用于访问该设备的句柄
 *
 * 当输入处理程序希望开始接收来自给定输入设备的事件时，应调用此函数。
 * 
 * man:这个函数和input_close_device配合使用
 */
int input_open_device(struct input_handle *handle)
{
	struct input_dev *dev = handle->dev;
	int retval;

	retval = mutex_lock_interruptible(&dev->mutex);
	if (retval)
		return retval;

	if (dev->going_away) {
		retval = -ENODEV;
		goto out;
	}

	handle->open++;

	if (!dev->users++ && dev->open)
		retval = dev->open(dev);

	if (retval) {
		dev->users--;
		if (!--handle->open) {
			/*
			 * Make sure we are not delivering any more events
			 * through this handle
			 */
			synchronize_rcu();
		}
	}

 out:
	mutex_unlock(&dev->mutex);
	return retval;
}
EXPORT_SYMBOL(input_open_device);

int input_flush_device(struct input_handle *handle, struct file *file)
{
	struct input_dev *dev = handle->dev;
	int retval;

	retval = mutex_lock_interruptible(&dev->mutex);
	if (retval)
		return retval;

	if (dev->flush)
		retval = dev->flush(dev, file);

	mutex_unlock(&dev->mutex);
	return retval;
}
EXPORT_SYMBOL(input_flush_device);

/**
 * input_close_device - close input device
 * @handle: handle through which device is being accessed
 *
 * This function should be called by input handlers when they
 * want to stop receive events from given input device.
 * 
 * 
 *  * input_close_device - 关闭输入设备
 * @handle: 用于访问该设备的句柄
 *
 * 当输入处理程序希望停止接收来自给定输入设备的事件时，应调用此函数。
 * 
 * man:这个函数和input_open_device配合使用
 */
void input_close_device(struct input_handle *handle)
{
	struct input_dev *dev = handle->dev;

	mutex_lock(&dev->mutex);

	__input_release_device(handle);

	if (!--dev->users && dev->close)
		dev->close(dev);

	if (!--handle->open) {
		/*
		 * synchronize_rcu() makes sure that input_pass_event()
		 * completed and that no more input events are delivered
		 * through this handle
		 */
		synchronize_rcu();
	}

	mutex_unlock(&dev->mutex);
}
EXPORT_SYMBOL(input_close_device);

/*
 * Simulate keyup events for all keys that are marked as pressed.
 * The function must be called with dev->event_lock held.
 */
static void input_dev_release_keys(struct input_dev *dev)
{
	bool need_sync = false;
	int code;

	if (is_event_supported(EV_KEY, dev->evbit, EV_MAX)) {
		for_each_set_bit(code, dev->key, KEY_CNT) {
			input_pass_event(dev, EV_KEY, code, 0);
			need_sync = true;
		}

		if (need_sync)
			input_pass_event(dev, EV_SYN, SYN_REPORT, 1);

		memset(dev->key, 0, sizeof(dev->key));
	}
}

/*
 * Prepare device for unregistering
 */
static void input_disconnect_device(struct input_dev *dev)
{
	struct input_handle *handle;

	/*
	 * Mark device as going away. Note that we take dev->mutex here
	 * not to protect access to dev->going_away but rather to ensure
	 * that there are no threads in the middle of input_open_device()
	 */
	mutex_lock(&dev->mutex);
	dev->going_away = true;
	mutex_unlock(&dev->mutex);

	spin_lock_irq(&dev->event_lock);

	/*
	 * Simulate keyup events for all pressed keys so that handlers
	 * are not left with "stuck" keys. The driver may continue
	 * generate events even after we done here but they will not
	 * reach any handlers.
	 */
	input_dev_release_keys(dev);

	list_for_each_entry(handle, &dev->h_list, d_node)
		handle->open = 0;

	spin_unlock_irq(&dev->event_lock);
}

/**
 * input_scancode_to_scalar() - converts scancode in &struct input_keymap_entry
 * @ke: keymap entry containing scancode to be converted.
 * @scancode: pointer to the location where converted scancode should
 *	be stored.
 *
 * This function is used to convert scancode stored in &struct keymap_entry
 * into scalar form understood by legacy keymap handling methods. These
 * methods expect scancodes to be represented as 'unsigned int'.
 * 
 * input_scancode_to_scalar() - 将 input_keymap_entry 结构体中的扫描码转换为标量形式
 * @ke: 包含待转换扫描码的键映射条目
 * @scancode: 指向存储转换后扫描码的位置的指针
 *
 * 该函数用于将存储在 keymap_entry 结构体中的扫描码转换成传统的
 * 键映射处理方法所能理解的标量形式。这些方法要求扫描码以
 * “unsigned int” 类型表示。 * 
 */
int input_scancode_to_scalar(const struct input_keymap_entry *ke,
			     unsigned int *scancode)
{
	switch (ke->len) {
	case 1:
		*scancode = *((u8 *)ke->scancode);
		break;

	case 2:
		*scancode = *((u16 *)ke->scancode);
		break;

	case 4:
		*scancode = *((u32 *)ke->scancode);
		break;

	default:
		return -EINVAL;
	}

	return 0;
}
EXPORT_SYMBOL(input_scancode_to_scalar);

/*
 * Those routines handle the default case where no [gs]etkeycode() is
 * defined. In this case, an array indexed by the scancode is used.
 */

static unsigned int input_fetch_keycode(struct input_dev *dev,
					unsigned int index)
{
	switch (dev->keycodesize) {
	case 1:
		return ((u8 *)dev->keycode)[index];

	case 2:
		return ((u16 *)dev->keycode)[index];

	default:
		return ((u32 *)dev->keycode)[index];
	}
}

static int input_default_getkeycode(struct input_dev *dev,
				    struct input_keymap_entry *ke)
{
	unsigned int index;
	int error;

	if (!dev->keycodesize)
		return -EINVAL;

	if (ke->flags & INPUT_KEYMAP_BY_INDEX)
		index = ke->index;
	else {
		error = input_scancode_to_scalar(ke, &index);
		if (error)
			return error;
	}

	if (index >= dev->keycodemax)
		return -EINVAL;

	ke->keycode = input_fetch_keycode(dev, index);
	ke->index = index;
	ke->len = sizeof(index);
	memcpy(ke->scancode, &index, sizeof(index));

	return 0;
}

static int input_default_setkeycode(struct input_dev *dev,
				    const struct input_keymap_entry *ke,
				    unsigned int *old_keycode)
{
	unsigned int index;
	int error;
	int i;

	if (!dev->keycodesize)
		return -EINVAL;

	if (ke->flags & INPUT_KEYMAP_BY_INDEX) {
		index = ke->index;
	} else {
		error = input_scancode_to_scalar(ke, &index);
		if (error)
			return error;
	}

	if (index >= dev->keycodemax)
		return -EINVAL;

	if (dev->keycodesize < sizeof(ke->keycode) &&
			(ke->keycode >> (dev->keycodesize * 8)))
		return -EINVAL;

	switch (dev->keycodesize) {
		case 1: {
			u8 *k = (u8 *)dev->keycode;
			*old_keycode = k[index];
			k[index] = ke->keycode;
			break;
		}
		case 2: {
			u16 *k = (u16 *)dev->keycode;
			*old_keycode = k[index];
			k[index] = ke->keycode;
			break;
		}
		default: {
			u32 *k = (u32 *)dev->keycode;
			*old_keycode = k[index];
			k[index] = ke->keycode;
			break;
		}
	}

	if (*old_keycode <= KEY_MAX) {
		__clear_bit(*old_keycode, dev->keybit);
		for (i = 0; i < dev->keycodemax; i++) {
			if (input_fetch_keycode(dev, i) == *old_keycode) {
				__set_bit(*old_keycode, dev->keybit);
				/* Setting the bit twice is useless, so break */
				break;
			}
		}
	}

	__set_bit(ke->keycode, dev->keybit);
	return 0;
}

/**
 * input_get_keycode - retrieve keycode currently mapped to a given scancode
 * @dev: input device which keymap is being queried
 * @ke: keymap entry
 *
 * This function should be called by anyone interested in retrieving current
 * keymap. Presently evdev handlers use it.
 * 
 * input_get_keycode - 获取当前映射到给定扫描码的键码
 * @dev: 被查询键映射表的输入设备
 * @ke: 键映射条目
 *
 * 任何需要获取当前键映射表的人都应调用此函数。
 * 目前 evdev 处理程序会使用它。 * 
 */
int input_get_keycode(struct input_dev *dev, struct input_keymap_entry *ke)
{
	unsigned long flags;
	int retval;

	spin_lock_irqsave(&dev->event_lock, flags);
	retval = dev->getkeycode(dev, ke);
	spin_unlock_irqrestore(&dev->event_lock, flags);

	return retval;
}
EXPORT_SYMBOL(input_get_keycode);

/**
 * input_set_keycode - attribute a keycode to a given scancode
 * @dev: input device which keymap is being updated
 * @ke: new keymap entry
 *
 * This function should be called by anyone needing to update current
 * keymap. Presently keyboard and evdev handlers use it.
 * 
 * input_set_keycode - 为给定的扫描码分配键码
 * @dev: 键映射表正在被更新的输入设备
 * @ke: 新的键映射条目
 *
 * 任何需要更新当前键映射表的人都应调用此函数。
 * 目前 keyboard 和 evdev 处理程序会使用它。
 *  * 
 */
int input_set_keycode(struct input_dev *dev,
		      const struct input_keymap_entry *ke)
{
	unsigned long flags;
	unsigned int old_keycode;
	int retval;

	if (ke->keycode > KEY_MAX)
		return -EINVAL;

	spin_lock_irqsave(&dev->event_lock, flags);

	retval = dev->setkeycode(dev, ke, &old_keycode);
	if (retval)
		goto out;

	/* Make sure KEY_RESERVED did not get enabled. */
	__clear_bit(KEY_RESERVED, dev->keybit);

	/*
	 * Simulate keyup event if keycode is not present
	 * in the keymap anymore
	 */
	if (old_keycode > KEY_MAX) {
		dev_warn(dev->dev.parent ?: &dev->dev,
			 "%s: got too big old keycode %#x\n",
			 __func__, old_keycode);
	} else if (test_bit(EV_KEY, dev->evbit) &&
		   !is_event_supported(old_keycode, dev->keybit, KEY_MAX) &&
		   __test_and_clear_bit(old_keycode, dev->key)) {
		struct input_value vals[] =  {
			{ EV_KEY, old_keycode, 0 },
			input_value_sync
		};

		input_pass_values(dev, vals, ARRAY_SIZE(vals));
	}

 out:
	spin_unlock_irqrestore(&dev->event_lock, flags);

	return retval;
}
EXPORT_SYMBOL(input_set_keycode);

bool input_match_device_id(const struct input_dev *dev,
			   const struct input_device_id *id)
{
	if (id->flags & INPUT_DEVICE_ID_MATCH_BUS)
		if (id->bustype != dev->id.bustype)
			return false;

	if (id->flags & INPUT_DEVICE_ID_MATCH_VENDOR)
		if (id->vendor != dev->id.vendor)
			return false;

	if (id->flags & INPUT_DEVICE_ID_MATCH_PRODUCT)
		if (id->product != dev->id.product)
			return false;

	if (id->flags & INPUT_DEVICE_ID_MATCH_VERSION)
		if (id->version != dev->id.version)
			return false;

	if (!bitmap_subset(id->evbit, dev->evbit, EV_MAX) ||
	    !bitmap_subset(id->keybit, dev->keybit, KEY_MAX) ||
	    !bitmap_subset(id->relbit, dev->relbit, REL_MAX) ||
	    !bitmap_subset(id->absbit, dev->absbit, ABS_MAX) ||
	    !bitmap_subset(id->mscbit, dev->mscbit, MSC_MAX) ||
	    !bitmap_subset(id->ledbit, dev->ledbit, LED_MAX) ||
	    !bitmap_subset(id->sndbit, dev->sndbit, SND_MAX) ||
	    !bitmap_subset(id->ffbit, dev->ffbit, FF_MAX) ||
	    !bitmap_subset(id->swbit, dev->swbit, SW_MAX) ||
	    !bitmap_subset(id->propbit, dev->propbit, INPUT_PROP_MAX)) {
		return false;
	}

	return true;
}
EXPORT_SYMBOL(input_match_device_id);

static const struct input_device_id *input_match_device(struct input_handler *handler,
							struct input_dev *dev)
{
	const struct input_device_id *id;

	for (id = handler->id_table; id->flags || id->driver_info; id++) {
		if (input_match_device_id(dev, id) &&
		    (!handler->match || handler->match(handler, dev))) {
			return id;
		}
	}

	return NULL;
}

static int input_attach_handler(struct input_dev *dev, struct input_handler *handler)
{
	const struct input_device_id *id;
	int error;

	id = input_match_device(handler, dev);
	if (!id)
		return -ENODEV;

	error = handler->connect(handler, dev, id);
	if (error && error != -ENODEV)
		pr_err("failed to attach handler %s to device %s, error: %d\n",
		       handler->name, kobject_name(&dev->dev.kobj), error);

	return error;
}

#ifdef CONFIG_COMPAT

static int input_bits_to_string(char *buf, int buf_size,
				unsigned long bits, bool skip_empty)
{
	int len = 0;

	if (in_compat_syscall()) {
		u32 dword = bits >> 32;
		if (dword || !skip_empty)
			len += snprintf(buf, buf_size, "%x ", dword);

		dword = bits & 0xffffffffUL;
		if (dword || !skip_empty || len)
			len += snprintf(buf + len, max(buf_size - len, 0),
					"%x", dword);
	} else {
		if (bits || !skip_empty)
			len += snprintf(buf, buf_size, "%lx", bits);
	}

	return len;
}

#else /* !CONFIG_COMPAT */

static int input_bits_to_string(char *buf, int buf_size,
				unsigned long bits, bool skip_empty)
{
	return bits || !skip_empty ?
		snprintf(buf, buf_size, "%lx", bits) : 0;
}

#endif

#ifdef CONFIG_PROC_FS

static struct proc_dir_entry *proc_bus_input_dir;
static DECLARE_WAIT_QUEUE_HEAD(input_devices_poll_wait);
static int input_devices_state;

static inline void input_wakeup_procfs_readers(void)
{
	input_devices_state++;
	wake_up(&input_devices_poll_wait);
}

static __poll_t input_proc_devices_poll(struct file *file, poll_table *wait)
{
	poll_wait(file, &input_devices_poll_wait, wait);
	if (file->f_version != input_devices_state) {
		file->f_version = input_devices_state;
		return EPOLLIN | EPOLLRDNORM;
	}

	return 0;
}

union input_seq_state {
	struct {
		unsigned short pos;
		bool mutex_acquired;
	};
	void *p;
};

static void *input_devices_seq_start(struct seq_file *seq, loff_t *pos)
{
	union input_seq_state *state = (union input_seq_state *)&seq->private;
	int error;

	/* We need to fit into seq->private pointer */
	BUILD_BUG_ON(sizeof(union input_seq_state) != sizeof(seq->private));

	error = mutex_lock_interruptible(&input_mutex);
	if (error) {
		state->mutex_acquired = false;
		return ERR_PTR(error);
	}

	state->mutex_acquired = true;

	return seq_list_start(&input_dev_list, *pos);
}

static void *input_devices_seq_next(struct seq_file *seq, void *v, loff_t *pos)
{
	return seq_list_next(v, &input_dev_list, pos);
}

static void input_seq_stop(struct seq_file *seq, void *v)
{
	union input_seq_state *state = (union input_seq_state *)&seq->private;

	if (state->mutex_acquired)
		mutex_unlock(&input_mutex);
}

static void input_seq_print_bitmap(struct seq_file *seq, const char *name,
				   unsigned long *bitmap, int max)
{
	int i;
	bool skip_empty = true;
	char buf[18];

	seq_printf(seq, "B: %s=", name);

	for (i = BITS_TO_LONGS(max) - 1; i >= 0; i--) {
		if (input_bits_to_string(buf, sizeof(buf),
					 bitmap[i], skip_empty)) {
			skip_empty = false;
			seq_printf(seq, "%s%s", buf, i > 0 ? " " : "");
		}
	}

	/*
	 * If no output was produced print a single 0.
	 */
	if (skip_empty)
		seq_putc(seq, '0');

	seq_putc(seq, '\n');
}

static int input_devices_seq_show(struct seq_file *seq, void *v)
{
	struct input_dev *dev = container_of(v, struct input_dev, node);
	const char *path = kobject_get_path(&dev->dev.kobj, GFP_KERNEL);
	struct input_handle *handle;

	seq_printf(seq, "I: Bus=%04x Vendor=%04x Product=%04x Version=%04x\n",
		   dev->id.bustype, dev->id.vendor, dev->id.product, dev->id.version);

	seq_printf(seq, "N: Name=\"%s\"\n", dev->name ? dev->name : "");
	seq_printf(seq, "P: Phys=%s\n", dev->phys ? dev->phys : "");
	seq_printf(seq, "S: Sysfs=%s\n", path ? path : "");
	seq_printf(seq, "U: Uniq=%s\n", dev->uniq ? dev->uniq : "");
	seq_puts(seq, "H: Handlers=");

	list_for_each_entry(handle, &dev->h_list, d_node)
		seq_printf(seq, "%s ", handle->name);
	seq_putc(seq, '\n');

	input_seq_print_bitmap(seq, "PROP", dev->propbit, INPUT_PROP_MAX);

	input_seq_print_bitmap(seq, "EV", dev->evbit, EV_MAX);
	if (test_bit(EV_KEY, dev->evbit))
		input_seq_print_bitmap(seq, "KEY", dev->keybit, KEY_MAX);
	if (test_bit(EV_REL, dev->evbit))
		input_seq_print_bitmap(seq, "REL", dev->relbit, REL_MAX);
	if (test_bit(EV_ABS, dev->evbit))
		input_seq_print_bitmap(seq, "ABS", dev->absbit, ABS_MAX);
	if (test_bit(EV_MSC, dev->evbit))
		input_seq_print_bitmap(seq, "MSC", dev->mscbit, MSC_MAX);
	if (test_bit(EV_LED, dev->evbit))
		input_seq_print_bitmap(seq, "LED", dev->ledbit, LED_MAX);
	if (test_bit(EV_SND, dev->evbit))
		input_seq_print_bitmap(seq, "SND", dev->sndbit, SND_MAX);
	if (test_bit(EV_FF, dev->evbit))
		input_seq_print_bitmap(seq, "FF", dev->ffbit, FF_MAX);
	if (test_bit(EV_SW, dev->evbit))
		input_seq_print_bitmap(seq, "SW", dev->swbit, SW_MAX);

	seq_putc(seq, '\n');

	kfree(path);
	return 0;
}

static const struct seq_operations input_devices_seq_ops = {
	.start	= input_devices_seq_start,
	.next	= input_devices_seq_next,
	.stop	= input_seq_stop,
	.show	= input_devices_seq_show,
};

static int input_proc_devices_open(struct inode *inode, struct file *file)
{
	return seq_open(file, &input_devices_seq_ops);
}

static const struct file_operations input_devices_fileops = {
	.owner		= THIS_MODULE,
	.open		= input_proc_devices_open,
	.poll		= input_proc_devices_poll,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= seq_release,
};

static void *input_handlers_seq_start(struct seq_file *seq, loff_t *pos)
{
	union input_seq_state *state = (union input_seq_state *)&seq->private;
	int error;

	/* We need to fit into seq->private pointer */
	BUILD_BUG_ON(sizeof(union input_seq_state) != sizeof(seq->private));

	error = mutex_lock_interruptible(&input_mutex);
	if (error) {
		state->mutex_acquired = false;
		return ERR_PTR(error);
	}

	state->mutex_acquired = true;
	state->pos = *pos;

	return seq_list_start(&input_handler_list, *pos);
}

static void *input_handlers_seq_next(struct seq_file *seq, void *v, loff_t *pos)
{
	union input_seq_state *state = (union input_seq_state *)&seq->private;

	state->pos = *pos + 1;
	return seq_list_next(v, &input_handler_list, pos);
}

static int input_handlers_seq_show(struct seq_file *seq, void *v)
{
	struct input_handler *handler = container_of(v, struct input_handler, node);
	union input_seq_state *state = (union input_seq_state *)&seq->private;

	seq_printf(seq, "N: Number=%u Name=%s", state->pos, handler->name);
	if (handler->filter)
		seq_puts(seq, " (filter)");
	if (handler->legacy_minors)
		seq_printf(seq, " Minor=%d", handler->minor);
	seq_putc(seq, '\n');

	return 0;
}

static const struct seq_operations input_handlers_seq_ops = {
	.start	= input_handlers_seq_start,
	.next	= input_handlers_seq_next,
	.stop	= input_seq_stop,
	.show	= input_handlers_seq_show,
};

static int input_proc_handlers_open(struct inode *inode, struct file *file)
{
	return seq_open(file, &input_handlers_seq_ops);
}

static const struct file_operations input_handlers_fileops = {
	.owner		= THIS_MODULE,
	.open		= input_proc_handlers_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= seq_release,
};

static int __init input_proc_init(void)
{
	struct proc_dir_entry *entry;

	proc_bus_input_dir = proc_mkdir("bus/input", NULL);
	if (!proc_bus_input_dir)
		return -ENOMEM;

	entry = proc_create("devices", 0, proc_bus_input_dir,
			    &input_devices_fileops);
	if (!entry)
		goto fail1;

	entry = proc_create("handlers", 0, proc_bus_input_dir,
			    &input_handlers_fileops);
	if (!entry)
		goto fail2;

	return 0;

 fail2:	remove_proc_entry("devices", proc_bus_input_dir);
 fail1: remove_proc_entry("bus/input", NULL);
	return -ENOMEM;
}

static void input_proc_exit(void)
{
	remove_proc_entry("devices", proc_bus_input_dir);
	remove_proc_entry("handlers", proc_bus_input_dir);
	remove_proc_entry("bus/input", NULL);
}

#else /* !CONFIG_PROC_FS */
static inline void input_wakeup_procfs_readers(void) { }
static inline int input_proc_init(void) { return 0; }
static inline void input_proc_exit(void) { }
#endif

#define INPUT_DEV_STRING_ATTR_SHOW(name)				\
static ssize_t input_dev_show_##name(struct device *dev,		\
				     struct device_attribute *attr,	\
				     char *buf)				\
{									\
	struct input_dev *input_dev = to_input_dev(dev);		\
									\
	return scnprintf(buf, PAGE_SIZE, "%s\n",			\
			 input_dev->name ? input_dev->name : "");	\
}									\
static DEVICE_ATTR(name, S_IRUGO, input_dev_show_##name, NULL)

INPUT_DEV_STRING_ATTR_SHOW(name);
INPUT_DEV_STRING_ATTR_SHOW(phys);
INPUT_DEV_STRING_ATTR_SHOW(uniq);

static int input_print_modalias_bits(char *buf, int size,
				     char name, unsigned long *bm,
				     unsigned int min_bit, unsigned int max_bit)
{
	int len = 0, i;

	len += snprintf(buf, max(size, 0), "%c", name);
	for (i = min_bit; i < max_bit; i++)
		if (bm[BIT_WORD(i)] & BIT_MASK(i))
			len += snprintf(buf + len, max(size - len, 0), "%X,", i);
	return len;
}

static int input_print_modalias(char *buf, int size, struct input_dev *id,
				int add_cr)
{
	int len;

	len = snprintf(buf, max(size, 0),
		       "input:b%04Xv%04Xp%04Xe%04X-",
		       id->id.bustype, id->id.vendor,
		       id->id.product, id->id.version);

	len += input_print_modalias_bits(buf + len, size - len,
				'e', id->evbit, 0, EV_MAX);
	len += input_print_modalias_bits(buf + len, size - len,
				'k', id->keybit, KEY_MIN_INTERESTING, KEY_MAX);
	len += input_print_modalias_bits(buf + len, size - len,
				'r', id->relbit, 0, REL_MAX);
	len += input_print_modalias_bits(buf + len, size - len,
				'a', id->absbit, 0, ABS_MAX);
	len += input_print_modalias_bits(buf + len, size - len,
				'm', id->mscbit, 0, MSC_MAX);
	len += input_print_modalias_bits(buf + len, size - len,
				'l', id->ledbit, 0, LED_MAX);
	len += input_print_modalias_bits(buf + len, size - len,
				's', id->sndbit, 0, SND_MAX);
	len += input_print_modalias_bits(buf + len, size - len,
				'f', id->ffbit, 0, FF_MAX);
	len += input_print_modalias_bits(buf + len, size - len,
				'w', id->swbit, 0, SW_MAX);

	if (add_cr)
		len += snprintf(buf + len, max(size - len, 0), "\n");

	return len;
}

static ssize_t input_dev_show_modalias(struct device *dev,
				       struct device_attribute *attr,
				       char *buf)
{
	struct input_dev *id = to_input_dev(dev);
	ssize_t len;

	len = input_print_modalias(buf, PAGE_SIZE, id, 1);

	return min_t(int, len, PAGE_SIZE);
}
static DEVICE_ATTR(modalias, S_IRUGO, input_dev_show_modalias, NULL);

static int input_print_bitmap(char *buf, int buf_size, unsigned long *bitmap,
			      int max, int add_cr);

static ssize_t input_dev_show_properties(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct input_dev *input_dev = to_input_dev(dev);
	int len = input_print_bitmap(buf, PAGE_SIZE, input_dev->propbit,
				     INPUT_PROP_MAX, true);
	return min_t(int, len, PAGE_SIZE);
}
static DEVICE_ATTR(properties, S_IRUGO, input_dev_show_properties, NULL);

static struct attribute *input_dev_attrs[] = {
	&dev_attr_name.attr,
	&dev_attr_phys.attr,
	&dev_attr_uniq.attr,
	&dev_attr_modalias.attr,
	&dev_attr_properties.attr,
	NULL
};

static const struct attribute_group input_dev_attr_group = {
	.attrs	= input_dev_attrs,
};

#define INPUT_DEV_ID_ATTR(name)						\
static ssize_t input_dev_show_id_##name(struct device *dev,		\
					struct device_attribute *attr,	\
					char *buf)			\
{									\
	struct input_dev *input_dev = to_input_dev(dev);		\
	return scnprintf(buf, PAGE_SIZE, "%04x\n", input_dev->id.name);	\
}									\
static DEVICE_ATTR(name, S_IRUGO, input_dev_show_id_##name, NULL)

INPUT_DEV_ID_ATTR(bustype);
INPUT_DEV_ID_ATTR(vendor);
INPUT_DEV_ID_ATTR(product);
INPUT_DEV_ID_ATTR(version);

static struct attribute *input_dev_id_attrs[] = {
	&dev_attr_bustype.attr,
	&dev_attr_vendor.attr,
	&dev_attr_product.attr,
	&dev_attr_version.attr,
	NULL
};

static const struct attribute_group input_dev_id_attr_group = {
	.name	= "id",
	.attrs	= input_dev_id_attrs,
};

static int input_print_bitmap(char *buf, int buf_size, unsigned long *bitmap,
			      int max, int add_cr)
{
	int i;
	int len = 0;
	bool skip_empty = true;

	for (i = BITS_TO_LONGS(max) - 1; i >= 0; i--) {
		len += input_bits_to_string(buf + len, max(buf_size - len, 0),
					    bitmap[i], skip_empty);
		if (len) {
			skip_empty = false;
			if (i > 0)
				len += snprintf(buf + len, max(buf_size - len, 0), " ");
		}
	}

	/*
	 * If no output was produced print a single 0.
	 */
	if (len == 0)
		len = snprintf(buf, buf_size, "%d", 0);

	if (add_cr)
		len += snprintf(buf + len, max(buf_size - len, 0), "\n");

	return len;
}

#define INPUT_DEV_CAP_ATTR(ev, bm)					\
static ssize_t input_dev_show_cap_##bm(struct device *dev,		\
				       struct device_attribute *attr,	\
				       char *buf)			\
{									\
	struct input_dev *input_dev = to_input_dev(dev);		\
	int len = input_print_bitmap(buf, PAGE_SIZE,			\
				     input_dev->bm##bit, ev##_MAX,	\
				     true);				\
	return min_t(int, len, PAGE_SIZE);				\
}									\
static DEVICE_ATTR(bm, S_IRUGO, input_dev_show_cap_##bm, NULL)

INPUT_DEV_CAP_ATTR(EV, ev);
INPUT_DEV_CAP_ATTR(KEY, key);
INPUT_DEV_CAP_ATTR(REL, rel);
INPUT_DEV_CAP_ATTR(ABS, abs);
INPUT_DEV_CAP_ATTR(MSC, msc);
INPUT_DEV_CAP_ATTR(LED, led);
INPUT_DEV_CAP_ATTR(SND, snd);
INPUT_DEV_CAP_ATTR(FF, ff);
INPUT_DEV_CAP_ATTR(SW, sw);

static struct attribute *input_dev_caps_attrs[] = {
	&dev_attr_ev.attr,
	&dev_attr_key.attr,
	&dev_attr_rel.attr,
	&dev_attr_abs.attr,
	&dev_attr_msc.attr,
	&dev_attr_led.attr,
	&dev_attr_snd.attr,
	&dev_attr_ff.attr,
	&dev_attr_sw.attr,
	NULL
};

static const struct attribute_group input_dev_caps_attr_group = {
	.name	= "capabilities",
	.attrs	= input_dev_caps_attrs,
};

static const struct attribute_group *input_dev_attr_groups[] = {
	&input_dev_attr_group,
	&input_dev_id_attr_group,
	&input_dev_caps_attr_group,
	NULL
};

static void input_dev_release(struct device *device)
{
	struct input_dev *dev = to_input_dev(device);

	input_ff_destroy(dev);
	input_mt_destroy_slots(dev);
	kfree(dev->absinfo);
	kfree(dev->vals);
	kfree(dev);

	module_put(THIS_MODULE);
}

/*
 * Input uevent interface - loading event handlers based on
 * device bitfields.
 */
static int input_add_uevent_bm_var(struct kobj_uevent_env *env,
				   const char *name, unsigned long *bitmap, int max)
{
	int len;

	if (add_uevent_var(env, "%s", name))
		return -ENOMEM;

	len = input_print_bitmap(&env->buf[env->buflen - 1],
				 sizeof(env->buf) - env->buflen,
				 bitmap, max, false);
	if (len >= (sizeof(env->buf) - env->buflen))
		return -ENOMEM;

	env->buflen += len;
	return 0;
}

static int input_add_uevent_modalias_var(struct kobj_uevent_env *env,
					 struct input_dev *dev)
{
	int len;

	if (add_uevent_var(env, "MODALIAS="))
		return -ENOMEM;

	len = input_print_modalias(&env->buf[env->buflen - 1],
				   sizeof(env->buf) - env->buflen,
				   dev, 0);
	if (len >= (sizeof(env->buf) - env->buflen))
		return -ENOMEM;

	env->buflen += len;
	return 0;
}

#define INPUT_ADD_HOTPLUG_VAR(fmt, val...)				\
	do {								\
		int err = add_uevent_var(env, fmt, val);		\
		if (err)						\
			return err;					\
	} while (0)

#define INPUT_ADD_HOTPLUG_BM_VAR(name, bm, max)				\
	do {								\
		int err = input_add_uevent_bm_var(env, name, bm, max);	\
		if (err)						\
			return err;					\
	} while (0)

#define INPUT_ADD_HOTPLUG_MODALIAS_VAR(dev)				\
	do {								\
		int err = input_add_uevent_modalias_var(env, dev);	\
		if (err)						\
			return err;					\
	} while (0)

static int input_dev_uevent(struct device *device, struct kobj_uevent_env *env)
{
	struct input_dev *dev = to_input_dev(device);

	INPUT_ADD_HOTPLUG_VAR("PRODUCT=%x/%x/%x/%x",
				dev->id.bustype, dev->id.vendor,
				dev->id.product, dev->id.version);
	if (dev->name)
		INPUT_ADD_HOTPLUG_VAR("NAME=\"%s\"", dev->name);
	if (dev->phys)
		INPUT_ADD_HOTPLUG_VAR("PHYS=\"%s\"", dev->phys);
	if (dev->uniq)
		INPUT_ADD_HOTPLUG_VAR("UNIQ=\"%s\"", dev->uniq);

	INPUT_ADD_HOTPLUG_BM_VAR("PROP=", dev->propbit, INPUT_PROP_MAX);

	INPUT_ADD_HOTPLUG_BM_VAR("EV=", dev->evbit, EV_MAX);
	if (test_bit(EV_KEY, dev->evbit))
		INPUT_ADD_HOTPLUG_BM_VAR("KEY=", dev->keybit, KEY_MAX);
	if (test_bit(EV_REL, dev->evbit))
		INPUT_ADD_HOTPLUG_BM_VAR("REL=", dev->relbit, REL_MAX);
	if (test_bit(EV_ABS, dev->evbit))
		INPUT_ADD_HOTPLUG_BM_VAR("ABS=", dev->absbit, ABS_MAX);
	if (test_bit(EV_MSC, dev->evbit))
		INPUT_ADD_HOTPLUG_BM_VAR("MSC=", dev->mscbit, MSC_MAX);
	if (test_bit(EV_LED, dev->evbit))
		INPUT_ADD_HOTPLUG_BM_VAR("LED=", dev->ledbit, LED_MAX);
	if (test_bit(EV_SND, dev->evbit))
		INPUT_ADD_HOTPLUG_BM_VAR("SND=", dev->sndbit, SND_MAX);
	if (test_bit(EV_FF, dev->evbit))
		INPUT_ADD_HOTPLUG_BM_VAR("FF=", dev->ffbit, FF_MAX);
	if (test_bit(EV_SW, dev->evbit))
		INPUT_ADD_HOTPLUG_BM_VAR("SW=", dev->swbit, SW_MAX);

	INPUT_ADD_HOTPLUG_MODALIAS_VAR(dev);

	return 0;
}

#define INPUT_DO_TOGGLE(dev, type, bits, on)				\
	do {								\
		int i;							\
		bool active;						\
									\
		if (!test_bit(EV_##type, dev->evbit))			\
			break;						\
									\
		for_each_set_bit(i, dev->bits##bit, type##_CNT) {	\
			active = test_bit(i, dev->bits);		\
			if (!active && !on)				\
				continue;				\
									\
			dev->event(dev, EV_##type, i, on ? active : 0);	\
		}							\
	} while (0)

static void input_dev_toggle(struct input_dev *dev, bool activate)
{
	if (!dev->event)
		return;

	INPUT_DO_TOGGLE(dev, LED, led, activate);
	INPUT_DO_TOGGLE(dev, SND, snd, activate);

	if (activate && test_bit(EV_REP, dev->evbit)) {
		dev->event(dev, EV_REP, REP_PERIOD, dev->rep[REP_PERIOD]);
		dev->event(dev, EV_REP, REP_DELAY, dev->rep[REP_DELAY]);
	}
}

/**
 * input_reset_device() - reset/restore the state of input device
 * @dev: input device whose state needs to be reset
 *
 * This function tries to reset the state of an opened input device and
 * bring internal state and state if the hardware in sync with each other.
 * We mark all keys as released, restore LED state, repeat rate, etc.
 * 
 * input_set_keycode - 为给定的扫描码分配键码
 * @dev: 键映射表正在被更新的输入设备
 * @ke: 新的键映射条目
 *
 * 任何需要更新当前键映射表的人都应调用此函数。
 * 目前 keyboard 和 evdev 处理程序会使用它。 * 
 */
void input_reset_device(struct input_dev *dev)
{
	unsigned long flags;

	mutex_lock(&dev->mutex);
	spin_lock_irqsave(&dev->event_lock, flags);

	input_dev_toggle(dev, true);
	input_dev_release_keys(dev);

	spin_unlock_irqrestore(&dev->event_lock, flags);
	mutex_unlock(&dev->mutex);
}
EXPORT_SYMBOL(input_reset_device);

#ifdef CONFIG_PM_SLEEP
static int input_dev_suspend(struct device *dev)
{
	struct input_dev *input_dev = to_input_dev(dev);

	spin_lock_irq(&input_dev->event_lock);

	/*
	 * Keys that are pressed now are unlikely to be
	 * still pressed when we resume.
	 */
	input_dev_release_keys(input_dev);

	/* Turn off LEDs and sounds, if any are active. */
	input_dev_toggle(input_dev, false);

	spin_unlock_irq(&input_dev->event_lock);

	return 0;
}

static int input_dev_resume(struct device *dev)
{
	struct input_dev *input_dev = to_input_dev(dev);

	spin_lock_irq(&input_dev->event_lock);

	/* Restore state of LEDs and sounds, if any were active. */
	input_dev_toggle(input_dev, true);

	spin_unlock_irq(&input_dev->event_lock);

	return 0;
}

static int input_dev_freeze(struct device *dev)
{
	struct input_dev *input_dev = to_input_dev(dev);

	spin_lock_irq(&input_dev->event_lock);

	/*
	 * Keys that are pressed now are unlikely to be
	 * still pressed when we resume.
	 */
	input_dev_release_keys(input_dev);

	spin_unlock_irq(&input_dev->event_lock);

	return 0;
}

static int input_dev_poweroff(struct device *dev)
{
	struct input_dev *input_dev = to_input_dev(dev);

	spin_lock_irq(&input_dev->event_lock);

	/* Turn off LEDs and sounds, if any are active. */
	input_dev_toggle(input_dev, false);

	spin_unlock_irq(&input_dev->event_lock);

	return 0;
}

static const struct dev_pm_ops input_dev_pm_ops = {
	.suspend	= input_dev_suspend,
	.resume		= input_dev_resume,
	.freeze		= input_dev_freeze,
	.poweroff	= input_dev_poweroff,
	.restore	= input_dev_resume,
};
#endif /* CONFIG_PM */

static const struct device_type input_dev_type = {
	.groups		= input_dev_attr_groups,
	.release	= input_dev_release,
	.uevent		= input_dev_uevent,
#ifdef CONFIG_PM_SLEEP
	.pm		= &input_dev_pm_ops,
#endif
};

static char *input_devnode(struct device *dev, umode_t *mode)
{
	return kasprintf(GFP_KERNEL, "input/%s", dev_name(dev));
}

struct class input_class = {
	.name		= "input",
	.devnode	= input_devnode,
};
EXPORT_SYMBOL_GPL(input_class);

/**
 * input_allocate_device - allocate memory for new input device
 *
 * Returns prepared struct input_dev or %NULL.
 *
 * NOTE: Use input_free_device() to free devices that have not been
 * registered; input_unregister_device() should be used for already
 * registered devices.
 * 
* input_allocate_device - 为新的输入设备分配内存
*
* 返回初始化好的 struct input_dev 指针，失败时返回 %NULL。
*
* 注意：对于尚未注册的设备，使用 input_free_device() 来释放；
* 对于已经注册的设备，应使用 input_unregister_device() 来注销。
 */

 struct input_dev *input_allocate_device(void)
{ 
	static atomic_t input_no = ATOMIC_INIT(-1);
	struct input_dev *dev;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (dev) {
		dev->dev.type = &input_dev_type;
		dev->dev.class = &input_class;
		device_initialize(&dev->dev);
		mutex_init(&dev->mutex);
		spin_lock_init(&dev->event_lock);
		timer_setup(&dev->timer, NULL, 0);
		INIT_LIST_HEAD(&dev->h_list);
		INIT_LIST_HEAD(&dev->node);

		dev_set_name(&dev->dev, "input%lu",
			     (unsigned long)atomic_inc_return(&input_no));

		__module_get(THIS_MODULE);
	}

	return dev;
}
EXPORT_SYMBOL(input_allocate_device);

struct input_devres {
	struct input_dev *input;
};

static int devm_input_device_match(struct device *dev, void *res, void *data)
{
	struct input_devres *devres = res;

	return devres->input == data;
}

static void devm_input_device_release(struct device *dev, void *res)
{
	struct input_devres *devres = res;
	struct input_dev *input = devres->input;

	dev_dbg(dev, "%s: dropping reference to %s\n",
		__func__, dev_name(&input->dev));
	input_put_device(input);
}

/**
 * devm_input_allocate_device - allocate managed input device
 * @dev: device owning the input device being created
 *
 * Returns prepared struct input_dev or %NULL.
 *
 * Managed input devices do not need to be explicitly unregistered or
 * freed as it will be done automatically when owner device unbinds from
 * its driver (or binding fails). Once managed input device is allocated,
 * it is ready to be set up and registered in the same fashion as regular
 * input device. There are no special devm_input_device_[un]register()
 * variants, regular ones work with both managed and unmanaged devices,
 * should you need them. In most cases however, managed input device need
 * not be explicitly unregistered or freed.
 *
 * NOTE: the owner device is set up as parent of input device and users
 * should not override it.
 * 
* devm_input_allocate_device - 分配托管的输入设备
 * @dev: 拥有此输入设备的设备
 *
 * 返回已准备好的 struct input_dev 或 %NULL。
 *
 * 托管的输入设备无需显式注销或释放，当所有者设备与其驱动解除绑定
 * （或绑定失败）时，该操作将自动完成。一旦分配了托管输入设备，
 * 它就可以像普通输入设备一样被设置和注册。没有专门的
 * devm_input_device_[un]register() 变体，如果需要，常规的注册/注销
 * 函数对托管和非托管设备均适用。然而，在大多数情况下，托管输入设备
 * 不需要显式注销或释放。
 *
 * 注意：所有者设备被设置为输入设备的父设备，用户不应覆盖它。
 */
struct input_dev *devm_input_allocate_device(struct device *dev)
{
	struct input_dev *input;
	struct input_devres *devres;

	devres = devres_alloc(devm_input_device_release,
			      sizeof(*devres), GFP_KERNEL);
	if (!devres)
		return NULL;

	input = input_allocate_device();
	if (!input) {
		devres_free(devres);
		return NULL;
	}

	input->dev.parent = dev;
	input->devres_managed = true;

	devres->input = input;
	devres_add(dev, devres);

	return input;
}
EXPORT_SYMBOL(devm_input_allocate_device);

/**
 * input_free_device - free memory occupied by input_dev structure
 * @dev: input device to free
 *
 * This function should only be used if input_register_device()
 * was not called yet or if it failed. Once device was registered
 * use input_unregister_device() and memory will be freed once last
 * reference to the device is dropped.
 *
 * Device should be allocated by input_allocate_device().
 *
 * NOTE: If there are references to the input device then memory
 * will not be freed until last reference is dropped.
 * 
 * input_free_device - 释放 input_dev 结构体占用的内存
 * @dev: 要释放的输入设备
 *
 * 本函数只应在 input_register_device() 尚未调用或调用失败的情况下使用。
 * 一旦设备已经注册，应使用 input_unregister_device() 来注销设备，内存会
 * 在该设备的最后一个引用被释放时自动回收。
 *
 * 设备应当通过 input_allocate_device() 进行分配。
 *
 * 注意：如果存在对输入设备的引用，则内存不会被释放，直到最后一个引用被消除。 * 
 */
void input_free_device(struct input_dev *dev)
{
	if (dev) {
		if (dev->devres_managed)
			WARN_ON(devres_destroy(dev->dev.parent,
						devm_input_device_release,
						devm_input_device_match,
						dev));
		input_put_device(dev);
	}
}
EXPORT_SYMBOL(input_free_device);

/**
 * input_set_timestamp - set timestamp for input events
 * @dev: input device to set timestamp for
 * @timestamp: the time at which the event has occurred
 *   in CLOCK_MONOTONIC
 *
 * This function is intended to provide to the input system a more
 * accurate time of when an event actually occurred. The driver should
 * call this function as soon as a timestamp is acquired ensuring
 * clock conversions in input_set_timestamp are done correctly.
 *
 * The system entering suspend state between timestamp acquisition and
 * calling input_set_timestamp can result in inaccurate conversions.
 * 
 * input_set_timestamp - 设置输入事件的时间戳
 * @dev: 要设置时间戳的输入设备
 * @timestamp: 事件发生的时间，以 CLOCK_MONOTONIC 时钟为基准
 *
 * 此函数旨在为输入系统提供事件实际发生时的更精确时间。
 * 驱动程序应在获取到时间戳后立刻调用本函数，以确保
 * input_set_timestamp 中的时钟转换能够正确进行。
 *
 * 在获取时间戳之后、调用 input_set_timestamp 之前，如果系统进入
 * 挂起状态，可能会导致时钟转换不准确。 * 
 */
void input_set_timestamp(struct input_dev *dev, ktime_t timestamp)
{
	dev->timestamp[INPUT_CLK_MONO] = timestamp;
	dev->timestamp[INPUT_CLK_REAL] = ktime_mono_to_real(timestamp);
	dev->timestamp[INPUT_CLK_BOOT] = ktime_mono_to_any(timestamp,
							   TK_OFFS_BOOT);
}
EXPORT_SYMBOL(input_set_timestamp);

/**
 * input_get_timestamp - get timestamp for input events
 * @dev: input device to get timestamp from
 *
 * A valid timestamp is a timestamp of non-zero value.
 * 
 * input_get_timestamp - 获取输入事件的时间戳
 * @dev: 要获取其时间戳的输入设备
 *
 * 一个有效的时间戳是指其值不为零的时间戳。 * 
 */
ktime_t *input_get_timestamp(struct input_dev *dev)
{
	const ktime_t invalid_timestamp = ktime_set(0, 0);

	if (!ktime_compare(dev->timestamp[INPUT_CLK_MONO], invalid_timestamp))
		input_set_timestamp(dev, ktime_get());

	return dev->timestamp;
}
EXPORT_SYMBOL(input_get_timestamp);

/**
 * input_set_capability - mark device as capable of a certain event
 * @dev: device that is capable of emitting or accepting event
 * @type: type of the event (EV_KEY, EV_REL, etc...)
 * @code: event code
 *
 * In addition to setting up corresponding bit in appropriate capability
 * bitmap the function also adjusts dev->evbit.
 * 
 *  * input_set_capability - 将设备标记为能够产生或接受某类事件
 * @dev: 能够产生或接受事件的设备
 * @type: 事件类型（EV_KEY、EV_REL 等）
 * @code: 事件代码
 *
 * 除了在对应能力位图中设置相应比特位之外，本函数还会调整 dev->evbit。
 * 
 * 
 */
void input_set_capability(struct input_dev *dev, unsigned int type, unsigned int code)
{
	switch (type) {
	case EV_KEY:
		__set_bit(code, dev->keybit);
		break;

	case EV_REL:
		__set_bit(code, dev->relbit);
		break;

	case EV_ABS:
		input_alloc_absinfo(dev);
		if (!dev->absinfo)
			return;

		__set_bit(code, dev->absbit);
		break;

	case EV_MSC:
		__set_bit(code, dev->mscbit);
		break;

	case EV_SW:
		__set_bit(code, dev->swbit);
		break;

	case EV_LED:
		__set_bit(code, dev->ledbit);
		break;

	case EV_SND:
		__set_bit(code, dev->sndbit);
		break;

	case EV_FF:
		__set_bit(code, dev->ffbit);
		break;

	case EV_PWR:
		/* do nothing */
		break;

	default:
		pr_err("%s: unknown type %u (code %u)\n", __func__, type, code);
		dump_stack();
		return;
	}

	__set_bit(type, dev->evbit);
}
EXPORT_SYMBOL(input_set_capability);

static unsigned int input_estimate_events_per_packet(struct input_dev *dev)
{
	int mt_slots;
	int i;
	unsigned int events;

	if (dev->mt) {
		mt_slots = dev->mt->num_slots;
	} else if (test_bit(ABS_MT_TRACKING_ID, dev->absbit)) {
		mt_slots = dev->absinfo[ABS_MT_TRACKING_ID].maximum -
			   dev->absinfo[ABS_MT_TRACKING_ID].minimum + 1,
		mt_slots = clamp(mt_slots, 2, 32);
	} else if (test_bit(ABS_MT_POSITION_X, dev->absbit)) {
		mt_slots = 2;
	} else {
		mt_slots = 0;
	}

	events = mt_slots + 1; /* count SYN_MT_REPORT and SYN_REPORT */

	if (test_bit(EV_ABS, dev->evbit))
		for_each_set_bit(i, dev->absbit, ABS_CNT)
			events += input_is_mt_axis(i) ? mt_slots : 1;

	if (test_bit(EV_REL, dev->evbit))
		events += bitmap_weight(dev->relbit, REL_CNT);

	/* Make room for KEY and MSC events */
	events += 7;

	return events;
}

#define INPUT_CLEANSE_BITMASK(dev, type, bits)				\
	do {								\
		if (!test_bit(EV_##type, dev->evbit))			\
			memset(dev->bits##bit, 0,			\
				sizeof(dev->bits##bit));		\
	} while (0)

static void input_cleanse_bitmasks(struct input_dev *dev)
{
	INPUT_CLEANSE_BITMASK(dev, KEY, key);
	INPUT_CLEANSE_BITMASK(dev, REL, rel);
	INPUT_CLEANSE_BITMASK(dev, ABS, abs);
	INPUT_CLEANSE_BITMASK(dev, MSC, msc);
	INPUT_CLEANSE_BITMASK(dev, LED, led);
	INPUT_CLEANSE_BITMASK(dev, SND, snd);
	INPUT_CLEANSE_BITMASK(dev, FF, ff);
	INPUT_CLEANSE_BITMASK(dev, SW, sw);
}

static void __input_unregister_device(struct input_dev *dev)
{
	struct input_handle *handle, *next;

	input_disconnect_device(dev);

	mutex_lock(&input_mutex);

	list_for_each_entry_safe(handle, next, &dev->h_list, d_node)
		handle->handler->disconnect(handle);
	WARN_ON(!list_empty(&dev->h_list));

	del_timer_sync(&dev->timer);
	list_del_init(&dev->node);

	input_wakeup_procfs_readers();

	mutex_unlock(&input_mutex);

	device_del(&dev->dev);
}

static void devm_input_device_unregister(struct device *dev, void *res)
{
	struct input_devres *devres = res;
	struct input_dev *input = devres->input;

	dev_dbg(dev, "%s: unregistering device %s\n",
		__func__, dev_name(&input->dev));
	__input_unregister_device(input);
}

/**
 * input_enable_softrepeat - enable software autorepeat
 * @dev: input device
 * @delay: repeat delay
 * @period: repeat period
 *
 * Enable software autorepeat on the input device.
 * 
 * input_enable_softrepeat - 启用软件自动重复
 * @dev: 输入设备
 * @delay: 重复延迟
 * @period: 重复周期
 *
 * 在输入设备上启用软件自动重复功能。
 *  */
void input_enable_softrepeat(struct input_dev *dev, int delay, int period)
{
	dev->timer.function = input_repeat_key;
	dev->rep[REP_DELAY] = delay;
	dev->rep[REP_PERIOD] = period;
}
EXPORT_SYMBOL(input_enable_softrepeat);

/**
 * input_register_device - register device with input core
 * @dev: device to be registered
 *
 * This function registers device with input core. The device must be
 * allocated with input_allocate_device() and all it's capabilities
 * set up before registering.
 * If function fails the device must be freed with input_free_device().
 * Once device has been successfully registered it can be unregistered
 * with input_unregister_device(); input_free_device() should not be
 * called in this case.
 *
 * Note that this function is also used to register managed input devices
 * (ones allocated with devm_input_allocate_device()). Such managed input
 * devices need not be explicitly unregistered or freed, their tear down
 * is controlled by the devres infrastructure. It is also worth noting
 * that tear down of managed input devices is internally a 2-step process:
 * registered managed input device is first unregistered, but stays in
 * memory and can still handle input_event() calls (although events will
 * not be delivered anywhere). The freeing of managed input device will
 * happen later, when devres stack is unwound to the point where device
 * allocation was made.
 * 
 * input_register_device - 向输入核心注册设备
 * @dev: 要注册的设备
 *
 * 此函数将设备注册到输入核心。设备必须事先通过 input_allocate_device()
 * 分配，并在注册前设置好其所有能力。
 * 如果函数失败，必须使用 input_free_device() 释放设备。
 * 一旦设备成功注册，就可以通过 input_unregister_device() 注销；
 * 在这种情况下不应调用 input_free_device()。
 *
 * 请注意，此函数也用于注册托管输入设备（通过
 * devm_input_allocate_device() 分配的设备）。此类托管输入设备
 * 无需显式注销或释放，它们的清理由 devres 基础框架控制。值得
 * 注意的是，托管输入设备的清理在内部是一个两步过程：注册的托管
 * 输入设备首先被注销，但仍保留在内存中，仍然可以处理
 * input_event() 调用（尽管事件不会被传递到任何地方）。托管输入
 * 设备的释放将在稍后进行，届时 devres 栈会回卷到该设备被分配
 * 的位置。
 */

int input_register_device(struct input_dev *dev)
{
	struct input_devres *devres = NULL;
	struct input_handler *handler;
	unsigned int packet_size;
	const char *path;
	int error;

	if (test_bit(EV_ABS, dev->evbit) && !dev->absinfo) {
		dev_err(&dev->dev,
			"Absolute device without dev->absinfo, refusing to register\n");
		return -EINVAL;
	}

	if (dev->devres_managed) {
		devres = devres_alloc(devm_input_device_unregister,
				      sizeof(*devres), GFP_KERNEL);
		if (!devres)
			return -ENOMEM;

		devres->input = dev;
	}

	/* Every input device generates EV_SYN/SYN_REPORT events. */
	__set_bit(EV_SYN, dev->evbit);

	/* KEY_RESERVED is not supposed to be transmitted to userspace. */
	__clear_bit(KEY_RESERVED, dev->keybit);

	/* Make sure that bitmasks not mentioned in dev->evbit are clean. */
	input_cleanse_bitmasks(dev);

	packet_size = input_estimate_events_per_packet(dev);
	if (dev->hint_events_per_packet < packet_size)
		dev->hint_events_per_packet = packet_size;

	dev->max_vals = dev->hint_events_per_packet + 2;
	dev->vals = kcalloc(dev->max_vals, sizeof(*dev->vals), GFP_KERNEL);
	if (!dev->vals) {
		error = -ENOMEM;
		goto err_devres_free;
	}

	/*
	 * If delay and period are pre-set by the driver, then autorepeating
	 * is handled by the driver itself and we don't do it in input.c.
	 */
	if (!dev->rep[REP_DELAY] && !dev->rep[REP_PERIOD])
		input_enable_softrepeat(dev, 250, 33);

	if (!dev->getkeycode)
		dev->getkeycode = input_default_getkeycode;

	if (!dev->setkeycode)
		dev->setkeycode = input_default_setkeycode;

	error = device_add(&dev->dev);
	if (error)
		goto err_free_vals;

	path = kobject_get_path(&dev->dev.kobj, GFP_KERNEL);
	pr_info("%s as %s\n",
		dev->name ? dev->name : "Unspecified device",
		path ? path : "N/A");
	kfree(path);

	error = mutex_lock_interruptible(&input_mutex);
	if (error)
		goto err_device_del;

	list_add_tail(&dev->node, &input_dev_list);

	list_for_each_entry(handler, &input_handler_list, node)
		input_attach_handler(dev, handler);

	input_wakeup_procfs_readers();

	mutex_unlock(&input_mutex);

	if (dev->devres_managed) {
		dev_dbg(dev->dev.parent, "%s: registering %s with devres.\n",
			__func__, dev_name(&dev->dev));
		devres_add(dev->dev.parent, devres);
	}
	return 0;

err_device_del:
	device_del(&dev->dev);
err_free_vals:
	kfree(dev->vals);
	dev->vals = NULL;
err_devres_free:
	devres_free(devres);
	return error;
}
EXPORT_SYMBOL(input_register_device);

/**
 * input_unregister_device - unregister previously registered device
 * @dev: device to be unregistered
 *
 * This function unregisters an input device. Once device is unregistered
 * the caller should not try to access it as it may get freed at any moment.
 */
void input_unregister_device(struct input_dev *dev)
{
	if (dev->devres_managed) {
		WARN_ON(devres_destroy(dev->dev.parent,
					devm_input_device_unregister,
					devm_input_device_match,
					dev));
		__input_unregister_device(dev);
		/*
		 * We do not do input_put_device() here because it will be done
		 * when 2nd devres fires up.
		 */
	} else {
		__input_unregister_device(dev);
		input_put_device(dev);
	}
}
EXPORT_SYMBOL(input_unregister_device);

/**
 * input_register_handler - register a new input handler
 * @handler: handler to be registered
 *
 * This function registers a new input handler (interface) for input
 * devices in the system and attaches it to all input devices that
 * are compatible with the handler.
 */
int input_register_handler(struct input_handler *handler)
{
	struct input_dev *dev;
	int error;

	error = mutex_lock_interruptible(&input_mutex);
	if (error)
		return error;

	INIT_LIST_HEAD(&handler->h_list);

	list_add_tail(&handler->node, &input_handler_list);

	list_for_each_entry(dev, &input_dev_list, node)
		input_attach_handler(dev, handler);

	input_wakeup_procfs_readers();

	mutex_unlock(&input_mutex);
	return 0;
}
EXPORT_SYMBOL(input_register_handler);

/**
 * input_unregister_handler - unregisters an input handler
 * @handler: handler to be unregistered
 *
 * This function disconnects a handler from its input devices and
 * removes it from lists of known handlers.
 * 
 * input_unregister_handler - 注销一个输入处理程序
 * @handler: 要注销的处理程序
 *
 * 此函数用于断开该处理程序与其关联输入设备的连接，
 * 并将其从已知处理程序列表中移除。 * 
 */
void input_unregister_handler(struct input_handler *handler)
{
	struct input_handle *handle, *next;

	mutex_lock(&input_mutex);

	list_for_each_entry_safe(handle, next, &handler->h_list, h_node)
		handler->disconnect(handle);
	WARN_ON(!list_empty(&handler->h_list));

	list_del_init(&handler->node);

	input_wakeup_procfs_readers();

	mutex_unlock(&input_mutex);
}
EXPORT_SYMBOL(input_unregister_handler);

/**
 * input_handler_for_each_handle - handle iterator
 * @handler: input handler to iterate
 * @data: data for the callback
 * @fn: function to be called for each handle
 *
 * Iterate over @bus's list of devices, and call @fn for each, passing
 * it @data and stop when @fn returns a non-zero value. The function is
 * using RCU to traverse the list and therefore may be using in atomic
 * contexts. The @fn callback is invoked from RCU critical section and
 * thus must not sleep.
 * 
 * input_handler_for_each_handle - 句柄迭代器
 * @handler: 要遍历的输入处理程序
 * @data: 传递给回调函数的数据
 * @fn: 为每个句柄调用的回调函数
 *
 * 遍历与 @handler 相关联的设备列表，对每个设备调用 @fn，
 * 并将 @data 传递给它；当 @fn 返回非零值时停止遍历。
 * 该函数使用 RCU 遍历列表，因此可以在原子上下文中使用。
 * @fn 回调在 RCU 临界区内被调用，因此不能睡眠。 * 
 */
int input_handler_for_each_handle(struct input_handler *handler, void *data,
				  int (*fn)(struct input_handle *, void *))
{
	struct input_handle *handle;
	int retval = 0;

	rcu_read_lock();

	list_for_each_entry_rcu(handle, &handler->h_list, h_node) {
		retval = fn(handle, data);
		if (retval)
			break;
	}

	rcu_read_unlock();

	return retval;
}
EXPORT_SYMBOL(input_handler_for_each_handle);

/**
 * input_register_handle - register a new input handle
 * @handle: handle to register
 *
 * This function puts a new input handle onto device's
 * and handler's lists so that events can flow through
 * it once it is opened using input_open_device().
 *
 * This function is supposed to be called from handler's
 * connect() method.
 * 
 * input_register_handle - 注册一个新的输入句柄
 * @handle: 要注册的句柄
 *
 * 此函数将一个新建的输入句柄挂入设备链表和处理程序链表，
 * 这样一旦通过 input_open_device() 打开该句柄，
 * 事件便能经由它进行传递。
 *
 * 此函数应该在处理程序的 connect() 方法中被调用。 * 
 */
int input_register_handle(struct input_handle *handle)
{
	struct input_handler *handler = handle->handler;
	struct input_dev *dev = handle->dev;
	int error;

	/*
	 * We take dev->mutex here to prevent race with
	 * input_release_device().
	 */
	error = mutex_lock_interruptible(&dev->mutex);
	if (error)
		return error;

	/*
	 * Filters go to the head of the list, normal handlers
	 * to the tail.
	 */
	if (handler->filter)
		list_add_rcu(&handle->d_node, &dev->h_list);
	else
		list_add_tail_rcu(&handle->d_node, &dev->h_list);

	mutex_unlock(&dev->mutex);

	/*
	 * Since we are supposed to be called from ->connect()
	 * which is mutually exclusive with ->disconnect()
	 * we can't be racing with input_unregister_handle()
	 * and so separate lock is not needed here.
	 *  * 由于我们预期是从 ->connect() 中被调用，
 	* 而该操作与 ->disconnect() 是互斥的，
	 * 所以我们不可能与 input_unregister_handle() 发生竞态，
	 * 因此这里不需要单独的锁。
	 * 
	 */
	list_add_tail_rcu(&handle->h_node, &handler->h_list);

	if (handler->start)
		handler->start(handle);

	return 0;
}
EXPORT_SYMBOL(input_register_handle);

/**
 * input_unregister_handle - unregister an input handle
 * @handle: handle to unregister
 *
 * This function removes input handle from device's
 * and handler's lists.
 *
 * This function is supposed to be called from handler's
 * disconnect() method.
 * 
 * input_unregister_handle - 注销一个输入句柄
 * @handle: 要注销的句柄
 *
 * 此函数将输入句柄从设备链表和处理程序链表中移除。
 *
 * 此函数应该在处理程序的 disconnect() 方法中被调用。 
 * 但是这不意味着handle这个结构体就被释放了，就很奇怪，如果是这样的话，
 * 我们要么在disconnect里面释放这个结构体，要么就在对应的handler把已经申请的但是Unregister
 * 的handle结构体用list连接起来，然后用handle.open来标记这个handle
 * * 
 */

void input_unregister_handle(struct input_handle *handle)
{
	struct input_dev *dev = handle->dev;

	list_del_rcu(&handle->h_node);

	/*
	 * Take dev->mutex to prevent race with input_release_device().
	 */
	mutex_lock(&dev->mutex);
	list_del_rcu(&handle->d_node);
	mutex_unlock(&dev->mutex);

	synchronize_rcu();
}
EXPORT_SYMBOL(input_unregister_handle);

/**
 * input_get_new_minor - allocates a new input minor number
 * @legacy_base: beginning or the legacy range to be searched
 * @legacy_num: size of legacy range
 * @allow_dynamic: whether we can also take ID from the dynamic range
 *
 * This function allocates a new device minor for from input major namespace.
 * Caller can request legacy minor by specifying @legacy_base and @legacy_num
 * parameters and whether ID can be allocated from dynamic range if there are
 * no free IDs in legacy range.
 * 
 * input_get_new_minor - 分配一个新的输入次设备号
 * @legacy_base: 要搜索的预留区间的起始值
 * @legacy_num: 预留区间的大小
 * @allow_dynamic: 是否允许从动态区间获取 ID
 *
 * 此函数从输入主设备号对应的命名空间中分配一个新的次设备号。
 * 调用者可以通过指定 @legacy_base 和 @legacy_num 参数来请求预留次设备号，
 * 同时指定在预留区间内没有空闲 ID 时，是否允许从动态区间进行分配。 * 
 */
int input_get_new_minor(int legacy_base, unsigned int legacy_num,
			bool allow_dynamic)
{
	/*
	 * This function should be called from input handler's ->connect()
	 * methods, which are serialized with input_mutex, so no additional
	 * locking is needed here.
	 */
	if (legacy_base >= 0) {
		int minor = ida_simple_get(&input_ida,
					   legacy_base,
					   legacy_base + legacy_num,
					   GFP_KERNEL);
		if (minor >= 0 || !allow_dynamic)
			return minor;
	}

	return ida_simple_get(&input_ida,
			      INPUT_FIRST_DYNAMIC_DEV, INPUT_MAX_CHAR_DEVICES,
			      GFP_KERNEL);
}
EXPORT_SYMBOL(input_get_new_minor);

/**
 * input_free_minor - release previously allocated minor
 * @minor: minor to be released
 *
 * This function releases previously allocated input minor so that it can be
 * reused later.
 * 
 * input_free_minor - 释放之前分配的次设备号
 * @minor: 要释放的次设备号
 *
 * 此函数释放之前分配的输入次设备号，以便之后可以重新使用。 * 
 */
void input_free_minor(unsigned int minor)
{
	ida_simple_remove(&input_ida, minor);
}
EXPORT_SYMBOL(input_free_minor);

static int __init input_init(void)
{
	int err;

	err = class_register(&input_class);
	if (err) {
		pr_err("unable to register input_dev class\n");
		return err;
	}

	err = input_proc_init();
	if (err)
		goto fail1;

	err = register_chrdev_region(MKDEV(INPUT_MAJOR, 0),
				     INPUT_MAX_CHAR_DEVICES, "input");
	if (err) {
		pr_err("unable to register char major %d", INPUT_MAJOR);
		goto fail2;
	}

	return 0;

 fail2:	input_proc_exit();
 fail1:	class_unregister(&input_class);
	return err;
}

static void __exit input_exit(void)
{
	input_proc_exit();
	unregister_chrdev_region(MKDEV(INPUT_MAJOR, 0),
				 INPUT_MAX_CHAR_DEVICES);
	class_unregister(&input_class);
}

subsys_initcall(input_init);
module_exit(input_exit);
