


/* drivers/i2c/chips/akm8973.c - akm8973 compass driver
 *
 * Copyright (C) 2007-2008 HTC Corporation.
 * Author: Hou-Kun Chen <houkun.chen@gmail.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */






#include <linux/interrupt.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/irq.h>
#include <linux/miscdevice.h>
#include <asm/gpio.h>
#include <asm/uaccess.h>
#include <linux/delay.h>
#include <linux/input.h>
#include <linux/workqueue.h>
#include <linux/freezer.h>
#include <linux/akm8973.h>
#include <linux/earlysuspend.h>

#ifdef CONFIG_ANDROID_POWER
#include <linux/android_power.h>
#endif

#include "linux/hardware_self_adapt.h"

#ifdef CONFIG_HUAWEI_HW_DEV_DCT
#include <linux/hw_dev_dec.h>
#endif

#define DEBUG 0
#define MAX_FAILURE_COUNT 10

#define COMPASS_RST    23
#define AKM8973_I2C_NAME "akm8973"

static struct i2c_client *this_client;


extern struct input_dev *sensor_dev;

struct akm8973_data {
	struct input_dev *input_dev;
	struct work_struct work;
	struct early_suspend early_suspend;

};

#ifdef CONFIG_HAS_EARLYSUSPEND
static void akm8973_early_suspend(struct early_suspend *h);
static void akm8973_early_resume(struct early_suspend *h);
#endif

static DECLARE_WAIT_QUEUE_HEAD(open_wq);


#ifdef CONFIG_UPDATE_COMPASS_FIRMWARE 
static char AKECS_ETST=0;
#define   AKECS_FACTORY_ETST       0xc7     /*the value  is factory  defined the goog chip*/
#define   AKECS_RECOMPOSE_ETST  0xCC    /* the value is recompose e2prom define value*/

static ssize_t set_e2prom_show(struct kobject *kobj, struct kobj_attribute *attr,char *buf);
static ssize_t  set_e2prom_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count);

static int akm8973_set_e2prom_file(void);

static struct kobj_attribute set_e2prom_attribute = {
	.attr = {.name = "set_e2prom", .mode = 0666},
	.show = set_e2prom_show,
	.store = set_e2prom_store,
};

#endif


static atomic_t open_count;
static atomic_t open_flag;
static atomic_t reserve_open_flag;

static atomic_t m_flag;
static atomic_t a_flag;
static atomic_t t_flag;
static atomic_t mv_flag;

static short akmd_delay = 0;

#ifdef CONFIG_ANDROID_POWER
static atomic_t suspend_flag = ATOMIC_INIT(0);
#endif

//static struct akm8973_platform_data *pdata;

/* following are the sysfs callback functions */

static int AKI2C_RxData(char *rxData, int length)
{
	struct i2c_msg msgs[] = {
		{
		 .addr = this_client->addr,
		 .flags = 0,
		 .len = 1,
		 .buf = rxData,
		 },
		{
		 .addr = this_client->addr,
		 .flags = I2C_M_RD,
		 .len = length,
		 .buf = rxData,
		 },
	};

	if (i2c_transfer(this_client->adapter, msgs, 2) < 0) {
		printk(KERN_ERR "AKI2C_RxData: transfer error\n");
		return -EIO;
	} else
		return 0;
}

static int AKI2C_TxData(char *txData, int length)
{
	struct i2c_msg msg[] = {
		{
		 .addr = this_client->addr,
		 .flags = 0,
		 .len = length,
		 .buf = txData,
		 },
	};

	if (i2c_transfer(this_client->adapter, msg, 1) < 0) {
		printk(KERN_ERR "AKI2C_TxData: transfer error\n");
		return -EIO;
	} else
		return 0;
}

static int AKECS_Init(void)
{

	return 0;
}

static void AKECS_Reset(void)
{
    gpio_tlmm_config(GPIO_CFG
        (COMPASS_RST, 0, GPIO_OUTPUT,
        GPIO_NO_PULL, GPIO_10MA),
        GPIO_ENABLE);

    gpio_set_value(COMPASS_RST, 0);
    udelay(120);
    gpio_set_value(COMPASS_RST, 1);
}


static int AKECS_StartMeasure(void)
{
	char buffer[2];
	/* Set measure mode */
	buffer[0] = AKECS_REG_MS1;
	buffer[1] = AKECS_MODE_MEASURE;

	/* Set data */
	return AKI2C_TxData(buffer, 2);
}

