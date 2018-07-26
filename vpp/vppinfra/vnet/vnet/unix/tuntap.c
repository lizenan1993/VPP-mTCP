/* 
 *------------------------------------------------------------------
 * tuntap.c - kernel stack (reverse) punt/inject path
 *
 * Copyright (c) 2009 Cisco and/or its affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *------------------------------------------------------------------
 */

#include <fcntl.h>		/* for open */
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h> 
#include <sys/uio.h>		/* for iovec */
#include <netinet/in.h>
#include <pthread.h>

#include <linux/if_arp.h>
#include <linux/if_tun.h>

#include <vlib/vlib.h>
#include <vlib/unix/unix.h>
#include <vlib/buffer.h>
#include <rte_ring.h>
#include <rte_mbuf.h>

#include <vnet/ip/ip.h>

#include <vnet/ethernet/ethernet.h>

#if DPDK == 1
#include <vnet/devices/dpdk/dpdk.h>
#endif

#include "tuntap.h"
#include <rte_ring.h>
#include <mtcp.h>
#include <time.h>

volatile int flag1 = 0;
int vec_count;
word n_bytes_left;
uword i_rx;
tuntap_main_t * tm;
pthread_mutex_t mylock;
vlib_main_t * mTCP_vm;
uint8_t * write_buf;
int write_len;
uword n_packets;
struct rte_ring * mtcp_recv_ring;
struct rte_ring * mtcp_send_ring;
clock_t finish, start;

#define rte_mbuf_from_vlib_buffer(x) (((struct rte_mbuf *)x) - 1)
#define vlib_buffer_from_rte_mbuf(x) ((vlib_buffer_t *)(x + 1))
#define vtom(x) ((struct rte_mbuf *)(x))

static vnet_device_class_t tuntap_dev_class;
static vnet_hw_interface_class_t tuntap_interface_class;

static void tuntap_punt_frame (vlib_main_t * vm,
                               vlib_node_runtime_t * node,
                               vlib_frame_t * frame);
static void tuntap_nopunt_frame (vlib_main_t * vm,
                                 vlib_node_runtime_t * node,
                                 vlib_frame_t * frame);

/* 
 * This driver runs in one of two distinct modes:
 * "punt/inject" mode, where we send pkts not otherwise processed
 * by the forwarding to the Linux kernel stack, and
 * "normal interface" mode, where we treat the Linux kernel stack
 * as a peer.
 *
 * By default, we select punt/inject mode.
 */

/*
typedef struct {
  u32 sw_if_index;
  u8 is_v6;
  u8 addr[16];
} subif_address_t;
*/

//typedef struct {
  /* Vector of iovecs for readv/writev calls. */
//  struct iovec * iovecs;

  /* Vector of VLIB rx buffers to use.  We allocate them in blocks
     of VLIB_FRAME_SIZE (256). */
//  u32 * rx_buffers;

  /* File descriptors for /dev/net/tun and provisioning socket. */
//  int dev_net_tun_fd, dev_tap_fd;

  /* Create a "tap" [ethernet] encaps device */
//  int is_ether;

  /* 1 if a "normal" routed intfc, 0 if a punt/inject interface */

//  int have_normal_interface;

  /* tap device destination MAC address. Required, or Linux drops pkts */
//  u8 ether_dst_mac[6];

  /* Interface MTU in bytes and # of default sized buffers. */
//  u32 mtu_bytes, mtu_buffers;

  /* Linux interface name for tun device. */
//  char * tun_name;

  /* Pool of subinterface addresses */
//  subif_address_t *subifs;

  /* Hash for subif addresses */
//  mhash_t subif_mhash;

//  u32 unix_file_index;

  /* For the "normal" interface, if configured */
//  u32 hw_if_index, sw_if_index;

//} tuntap_main_t;


static tuntap_main_t tuntap_main = {
  .tun_name = "vnet",

  /* Suitable defaults for an Ethernet-like tun/tap device */
  .mtu_bytes = 4096 + 256,
};

/*
 * tuntap_tx
 * Output node, writes the buffers comprising the incoming frame 
 * to the tun/tap device, aka hands them to the Linux kernel stack.
 * 
 * to the tun/tap device, aka hands them to the Linux kernel stack.
 * 
 */
