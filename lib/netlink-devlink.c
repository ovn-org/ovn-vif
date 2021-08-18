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
#include <linux/devlink.h>
#include <linux/genetlink.h>
#include "netlink.h"
#include "netlink-socket.h"
#include "netlink-devlink.h"
#include "openvswitch/vlog.h"
#include "packets.h"

VLOG_DEFINE_THIS_MODULE(netlink_devlink);

/* Initialized by nl_devlink_init() */
static int ovs_devlink_family;

struct nl_dl_dump_state {
    struct nl_dump dump;
    struct ofpbuf buf;
    int error;
};

static int nl_devlink_init(void);

const char *dl_str_not_present = "";

/* Allocates memory for and returns a pointer to devlink dump state object.
 *
 * One-time initialization and lookup of the devlink generic netlink family is
 * also performed, and the caller should check for error condition with a call
 * to nl_dl_dump_init_error before attempting to dump devlink data.
 *
 * The caller owns the allocated object and is responsible for freeing the
 * allocated memory with a call to nl_dl_dump_destroy when done. */
struct nl_dl_dump_state *
nl_dl_dump_init(void)
{
    struct nl_dl_dump_state *dump_state;

    dump_state = xmalloc(sizeof(*dump_state));
    dump_state->error = nl_devlink_init();
    return dump_state;
}

/* Get error indicator from the devlink initialization process. */
int
nl_dl_dump_init_error(struct nl_dl_dump_state *dump_state)
{
    return dump_state->error;
}

/* Free memory previously allocated by call to nl_dl_dump_init.
 *
 * Note that the caller is responsible for making a call to nl_dl_dump_finish
 * to free up resources associated with any in-flight dump process prior to
 * destroying the dump state object. */
void
nl_dl_dump_destroy(struct nl_dl_dump_state *dump_state)
{
    free(dump_state);
}

void
nl_msg_put_dlgenmsg(struct ofpbuf *msg, size_t expected_payload,
                    int family, uint8_t cmd, uint32_t flags)
{
    nl_msg_put_genlmsghdr(msg, expected_payload, family,
                          flags, cmd, DEVLINK_GENL_VERSION);
}

/* Starts a Netlink-devlink "dump" operation, by sending devlink request with
 * command 'cmd' to the kernel on a Netlink socket, and initializes 'state'
 * with buffer and dump state. */
void
nl_dl_dump_start(uint8_t cmd, struct nl_dl_dump_state *state)
{
    struct ofpbuf *request;

    request = ofpbuf_new(NLMSG_HDRLEN + GENL_HDRLEN);
    nl_msg_put_dlgenmsg(request, 0, ovs_devlink_family, cmd,
                        NLM_F_REQUEST);
    nl_dump_start(&state->dump, NETLINK_GENERIC, request);
    ofpbuf_delete(request);

    ofpbuf_init(&state->buf, NL_DUMP_BUFSIZE);
}

static bool
nl_dl_dump_next__(struct nl_dl_dump_state *state,
                  bool (*parse_function)(struct ofpbuf *, void *),
                  void *entry)
{
    struct ofpbuf msg;

    if (!nl_dump_next(&state->dump, &msg, &state->buf)) {
        return false;
    }
    if (!parse_function(&msg, entry)) {
        ovs_mutex_lock(&state->dump.mutex);
        state->dump.status = EPROTO;
        ovs_mutex_unlock(&state->dump.mutex);
        return false;
    }
    return true;
}

/* Attempts to retrieve and parse another reply in on-going dump operation.
 *
 * If successful, returns true and assignes values or pointers to data in
 * 'port_entry'.  The caller must not modify 'port_entry' (because it may
 * contain pointers to data within the buffer which will be used by future
 * calls to this function.
 *
 * On failure, returns false.  Failure might indicate an actual error or merely
 * the end of replies.  An error status for the entire dump operation is
 * provided when it is completed by calling nl_dl_dump_finish()
 */
bool
nl_dl_port_dump_next(struct nl_dl_dump_state *state,
                     struct dl_port *port_entry)
{
    return nl_dl_dump_next__(
        state,
        (bool ( * )(struct ofpbuf *, void *)) &nl_dl_parse_port_policy,
        (void *) port_entry);
}

