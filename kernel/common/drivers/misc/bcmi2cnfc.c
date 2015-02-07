/*
 * Copyright (C) 2011 Broadcom Corporation.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/i2c.h>
#include <linux/irq.h>
#include <linux/jiffies.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/gpio.h>
#include <linux/miscdevice.h>
#include <linux/spinlock.h>
#include <linux/bcmi2cnfc.h>
#include <linux/poll.h>
#include <linux/version.h>

#define     TRUE                1
#define     FALSE               0
#define     STATE_HIGH          1
#define     STATE_LOW           0

#define     NFC_REQ_ACTIVE_STATE    STATE_HIGH
#define     NFC_WAKE_ACTIVE_STATE   STATE_HIGH
#define     NFC_REQ_INACTIVE_STATE  STATE_LOW
// compile options that can be changed/customized
// change belkow variable to TRUE to enable certain feature or workaround.
#define     BCM20791A0          0
#define     ACAR_PLATFORM       1
#define     MSM_PLATFORM        0
#define     CHANGE_CLIENT_ADDR  TRUE

#define     READ_MULTIPLE_PACKETS   0
// end of compile options


// do not change below
#define     MAX_BUFFER_SIZE     780

struct bcmi2cnfc_dev    {
    wait_queue_head_t   read_wq;
    struct mutex        read_mutex;
    struct i2c_client   *client;
    struct miscdevice   bcmi2cnfc_device;
    unsigned int        wake_gpio;
    unsigned int        en_gpio;
    unsigned int        irq_gpio;
    bool                irq_enabled;
    spinlock_t          irq_enabled_lock;

    unsigned int        packet_size;
    unsigned int        wake_active_state;
    unsigned int        read_multiple_packets;
};
#if ACAR_PLATFORM || MSM_PLATFORM
static struct i2c_client *nfc_client;
#endif

#if ACAR_PLATFORM
#define INTERRUPT_TRIGGER_TYPE  IRQF_TRIGGER_RISING
#else
#if !defined (INTERRUPT_TRIGGER_TYPE)
#define INTERRUPT_TRIGGER_TYPE  IRQF_TRIGGER_HIGH
#endif
#endif

static void bcmi2cnfc_disable_irq(struct bcmi2cnfc_dev *bcmi2cnfc_dev)
{
    unsigned long flags;
    dev_dbg(&bcmi2cnfc_dev->client->dev,
            "disable irq\n");

    spin_lock_irqsave(&bcmi2cnfc_dev->irq_enabled_lock, flags);
    if (bcmi2cnfc_dev->irq_enabled) {
        disable_irq_nosync(bcmi2cnfc_dev->client->irq);
        bcmi2cnfc_dev->irq_enabled = false;
    }
    spin_unlock_irqrestore(&bcmi2cnfc_dev->irq_enabled_lock, flags);
}

static void bcmi2cnfc_enable_irq(struct bcmi2cnfc_dev *bcmi2cnfc_dev)
{
    unsigned long flags;
    spin_lock_irqsave(&bcmi2cnfc_dev->irq_enabled_lock, flags);
    if (!bcmi2cnfc_dev->irq_enabled) {
        bcmi2cnfc_dev->irq_enabled = true;
        enable_irq(bcmi2cnfc_dev->client->irq);
	dev_info(&bcmi2cnfc_dev->client->dev,
                "irq enabled \n");
    }
    spin_unlock_irqrestore(&bcmi2cnfc_dev->irq_enabled_lock, flags);
}

#if CHANGE_CLIENT_ADDR
//
// The alias address 0x79, when sent as a 7-bit address from the host processor will match the first byte
// (highest 2 bits) of the default client address (0x1FA) that is programmed in bcm20791.
// When used together with the first byte (0xFA) of the byte sequence below, it can be used to address the bcm20791
// in a system that does not support 10-bit address and change the default address to 0x38.
//
// the new address can be changed by changing the CLIENT_ADDRESS below if 0x38 conflicts with other device
// on the same i2c bus.
//
#define ALIAS_ADDRESS       0x79
static char addr_data[] = {
    0xFA, 0xF2, 0x00, 0x00, 0x00, 0x38, 0x00, 0x00, 0x00, 0x2A
};

static void change_client_addr(struct bcmi2cnfc_dev *bcmi2cnfc_dev, int addr)
{
    struct i2c_client* client;
    int ret;
    int i;

    client = bcmi2cnfc_dev->client;

    client->addr = ALIAS_ADDRESS;
    client->flags &= ~I2C_CLIENT_TEN;
    
    addr_data[5] = addr & 0xFF;
    ret = 0;
    for (i = 1; i < sizeof(addr_data) - 1; ++i)
        ret += addr_data[i];
    addr_data[sizeof(addr_data)-1] = (ret & 0xFF);
    dev_info(&client->dev,
            "Change client device at (0x%04X) flag = %04x, addr_data[%d] = %02x\n",
            client->addr, client->flags, sizeof(addr_data)-1, addr_data[sizeof(addr_data)-1]);
    ret = i2c_master_send(client, addr_data, sizeof(addr_data));
    client->addr = addr_data[5];

    dev_info(&client->dev,
            "Change client device changed to (0x%04X) flag = %04x, ret = %d\n",
            client->addr, client->flags, ret);
}
#endif

#if BCM20791A0
static ssize_t bcmi2cnfc_dev_read_internal(struct bcmi2cnfc_dev *bcmi2cnfc_dev, int count)
{
    unsigned char tmp[12];
    int ret = 0;

    mutex_lock(&bcmi2cnfc_dev->read_mutex);

    ret = wait_event_interruptible_timeout(bcmi2cnfc_dev->read_wq,
                                           gpio_get_value(bcmi2cnfc_dev->irq_gpio),
                                           1*HZ);
    if (ret<1)
        goto fail;
    /* Read data */
    ret = i2c_master_recv(bcmi2cnfc_dev->client, tmp, count);