static uword
tuntap_tx (vlib_main_t * vm,
	   vlib_node_runtime_t * node,
	   vlib_frame_t * frame)
{
  u32 * buffers = vlib_frame_args (frame);
  n_packets = frame->n_vectors;
  tm = &tuntap_main;
  u32 n_bytes = 0;
  int i;
  struct rte_mbuf * mb;
  for (i = 0; i < n_packets; i++)
    {
      vlib_buffer_t * b;
      uword l;

      b = vlib_get_buffer (vm, buffers[i]);

      if (tm->is_ether && (!tm->have_normal_interface))
        {
          vlib_buffer_reset(b);
          clib_memcpy (vlib_buffer_get_current (b), tm->ether_dst_mac, 6);
        }

      /* Re-set iovecs if present. */
      //if (tm->iovecs)
	//_vec_len (tm->iovecs) = 0;
	
      /* VLIB buffer chain -> Unix iovec(s). */
      //vec_add2 (tm->iovecs, iov, 1);
      //for(int i = 0; i < l; i++){
	//printf("%02x", *(b->data+b->current_data + i));
      //}
      //printf("\n");
      
      mb = rte_mbuf_from_vlib_buffer(b);
      mb->data_len = b->current_length;
      mb->pkt_len = b->current_length;
      mb->data_off = RTE_PKTMBUF_HEADROOM + b->current_data;
      mb->next = 0;
      mb->nb_segs = 1;
      mb->buf_addr = b->data + b->current_data + mb->data_off;
      rte_ring_enqueue(mtcp_recv_ring, mb);
   
      if (PREDICT_FALSE (b->flags & VLIB_BUFFER_NEXT_PRESENT))
	{
	  do {
	    b = vlib_get_buffer (vm, b->next_buffer);
	    //vec_add2 (tm->iovecs, write_iov[i], 1);
	//now we cannot deal with big packets
	  } while (b->flags & VLIB_BUFFER_NEXT_PRESENT);
	}
	n_bytes += 1;
    }
  /* The normal interface path flattens the buffer chain */
  if (tm->have_normal_interface)
    vlib_buffer_free_no_next (vm, buffers, n_packets);
  else
    vlib_buffer_free (vm, buffers, n_packets);
  return n_packets;
}

VLIB_REGISTER_NODE (tuntap_tx_node,static) = {
  .function = tuntap_tx,
  .name = "tuntap-tx",
  .type = VLIB_NODE_TYPE_INTERNAL,
  .vector_size = 4,
};

enum {
  TUNTAP_RX_NEXT_IP4_INPUT, 
  TUNTAP_RX_NEXT_IP6_INPUT, 
  TUNTAP_RX_NEXT_ETHERNET_INPUT,
  TUNTAP_RX_NEXT_DROP,
  TUNTAP_RX_N_NEXT,
};

