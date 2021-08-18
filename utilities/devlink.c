/*
 * Copyright (c) 2021 Canonical
 *
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
 */
#include <config.h>
#include <errno.h>
#include <inttypes.h>
#include <sysexits.h>
#include <net/if.h>
#include <poll.h>
#include <stddef.h>
#include <linux/devlink.h>

#include "netlink.h"
#include "netlink-socket.h"
#include "netlink-devlink.h"

#include "openvswitch/ofpbuf.h"
#include "openvswitch/poll-loop.h"
#include "openvswitch/vlog.h"
#include "packets.h"

VLOG_DEFINE_THIS_MODULE(devlink);

enum {
    CMD_DUMP,
    CMD_MONITOR,
};

static const char *CMD_NAME[] = {
    "dump",
    "monitor",
};

static void
usage(void)
{
    printf("usage: %s MODE\n"
           "where MODE is one of 'dump' or 'monitor'.\n",
           program_name);
}

static void
print_port(struct dl_port *port_entry) {
    VLOG_INFO("bus_name: '%s'", port_entry->bus_name);
    VLOG_INFO("dev_name: '%s'", port_entry->dev_name);
    VLOG_INFO("index: %"PRIu32, port_entry->index);
    VLOG_INFO("type: %s",
        port_entry->type == DEVLINK_PORT_TYPE_AUTO ? "AUTO" :
        port_entry->type == DEVLINK_PORT_TYPE_ETH ? "ETH" :
        port_entry->type == DEVLINK_PORT_TYPE_IB ? "IB" :
        "unknown");
    VLOG_INFO("desired_type: %"PRIu16, port_entry->desired_type);
    VLOG_INFO("netdev_ifindex: %"PRIu32, port_entry->netdev_ifindex);
    VLOG_INFO("netdev_name: '%s'", port_entry->netdev_name);
    VLOG_INFO("split_count: %"PRIu32, port_entry->split_count);
    VLOG_INFO("split_group: %"PRIu32, port_entry->split_group);
    VLOG_INFO("flavour: %s",
        port_entry->flavour == DEVLINK_PORT_FLAVOUR_PHYSICAL ? "PHYSICAL" :
        port_entry->flavour == DEVLINK_PORT_FLAVOUR_CPU ? "CPU" :
        port_entry->flavour == DEVLINK_PORT_FLAVOUR_DSA ? "DSA" :
        port_entry->flavour == DEVLINK_PORT_FLAVOUR_PCI_PF ? "PCI_PF":
        port_entry->flavour == DEVLINK_PORT_FLAVOUR_PCI_VF ? "PCI_VF":
        port_entry->flavour == DEVLINK_PORT_FLAVOUR_VIRTUAL ? "VIRTUAL":
        port_entry->flavour == DEVLINK_PORT_FLAVOUR_UNUSED ? "UNUSED":
        port_entry->flavour == DEVLINK_PORT_FLAVOUR_PCI_SF ? "PCI_SF":
        "UNKNOWN");
    VLOG_INFO("number: %"PRIu32, port_entry->number);
    VLOG_INFO("split_subport_number: %"PRIu32,
        port_entry->split_subport_number);
    VLOG_INFO("pci_pf_number: %"PRIu16, port_entry->pci_pf_number);
    VLOG_INFO("pci_vf_number: %"PRIu16, port_entry->pci_vf_number);
    VLOG_INFO("function eth_addr: "ETH_ADDR_FMT,
        ETH_ADDR_ARGS(port_entry->function.eth_addr));
    VLOG_INFO("function state: %"PRIu8, port_entry->function.state);
    VLOG_INFO("function opstate: %"PRIu8, port_entry->function.opstate);
    VLOG_INFO("lanes: %"PRIu32, port_entry->lanes);
    VLOG_INFO("splittable: %s",
        port_entry->splittable == 0 ? "false" :
        port_entry->splittable == 1 ? "true" :
        "unknown");
    VLOG_INFO("external: %s",
        port_entry->external == 0 ? "false" :
        port_entry->external == 1 ? "true" :
        "unknown");
    VLOG_INFO("controller_number: %"PRIu32, port_entry->controller_number);
    VLOG_INFO("pci_sf_number: %"PRIu32, port_entry->pci_sf_number);
}

static void
print_version(const char *prefix, struct dl_info_version *version) {
    if (!version->name || version->name == dl_str_not_present) {
        return;
    }
    VLOG_INFO("%s %s: %s", prefix, version->name, version->value);
}

static void
print_info(struct dl_info *info_entry) {
    VLOG_INFO("driver_name: '%s'", info_entry->driver_name);
    VLOG_INFO("serial_number: '%s'", info_entry->serial_number);
    VLOG_INFO("board_serial_number: '%s'", info_entry->board_serial_number);
    print_version("fixed", &info_entry->version_fixed);
    print_version("running", &info_entry->version_running);
    print_version("stored", &info_entry->version_stored);
}

