/*
 * Copyright (c) 2006-2018, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2009-07-01     Bernard      the first version
 */
#include <rtthread.h>
#include "dm9000a.h"

#include <netif/ethernetif.h>
#include "lwipopts.h"
#include "stm32f10x.h"
#include "stm32f10x_fsmc.h"


// #define DM9000_DEBUG		1
#if DM9000_DEBUG
#define DM9000_TRACE	rt_kprintf
#else
#define DM9000_TRACE(...)
#endif

/*
 * DM9000 interrupt line is connected to PE4
 */
//--------------------------------------------------------

#define DM9000_PHY          0x40    /* PHY address 0x01 */
#define RST_1()             GPIO_SetBits(GPIOD,GPIO_Pin_7)
#define RST_0()             GPIO_ResetBits(GPIOD,GPIO_Pin_7)

#define MAX_ADDR_LEN 6
enum DM9000_PHY_mode
{
    DM9000_10MHD = 0, DM9000_100MHD = 1,
    DM9000_10MFD = 4, DM9000_100MFD = 5,
    DM9000_AUTO  = 8, DM9000_1M_HPNA = 0x10
};

enum DM9000_TYPE
{
    TYPE_DM9000E,
    TYPE_DM9000A,
    TYPE_DM9000B
};
//void EXTI9_5_IRQHandler(void)
//{
//    extern void rt_dm9000_isr(void);

//    /* enter interrupt */
//    rt_interrupt_enter();

//    /* Clear the DM9000A EXTI line pending bit */
//    EXTI_ClearITPendingBit(EXTI_Line6);

//    rt_dm9000_isr();

//    /* leave interrupt */
//    rt_interrupt_leave();
//}
struct rt_dm9000_eth
{
    /* inherit from ethernet device */
    struct eth_device parent;

    enum DM9000_TYPE type;
	  enum DM9000_PHY_mode mode;

    rt_uint8_t imr_all;

    rt_uint8_t packet_cnt;                  /* packet I or II */
    rt_uint16_t queue_packet_len;           /* queued packet (packet II) */

    /* interface address info. */
    rt_uint8_t  dev_addr[MAX_ADDR_LEN];		/* hw address	*/
};

static struct rt_dm9000_eth dm9000_device;
static struct rt_semaphore sem_ack, sem_lock;

void rt_dm9000_isr(void);

static void delay_ms(rt_uint32_t ms)
{
    rt_uint32_t len;
    for (;ms > 0; ms --)
        for (len = 0; len < 100; len++ );
}

/* Read a byte from I/O port */
rt_inline rt_uint8_t dm9000_io_read(rt_uint16_t reg)
{
    DM9000_IO = reg;
    return (rt_uint8_t) DM9000_DATA;
}

/* Write a byte to I/O port */
rt_inline void dm9000_io_write(rt_uint16_t reg, rt_uint16_t value)
{
    DM9000_IO = reg;
    DM9000_DATA = value;
}

/* Read a word from phyxcer */
rt_inline rt_uint16_t phy_read(rt_uint16_t reg)
{
    rt_uint16_t val;

    /* Fill the phyxcer register into REG_0C */
    dm9000_io_write(DM9000_EPAR, DM9000_PHY | reg);
    dm9000_io_write(DM9000_EPCR, 0xc);	/* Issue phyxcer read command */

    delay_ms(100);		/* Wait read complete */

    dm9000_io_write(DM9000_EPCR, 0x0);	/* Clear phyxcer read command */
    val = (dm9000_io_read(DM9000_EPDRH) << 8) | dm9000_io_read(DM9000_EPDRL);

    return val;
}

/* Write a word to phyxcer */
rt_inline void phy_write(rt_uint16_t reg, rt_uint16_t value)
{
    /* Fill the phyxcer register into REG_0C */
    dm9000_io_write(DM9000_EPAR, DM9000_PHY | reg);

    /* Fill the written data into REG_0D & REG_0E */
    dm9000_io_write(DM9000_EPDRL, (value & 0xff));
    dm9000_io_write(DM9000_EPDRH, ((value >> 8) & 0xff));
    dm9000_io_write(DM9000_EPCR, 0xa);	/* Issue phyxcer write command */

    delay_ms(500);		/* Wait write complete */

    dm9000_io_write(DM9000_EPCR, 0x0);	/* Clear phyxcer write command */
}