//static uword
uword
tuntap_rx (vlib_main_t * vm,
	   vlib_node_runtime_t * node,
	   vlib_frame_t * frame)
{
  //flag1 = 0;
  mTCP_vm = vm;
  tm = &tuntap_main;
  pthread_mutex_init(&mylock, NULL);
  vlib_buffer_t * b;
  u32 bi;
  vec_count = 0;
  const uword buffer_size = VLIB_BUFFER_DATA_SIZE;
#if DPDK == 0
  u32 free_list_index = VLIB_BUFFER_DEFAULT_FREE_LIST_INDEX;
#else
  dpdk_main_t * dm = &dpdk_main;
  u32 free_list_index = dm->vlib_buffer_free_list_index;
#endif

  /* Make sure we have some RX buffers. */

  {
    uword n_left = vec_len (tm->rx_buffers);
    uword n_alloc;

    if (n_left < VLIB_FRAME_SIZE / 2)
      {
	if (! tm->rx_buffers)
	  vec_alloc (tm->rx_buffers, VLIB_FRAME_SIZE);

	n_alloc = vlib_buffer_alloc_from_free_list 
            (vm, tm->rx_buffers + n_left, VLIB_FRAME_SIZE - n_left, 
             free_list_index);
	_vec_len (tm->rx_buffers) = n_left + n_alloc;
      }
  }

  /* Allocate RX buffers from end of rx_buffers.
     Turn them into iovecs to pass to readv. */

    uword i_rx = vec_len (tm->rx_buffers) - 1;

    word i, n_bytes_in_packet;
    if (!tm->iovecs)
	tm->iovecs = malloc(10240);
    /* We should have enough buffers left for an MTU sized packet. */
    ASSERT (vec_len (tm->rx_buffers) >= tm->mtu_buffers);
    tm->mtu_buffers = 32;
    //vec_validate (tm->iovecs, tm->mtu_buffers - 1);

      flag1 = 1;
    void * packet;
    int packet_num = 0;
    while (!rte_ring_dequeue(mtcp_send_ring, &packet)){
	packet_num ++;
	b = vlib_get_buffer(vm, tm->rx_buffers[i_rx]);
	vlib_buffer_t * temp = vlib_buffer_from_rte_mbuf(vtom(packet));
	uint8_t * con = rte_pktmbuf_mtod(vtom(packet), uint8_t *);
	//for(int i = 0; i < vtom(packet)->data_len; i++){
	//	printf("%02x", con[i]);
	//}
	//printf("\n");
	memcpy(b, temp, sizeof(vlib_buffer_t));
	b->current_data = rte_pktmbuf_mtod(vtom(packet), uint8_t *) - temp->data;
	uint8_t * check = rte_pktmbuf_mtod(vtom(packet), uint8_t *);
	b->current_length = vtom(packet)->data_len;
	n_bytes_left = b->current_length;
    	n_bytes_in_packet = n_bytes_left; 
    	bi = tm->rx_buffers[i_rx];
    	while (1)
      	{
#if DPDK == 1
        	struct rte_mbuf * mb;
#endif
	//b = vlib_get_buffer (vm, tm->rx_buffers[i_rx]);
#if DPDK == 1
		mb = rte_mbuf_from_vlib_buffer(b);
#endif
		n_bytes_left -= buffer_size;
#if DPDK == 1
        	rte_pktmbuf_data_len (mb) = b->current_length;
#endif

		if (n_bytes_left <= 0)
          	{
#if DPDK == 1
            		rte_pktmbuf_pkt_len (mb) = n_bytes_in_packet;
#endif
            		break;
          	}
		i_rx--;
		b->flags |= VLIB_BUFFER_NEXT_PRESENT;
		b->next_buffer = tm->rx_buffers[i_rx];
#if DPDK == 1
        	ASSERT(0);
#endif
      	}
    /* Interface counters for tuntap interface. */
    	vlib_increment_combined_counter 
        	(vnet_main.interface_main.combined_sw_if_counters
         	+ VNET_INTERFACE_COUNTER_RX,
         	os_get_cpu_number(),
        	tm->sw_if_index,
         	1, n_bytes_in_packet);
    
    	_vec_len (tm->rx_buffers) = i_rx;

  //b = vlib_get_buffer (vm, bi);

  {
    	u32 next_index;
    	uword n_trace = vlib_get_trace_count (vm, node);

    	vnet_buffer (b)->sw_if_index[VLIB_RX] = tm->sw_if_index;
    	vnet_buffer (b)->sw_if_index[VLIB_TX] = (u32)~0;

    /*
     * Turn this on if you run iinto
     * "bad monkey" contexts, and you want to know exactly
     * which nodes they've visited...
     */
    	if (VLIB_BUFFER_TRACE_TRAJECTORY)
        	b->pre_data[0] = 0;

   	b->error = node->errors[0];
    	if (tm->is_ether)
      	{
		next_index = TUNTAP_RX_NEXT_ETHERNET_INPUT;
      	}
    	else
      	switch (b->data[14] & 0xf0)
        {
        case 0x40:
          next_index = TUNTAP_RX_NEXT_IP4_INPUT;
	  printf("ip4 packets\n");
	  b->current_data = 14;
          break;
        case 0x60:
          next_index = TUNTAP_RX_NEXT_IP6_INPUT;
	  printf("ip6 packets\n");
          break;
        default:
          next_index = TUNTAP_RX_NEXT_DROP;
	  printf("drop packets\n");
          break;
        }

    /* The linux kernel couldn't care less if our interface is up */
/*    if (tm->have_normal_interface)
      {
        vnet_main_t *vnm = vnet_get_main();
        vnet_sw_interface_t * si;
        si = vnet_get_sw_interface (vnm, tm->sw_if_index);
        if (!(si->flags & VNET_SW_INTERFACE_FLAG_ADMIN_UP))
          next_index = TUNTAP_RX_NEXT_DROP;
      }
*/
    	vlib_set_next_frame_buffer (vm, node, next_index, bi);

    	if (n_trace > 0)
      	{
        	vlib_trace_buffer (vm, node, next_index,
                           b, /* follow_chain */ 1);
        	vlib_set_trace_count (vm, node, n_trace - 1);
      	}
	i_rx--;
  }
 } 

  return 1;
}