static int AKECS_PowerDown(void)
{
	char buffer[2];
	int ret;

	/* Set powerdown mode */
	buffer[0] = AKECS_REG_MS1;
	buffer[1] = AKECS_MODE_POWERDOWN;
	/* Set data */
	ret = AKI2C_TxData(buffer, 2);
	if (ret < 0)
		return ret;

	/* Dummy read for clearing INT pin */
	buffer[0] = AKECS_REG_TMPS;
	/* Read data */
	ret = AKI2C_RxData(buffer, 1);
	if (ret < 0)
		return ret;

	return ret;
}

static int AKECS_StartE2PRead(void)
{
	char buffer[2];
#if DEBUG
	printk(KERN_ERR "%s\n", __FUNCTION__);
#endif

	/* Set E2P mode */
	buffer[0] = AKECS_REG_MS1;
	buffer[1] = AKECS_MODE_E2P_READ;
	/* Set data */
	return AKI2C_TxData(buffer, 2);
}

static int AKECS_SetMode(char mode)
{
	int ret;
	switch (mode) {
	case AKECS_MODE_MEASURE:
		ret = AKECS_StartMeasure();
		break;
	case AKECS_MODE_E2P_READ:
		ret = AKECS_StartE2PRead();
		break;
	case AKECS_MODE_POWERDOWN:
		ret = AKECS_PowerDown();
		break;
	default:
		return -EINVAL;
	}

	/* wait at least 300us after changing mode */
	msleep(1);
	return ret;
}


static int AKECS_TransRBuff(char *rbuf, int size)
{
	int  i,ret;

	if(size < RBUFF_SIZE + 1)
	  return -EINVAL;

	// read C0 - C4
	rbuf[0] = AKECS_REG_ST;
	ret = AKI2C_RxData(rbuf, RBUFF_SIZE + 1);
	for(i=0;i<sizeof(rbuf);i++)
		printk(KERN_ERR "%x\n", rbuf[i]);
	
	return  ret;

}

#if 0
/********************************************************************/
int Acc_buf[3];
//extern int gs_adi_sensor_flag (void );
extern int gs_st_data_to_compass(int accel_data [3]);
//extern int gs_adi_data_to_compass(int accel_data [3]);

static int Compass_GetAccelerationData(int * accel_data )
{
	printk(KERN_DEBUG "akm Compass_GetAcceleaationData");
   	//if (gs_adi_sensor_flag())
	//	return gs_adi_data_to_compass(accel_data);
	
	//else
	int ret=gs_st_data_to_compass(accel_data);
	printk(KERN_DEBUG "akm Compass_GetAcceleaationData,accel_data[0]=%d,[1]=%d,[2]=%d",accel_data[0],accel_data[1],accel_data[2]);
		return ret;
}

/**********************************************************************/
#endif

static void AKECS_Report_Value(short *rbuf)
{
	struct akm8973_data *data = i2c_get_clientdata(this_client);
#if DEBUG
	printk(KERN_ERR "%s\n", __FUNCTION__);
#endif

#if DEBUG
	printk("AKECS_Report_Value: yaw = %d, pitch = %d, roll = %d\n",
	    rbuf[0], rbuf[1], rbuf[2]);
	printk("                    tmp = %d, m_stat= %d, g_stat=%d\n",
	    rbuf[3], rbuf[4], rbuf[5]);
	printk("      Acceleration:   x = %d LSB, y = %d LSB, z = %d LSB\n",
	    rbuf[6], rbuf[7], rbuf[8]);
	printk("          Magnetic:   x = %d LSB, y = %d LSB, z = %d LSB\n\n",
	    rbuf[9], rbuf[10], rbuf[11]);
#endif
	/* Report magnetic sensor information */
	if (atomic_read(&m_flag)) {
		input_report_abs(data->input_dev, ABS_RX, rbuf[0]);
		input_report_abs(data->input_dev, ABS_RY, rbuf[1]);
		input_report_abs(data->input_dev, ABS_RZ, rbuf[2]);
		input_report_abs(data->input_dev, ABS_RUDDER, rbuf[4]);
	}
	/* Report acceleration sensor information */
	if (atomic_read(&a_flag)) {
		input_report_abs(data->input_dev, ABS_X, rbuf[6]);
		input_report_abs(data->input_dev, ABS_Y, rbuf[7]);
		input_report_abs(data->input_dev, ABS_Z, rbuf[8]);
		input_report_abs(data->input_dev, ABS_WHEEL, rbuf[5]);
	}

	/* Report temperature information */
	if (atomic_read(&t_flag)) {
		input_report_abs(data->input_dev, ABS_THROTTLE, rbuf[3]);
	}

	if (atomic_read(&mv_flag)) {
		input_report_abs(data->input_dev, ABS_HAT0X, rbuf[9]);
		input_report_abs(data->input_dev, ABS_HAT0Y, rbuf[10]);
		input_report_abs(data->input_dev, ABS_BRAKE, rbuf[11]);
	}
	
	
	input_sync(data->input_dev);
}

