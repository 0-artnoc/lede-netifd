#define _GNU_SOURCE

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <glob.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <libubox/blobmsg_json.h>

#include "netifd.h"
#include "interface.h"
#include "interface-ip.h"
#include "proto.h"

static struct netifd_fd proto_fd;

struct proto_shell_handler {
	struct list_head list;
	struct proto_handler proto;
	struct config_param_list config;
	char *config_buf;
	char script_name[];
};

struct proto_shell_state {
	struct interface_proto_state proto;
	struct proto_shell_handler *handler;
	struct blob_attr *config;

	struct device_user l3_dev;

	struct uloop_timeout setup_timeout;
	struct netifd_process setup_task;
	struct netifd_process teardown_task;
	bool teardown_pending;
	bool teardown_wait_task;

	struct netifd_process proto_task;
	int last_error;
};

static int
proto_shell_handler(struct interface_proto_state *proto,
		    enum interface_proto_cmd cmd, bool force)
{
	struct proto_shell_state *state;
	struct proto_shell_handler *handler;
	struct netifd_process *proc;
	static char error_buf[32];
	const char *argv[7];
	char *envp[2];
	const char *action;
	char *config;
	int ret, i = 0, j = 0;

	state = container_of(proto, struct proto_shell_state, proto);
	handler = state->handler;

	if (cmd == PROTO_CMD_SETUP) {
		action = "setup";
		proc = &state->setup_task;
		state->last_error = -1;
	} else {
		action = "teardown";
		proc = &state->teardown_task;
		if (state->setup_task.uloop.pending && !state->teardown_wait_task) {
			uloop_timeout_set(&state->setup_timeout, 1000);
			kill(state->setup_task.uloop.pid, SIGTERM);
			state->teardown_pending = true;
			return 0;
		}
		if (state->last_error >= 0) {
			snprintf(error_buf, sizeof(error_buf), "ERROR=%d", state->last_error);
			envp[j++] = error_buf;
		}
	}

	config = blobmsg_format_json(state->config, true);
	if (!config)
		return -1;

	argv[i++] = handler->script_name;
	argv[i++] = handler->proto.name;
	argv[i++] = action;
	argv[i++] = proto->iface->name;
	argv[i++] = config;
	if (proto->iface->main_dev.dev)
		argv[i++] = proto->iface->main_dev.dev->ifname;
	argv[i] = NULL;
	envp[j] = NULL;

	ret = netifd_start_process(argv, envp, proc);
	free(config);

	return ret;
}

static void
proto_shell_setup_timeout_cb(struct uloop_timeout *timeout)
{
	struct proto_shell_state *state;

	state = container_of(timeout, struct proto_shell_state, setup_timeout);
	kill(state->setup_task.uloop.pid, SIGKILL);
}

static void
proto_shell_setup_cb(struct netifd_process *p, int ret)
{
	struct proto_shell_state *state;

	state = container_of(p, struct proto_shell_state, setup_task);
	uloop_timeout_cancel(&state->setup_timeout);
	if (state->teardown_pending) {
		state->teardown_pending = false;
		proto_shell_handler(&state->proto, PROTO_CMD_TEARDOWN, false);
	}
}

static void
proto_shell_teardown_cb(struct netifd_process *p, int ret)
{
	struct proto_shell_state *state;

	state = container_of(p, struct proto_shell_state, teardown_task);

	if (state->teardown_wait_task)
		return;

	netifd_kill_process(&state->proto_task);
	state->proto.proto_event(&state->proto, IFPEV_DOWN);
}

static void
proto_shell_task_cb(struct netifd_process *p, int ret)
{
	struct proto_shell_state *state;
	bool teardown_wait_task;

	state = container_of(p, struct proto_shell_state, proto_task);

	teardown_wait_task = state->teardown_wait_task;
	state->teardown_wait_task = false;
	if (state->teardown_pending || state->teardown_task.uloop.pending)
		return;

	if (teardown_wait_task) {
		proto_shell_teardown_cb(&state->teardown_task, 0);
		return;
	}

	state->last_error = ret;
	state->proto.proto_event(&state->proto, IFPEV_LINK_LOST);
	proto_shell_handler(&state->proto, PROTO_CMD_TEARDOWN, false);
}

static void
proto_shell_free(struct interface_proto_state *proto)
{
	struct proto_shell_state *state;

	state = container_of(proto, struct proto_shell_state, proto);
	free(state->config);
	free(state);
}

