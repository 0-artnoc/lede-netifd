#!/bin/sh

. ../netifd-proto.sh
init_proto "$@"

ppp_init_config() {
	proto_config_add_string "username"
	proto_config_add_string "password"
	proto_config_add_int "keepalive"
}

ppp_setup() {
	echo "ppp_setup($1): $2"
}

ppp_teardown() {
	return
}

ppp_init() {
	no_device=1
	available=1
}

add_protocol ppp

pppoe_init_config() {
	ppp_init_config
}

pppoe_init() {
	return
}

pppoe_setup() {
	local interface="$1"
	local device="$2"

	json_get_var username username
	json_get_var password password
	echo "pppoe_setup($interface, $device), username=$username, password=$password"
	proto_init_update ppp0 1
	proto_add_ipv4_address "192.168.2.1" 32
	proto_add_dns_server "192.168.2.2"
	proto_send_update "$interface"
	proto_run_command "$interface" sleep 10
}

pppoe_teardown() {
	return
}

add_protocol pppoe