static int AKECS_GetOpenStatus(void)
{

#if DEBUG
	printk(KERN_ERR "%s\n", __FUNCTION__);
#endif
	wait_event_interruptible(open_wq, (atomic_read(&open_flag) != 0));
	return atomic_read(&open_flag);

}

static int AKECS_GetCloseStatus(void)
{

#if DEBUG
	printk(KERN_ERR "%s\n", __FUNCTION__);
#endif
	wait_event_interruptible(open_wq, (atomic_read(&open_flag) <= 0));
	return atomic_read(&open_flag);



}

static void AKECS_CloseDone(void)
{
#if DEBUG
	printk(KERN_ERR "%s\n", __FUNCTION__);
#endif
	atomic_set(&m_flag, 1);
	atomic_set(&a_flag, 1);
	atomic_set(&t_flag, 1);
	atomic_set(&mv_flag, 1);
}

static int akm_aot_open(struct inode *inode, struct file *file)
{
	int ret = -1;
#if DEBUG
	printk(KERN_ERR "%s\n", __FUNCTION__);
#endif
	if (atomic_cmpxchg(&open_count, 0, 1) == 0) {
		if (atomic_cmpxchg(&open_flag, 0, 1) == 0) {
			atomic_set(&reserve_open_flag, 1);
			wake_up(&open_wq);
			ret = 0;
		}
	}
	return ret;
}

static int akm_aot_release(struct inode *inode, struct file *file)
{
#if DEBUG
	printk(KERN_ERR "%s\n", __FUNCTION__);
#endif
	atomic_set(&reserve_open_flag, 0);
	atomic_set(&open_flag, 0);
	atomic_set(&open_count, 0);
	wake_up(&open_wq);
	return 0;
}

static int
akm_aot_ioctl(struct inode *inode, struct file *file,
	      unsigned int cmd, unsigned long arg)
{
	void __user *argp = (void __user *)arg;
	short flag;
#if DEBUG
	printk(KERN_ERR "%s\n", __FUNCTION__);
#endif

	switch (cmd) {
	case ECS_IOCTL_APP_SET_MFLAG:     /*set open magnetic sensor flag*/
	case ECS_IOCTL_APP_SET_AFLAG:     /*set open acceleration sensor flag*/
	case ECS_IOCTL_APP_SET_TFLAG:     /*set open temprature sensor flag*/
	case ECS_IOCTL_APP_SET_MVFLAG:   /*set open move  sensor flag*/
		if (copy_from_user(&flag, argp, sizeof(flag)))
			return -EFAULT;
		if (flag < 0 || flag > 1)
			return -EINVAL;
		break;
	case ECS_IOCTL_APP_SET_DELAY:
		if (copy_from_user(&flag, argp, sizeof(flag)))
			return -EFAULT;
		break;
	default:
		break;
	}

	switch (cmd) {
	case ECS_IOCTL_APP_SET_MFLAG: 
		atomic_set(&m_flag, flag);
		break;
	case ECS_IOCTL_APP_GET_MFLAG: /*get open magnetic sensor flag*/
		flag = atomic_read(&m_flag);
		break;
	case ECS_IOCTL_APP_SET_AFLAG:
		atomic_set(&a_flag, flag);
		//printk(KERN_DEBUG "Inside akm ECS_IOCTL_APP_SET_AFLAG=%d",atomic_read(&a_flag));
		break;
	case ECS_IOCTL_APP_GET_AFLAG:  /*get open acceleration sensor flag*/
		flag = atomic_read(&a_flag);
		break;
	case ECS_IOCTL_APP_SET_TFLAG:
		atomic_set(&t_flag, flag);
		break;
	case ECS_IOCTL_APP_GET_TFLAG:
		flag = atomic_read(&t_flag);/*get open tempature sensor flag*/
		break;
	case ECS_IOCTL_APP_SET_MVFLAG:
		atomic_set(&mv_flag, flag);
		break;
	case ECS_IOCTL_APP_GET_MVFLAG: /*get open move sensor flag*/
		flag = atomic_read(&mv_flag);
		break;
	case ECS_IOCTL_APP_SET_DELAY:
		akmd_delay = flag;
		break;
	case ECS_IOCTL_APP_GET_DELAY:
		flag = akmd_delay;
		break;
	case ECS_IOCTL_APP_GET_DEVID:
		break;
	default:
		return -ENOTTY;
	}

	switch (cmd) {
	case ECS_IOCTL_APP_GET_MFLAG:
	case ECS_IOCTL_APP_GET_AFLAG:
	case ECS_IOCTL_APP_GET_TFLAG:
	case ECS_IOCTL_APP_GET_MVFLAG:
	case ECS_IOCTL_APP_GET_DELAY:
		if (copy_to_user(argp, &flag, sizeof(flag)))
			return -EFAULT;
		break;
	case ECS_IOCTL_APP_GET_DEVID:
		if (copy_to_user(argp, AKM8973_I2C_NAME, strlen(AKM8973_I2C_NAME)+1))
			return -EFAULT;
		break;
	default:
		break;
	}

	return 0;
}

