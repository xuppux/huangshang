#include "rtconfig.h"
#include "bf0_hal.h"
#include "drv_io.h"
#include "stdio.h"
#include "string.h"

#include "rtthread.h"
#include "board.h"

#define KEY2 43
#define PIN 20


// 按键中断回调
static void key2_irq(void *args)
{
  if (rt_pin_read(KEY2) == 1)
  {
    rt_kprintf("KEY2 pressed\n"); // 打印按键按下
  }
  else
  {
    rt_kprintf("KEY2 released\n"); // 打印按键释放
  }
}

static void example_gpio(void)
{
    // 存储GPIO状态
    rt_uint8_t pin_state = 0;
    rt_kprintf("GPIO example\n");
    // 初始化GPIO引脚
    rt_pin_mode(PIN, PIN_MODE_OUTPUT);                // 配置为输出模式
    rt_pin_write(PIN, PIN_HIGH);                      // 设置引脚高电平
    rt_thread_mdelay(10);                             // 延时10ms

    pin_state = rt_pin_read(PIN);                     // 读取引脚状态
    rt_kprintf("PIN %d state: %d\n", PIN, pin_state); // 打印引脚状态

    rt_pin_write(PIN, PIN_LOW);                       
    rt_thread_mdelay(10);                             
    
    pin_state = rt_pin_read(PIN);                     
    rt_kprintf("PIN %d state: %d\n", PIN, pin_state);
    rt_kprintf("GPIO example end\n");

    rt_pin_mode(KEY2, PIN_MODE_INPUT);                                       // 配置为输入模式
    rt_pin_attach_irq(KEY2, PIN_IRQ_MODE_RISING_FALLING, key2_irq, RT_NULL); // 配置中断回调函数，上升和下降沿触发中断
    rt_pin_irq_enable(KEY2, PIN_IRQ_ENABLE);  
}

/**
  * @brief  Main program
  * @param  None
  * @retval 0 if success, otherwise failure number
  */
int main(void)
{
   
    example_gpio();
    while (1)
    {
        rt_kprintf("Waiting\n");
        rt_thread_mdelay(5000);
    }
    return 0;
}

/************************ (C) COPYRIGHT Sifli Technology *******END OF FILE****/

