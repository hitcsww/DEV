#include <asm/io.h>
#include <asm/system.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/joystick.h>
#include <linux/input.h>
#include <linux/kernel.h>
#include <linux/major.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/sched.h>
#include <linux/signal.h>

MODULE_LICENSE("GPL");//加载到内核的形式

#define JOYDEV_MINOR_BASE	0  //次设备号的基值
#define JOYDEV_MINORS		16 //次设备号的范围
#define JOYDEV_BUFFER_SIZE	64//缓冲区长度

#define false			0
#define true			1

#define EDGE_MAX		32767    //摇杆最右边距离  
#define EDGE_MIN	       -32768    //最左距离（两个字节表示的十进制数范围）

struct joy_event{        //存放的鼠标事件
	int dx, dy;     //移动相对距离
	bool rel_event;   //是否是相对位移事件
	unsigned long buttons;   //存放按键数据
};

struct joydev {          //处理事件的对象
	int exist;       //表示对象有误
	int open;        //表示设备节点打开次数
	int minor;       //次设备号
	struct input_handle handle;    //存放joy与js中input_dev匹配成功的信息
	wait_queue_head_t wait;         //存放等待进程的链表
	struct list_head client_list;    //joy_client的链表
	spinlock_t client_lock;  //锁   保护链表，只允许一个进程读写
	struct mutex mutex;      //信号量
	struct device dev;         

	struct joy_event packet;  
};

struct joydev_client {    //事件缓冲区   （与joydev对应）
	struct joy_event buffer[JOYDEV_BUFFER_SIZE];   //缓冲区
	int head;//头
	int tail;

	spinlock_t buffer_lock; //保护缓冲区的锁
	struct fasync_struct *fasync;//异步通知应用程序读缓冲区的结构体
	struct joydev *joydev;
	struct list_head node;
	int ready;
};

static struct joydev *joydev_table[JOYDEV_MINORS];  //声明一个数组     存次设备号对应的结构体
static DEFINE_MUTEX(joydev_table_mutex);  //枷锁

static void joydev_key_event(struct joydev *joydev,    //处理位移
				unsigned int code, int value)   //code按键类型   value按键状态      
{
	int index;
	switch (code) {
	case BTN_A:		index = 0; break;
	case BTN_B:		index = 1; break;
	case BTN_X:		index = 2; break;
	case BTN_Y:		index = 3; break;
	case BTN_THUMBL:	index = 4; break; //摇杆按键
	case BTN_THUMBR:	index = 5; break; //
	case BTN_START:		index = 6; break;
	case BTN_BACK:		index = 7; break;
	default:		return;
	}
	if(value)	//按下置一
                  set_bit(index,&joydev->packet.buttons);   //置1     第（index+1）位，置1      index由0开始
	else		clear_bit(index,&joydev->packet.buttons);  //置0
}

static void joydev_rel_event(struct joydev *joydev,
				unsigned int code, int value)  //处理位移    code位移类型  value位移数值
{
	switch(code){
		case REL_X:
			joydev->packet.dx = value >= EDGE_MAX ?   //判断数据是否有效  >=32767有效     <=-32768   有效
				2 : (value <= EDGE_MIN ? -2 : 0);  //  1表示移动距离（向右）  -1距离、向左   0  无动作
			break;
		case REL_Y:
			joydev->packet.dy = value >= EDGE_MAX ?
				2 : (value <= EDGE_MIN ? -2 : 0);
			break;
			
	}
	
	if(joydev->packet.dx || joydev->packet.dy)	 //判断是否相对位移
		joydev->packet.rel_event = true;
	else	joydev->packet.rel_event = false;
}

static int cmp(struct joy_event *p, struct joy_event *packet){   
	if(packet->rel_event)	return true;   //判断是否是位移事件
	if(p->buttons!=packet->buttons)	return true;   //不是位移事件   与前一次按键值（0、1、2、4、8）比较
	return false;//表示一直按着
}

static void joydev_notify_readers(struct joydev *joydev,
				    struct joy_event *packet)  //通知应用程序读缓冲区
{
	struct joydev_client *client;  
	struct joy_event *p;
	unsigned int new_head;//控制指针的位置
	int wake_readers = 0;    //1  唤醒进程  

