#include "bf0_hal.h"
#include "board.h"
#include "drv_io.h"
#include "rtthread.h"
#include "stdbool.h"
#include "stdio.h"
#include "string.h"

#define DBG_TAG "i2c_example"
#define DBG_LVL DBG_LOG
#include <rtdbg.h>

static struct rt_i2c_bus_device* i2c_bus = NULL;

static const uint8_t AW32001_ADDRESS = 0x49;  // 7-bit I2C address for AW32001
static const char*   I2C_BUS_NAME    = "i2c2";

static bool i2c_init(struct rt_i2c_bus_device** bus)
{
  *bus = rt_i2c_bus_device_find(I2C_BUS_NAME);
  if (*bus == RT_NULL)
  {
    LOG_E("I2C bus not found");
    return false;
  }
  rt_kprintf("I2C bus found success\n");

  rt_device_open((rt_device_t) (*bus), RT_DEVICE_FLAG_RDWR);
  rt_kprintf("I2C bus opened success\n");
  struct rt_i2c_configuration configuration = {
      .mode    = 0,
      .addr    = 0,
      .timeout = 500,     // Waiting for timeout period (ms)
      .max_hz  = 400000,  // I2C rate (hz)
  };

  rt_i2c_configure(*bus, &configuration);

  rt_kprintf("I2C bus configured success\n");
  return true;
}

static void scan_i2c_devices(struct rt_i2c_bus_device** bus)
{
  rt_uint8_t        buf[1];
  struct rt_i2c_msg msg;

  msg.addr  = AW32001_ADDRESS;
  msg.flags = RT_I2C_RD; /* 读标志，底层会先发从机地址 + R/W = R */
  msg.buf   = buf;       /* 接收一个字节（不用在意接收到的值）*/
  msg.len   = 1;

  for (rt_uint8_t addr = 0x03; addr <= 0x77; addr++)
  {
    /* 只做一次字节的读操作来检测 ACK */
    msg.addr  = addr;
    msg.flags = RT_I2C_RD; /* 读标志，底层会先发从机地址 + R/W = R */
    msg.buf   = buf;       /* 接收一个字节（不用在意接收到的值）*/
    msg.len   = 1;

    rt_int32_t ret = rt_i2c_transfer(*bus, &msg, 1);
    if (ret == 1)
    {
      /* 设备有响应 */
      rt_kprintf("Find I2C device at address 0x%02X\n", addr);
    }
  }
}

static void read_chip_id(struct rt_i2c_bus_device** bus)
{
  rt_uint8_t        buf[1];
  struct rt_i2c_msg msg;
  rt_int32_t        ret;

  msg.addr  = AW32001_ADDRESS;
  msg.flags = RT_I2C_WR;
  msg.buf   = buf;
  msg.len   = 1;
  buf[0]    = 0x0A;  // AW32001 chip ID register address

  ret = rt_i2c_transfer(*bus, &msg, 1);
  if (ret != 1)
  {
    LOG_E("Failed to write to I2C device");
    return;
  }
  msg.flags = RT_I2C_RD;

  ret = rt_i2c_transfer(*bus, &msg, 1);
  if (ret != 1)
  {
    LOG_E("Failed to read from I2C device");
    return;
  }
  rt_kprintf("AW32001 chip ID: 0x%02X\n", buf[0]);
}

static void read_chip_id_simple(struct rt_i2c_bus_device** bus)
{
  rt_uint8_t buf[1];
  rt_int32_t ret = rt_i2c_mem_read(*bus, AW32001_ADDRESS, 0x0A, 8, buf, 1);
  if (ret != 1)
  {
    LOG_E("Failed to read from I2C device");
    return;
  }
  rt_kprintf("AW32001 chip ID: 0x%02X\n", buf[0]);
}

static void write_charge_current(struct rt_i2c_bus_device** bus, uint8_t current)
{
  const uint8_t CHARGE_CURRENT_ADDRESS = 0x02;
  rt_uint8_t    buf[1];
  rt_int32_t    ret;
  ret = rt_i2c_mem_read(*bus, AW32001_ADDRESS, CHARGE_CURRENT_ADDRESS, 8, buf, 1);
  if (ret != 1)
  {
    LOG_E("Failed to read from I2C device");
    return;
  }
  // current: bit0-bit5
  buf[0] = (buf[0] & 0xC0) | (current & 0x3F);
  ret    = rt_i2c_mem_write(*bus, AW32001_ADDRESS, CHARGE_CURRENT_ADDRESS, 8, buf, 1);
  if (ret != 1)
  {
    LOG_E("Failed to write to I2C device");
    return;
  }
  rt_kprintf("AW32001 charge current set to: 0x%02X\n", buf[0]);
}

/**
 * @brief  Main program
 * @param  None
 * @retval 0 if success, otherwise failure number
 */
int main(void)
{
  /* Output a message on console using printf function */
  rt_kprintf("Hello world!\n");

  if (i2c_init(&i2c_bus) == false)
  {
    LOG_E("I2C initialization failed");
    return -1;
  }
  if (i2c_bus == RT_NULL)
  {
    LOG_E("I2C bus not found");
    return false;
  }

  scan_i2c_devices(&i2c_bus);
  read_chip_id(&i2c_bus);
  read_chip_id_simple(&i2c_bus);

  uint8_t charge_current = 0;
  /* Infinite loop */
  while (1)
  {
    charge_current++;
    if (charge_current > 0x1F)
    {
      charge_current = 0;
    }
    write_charge_current(&i2c_bus, charge_current);
    rt_thread_mdelay(100);
  }
  return 0;
}