static void
proto_shell_parse_addr_list(struct interface_ip_settings *ip, struct blob_attr *attr,
			    bool v6, bool external)
{
	struct device_addr *addr;
	struct blob_attr *cur;
	int rem;

	blobmsg_for_each_attr(cur, attr, rem) {
		if (blobmsg_type(cur) != BLOBMSG_TYPE_STRING) {
			DPRINTF("Ignore wrong address type: %d\n", blobmsg_type(cur));
			continue;
		}

		addr = proto_parse_ip_addr_string(blobmsg_data(cur), v6, v6 ? 32 : 128);
		if (!addr) {
			DPRINTF("Failed to parse IP address string: %s\n", (char *) blobmsg_data(cur));
			continue;
		}

		if (external)
			addr->flags |= DEVADDR_EXTERNAL;

		vlist_add(&ip->addr, &addr->node);
	}
}

static void
proto_shell_parse_route_list(struct interface *iface, struct blob_attr *attr,
			     bool v6)
{
	struct blob_attr *cur;
	int rem;

	blobmsg_for_each_attr(cur, attr, rem) {
		if (blobmsg_type(cur) != BLOBMSG_TYPE_TABLE) {
			DPRINTF("Ignore wrong route type: %d\n", blobmsg_type(cur));
			continue;
		}

		interface_ip_add_route(iface, cur, v6);
	}
}

enum {
	NOTIFY_ACTION,
	NOTIFY_COMMAND,
	NOTIFY_ENV,
	NOTIFY_SIGNAL,
	NOTIFY_LINK_UP,
	NOTIFY_IFNAME,
	NOTIFY_ADDR_EXT,
	NOTIFY_IPADDR,
	NOTIFY_IP6ADDR,
	NOTIFY_ROUTES,
	NOTIFY_ROUTES6,
	NOTIFY_DNS,
	NOTIFY_DNS_SEARCH,
	__NOTIFY_LAST
};

static const struct blobmsg_policy notify_attr[__NOTIFY_LAST] = {
	[NOTIFY_ACTION] = { .name = "action", .type = BLOBMSG_TYPE_INT32 },
	[NOTIFY_COMMAND] = { .name = "command", .type = BLOBMSG_TYPE_ARRAY },
	[NOTIFY_ENV] = { .name = "env", .type = BLOBMSG_TYPE_ARRAY },
	[NOTIFY_SIGNAL] = { .name = "signal", .type = BLOBMSG_TYPE_INT32 },
	[NOTIFY_LINK_UP] = { .name = "link-up", .type = BLOBMSG_TYPE_BOOL },
	[NOTIFY_IFNAME] = { .name = "ifname", .type = BLOBMSG_TYPE_STRING },
	[NOTIFY_ADDR_EXT] = { .name = "address-external", .type = BLOBMSG_TYPE_BOOL },
	[NOTIFY_IPADDR] = { .name = "ipaddr", .type = BLOBMSG_TYPE_ARRAY },
	[NOTIFY_IP6ADDR] = { .name = "ip6addr", .type = BLOBMSG_TYPE_ARRAY },
	[NOTIFY_ROUTES] = { .name = "routes", .type = BLOBMSG_TYPE_ARRAY },
	[NOTIFY_ROUTES6] = { .name = "routes6", .type = BLOBMSG_TYPE_ARRAY },
	[NOTIFY_DNS] = { .name = "dns", .type = BLOBMSG_TYPE_ARRAY },
	[NOTIFY_DNS_SEARCH] = { .name = "dns_search", .type = BLOBMSG_TYPE_ARRAY },
};

static int
proto_shell_update_link(struct proto_shell_state *state, struct blob_attr **tb)
{
	struct interface_ip_settings *ip;
	struct blob_attr *cur;
	int dev_create = 1;
	bool addr_ext = false;
	bool up;

	if (!tb[NOTIFY_LINK_UP])
		return UBUS_STATUS_INVALID_ARGUMENT;

	up = blobmsg_get_bool(tb[NOTIFY_LINK_UP]);
	if (!up) {
		state->proto.proto_event(&state->proto, IFPEV_LINK_LOST);
		return 0;
	}

	if ((cur = tb[NOTIFY_ADDR_EXT]) != NULL) {
		addr_ext = blobmsg_get_bool(cur);
		if (addr_ext)
			dev_create = 2;
	}

	if (!tb[NOTIFY_IFNAME]) {
		if (!state->proto.iface->main_dev.dev)
			return UBUS_STATUS_INVALID_ARGUMENT;
	} else {
		if (state->l3_dev.dev)
			device_remove_user(&state->l3_dev);

		device_add_user(&state->l3_dev,
			device_get(blobmsg_data(tb[NOTIFY_IFNAME]), dev_create));
		state->proto.iface->l3_dev = &state->l3_dev;
		device_claim(&state->l3_dev);
	}

	ip = &state->proto.iface->proto_ip;
	interface_update_start(state->proto.iface);

	if ((cur = tb[NOTIFY_IPADDR]) != NULL)
		proto_shell_parse_addr_list(ip, cur, false, addr_ext);

	if ((cur = tb[NOTIFY_IP6ADDR]) != NULL)
		proto_shell_parse_addr_list(ip, cur, true, addr_ext);

	if ((cur = tb[NOTIFY_ROUTES]) != NULL)
		proto_shell_parse_route_list(state->proto.iface, cur, false);

	if ((cur = tb[NOTIFY_ROUTES6]) != NULL)
		proto_shell_parse_route_list(state->proto.iface, cur, true);

	if ((cur = tb[NOTIFY_DNS]) != NULL)
		interface_add_dns_server_list(ip, cur);

	if ((cur = tb[NOTIFY_DNS_SEARCH]) != NULL)
		interface_add_dns_search_list(ip, cur);

	interface_update_complete(state->proto.iface);

	state->proto.proto_event(&state->proto, IFPEV_UP);

	return 0;
}