	rcu_read_lock();//？
	list_for_each_entry_rcu(client, &joydev->client_list, node) {

		spin_lock(&client->buffer_lock);//锁缓冲区

		p = &client->buffer[client->head];//取前一次事件
		if (cmp(p,packet)) {
			new_head = (client->head+1) % JOYDEV_BUFFER_SIZE;  //后移1位
			if (new_head != client->tail) {   //不满
				p = &client->buffer[client->head = new_head];   //取移后的空位地址
				memset(p, 0, sizeof(struct joy_event));  //初始
				client->ready = 1;  //当前缓冲区是否可读
			}	
		}
		p->dx = packet->dx;
		p->dy = packet->dy;
		p->rel_event = packet->rel_event;
		p->buttons = packet->buttons;
		
		if (client->ready && client->head!=client->tail) {
			
			kill_fasync(&client->fasync, SIGIO, POLL_IN); //发送SIGIO信号给应用程序
			wake_readers = 1;
		}
		spin_unlock(&client->buffer_lock);  //开锁
	}
	rcu_read_unlock();//？
	
	if(wake_readers)	wake_up_interruptible(&joydev->wait); //唤醒读得进程
}

static void joydev_event(struct input_handle *handle,
			 unsigned int type, unsigned int code, int value)
{
	struct joydev *joydev = handle->private;
	switch (type) {

	case EV_REL:
		joydev_rel_event(joydev, code, value);
		break;

	case EV_KEY:
		
		if(value!=2)	joydev_key_event(joydev, code, value);
		break;

	case EV_SYN://结束标志  无动作了0000
		if (code == SYN_REPORT) {
			joydev_notify_readers(joydev, &joydev->packet);
			joydev->packet.dx = 0;
			joydev->packet.dy = 0;
			joydev->packet.rel_event = 0;
		}
		break;
	}
}


static int joydev_fasync(int fd, struct file *file, int on)   //？
{
	struct joydev_client *client = file->private_data;

	return fasync_helper(fd, file, on, &client->fasync);
}

static void joydev_free(struct device *dev)//断开js与joydev在input_dev的链接，减少引用次数      
{
	struct joydev *joydev = container_of(dev, struct joydev, dev);//？

	input_put_device(joydev->handle.dev);  ///？
	kfree(joydev);  //减少引用次数
}

static void joydev_attach_client(struct joydev *joydev,
				 struct joydev_client *client)     //每打开一个设备节点就会生成一个client    attach将client加到链表里
{
	spin_lock(&joydev->client_lock);
	list_add_tail_rcu(&client->node, &joydev->client_list); //加client到链表里
	spin_unlock(&joydev->client_lock);
	synchronize_rcu();  //？
}

static void joydev_detach_client(struct joydev *joydev,
				 struct joydev_client *client)  //从链表里删掉client
{
	spin_lock(&joydev->client_lock);
	list_del_rcu(&client->node);   //delete
	spin_unlock(&joydev->client_lock);
	synchronize_rcu();
}

static int joydev_open_device(struct joydev *joydev)  //打开设备
{
	int retval;

	retval = mutex_lock_interruptible(&joydev->mutex);  //枷锁  信号量

	if (retval)
		return retval;

	if (!joydev->exist)   //
		retval = -ENODEV;
	else if (!joydev->open++) {   //open  打开设备节点次数    为0 
		retval = input_open_device(&joydev->handle);  //打开设备
		if (retval)
			joydev->open--;  //如果没打开  减减
	}

	mutex_unlock(&joydev->mutex);  //开锁
	return retval;
}

static void joydev_close_device(struct joydev *joydev)//与release共同实现关闭设备节点功能
{
	mutex_lock(&joydev->mutex);  //枷锁

	if (joydev->exist && !--joydev->open)  //存在   且只有一个应用程序引用
		input_close_device(&joydev->handle);

	mutex_unlock(&joydev->mutex);
}

static int joydev_release(struct inode *inode, struct file *file)  //关闭设备节点
{
	struct joydev_client *client = file->private_data;  //取client地址
	struct joydev *joydev = client->joydev;  //取client存的joydev事件处理对象结构体

	joydev_detach_client(joydev, client);  //从链表删除client
	kfree(client);//减少引用次数

	joydev_close_device(joydev);
	put_device(&joydev->dev);//减少引用次数

	return 0;
}