static char * tuntap_rx_error_strings[] = {
  "unknown packet type",
};

VLIB_REGISTER_NODE (tuntap_rx_node,static) = {
  .function = tuntap_rx,
  .name = "tuntap-rx",
  .type = VLIB_NODE_TYPE_INPUT,
  .state = VLIB_NODE_STATE_POLLING,
  .vector_size = 4,
  .n_errors = 1,
  .error_strings = tuntap_rx_error_strings,

  .n_next_nodes = TUNTAP_RX_N_NEXT,
  .next_nodes = {
    [TUNTAP_RX_NEXT_IP4_INPUT] = "ip4-input-no-checksum",
    [TUNTAP_RX_NEXT_IP6_INPUT] = "ip6-input",
    [TUNTAP_RX_NEXT_DROP] = "error-drop",
    [TUNTAP_RX_NEXT_ETHERNET_INPUT] = "ethernet-input",
  },
};

/* Gets called when file descriptor is ready from epoll. */
static clib_error_t * tuntap_read_ready (unix_file_t * uf)
{
  vlib_main_t * vm = vlib_get_main();
  vlib_node_set_interrupt_pending (vm, tuntap_rx_node.index);
  return 0;
}

/*
 * tuntap_exit
 * Clean up the tun/tap device
 */

static clib_error_t *
tuntap_exit (vlib_main_t * vm)
{
  tuntap_main_t *tm = &tuntap_main;
  struct ifreq ifr;
  int sfd;

  /* Not present. */
  if (! tm->dev_net_tun_fd || tm->dev_net_tun_fd < 0)
    return 0;

  sfd = socket (AF_INET, SOCK_STREAM, 0);
  if (sfd < 0)
    clib_unix_warning("provisioning socket");

  memset(&ifr, 0, sizeof (ifr));
  strncpy (ifr.ifr_name, tm->tun_name, sizeof (ifr.ifr_name)-1);

  /* get flags, modify to bring down interface... */
  if (ioctl (sfd, SIOCGIFFLAGS, &ifr) < 0)
    clib_unix_warning ("SIOCGIFFLAGS");

  ifr.ifr_flags &= ~(IFF_UP | IFF_RUNNING);

  if (ioctl (sfd, SIOCSIFFLAGS, &ifr) < 0)
    clib_unix_warning ("SIOCSIFFLAGS");

  /* Turn off persistence */
  if (ioctl (tm->dev_net_tun_fd, TUNSETPERSIST, 0) < 0)
    clib_unix_warning ("TUNSETPERSIST");
  close(tm->dev_tap_fd);
  close(tm->dev_net_tun_fd);
  close (sfd);

  return 0;
}

VLIB_MAIN_LOOP_EXIT_FUNCTION (tuntap_exit);

u8 ** mac_addr;
extern struct mtcp_config CONFIG;

static clib_error_t * mtcp_config(vlib_main_t * vm){
  mtcp_recv_ring = rte_ring_create("vpp_to_mtcp", 64, rte_socket_id(), 0);
  mtcp_send_ring = rte_ring_create("mtcp_to_vpp", 64, rte_socket_id(), 0);
  clib_error_t * error = 0;
  tuntap_main_t * tm = &tuntap_main;
  u8 * name;
  int is_enabled = 1, is_ether =1 , have_normal_interface = 1;
  mac_addr[0] = CONFIG.eths[0].haddr;
  const uword buffer_size = VLIB_BUFFER_DATA_SIZE;
  tm->is_ether = is_ether;
  tm->have_normal_interface = have_normal_interface;
  int flags = IFF_TAP | IFF_NO_PI;
  if(have_normal_interface){
	vnet_main_t * vnm = vnet_get_main();
	error = ethernet_register_interface(vnm, tuntap_dev_class.index, 0, mac_addr[0], &tm->hw_if_index, 0);
	if(error)
		clib_error_report(error);
	tm->sw_if_index = tm->hw_if_index;
	vm->os_punt_frame = tuntap_nopunt_frame;
	//fix the IP address, later will change it
	ip4_address_t * ip4_address = malloc(4);
	ip4_address->data[0] = 192;
	ip4_address->data[1] = 168;
	ip4_address->data[2] = 12;
	ip4_address->data[3] = 1;
	error = ip4_add_del_interface_address(vm, tm->sw_if_index, ip4_address, 24, 0);
	if(error)
		clib_error_report(error);
	error = vnet_sw_interface_set_flags (vnm, tm->sw_if_index, 1);
	error = vnet_hw_interface_set_flags (vnm, tm->hw_if_index, 1);
	if(error)
		clib_error_report(error);
  }
  return error;
}


