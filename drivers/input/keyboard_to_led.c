#include <linux/module.h>
#include <linux/input.h>
#include <linux/leds.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/mod_devicetable.h>
#include <linux/bitops.h>
#include <linux/cdev.h>
#include <linux/kernel.h>   /* 提供 va_* 系列宏、vscnprintf()、WARN()、PAGE_SIZE */
#include <linux/mm.h>       /* 提供 offset_in_page() */
#include <drivers/leds/leds.h> 
#include <linux/idr.h>
#include <linux/types.h>


#define pr_fmt(fmt)  "%s:%s: " fmt,KBUILD_MODNAME,__func__ 
static struct ida id_for_ktl_device;

//这个最终版本应该是应用层通过sys文件系统来选择哪个led可以亮，然后还可以选择对应的led亮灭机制。那样就很成熟了
//对于每一个输入而且是键盘设备都会注册一个这样的handler
//TODO:将cdev去掉，换成属性，因为cdev实际上包含数据交换，像这种切换led这种更像属性才对

struct kbd_to_led {

    struct device dev;               //实际上kbd_to_led可以理解成虚拟的控制设备。
    struct input_handle handle;
    struct led_classdev *led_cdev;   //通过led子系统操作led灯
    int which_led;
    int open;   //用来控制这个handler是否打开，打开了才会响应事件
    struct ida *id_for_device;
};


/**
 *	sysfs_emit - scnprintf equivalent, aware of PAGE_SIZE buffer.
 *	@buf:	start of PAGE_SIZE buffer.
 *	@fmt:	format
 *	@...:	optional arguments to @format
 *
 *
 * Returns number of characters written to @buf.
 * 
 *  * sysfs_emit - 与 scnprintf 等价，但能感知 PAGE_SIZE 缓冲区。
 * @buf: PAGE_SIZE 缓冲区的起始地址。
 * @fmt: 格式化字符串。
 * @...: 传递给 @fmt 的可选参数。
 *
 *
 * 返回值是写入 @buf 的字符数。
 */
static int my_sysfs_emit(char *buf, const char *fmt, ...)
{
	va_list args;
	int len;

	if (WARN(!buf || offset_in_page(buf),
		 "invalid sysfs_emit: buf:%p\n", buf))
		return 0;

	va_start(args, fmt);
	len = vscnprintf(buf, PAGE_SIZE, fmt, args);
	va_end(args);

	return len;
}


static int get_led_num(struct kbd_to_led *ktl)
{
    struct led_classdev *led_cdev = ktl->led_cdev;
    int count = 0;
    down_read(&leds_list_lock);
    list_for_each_entry(led_cdev,&leds_list,node)
    count++;
    up_read(&leds_list_lock);
    return count;
}



static int select_led(struct kbd_to_led *ktl,int which_one)
{
    struct led_classdev *led_cdev;
    int led_num = 0;
    int index = 0;
    if(!(led_num = get_led_num(ktl)))
        return -ENODEV;

    which_one = which_one % (led_num+1);
    down_read(&leds_list_lock);
    list_for_each_entry(led_cdev,&leds_list,node)
    {
        index++;
        if(index == which_one)
        {
           break; 
        }
    }
    up_read(&leds_list_lock);

    ktl->led_cdev = led_cdev;
    ktl->which_led = index;
    return 0;
}


static ssize_t ktl_select_led(struct device *dev, 
                                struct device_attribute *attr, 
                                char *buf,
                                size_t count)
{
    struct kbd_to_led *ktl = container_of(dev,struct kbd_to_led,dev);
    int ret = 0;
    bool result = 0;

    ret = kstrtobool(buf,&result);

    if(ret < 0)
    {
        pr_debug("Invalid input format: %s\n", buf);
        return ret;
    }

    ret = select_led(ktl,result);
    if(ret < 0)
    {
        pr_debug("Failed to select LED\n");
        return (ssize_t)ret;
    }

    return count;
}

