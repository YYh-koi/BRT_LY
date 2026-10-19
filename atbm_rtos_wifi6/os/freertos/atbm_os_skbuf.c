#include <stdlib.h>
#include "atbm_hal.h"
#include "atbm_os_skbuf.h"
#include "atbm_os_api.h"

#ifndef ATBM_FREERTOS_TCP_TX_DETAIL_TRACE
#define ATBM_FREERTOS_TCP_TX_DETAIL_TRACE 1
#endif

#if ATBM_FREERTOS_TCP_TX_DETAIL_TRACE
#define ATBM_TX_DETAIL_LOG(...) iot_printf("[ATBM_TX_DETAIL] " __VA_ARGS__)
#else
#define ATBM_TX_DETAIL_LOG(...) do { } while (0)
#endif

#if ATBM_TCPIP_STACK == ATBM_TCPIP_STACK_LWIP
#include "lwip/tcpip.h"
#include "lwip/netif.h"
#include "netif/etharp.h"
#include "inet.h"
#include "lwip/pbuf.h"
#include "lwip/inet.h"
#include "init.h"
#include "tcpip.h"
#include "lwip/opt.h"
#include "lwip/def.h"
#include "lwip/mem.h"
#include "lwip/sys.h"
#include "lwip/icmp.h"
#include "lwip/dhcp.h"
#include "lwip/ip_addr.h"

static const uint8_t ucIPAddress[4] = {192, 168, 13, 1};
static const uint8_t ucNetMask[4] = {255, 255, 255, 0};
static const uint8_t ucGatewayAddress[4] = {0, 0, 0, 0};
#define lwip_ipv4_addr(addr) ((addr[0]) | (addr[1] << 8) | (addr[2] << 16) | (addr[3] << 24))

err_t atbm_wifi_tx_pkt_netvif(struct netif *netif, struct pbuf *p);
#endif

extern struct atbmwifi_common g_hw_prv;

static atbm_tcpip_rx_callback_t g_tcpip_rx_callback;
static void *g_tcpip_rx_callback_arg;

atbm_void * netdev_drv_priv(struct atbm_net_device *ndev)
{
	return &ndev->drv_priv[0];

}


atbm_void atbm_skbbuffer_init()
{
}

#if ATBM_TCPIP_STACK == ATBM_TCPIP_STACK_LWIP
static err_t atbm_wifi_if_init(struct netif *netif)
{
	netif->output = etharp_output;
	netif->linkoutput = atbm_wifi_tx_pkt_netvif;
	netif->hwaddr_len = 6;
	/* maximum transfer unit */
	netif->mtu = 1500;
	/* device capabilities */
	/* don't set NETIF_FLAG_ETHARP if this device is not an ethernet one */
	netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP | NETIF_FLAG_IGMP;  //lkz debug NETIF_FLAG_IGMP FOR EC
	//iot_printf("[%s] %d\r\n",__FUNCTION__,__LINE__);
	return 0;
}

int atbm_wifi_netif_init(struct atbm_net_device *dev)
{
	struct atbmwifi_vif *priv = netdev_drv_priv(dev);
	struct netif *p_netif = dev->nif;
	/*atbm_uint32 ipaddr=0;
	atbm_uint32 netmask=0;
	atbm_uint32 gw=0;*/

	
	ip4_addr_t ip_addr;
	ip4_addr_t netmask;
	ip4_addr_t gw;

	ip_addr.addr = lwip_ipv4_addr(ucIPAddress);
	netmask.addr = lwip_ipv4_addr(ucNetMask);
	gw.addr 	 = lwip_ipv4_addr(ucGatewayAddress);

	sprintf(p_netif->name, "w%d", priv->if_id);

	if(netif_add(p_netif, &ip_addr, &netmask, &gw, priv,atbm_wifi_if_init, tcpip_input)==0)
    {
        iot_printf("wifi_netif_init netif_add err\n");
        return  - 1;
    }
	atbm_memcpy(p_netif->hwaddr,priv->mac_addr,6);
	iot_printf("if[%d] MAC addr :"MACSTR"\n",priv->if_id,MAC2STR(p_netif->hwaddr));
	return 0;
}