bool
nl_dl_info_dump_next(struct nl_dl_dump_state *state,
                     struct dl_info *info_entry)
{
    return nl_dl_dump_next__(
        state,
        (bool ( * )(struct ofpbuf *, void *)) &nl_dl_parse_info_policy,
        (void *) info_entry);
}

int
nl_dl_dump_finish(struct nl_dl_dump_state *state)
{
    ofpbuf_uninit(&state->buf);
    return nl_dump_done(&state->dump);
}

static uint64_t
attr_get_up_to_u64(size_t attr_idx, struct nlattr *attrs[],
                   const struct nl_policy policy[],
                   size_t policy_len)
{
    if (attr_idx < policy_len && attrs[attr_idx]) {
        switch (policy[attr_idx].type) {
        case NL_A_U8:
            return nl_attr_get_u8(attrs[attr_idx]);
            break;
        case NL_A_U16:
            return nl_attr_get_u16(attrs[attr_idx]);
            break;
        case NL_A_U32:
            return nl_attr_get_u32(attrs[attr_idx]);
            break;
        case NL_A_U64:
            return nl_attr_get_u64(attrs[attr_idx]);
            break;
        case NL_A_U128:
        case NL_A_STRING:
        case NL_A_NO_ATTR:
        case NL_A_UNSPEC:
        case NL_A_FLAG:
        case NL_A_IPV6:
        case NL_A_NESTED:
        case NL_A_LL_ADDR:
        case N_NL_ATTR_TYPES: default: OVS_NOT_REACHED();
        };
    }
    return -1;
}

static const char *
attr_get_str(size_t attr_idx, struct nlattr *attrs[],
             const struct nl_policy policy[],
             size_t policy_len)
{
    if (attr_idx < policy_len && attrs[attr_idx]) {
        ovs_assert(policy[attr_idx].type == NL_A_STRING);
        return nl_attr_get_string(attrs[attr_idx]);
    }
    return dl_str_not_present;
}

bool
nl_dl_parse_port_function(struct nlattr *nla, struct dl_port_function *port_fn)
{
    static const struct nl_policy policy[] = {
        /* Appeared in Linux v5.9 */
        [DEVLINK_PORT_FUNCTION_ATTR_UNSPEC] = { .type = NL_A_UNSPEC,
                                                .optional = true, },
        [DEVLINK_PORT_FUNCTION_ATTR_HW_ADDR] = { .type = NL_A_LL_ADDR,
                                                 .optional = true, },

        /* Appeared in Linnux v5.12 */
        [DEVLINK_PORT_FN_ATTR_STATE] = { .type = NL_A_U8, .optional = true, },
        [DEVLINK_PORT_FN_ATTR_OPSTATE] = { .type = NL_A_U8,
                                           .optional = true, },
    };
    struct nlattr *attrs[ARRAY_SIZE(policy)];
    bool parsed;

    parsed = nl_parse_nested(nla, policy, attrs, ARRAY_SIZE(policy));

    if (parsed) {
        if (attrs[DEVLINK_PORT_FUNCTION_ATTR_HW_ADDR]) {
            size_t hw_addr_size = nl_attr_get_size(
                            attrs[DEVLINK_PORT_FUNCTION_ATTR_HW_ADDR]);
            if (hw_addr_size == sizeof(struct eth_addr)) {
                port_fn->eth_addr = nl_attr_get_eth_addr(
                                attrs[DEVLINK_PORT_FUNCTION_ATTR_HW_ADDR]);
            } else if (hw_addr_size == sizeof(struct ib_addr)) {
                port_fn->ib_addr = nl_attr_get_ib_addr(
                                attrs[DEVLINK_PORT_FUNCTION_ATTR_HW_ADDR]);
            } else {
                return false;
            }
        } else {
            memset(&port_fn->eth_addr, 0, sizeof(port_fn->eth_addr));
            memset(&port_fn->ib_addr, 0, sizeof(port_fn->ib_addr));
        }
        port_fn->state = attr_get_up_to_u64(
                        DEVLINK_PORT_FN_ATTR_STATE,
                        attrs, policy, ARRAY_SIZE(policy));
        port_fn->opstate = attr_get_up_to_u64(
                        DEVLINK_PORT_FN_ATTR_OPSTATE,
                        attrs, policy, ARRAY_SIZE(policy));
    }

    return parsed;
}

