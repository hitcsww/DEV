#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/stat.h>
#include <linux/module.h>
#include <linux/usb/input.h>
#include <linux/uaccess.h>

#define XPAD_PKT_LEN 32

#define MAP_DPAD_TO_BUTTONS    0
#define MAP_DPAD_TO_AXES       1
#define MAP_DPAD_UNKNOWN       2

#define XTYPE_XBOX        0
#define XTYPE_XBOX360     1
#define XTYPE_XBOX360W    2
#define XTYPE_UNKNOWN     3


static const signed short xpad_common_btn[] = {
	BTN_A, BTN_B, BTN_X, BTN_Y,			
	BTN_START, BTN_BACK, BTN_THUMBL, BTN_THUMBR,	
	-1						
};

#define XPAD_XBOX360_VENDOR_PROTOCOL(vend,pr) \
	.match_flags = USB_DEVICE_ID_MATCH_VENDOR | USB_DEVICE_ID_MATCH_INT_INFO, \
	.idVendor = (vend), \
	.bInterfaceClass = USB_CLASS_VENDOR_SPEC, \
	.bInterfaceSubClass = 93, \
	.bInterfaceProtocol = (pr)
#define XPAD_XBOX360_VENDOR(vend) \
	{ XPAD_XBOX360_VENDOR_PROTOCOL(vend,1) }, \
	{ XPAD_XBOX360_VENDOR_PROTOCOL(vend,129) }

static struct usb_device_id xpad_table [] = {
	{ USB_INTERFACE_INFO('X', 'B', 0) },		
	XPAD_XBOX360_VENDOR(0x1bad),	
	XPAD_XBOX360_VENDOR(0x045e),
	{ }
};

MODULE_DEVICE_TABLE (usb, xpad_table);

struct usb_xpad {
	struct input_dev *dev;		
	struct usb_device *udev;	

	struct urb *irq_in;		
	unsigned char *idata;		
	dma_addr_t idata_dma;
	
 	char phys[64];
};

static void xpad_irq_in(struct urb *urb)
{
	struct usb_xpad *xpad = urb->context;
	struct input_dev *dev = xpad->dev;
	int retval, status;

	status = urb->status;
	switch (status) {
	case 0:		
		break;
	case -ECONNRESET:
	case -ENOENT:
	case -ESHUTDOWN:		
		dbg("%s - urb shutting down with status: %d",
			__func__, status);
		return;
	default:
		dbg("%s - nonzero urb status received: %d",
			__func__, status);
		goto exit;
	}

	unsigned char *data;
	data = xpad->idata;

	/*printk("data:%02X %02X %02X %02X %02X %02X %d %d\n",
		(char)data[0]&0xFF,(char)data[1]&0xFF,(char)data[2]&0xFF,(char)data[3]&0xFF,(char)data[4]&0xFF,(char)data[5]&0xFF,
		(__s16) le16_to_cpup((__le16 *)(data + 6)), ~(__s16) le16_to_cpup((__le16 *)(data + 8)));*/
	input_report_key(dev, BTN_A,	data[3] & 0x10);
	input_report_key(dev, BTN_B,	data[3] & 0x20);
	input_report_key(dev, BTN_X,	data[3] & 0x40);
	input_report_key(dev, BTN_Y,	data[3] & 0x80);
	
	input_report_key(dev, BTN_START,  data[2] & 0x10);
	input_report_key(dev, BTN_BACK,   data[2] & 0x20);


	input_report_key(dev, BTN_THUMBL, data[2] & 0x40);
	input_report_key(dev, BTN_THUMBR, data[2] & 0x80);

	input_report_rel(dev, REL_X,
			 (__s16) le16_to_cpup((__le16 *)(data + 6)));
	input_report_rel(dev, REL_Y,
			 ~(__s16) le16_to_cpup((__le16 *)(data + 8)));
	input_sync(dev);
	
exit:
	retval = usb_submit_urb (urb, GFP_ATOMIC);
	if (retval)
		err ("%s - usb_submit_urb failed with result %d",
		     __func__, retval);
}