/**
 * @brief  wifi_netif_deinit, remove netif of wifi
 * @param  :
 * @retval void
 */
void atbm_wifi_netif_deinit(struct atbm_net_device *dev)
{
    ATBM_ASSERT(dev);
	struct netif *netif = dev->nif;
	if(netif){
        netif_remove(netif);
	}
}
#else
int atbm_wifi_netif_init(struct atbm_net_device *dev)
{
	(void)dev;
	return 0;
}

void atbm_wifi_netif_deinit(struct atbm_net_device *dev)
{
	(void)dev;
}
#endif


struct atbm_net_device * atbm_alloc_netdev(atbm_int32 size)
{
	struct atbm_net_device *  netdev = atbm_kmalloc(size + sizeof(struct atbm_net_device),GFP_KERNEL);

	ATBM_ASSERT((netdev != ATBM_NULL));
	if(netdev)
		atbm_memset(netdev,0,(size + sizeof(struct atbm_net_device)));

#if ATBM_TCPIP_STACK == ATBM_TCPIP_STACK_LWIP
	netdev->nif = atbm_kmalloc(sizeof(struct netif),GFP_KERNEL);
	ATBM_ASSERT(netdev->nif != ATBM_NULL);
	if(netdev->nif)
		atbm_memset(netdev->nif,0,sizeof(struct netif));
#else
	netdev->nif = ATBM_NULL;
#endif
	atbm_os_init_waitevent(&netdev->tx_enable);
	atbm_os_init_waitevent(&netdev->rx_enable);

	wifi_printk(WIFI_ALWAYS,"atbm_alloc_netdev,netdev(%p),nif(%p),priv(%p)\n",netdev,netdev->nif,&netdev->drv_priv[0]);
	return  netdev;
}

atbm_void atbm_free_netdev(struct atbm_net_device * netdev)
{
	if(netdev != ATBM_NULL){
        atbm_wifi_netif_deinit(netdev);
		atbm_os_delete_waitevent(&netdev->tx_enable);
		atbm_os_delete_waitevent(&netdev->rx_enable);
		atbm_kfree(netdev);
	}
}

#if ATBM_TCPIP_STACK == ATBM_TCPIP_STACK_LWIP
atbm_void atbm_lwip_init(struct atbm_net_device *dev)
{
}

atbm_void atbm_lwip_enable(struct atbm_net_device *dev)
{
	int queue_id;
	struct atbmwifi_vif *priv = netdev_drv_priv(dev);
	struct netif *netif = dev->nif;
	struct atbmwifi_common *hw_priv = _atbmwifi_vifpriv_to_hwpriv(priv);

	if (!dev->lwip_enable)
	{
		dev->lwip_enable = 1;
		dev->lwip_queue_enable = 1;
		atbm_os_wakeup_event(&dev->tx_enable);

		netif_set_default(netif);
		netif_set_link_up(netif);
		netif_set_up(netif);

		if (atbmwifi_is_sta_mode(priv->iftype))
		{
			iot_printf("dhcp start\r\n");
			dhcp_start(netif);
		}
		else
		{
			strcpy(dev->nif->name, "p0");
			iot_printf("dhcpd start\r\n");
			//dhcps_start(netif);
			printf("ap  dhcps_init 11\n");
			extern void netif_set_up(struct netif *netif);
			extern void netif_set_ipaddr(struct netif *netif, const ip4_addr_t *ipaddr);
			extern void netif_set_netmask(struct netif *netif, const ip4_addr_t *netmask);
			extern void netif_set_gw(struct netif *netif, const ip4_addr_t *gw);
#define inet_addr(cp)					ipaddr_addr(cp)
#if 1
			struct ip4_addr netmask, gw, ip_addr;

			//struct ip4_addr netmask, gw, ip_addr;
			wifi_printk(WIFI_ALWAYS,"init_udhcpd\n");	
			netif_set_up(dev->nif);
			ip_addr.addr = lwip_ipv4_addr(ucIPAddress);//inet_addr("192.168.13.1");
			netif_set_ipaddr(dev->nif, &ip_addr);
			netmask.addr = lwip_ipv4_addr(ucNetMask);//inet_addr("255.255.255.0");
			netif_set_netmask(dev->nif, &netmask);
			gw.addr = lwip_ipv4_addr(ucIPAddress);//inet_addr("192.168.13.1");
			netif_set_gw(dev->nif, &gw);
			netif_set_default(dev->nif);
			
#endif
			//init_udhcpd(dev->nif);
			//dhcp_start();
			 dhcps_init(dev->nif);

		}
		for (queue_id = ATBM_IEEE80211_AC_VO; queue_id <= ATBM_IEEE80211_AC_BK; queue_id++)
		{
			if (hw_priv->tx_queue[queue_id].overfull == ATBM_TRUE)
			{
				dev->lwip_queue_enable = 0;
			}
		}
	}
}