static int akmd_open(struct inode *inode, struct file *file)
{
#if DEBUG
	printk(KERN_ERR "%s\n", __FUNCTION__);
#endif
	return nonseekable_open(inode, file);
}

static int akmd_release(struct inode *inode, struct file *file)
{
#if DEBUG
	printk(KERN_ERR "%s\n", __FUNCTION__);
#endif
	AKECS_CloseDone();
	return 0;
}

static int
akmd_ioctl(struct inode *inode, struct file *file, unsigned int cmd,
	   unsigned long arg)
{
#if DEBUG
	int i;
#endif
	void __user *argp = (void __user *)arg;

	char msg[RBUFF_SIZE + 1], rwbuf[16];//, numfrq[2];
	int ret = -1, status;
	short mode, value[12], delay; /* step_count,*/
//	char *pbuffer = 0;
#if DEBUG
	printk(KERN_ERR "%s\n", __FUNCTION__);
#endif

	switch (cmd) {
	case ECS_IOCTL_READ:
	case ECS_IOCTL_WRITE:
		if (copy_from_user(&rwbuf, argp, sizeof(rwbuf)))
			return -EFAULT;
		break;
	case ECS_IOCTL_SET_MODE:
		if (copy_from_user(&mode, argp, sizeof(mode)))
			return -EFAULT;
		break;
	case ECS_IOCTL_SET_YPR:
		if (copy_from_user(&value, argp, sizeof(value)))
			return -EFAULT;
		break;
	
	default:
		break;
	}

	switch (cmd) {
	case ECS_IOCTL_INIT:
#if DEBUG
		printk("ECS_IOCTL_INIT %x\n", cmd);
#endif
		ret = AKECS_Init();
		if (ret < 0)
			return ret;
		break;
	case ECS_IOCTL_RESET:
#if DEBUG
		printk("ECS_IOCTL_RESET %x\n", cmd);
#endif
		AKECS_Reset();
		break;
	case ECS_IOCTL_READ:
#if DEBUG
		printk("ECS_IOCTL_READ %x\n", cmd);
		printk(" len %02x:", rwbuf[0]);
		printk(" addr %02x:", rwbuf[1]);
#endif
		if (rwbuf[0] < 1)
			return -EINVAL;
		ret = AKI2C_RxData(&rwbuf[1], rwbuf[0]);
#if DEBUG
		for(i=0; i<rwbuf[0]; i++){
			printk(" %02x", rwbuf[i+1]);
		}
		printk(" ret = %d\n", ret);
#endif
		if (ret < 0)
			return ret;
		break;
	case ECS_IOCTL_WRITE:
#if DEBUG
		printk("ECS_IOCTL_WRITE %x\n", cmd);
		printk(" len %02x:", rwbuf[0]);
		for(i=0; i<rwbuf[0]; i++){
			printk(" %02x", rwbuf[i+1]);
		}
#endif
		if (rwbuf[0] < 2)
			return -EINVAL;
		ret = AKI2C_TxData(&rwbuf[1], rwbuf[0]);
#if DEBUG
		printk(" ret = %d\n", ret);
#endif
		if (ret < 0)
			return ret;
		break;
	case ECS_IOCTL_SET_MODE:
#if DEBUG
		printk("ECS_IOCTL_SET_MODE %x mode=%x\n", cmd, mode);
#endif
		ret = AKECS_SetMode((char)mode);
#if DEBUG
		printk(" ret = %d\n", ret);
#endif
		if (ret < 0)
			return ret;
		break;
	case ECS_IOCTL_GETDATA:
#if DEBUG
		printk("ECS_IOCTL_GETDATA %x\n", cmd);
#endif
		ret = AKECS_TransRBuff(msg, RBUFF_SIZE+1);
#if DEBUG
		printk(" ret = %d\n", ret);
#endif
		if (ret < 0)
			return ret;
#if DEBUG
		for(i=0; i<ret; i++){
			printk(" %02x", msg[i]);
		}
		printk("\n");
#endif
		break;
	case ECS_IOCTL_SET_YPR:
#if DEBUG
		printk("ECS_IOCTL_SET_YPR %x ypr=%x\n", cmd, value);
#endif
		AKECS_Report_Value(value);
		break;
	case ECS_IOCTL_GET_OPEN_STATUS:
#if DEBUG
		printk("ECS_IOCTL_GET_OPEN_STATUS %x start\n", cmd);
#endif
		status = AKECS_GetOpenStatus();
#if DEBUG
		printk("ECS_IOCTL_GET_OPEN_STATUS %x end status=%x\n", cmd, status);
#endif
		break;
	case ECS_IOCTL_GET_CLOSE_STATUS:
#if DEBUG
		printk("ECS_IOCTL_GET_CLOSE_STATUS %x start\n", cmd);
#endif
		status = AKECS_GetCloseStatus();
#if DEBUG
		printk("ECS_IOCTL_GET_CLOSE_STATUS %x end status=%x\n", cmd, status);
#endif
		break;

#if 0
/*************************************************************/

	case  ECS_IOCTL_READ_ACCEL_XYZ:
		
#if DEBUG
		printk("ECS_IOCTL_READ_ACCEL_XYZ %x start\n", cmd);
#endif
		status = Compass_GetAccelerationData(Acc_buf);
             
#if DEBUG
		printk("ECS_IOCTL_READ_ACCEL_XYZ %x end status=%x\n", cmd, status);
#endif
		break;
/*************************************************************/
#endif


	case ECS_IOCTL_GET_DELAY:
		delay = akmd_delay;
#if DEBUG
		printk("ECS_IOCTL_GET_DELAY %x delay=%x\n", cmd, delay);
#endif
		break;
	default:
#if DEBUG
		printk("Unknown cmd %x\n", cmd);
#endif
		return -ENOTTY;
	}

	switch (cmd) {
	case ECS_IOCTL_READ:
		if (copy_to_user(argp, &rwbuf, sizeof(rwbuf)))
			return -EFAULT;
		break;
	case ECS_IOCTL_GETDATA:
		if (copy_to_user(argp, &msg, sizeof(msg)))
			return -EFAULT;
		break;
	case ECS_IOCTL_GET_OPEN_STATUS:
		if (copy_to_user(argp, &status, sizeof(status)))
			return -EFAULT;
	case ECS_IOCTL_GET_CLOSE_STATUS:
		if (copy_to_user(argp, &status, sizeof(status)))
			return -EFAULT;
		break;
	case ECS_IOCTL_GET_DELAY:
		if (copy_to_user(argp, &delay, sizeof(delay)))
			return -EFAULT;
		break;
#if 0		
/*************************************************************/
	case ECS_IOCTL_READ_ACCEL_XYZ:
		if (copy_to_user(argp, &Acc_buf, sizeof(Acc_buf)))
			return -EFAULT;
		break;
/*************************************************************/
#endif

		
	default:
		break;
	}

	return 0;
}