fail:
    mutex_unlock(&bcmi2cnfc_dev->read_mutex);

    dev_info(&bcmi2cnfc_dev->client->dev,
            "returned internal reading %d bytes\n", ret);

    bcmi2cnfc_enable_irq(bcmi2cnfc_dev);
    if (ret != count) {
        return -EIO;
    }

    return ret;
}

static char init_data[] = {
    0x10, 0x20, 0x00, 0x00
};

#define RESPONSE_COUNT  (6)

static int scan_client_addr(struct bcmi2cnfc_dev *bcmi2cnfc_dev)
{
    struct i2c_client* client;
    int found;
    int ret;
    int i;
    unsigned short range[6];

    client = bcmi2cnfc_dev->client;

    dev_err(&client->dev,
            "scanning brcm client device address\n");

    found = 0;

    range[0] = client->addr;    // default address, 0x1FA as configured
    range[1] = client->addr-1;
    range[2] = 0x007F;          // highest 7-bit address
    range[3] = 0x0000;          // lowest  7-bit address
    range[4] = 0x03FF;          // highest 10-bit address
    range[5] = 0x007F;          // lowest  10-bit address

    bcmi2cnfc_enable_irq(bcmi2cnfc_dev);
    for (i = 0; i < 6; i += 2) {

        if (range[i] > 0x7F)
            client->flags |= I2C_CLIENT_TEN;
        else
            client->flags &= ~I2C_CLIENT_TEN;

        dev_err(&client->dev,
                "testing client device at range (0x%04X-0x%04X)\n", range[i] , range[i+1]);

        for (client->addr = range[i]; client->addr > range[i+1]; --client->addr) {
            dev_err(&client->dev,
                    "testing client device at (0x%04X)\n",
                    client->addr);
            ret = i2c_master_send(client, init_data, 4);
            if (ret == sizeof(init_data)) {
                ret = bcmi2cnfc_dev_read_internal(bcmi2cnfc_dev, RESPONSE_COUNT);
                if (ret == RESPONSE_COUNT) {
                    found = 1;
                    break;
                }
            }
        }

        if (found == 1)
            break;
    }

    if (found)
        dev_err(&client->dev,
                "Successfully found brcm client device address at (0x%04X)\n",
                client->addr);
    else
        dev_err(&client->dev,
                "Cannot find brcm client device address, last tested at (0x%04X)\n",
                client->addr+1);

    return found ? client->addr : -1;
}
#endif