atbm_void atbm_lwip_disable(struct atbm_net_device *dev)
{
    struct netif *netif = dev->nif;
    struct atbmwifi_vif *priv = netdev_drv_priv(dev);

	if (dev->lwip_enable)
	{
		dev->lwip_enable = 0;
		dev->lwip_queue_enable = 0;
		if (atbmwifi_is_sta_mode(priv->iftype))
		{
			dhcp_stop(netif);
		}
		else
		{
			dhcps_deinit();
		}

		netif_set_down(netif);
		netif_set_link_down(netif);
	}
}


atbm_void atbm_lwip_txdone(struct atbm_net_device *dev)
{
}

atbm_void atbm_lwip_wake_queue(struct atbm_net_device *dev,int num)
{
	if(!dev->lwip_queue_enable && dev->lwip_enable){
		dev->lwip_queue_enable = 1;
		atbm_os_wakeup_event(&dev->tx_enable);
	}
}

atbm_void atbm_lwip_stop_queue(struct atbm_net_device *dev,int num)
{
	if(dev->lwip_queue_enable && dev->lwip_enable){
		dev->lwip_queue_enable = 0;
	}
}

atbm_void atbm_lwip_task_event(struct atbm_net_device *dev)
{
}

/****************************************************************************
* Function:   	atbm_wifi_tx_pkt_netvif
*
* Purpose:   	This function is used to send packet to wifi driver
*
* Parameters: point to buffer of packet
*
* Returns:	None.
******************************************************************************/
err_t atbm_wifi_tx_pkt_netvif(struct netif *netif, struct pbuf *p)
{
	struct pbuf *q = p;
	struct pbuf *temp_pbuf=ATBM_NULL;
	struct atbm_buff *AtbmBuf = ATBM_NULL;
	struct atbmwifi_vif *priv = (struct atbmwifi_vif *)(netif->state);
	struct atbmwifi_common * hw_priv = priv->hw_priv;
    //iot_printf("tx+++++\r\n");
	if(priv == ATBM_NULL)
	{
		return -1;
	}
	AtbmBuf = atbm_dev_alloc_skb(p->tot_len);
	if (!AtbmBuf)
	{
		ATBM_BUG_ON(1);
		wifi_printk(WIFI_TX,"<ERROR> tx_pkt alloc skb \n");
		return -1;
	}
	/*Here should copy pubf chain packet to atbmBuf*/
    for (temp_pbuf = p; temp_pbuf != NULL; temp_pbuf = temp_pbuf->next){
		atbm_memcpy(atbm_skb_put(AtbmBuf,temp_pbuf->len), temp_pbuf->payload, temp_pbuf->len);
		/*if q->len==q->tot_len,it means that it is the last packet*/
		if ( temp_pbuf->len == temp_pbuf->tot_len) {
			break;
		}
	}
#if 0
	atbm_uint8 *pdata;
	atbm_uint16 data_len;
        pdata = ATBM_OS_SKB_DATA(AtbmBuf);
        data_len = ATBM_OS_SKB_LEN(AtbmBuf);

        wifi_printk(WIFI_ALWAYS,"atbm: wifi TX, len=%d \n", data_len);

        dump_mem(pdata, data_len);
#endif
	if(priv->ndev && priv->ndev->netdev_ops){
        //iot_printf("tx>>>\r\n");
		priv->ndev->netdev_ops->ndo_start_xmit(priv, AtbmBuf);
	}else{
		ATBM_BUG_ON(1);
		atbm_dev_kfree_skb(AtbmBuf);
	}

	return 0;
}
//void  ethernetif_input(struct netif *netif, void *p_buf,int size);
static atbm_void __atbm_wifi_rx_pkt(struct atbm_net_device *dev, struct atbm_buff *atbm_skb)
{
	struct pbuf *p=ATBM_NULL;
	atbm_uint16 len = 0;
	atbm_uint8 *data=ATBM_NULL;
	ATBM_NETIF *netif = dev->nif;
	data = ATBM_OS_SKB_DATA(atbm_skb);
	/* Obtain the size of the packet and put it into the "len" variable. */
	len = ATBM_OS_SKB_LEN(atbm_skb);
    //iot_printf("rx++++\r\n");
	if(netif==NULL){
		goto RcvErr;
	}
	if (0 == len) {
		goto RcvErr;
	}
	//p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL);
	p = pbuf_alloc(PBUF_RAW, len, PBUF_RAM);
	if (p == ATBM_NULL){
		ATBM_BUG_ON(1);
		goto AllocErr;
	}
	pbuf_take(p,data,len);
	/* full packet send to tcpip_thread to process */
	if(netif->input){
        //iot_printf("rx<<<\r\n");
		if(netif->input(p,netif)!=ERR_OK)
			goto AllocErr;
	}else{
		goto AllocErr;
	}
	atbm_dev_kfree_skb(atbm_skb);
	return;
AllocErr:
	wifi_printk(WIFI_ALWAYS,"<ERROR> rx_pkt alloc pbuf \n");
	if(p){
//		wifi_printk(WIFI_ALWAYS,"Rx Why Here !!!\n");
		pbuf_free(p);
	}
RcvErr:
	atbm_dev_kfree_skb(atbm_skb);
	atbm_skb=ATBM_NULL;
	p=ATBM_NULL;
	return;
}
//not required here ,   lwip_tcp_opt.net_rx = ethernetif_input.
atbm_uint32 atbm_wifi_rx_pkt(struct atbm_net_device *dev, struct atbm_buff *at_skb)   
{
	struct eth_hdr *ethhdr;
	ethhdr = (struct eth_hdr *)ATBM_OS_SKB_DATA(at_skb);
	//printf("ethhdr type:%x\n",htons(ethhdr->type));
#if 0
	atbm_uint8 *pdata;
	atbm_uint16 data_len;
    	pdata = ATBM_OS_SKB_DATA(at_skb);
    	data_len = ATBM_OS_SKB_LEN(at_skb);

    	wifi_printk(WIFI_ALWAYS,"atbm: wifi RX, len=%d \n", data_len);

    	dump_mem(pdata, data_len);
#endif

	switch (htons(ethhdr->type)) {
	  /* IP or ARP packet? */
	  case ETHTYPE_IP:
	  case ETHTYPE_ARP:
      case ETHTYPE_IPV6:
	  case 0x888E:
#if PPPOE_SUPPORT
	  /* PPPoE packet? */
	  case ETHTYPE_PPPOEDISC:
	  case ETHTYPE_PPPOE:
#endif /* PPPOE_SUPPORT */
	    /* full packet send to tcpip_thread to process */
	    __atbm_wifi_rx_pkt(dev,at_skb);
	    break;
	  default:
	    atbm_dev_kfree_skb(at_skb);
	    break;
	}
	return 0;
}

