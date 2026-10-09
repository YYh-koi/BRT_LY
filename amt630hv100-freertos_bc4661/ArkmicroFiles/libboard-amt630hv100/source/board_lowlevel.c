#include "FreeRTOS.h"
#include "board.h"
#include "sysinfo.h"
#include "chip.h"

static const char* abort_status[][2]=
{
  // IFSR status        ,       DFSR status
  {"Unknown(reserved status)",                          "Unknown(reserved status)"                      },//0
  {"Unknown(reserved status)",                          "Alignment Fault"                               },//1
  {"Debug Event",                                       "Debug Event"                                   },//2
  {"Access flag - section",                             "Access flag - section"                         },//3
  {"Unknown(reserved status)",                          "Instruction cache maintenance"                 },//4
  {"Translation fault - section",                       "Translation fault - section"                   },//5
  {"Access flag - Page",                                "Access flag - Page"                            },//6
  {"Translation fault -Page",                           "Translation fault -Page"                       },//7
  {"Synchronous external abort",                        "Synchronous external abort, nontranslation"    },//8
  {"Domain fault - Section",                            "Domain fault - Section"                        },//9
  {"Unknown(reserved status)",                          "Unknown(reserved status)"                      },//10
  {"Domain fault - Page",                               "Domain fault - Page"                           },//11
  {"Synchronous external abort - L1 Translation",       "Synchronous external abort - L1 Translation"   },//12
  {"Permission fault - Section",                        "Permission fault - Section"                    },//13
  {"Synchronous external abort - L2 Translation",       "Synchronous external abort - L2 Translation"   },//14
  {"Permission fault - Page",                           "Permission fault - Page"                       },//15
  {"Unknown(reserved status)",                          "Unknown(reserved status)"                      },//16
  {"Unknown(reserved status)",                          "Unknown(reserved status)"                      },//17
  {"Unknown(reserved status)",                          "Unknown(reserved status)"                      },//18
  {"Unknown(reserved status)",                          "Unknown(reserved status)"                      },//19
  {"Unknown(reserved status)",                          "Unknown(reserved status)"                      },//20
  {"Unknown(reserved status)",                          "Unknown(reserved status)"                      },//21
  {"Unknown(reserved status)",                          "Asynchronous external abort"}

};

/* 数据中止现场保存：全局变量不会被优化丢弃，调试器一定能读到 */
volatile unsigned int g_abort_v1     = 0;   /* DFSR  数据故障状态 */
volatile unsigned int g_abort_v2     = 0;   /* DFAR  数据故障地址（最关键） */
volatile unsigned int g_abort_lr     = 0;   /* 故障指令地址(lr)+8 */
volatile unsigned int g_abort_dfsr   = 0;   /* 解码后的 fault reason */
volatile unsigned int g_abort_caller = 0;   /* 被中断模式的 LR（strncmp 调用者返回地址） */
volatile unsigned int g_abort_sp     = 0;   /* 被中断模式的 SP */
volatile unsigned int g_abort_spsr   = 0;   /* 被中断模式的 CPSR（低5位=模式） */
volatile unsigned int g_abort_r0     = 0;   /* 崩溃点 r0（strncmp 的 s1） */
volatile unsigned int g_abort_r1     = 0;   /* 崩溃点 r1（strncmp 的 s2） */
volatile unsigned int g_abort_r2     = 0;   /* 崩溃点 r2（strncmp 的 len） */
volatile unsigned int g_abort_r3     = 0;   /* 崩溃点 r3 */
volatile unsigned int g_abort_stack[16] = {0}; /* 被中断模式栈现场（前 16 字） */

void LowLevelInit( void )
{
}