/* Set PHY operationg mode */
rt_inline void phy_mode_set(rt_uint32_t media_mode)
{
    rt_uint16_t phy_reg4 = 0x01e1, phy_reg0 = 0x1000;
    if (!(media_mode & DM9000_AUTO))
    {
        switch (media_mode)
        {
        case DM9000_10MHD:
            phy_reg4 = 0x21;
            phy_reg0 = 0x0000;
            break;
        case DM9000_10MFD:
            phy_reg4 = 0x41;
            phy_reg0 = 0x0100;
            break;
        case DM9000_100MHD:
            phy_reg4 = 0x81;
            phy_reg0 = 0x2000;
            break;
        case DM9000_100MFD:
            phy_reg4 = 0x101;
            phy_reg0 = 0x2100;
            break;
        }
        phy_write(4, phy_reg4);	/* Set PHY media mode */
        phy_write(0, phy_reg0);	/*  Tmp */
    }

    dm9000_io_write(DM9000_GPCR, 0x01);	/* Let GPIO0 output */
    dm9000_io_write(DM9000_GPR, 0x00);	/* Enable PHY */
}

/* interrupt service routine */
void rt_dm9000_isr()
{
    rt_uint16_t int_status;
    rt_uint16_t last_io;

    last_io = DM9000_IO;

    /* Disable all interrupts */
    dm9000_io_write(DM9000_IMR, IMR_PAR);

    /* Got DM9000 interrupt status */
    int_status = dm9000_io_read(DM9000_ISR);               /* Got ISR */
    dm9000_io_write(DM9000_ISR, int_status);    /* Clear ISR status */

	  DM9000_TRACE("dm9000 isr: int status %04x\n", int_status);

    /* receive overflow */
    if (int_status & ISR_ROS)
    {
        rt_kprintf("overflow\n");
    }

    if (int_status & ISR_ROOS)
    {
        rt_kprintf("overflow counter overflow\n");
    }

    /* Received the coming packet */
    if (int_status & ISR_PRS)
    {
	    /* disable receive interrupt */
	    dm9000_device.imr_all = IMR_PAR | IMR_PTM;

        /* a frame has been received */
        eth_device_ready(&(dm9000_device.parent));
    }

    /* Transmit Interrupt check */
    if (int_status & ISR_PTS)
    {
        /* transmit done */
        int tx_status = dm9000_io_read(DM9000_NSR);    /* Got TX status */

        if (tx_status & (NSR_TX2END | NSR_TX1END))
        {
            dm9000_device.packet_cnt --;
            if (dm9000_device.packet_cnt > 0)
            {
            	DM9000_TRACE("dm9000 isr: tx second packet\n");

                /* transmit packet II */
                /* Set TX length to DM9000 */
                dm9000_io_write(DM9000_TXPLL, dm9000_device.queue_packet_len & 0xff);
                dm9000_io_write(DM9000_TXPLH, (dm9000_device.queue_packet_len >> 8) & 0xff);

                /* Issue TX polling command */
                dm9000_io_write(DM9000_TCR, TCR_TXREQ);	/* Cleared after TX complete */
            }

            /* One packet sent complete */
            rt_sem_release(&sem_ack);
        }
    }

    /* Re-enable interrupt mask */
    dm9000_io_write(DM9000_IMR, dm9000_device.imr_all);

    DM9000_IO = last_io;
}