struct tcpip_opt lwip_tcp_opt ={
	.net_init = atbm_lwip_init,
	.net_enable = atbm_lwip_enable,//
	.net_disable = atbm_lwip_disable,//
	.net_rx = atbm_wifi_rx_pkt,
	.net_tx_done =	atbm_lwip_txdone,
	.net_start_queue =	atbm_lwip_wake_queue,
	.net_stop_queue =	atbm_lwip_stop_queue,
	.net_task_event =	atbm_lwip_task_event,//
};
#else
static atbm_void atbm_freertos_tcp_init(struct atbm_net_device *dev)
{
	(void)dev;
}

static atbm_void atbm_freertos_tcp_enable(struct atbm_net_device *dev)
{
	if (!dev->lwip_enable) {
		dev->lwip_enable = 1;
		dev->lwip_queue_enable = 1;
		atbm_os_wakeup_event(&dev->tx_enable);
	}
}

static atbm_void atbm_freertos_tcp_disable(struct atbm_net_device *dev)
{
	dev->lwip_enable = 0;
	dev->lwip_queue_enable = 0;
}

static atbm_void atbm_freertos_tcp_txdone(struct atbm_net_device *dev)
{
	(void)dev;
}

static atbm_void atbm_freertos_tcp_wake_queue(struct atbm_net_device *dev,
		int num)
{
	(void)num;
	if (!dev->lwip_queue_enable && dev->lwip_enable) {
		dev->lwip_queue_enable = 1;
		atbm_os_wakeup_event(&dev->tx_enable);
	}
}