static int AKECS_CheckChipName(void)
{
	char buffer[2];
	int ret;

	/* Set device info reg*/
	buffer[0] = AKECS_MODE_MEASURE;
	/* Read data */
	ret = AKI2C_RxData(buffer, 1);
	if (ret < 0) {
		printk(KERN_ERR "akm8973_probe: Failed to check device id = %d\n", ret);
	}
	return ret;
}

static int akm8973_init_client(struct i2c_client *client)
{
	
#if DEBUG
	printk(KERN_ERR "%s\n", __FUNCTION__);
#endif
	
	init_waitqueue_head(&open_wq);

	/* As default, report all information */
	atomic_set(&m_flag, 1);
	atomic_set(&a_flag, 1);
	atomic_set(&t_flag, 1);
	atomic_set(&mv_flag, 1);

	return 0;
}

static struct file_operations akmd_fops = {
	.owner = THIS_MODULE,
	.open = akmd_open,
	.release = akmd_release,
	.ioctl = akmd_ioctl,
};

static struct file_operations akm_aot_fops = {
	.owner = THIS_MODULE,
	.open = akm_aot_open,
	.release = akm_aot_release,
	.ioctl = akm_aot_ioctl,
};

static struct miscdevice akm_aot_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "akm8973_aot",
	.fops = &akm_aot_fops,
};

static struct miscdevice akmd_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "akm8973_dev",
	.fops = &akmd_fops,
};