static irqreturn_t bcmi2cnfc_dev_irq_handler(int irq, void *dev_id)
{
    struct bcmi2cnfc_dev *bcmi2cnfc_dev = dev_id;
    dev_info(&bcmi2cnfc_dev->client->dev,
            "irq handler\n");

    if (!gpio_get_value(bcmi2cnfc_dev->irq_gpio))
        return IRQ_HANDLED;

    bcmi2cnfc_disable_irq(bcmi2cnfc_dev);

    /* Wake up waiting readers */
    wake_up(&bcmi2cnfc_dev->read_wq);

    return IRQ_HANDLED;
}

static unsigned int bcmi2cnfc_dev_poll(struct file *filp, poll_table *wait)
{
    struct bcmi2cnfc_dev *bcmi2cnfc_dev = filp->private_data;
    unsigned int mask = 0;

    poll_wait (filp, &bcmi2cnfc_dev->read_wq, wait);

    if (gpio_get_value(bcmi2cnfc_dev->irq_gpio) == NFC_REQ_ACTIVE_STATE)
        mask |= POLLIN | POLLRDNORM;
    else
        bcmi2cnfc_enable_irq(bcmi2cnfc_dev);

    return mask;
}

static ssize_t bcmi2cnfc_dev_read(struct file *filp, char __user *buf,
                                  size_t count, loff_t *offset)
{
    struct bcmi2cnfc_dev *bcmi2cnfc_dev = filp->private_data;
    unsigned char tmp[MAX_BUFFER_SIZE];
    int ret, len, total, packets, prev_total;

    ret = 0;
    len = 1;
    total = 0;
    packets = 0;
    prev_total = 0;
    if (count > MAX_BUFFER_SIZE)
        count = MAX_BUFFER_SIZE;

    if (bcmi2cnfc_dev->packet_size > 0) {
        count = bcmi2cnfc_dev->packet_size;
        len = count;
    }

    mutex_lock(&bcmi2cnfc_dev->read_mutex);
    /* Read data */
#define PACKET_HEADER_SIZE_NCI      (4)
#define PACKET_HEADER_SIZE_HCI      (3)
#define PACKET_TYPE_NCI             (16)
#define PACKET_TYPE_HCIEV           (4)
#define MAX_PACKET_SIZE             (PACKET_HEADER_SIZE_NCI + 255)

    do {
        ret = i2c_master_recv(bcmi2cnfc_dev->client, tmp+total, len);

        if (ret != len) {
            if (total == 0)
            dev_err(&bcmi2cnfc_dev->client->dev,
                    "(type byte)IO Error ret=%d !!!!\n", ret);
            break;
        }

        if (len == count) {
            total = ret;
            ++packets;
            break;
        }
 
        if (tmp[total] == PACKET_TYPE_NCI)
            len = PACKET_HEADER_SIZE_NCI-1;
        else if (tmp[total] == PACKET_TYPE_HCIEV)
            len = PACKET_HEADER_SIZE_HCI-1;
        else {
            dev_err(&bcmi2cnfc_dev->client->dev,
                    "unknown type header tmp[%d]=%d !!!!\n", total, tmp[total]);
            break;
        }
        if (len > 0) {
             // read the remainder of the packet header
            ret = i2c_master_recv(bcmi2cnfc_dev->client, tmp+total+1, len);
            if (ret != len) {
                dev_err(&bcmi2cnfc_dev->client->dev,
                        "(type header) IO Error ret = %d!!!!\n", ret);
                total = prev_total;
                break;
            }
            // get the packet payload length
            len = tmp[total+ret];
            total += ret+1;
        }
        if (len > 0) {
            // read the packet payload
            ret = i2c_master_recv(bcmi2cnfc_dev->client, tmp+total, len );
            if (ret > 0) {
                total += len;
            } else {
                dev_err(&bcmi2cnfc_dev->client->dev,
                        "packet IO Error ret = %d!!!!\n", ret);
                total = prev_total;
                break;
            }
        }
        ++packets;
        len = 1;
        prev_total = total;
    } while (bcmi2cnfc_dev->read_multiple_packets > 0 && total + MAX_PACKET_SIZE <= count);

    bcmi2cnfc_enable_irq(bcmi2cnfc_dev);
if (total > 0)
    if (total > count || copy_to_user(buf, tmp, total)) {
        dev_err(&bcmi2cnfc_dev->client->dev,
                "failed to copy to user space, total = %d\n", total);
            total = -EFAULT;
    }

    mutex_unlock(&bcmi2cnfc_dev->read_mutex);

    if (packets > 1)
    dev_info(&bcmi2cnfc_dev->client->dev,
                "read %d packets total = %d bytes\n", packets, total);
    return total > 0 ? total : len;
}