void Abort_C_Handler( void)
{
    uint32_t v1, v2;
    uint32_t  lr;
    uint32_t  dfsr;

    /* 1. 第一时间保存崩溃点通用寄存器 r0-r3。
          通用寄存器跨模式共享，进入 Abort 时即崩溃瞬间的值。
          必须放在任何 C 变量初始化之前——否则 IAR 会用 r0-r3 做临时寄存器，
          把崩溃现场覆写掉（之前 r0 读成 0 就是这个原因）。
          r0/r1 即 strncmp(s1, s2, len) 的两个指针。 */
    asm volatile("mov  %0, r0" : "=r"(g_abort_r0));
    asm volatile("mov  %0, r1" : "=r"(g_abort_r1));
    asm volatile("mov  %0, r2" : "=r"(g_abort_r2));
    asm volatile("mov  %0, r3" : "=r"(g_abort_r3));

    /* 2. 读取 CPU 状态寄存器 */
    asm volatile("mov  %0, lr"              : "=r"(lr));
    asm volatile("mrc  p15, 0, %0, c5, c0, 0" : "=r"(v1));   /* DFSR */
    asm volatile("mrc  p15, 0, %0, c6, c0, 0" : "=r"(v2));   /* DFAR */

    /* 2. 立即存入全局变量：串口/printf 此时可能还没初始化，
          只有 Memory 窗口能可靠读到这些值 */
    g_abort_lr   = lr;
    g_abort_v1   = v1;
    g_abort_v2   = v2;
    dfsr         = (((v1 & 0x400) >> 6) | (v1 & 0x0F));
    g_abort_dfsr = dfsr;

    /* 2.5 读取被中断模式(SVC/SYS/FIQ/IRQ)的 SP/R14，用于定位 strncmp 的调用者。
          原理：Data Abort 进入 Abort 模式，被中断模式的 banked sp/lr 需切模式才能读取。
          USR(0x10) 与 SYS(0x1F) 共享 sp/lr，故把 USR 映射到 SYS。 */
    {
        uint32_t sp_val = 0, caller = 0, spsr = 0;
        asm volatile(
            "mrs  %2, spsr\n"
            "and  r1, %2, #0x1f\n"
            "cmp  r1, #0x10\n"
            "moveq r1, #0x1f\n"
            "mrs  r0, cpsr\n"
            "bic  r2, r0, #0x1f\n"
            "orr  r2, r2, r1\n"
            "msr  cpsr_c, r2\n"
            "mov  r3, sp\n"
            "mov  r4, lr\n"
            "msr  cpsr_c, r0\n"
            "mov  %0, r3\n"
            "mov  %1, r4\n"
            : "=r"(sp_val), "=r"(caller), "=r"(spsr)
            :
            : "r0", "r1", "r2", "r3", "r4", "cc", "memory");

        g_abort_sp     = sp_val;
        g_abort_caller = caller;
        g_abort_spsr   = spsr;

        /* 顺带抓一段被中断模式栈顶内容，若栈可读则含返回地址链 */
        if (sp_val && (sp_val >= 0x20000000u) && (sp_val < 0x24000000u))
        {
            uint32_t i;
            volatile unsigned int* p = (volatile unsigned int*)sp_val;
            for (i = 0; i < 16u; i++)
                g_abort_stack[i] = p[i];
        }
    }

    /* 3. 数组越界保护：dfsr 最大 31，而 abort_status 只有 22 项(0~21) */
    if (dfsr > 21) dfsr = 0;

    /* 4. 之后再尝试打印（串口未起则无输出，不影响上面的现场保存） */
    printf("abort lr=0x%x.\n", lr);
    printf("abort caller=0x%x r0=0x%x r1=0x%x r2=0x%x sp=0x%x spsr=0x%x\n",
        g_abort_caller, g_abort_r0, g_abort_r1, g_abort_r2, g_abort_sp, g_abort_spsr);
    printf("\n\r######################################################################\n\r");
    printf("Data Abort occured in %x domain\n\r", (unsigned int)dfsr);
    printf("Data abort fault reason is: %s\n\r", (char*)abort_status[dfsr][1]);
    printf("Data fault occured at Address = 0x%08x\n\n\r",(unsigned int)v2);
    printf("-[Info]-Data fault status register value = 0x%x\n\r",(unsigned int)v1);

    while(1);

}