static clib_error_t *
tuntap_config (vlib_main_t * vm, unformat_input_t * input)
{
  tuntap_main_t *tm = &tuntap_main;
  clib_error_t * error = 0;
  struct ifreq ifr;
  u8 * name;
  int flags = IFF_TUN | IFF_NO_PI;
  int is_enabled = 0, is_ether = 0, have_normal_interface = 0;
  const uword buffer_size = VLIB_BUFFER_DATA_SIZE;

  while (unformat_check_input (input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (input, "mtu %d", &tm->mtu_bytes))
	;
      else if (unformat (input, "enable"))
        is_enabled = 1;
      else if (unformat (input, "disable"))
        is_enabled = 0;
      else if (unformat (input, "ethernet") ||
               unformat (input, "ether"))
        is_ether = 1;
      else if (unformat (input, "have-normal-interface") ||
               unformat (input, "have-normal"))
        have_normal_interface = 1;
      else if (unformat (input, "name %s", &name))
	tm->tun_name = (char *) name;
      else
	return clib_error_return (0, "unknown input `%U'",
				  format_unformat_error, input);
    }

  tm->dev_net_tun_fd = -1;
  tm->dev_tap_fd = -1;

  if (is_enabled == 0)
    return 0;

  if (geteuid()) 
    {
      clib_warning ("tuntap disabled: must be superuser");
      return 0;
    }    

  tm->is_ether = is_ether;
  tm->have_normal_interface = have_normal_interface;

  if (is_ether)
    flags = IFF_TAP | IFF_NO_PI;

  if ((tm->dev_net_tun_fd = open ("/dev/net/tun", O_RDWR)) < 0)
    {
      error = clib_error_return_unix (0, "open /dev/net/tun");
      goto done;
    }

  memset (&ifr, 0, sizeof (ifr));
  strncpy(ifr.ifr_name, tm->tun_name, sizeof(ifr.ifr_name)-1);
  ifr.ifr_flags = flags;
  if (ioctl (tm->dev_net_tun_fd, TUNSETIFF, (void *)&ifr) < 0)
    {
      error = clib_error_return_unix (0, "ioctl TUNSETIFF");
      goto done;
    }
    
  /* Make it persistent, at least until we split. */
  if (ioctl (tm->dev_net_tun_fd, TUNSETPERSIST, 1) < 0)
    {
      error = clib_error_return_unix (0, "TUNSETPERSIST");
      goto done;
    }

  /* Open a provisioning socket */
  if ((tm->dev_tap_fd = socket(PF_PACKET, SOCK_RAW,
			       htons(ETH_P_ALL))) < 0 )
    {
      error = clib_error_return_unix (0, "socket");
      goto done;
    }

  /* Find the interface index. */
  {
    struct ifreq ifr;
    struct sockaddr_ll sll;

    memset (&ifr, 0, sizeof(ifr));
    strncpy (ifr.ifr_name, tm->tun_name, sizeof(ifr.ifr_name)-1);
    if (ioctl (tm->dev_tap_fd, SIOCGIFINDEX, &ifr) < 0 )
      {
	error = clib_error_return_unix (0, "ioctl SIOCGIFINDEX");
	goto done;
      }

    /* Bind the provisioning socket to the interface. */
    memset(&sll, 0, sizeof(sll));
    sll.sll_family   = AF_PACKET;
    sll.sll_ifindex  = ifr.ifr_ifindex;
    sll.sll_protocol = htons(ETH_P_ALL);

    if (bind(tm->dev_tap_fd, (struct sockaddr*) &sll, sizeof(sll)) < 0)
      {
	error = clib_error_return_unix (0, "bind");
	goto done;
      }
  }

  /* non-blocking I/O on /dev/tapX */
  {
    int one = 1;
    if (ioctl (tm->dev_net_tun_fd, FIONBIO, &one) < 0)
      {
	error = clib_error_return_unix (0, "ioctl FIONBIO");
	goto done;
      }
  }

  tm->mtu_buffers = (tm->mtu_bytes + (buffer_size - 1)) / buffer_size;

  ifr.ifr_mtu = tm->mtu_bytes;
  if (ioctl (tm->dev_tap_fd, SIOCSIFMTU, &ifr) < 0)
    {
      error = clib_error_return_unix (0, "ioctl SIOCSIFMTU");
      goto done;
    }

  /* get flags, modify to bring up interface... */
  if (ioctl (tm->dev_tap_fd, SIOCGIFFLAGS, &ifr) < 0)
    {
      error = clib_error_return_unix (0, "ioctl SIOCGIFFLAGS");
      goto done;
    }

  ifr.ifr_flags |= (IFF_UP | IFF_RUNNING);

  if (ioctl (tm->dev_tap_fd, SIOCSIFFLAGS, &ifr) < 0)
    {
      error = clib_error_return_unix (0, "ioctl SIOCSIFFLAGS");
      goto done;
    }

  if (is_ether)
    {
      if (ioctl (tm->dev_tap_fd, SIOCGIFHWADDR, &ifr) < 0)
        {
          error = clib_error_return_unix (0, "ioctl SIOCGIFHWADDR");
          goto done;
        }
      else
        clib_memcpy (tm->ether_dst_mac, ifr.ifr_hwaddr.sa_data, 6);
    }

  if (have_normal_interface)
    {
      vnet_main_t *vnm = vnet_get_main();
      error = ethernet_register_interface
        (vnm,
         tuntap_dev_class.index,
         0 /* device instance */,
         tm->ether_dst_mac /* ethernet address */,
         &tm->hw_if_index, 
         0 /* flag change */);
      if (error)
        clib_error_report (error);
      tm->sw_if_index = tm->hw_if_index;
      vm->os_punt_frame = tuntap_nopunt_frame;
    }
  else
    {
      vnet_main_t *vnm = vnet_get_main();
      vnet_hw_interface_t * hi;
      
      vm->os_punt_frame = tuntap_punt_frame;
      
      tm->hw_if_index = vnet_register_interface
        (vnm,
         tuntap_dev_class.index, 0 /* device instance */,
         tuntap_interface_class.index, 0);
      hi = vnet_get_hw_interface (vnm, tm->hw_if_index);
      tm->sw_if_index = hi->sw_if_index;
      
      /* Interface is always up. */
      vnet_hw_interface_set_flags (vnm, tm->hw_if_index, 
                                   VNET_HW_INTERFACE_FLAG_LINK_UP);
      vnet_sw_interface_set_flags (vnm, tm->sw_if_index, 
                                   VNET_SW_INTERFACE_FLAG_ADMIN_UP);
    }

  {
    unix_file_t template = {0};
    template.read_function = tuntap_read_ready;
    template.file_descriptor = tm->dev_net_tun_fd;
    tm->unix_file_index = unix_file_add (&unix_main, &template);
  }

 done:
  if (error)
    {
      if (tm->dev_net_tun_fd >= 0)
	close (tm->dev_net_tun_fd);
      if (tm->dev_tap_fd >= 0)
	close (tm->dev_tap_fd);
    }

  return error;
}