static atbm_void atbm_freertos_tcp_stop_queue(struct atbm_net_device *dev,
		int num)
{
	(void)num;
	if (dev->lwip_queue_enable && dev->lwip_enable) {
		dev->lwip_queue_enable = 0;
	}
}

static atbm_void atbm_freertos_tcp_task_event(struct atbm_net_device *dev)
{
	(void)dev;
}

static atbm_uint32 atbm_freertos_tcp_rx(struct atbm_net_device *dev,
		struct atbm_buff *skb)
{
	atbm_tcpip_rx_callback_t callback = g_tcpip_rx_callback;
	atbm_uint8 *data = ATBM_OS_SKB_DATA(skb);
	atbm_uint16 length = ATBM_OS_SKB_LEN(skb);
	int ret = -1;

	(void)dev;
	if (callback != ATBM_NULL && data != ATBM_NULL && length != 0) {
		ret = callback(data, (unsigned int)length, g_tcpip_rx_callback_arg);
	}
	if (ret != 0) {
		wifi_printk(WIFI_DBG_ERROR,
				"[ATBM_FREERTOS_TCP] drop RX frame, len=%u ret=%d\n",
				(unsigned int)length, ret);
	}
	atbm_dev_kfree_skb(skb);
	return 0;
}

struct tcpip_opt freertos_tcp_opt = {
	.net_init = atbm_freertos_tcp_init,
	.net_enable = atbm_freertos_tcp_enable,
	.net_disable = atbm_freertos_tcp_disable,
	.net_rx = atbm_freertos_tcp_rx,
	.net_tx_done = atbm_freertos_tcp_txdone,
	.net_start_queue = atbm_freertos_tcp_wake_queue,
	.net_stop_queue = atbm_freertos_tcp_stop_queue,
	.net_task_event = atbm_freertos_tcp_task_event,
};
#endif

int atbm_wifi_register_tcpip_rx_callback(atbm_tcpip_rx_callback_t callback,
		void *arg)
{
	if (callback == ATBM_NULL) {
		g_tcpip_rx_callback = ATBM_NULL;
		g_tcpip_rx_callback_arg = ATBM_NULL;
		return 0;
	}

	g_tcpip_rx_callback_arg = arg;
	g_tcpip_rx_callback = callback;
	return 0;
}