/* RT-Thread Device Interface */
/* initialize the interface */
static rt_err_t rt_dm9000_init(rt_device_t dev)
{
    int i, oft, lnk;
    rt_uint32_t value = 0;
		RST_0();
		rt_thread_mdelay(25);//DM9000硬件复位结束
		RST_1();
		rt_thread_mdelay(100);//DM9000硬件复位结束							//一定要有这个延时，让DM9000准备就绪！
		dm9000_io_write(DM9000_GPCR,0x01);			//第一步:设置GPCR寄存器(0X1E)的bit0为1 
		dm9000_io_write(DM9000_GPR,0);				//第二步:设置GPR寄存器(0X1F)的bit1为0，DM9000内部的PHY上电
		dm9000_io_write(DM9000_NCR,(0x02|NCR_RST));	//第三步:软件复位DM9000 
		do 
		{
			delay_ms(25); 	
		}while(dm9000_io_read(DM9000_NCR)&1);		//等待DM9000软复位完成
		dm9000_io_write(DM9000_NCR,0);
		dm9000_io_write(DM9000_NCR,(0x02|NCR_RST));	//DM9000第二次软复位
		do 
		{
			delay_ms(25);	
		}while (dm9000_io_read(DM9000_NCR)&1);
			/* identfy DM9000 */
    value  = dm9000_io_read(DM9000_VIDL);
    value |= dm9000_io_read(DM9000_VIDH) << 8;
    value |= dm9000_io_read(DM9000_PIDL) << 16;
    value |= dm9000_io_read(DM9000_PIDH) << 24;
    if (value == DM9000_ID)
    {
      rt_kprintf("dm9000 id: 0x%x\n", value);
    }
    else
    {
        return -RT_ERROR;
    }

    /* GPIO0 on pre-activate PHY */
    dm9000_io_write(DM9000_GPR, 0x00);	            /* REG_1F bit0 activate phyxcer */
    dm9000_io_write(DM9000_GPCR, GPCR_GEP_CNTL);    /* Let GPIO0 output */
    dm9000_io_write(DM9000_GPR, 0x00);                 /* Enable PHY */

    /* Set PHY */
    phy_mode_set(dm9000_device.mode);

    /* Program operating register */
    dm9000_io_write(DM9000_NCR, 0x00);	/* only intern phy supported by now */
    dm9000_io_write(DM9000_TCR, 0x00);	    /* TX Polling clear */
    dm9000_io_write(DM9000_BPTR, 0x3f);	/* Less 3Kb, 200us */
    dm9000_io_write(DM9000_FCTR, FCTR_HWOT(3) | FCTR_LWOT(8));	/* Flow Control : High/Low Water */
    dm9000_io_write(DM9000_FCR, 0x00);	/* SH FIXME: This looks strange! Flow Control */
    dm9000_io_write(DM9000_SMCR, 0x00);	/* Special Mode */
    dm9000_io_write(DM9000_NSR, NSR_WAKEST | NSR_TX2END | NSR_TX1END);	/* clear TX status */
    dm9000_io_write(DM9000_ISR, 0x0f);	/* Clear interrupt status */
    dm9000_io_write(DM9000_TCR2, 0x80);	/* Switch LED to mode 1 */

    /* set mac address */
    for (i = 0, oft = 0x10; i < 6; i++, oft++)
        dm9000_io_write(oft, dm9000_device.dev_addr[i]);
    /* set multicast address */
    for (i = 0, oft = 0x16; i < 8; i++, oft++)
        dm9000_io_write(oft, 0xff);

    /* Activate DM9000 */
    dm9000_io_write(DM9000_RCR, RCR_DIS_LONG | RCR_DIS_CRC | RCR_RXEN);	/* RX enable */
    dm9000_io_write(DM9000_IMR, IMR_PAR);

	if (dm9000_device.mode == DM9000_AUTO)
	{
	    while (!(phy_read(1) & 0x20))
	    {
	        /* autonegation complete bit */
	        rt_thread_delay(10);
	        i++;
	        if (i == 10000)
	        {
	            rt_kprintf("could not establish link\n");
	            return 0;
	        }
	    }
	}

    /* see what we've got */
    lnk = phy_read(17) >> 12;
    rt_kprintf("operating at ");
    switch (lnk)
    {
			case 1:
					rt_kprintf("10M half duplex ");
					break;
			case 2:
					rt_kprintf("10M full duplex ");
					break;
			case 4:
					rt_kprintf("100M half duplex ");
					break;
			case 8:
					rt_kprintf("100M full duplex ");
					break;
			default:
					rt_kprintf("unknown: %d ", lnk);
					break;
    }
    rt_kprintf("mode\n");

    dm9000_io_write(DM9000_IMR, dm9000_device.imr_all);	/* Enable TX/RX interrupt mask */

    return RT_EOK;
}

static rt_err_t rt_dm9000_open(rt_device_t dev, rt_uint16_t oflag)
{
    return RT_EOK;
}

