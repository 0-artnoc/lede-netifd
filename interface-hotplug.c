#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libubox/uloop.h>

#include "netifd.h"
#include "interface.h"

char *hotplug_cmd_path = DEFAULT_HOTPLUG_PATH;
static struct interface *current;
static enum interface_event current_ev;
static struct list_head pending = LIST_HEAD_INIT(pending);

static void task_complete(struct uloop_process *proc, int ret);
static struct uloop_process task = {
	.cb = task_complete,
};

static void
run_cmd(const char *ifname, bool up)
{
	char *argv[3];
	int pid;

	pid = fork();
	if (pid < 0)
		return task_complete(NULL, -1);

	if (pid > 0) {
		task.pid = pid;
		uloop_process_add(&task);
		return;
	}

	setenv("ACTION", up ? "ifup" : "ifdown", 1);
	setenv("INTERFACE", ifname, 1);
	argv[0] = hotplug_cmd_path;
	argv[1] = "network";
	argv[2] = NULL;
	execvp(argv[0], argv);
	exit(127);
}

static void
call_hotplug(void)
{
	if (list_empty(&pending))
		return;

	current = list_first_entry(&pending, struct interface, hotplug_list);
	current_ev = current->hotplug_ev;
	list_del_init(&current->hotplug_list);
	run_cmd(current->name, current_ev == IFEV_UP);
}

static void
task_complete(struct uloop_process *proc, int ret)
{
	current = NULL;
	call_hotplug();
}

/*
 * Queue an interface for an up/down event.
 * An interface can only have one event in the queue and one
 * event waiting for completion.
 * When queueing an event that is the same as the one waiting for
 * completion, remove the interface from the queue
 */
void
interface_queue_event(struct interface *iface, enum interface_event ev)
{
	enum interface_event last_ev;

	D(SYSTEM, "Queue hotplug handler for interface '%s'\n", iface->name);
	if (current == iface)
		last_ev = current_ev;
	else
		last_ev = iface->hotplug_ev;

	iface->hotplug_ev = ev;
	if (last_ev == ev && !list_empty(&iface->hotplug_list))
		list_del(&iface->hotplug_list);
	else if (last_ev != ev && list_empty(&iface->hotplug_list))
		list_add(&iface->hotplug_list, &pending);

	if (!task.pending && !current)
		call_hotplug();
}

void
interface_dequeue_event(struct interface *iface)
{
	if (iface == current)
		current = NULL;

	if (!list_empty(&iface->hotplug_list))
		list_del_init(&iface->hotplug_list);
}