bool
nl_dl_parse_port_policy(struct ofpbuf *msg, struct dl_port *port)
{
    static const struct nl_policy policy[] = {
        /* Appeared in Linux v4.6 */
        [DEVLINK_ATTR_BUS_NAME] = { .type = NL_A_STRING, .optional = false, },
        [DEVLINK_ATTR_DEV_NAME] = { .type = NL_A_STRING, .optional = false, },
        [DEVLINK_ATTR_PORT_INDEX] = { .type = NL_A_U32, .optional = false, },

        [DEVLINK_ATTR_PORT_TYPE] = { .type = NL_A_U16, .optional = true, },
        [DEVLINK_ATTR_PORT_DESIRED_TYPE] = { .type = NL_A_U16,
                                            .optional = true, },
        [DEVLINK_ATTR_PORT_NETDEV_IFINDEX] = { .type = NL_A_U32,
                                               .optional = true, },
        [DEVLINK_ATTR_PORT_NETDEV_NAME] = { .type = NL_A_STRING,
                                            .optional = true, },
        [DEVLINK_ATTR_PORT_IBDEV_NAME] = { .type = NL_A_STRING,
                                           .optional = true, },
        [DEVLINK_ATTR_PORT_SPLIT_COUNT] = { .type = NL_A_U32,
                                            .optional = true, },
        [DEVLINK_ATTR_PORT_SPLIT_GROUP] = { .type = NL_A_U32,
                                            .optional = true, },

        /* Appeared in Linux v4.18 */
        [DEVLINK_ATTR_PORT_FLAVOUR] = { .type = NL_A_U16, .optional = true, },
        [DEVLINK_ATTR_PORT_NUMBER] = { .type = NL_A_U32, .optional = true, },
        [DEVLINK_ATTR_PORT_SPLIT_SUBPORT_NUMBER] = { .type = NL_A_U32,
                                                     .optional = true, },

        /* Appeared in Linux v5.3 */
        [DEVLINK_ATTR_PORT_PCI_PF_NUMBER] = { .type = NL_A_U16,
                                              .optional = true, },
        [DEVLINK_ATTR_PORT_PCI_VF_NUMBER] = { .type = NL_A_U16,
                                              .optional = true, },

        /* Appeared in Linux v5.9 */
        [DEVLINK_ATTR_PORT_FUNCTION] = { .type = NL_A_NESTED,
                                         .optional = true, },
        [DEVLINK_ATTR_PORT_LANES] = { .type = NL_A_U32, .optional = true, },
        [DEVLINK_ATTR_PORT_SPLITTABLE] = { .type = NL_A_U8,
                                           .optional = true, },

        /* Appeared in Linux v5.10 */
        [DEVLINK_ATTR_PORT_EXTERNAL] = { .type = NL_A_U8, .optional = true },
        [DEVLINK_ATTR_PORT_CONTROLLER_NUMBER] = { .type = NL_A_U32,
                                                  .optional = true},

        /* Appeared in Linux v5.12 */
        [DEVLINK_ATTR_PORT_PCI_SF_NUMBER] = { .type = NL_A_U32,
                                              .optional = true },
    };
    struct nlattr *attrs[ARRAY_SIZE(policy)];

    if (!nl_policy_parse(msg, NLMSG_HDRLEN + GENL_HDRLEN,
                         policy, attrs,
                         ARRAY_SIZE(policy)))
    {
        return false;
    }
    port->bus_name = nl_attr_get_string(attrs[DEVLINK_ATTR_BUS_NAME]);
    port->dev_name = nl_attr_get_string(attrs[DEVLINK_ATTR_DEV_NAME]);
    port->index = nl_attr_get_u32(attrs[DEVLINK_ATTR_PORT_INDEX]);

    port->type = attr_get_up_to_u64(
                    DEVLINK_ATTR_PORT_TYPE,
                    attrs, policy, ARRAY_SIZE(policy));
    port->desired_type = attr_get_up_to_u64(
                    DEVLINK_ATTR_PORT_DESIRED_TYPE,
                    attrs, policy, ARRAY_SIZE(policy));
    port->netdev_ifindex = attr_get_up_to_u64(
                    DEVLINK_ATTR_PORT_NETDEV_IFINDEX,
                    attrs, policy, ARRAY_SIZE(policy));
    if (port->type == DEVLINK_PORT_TYPE_ETH &&
            attrs[DEVLINK_ATTR_PORT_NETDEV_NAME]) {
        port->netdev_name = nl_attr_get_string(
            attrs[DEVLINK_ATTR_PORT_NETDEV_NAME]);
    } else if (port->type == DEVLINK_PORT_TYPE_IB &&
            attrs[DEVLINK_ATTR_PORT_IBDEV_NAME]) {
        port->ibdev_name = nl_attr_get_string(
            attrs[DEVLINK_ATTR_PORT_IBDEV_NAME]);
    } else {
        port->netdev_name = dl_str_not_present;
    }
    port->split_count = attr_get_up_to_u64(
                    DEVLINK_ATTR_PORT_SPLIT_COUNT,
                    attrs, policy, ARRAY_SIZE(policy));
    port->split_group = attr_get_up_to_u64(
                    DEVLINK_ATTR_PORT_SPLIT_GROUP,
                    attrs, policy, ARRAY_SIZE(policy));
    port->flavour = attr_get_up_to_u64(
                    DEVLINK_ATTR_PORT_FLAVOUR,
                    attrs, policy, ARRAY_SIZE(policy));
    port->number = attr_get_up_to_u64(
                    DEVLINK_ATTR_PORT_NUMBER,
                    attrs, policy, ARRAY_SIZE(policy));
    port->split_subport_number = attr_get_up_to_u64(
                    DEVLINK_ATTR_PORT_SPLIT_SUBPORT_NUMBER,
                    attrs, policy, ARRAY_SIZE(policy));
    port->pci_pf_number = attr_get_up_to_u64(
                    DEVLINK_ATTR_PORT_PCI_PF_NUMBER,
                    attrs, policy, ARRAY_SIZE(policy));
    port->pci_vf_number = attr_get_up_to_u64(
                    DEVLINK_ATTR_PORT_PCI_VF_NUMBER,
                    attrs, policy, ARRAY_SIZE(policy));
    port->lanes = attr_get_up_to_u64(
                    DEVLINK_ATTR_PORT_LANES,
                    attrs, policy, ARRAY_SIZE(policy));
    port->splittable = attr_get_up_to_u64(
                    DEVLINK_ATTR_PORT_SPLITTABLE,
                    attrs, policy, ARRAY_SIZE(policy));
    port->external = attr_get_up_to_u64(
                    DEVLINK_ATTR_PORT_EXTERNAL,
                    attrs, policy, ARRAY_SIZE(policy));
    port->controller_number = attr_get_up_to_u64(
                    DEVLINK_ATTR_PORT_CONTROLLER_NUMBER,
                    attrs, policy, ARRAY_SIZE(policy));
    port->pci_sf_number = attr_get_up_to_u64(
                    DEVLINK_ATTR_PORT_PCI_SF_NUMBER,
                    attrs, policy, ARRAY_SIZE(policy));

    if (attrs[DEVLINK_ATTR_PORT_FUNCTION]) {
        if (!nl_dl_parse_port_function(attrs[DEVLINK_ATTR_PORT_FUNCTION],
                                       &port->function))
        {
            return false;
        }
    } else {
        memset(&port->function, 0, sizeof(port->function));
        port->function.state = UINT8_MAX;
        port->function.opstate = UINT8_MAX;
    }

    return true;
}

