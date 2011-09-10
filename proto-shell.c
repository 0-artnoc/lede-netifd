#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <glob.h>
#include <unistd.h>
#include <fcntl.h>

#include <libubox/blobmsg_json.h>

#include "netifd.h"
#include "interface.h"
#include "interface-ip.h"
#include "proto.h"

static LIST_HEAD(handlers);
static int proto_fd;

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
};

#define DUMP_SUFFIX	" '' dump"

static int run_script(const char **argv)
{
	int pid, ret;

	if ((pid = fork()) < 0)
		return -1;

	if (!pid) {
		fchdir(proto_fd);
		execvp(argv[0], (char **) argv);
		exit(127);
	}

	if (waitpid(pid, &ret, 0) == -1)
		ret = -1;

	if (ret > 0)
		return -ret;

	return 0;
}

static int
proto_shell_handler(struct interface_proto_state *proto,
		    enum interface_proto_cmd cmd, bool force)
{
	struct proto_shell_state *state;
	struct proto_shell_handler *handler;
	const char *argv[5];
	char *config;
	int ret;

	state = container_of(proto, struct proto_shell_state, proto);
	handler = state->handler;

	config = blobmsg_format_json(state->config, true);
	if (!config)
		return -1;

	argv[0] = handler->script_name;
	argv[1] = handler->proto.name;
	argv[2] = "teardown";
	argv[3] = config;
	argv[4] = NULL;

	switch(cmd) {
	case PROTO_CMD_SETUP:
		argv[2] = "setup";
		/* fall through */
	case PROTO_CMD_TEARDOWN:
		ret = run_script(argv);
		break;
	}

	free(config);

	return ret;
}

static void
proto_shell_free(struct interface_proto_state *proto)
{
	struct proto_shell_state *state;

	state = container_of(proto, struct proto_shell_state, proto);
	free(state->config);
	free(state);
}

struct interface_proto_state *
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
	state->proto.handler = proto_shell_handler;
	state->handler = container_of(h, struct proto_shell_handler, proto);

	return &state->proto;

error:
	free(state);
	return NULL;
}

static char *
proto_shell_parse_config(struct config_param_list *config, struct json_object *obj)
{
	struct blobmsg_policy *attrs;
	char *str_buf, *str_cur;
	int str_len = 0;
	int i;

	attrs = calloc(1, sizeof(*attrs));
	if (!attrs)
		return NULL;

	config->n_params = json_object_array_length(obj);
	config->params = attrs;
	for (i = 0; i < config->n_params; i++) {
		struct json_object *cur, *name, *type;

		cur = json_object_array_get_idx(obj, i);
		if (!cur || json_object_get_type(cur) != json_type_array)
			goto error;

		name = json_object_array_get_idx(cur, 0);
		if (!name || json_object_get_type(name) != json_type_string)
			goto error;

		type = json_object_array_get_idx(cur, 1);
		if (!type || json_object_get_type(type) != json_type_int)
			goto error;

		attrs[i].name = json_object_get_string(name);
		attrs[i].type = json_object_get_int(type);
		if (attrs[i].type > BLOBMSG_TYPE_LAST)
			goto error;

		str_len += strlen(attrs[i].name + 1);
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
proto_shell_add_handler(const char *script, struct json_object *obj)
{
	struct proto_shell_handler *handler;
	struct proto_handler *proto;
	json_object *config, *tmp;
	const char *name;
	char *str;

	if (json_object_get_type(obj) != json_type_object)
		return;

	tmp = json_object_object_get(obj, "name");
	if (!tmp || json_object_get_type(tmp) != json_type_string)
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

	config = json_object_object_get(obj, "config");
	if (config && json_object_get_type(config) == json_type_array)
		handler->config_buf = proto_shell_parse_config(&handler->config, config);

	DPRINTF("Add handler for script %s: %s\n", script, proto->name);
	add_proto_handler(proto);
}

static void proto_shell_add_script(const char *name)
{
	struct json_tokener *tok = NULL;
	struct json_object *obj;
	static char buf[512];
	char *start, *end, *cmd;
	FILE *f;
	int buflen, len;

	cmd = alloca(strlen(name) + 1 + sizeof(DUMP_SUFFIX));
	sprintf(cmd, "%s" DUMP_SUFFIX, name);

	f = popen(cmd, "r");
	if (!f)
		return;

	do {
		buflen = fread(buf, 1, sizeof(buf) - 1, f);
		if (buflen <= 0)
			continue;

		start = buf;
		len = buflen;
		do {
			end = memchr(start, '\n', len);
			if (end)
				len = end - start;

			if (!tok)
				tok = json_tokener_new();

			obj = json_tokener_parse_ex(tok, start, len);
			if (!is_error(obj)) {
				proto_shell_add_handler(name, obj);
				json_object_put(obj);
				json_tokener_free(tok);
				tok = NULL;
			}

			if (end) {
				start = end + 1;
				len = buflen - (start - buf);
			}
		} while (len > 0);
	} while (!feof(f) && !ferror(f));

	if (tok)
		json_tokener_free(tok);

	pclose(f);
}

void __init proto_shell_init(void)
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

	proto_fd = open(".", O_RDONLY | O_DIRECTORY);
	if (proto_fd < 0)
		goto close_cur;

	glob("./*.sh", 0, NULL, &g);
	for (i = 0; i < g.gl_pathc; i++)
		proto_shell_add_script(g.gl_pathv[i]);

close_cur:
	fchdir(main_fd);
	close(main_fd);
}