static bool
fill_string_list(struct blob_attr *attr, char **argv, int max)
{
	struct blob_attr *cur;
	int argc = 0;
	int rem;

	if (!attr)
		goto out;

	blobmsg_for_each_attr(cur, attr, rem) {
		if (blobmsg_type(cur) != BLOBMSG_TYPE_STRING)
			return false;

		if (!blobmsg_check_attr(cur, NULL))
			return false;

		argv[argc++] = blobmsg_data(cur);
		if (argc == max - 1)
			return false;
	}

out:
	argv[argc] = NULL;
	return true;
}

static int
proto_shell_run_command(struct proto_shell_state *state, struct blob_attr **tb)
{
	static char *argv[64];
	static char *env[32];

	if (!tb[NOTIFY_COMMAND])
		goto error;

	if (!fill_string_list(tb[NOTIFY_COMMAND], argv, ARRAY_SIZE(argv)))
		goto error;

	if (!fill_string_list(tb[NOTIFY_ENV], env, ARRAY_SIZE(env)))
		goto error;

	netifd_start_process((const char **) argv, (char **) env, &state->proto_task);

	return 0;

error:
	return UBUS_STATUS_INVALID_ARGUMENT;
}

static int
proto_shell_kill_command(struct proto_shell_state *state, struct blob_attr **tb)
{
	unsigned int signal = ~0;

	if (tb[NOTIFY_SIGNAL])
		signal = blobmsg_get_u32(tb[NOTIFY_SIGNAL]);

	if (signal > 31)
		signal = SIGTERM;

	if (state->proto_task.uloop.pending) {
		kill(state->proto_task.uloop.pid, signal);
		state->teardown_wait_task = true;
	}

	return 0;
}

static int
proto_shell_notify(struct interface_proto_state *proto, struct blob_attr *attr)
{
	struct proto_shell_state *state;
	struct blob_attr *tb[__NOTIFY_LAST];

	state = container_of(proto, struct proto_shell_state, proto);

	blobmsg_parse(notify_attr, __NOTIFY_LAST, tb, blob_data(attr), blob_len(attr));
	if (!tb[NOTIFY_ACTION])
		return UBUS_STATUS_INVALID_ARGUMENT;

	switch(blobmsg_get_u32(tb[NOTIFY_ACTION])) {
	case 0:
		return proto_shell_update_link(state, tb);
	case 1:
		return proto_shell_run_command(state, tb);
	case 2:
		return proto_shell_kill_command(state, tb);
	default:
		return UBUS_STATUS_INVALID_ARGUMENT;
	}
}

static struct interface_proto_state *
proto_shell_attach(const struct proto_handler *h, struct interface *iface,
		   struct blob_attr *attr)
{
	struct proto_shell_state *state;

	state = calloc(1, sizeof(*state));
	state->config = malloc(blob_pad_len(attr));
	if (!state->config)
		goto error;

	memcpy(state->config, attr, blob_pad_len(attr));
	state->proto.free = proto_shell_free;
	state->proto.notify = proto_shell_notify;
	state->proto.cb = proto_shell_handler;
	state->setup_timeout.cb = proto_shell_setup_timeout_cb;
	state->setup_task.cb = proto_shell_setup_cb;
	state->setup_task.dir_fd = proto_fd.fd;
	state->setup_task.log_prefix = iface->name;
	state->teardown_task.cb = proto_shell_teardown_cb;
	state->teardown_task.dir_fd = proto_fd.fd;
	state->teardown_task.log_prefix = iface->name;
	state->proto_task.cb = proto_shell_task_cb;
	state->proto_task.dir_fd = proto_fd.fd;
	state->proto_task.log_prefix = iface->name;
	state->handler = container_of(h, struct proto_shell_handler, proto);

	return &state->proto;

error:
	free(state);
	return NULL;
}

static json_object *
check_type(json_object *obj, json_type type)
{
	if (!obj)
		return NULL;

	if (json_object_get_type(obj) != type)
		return NULL;

	return obj;
}

