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

#ifndef NETLINK_DEVLINK_H
#define NETLINK_DEVLINK_H 1

/* Presence of each individual value in these structs is determined at runtime
 * and depends on which kernel version we are communicating with as well as
 * which driver implementation is filling in the information for each
 * individual device or port.
 *
 * To signal non-presence of values the library follows the following
 * convention:
 *
 * - integer type values will be set to their maximum value
 *   (e.g. UNIT8_MAX for a unit8_t)
 *
 * - hardware address type values will be set to all zero
 *
 * - string type values will be set to a pointer to dl_str_not_present
 *   (an empty string).
 */

extern const char *dl_str_not_present;

struct dl_port_function {
    struct eth_addr eth_addr;
    struct ib_addr ib_addr;
    uint8_t state;
    uint8_t opstate;
};

struct dl_port {
    const char *bus_name;
    const char *dev_name;
    uint32_t index;
    uint16_t type;
    uint16_t desired_type;
    uint32_t netdev_ifindex;
    union {
        const char *netdev_name; /* type DEVLINK_PORT_TYPE_ETH */
        const char *ibdev_name;  /* type DEVLINK_PORT_TYPE_IB */
    };
    uint32_t split_count;
    uint32_t split_group;
    uint16_t flavour;
    uint32_t number;
    uint32_t split_subport_number;
    uint16_t pci_pf_number;
    uint16_t pci_vf_number;
    struct dl_port_function function;
    uint32_t lanes;
    uint8_t splittable;
    uint8_t external;
    uint32_t controller_number;
    uint32_t pci_sf_number;
};

struct dl_info_version {
    const char *name;
    const char *value;
};

struct dl_info {
    const char *driver_name;
    const char *serial_number;
    const char *board_serial_number;
    struct dl_info_version version_fixed;
    struct dl_info_version version_running;
    struct dl_info_version version_stored;
};

struct eth_addr nl_attr_get_eth_addr(const struct nlattr *nla);
struct ib_addr nl_attr_get_ib_addr(const struct nlattr *nla);

/* The nl_dl_dump_state record declaration refers to types declared in
 * netlink-socket.h, which requires OVS internal autoconf macros and
 * definitions to be present for successful compilation.
 *
 * To enable friction free consumtion of these interfaces from programs
 * external to Open vSwitch, such as OVN, we keep the declaration of
 * nl_dl_dump_state private.
 *
 * Use the nl_dl_dump_init function to allocate memory for and get a pointer to
 * a devlink dump state object. The caller owns the allocated object and is
 * responsible for freeing the allocated memory when done. */
struct nl_dl_dump_state;

struct nl_dl_dump_state * nl_dl_dump_init(void);
int nl_dl_dump_init_error(struct nl_dl_dump_state *);
void nl_dl_dump_destroy(struct nl_dl_dump_state *);
void nl_msg_put_dlgenmsg(struct ofpbuf *, size_t, int, uint8_t, uint32_t);
void nl_dl_dump_start(uint8_t, struct nl_dl_dump_state *);
bool nl_dl_port_dump_next(struct nl_dl_dump_state *, struct dl_port *);
bool nl_dl_info_dump_next(struct nl_dl_dump_state *, struct dl_info *);
int nl_dl_dump_finish(struct nl_dl_dump_state *);
bool nl_dl_parse_port_policy(struct ofpbuf *, struct dl_port *);
bool nl_dl_parse_port_function(struct nlattr *, struct dl_port_function *);
bool nl_dl_parse_info_policy(struct ofpbuf *, struct dl_info *);
bool nl_dl_parse_info_version(struct nlattr *, struct dl_info_version *);

#endif /* NETLINK_DEVLINK_H */