static ssize_t bcmi2cnfc_dev_write(struct file *filp, const char __user *buf,
                                   size_t count, loff_t *offset)
{
    struct bcmi2cnfc_dev  *bcmi2cnfc_dev = filp->private_data;
    char tmp[MAX_BUFFER_SIZE];
    int ret;

    dev_info(&bcmi2cnfc_dev->client->dev,
            "%s, %d\n", __func__, count);

    if (count > MAX_BUFFER_SIZE) {
        dev_err(&bcmi2cnfc_dev->client->dev,
                "out of memory\n");
        return -ENOMEM;
    }

    if (copy_from_user(tmp, buf, count)) {
        dev_err(&bcmi2cnfc_dev->client->dev,
                "failed to copy from user space\n");
        return -EFAULT;
    }

    mutex_lock(&bcmi2cnfc_dev->read_mutex);
    /* Write data */
    ret = i2c_master_send(bcmi2cnfc_dev->client, tmp, count);
    if (ret != count) {
        dev_err(&bcmi2cnfc_dev->client->dev,
                "failed to write %d\n", ret);
        ret = -EIO;
    }
    mutex_unlock(&bcmi2cnfc_dev->read_mutex);

    return ret;
}


static int bcmi2cnfc_dev_open(struct inode *inode, struct file *filp)
{
    int         ret = 0;
#if ACAR_PLATFORM  || MSM_PLATFORM
	struct bcmi2cnfc_dev *bcmi2cnfc_dev  = i2c_get_clientdata(nfc_client);
#else
    struct bcmi2cnfc_dev *bcmi2cnfc_dev = container_of(filp->private_data,
                                                      struct bcmi2cnfc_dev,
                                                      bcmi2cnfc_device);
#endif
    filp->private_data = bcmi2cnfc_dev;
    bcmi2cnfc_dev->wake_active_state = NFC_WAKE_ACTIVE_STATE;
    bcmi2cnfc_dev->read_multiple_packets = READ_MULTIPLE_PACKETS;

    dev_info(&bcmi2cnfc_dev->client->dev,
            "%d,%d\n", imajor(inode), iminor(inode));
#if BCM20791A0
    ret = scan_client_addr(bcmi2cnfc_dev);
    if (ret >= 0)
        return 0;

    ret = -ENODEV;
#endif
    return ret;
}
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,36))
static long bcmi2cnfc_dev_unlocked_ioctl(struct file *filp,
                               unsigned int cmd, unsigned long arg)
#else
static int bcmi2cnfc_dev_ioctl(struct inode *inode, struct file *filp,
                               unsigned int cmd, unsigned long arg)