int akm8973_probe(struct i2c_client *client, const struct i2c_device_id * devid)
{
	struct akm8973_data *akm;
	int err;
#if DEBUG
	printk(KERN_ERR "%s\n", __FUNCTION__);
#endif

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		err = -ENODEV;
		goto exit_check_functionality_failed;
	}

	akm = kzalloc(sizeof(struct akm8973_data), GFP_KERNEL);
	if (!akm) {
		err = -ENOMEM;
		goto exit_alloc_data_failed;
	}

   	i2c_set_clientdata(client, akm);
	akm8973_init_client(client);
	this_client = client;

	akm->input_dev = sensor_dev;
	if ((akm->input_dev == NULL)||((akm->input_dev->id.vendor != GS_ADIX345)&&(akm->input_dev->id.vendor != GS_ST35DE))) {
		err = -ENOMEM;
		printk(KERN_ERR "akm8973_probe: Failed to allocate input device\n");
		goto exit_input_dev_alloc_failed;
	}

	AKECS_Reset();
	mdelay(2);
	err = AKECS_CheckChipName();
	if(err < 0){
		err = -ENODEV;
		printk(KERN_ERR "akm8973_probe: Failed to check device id = %d\n", err);
		goto exit_input_dev_alloc_failed;
	}

	set_bit(EV_ABS, akm->input_dev->evbit);
	set_bit(ABS_X, akm->input_dev->absbit);
	set_bit(ABS_Y, akm->input_dev->absbit);
	set_bit(ABS_Z, akm->input_dev->absbit);
	/* azimuth */
	input_set_abs_params(akm->input_dev, ABS_RX, 0, 360, 0, 0);
	/* pitch */
	input_set_abs_params(akm->input_dev, ABS_RY, -180, 180, 0, 0);
	/* roll */
	input_set_abs_params(akm->input_dev, ABS_RZ, -90, 90, 0, 0);
	/* temparature */
	input_set_abs_params(akm->input_dev, ABS_THROTTLE, -30, 85, 0, 0);
	/* status of magnetic sensor */
	input_set_abs_params(akm->input_dev, ABS_RUDDER, 0, 3, 0, 0);
	
	/* x-axis of raw magnetic vector */
	input_set_abs_params(akm->input_dev, ABS_HAT0X, -2048, 2032, 0, 0);
	/* y-axis of raw magnetic vector */
	input_set_abs_params(akm->input_dev, ABS_HAT0Y, -2048, 2032, 0, 0);
	/* z-axis of raw magnetic vector */
	input_set_abs_params(akm->input_dev, ABS_BRAKE, -2048, 2032, 0, 0);

    // delete register compass input dev, and let err=0
//modified by Joey Jiao    
/*
	akm->input_dev->name = "compass";
	err = input_register_device(akm->input_dev);

    //err = 0;//disabled by Joey Jiao
	if (err) {
		printk(KERN_ERR
		       "akm8973_probe: Unable to register input device: %s\n",
		       akm->input_dev->name);
		goto exit_input_register_device_failed;
	}*/

	err = misc_register(&akmd_device);
	if (err) {
		printk(KERN_ERR "akm8973_probe: akmd_device register failed\n");
		goto exit_misc_device_register_failed;
	}

	err = misc_register(&akm_aot_device);
	if (err) {
		printk(KERN_ERR
		       "akm8973_probe: akm_aot_device register failed\n");
		goto exit_misc_device_register_failed;
	}
//Modified by Joey Jiao from 0 -> 1
#if 0
	AKECS_Reset();
	AKECS_StartMeasure();
#endif

#ifdef CONFIG_HAS_EARLYSUSPEND
	akm->early_suspend.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN + 1;
	akm->early_suspend.suspend = akm8973_early_suspend;
	akm->early_suspend.resume = akm8973_early_resume;
	register_early_suspend(&akm->early_suspend);
#endif

#ifdef CONFIG_UPDATE_COMPASS_FIRMWARE
	akm8973_set_e2prom_file();
#endif
	AKECS_Reset();

	printk(KERN_INFO "Compass akm8973 is successfully probed.\n");
#ifdef CONFIG_HUAWEI_HW_DEV_DCT
    /* detect current device successful, set the flag as present */
    set_hw_dev_flag(DEV_I2C_COMPASS);
#endif

	return 0;

exit_misc_device_register_failed:
exit_input_register_device_failed:
	input_free_device(akm->input_dev);
exit_input_dev_alloc_failed:
	kfree(akm);
exit_alloc_data_failed:
exit_check_functionality_failed:
	return err;
}