static int xpad_open(struct input_dev *dev)
{
	struct usb_xpad *xpad = input_get_drvdata(dev);

	xpad->irq_in->dev = xpad->udev;
	if (usb_submit_urb(xpad->irq_in, GFP_KERNEL))
		return -EIO;

	return 0;
}

static void xpad_close(struct input_dev *dev)
{
	struct usb_xpad *xpad = input_get_drvdata(dev);

	usb_kill_urb(xpad->irq_in);

}

static int xpad_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	struct usb_xpad *xpad;
	struct input_dev *input_dev;
	struct usb_endpoint_descriptor *ep_irq_in;
	int i;
	int error = -ENOMEM;

	xpad = kzalloc(sizeof(struct usb_xpad), GFP_KERNEL);
	input_dev = input_allocate_device();
	if (!xpad || !input_dev)
		goto fail1;

	xpad->idata = usb_alloc_coherent(udev, XPAD_PKT_LEN,
				       GFP_ATOMIC, &xpad->idata_dma);
	if (!xpad->idata)
		goto fail1;

	xpad->irq_in = usb_alloc_urb(0, GFP_KERNEL);
	if (!xpad->irq_in)
		goto fail2;

	xpad->udev = udev;

	xpad->dev = input_dev;
	usb_make_path(udev, xpad->phys, sizeof(xpad->phys));
	strlcat(xpad->phys, "/input0", sizeof(xpad->phys));

	input_dev->name = "Microsoft Xbox 360 Controller";
	input_dev->phys = xpad->phys;
	usb_to_input_id(udev, &input_dev->id);
	input_dev->dev.parent = &intf->dev;

	input_set_drvdata(input_dev, xpad);

	input_dev->open = xpad_open;
	input_dev->close = xpad_close;


	input_dev->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_REL);


	for (i = 0; xpad_common_btn[i] >= 0; i++)
		set_bit(xpad_common_btn[i], input_dev->keybit);

	input_dev->relbit[0] = BIT_MASK(REL_X) | BIT_MASK(REL_Y);

	ep_irq_in = &intf->cur_altsetting->endpoint[0].desc;
	usb_fill_int_urb(xpad->irq_in, udev,
			 usb_rcvintpipe(udev, ep_irq_in->bEndpointAddress),
			 xpad->idata, XPAD_PKT_LEN, xpad_irq_in,
			 xpad, ep_irq_in->bInterval);
	xpad->irq_in->transfer_dma = xpad->idata_dma;
	xpad->irq_in->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

	error = input_register_device(xpad->dev);
 
	usb_set_intfdata(intf, xpad);

	return 0;

 fail2:	usb_free_coherent(udev, XPAD_PKT_LEN, xpad->idata, xpad->idata_dma);
 fail1:	input_free_device(input_dev);
	kfree(xpad);
	return error;

}

static void xpad_disconnect(struct usb_interface *intf)
{
	struct usb_xpad *xpad = usb_get_intfdata (intf);

	usb_set_intfdata(intf, NULL);
	if (xpad) {
		input_unregister_device(xpad->dev);
	
		usb_free_urb(xpad->irq_in);
		usb_free_coherent(xpad->udev, XPAD_PKT_LEN,
				xpad->idata, xpad->idata_dma);
		kfree(xpad);
	}
}

static struct usb_driver xpad_driver = {
	.name		= "js",
	.probe		= xpad_probe,
	.disconnect	= xpad_disconnect,
	.id_table	= xpad_table,
};

static int __init usb_xpad_init(void)
{
	int result = usb_register(&xpad_driver);
	if (result == 0){
		printk("register error");
	}
	return result;
}

static void __exit usb_xpad_exit(void)
{
	usb_deregister(&xpad_driver);
}

module_init(usb_xpad_init);
module_exit(usb_xpad_exit);
MODULE_LICENSE("GPL");
