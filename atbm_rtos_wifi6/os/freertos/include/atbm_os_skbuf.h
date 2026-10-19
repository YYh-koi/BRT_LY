#ifndef ATBM_OS_SKBUFF_H
#define ATBM_OS_SKBUFF_H

#include "atbm_sysops.h"

#ifndef ATBM_TCPIP_STACK_LWIP
#define ATBM_TCPIP_STACK_LWIP          1
#endif
#ifndef ATBM_TCPIP_STACK_FREERTOS_TCP
#define ATBM_TCPIP_STACK_FREERTOS_TCP  2
#endif
#ifndef ATBM_TCPIP_STACK
#define ATBM_TCPIP_STACK ATBM_TCPIP_STACK_LWIP
#endif

#ifndef ATBM_NETIF_TYPE_DEFINED
#define ATBM_NETIF_TYPE_DEFINED
	#if ATBM_TCPIP_STACK == ATBM_TCPIP_STACK_LWIP
	typedef struct netif ATBM_NETIF;
	#else
	typedef void ATBM_NETIF;
	#endif
#endif

struct atbm_net_device * atbm_alloc_netdev(atbm_int32 size);
atbm_void atbm_skbbuffer_init(void);

#endif /* ATBM_OS_SKBUFF_H */
