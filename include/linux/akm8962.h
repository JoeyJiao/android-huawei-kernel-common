/*
 * Definitions for akm8962 compass chip.
 */
#ifndef AKM8962_H
#define AKM8962_H

#include <linux/ioctl.h>

#define AKM8962_I2C_NAME "akm8962"

/*! \name AK8962 operation mode
 \anchor AK8962_Mode
 Defines an operation mode of the AK8962.*/
/*! @{*/
#define AK8962_MODE_SNG_MEASURE	0x01
#define	AK8962_MODE_SELF_TEST	0x08
#define	AK8962_MODE_FUSE_ACCESS	0x0F
#define	AK8962_MODE_POWERDOWN	0x00
/*! @}*/

#define SENSOR_DATA_SIZE		8	/* Rx buffer size, i.e from ST1 to ST2 */
#define RWBUF_SIZE				16	/* Read/Write buffer size.*/


/*! \name AK8962 register address
\anchor AK8962_REG
Defines a register address of the AK8962.*/
/*! @{*/
#define AK8962_REG_WIA		0x00
#define AK8962_REG_INFO		0x01
#define AK8962_REG_ST1		0x02
#define AK8962_REG_HXL		0x03
#define AK8962_REG_HXH		0x04
#define AK8962_REG_HYL		0x05
#define AK8962_REG_HYH		0x06
#define AK8962_REG_HZL		0x07
#define AK8962_REG_HZH		0x08
#define AK8962_REG_ST2		0x09
#define AK8962_REG_CNTL		0x0A
#define AK8962_REG_RSV		0x0B
#define AK8962_REG_ASTC		0x0C
#define AK8962_REG_TS1		0x0D
#define AK8962_REG_TS2		0x0E
#define AK8962_REG_I2CDIS	0x0F
/*! @}*/

/*! \name AK8962 fuse-rom address
\anchor AK8962_FUSE
Defines a read-only address of the fuse ROM of the AK8962.*/
/*! @{*/
#define AK8962_FUSE_ASAX	0x10
#define AK8962_FUSE_ASAY	0x11
#define AK8962_FUSE_ASAZ	0x12
/*! @}*/

#define AKMIO                   0xA1

/* IOCTLs for AKM library */
#define ECS_IOCTL_WRITE                 _IOW(AKMIO, 0x01, char*)
#define ECS_IOCTL_READ                  _IOWR(AKMIO, 0x02, char*)
#define ECS_IOCTL_RESET      	        _IO(AKMIO, 0x03) /* NOT used in AK8962 */
#define ECS_IOCTL_SET_MODE              _IOW(AKMIO, 0x04, short)
#define ECS_IOCTL_GETDATA               _IOR(AKMIO, 0x05, char[SENSOR_DATA_SIZE])
#define ECS_IOCTL_SET_YPR               _IOW(AKMIO, 0x06, short[12])
#define ECS_IOCTL_GET_OPEN_STATUS       _IOR(AKMIO, 0x07, int)
#define ECS_IOCTL_GET_CLOSE_STATUS      _IOR(AKMIO, 0x08, int)
#define ECS_IOCTL_GET_DELAY             _IOR(AKMIO, 0x30, short)
#define ECS_IOCTL_GET_PROJECT_NAME      _IOR(AKMIO, 0x0D, char[64])
#define ECS_IOCTL_GET_MATRIX            _IOR(AKMIO, 0x0E, short [4][3][3])

/* IOCTLs for APPs */
#define ECS_IOCTL_APP_SET_MODE		_IOW(AKMIO, 0x10, short)
#define ECS_IOCTL_APP_SET_MFLAG		_IOW(AKMIO, 0x11, short)
#define ECS_IOCTL_APP_GET_MFLAG		_IOW(AKMIO, 0x12, short)
#define ECS_IOCTL_APP_SET_AFLAG		_IOW(AKMIO, 0x13, short)
#define ECS_IOCTL_APP_GET_AFLAG		_IOR(AKMIO, 0x14, short)
#define ECS_IOCTL_APP_SET_TFLAG		_IOR(AKMIO, 0x15, short)/* NOT use */
#define ECS_IOCTL_APP_GET_TFLAG		_IOR(AKMIO, 0x16, short)/* NOT use */
#define ECS_IOCTL_APP_RESET_PEDOMETER   _IO(AKMIO, 0x17)	/* NOT use */
#define ECS_IOCTL_APP_SET_DELAY		_IOW(AKMIO, 0x18, short)
#define ECS_IOCTL_APP_GET_DELAY		ECS_IOCTL_GET_DELAY
#define ECS_IOCTL_APP_SET_MVFLAG	_IOW(AKMIO, 0x19, short)
#define ECS_IOCTL_APP_GET_MVFLAG	_IOR(AKMIO, 0x1A, short)

#define ECS_IOCTL_SET_CAL  		_IOR(AKMIO, 0x0C, short)
#define ECS_IOCTL_APP_GET_CAL  	_IOR(AKMIO, 0x21, short)

struct akm8962_platform_data {
	char layouts[3][3];
	char project_name[64];
	int gpio_DRDY;
};

#endif