static int joydev_open(struct inode *inode, struct file *file)    //  inode 设备号
{
	struct joydev_client *client;
	struct joydev *joydev;

	int i = iminor(inode) - JOYDEV_MINOR_BASE;   //iminor  取次设备号  减  基值   i
	int error;

	if (i >= JOYDEV_MINORS)  //次设备号不在处理范围内
		return -ENODEV;

	error = mutex_lock_interruptible(&joydev_table_mutex);  //加上锁
	if (error)
		return error;
	joydev = joydev_table[i];//取址
	if (joydev)
		get_device(&joydev->dev);   //增加DEV引用次数
	mutex_unlock(&joydev_table_mutex);   //解锁

	if (!joydev)
		return -ENODEV;

	client = kzalloc(sizeof(struct joydev_client), GFP_KERNEL);   //申请一个内存  存client
	if (!client) {       //不成功    要减掉引用次数
		error = -ENOMEM;
		goto err_put_joydev;
	}

	spin_lock_init(&client->buffer_lock);  //初始化  缓冲区的那个锁
	client->joydev = joydev;     //申请成功
	joydev_attach_client(joydev, client);  //加client到链表

	error = joydev_open_device(joydev);//
	if (error)
		goto err_free_client;

	file->private_data = client; //成功，则将client地址保存到file 指向的结构体
	return 0;

 err_free_client:
	joydev_detach_client(joydev, client);   //删除刚加的client
	kfree(client);         //减少引用次数
 err_put_joydev:
	put_device(&joydev->dev);   //减少引用次数
	return error;
}

static int joydev_fetch_next_event(struct joydev_client *client,
				   struct joy_event *event)   //取事件函数   与read共同实现  被read调用
{
	int have_event;

	spin_lock_irq(&client->buffer_lock);  //加锁   读缓冲区数

	have_event = client->head != client->tail;
	
	if (have_event) {
		client->tail = (client->tail+1) % JOYDEV_BUFFER_SIZE;
		*event = client->buffer[client->tail];
		
	}else
		client->ready = 0; //无事件

	spin_unlock_irq(&client->buffer_lock);    //开锁

	return have_event;
}


static ssize_t joydev_read(struct file *file, char __user *buf,
			   size_t count, loff_t *ppos)
{
	struct joydev_client *client = file->private_data;
	struct joydev *joydev = client->joydev;
	struct joy_event event;
	int retval;

	if (!joydev->exist)    //joydev不存在 返回
		return -ENODEV;

	if (count < sizeof(struct joy_event))   //不能完整读一个事件    count是拷贝数据总量
		return -EINVAL;

	//wait_event_interruptible参数成立   进程睡觉
	retval = wait_event_interruptible(joydev->wait,!joydev->exist || client->ready);   //没数据 就睡          retval为1  睡觉  否则则不睡
	if (retval)
		return retval;
	

	while (retval + sizeof(struct joy_event) <= count &&joydev_fetch_next_event(client, &event))  //判断有无事件    0 + 能读一个完整的事件  <=count（能读事件）并且有事件
	{    
		
		if (copy_to_user(buf + retval, &event, sizeof(struct joy_event)))  //把数据从内核空间拷贝到用户空间
			return -EFAULT;

		retval += sizeof(struct joy_event);      //retval表示已经拷贝的数据量
	}

	return retval;  //返回拷贝总量
}
/*
static void joydev_generate_response(struct joydev_client *client,
					int command)
{
	switch(command){
		default:;
	}
}

static ssize_t joydev_write(struct file *file, const char __user *buffer,
				size_t count, loff_t *ppos)//往设备写数据
{
	struct joydev_client *client = file->private_data;
	unsigned char c;
	unsigned int i;

	for (i = 0; i < count; i++) {
		spin_lock_irq(&client->buffer_lock);

		if (get_user(c, buffer + i))
			return -EFAULT;

		joydev_generate_response(client, c);

		spin_unlock_irq(&client->buffer_lock);
	}

	kill_fasync(&client->fasync, SIGIO, POLL_IN);
	wake_up_interruptible(&client->joydev->wait);

	return count;
}
*/
static const struct file_operations joydev_fops = {      //动作操作  结构体    内=外
	.owner		= THIS_MODULE,
	//.write		= joydev_write,
	.read		= joydev_read,
	.open		= joydev_open,
	.release	= joydev_release,
	.fasync		= joydev_fasync,
};

static int joydev_install_chrdev(struct joydev *joydev)   //将joydev结构体存到  table数组
{
	joydev_table[joydev->minor] = joydev;
	return 0;
}

static void joydev_remove_chrdev(struct joydev *joydev)   //删除设备节点
{
	mutex_lock(&joydev_table_mutex);
	joydev_table[joydev->minor] = NULL;  //数组地址清空
	mutex_unlock(&joydev_table_mutex);
}



static void joydev_mark_dead(struct joydev *joydev)//exesit置0
{
	mutex_lock(&joydev->mutex);
	joydev->exist = false;
	mutex_unlock(&joydev->mutex);
}