static inline json_object *
get_field(json_object *obj, const char *name, json_type type)
{
	return check_type(json_object_object_get(obj, name), type);
}

static char *
proto_shell_parse_config(struct config_param_list *config, json_object *obj)
{
	struct blobmsg_policy *attrs;
	char *str_buf, *str_cur;
	int str_len = 0;
	int i;

	config->n_params = json_object_array_length(obj);
	attrs = calloc(1, sizeof(*attrs) * config->n_params);
	if (!attrs)
		return NULL;

	config->params = attrs;
	for (i = 0; i < config->n_params; i++) {
		json_object *cur, *name, *type;

		cur = check_type(json_object_array_get_idx(obj, i), json_type_array);
		if (!cur)
			goto error;

		name = check_type(json_object_array_get_idx(cur, 0), json_type_string);
		if (!name)
			goto error;

		type = check_type(json_object_array_get_idx(cur, 1), json_type_int);
		if (!type)
			goto error;

		attrs[i].name = json_object_get_string(name);
		attrs[i].type = json_object_get_int(type);
		if (attrs[i].type > BLOBMSG_TYPE_LAST)
			goto error;

		str_len += strlen(attrs[i].name) + 1;
	}

	str_buf = malloc(str_len);
	if (!str_buf)
		goto error;

	str_cur = str_buf;
	for (i = 0; i < config->n_params; i++) {
		const char *name = attrs[i].name;

		attrs[i].name = str_cur;
		str_cur += sprintf(str_cur, "%s", name) + 1;
	}

	return str_buf;

error:
	free(attrs);
	config->n_params = 0;
	return NULL;
}

static void
proto_shell_add_handler(const char *script, json_object *obj)
{
	struct proto_shell_handler *handler;
	struct proto_handler *proto;
	json_object *config, *tmp;
	const char *name;
	char *str;

	if (!check_type(obj, json_type_object))
		return;

	tmp = get_field(obj, "name", json_type_string);
	if (!tmp)
		return;

	name = json_object_get_string(tmp);

	handler = calloc(1, sizeof(*handler) +
			 strlen(script) + 1 +
			 strlen(name) + 1);
	if (!handler)
		return;

	strcpy(handler->script_name, script);

	str = handler->script_name + strlen(handler->script_name) + 1;
	strcpy(str, name);

	proto = &handler->proto;
	proto->name = str;
	proto->config_params = &handler->config;
	proto->attach = proto_shell_attach;

	tmp = get_field(obj, "no-device", json_type_boolean);
	if (tmp && json_object_get_boolean(tmp))
		handler->proto.flags |= PROTO_FLAG_NODEV;

	config = get_field(obj, "config", json_type_array);
	if (config)
		handler->config_buf = proto_shell_parse_config(&handler->config, config);

	DPRINTF("Add handler for script %s: %s\n", script, proto->name);
	add_proto_handler(proto);
}

static void proto_shell_add_script(const char *name)
{
	struct json_tokener *tok = NULL;
	json_object *obj;
	static char buf[512];
	char *start, *cmd;
	FILE *f;
	int len;

#define DUMP_SUFFIX	" '' dump"

	cmd = alloca(strlen(name) + 1 + sizeof(DUMP_SUFFIX));
	sprintf(cmd, "%s" DUMP_SUFFIX, name);

	f = popen(cmd, "r");
	if (!f)
		return;

	do {
		start = fgets(buf, sizeof(buf), f);
		if (!start)
			continue;

		len = strlen(start);

		if (!tok)
			tok = json_tokener_new();

		obj = json_tokener_parse_ex(tok, start, len);
		if (!is_error(obj)) {
			proto_shell_add_handler(name, obj);
			json_object_put(obj);
			json_tokener_free(tok);
			tok = NULL;
		} else if (start[len - 1] == '\n') {
			json_tokener_free(tok);
			tok = NULL;
		}
	} while (!feof(f) && !ferror(f));

	if (tok)
		json_tokener_free(tok);

	pclose(f);
}

static void __init proto_shell_init(void)
{
	glob_t g;
	int main_fd;
	int i;

	main_fd = open(".", O_RDONLY | O_DIRECTORY);
	if (main_fd < 0)
		return;

	if (chdir(main_path)) {
		perror("chdir(main path)");
		goto close_cur;
	}

	if (chdir("./proto"))
		goto close_cur;

	proto_fd.fd = open(".", O_RDONLY | O_DIRECTORY);
	if (proto_fd.fd < 0)
		goto close_cur;

	netifd_fd_add(&proto_fd);
	glob("./*.sh", 0, NULL, &g);
	for (i = 0; i < g.gl_pathc; i++)
		proto_shell_add_script(g.gl_pathv[i]);

close_cur:
	fchdir(main_fd);
	close(main_fd);
}