static int akm8973_detect(struct i2c_client *client, int kind,
			  struct i2c_board_info *info)
{
#if DEBUG
	printk(KERN_ERR "%s\n", __FUNCTION__);
#endif
	strlcpy(info->type, "akm8973", I2C_NAME_SIZE);
	return 0;
}

static int akm8973_remove(struct i2c_client *client)
{
	struct akm8973_data *akm = i2c_get_clientdata(client);
#if DEBUG
	printk(KERN_ERR "AK8973 compass driver: remove\n");
#endif
	input_unregister_device(akm->input_dev);

	//i2c_detach_client(client);
	kfree(akm);
	return 0;
}


static int akm8973_suspend(struct i2c_client *client, pm_message_t mesg)
{ 

	int ret;
#if DEBUG
	printk(KERN_ERR "AK8973 compass driver: akm8973_suspend\n");
#endif
	disable_irq(client->irq);
	ret = AKECS_SetMode(AKECS_MODE_POWERDOWN);
	if (ret < 0)
			printk(KERN_ERR "akm8973_suspend power off failed\n");

	atomic_set(&reserve_open_flag, atomic_read(&open_flag));
	atomic_set(&open_flag, 0);
	wake_up(&open_wq);
	
	return 0;
}

static int akm8973_resume(struct i2c_client *client)
{

	int ret;
#if DEBUG
	printk(KERN_ERR "AK8973 compass driver: akm8973_resume\n");
#endif
	ret = AKECS_SetMode(AKECS_MODE_MEASURE);
	if (ret < 0)
			printk(KERN_ERR "akm8973_resume power measure failed\n");
	enable_irq(client->irq);
	atomic_set(&open_flag, atomic_read(&reserve_open_flag));
	wake_up(&open_wq);

	return 0;
}


#ifdef CONFIG_HAS_EARLYSUSPEND
static void akm8973_early_suspend(struct early_suspend *h)
{
      struct akm8973_data *akm ;
	akm = container_of(h, struct akm8973_data, early_suspend);
	
	akm8973_suspend(this_client, PMSG_SUSPEND);
}

static void akm8973_early_resume(struct early_suspend *h)
{	
	struct akm8973_data *akm ;
	akm = container_of(h, struct akm8973_data, early_suspend);
	
	akm8973_resume(this_client);
}
#endif
 
#ifdef CONFIG_UPDATE_COMPASS_FIRMWARE 

static int AKECS_StartE2PWrite(void)
{
	char buffer[2];
       buffer[0] = AKECS_REG_MS1;
	buffer[1] = 0xAA;    /*write e2prom */
	return AKI2C_TxData(buffer, 2);
}

static  int AKECS_WriteE2PReg( void )
{
	int ret;
	char tx_buffer[3];
	
	tx_buffer[0] = AKECS_REG_ETS;
	tx_buffer[1] =0x26;   /*akm factory support value*/
	ret= AKI2C_TxData(tx_buffer, 2);
	if(ret < 0) {
		printk(KERN_ERR "AKECS_WriteE2PReg: transfer error\n");
		return -EIO;
	}
	msleep(10);
	tx_buffer[0] = AKECS_REG_EVIR;
	tx_buffer[1] =0x77;   /*akm factory support value*/
	ret= AKI2C_TxData(tx_buffer, 2);
	if(ret < 0) {
		printk(KERN_ERR "AKECS_WriteE2PReg: transfer error\n");
		return -EIO;
	}
	msleep(10);
	tx_buffer[0] = AKECS_REG_EIHE;
	tx_buffer[1] =0x66;   /*akm factory support value*/
	ret= AKI2C_TxData(tx_buffer, 2);
	if(ret < 0) {
		printk(KERN_ERR "AKECS_WriteE2PReg: transfer error\n");
		return -EIO;
	}
	msleep(10);
	tx_buffer[0] = AKECS_REG_EHXGA;
	tx_buffer[1] =0x06;   /*akm factory support value*/
	ret= AKI2C_TxData(tx_buffer, 2);
	if(ret < 0) {
		printk(KERN_ERR "AKECS_WriteE2PReg: transfer error\n");
		return -EIO;
	}
	msleep(10);
	tx_buffer[0] = AKECS_REG_EHYGA;
	tx_buffer[1] =0x06;   /*akm factory support value*/
	ret= AKI2C_TxData(tx_buffer, 2);
	if(ret < 0) {
		printk(KERN_ERR "AKECS_WriteE2PReg: transfer error\n");
		return -EIO;
	}
	msleep(10);
	tx_buffer[0] = AKECS_REG_EHZGA;
	tx_buffer[1] =0x07;   /*akm factory support value*/
	ret= AKI2C_TxData(tx_buffer, 2);
	if(ret < 0) {
		printk(KERN_ERR "AKECS_WriteE2PReg: transfer error\n");
		return -EIO;
	}
	msleep(10);
	return ret;
}