static void
dump(void)
{
    struct nl_dl_dump_state *port_dump;
    struct nl_dl_dump_state *info_dump;
    struct dl_port port_entry;
    struct dl_info info_entry;
    int error;

    printf("port dump\n");
    port_dump = nl_dl_dump_init();
    if ((error = nl_dl_dump_init_error(port_dump))) {
        ovs_fatal(error, "error");
    }

    nl_dl_dump_start(DEVLINK_CMD_PORT_GET, port_dump);
    while (nl_dl_port_dump_next(port_dump, &port_entry)) {
        print_port(&port_entry);
    }
    nl_dl_dump_finish(port_dump);
    nl_dl_dump_destroy(port_dump);

    printf("info dump\n");
    info_dump = nl_dl_dump_init();
    if ((error = nl_dl_dump_init_error(info_dump))) {
        ovs_fatal(error, "error");
    }
    nl_dl_dump_start(DEVLINK_CMD_INFO_GET, info_dump);
    while (nl_dl_info_dump_next(info_dump, &info_entry)) {
        print_info(&info_entry);
    }
    nl_dl_dump_finish(info_dump);
    nl_dl_dump_destroy(info_dump);
}

static void
monitor(void)
{
    uint64_t buf_stub[4096 / 64];
    struct nl_sock *sock;
    struct ofpbuf buf;
    unsigned int devlink_mcgroup;
    int error;

    error = nl_lookup_genl_mcgroup(DEVLINK_GENL_NAME,
                                   DEVLINK_GENL_MCGRP_CONFIG_NAME,
                                   &devlink_mcgroup);
    if (error) {
        ovs_fatal(error, "unable to lookup devlink genl multicast group");
    }

    error = nl_sock_create(NETLINK_GENERIC, &sock);
    if (error) {
        ovs_fatal(error, "could not create genlnetlink socket");
    }

    error = nl_sock_join_mcgroup(sock, devlink_mcgroup);
    if (error) {
        ovs_fatal(error, "could not join devlink config multicast group");
    }

    ofpbuf_use_stub(&buf, buf_stub, sizeof buf_stub);
    for (;;) {
        error = nl_sock_recv(sock, &buf, NULL, false);
        if (error == EAGAIN) {
            /* Nothing to do. */
        } else if (error == ENOBUFS) {
            ovs_error(0, "network monitor socket overflowed");
        } else if (error) {
            ovs_fatal(error, "error on network monitor socket");
        } else {
            struct genlmsghdr *genl;
            struct dl_port port_entry;

            genl = nl_msg_genlmsghdr(&buf);
            printf("cmd=%"PRIu8",version=%"PRIu8")\n",
                   genl->cmd, genl->version);
            switch (genl->cmd) {
            case DEVLINK_CMD_PORT_GET:
            case DEVLINK_CMD_PORT_SET:
            case DEVLINK_CMD_PORT_NEW:
            case DEVLINK_CMD_PORT_DEL:
                if (!nl_dl_parse_port_policy(&buf, &port_entry)) {
                    VLOG_WARN("could not parse port entry");
                    continue;
                }
                VLOG_INFO("%s",
                    genl->cmd == DEVLINK_CMD_PORT_GET ? "DEVLINK_CMD_PORT_GET":
                    genl->cmd == DEVLINK_CMD_PORT_SET ? "DEVLINK_CMD_PORT_SET":
                    genl->cmd == DEVLINK_CMD_PORT_NEW ? "DEVLINK_CMD_PORT_NEW":
                    genl->cmd == DEVLINK_CMD_PORT_DEL ? "DEVLINK_CMD_PORT_DEL":
                    "UNKNOWN");
                print_port(&port_entry);
                break;
            };

        }

        nl_sock_wait(sock, POLLIN);
        poll_block();
    }
}

int
main(int argc, char *argv[])
{
    int cmd = -1;

    set_program_name(argv[0]);
    vlog_set_levels(NULL, VLF_ANY_DESTINATION, VLL_DBG);

    if (argc > 1 && !strcmp(argv[1], CMD_NAME[CMD_DUMP])) {
        cmd = CMD_DUMP;
    } else if (argc > 1 && !strcmp(argv[1], CMD_NAME[CMD_MONITOR])) {
        cmd = CMD_MONITOR;
    }

    switch (cmd) {
    case CMD_DUMP:
        dump();
        break;
    case CMD_MONITOR:
        monitor();
        break;
    default:
        usage();
        return EX_USAGE;
    };

    return 0;
}