void Prefetch_C_Handler( void)
{
    uint32_t v1,v2, ifsr;
    v1= 0;
    v2= 0;

    asm("mrc   p15, 0, %0, c5, c0, 1" : : "r"(v1));
    asm("mrc   p15, 0, %0, c6, c0, 2" : : "r"(v2));

    ifsr = (((v1 & 0x400) >> 6) | (v1 & 0x0F));
    printf("\n\r######################################################################\n\r");
    printf("Instruction prefetch abort reason is: %s\n\r", (char*)abort_status[ifsr][0]);
    printf("Instruction prefetch Fault occured at Address = 0x%08x\n\n\r",(unsigned int)v2);

    printf("-[INFO]- Prefetch Fault status register value by = 0x%x\n\r",(unsigned int)v1);

    while(1);

}

void Undefined_C_Handler( void)
{
  printf("Undefined abort \n\r");
  while(1);
}

typedef enum {
	VBUF_STATUS_FREE = 0,
	VBUF_STATUS_USED,
	VBUF_STATUS_RENDERED,
} VIDEO_BUFFER_STATUS;

typedef struct {
	uint32_t addr;
	int status;
}VideoBufInfo;

static SemaphoreHandle_t vdisbuf_mutex = NULL;
static VideoBufInfo vdisbufs[VIDEO_DISPLAY_BUF_NUM] = {0};

void VideoDisplayBufInit(void)
{
	int i;

	vdisbuf_mutex = xSemaphoreCreateRecursiveMutex();

	for (i = 0; i < VIDEO_DISPLAY_BUF_NUM; i++) {
		vdisbufs[i].addr = (uint32_t)pvPortMalloc(VIDEO_DISPLAY_WIDTH * VIDEO_DISPLAY_HEIGHT * 2);
		vdisbufs[i].status = VBUF_STATUS_FREE;
	}
}

int xVideoDisplayBufTake(uint32_t xTicksToWait)
{
	return xSemaphoreTakeRecursive(vdisbuf_mutex, xTicksToWait);
}

void vVideoDisplayBufGive(void)
{
	xSemaphoreGiveRecursive(vdisbuf_mutex);
}

uint32_t ulVideoDisplayBufGet(void)
{
	uint32_t bufaddr = 0;
	int i;
	
	xSemaphoreTakeRecursive(vdisbuf_mutex, portMAX_DELAY);

	for (i = 0; i < VIDEO_DISPLAY_BUF_NUM; i++) {
		if (vdisbufs[i].status == VBUF_STATUS_FREE) {
			bufaddr = vdisbufs[i].addr;
			vdisbufs[i].status = VBUF_STATUS_USED;
			break;
		}
	}

	xSemaphoreGiveRecursive(vdisbuf_mutex);
	
	configASSERT(bufaddr != 0);

	return bufaddr;
}

uint32_t ulVideoDisplayBufGetSize(void)
{
	return VIDEO_DISPLAY_WIDTH * VIDEO_DISPLAY_HEIGHT * 2;
}

void vVideoDisplayBufRender(uint32_t buf_addr)
{
	int i, j;

	xSemaphoreTakeRecursive(vdisbuf_mutex, portMAX_DELAY);
	
	for (i = 0; i < VIDEO_DISPLAY_BUF_NUM; i++) {
		if (vdisbufs[i].addr == buf_addr) {
			for (j = 0; j < VIDEO_DISPLAY_BUF_NUM; j++) {
				if (vdisbufs[j].status == VBUF_STATUS_RENDERED)
					vdisbufs[j].status = VBUF_STATUS_FREE;
			}
			vdisbufs[i].status = VBUF_STATUS_RENDERED;
			break;
		}
	}
	
	xSemaphoreGiveRecursive(vdisbuf_mutex);
}

void vVideoDisplayBufFree(uint32_t buf_addr)
{
	int i;

	xSemaphoreTakeRecursive(vdisbuf_mutex, portMAX_DELAY);

	for (i = 0; i < VIDEO_DISPLAY_BUF_NUM; i++) {
		if (vdisbufs[i].addr == buf_addr) {
			vdisbufs[i].status = VBUF_STATUS_FREE;
			break;
		}
	}

	xSemaphoreGiveRecursive(vdisbuf_mutex);
}

unsigned int ulVideoDisplayGetBufferAddr(int index)
{
	configASSERT(index < VIDEO_DISPLAY_BUF_NUM);
	return vdisbufs[index].addr;
}
