. /usr/share/libubox/jshn.sh

proto_config_add_generic() {
	json_add_array ""
	json_add_string "" "$1"
	json_add_int "" "$2"
	json_close_array
}

proto_config_add_int() {
	proto_config_add_generic "$1" 5
}

proto_config_add_string() {
	proto_config_add_generic "$1" 3
}

proto_config_add_boolean() {
	proto_config_add_generic "$1" 7
}

add_default_handler() {
	case "$(type $1 2>/dev/null)" in
		*function*) return;;
		*) eval "$1() { return; }"
	esac
}

_proto_do_teardown() {
	json_load "$data"
	eval "$1_teardown \"$interface\" \"$ifname\""
}

_proto_do_setup() {
	json_load "$data"
	eval "$1_setup \"$interface\" \"$ifname\""
}

proto_init_update() {
	local ifname="$1"
	local up="$2"
	local external="$3"

	PROTO_INIT=1
	PROTO_IPADDR=
	PROTO_IP6ADDR=
	PROTO_ROUTE=
	PROTO_ROUTE6=
	json_init
	json_add_string "ifname" "$ifname"
	json_add_boolean "link-up" "$up"
	[ -n "$3" ] && json_add_boolean "address-external" "$external"
}

proto_add_ipv4_address() {
	local address="$1"
	local mask="$2"

	jshn_append PROTO_IPADDR "$address/$mask"
}

proto_add_ipv6_address() {
	local address="$1"
	local mask="$2"

	jshn_append PROTO_IP6ADDR "$address/$mask"
}

proto_add_ipv4_route() {
	local target="$1"
	local mask="$2"
	local gw="$3"

	jshn_append PROTO_ROUTE "$target/$mask/$gw"
}

proto_add_ipv6_route() {
	local target="$1"
	local mask="$2"
	local gw="$3"

	jshn_append PROTO_ROUTE6 "$target/$mask/$gw"
}

_proto_push_ip() {
	json_add_string "" "$1"
}

_proto_push_route() {
	local str="$1";
	local target="${str%%/*}"
	str="${str#*/}"
	local mask="${str%%/*}"
	local gw="${str#*/}"

	json_add_table ""
	json_add_string target "$target"
	json_add_integer mask "$mask"
	json_add_string gateway "$gw"
	json_close_table
}

_proto_push_array() {
	local name="$1"
	local val="$2"
	local cb="$3"

	[ -n "$val" ] || return 0
	json_add_array "$name"
	for item in $val; do
		eval "$cb \"\$item\""
	done
	json_close_array
}

proto_send_update() {
	local interface="$1"

	_proto_push_array "ipaddr" "$PROTO_IPADDR" _proto_push_ip
	_proto_push_array "ip6addr" "$PROTO_IP6ADDR" _proto_push_ip
	_proto_push_array "route" "$PROTO_ROUTE" _proto_push_route
	_proto_push_array "route6" "$PROTO_ROUTE6" _proto_push_route
	ubus call network.interface."$interface" notify_proto "$(json_dump)" &
}

init_proto() {
	proto="$1"; shift
	cmd="$1"; shift

	case "$cmd" in
		dump)
			add_protocol() {
				no_device=0
				available=0

				add_default_handler "$1_init_config"

				json_init
				json_add_string "name" "$1"
				eval "$1_init"
				json_add_boolean no-device "$no_device"
				json_add_boolean available "$available"
				json_add_array "config"
				eval "$1_init_config"
				json_close_array
				json_dump
			}
		;;
		setup|teardown)
			interface="$1"; shift
			data="$1"; shift
			ifname="$1"; shift

			add_protocol() {
				[[ "$proto" == "$1" ]] || return 0

				case "$cmd" in
					setup) _proto_do_setup "$1";;
					teardown) _proto_do_teardown "$1" ;;
					*) return 1 ;;
				esac
			}
		;;
	esac
}