VLIB_CONFIG_FUNCTION (mtcp_config, "mtcp");

void
tuntap_ip4_add_del_interface_address (ip4_main_t * im,
				      uword opaque,
				      u32 sw_if_index,
				      ip4_address_t * address,
				      u32 address_length,
				      u32 if_address_index,
				      u32 is_delete)
{
  tuntap_main_t * tm = &tuntap_main;
  struct ifreq ifr;
  subif_address_t subif_addr, * ap;
  uword * p;

  /* Tuntap disabled, or using a "normal" interface. */
  if (tm->have_normal_interface ||  tm->dev_tap_fd < 0)
    return;

  /* See if we already know about this subif */
  memset (&subif_addr, 0, sizeof (subif_addr));
  subif_addr.sw_if_index = sw_if_index;
  clib_memcpy (&subif_addr.addr, address, sizeof (*address));
  
  p = mhash_get (&tm->subif_mhash, &subif_addr);

  if (p)
    ap = pool_elt_at_index (tm->subifs, p[0]);
  else
    {
      pool_get (tm->subifs, ap);
      *ap = subif_addr;
      mhash_set (&tm->subif_mhash, ap, ap - tm->subifs, 0);
    }

  /* Use subif pool index to select alias device. */
  memset (&ifr, 0, sizeof (ifr));
  snprintf (ifr.ifr_name, sizeof(ifr.ifr_name), 
            "%s:%d", tm->tun_name, (int)(ap - tm->subifs));

  if (! is_delete)
    {
      struct sockaddr_in * sin;

      sin = (struct sockaddr_in *)&ifr.ifr_addr;

      /* Set ipv4 address, netmask. */
      sin->sin_family = AF_INET;
      clib_memcpy (&sin->sin_addr.s_addr, address, 4);
      if (ioctl (tm->dev_tap_fd, SIOCSIFADDR, &ifr) < 0)
	clib_unix_warning ("ioctl SIOCSIFADDR");
    
      sin->sin_addr.s_addr = im->fib_masks[address_length];
      if (ioctl (tm->dev_tap_fd, SIOCSIFNETMASK, &ifr) < 0)
	clib_unix_warning ("ioctl SIOCSIFNETMASK");
    }
  else
    {
      mhash_unset (&tm->subif_mhash, &subif_addr, 0 /* old value ptr */);
      pool_put (tm->subifs, ap);
    }

  /* get flags, modify to bring up interface... */
  if (ioctl (tm->dev_tap_fd, SIOCGIFFLAGS, &ifr) < 0)
    clib_unix_warning ("ioctl SIOCGIFFLAGS");

  if (is_delete)
    ifr.ifr_flags &= ~(IFF_UP | IFF_RUNNING);
  else
    ifr.ifr_flags |= (IFF_UP | IFF_RUNNING);

  if (ioctl (tm->dev_tap_fd, SIOCSIFFLAGS, &ifr) < 0)
    clib_unix_warning ("ioctl SIOCSIFFLAGS");
}