static ssize_t ktl_get_led(struct device *dev, 
                            struct device_attribute *attr, 
                            const char *buf)
{
    struct kbd_to_led *ktl = container_of(dev,struct kbd_to_led,dev);
    // struct led_classdev *led_cdev;
    // int count = 0;
    // int err;
    // //char *buf_bak = buf;
    // down_read(&leds_list_lock);
    // list_for_each_entry(led_cdev,&leds_list,node)
    // //{
    // //     ret = scnprintf(buf_bak,PAGE_SIZE,"%s ",led_cdev->dev.kobj.name);
    // //     buf_bak += ret;
    // //     if(buf_bak - buf > (PAGE_SIZE -1))
    // //     {
    // //         return -ENOSPC;
    // //     }
    // // }
    // count++;
    // up_read(&leds_list_lock);
    scnprintf(buf,PAGE_SIZE,"The LED selected for this device is: %d,The number of LEDs recognized by the system is:%d\n",ktl->which_led,get_led_num());

    return strlen(buf);
}

static ssize_t ktl_get_help(struct device *dev, 
                            struct device_attribute *attr, 
                            const char *buf)
{
    return my_sysfs_emit(buf, "%s\n", "Please read the led_select file to get the number of LED devices in the system,\
                                        with spaces as the delimiter. \
                                        To select an LED, use 1, 2, or 3; only one value can be written at a time.");
}


static int ktl_open_device(struct kbd_to_led *ktl)
{
    if(ktl->open)
    {
       return input_open_device(ktl->handle->dev);
    }
}


static void ktl_close_device(struct kbd_to_led *ktl)
{
    if(!ktl->open)
    {
        return input_close_device(ktl->handle->dev);
    }
}

//值得一提的是，sys文件的读写操作是串行的。
static ssize_t state_store(struct device *dev, 
                                struct device_attribute *attr, 
                                char *buf,
                                size_t count)
{
    struct kbd_to_led *ktl = container_of(dev,struct kbd_to_led,dev);
    int ret = 0;
    unsigned long result = 0;

    ret = kstrtoul(buf, 10, &result);

    if(ret < 0)
    {
        return ret;
    }

    if(ktl->open)
    {
        if(result == 0)
        {
            ktl->open = 0;
            ktl_close_device(ktl);

        }
    }
    else
    {
        if(result !=0)
        {
            ktl->open = 1;
            int ret = ktl_open_device(ktl);
            if(ret < 0)
            {
                pr_debug("Failed to open ktl\n");
                ktl->open = 0;
                return ret;
            }
        }
    }

    return (ssize_t)count;

}

static ssize_t state_show(struct device *dev, 
                            struct device_attribute *attr, 
                            const char *buf)
{
    struct kbd_to_led *ktl = container_of(dev,struct kbd_to_led,dev);

    my_sysfs_emit(buf, "The state of kbd_to_led is %d\n",ktl->open);


    return (ssize_t)strlen(buf);
}

static DEVICE_ATTR(led_select, 0744, ktl_get_led,ktl_select_led);
static DEVICE_ATTR(help,0444,ktl_get_help,NULL);
static DEVICE_ATTR_RW(state);


static struct attribute *ktl_attrs[] = {
    &dev_attr_led_select.attr,
    &dev_attr_help.attr,
    &dev_attr_state.attr,
    NULL
};

static const struct attribute_group ktl_attr_group = {
    .attrs = ktl_attrs,
};

static struct attribute_group *ktl_attr_group[] = {
    &ktl_attr_group,
    NULL
};



static void ktl_dev_release(struct device *dev){

    struct kbd_to_led *ktl = container_of(dev,struct kbd_to_led,dev);
    input_put_device(ktl->handle.dev);//这个要小心
    kfree(ktl);
}




