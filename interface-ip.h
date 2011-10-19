#ifndef __INTERFACE_IP_H
#define __INTERFACE_IP_H

#include "interface.h"

enum device_addr_flags {
	/* address family for routes and addresses */
	DEVADDR_INET4		= (0 << 0),
	DEVADDR_INET6		= (1 << 0),
	DEVADDR_FAMILY		= DEVADDR_INET4 | DEVADDR_INET6,

	/* device route (no gateway) */
	DEVADDR_DEVICE		= (1 << 1),

	/* externally added address */
	DEVADDR_EXTERNAL	= (1 << 2),
};

union if_addr {
	struct in_addr in;
	struct in6_addr in6;
};

struct device_addr {
	struct vlist_node node;

	enum device_addr_flags flags;

	/* must be last */
	unsigned int mask;
	union if_addr addr;
};

struct device_route {
	struct vlist_node node;

	enum device_addr_flags flags;
	bool keep;

	union if_addr nexthop;
	struct device *device;

	/* must be last */
	unsigned int mask;
	union if_addr addr;
};

struct dns_server {
	struct list_head list;
	int af;
	union if_addr addr;
};

struct dns_search_domain {
	struct list_head list;
	char name[];
};

void interface_ip_init(struct interface_ip_settings *ip, struct interface *iface);
void interface_add_dns_server(struct interface_ip_settings *ip, const char *str);
void interface_add_dns_server_list(struct interface_ip_settings *ip, struct blob_attr *list);
void interface_add_dns_search_list(struct interface_ip_settings *ip, struct blob_attr *list);
void interface_write_resolv_conf(void);

void interface_ip_update_start(struct interface_ip_settings *ip);
void interface_ip_update_complete(struct interface_ip_settings *ip);
void interface_ip_flush(struct interface_ip_settings *ip);


#endif