/*
 * $$$$ gross workaround for a known #include bug 
 * #include <linux/ipv6.h> causes multiple definitions if
 * netinet/in.h is also included.
 */
struct in6_ifreq {
	struct in6_addr	ifr6_addr;
        u32		ifr6_prefixlen;
	int		ifr6_ifindex; 
};

/* 
 * Both the v6 interface address API and the way ifconfig
 * displays subinterfaces differ from their v4 couterparts.
 * The code given here seems to work but YMMV.
 */
void
tuntap_ip6_add_del_interface_address (ip6_main_t * im,
				      uword opaque,
				      u32 sw_if_index,
				      ip6_address_t * address,
				      u32 address_length,
				      u32 if_address_index,
				      u32 is_delete)
{
  tuntap_main_t * tm = &tuntap_main;
  struct ifreq ifr;
  struct in6_ifreq ifr6;
  subif_address_t subif_addr, * ap;
  uword * p;

  /* Tuntap disabled, or using a "normal" interface. */
  if (tm->have_normal_interface ||  tm->dev_tap_fd < 0)
    return;

  /* See if we already know about this subif */
  memset (&subif_addr, 0, sizeof (subif_addr));
  subif_addr.sw_if_index = sw_if_index;
  subif_addr.is_v6 = 1;
  clib_memcpy (&subif_addr.addr, address, sizeof (*address));
  
  p = mhash_get (&tm->subif_mhash, &subif_addr);

  if (p)
    ap = pool_elt_at_index (tm->subifs, p[0]);
  else
    {
      pool_get (tm->subifs, ap);
      *ap = subif_addr;
      mhash_set (&tm->subif_mhash, ap, ap - tm->subifs, 0);
    }

  /* Use subif pool index to select alias device. */
  memset (&ifr, 0, sizeof (ifr));
  memset (&ifr6, 0, sizeof (ifr6));
  snprintf (ifr.ifr_name, sizeof(ifr.ifr_name), 
            "%s:%d", tm->tun_name, (int)(ap - tm->subifs));

  if (! is_delete)
    {
      int sockfd = socket (AF_INET6, SOCK_STREAM, 0);
      if (sockfd < 0)
        clib_unix_warning ("get ifindex socket");

      if (ioctl (sockfd, SIOGIFINDEX, &ifr) < 0)
        clib_unix_warning ("get ifindex");

      ifr6.ifr6_ifindex = ifr.ifr_ifindex;
      ifr6.ifr6_prefixlen = address_length;
      clib_memcpy (&ifr6.ifr6_addr, address, 16);

      if (ioctl (sockfd, SIOCSIFADDR, &ifr6) < 0)
        clib_unix_warning ("set address");

      close (sockfd);
    }
  else
    {
      int sockfd = socket (AF_INET6, SOCK_STREAM, 0);
      if (sockfd < 0)
        clib_unix_warning ("get ifindex socket");

      if (ioctl (sockfd, SIOGIFINDEX, &ifr) < 0)
        clib_unix_warning ("get ifindex");

      ifr6.ifr6_ifindex = ifr.ifr_ifindex;
      ifr6.ifr6_prefixlen = address_length;
      clib_memcpy (&ifr6.ifr6_addr, address, 16);

      if (ioctl (sockfd, SIOCDIFADDR, &ifr6) < 0)
        clib_unix_warning ("del address");

      close (sockfd);

      mhash_unset (&tm->subif_mhash, &subif_addr, 0 /* old value ptr */);
      pool_put (tm->subifs, ap);
    }
}