bool
nl_dl_parse_info_version(struct nlattr *nla, struct dl_info_version *info_ver)
{
    static const struct nl_policy policy[] = {
        /* Appeared in Linux v5.1 */
        [DEVLINK_ATTR_INFO_VERSION_NAME] = { .type = NL_A_STRING,
                                             .optional = true, },
        [DEVLINK_ATTR_INFO_VERSION_VALUE] = { .type = NL_A_STRING,
                                              .optional = true, },
    };
    struct nlattr *attrs[ARRAY_SIZE(policy)];
    bool parsed;

    parsed = nl_parse_nested(nla, policy, attrs, ARRAY_SIZE(policy));

    if (parsed) {
        info_ver->name = attr_get_str(
                        DEVLINK_ATTR_INFO_VERSION_NAME,
                        attrs, policy, ARRAY_SIZE(policy));
        info_ver->value = attr_get_str(
                        DEVLINK_ATTR_INFO_VERSION_NAME,
                        attrs, policy, ARRAY_SIZE(policy));
    }

    return parsed;
}

static bool
attr_fill_version(size_t attr_idx, struct nlattr *attrs[],
                  size_t attrs_len,
                  struct dl_info_version *version)
{
    if (attr_idx < attrs_len && attrs[attr_idx]) {
        if (!nl_dl_parse_info_version(attrs[attr_idx],
                                      version))
        {
            return false;
        }
    } else {
        version->name = dl_str_not_present;
        version->value = dl_str_not_present;
    }
    return true;
}

