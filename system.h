#ifndef __NETIFD_SYSTEM_H
#define __NETIFD_SYSTEM_H

#include <sys/socket.h>
#include "device.h"
#include "interface-ip.h"

enum bridge_opt {
	/* stp and forward delay always set */
	BRIDGE_OPT_AGEING_TIME = (1 << 0),
	BRIDGE_OPT_HELLO_TIME  = (1 << 1),
	BRIDGE_OPT_MAX_AGE     = (1 << 2),
};

struct bridge_config {
	enum bridge_opt flags;
	bool stp;
	int forward_delay;

	int ageing_time;
	int hello_time;
	int max_age;
};

int system_init(void);

int system_bridge_addbr(struct device *bridge, struct bridge_config *cfg);
int system_bridge_delbr(struct device *bridge);
int system_bridge_addif(struct device *bridge, struct device *dev);
int system_bridge_delif(struct device *bridge, struct device *dev);

int system_vlan_add(struct device *dev, int id);
int system_vlan_del(struct device *dev);

int system_if_up(struct device *dev);
int system_if_down(struct device *dev);
int system_if_check(struct device *dev);

int system_add_address(struct device *dev, struct device_addr *addr);
int system_del_address(struct device *dev, struct device_addr *addr);

int system_add_route(struct device *dev, struct device_route *route);
int system_del_route(struct device *dev, struct device_route *route);

#endif