static void
tuntap_punt_frame (vlib_main_t * vm,
                   vlib_node_runtime_t * node,
                   vlib_frame_t * frame)
{
  tuntap_tx (vm, node, frame);
  vlib_frame_free (vm, node, frame);
}

static void
tuntap_nopunt_frame (vlib_main_t * vm,
                   vlib_node_runtime_t * node,
                   vlib_frame_t * frame)
{
  u32 * buffers = vlib_frame_args (frame);
  uword n_packets = frame->n_vectors;
  vlib_buffer_free (vm, buffers, n_packets);
  vlib_frame_free (vm, node, frame);
}

VNET_HW_INTERFACE_CLASS (tuntap_interface_class,static) = {
  .name = "tuntap",
};

static u8 * format_tuntap_interface_name (u8 * s, va_list * args)
{
  u32 i = va_arg (*args, u32);

  s = format (s, "tuntap-%d", i);
  return s;
}

static uword
tuntap_intfc_tx (vlib_main_t * vm,
		 vlib_node_runtime_t * node,
		 vlib_frame_t * frame)
{
  tuntap_main_t * tm = &tuntap_main;
  u32 * buffers = vlib_frame_args (frame);
  uword n_buffers = frame->n_vectors;

  /* Normal interface transmit happens only on the normal interface... */
  if (tm->have_normal_interface)
    return tuntap_tx (vm, node, frame);

  vlib_buffer_free (vm, buffers, n_buffers);
  return n_buffers;
}

VNET_DEVICE_CLASS (tuntap_dev_class,static) = {
  .name = "tuntap",
  .tx_function = tuntap_intfc_tx,
  .format_device_name = format_tuntap_interface_name,
};

static clib_error_t *
tuntap_init (vlib_main_t * vm)
{
  clib_error_t * error;
  ip4_main_t * im4 = &ip4_main;
  ip6_main_t * im6 = &ip6_main;
  ip4_add_del_interface_address_callback_t cb4;
  ip6_add_del_interface_address_callback_t cb6;
  tuntap_main_t * tm = &tuntap_main;

  error = vlib_call_init_function (vm, ip4_init);
  if (error)
    return error;

  mhash_init (&tm->subif_mhash, sizeof (u32), sizeof(subif_address_t));
  cb4.function = tuntap_ip4_add_del_interface_address;
  cb4.function_opaque = 0;
  vec_add1 (im4->add_del_interface_address_callbacks, cb4);

  cb6.function = tuntap_ip6_add_del_interface_address;
  cb6.function_opaque = 0;
  vec_add1 (im6->add_del_interface_address_callbacks, cb6);

  return 0;
}

VLIB_INIT_FUNCTION (tuntap_init)