bool
nl_dl_parse_info_policy(struct ofpbuf *msg, struct dl_info *info)
{
    static const struct nl_policy policy[] = {
        /* Appeared in Linux v5.1 */
        [DEVLINK_ATTR_INFO_DRIVER_NAME] = { .type = NL_A_STRING,
                                            .optional = false, },
        [DEVLINK_ATTR_INFO_SERIAL_NUMBER] = { .type = NL_A_STRING,
                                              .optional = true, },
        [DEVLINK_ATTR_INFO_VERSION_FIXED] = { .type = NL_A_NESTED,
                                              .optional = true, },
        [DEVLINK_ATTR_INFO_VERSION_RUNNING] = { .type = NL_A_NESTED,
                                                .optional = true, },
        [DEVLINK_ATTR_INFO_VERSION_STORED] = { .type = NL_A_NESTED,
                                               .optional = true, },

        /* Appeared in Linux v5.9 */
        [DEVLINK_ATTR_INFO_BOARD_SERIAL_NUMBER] = { .type = NL_A_STRING,
                                                    .optional = true, },
    };
    struct nlattr *attrs[ARRAY_SIZE(policy)];

    if (!nl_policy_parse(msg, NLMSG_HDRLEN + GENL_HDRLEN,
                         policy, attrs,
                         ARRAY_SIZE(policy)))
    {
        return false;
    }
    info->driver_name = attr_get_str(
                    DEVLINK_ATTR_INFO_DRIVER_NAME,
                    attrs, policy, ARRAY_SIZE(policy));
    info->serial_number = attr_get_str(
                    DEVLINK_ATTR_INFO_SERIAL_NUMBER,
                    attrs, policy, ARRAY_SIZE(policy));
    info->board_serial_number = attr_get_str(
                    DEVLINK_ATTR_INFO_BOARD_SERIAL_NUMBER,
                    attrs, policy, ARRAY_SIZE(policy));
    if (!attr_fill_version(DEVLINK_ATTR_INFO_VERSION_FIXED, attrs,
                           ARRAY_SIZE(policy), &info->version_fixed)
        || !attr_fill_version(DEVLINK_ATTR_INFO_VERSION_RUNNING, attrs,
                              ARRAY_SIZE(policy), &info->version_running)
        || !attr_fill_version(DEVLINK_ATTR_INFO_VERSION_STORED, attrs,
                              ARRAY_SIZE(policy), &info->version_stored))
    {
        return false;
    }

    return true;
}

static int
nl_devlink_init(void)
{
    static struct ovsthread_once once = OVSTHREAD_ONCE_INITIALIZER;
    static int error;

    if (ovsthread_once_start(&once)) {
        error = nl_lookup_genl_family(DEVLINK_GENL_NAME, &ovs_devlink_family);
        if (error) {
            VLOG_INFO("Generic Netlink family '%s' does not exist. "
                      "Linux version 4.6 or newer required.",
                      DEVLINK_GENL_NAME);
        }
        ovsthread_once_done(&once);
    }
    return error;
}