static rt_err_t rt_dm9000_close(rt_device_t dev)
{
    /* RESET devie */
    phy_write(0, 0x8000);	/* PHY RESET */
    dm9000_io_write(DM9000_GPR, 0x01);	/* Power-Down PHY */
    dm9000_io_write(DM9000_IMR, 0x80);	/* Disable all interrupt */
    dm9000_io_write(DM9000_RCR, 0x00);	/* Disable RX */

    return RT_EOK;
}

static rt_size_t rt_dm9000_read(rt_device_t dev, rt_off_t pos, void* buffer, rt_size_t size)
{
    rt_set_errno(-RT_ENOSYS);
    return 0;
}

static rt_size_t rt_dm9000_write (rt_device_t dev, rt_off_t pos, const void* buffer, rt_size_t size)
{
    rt_set_errno(-RT_ENOSYS);
    return 0;
}

static rt_err_t rt_dm9000_control(rt_device_t dev, int cmd, void *args)
{
    switch (cmd)
    {
    case NIOCTL_GADDR:
        /* get mac address */
        if (args) rt_memcpy(args, dm9000_device.dev_addr, 6);
        else return -RT_ERROR;
        break;

    default :
        break;
    }

    return RT_EOK;
}

/* ethernet device interface */
/* transmit packet. */
rt_err_t rt_dm9000_tx( rt_device_t dev, struct pbuf* p)
{
	DM9000_TRACE("dm9000 tx: %d\n", p->tot_len);

    /* lock DM9000 device */
    rt_sem_take(&sem_lock, RT_WAITING_FOREVER);

    /* disable dm9000a interrupt */
    dm9000_io_write(DM9000_IMR, IMR_PAR);

    /* Move data to DM9000 TX RAM */
    DM9000_outb(DM9000_IO_BASE, DM9000_MWCMD);

    {
		/* q traverses through linked list of pbuf's
		 * This list MUST consist of a single packet ONLY */
		struct pbuf *q;
		rt_uint16_t pbuf_index = 0;
		rt_uint8_t word[2], word_index = 0;

		q = p;
		/* Write data into dm9000a, two bytes at a time
		 * Handling pbuf's with odd number of bytes correctly
		 * No attempt to optimize for speed has been made */
		while (q)
		{
			if (pbuf_index < q->len)
			{
				word[word_index++] = ((u8_t*)q->payload)[pbuf_index++];
				if (word_index == 2)
				{
				    DM9000_outw(DM9000_DATA_BASE, (word[1] << 8) | word[0]);
					word_index = 0;
				}
			}
			else
			{
				q = q->next;
				pbuf_index = 0;
			}
		}
		/* One byte could still be unsent */
		if (word_index == 1)
		{
		    DM9000_outw(DM9000_DATA_BASE, word[0]);
		}
    }

    if (dm9000_device.packet_cnt == 0)
    {
    	DM9000_TRACE("dm9000 tx: first packet\n");

        dm9000_device.packet_cnt ++;
        /* Set TX length to DM9000 */
        dm9000_io_write(DM9000_TXPLL, p->tot_len & 0xff);
        dm9000_io_write(DM9000_TXPLH, (p->tot_len >> 8) & 0xff);

        /* Issue TX polling command */
        dm9000_io_write(DM9000_TCR, TCR_TXREQ);	/* Cleared after TX complete */
    }
    else
    {
    	DM9000_TRACE("dm9000 tx: second packet\n");

        dm9000_device.packet_cnt ++;
        dm9000_device.queue_packet_len = p->tot_len;
    }

    /* enable dm9000a interrupt */
    dm9000_io_write(DM9000_IMR, dm9000_device.imr_all);

    /* unlock DM9000 device */
    rt_sem_release(&sem_lock);

    /* wait ack */
    rt_sem_take(&sem_ack, RT_WAITING_FOREVER);

   	DM9000_TRACE("dm9000 tx done\n");

    return RT_EOK;
}

/* reception packet. */
struct pbuf *rt_dm9000_rx(rt_device_t dev)
{
    struct pbuf* p;
    rt_uint32_t rxbyte;

    /* init p pointer */
    p = RT_NULL;

    /* lock DM9000 device */
    rt_sem_take(&sem_lock, RT_WAITING_FOREVER);

