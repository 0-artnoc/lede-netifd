#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

#include <arpa/inet.h>

#include "netifd.h"
#include "device.h"
#include "interface.h"
#include "interface-ip.h"
#include "proto.h"
#include "ubus.h"
#include "system.h"

enum {
	ROUTE_INTERFACE,
	ROUTE_TARGET,
	ROUTE_MASK,
	ROUTE_GATEWAY,
	ROUTE_DEVICE,
	ROUTE_METRIC,
	ROUTE_MTU,
	__ROUTE_MAX
};

static const struct blobmsg_policy route_attr[__ROUTE_MAX] = {
	[ROUTE_INTERFACE] = { .name = "interface", .type = BLOBMSG_TYPE_STRING },
	[ROUTE_TARGET] = { .name = "target", .type = BLOBMSG_TYPE_STRING },
	[ROUTE_MASK] = { .name = "netmask", .type = BLOBMSG_TYPE_STRING },
	[ROUTE_GATEWAY] = { .name = "gateway", .type = BLOBMSG_TYPE_STRING },
	[ROUTE_DEVICE] = { .name = "device", .type = BLOBMSG_TYPE_STRING },
	[ROUTE_METRIC] = { .name = "metric", .type = BLOBMSG_TYPE_INT32 },
	[ROUTE_MTU] = { .name = "mtu", .type = BLOBMSG_TYPE_INT32 },
};

const struct config_param_list route_attr_list = {
	.n_params = __ROUTE_MAX,
	.params = route_attr,
};

void
interface_ip_add_route(struct interface *iface, struct blob_attr *attr, bool v6)
{
	struct interface_ip_settings *ip;
	struct blob_attr *tb[__ROUTE_MAX], *cur;
	struct device_route *route;
	int af = v6 ? AF_INET6 : AF_INET;
	bool config = false;

	blobmsg_parse(route_attr, __ROUTE_MAX, tb, blobmsg_data(attr), blobmsg_data_len(attr));

	if (!tb[ROUTE_GATEWAY] && !tb[ROUTE_DEVICE])
		return;

	if (!iface) {
		if ((cur = tb[ROUTE_INTERFACE]) == NULL)
			return;

		iface = vlist_find(&interfaces, blobmsg_data(cur), iface, node);
		if (!iface)
			return;

		ip = &iface->config_ip;
		config = true;
	} else {
		ip = &iface->proto_ip;
	}

	route = calloc(1, sizeof(*route));
	if (!route)
		return;

	route->mask = v6 ? 128 : 32;
	if ((cur = tb[ROUTE_MASK]) != NULL) {
		route->mask = parse_netmask_string(blobmsg_data(cur), v6);
		if (route->mask > (v6 ? 128 : 32))
			goto error;
	}

	if ((cur = tb[ROUTE_TARGET]) != NULL) {
		if (!inet_pton(af, blobmsg_data(cur), &route->addr)) {
			DPRINTF("Failed to parse route target: %s\n", (char *) blobmsg_data(cur));
			goto error;
		}
	}

	if ((cur = tb[ROUTE_GATEWAY]) != NULL) {
		if (!inet_pton(af, blobmsg_data(cur), &route->nexthop)) {
			DPRINTF("Failed to parse route gateway: %s\n", (char *) blobmsg_data(cur));
			goto error;
		}
	}

	if ((cur = tb[ROUTE_METRIC]) != NULL)
		route->metric = blobmsg_get_u32(cur);

	if ((cur = tb[ROUTE_MTU]) != NULL)
		route->mtu = blobmsg_get_u32(cur);

	if (!config && (cur = tb[ROUTE_DEVICE]) != NULL)
		route->device = device_get(blobmsg_data(cur), true);

	vlist_add(&ip->route, &route->node);
	return;

error:
	free(route);
}

static int
addr_cmp(const void *k1, const void *k2, void *ptr)
{
	return memcmp(k1, k2, sizeof(struct device_addr) -
		      offsetof(struct device_addr, mask));
}

static int
route_cmp(const void *k1, const void *k2, void *ptr)
{
	return memcmp(k1, k2, sizeof(struct device_route) -
		      offsetof(struct device_route, mask));
}

static void
interface_update_proto_addr(struct vlist_tree *tree,
			    struct vlist_node *node_new,
			    struct vlist_node *node_old)
{
	struct interface_ip_settings *ip;
	struct interface *iface;
	struct device *dev;
	struct device_addr *addr;

	ip = container_of(tree, struct interface_ip_settings, addr);
	iface = ip->iface;
	dev = iface->l3_dev->dev;

	if (node_old) {
		addr = container_of(node_old, struct device_addr, node);
		if (!(addr->flags & DEVADDR_EXTERNAL) && addr->enabled)
			system_del_address(dev, addr);
		free(addr);
	}

	if (node_new) {
		addr = container_of(node_new, struct device_addr, node);
		if (!(addr->flags & DEVADDR_EXTERNAL))
			system_add_address(dev, addr);
		addr->enabled = true;
	}
}

static void
interface_update_proto_route(struct vlist_tree *tree,
			     struct vlist_node *node_new,
			     struct vlist_node *node_old)
{
	struct interface_ip_settings *ip;
	struct interface *iface;
	struct device *dev;
	struct device_route *route;

	ip = container_of(tree, struct interface_ip_settings, route);
	iface = ip->iface;
	dev = iface->l3_dev->dev;

	if (node_old) {
		route = container_of(node_old, struct device_route, node);
		if (!(route->flags & DEVADDR_EXTERNAL) && route->enabled)
			system_del_route(dev, route);
		free(route);
	}

	if (node_new) {
		route = container_of(node_new, struct device_route, node);
		if (!(route->flags & DEVADDR_EXTERNAL))
			system_add_route(dev, route);
		route->enabled = true;
	}
}