static  int AKECS_ReadE2PReg( void )
{
	int ret;
	char rx_buffer[6];
	rx_buffer[0] = AKECS_REG_ETS;
	ret= AKI2C_RxData(rx_buffer, 5);
	if(ret < 0) {
		printk(KERN_ERR "AKECS_ReadE2PReg: transfer error\n");
		return -EIO;
	}
	rx_buffer[0] = AKECS_REG_EHXGA;
	ret= AKI2C_RxData(rx_buffer, 4);
	if(ret < 0) {
		printk(KERN_ERR "AKECS_ReadE2PReg: transfer error\n");
		return -EIO;
	}
			
	if(( rx_buffer[0] ==0x06)&&( rx_buffer[1] ==0x06)&& rx_buffer[2] ==0x07)
		return 0;
	else 
		return -EIO;
	
}

static int akm8973_set_e2prom_file(void)
{
	int ret;
	struct kobject *kobject_akm;
	kobject_akm = kobject_create_and_add("compass", NULL);
	if (!kobject_akm) {
		printk(KERN_ERR "create kobjetct error!\n");
		return -EIO;
	}
	ret = sysfs_create_file(kobject_akm, &set_e2prom_attribute.attr);
	if (ret) {
		kobject_put(kobject_akm);
		printk(KERN_ERR "create file error\n");
		return -EIO;
	}
	return 0;	
}

static ssize_t set_e2prom_show(struct kobject *kobj, struct kobj_attribute *attr,char *buf)
{
	AKECS_StartE2PRead();
	msleep(1);  /* start-upwait at least 300us */
	AKECS_ETST=i2c_smbus_read_byte_data(this_client,AKECS_REG_ETST); 
	return sprintf(buf, "%x\n", AKECS_ETST);
}

static ssize_t  set_e2prom_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	int ret,i=10;
	if(buf[0] != '1')
	{   
		return -1;
	}
	AKECS_StartE2PRead();
	msleep(1);  /* start-upwait at least 300us */
	AKECS_ETST=i2c_smbus_read_byte_data(this_client,AKECS_REG_ETST); 
	if((AKECS_ETST != AKECS_FACTORY_ETST)&&(AKECS_ETST != AKECS_RECOMPOSE_ETST)){
		while(i--){
			ret = AKECS_StartE2PWrite();
			if(ret<0)
				return ret;
			msleep(1);
			ret = AKECS_WriteE2PReg();
			if(ret<0)
				return ret;
			ret = AKECS_StartE2PRead();
			if(ret<0)
				return ret;
			msleep(1);
			if(!AKECS_ReadE2PReg())
				break;
		}
	}
	ret = AKECS_StartE2PWrite();
	if(ret<0)
		return ret;
	msleep(1);
	ret = i2c_smbus_write_byte_data(this_client,AKECS_REG_ETST,AKECS_RECOMPOSE_ETST);
	if(ret<0)
		return ret;
	msleep(10);
	ret = AKECS_SetMode(AKECS_MODE_POWERDOWN);
	if(ret<0)
		return ret;
	AKECS_ETST = AKECS_RECOMPOSE_ETST;
	return count;
}
#endif


static const struct i2c_device_id akm8973_id[] = {
	{ "akm8973", 0 },
	{ }
};

static struct i2c_driver akm8973_driver = {
	.class = I2C_CLASS_HWMON,
	.probe = akm8973_probe,
	.remove = akm8973_remove,

#ifndef CONFIG_HAS_EARLYSUSPEND
	.suspend = akm8973_suspend,
	.resume = akm8973_resume,
#endif

	.id_table = akm8973_id,
	.driver = {
		   .owner = THIS_MODULE,
		   .name = "akm8973",
		   },
	.detect = akm8973_detect,
	
};

static int __init akm8973_init(void)
{
#if DEBUG
	printk(KERN_ERR "AK8973 compass driver: init\n");
#endif
	return i2c_add_driver(&akm8973_driver);
}

static void __exit akm8973_exit(void)
{
#if DEBUG
	printk(KERN_ERR "AK8973 compass driver: exit\n");
#endif
	i2c_del_driver(&akm8973_driver);
}

__define_initcall("7s",akm8973_init,7s);
// module_init(akm8973_init);
module_exit(akm8973_exit);

MODULE_DESCRIPTION("AK8973 compass driver");
MODULE_LICENSE("GPL");