    /* Check packet ready or not */
    dm9000_io_read(DM9000_MRCMDX);	    		/* Dummy read */
    rxbyte = DM9000_inb(DM9000_DATA_BASE);		/* Got most updated data */
    if (rxbyte)
    {
        rt_uint16_t rx_status, rx_len;
        rt_uint16_t* data;

        if (rxbyte > 1)
        {
			DM9000_TRACE("dm9000 rx: rx error, stop device\n");

            dm9000_io_write(DM9000_RCR, 0x00);	/* Stop Device */
            dm9000_io_write(DM9000_ISR, 0x80);	/* Stop INT request */
        }

        /* A packet ready now  & Get status/length */
        DM9000_outb(DM9000_IO_BASE, DM9000_MRCMD);

        rx_status = DM9000_inw(DM9000_DATA_BASE);
        rx_len = DM9000_inw(DM9000_DATA_BASE);

		DM9000_TRACE("dm9000 rx: status %04x len %d\n", rx_status, rx_len);

        /* allocate buffer */
        p = pbuf_alloc(PBUF_LINK, rx_len, PBUF_RAM);
        if (p != RT_NULL)
        {
            struct pbuf* q;
            rt_int32_t len;

            for (q = p; q != RT_NULL; q= q->next)
            {
                data = (rt_uint16_t*)q->payload;
                len = q->len;

                while (len > 0)
                {
                    *data = DM9000_inw(DM9000_DATA_BASE);
                    data ++;
                    len -= 2;
                }
            }
			DM9000_TRACE("\n");
        }
        else
        {
            rt_uint16_t dummy;
			rt_int32_t len;

			DM9000_TRACE("dm9000 rx: no pbuf\n");

            /* no pbuf, discard data from DM9000 */
            data = &dummy;
			len = rx_len;
            while (len > 0)
            {
                *data = DM9000_inw(DM9000_DATA_BASE);
                len -= 2;
            }
        }

        if ((rx_status & 0xbf00) || (rx_len < 0x40)
                || (rx_len > DM9000_PKT_MAX))
        {
			rt_kprintf("rx error: status %04x\n", rx_status);

            if (rx_status & 0x100)
            {
                rt_kprintf("rx fifo error\n");
            }
            if (rx_status & 0x200)
            {
                rt_kprintf("rx crc error\n");
            }
            if (rx_status & 0x8000)
            {
                rt_kprintf("rx length error\n");
            }
            if (rx_len > DM9000_PKT_MAX)
            {
                rt_kprintf("rx length too big\n");

                /* RESET device */
                dm9000_io_write(DM9000_NCR, NCR_RST);
                rt_thread_delay(1); /* delay 5ms */
            }

            /* it issues an error, release pbuf */
            pbuf_free(p);
            p = RT_NULL;
        }
    }
    else
    {
        /* restore receive interrupt */
	    dm9000_device.imr_all = IMR_PAR | IMR_PTM | IMR_PRM;
        dm9000_io_write(DM9000_IMR, dm9000_device.imr_all);
    }

    /* unlock DM9000 device */
    rt_sem_release(&sem_lock);

    return p;
}