#endif
{
    struct bcmi2cnfc_dev *bcmi2cnfc_dev = filp->private_data;

    switch (cmd) {
    case BCMNFC_SET_WAKE_ACTIVE_STATE:
        dev_info(&bcmi2cnfc_dev->client->dev,
                "%s, BCMNFC_SET_WAKE_ACTIVE_STATE (%x, %lx):\n", __func__, cmd, arg);
        bcmi2cnfc_dev->wake_active_state = arg;
        break;
    case BCMNFC_READ_FULL_PACKET:
        dev_info(&bcmi2cnfc_dev->client->dev,
                "%s, BCMNFC_READ_FULL_PACKET (%x, %lx):\n", __func__, cmd, arg);
        bcmi2cnfc_dev->packet_size = arg;
        break;
    case BCMNFC_READ_MULTI_PACKETS:
        dev_info(&bcmi2cnfc_dev->client->dev,
                "%s, BCMNFC_READ_MULTI_PACKETS (%x, %lx):\n", __func__, cmd, arg);
        bcmi2cnfc_dev->read_multiple_packets = arg;
        break;
#if CHANGE_CLIENT_ADDR
    case BCMNFC_CHANGE_ADDR:
        dev_info(&bcmi2cnfc_dev->client->dev,
                "%s, BCMNFC_CHANGE_ADDR (%x, %lx):\n", __func__, cmd, arg);
        change_client_addr(bcmi2cnfc_dev, arg);
        break;
#endif
    case BCMNFC_POWER_CTL:
        dev_info(&bcmi2cnfc_dev->client->dev,
                "%s, BCMNFC_POWER_CTL (%x, %lx):\n", __func__, cmd, arg);
        if (arg == BCMNFC_POWER_OFF) {
			 bcmi2cnfc_disable_irq(bcmi2cnfc_dev);
			  dev_info(&bcmi2cnfc_dev->client->dev,"irq disabled \n");
        } else if (arg == BCMNFC_POWER_ON) {
			bcmi2cnfc_enable_irq(bcmi2cnfc_dev);
        }
		gpio_set_value(bcmi2cnfc_dev->en_gpio, arg);
        break;
    case BCMNFC_WAKE_CTL:
        dev_info(&bcmi2cnfc_dev->client->dev,
                "%s, BCMNFC_WAKE_CTL (%x, %lx):\n", __func__, cmd, arg);
        gpio_set_value(bcmi2cnfc_dev->wake_gpio, arg);
        break;
    default:
        dev_err(&bcmi2cnfc_dev->client->dev,
                "%s, unknown cmd (%x, %lx)\n", __func__, cmd, arg);
        return 0;
    }

    return 0;
}

static const struct file_operations bcmi2cnfc_dev_fops = {
    .owner  = THIS_MODULE,
    .llseek = no_llseek,
    .poll   = bcmi2cnfc_dev_poll,
    .read   = bcmi2cnfc_dev_read,
    .write  = bcmi2cnfc_dev_write,
    .open   = bcmi2cnfc_dev_open,
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,36))
    .unlocked_ioctl  = bcmi2cnfc_dev_unlocked_ioctl
#else
    .ioctl  = bcmi2cnfc_dev_ioctl
#endif
};


static int bcmi2cnfc_probe(struct i2c_client *client,
                           const struct i2c_device_id *id)
{
    int ret;
    struct bcmi2cnfc_i2c_platform_data *platform_data;
    struct bcmi2cnfc_dev *bcmi2cnfc_dev;

    platform_data = client->dev.platform_data;

    dev_info(&client->dev,
            "%s, probing bcmi2cnfc driver\n", __func__);
    if (platform_data == NULL) {
        dev_err(&client->dev,
                "nfc probe fail\n");
        return  -ENODEV;
    }

    if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
        dev_err(&client->dev,
                "need I2C_FUNC_I2C\n");
        return  -ENODEV;
    }

    ret = gpio_request(platform_data->irq_gpio, "nfc_int");
    if (ret)
        return  -ENODEV;
    ret = gpio_request(platform_data->en_gpio, "nfc_ven");
    if (ret)
        goto err_en;
    ret = gpio_request(platform_data->wake_gpio, "nfc_firm");
    if (ret)
        goto err_firm;

    gpio_set_value(platform_data->en_gpio, 0);
    gpio_set_value(platform_data->wake_gpio, 0);

    bcmi2cnfc_dev = kzalloc(sizeof(*bcmi2cnfc_dev), GFP_KERNEL);
    if (bcmi2cnfc_dev == NULL) {
        dev_err(&client->dev,
                "failed to allocate memory for module data\n");
        ret = -ENOMEM;
        goto err_exit;
    }

    bcmi2cnfc_dev->wake_gpio = platform_data->wake_gpio;
    bcmi2cnfc_dev->irq_gpio = platform_data->irq_gpio;
    bcmi2cnfc_dev->en_gpio = platform_data->en_gpio;
    bcmi2cnfc_dev->client   = client;
#if ACAR_PLATFORM
    platform_data->init();