//记得返回对应的错误代码
static int kbd_to_led_connect(struct input_handler *handler, 
                                    struct input_dev *dev, 
                                    const struct input_device_id *id)
{
    struct kbd_to_led *ktl;
    int ret = 0;

    ktl = kzalloc(sizeof(struct kbd_to_led), GFP_KERNEL); //我还没弄懂这个devm_,所以就没有使用devm_
    if(!ktl)
        return -ENOMEM;

    ktl->id_for_device = &id_for_ktl_device;
    //等下需要改一下，这个不是给ret的，而是给device.id的，然后弄清楚id是怎么free的
   
    ktl->dev.groups = ktl_attr_group;
    ktl->dev.release = ktl_dev_release;

    ktl->handle.dev = input_get_device(dev);
    ktl->handle.handler = handler;
    ktl->handle.name = "keyboard_to_led_handle";
    ktl->handle.private = ktl;
    ktl->handle.open = 0;

    ktl->dev.id = ida_alloc_min(ktl->id_for_device, 0, GFP_KERNEL);
    if(ktl->dev.id < 0)
    {
       pr_debug("Failed to allocate id for device\n");
       goto free_ktl;
    }
    dev_set_name(&ktl->dev, "keyboard_to_led%d", ktl->dev.id);

    ret = device_register(ktl->dev);
    if(ret < 0)
    {
        pr_debug("Failed to register device\n");
        goto free_ida;
    }

    ret = input_register_handle(&ktl->handle);
    if(ret < 0)
    {
        pr_debug("Failed to register input handle\n");
        goto free_device;
    }

    ret = select_led(ktl,1);//这里的select_led是不是需要把led_classdev增加一下引用？

    if(ret < 0)
    {
        pr_debug("NO led exists\n");
        goto free_handle;
    }

    return ret;
    free_handle:
        input_unregister_handle(&ktl->handle);
    free_device:
        device_del(&ktl->dev);
    free_ida:
        ida_free(ktl->id_for_device, ktl->dev.id);
    free_ktl:
        put_device(&ktl->dev);
    return ret;

}


/* 如果说kbd_to_led_connect是注册handle的话，那么disconnect就是注销handle了,不仅仅只是释放handle
 * 还要释放所有的connect申请资源的以及全局资源 
*/
static void kbd_to_led_disconnect(struct input_handle *handle)
{
    struct kbd_to_led *ktl = handle->private;

    input_unregister_handle(&ktl->handle);
    ida_free(ktl->id_for_device, ktl->dev.id);
    unresigter_device(&ktl->dev);
}



//TODO: 传给handler 的event 回调函数，然后在这里面需要实现打开led
//这里面注意需要知道我们ktl是有open的，我们需要在event里面使用这个
static void kbd_to_led_event(struct input_handle *handle, 
                                    unsigned int type,
                                    unsigned int code, int value)
{
    struct kbd_to_led *ktl = handle->private;
    struct led_classdev *led = ktl->led_cdev;
    int brightness = 0;
    
    if(type == EV_KEY) //过滤掉EV_SYN,确实可以用filter函数来过滤一些事件
    {
        if(led->brightness_set != NULL) //高优先级
        {
           led_set_brightness_nosleep(led,(brightness == LED_OFF) ? LED_ON : LED_OFF);
        }
        else if(led->brightness_set_blocking != NULL)
        {
            int ret = led_set_brightness_sync(led,(brightness == LED_OFF) ? LED_ON : LED_OFF);
            if(ret < 0)
            {
                pr_debug("Failed to set brightness\n");
            }
        }
    }

}



//TODO: 了解一下input_device_id 是如何填充才可以被正确匹配的
static struct input_device_id kbd_to_led_ids[] = {
    { 
        .flags = INPUT_DEVICE_ID_MATCH_BUS | INPUT_DEVICE_ID_MATCH_EVBIT | INPUT_DEVICE_ID_MATCH_KEYBIT,
        .bustype = BUS_USB,
        .evbit = {BIT(EV_KEY)},
    },	        /* Matches all devices */
    { }			/* Terminating zero entry */
};

MODULE_DEVICE_TABLE(input, kbd_to_led_ids);


static struct input_handler kbd_to_led_handler = {
    
    .event = kbd_to_led_event,
    .connect = kbd_to_led_connect,
    .disconnect = kbd_to_led_disconnect,
    .name = "kbd_to_led",
    .id_table = &kbd_to_led_ids,

};



static int __init kbd_to_led_init()
{
    ida_init(&id_for_ktl_device);
    return input_register_handler(&kbd_to_led_handler);
} 


static void __exit kbd_to_led_exit()
{
    input_unregister_handler(&kbd_to_led_handler);
    ida_destroy(&id_for_ktl_device);
}




module_init(keyboard_to_led_init);
module_exit(keyboard_to_led_exit);

MODULE_AUTHOR("HDOO");
MODULE_LICENSE("GPL");