int rt_hw_dm9000_init(void)
{
	 
    
		GPIO_InitTypeDef GPIO_InitStructure;
		EXTI_InitTypeDef EXTI_InitStructure;
		NVIC_InitTypeDef NVIC_InitStructure;
		
		FSMC_NORSRAMInitTypeDef FSMC_NORSRAMInitStructure;
		FSMC_NORSRAMTimingInitTypeDef ReadWriteTiming; 	//DM9000的读写时序
		
		RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOD|RCC_APB2Periph_GPIOE|\
								 RCC_APB2Periph_GPIOF|RCC_APB2Periph_GPIOG,ENABLE);	//使能GPIOD E F G时钟
		RCC_AHBPeriphClockCmd(RCC_AHBPeriph_FSMC,ENABLE);	//使能FSMC时钟
		RCC_APB2PeriphClockCmd(RCC_APB2Periph_AFIO,ENABLE);	//使能复用功能时钟

		GPIO_InitStructure.GPIO_Pin = GPIO_Pin_7; 		//PD7 推挽输出 
		GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
		GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;//推挽输出
		GPIO_Init(GPIOD,&GPIO_InitStructure);

		GPIO_InitStructure.GPIO_Pin = GPIO_Pin_6; 		//PG6 推挽输出 
		GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;	//上拉输入
		GPIO_Init(GPIOG,&GPIO_InitStructure);
		
		GPIO_InitStructure.GPIO_Pin = GPIO_Pin_0|GPIO_Pin_1|GPIO_Pin_4|GPIO_Pin_5|\
										GPIO_Pin_8|GPIO_Pin_9|GPIO_Pin_10|GPIO_Pin_14|GPIO_Pin_15; //PD0 1 4 5 8 9 10 14 15复用
		GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
		GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;//复用推挽输出
		GPIO_Init(GPIOD,&GPIO_InitStructure);
		
		GPIO_InitStructure.GPIO_Pin = GPIO_Pin_7|GPIO_Pin_8|GPIO_Pin_9|GPIO_Pin_10|GPIO_Pin_11|\
										GPIO_Pin_12|GPIO_Pin_13|GPIO_Pin_14|GPIO_Pin_15; 			//PG7 8 9 10 11 12 13 14 15复用
		GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
		GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;//复用推挽输出
		GPIO_Init(GPIOE,&GPIO_InitStructure);
		
		GPIO_InitStructure.GPIO_Pin = GPIO_Pin_13;		//PF13复用
		GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
		GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;//复用推挽输出
		GPIO_Init(GPIOF,&GPIO_InitStructure);
		
		GPIO_InitStructure.GPIO_Pin = GPIO_Pin_9;		//PG9复用
		GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
		GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;//复用推挽输出
		GPIO_Init(GPIOG,&GPIO_InitStructure);
		

		//PG6外部中断，中断线6
		GPIO_EXTILineConfig(GPIO_PortSourceGPIOG,GPIO_PinSource6);
		
		EXTI_InitStructure.EXTI_Line = EXTI_Line6;
		EXTI_InitStructure.EXTI_Mode = EXTI_Mode_Interrupt;
		EXTI_InitStructure.EXTI_Trigger = EXTI_Trigger_Falling;
		EXTI_InitStructure.EXTI_LineCmd = ENABLE;
		EXTI_Init(&EXTI_InitStructure);
		
		EXTI_ClearITPendingBit(EXTI_Line6); //清除中断线6挂起标志位
		
		NVIC_InitStructure.NVIC_IRQChannel = EXTI9_5_IRQn;			//外部中断线6
		NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 1;	//抢占优先级
		NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0;			//子优先级
		NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
		NVIC_Init(&NVIC_InitStructure);
		
		ReadWriteTiming.FSMC_AddressSetupTime = 0;		//地址建立时间
		ReadWriteTiming.FSMC_AddressHoldTime = 0;
		ReadWriteTiming.FSMC_DataSetupTime = 3;		//数据建立时间
		ReadWriteTiming.FSMC_BusTurnAroundDuration = 0x00;
		ReadWriteTiming.FSMC_CLKDivision = 0x00;
		ReadWriteTiming.FSMC_DataLatency = 0x00;
		ReadWriteTiming.FSMC_AccessMode = FSMC_AccessMode_A;//使用模式A
		
		FSMC_NORSRAMInitStructure.FSMC_Bank = FSMC_Bank1_NORSRAM2;	//NE2
		FSMC_NORSRAMInitStructure.FSMC_DataAddressMux = FSMC_DataAddressMux_Disable;
		FSMC_NORSRAMInitStructure.FSMC_MemoryType = FSMC_MemoryType_SRAM;
		FSMC_NORSRAMInitStructure.FSMC_MemoryDataWidth = FSMC_MemoryDataWidth_16b;
		FSMC_NORSRAMInitStructure.FSMC_BurstAccessMode = FSMC_BurstAccessMode_Disable;
		FSMC_NORSRAMInitStructure.FSMC_AsynchronousWait = FSMC_AsynchronousWait_Disable;
		FSMC_NORSRAMInitStructure.FSMC_WaitSignalPolarity = FSMC_WaitSignalPolarity_Low;
		FSMC_NORSRAMInitStructure.FSMC_WrapMode = FSMC_WrapMode_Disable;
		FSMC_NORSRAMInitStructure.FSMC_WaitSignalActive = FSMC_WaitSignalActive_BeforeWaitState;
		FSMC_NORSRAMInitStructure.FSMC_WriteOperation = FSMC_WriteOperation_Enable;
		FSMC_NORSRAMInitStructure.FSMC_WaitSignal = FSMC_WaitSignal_Disable;
		FSMC_NORSRAMInitStructure.FSMC_ExtendedMode = FSMC_ExtendedMode_Disable;
		FSMC_NORSRAMInitStructure.FSMC_WriteBurst = FSMC_WriteBurst_Disable;
		FSMC_NORSRAMInitStructure.FSMC_ReadWriteTimingStruct = &ReadWriteTiming;
		FSMC_NORSRAMInitStructure.FSMC_WriteTimingStruct = &ReadWriteTiming;
		FSMC_NORSRAMInit(&FSMC_NORSRAMInitStructure);
		FSMC_NORSRAMCmd(FSMC_Bank1_NORSRAM2,ENABLE); //使能FSMC的Bank1_Bank1_NORSRAM2
    rt_sem_init(&sem_ack, "tx_ack", 1, RT_IPC_FLAG_FIFO);
    rt_sem_init(&sem_lock, "eth_lock", 1, RT_IPC_FLAG_FIFO);

		dm9000_device.type  = TYPE_DM9000A;
		dm9000_device.mode	= DM9000_AUTO;
		dm9000_device.packet_cnt = 0;
		dm9000_device.queue_packet_len = 0;

    /*
     * SRAM Tx/Rx pointer automatically return to start address,
     * Packet Transmitted, Packet Received
     */
    dm9000_device.imr_all = IMR_PAR | IMR_PTM | IMR_PRM;

    dm9000_device.dev_addr[0] = 0x00;
    dm9000_device.dev_addr[1] = 0x60;
    dm9000_device.dev_addr[2] = 0x6E;
    dm9000_device.dev_addr[3] = 0x11;
    dm9000_device.dev_addr[4] = 0x22;
    dm9000_device.dev_addr[5] = 0x33;

    dm9000_device.parent.parent.init       = rt_dm9000_init;
    dm9000_device.parent.parent.open       = rt_dm9000_open;
    dm9000_device.parent.parent.close      = rt_dm9000_close;
    dm9000_device.parent.parent.read       = rt_dm9000_read;
    dm9000_device.parent.parent.write      = rt_dm9000_write;
    dm9000_device.parent.parent.control    = rt_dm9000_control;
    dm9000_device.parent.parent.user_data  = RT_NULL;

    dm9000_device.parent.eth_rx     = rt_dm9000_rx;
    dm9000_device.parent.eth_tx     = rt_dm9000_tx;

    eth_device_init(&(dm9000_device.parent), "e0");

    return 0;
}
INIT_DEVICE_EXPORT(rt_hw_dm9000_init);