static void joydev_cleanup(struct joydev *joydev)  //清除之前所有注册信息
{
	struct input_handle *handle = &joydev->handle;

	joydev_mark_dead(joydev);//exesit置0

	joydev_remove_chrdev(joydev);//删除设备节点

	if (joydev->open)  //打开过
		input_close_device(handle);//关闭
}




static int joydev_connect(struct input_handler *handler, struct input_dev *dev,
			  const struct input_device_id *id)          //链接hanlder（joydev）   与  divice（js）  
{
	struct joydev *joydev;
	int minor;
	int error;
//将joydev地址放到为空的table数组，再申请一个空内存   放joydev结构体
	
	for (minor = 0; minor < JOYDEV_MINORS; minor++)   //遍历table数组              在table里找到一个空位 存放joydev
		if (!joydev_table[minor])  //不为空  跳出
			break;
	if (minor == JOYDEV_MINORS) {   //为空
		printk(KERN_ERR "joydev: no more free joydev devices\n");
		return -ENFILE;
	}
	joydev = kzalloc(sizeof(struct joydev), GFP_KERNEL);    //申请新的内存  
	if (!joydev)
		return -ENOMEM;

	INIT_LIST_HEAD(&joydev->client_list);    //初始化新内存
	spin_lock_init(&joydev->client_lock);    //初始化锁
	mutex_init(&joydev->mutex);             //初始化锁
	init_waitqueue_head(&joydev->wait);    //初始化存放睡眠进程的队列

	dev_set_name(&joydev->dev, "js%d", minor);
	joydev->exist = 1;
	joydev->minor = minor;
	//保存inputdev信息
	joydev->handle.dev = input_get_device(dev);
	joydev->handle.name = dev_name(&joydev->dev);
	//保存handler信息
	joydev->handle.handler = handler;
	joydev->handle.private = joydev;

	joydev->dev.devt = MKDEV(INPUT_MAJOR, JOYDEV_MINOR_BASE + minor);
	joydev->dev.class = &input_class;
	joydev->dev.parent = &dev->dev;
	joydev->dev.release = joydev_free;
	device_initialize(&joydev->dev);

	error = input_register_handle(&joydev->handle);   //注册handle
	if (error)   //失败
		goto err_free_joydev;

	error = joydev_install_chrdev(joydev);//将joydev结构体存到  table数组
	if (error)  //没存进去
		goto err_unregister_handle;  

	error = device_add(&joydev->dev);   //?

	if (error)
		goto err_cleanup_joydev;  
	return 0;

 err_cleanup_joydev:
	joydev_cleanup(joydev);  //清除
 err_unregister_handle:
	input_unregister_handle(&joydev->handle);   //删去注册的handle
 err_free_joydev:
	put_device(&joydev->dev);   //减少引用次数
	return error;
}

static void joydev_disconnect(struct input_handle *handle)
{
	struct joydev *joydev = handle->private;

	device_del(&joydev->dev);
	joydev_cleanup(joydev);  //清除
	input_unregister_handle(handle);//删除设备节点
	put_device(&joydev->dev);//减少引用次数
}


//intput_device（js.c）   input_handler(joydev.c)    两个结构体对应     按键匹配
static const struct input_device_id joydev_ids[] = {    //
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |      
					INPUT_DEVICE_ID_MATCH_KEYBIT |
					INPUT_DEVICE_ID_MATCH_RELBIT,
		.evbit = { BIT_MASK(EV_KEY) | BIT_MASK(EV_REL) },
		.keybit = { [BIT_WORD(BTN_A)] = BIT_MASK(BTN_A) },
		.relbit = { BIT_MASK(REL_X) | BIT_MASK(REL_Y) },
	},
	{ }	
};

MODULE_DEVICE_TABLE(input, joydev_ids);

static struct input_handler joydev_handler = {
	.event		= joydev_event,
	.connect	= joydev_connect,
	.disconnect	= joydev_disconnect,
	.fops		= &joydev_fops,
	.minor		= JOYDEV_MINOR_BASE,
	.name		= "jsdev",
	.id_table	= joydev_ids,
};

static int __init joydev_init(void)   //初始化函数    注册joydev信息
{
	printk("init for handler\n");
	return input_register_handler(&joydev_handler);
}

static void __exit joydev_exit(void)   //退出时   删除信息
{
	input_unregister_handler(&joydev_handler);
}

module_init(joydev_init);//指定初始化函数
module_exit(joydev_exit);//