#endif
    /* init mutex and queues */
    init_waitqueue_head(&bcmi2cnfc_dev->read_wq);
    mutex_init(&bcmi2cnfc_dev->read_mutex);
    spin_lock_init(&bcmi2cnfc_dev->irq_enabled_lock);

    bcmi2cnfc_dev->bcmi2cnfc_device.minor = MISC_DYNAMIC_MINOR;
    bcmi2cnfc_dev->bcmi2cnfc_device.name = "bcmi2cnfc";
    bcmi2cnfc_dev->bcmi2cnfc_device.fops = &bcmi2cnfc_dev_fops;

    ret = misc_register(&bcmi2cnfc_dev->bcmi2cnfc_device);
    if (ret) {
        dev_err(&client->dev,
                "misc_register failed\n");
        goto err_misc_register;
    }

    /* request irq.  the irq is set whenever the chip has data available
     * for reading.  it is cleared when all data has been read.
     */
    dev_info(&client->dev,
            "requesting IRQ %d\n", client->irq);
    bcmi2cnfc_dev->irq_enabled = true;
    ret = request_irq(client->irq, bcmi2cnfc_dev_irq_handler,
                      INTERRUPT_TRIGGER_TYPE, client->name, bcmi2cnfc_dev);
    if (ret) {
        dev_err(&client->dev, "request_irq failed\n");
        goto err_request_irq_failed;
    }
    i2c_set_clientdata(client, bcmi2cnfc_dev);
#if ACAR_PLATFORM || MSM_PLATFORM
	nfc_client = client;
#endif
bcmi2cnfc_dev->packet_size = 0;
    dev_info(&client->dev,
            "%s, probing bcmi2cnfc driver exited successfully\n", __func__);
    return 0;

err_request_irq_failed:
    misc_deregister(&bcmi2cnfc_dev->bcmi2cnfc_device);
err_misc_register:
    mutex_destroy(&bcmi2cnfc_dev->read_mutex);
    kfree(bcmi2cnfc_dev);
err_exit:
    gpio_free(platform_data->wake_gpio);
err_firm:
    gpio_free(platform_data->en_gpio);
err_en:
    gpio_free(platform_data->irq_gpio);
    return ret;
}

static int bcmi2cnfc_remove(struct i2c_client *client)
{
	struct bcmi2cnfc_i2c_platform_data *platform_data;
    struct bcmi2cnfc_dev *bcmi2cnfc_dev;
	pr_info(" bcmi2cnfc_remove  Enter \n");

	platform_data = client->dev.platform_data;

    bcmi2cnfc_dev = i2c_get_clientdata(client);
    free_irq(client->irq, bcmi2cnfc_dev);
    misc_deregister(&bcmi2cnfc_dev->bcmi2cnfc_device);
    mutex_destroy(&bcmi2cnfc_dev->read_mutex);
    gpio_free(bcmi2cnfc_dev->irq_gpio);
    gpio_free(bcmi2cnfc_dev->en_gpio);
    kfree(bcmi2cnfc_dev);
#if ACAR_PLATFORM
		platform_data->reset();
#endif

    return 0;
}

static const struct i2c_device_id bcmi2cnfc_id[] = {
    { "bcmi2cnfc", 0 },
    { }
};

static struct i2c_driver bcmi2cnfc_driver = {
    .id_table   = bcmi2cnfc_id,
    .probe      = bcmi2cnfc_probe,
    .remove     = bcmi2cnfc_remove,
    .driver     = {
        .owner  = THIS_MODULE,
        .name   = "bcmi2cnfc",
    },
};

/*
 * module load/unload record keeping
 */

static int __init bcmi2cnfc_dev_init(void)
{
    pr_info("Loading bcmi2cnfc driver\n");
    return i2c_add_driver(&bcmi2cnfc_driver);
}
module_init(bcmi2cnfc_dev_init);

static void __exit bcmi2cnfc_dev_exit(void)
{
    pr_info("Unloading bcmi2cnfc driver\n");
    i2c_del_driver(&bcmi2cnfc_driver);
}
module_exit(bcmi2cnfc_dev_exit);

MODULE_AUTHOR("Broadcom");
MODULE_DESCRIPTION("NFC bcmi2cnfc driver");
MODULE_LICENSE("GPL");