void dm9000(void)
{
    rt_kprintf("\n");
    rt_kprintf("NCR   (0x00): %02x\n", dm9000_io_read(DM9000_NCR));
    rt_kprintf("NSR   (0x01): %02x\n", dm9000_io_read(DM9000_NSR));
    rt_kprintf("TCR   (0x02): %02x\n", dm9000_io_read(DM9000_TCR));
    rt_kprintf("TSRI  (0x03): %02x\n", dm9000_io_read(DM9000_TSR1));
    rt_kprintf("TSRII (0x04): %02x\n", dm9000_io_read(DM9000_TSR2));
    rt_kprintf("RCR   (0x05): %02x\n", dm9000_io_read(DM9000_RCR));
    rt_kprintf("RSR   (0x06): %02x\n", dm9000_io_read(DM9000_RSR));
    rt_kprintf("ORCR  (0x07): %02x\n", dm9000_io_read(DM9000_ROCR));
    rt_kprintf("CRR   (0x2C): %02x\n", dm9000_io_read(DM9000_CHIPR));
    rt_kprintf("CSCR  (0x31): %02x\n", dm9000_io_read(DM9000_CSCR));
    rt_kprintf("RCSSR (0x32): %02x\n", dm9000_io_read(DM9000_RCSSR));
    rt_kprintf("ISR   (0xFE): %02x\n", dm9000_io_read(DM9000_ISR));
    rt_kprintf("IMR   (0xFF): %02x\n", dm9000_io_read(DM9000_IMR));
    rt_kprintf("\n");
}

#ifdef RT_USING_FINSH
#include <finsh.h>
FINSH_FUNCTION_EXPORT(dm9000, dm9000 register dump);
#endif