int atbm_wifi_is_sta_connected(void)
{
	struct atbmwifi_vif *priv;

	priv = _atbmwifi_hwpriv_to_vifpriv(&g_hw_prv, 0);
	if (priv == ATBM_NULL || !atbmwifi_is_sta_mode(priv->iftype)) {
		return 0;
	}
	return priv->connect_ok ? 1 : 0;
}

int atbm_wifi_send_ethernet_frame(const unsigned char *data,
		unsigned int length)
{
	struct atbmwifi_vif *priv;
	struct atbm_buff *skb;
	atbm_uint8 *copy_target;
	int xmit_ret;
#if ATBM_FREERTOS_TCP_TX_DETAIL_TRACE
	static unsigned int tx_detail_sequence;
	unsigned int call_id = ++tx_detail_sequence;
#endif

	ATBM_TX_DETAIL_LOG("call=%u stage=enter data=%p len=%u\r\n",
			call_id, data, length);

	if (data == ATBM_NULL || length == 0 || length > 0xffffU) {
		ATBM_TX_DETAIL_LOG(
				"call=%u stage=validate_fail data=%p len=%u ret=-1\r\n",
				call_id, data, length);
		return -1;
	}

	priv = _atbmwifi_hwpriv_to_vifpriv(&g_hw_prv, 0);
	ATBM_TX_DETAIL_LOG("call=%u stage=priv_lookup priv=%p\r\n",
			call_id, priv);
	if (priv == ATBM_NULL || priv->ndev == ATBM_NULL ||
		priv->ndev->netdev_ops == ATBM_NULL ||
		priv->ndev->netdev_ops->ndo_start_xmit == ATBM_NULL ||
		!priv->ndev->lwip_enable ||
		!priv->ndev->lwip_queue_enable ||
		!atbm_wifi_is_sta_connected()) {
		ATBM_TX_DETAIL_LOG("call=%u stage=state_fail ret=-1\r\n",
				call_id);
		return -1;
	}
	ATBM_TX_DETAIL_LOG(
			"call=%u stage=state_ok priv=%p ndev=%p ops=%p\r\n",
			call_id, priv, priv->ndev, priv->ndev->netdev_ops);

	ATBM_TX_DETAIL_LOG("call=%u stage=alloc_enter len=%u\r\n",
			call_id, length);
	skb = atbm_dev_alloc_skb((atbm_uint16)length);
	ATBM_TX_DETAIL_LOG("call=%u stage=alloc_exit skb=%p\r\n",
			call_id, skb);
	if (skb == ATBM_NULL) {
		ATBM_TX_DETAIL_LOG("call=%u stage=alloc_fail ret=-1\r\n",
				call_id);
		return -1;
	}

	ATBM_TX_DETAIL_LOG(
			"call=%u stage=put_enter skb=%p len=%u\r\n",
			call_id, skb, length);
	copy_target = atbm_skb_put(skb, (atbm_uint16)length);
	ATBM_TX_DETAIL_LOG("call=%u stage=put_exit dst=%p\r\n",
			call_id, copy_target);

	ATBM_TX_DETAIL_LOG(
			"call=%u stage=copy_enter dst=%p src=%p len=%u\r\n",
			call_id, copy_target, data, length);
	atbm_memcpy(copy_target, data, length);
	ATBM_TX_DETAIL_LOG("call=%u stage=copy_exit skb=%p\r\n",
			call_id, skb);

	ATBM_TX_DETAIL_LOG(
			"call=%u stage=xmit_enter priv=%p skb=%p\r\n",
			call_id, priv, skb);
	xmit_ret = priv->ndev->netdev_ops->ndo_start_xmit(priv, skb);
	ATBM_TX_DETAIL_LOG("call=%u stage=xmit_exit ret=%d\r\n",
			call_id, xmit_ret);
	if (xmit_ret != 0) {
		atbm_dev_kfree_skb(skb);
		ATBM_TX_DETAIL_LOG("call=%u stage=return ret=-1\r\n",
				call_id);
		return -1;
	}
	ATBM_TX_DETAIL_LOG("call=%u stage=return ret=0\r\n", call_id);
	return 0;
}