void
interface_add_dns_server(struct interface_ip_settings *ip, const char *str)
{
	struct dns_server *s;

	s = calloc(1, sizeof(*s));
	s->af = AF_INET;
	if (inet_pton(s->af, str, &s->addr.in))
		goto add;

	s->af = AF_INET6;
	if (inet_pton(s->af, str, &s->addr.in))
		goto add;

	free(s);
	return;

add:
	D(INTERFACE, "Add IPv%c DNS server: %s\n",
	  s->af == AF_INET6 ? '6' : '4', str);
	list_add_tail(&s->list, &ip->dns_servers);
}

void
interface_add_dns_server_list(struct interface_ip_settings *ip, struct blob_attr *list)
{
	struct blob_attr *cur;
	int rem;

	blobmsg_for_each_attr(cur, list, rem) {
		if (blobmsg_type(cur) != BLOBMSG_TYPE_STRING)
			continue;

		if (!blobmsg_check_attr(cur, NULL))
			continue;

		interface_add_dns_server(ip, blobmsg_data(cur));
	}
}

static void
interface_add_dns_search_domain(struct interface_ip_settings *ip, const char *str)
{
	struct dns_search_domain *s;
	int len = strlen(str);

	s = calloc(1, sizeof(*s) + len + 1);
	if (!s)
		return;

	D(INTERFACE, "Add DNS search domain: %s\n", str);
	memcpy(s->name, str, len);
	list_add_tail(&s->list, &ip->dns_search);
}

void
interface_add_dns_search_list(struct interface_ip_settings *ip, struct blob_attr *list)
{
	struct blob_attr *cur;
	int rem;

	blobmsg_for_each_attr(cur, list, rem) {
		if (blobmsg_type(cur) != BLOBMSG_TYPE_STRING)
			continue;

		if (!blobmsg_check_attr(cur, NULL))
			continue;

		interface_add_dns_search_domain(ip, blobmsg_data(cur));
	}
}

static void
interface_clear_dns_servers(struct interface_ip_settings *ip)
{
	struct dns_server *s, *tmp;

	list_for_each_entry_safe(s, tmp, &ip->dns_servers, list) {
		list_del(&s->list);
		free(s);
	}
}

static void
interface_clear_dns_search(struct interface_ip_settings *ip)
{
	struct dns_search_domain *s, *tmp;

	list_for_each_entry_safe(s, tmp, &ip->dns_search, list) {
		list_del(&s->list);
		free(s);
	}
}

void
interface_write_resolv_conf(void)
{
	struct interface *iface;
	struct dns_server *s;
	struct dns_search_domain *d;
	char *path = alloca(strlen(resolv_conf) + 5);
	const char *str;
	char buf[32];
	FILE *f;

	sprintf(path, "%s.tmp", resolv_conf);
	unlink(path);
	f = fopen(path, "w");
	if (!f) {
		D(INTERFACE, "Failed to open %s for writing\n", path);
		return;
	}

	vlist_for_each_element(&interfaces, iface, node) {
		if (iface->state != IFS_UP)
			continue;

		if (list_empty(&iface->proto_ip.dns_search) &&
		    list_empty(&iface->proto_ip.dns_servers))
			continue;

		fprintf(f, "# Interface %s\n", iface->name);
		list_for_each_entry(s, &iface->proto_ip.dns_servers, list) {
			str = inet_ntop(s->af, &s->addr, buf, sizeof(buf));
			if (!str)
				continue;

			fprintf(f, "nameserver %s\n", str);
		}

		list_for_each_entry(d, &iface->proto_ip.dns_search, list) {
			fprintf(f, "search %s\n", d->name);
		}
	}
	fclose(f);
	if (rename(path, resolv_conf) < 0) {
		D(INTERFACE, "Failed to replace %s\n", resolv_conf);
		unlink(path);
	}
}

void interface_ip_set_enabled(struct interface_ip_settings *ip, bool enabled)
{
	struct device_addr *addr;
	struct device_route *route;
	struct device *dev;

	ip->enabled = enabled;
	dev = ip->iface->l3_dev->dev;
	if (!dev)
		return;

	vlist_for_each_element(&ip->addr, addr, node) {
		if (addr->enabled == enabled)
			continue;

		if (enabled)
			system_add_address(dev, addr);
		else
			system_del_address(dev, addr);
		addr->enabled = enabled;
	}

	vlist_for_each_element(&ip->route, route, node) {
		if (route->enabled == enabled)
			continue;

		if (enabled)
			system_add_route(dev, route);
		else
			system_del_route(dev, route);
		route->enabled = enabled;
	}
}

void
interface_ip_update_start(struct interface_ip_settings *ip)
{
	interface_clear_dns_servers(ip);
	interface_clear_dns_search(ip);
	vlist_update(&ip->route);
	vlist_update(&ip->addr);
}

void
interface_ip_update_complete(struct interface_ip_settings *ip)
{
	vlist_flush(&ip->route);
	vlist_flush(&ip->addr);
}

void
interface_ip_flush(struct interface_ip_settings *ip)
{
	interface_clear_dns_servers(ip);
	interface_clear_dns_search(ip);
	vlist_flush_all(&ip->route);
	vlist_flush_all(&ip->addr);
}

void
interface_ip_init(struct interface_ip_settings *ip, struct interface *iface)
{
	ip->iface = iface;
	ip->enabled = true;
	INIT_LIST_HEAD(&ip->dns_search);
	INIT_LIST_HEAD(&ip->dns_servers);
	vlist_init(&ip->route, route_cmp, interface_update_proto_route,
		   struct device_route, node, mask);
	vlist_init(&ip->addr, addr_cmp, interface_update_proto_addr,
		   struct device_addr, node, mask);
}
