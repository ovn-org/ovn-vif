/* Copyright (c) 2021 Canonical
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
#include <net/if.h>

#ifdef HAVE_UDEV
#include <libudev.h>
#endif /* HAVE_UDEV */

#include "vif-plug-provider.h"

#include "hash.h"
#include "openvswitch/hmap.h"
#include "openvswitch/vlog.h"
#include "netlink.h"
#include "netlink-socket.h"
#include "netlink-devlink.h"
#include "packets.h"
#include "random.h"
#include "timeval.h"
#include "openvswitch/shash.h"

VLOG_DEFINE_THIS_MODULE(vif_plug_representor);

enum port_node_source {
    PORT_NODE_SOURCE_DUMP,
    PORT_NODE_SOURCE_RUNTIME,
};

struct port_node {
    struct hmap_node mac_vf_node;
    struct hmap_node ifindex_node;
    struct hmap_node bus_dev_node;
    char *bus_name;
    char *dev_name;
    uint32_t port_index;
    uint32_t netdev_ifindex;
    char *netdev_name;
    bool netdev_renamed;
    /* Which attribute is stored here depends on the value of 'flavour'.
     *
     * Flavour:                       Devlink attrbiute:
     * DEVLINK_PORT_FLAVOUR_PHYSICAL  DEVLINK_ATTR_PORT_NUMBER
     * DEVLINK_PORT_FLAVOUR_PCI_PF    DEVLINK_ATTR_PORT_PCI_PF_NUMBER
     * DEVLINK_PORT_FLAVOUR_PCI_VF    DEVLINK_ATTR_PORT_PCI_VF_NUMBER */
    uint32_t number;
    uint16_t flavour;
    struct eth_addr mac;
    bool vf_mac_programming_pending;
    struct eth_addr vf_mac_requested;
    long long int vf_mac_requested_at;
    struct port_node *pf;
    enum port_node_source port_node_source;
};

enum {
    VF_MAC_CONFIRM_TIMEOUT_MS = 5000,
};

/* Port table.
 *
 * This data structure contains three indexes:
 *
 * mac_vf_table   - port_node by PF MAC and VF number.
 * ifindex_table  - port_node by netdev ifindex.
 * bus_dev_table  - port_node by bus/dev name (only contains PHYSICAL and
 *                  PCI_PF ports).
 *
 * There is a small number of PHYSICAL and PF flavoured ports per device.  We
 * will need to refer to these for every update we get to a VF in order to
 * maintain the PF MAC+VF number index.
 *
 * Note that there is not really any association between PHYSICAL and PF
 * representor ports from the devlink data structure point of view.  However
 * for systems running a kernel that does not provide the host facing MAC
 * through devlink on the PF representor there is a compatibility interface in
 * sysfs which is relative to a PHYSICAL ports netdev name (see the
 * compat_get_host_pf_mac function).
 */
struct port_table {
    struct hmap mac_vf_table; /* Hash table for lookups by mac+vf_num */
    uint32_t mac_seed; /* We reuse the OVS mac+vlan hash functions for the
                        * PF MAC+VF number, and they require a uint32_t seed */
    struct hmap ifindex_table; /* Hash table for lookups by ifindex */
    struct hmap bus_dev_table; /* Hash table for lookup of PHYSICAL and PF
                                * ports by their bus_name/dev_name string.
                                * While there is a large number of VFs or SFs
                                * they will be associated with a small number
                                * of PFs */
};

static struct port_table *port_table;

static struct port_node *
port_node_create(const char *bus_name, const char *dev_name,
                 uint32_t port_index, uint32_t netdev_ifindex,
                 const char *netdev_name,
                 uint32_t number, uint16_t flavour,
                 struct eth_addr mac, struct port_node *pf,
                 enum port_node_source port_node_source)
{
    struct port_node *pn;

    pn = xmalloc(sizeof *pn);
    pn->bus_name = xstrdup(bus_name);
    pn->dev_name = xstrdup(dev_name);
    pn->port_index = port_index;
    pn->netdev_ifindex = netdev_ifindex;
    pn->netdev_name = xstrdup(netdev_name);
    pn->netdev_renamed = false;
    pn->number = number;
    pn->flavour = flavour;
    pn->mac = mac;
    pn->vf_mac_programming_pending = false;
    pn->vf_mac_requested = eth_addr_zero;
    pn->vf_mac_requested_at = 0;
    pn->pf = pf;
    pn->port_node_source = port_node_source;

    return pn;
}

static void
port_node_destroy(struct port_node *pn)
{
    if (pn->bus_name) {
        free(pn->bus_name);
    }
    if (pn->dev_name) {
        free(pn->dev_name);
    }
    if (pn->netdev_name) {
        free(pn->netdev_name);
    }
    free(pn);
}

static void
port_node_update(struct port_node *pn, const char *netdev_name)
{
    if (pn->netdev_name) {
        free (pn->netdev_name);
        pn->netdev_renamed = true;
    }
    pn->netdev_name = xstrdup(netdev_name);
}

static bool
port_node_rename_expected(struct port_node *pn)
{
#ifdef HAVE_UDEV
    return pn->port_node_source == PORT_NODE_SOURCE_RUNTIME
           && pn->netdev_renamed == false;
#else
    return false;
#endif /* HAVE_UDEV */

}

static void
vif_plug_representor_get_hw_addr(const struct port_node *pn,
                                 struct eth_addr *vf_mac, bool *found,
                                 int *error);
static int
vif_plug_representor_set_hw_addr(const struct port_node *pn,
                                 const struct eth_addr *vf_mac);
static bool
vif_plug_representor_request_vf_mac(struct port_node *pn,
                                    const struct eth_addr *requested_mac,
                                    long long int requested_at, int *error);
static void
vif_plug_representor_log_vf_mac_set_error(
    int error, const struct vif_plug_port_ctx_in *ctx_in,
    const char *opt_pf_mac, const char *opt_vf_num, const char *opt_lport_mac);
static void
vif_plug_representor_log_vf_mac_retry_error(
    int error, const char *reason, const struct port_node *pn,
    const struct eth_addr *requested_mac);

static void
port_table_expire_pending_vf_mac_programming(struct port_table *tbl)
{
    if (!tbl) {
        return;
    }
    long long int now = time_msec();
    struct port_node *pn;

    HMAP_FOR_EACH (pn, ifindex_node, &tbl->ifindex_table) {
        if (!pn->vf_mac_programming_pending) {
            continue;
        }
        if (now - pn->vf_mac_requested_at < VF_MAC_CONFIRM_TIMEOUT_MS) {
            continue;
        }
        struct eth_addr current_mac;
        bool found;
        int error;

        vif_plug_representor_get_hw_addr(pn, &current_mac, &found, &error);
        if (error) {
            VLOG_WARN("Unable to validate pending VF MAC request on timeout "
                      "bus=%s dev=%s port-index=%"PRIu32" requested-mac="
                      ETH_ADDR_FMT" after %lld ms (%s).",
                      pn->bus_name, pn->dev_name, pn->port_index,
                      ETH_ADDR_ARGS(pn->vf_mac_requested),
                      now - pn->vf_mac_requested_at, ovs_strerror(error));
            pn->vf_mac_programming_pending = false;
            continue;
        }
        if (!found) {
            VLOG_WARN("Unable to validate pending VF MAC request on timeout "
                      "bus=%s dev=%s port-index=%"PRIu32" requested-mac="
                      ETH_ADDR_FMT" after %lld ms (port not found).",
                      pn->bus_name, pn->dev_name, pn->port_index,
                      ETH_ADDR_ARGS(pn->vf_mac_requested),
                      now - pn->vf_mac_requested_at);
            pn->vf_mac_programming_pending = false;
            continue;
        }
        if (eth_addr_equals(current_mac, pn->vf_mac_requested)) {
            VLOG_INFO("Validated VF MAC request by polling devlink after "
                      "timeout bus=%s dev=%s port-index=%"PRIu32" mac="
                      ETH_ADDR_FMT" after %lld ms.",
                      pn->bus_name, pn->dev_name, pn->port_index,
                      ETH_ADDR_ARGS(current_mac),
                      now - pn->vf_mac_requested_at);
            pn->mac = current_mac;
            pn->vf_mac_programming_pending = false;
            continue;
        }

        VLOG_WARN("No async devlink confirmation for VF MAC request "
                  "bus=%s dev=%s port-index=%"PRIu32" requested-mac="
                  ETH_ADDR_FMT" observed-mac="ETH_ADDR_FMT" after %lld ms; "
                  "retrying.",
                  pn->bus_name, pn->dev_name, pn->port_index,
                  ETH_ADDR_ARGS(pn->vf_mac_requested),
                  ETH_ADDR_ARGS(current_mac),
                  now - pn->vf_mac_requested_at);

        if (vif_plug_representor_request_vf_mac(pn, &pn->vf_mac_requested,
                                                now, &error)) {
            continue;
        }
        vif_plug_representor_log_vf_mac_retry_error(
            error, "timeout request", pn, &pn->vf_mac_requested);
        pn->vf_mac_programming_pending = false;
    }
}

static bool
vif_plug_representor_parse_lport_mac(const char *lport_mac,
                                     struct eth_addr *mac)
{
    char mac_buf[ETH_ADDR_STRLEN + 1];
    size_t mac_len;

    if (!lport_mac) {
        return false;
    }

    mac_len = strcspn(lport_mac, " \t");
    if (!mac_len || mac_len >= sizeof mac_buf) {
        return false;
    }

    memcpy(mac_buf, lport_mac, mac_len);
    mac_buf[mac_len] = '\0';

    return eth_addr_from_string(mac_buf, mac);
}

#ifdef OVSTEST
static int ovstest_set_hw_addr_error;
static size_t ovstest_set_hw_addr_calls;
static const char *ovstest_set_hw_addr_last_bus_name;
static const char *ovstest_set_hw_addr_last_dev_name;
static uint32_t ovstest_set_hw_addr_last_port_index;
static struct eth_addr ovstest_set_hw_addr_last_mac;
static int ovstest_get_hw_addr_error;
static bool ovstest_get_hw_addr_found;
static struct eth_addr ovstest_get_hw_addr_mac;
static size_t ovstest_get_hw_addr_calls;
#endif

/* Indirection that allows OVSTEST to unit test VF MAC programming logic. */
static int
vif_plug_representor_set_hw_addr(const struct port_node *pn,
                                 const struct eth_addr *vf_mac)
{
#ifdef OVSTEST
    ovstest_set_hw_addr_calls++;
    ovstest_set_hw_addr_last_bus_name = pn->bus_name;
    ovstest_set_hw_addr_last_dev_name = pn->dev_name;
    ovstest_set_hw_addr_last_port_index = pn->port_index;
    ovstest_set_hw_addr_last_mac = *vf_mac;
    return ovstest_set_hw_addr_error;
#else
    return nl_dl_port_function_set_hw_addr(
        pn->bus_name, pn->dev_name, pn->port_index, vf_mac);
#endif
}

static void
vif_plug_representor_log_vf_mac_set_error(
    int error, const struct vif_plug_port_ctx_in *ctx_in,
    const char *opt_pf_mac, const char *opt_vf_num, const char *opt_lport_mac)
{
    if (error == EOPNOTSUPP || error == EINVAL) {
        VLOG_INFO("devlink does not support VF MAC programming for "
                  "lport: %s pf-mac: '%s' vf-num: '%s' mac: '%s' "
                  "(%s)",
                  ctx_in->lport_name, opt_pf_mac, opt_vf_num, opt_lport_mac,
                  ovs_strerror(error));
    } else {
        VLOG_WARN("Failed to program VF MAC via devlink for lport: %s "
                  "pf-mac: '%s' vf-num: '%s' mac: '%s' (%s)",
                  ctx_in->lport_name, opt_pf_mac, opt_vf_num, opt_lport_mac,
                  ovs_strerror(error));
    }
}

static void
vif_plug_representor_log_vf_mac_retry_error(
    int error, const char *reason, const struct port_node *pn,
    const struct eth_addr *requested_mac)
{
    if (error == EOPNOTSUPP || error == EINVAL) {
        VLOG_INFO("devlink does not support VF MAC programming while "
                  "retrying %s bus=%s dev=%s port-index=%"PRIu32
                  " requested-mac="ETH_ADDR_FMT" (%s)",
                  reason, pn->bus_name, pn->dev_name, pn->port_index,
                  ETH_ADDR_ARGS(*requested_mac), ovs_strerror(error));
    } else {
        VLOG_WARN("Failed to retry VF MAC programming (%s) bus=%s dev=%s "
                  "port-index=%"PRIu32" requested-mac="ETH_ADDR_FMT" "
                  "(%s)",
                  reason, pn->bus_name, pn->dev_name, pn->port_index,
                  ETH_ADDR_ARGS(*requested_mac), ovs_strerror(error));
    }
}

static bool
vif_plug_representor_request_vf_mac(struct port_node *pn,
                                    const struct eth_addr *requested_mac,
                                    long long int requested_at, int *error)
{
    *error = vif_plug_representor_set_hw_addr(pn, requested_mac);
    if (*error) {
        return false;
    }
    pn->mac = *requested_mac;
    pn->vf_mac_requested = *requested_mac;
    pn->vf_mac_requested_at = requested_at;
    return true;
}

static void
vif_plug_representor_get_hw_addr(const struct port_node *pn,
                                 struct eth_addr *vf_mac, bool *found,
                                 int *error)
{
#ifdef OVSTEST
    (void) pn;
    ovstest_get_hw_addr_calls++;
    *vf_mac = ovstest_get_hw_addr_mac;
    *found = ovstest_get_hw_addr_found;
    *error = ovstest_get_hw_addr_error;
#else
    struct nl_dl_dump_state *port_dump;
    struct dl_port port_entry;

    *vf_mac = eth_addr_zero;
    *found = false;
    *error = 0;

    port_dump = nl_dl_dump_init();
    *error = nl_dl_dump_init_error(port_dump);
    if (*error) {
        nl_dl_dump_destroy(port_dump);
        return;
    }

    nl_dl_dump_start(DEVLINK_CMD_PORT_GET, port_dump);
    while (nl_dl_port_dump_next(port_dump, &port_entry)) {
        if (strcmp(port_entry.bus_name, pn->bus_name)
                || strcmp(port_entry.dev_name, pn->dev_name)
                || port_entry.index != pn->port_index) {
            continue;
        }
        *vf_mac = port_entry.function.eth_addr;
        *found = true;
        break;
    }

    *error = nl_dl_dump_finish(port_dump);
    nl_dl_dump_destroy(port_dump);
#endif
}

static void
vif_plug_representor_program_vf_mac(
        const struct vif_plug_port_ctx_in *ctx_in, struct port_node *pn,
        const char *opt_pf_mac, const char *opt_vf_num)
{
    const char *opt_lport_mac = smap_get(&ctx_in->lport_options, "mac");
    if (!opt_lport_mac) {
        opt_lport_mac = smap_get(&ctx_in->lport_options, "addresses");
    }
    struct eth_addr vf_mac;
    if (!vif_plug_representor_parse_lport_mac(opt_lport_mac, &vf_mac)) {
        if (opt_lport_mac) {
            VLOG_WARN("Unable to parse logical port MAC for lport: %s "
                      "mac: '%s'",
                      ctx_in->lport_name, opt_lport_mac);
        }
        return;
    }

    if (pn->flavour != DEVLINK_PORT_FLAVOUR_PCI_VF) {
        VLOG_INFO("Skipping VF MAC programming for non-VF representor "
                  "lport: %s pf-mac: '%s' vf-num: '%s' mac: '%s'",
                  ctx_in->lport_name, opt_pf_mac, opt_vf_num, opt_lport_mac);
        return;
    }

    if (!pn->bus_name || !pn->dev_name || pn->port_index == UINT32_MAX) {
        VLOG_WARN("Insufficient devlink metadata to program VF MAC for "
                  "lport: %s pf-mac: '%s' vf-num: '%s' mac: '%s'",
                  ctx_in->lport_name, opt_pf_mac, opt_vf_num, opt_lport_mac);
        return;
    }

    if (eth_addr_equals(pn->mac, vf_mac)) {
        return;
    }

    struct eth_addr old_mac = pn->mac;
    int error;
    if (vif_plug_representor_request_vf_mac(pn, &vf_mac, time_msec(),
                                            &error)) {
        pn->vf_mac_programming_pending = true;
        VLOG_INFO("Requested VF MAC programming via devlink for lport: %s "
                  "pf-mac: '%s' vf-num: '%s' old-mac: "ETH_ADDR_FMT
                  " new-mac: "ETH_ADDR_FMT,
                  ctx_in->lport_name, opt_pf_mac, opt_vf_num,
                  ETH_ADDR_ARGS(old_mac), ETH_ADDR_ARGS(vf_mac));
        return;
    }
    vif_plug_representor_log_vf_mac_set_error(error, ctx_in, opt_pf_mac,
                                              opt_vf_num, opt_lport_mac);
}

static struct port_table *
port_table_create(void)
{
    struct port_table *tbl;

    tbl = xmalloc(sizeof *tbl);
    hmap_init(&tbl->mac_vf_table);
    tbl->mac_seed = random_uint32();
    hmap_init(&tbl->ifindex_table);
    hmap_init(&tbl->bus_dev_table);

    return tbl;
}

static void
port_table_destroy(struct port_table *tbl)
{
    struct port_node *port_node;
    HMAP_FOR_EACH_POP (port_node, mac_vf_node, &tbl->mac_vf_table) {
        port_node_destroy(port_node);
    }
    hmap_destroy(&tbl->mac_vf_table);

    /* The PHYSICAL and PF ports are stored in both ifindex_table and
     * bus_dev_table */
    HMAP_FOR_EACH_POP (port_node, bus_dev_node, &tbl->bus_dev_table) {
        port_node_destroy(port_node);
    }
    hmap_destroy(&tbl->bus_dev_table);

    /* All entries in the ifindex table are also in the mac_vf table or
     * bus_dev_table which nodes were destroyed above, so we only
     * need to destroy the hmap data for the ifindex table. */
    hmap_destroy(&tbl->ifindex_table);
    free(tbl);
}

static uint32_t port_table_hash_mac_vf(const struct port_table *tbl,
                                       const struct eth_addr mac,
                                       uint16_t vf_num)
{
    return hash_mac(mac, vf_num, tbl->mac_seed);
}

static struct port_node *
port_table_lookup_ifindex(struct port_table *tbl, uint32_t netdev_ifindex)
{
    struct port_node *pn;

    HMAP_FOR_EACH_WITH_HASH (pn, ifindex_node, netdev_ifindex,
                            &tbl->ifindex_table) {
        if (pn->netdev_ifindex == netdev_ifindex) {
            return pn;
        }
    }
    return NULL;
}

static struct port_node *
port_table_lookup_pf_mac_vf(struct port_table *tbl, struct eth_addr mac,
                            uint16_t vf_num)
{
    struct port_node *pn;

    HMAP_FOR_EACH_WITH_HASH (pn, mac_vf_node,
                             port_table_hash_mac_vf(tbl, mac, vf_num),
                             &tbl->mac_vf_table) {
        if (pn->number == vf_num && pn->pf
            && eth_addr_equals(pn->pf->mac, mac)) {
            return pn;
        }
    }
    return NULL;
}

static uint32_t
hash_bus_dev(const char *bus_name, const char *dev_name)
{
    char bus_dev[128];

    if (snprintf(bus_dev, sizeof(bus_dev),
                 "%s/%s", bus_name, dev_name) >= sizeof(bus_dev)) {
        VLOG_WARN("bus_dev key buffer overflow, bus_name=%s dev_name=%s",
                  bus_name, dev_name);
    }
    return hash_string(bus_dev, 0);
}

static struct port_node *
port_table_lookup_phy_bus_dev(struct port_table *tbl,
                              const char *bus_name, const char *dev_name,
                              uint16_t flavour, uint32_t number)
{
    struct port_node *pn;
    HMAP_FOR_EACH_WITH_HASH (pn, bus_dev_node,
                             hash_bus_dev(bus_name, dev_name),
                             &tbl->bus_dev_table) {
       if (pn->flavour == flavour && pn->number == number) {
           return pn;
       }
    }
    return NULL;
}

static struct port_node *
port_table_update_phy__(struct port_table *tbl,
                        const char *bus_name, const char *dev_name,
                        uint32_t port_index, uint32_t netdev_ifindex,
                        const char *netdev_name,
                        uint32_t number, uint16_t flavour,
                        struct eth_addr mac,
                        enum port_node_source port_node_source)
{
    struct port_node *pn;

    pn = port_table_lookup_phy_bus_dev(tbl, bus_name, dev_name,
                                       flavour, number);
    if (!pn) {
        pn = port_node_create(bus_name, dev_name, port_index,
                              netdev_ifindex, netdev_name,
                              number, flavour, mac, NULL, port_node_source);
        hmap_insert(&tbl->ifindex_table, &pn->ifindex_node, netdev_ifindex);
        hmap_insert(&tbl->bus_dev_table, &pn->bus_dev_node,
                    hash_bus_dev(bus_name, dev_name));
    } else {
        port_node_update(pn, netdev_name);
    }

    return pn;
}

static struct port_node *
port_table_update_function__(struct port_table *tbl, struct port_node *pf,
                             const char *bus_name, const char *dev_name,
                             uint32_t port_index, uint32_t netdev_ifindex,
                             const char *netdev_name,
                             uint32_t number, uint16_t flavour,
                             struct eth_addr mac,
                             enum port_node_source port_node_source)
{
    struct port_node *pn = port_table_lookup_ifindex(tbl, netdev_ifindex);

    if (!pn) {
        pn = port_node_create(
            bus_name, dev_name, port_index, netdev_ifindex, netdev_name,
            number, flavour, mac, pf,
            port_node_source);
        hmap_insert(&tbl->ifindex_table, &pn->ifindex_node, netdev_ifindex);
        hmap_insert(&tbl->mac_vf_table, &pn->mac_vf_node,
                    port_table_hash_mac_vf(tbl, pf->mac, number));
    } else {
        struct eth_addr old_mac = pn->mac;
        bool was_pending = pn->vf_mac_programming_pending;
        struct eth_addr requested_mac = pn->vf_mac_requested;
        long long int request_age_ms = time_msec() - pn->vf_mac_requested_at;

        pn->port_index = port_index;
        pn->mac = mac;
        port_node_update(pn, netdev_name);

        if (was_pending && flavour == DEVLINK_PORT_FLAVOUR_PCI_VF) {
            bool keep_pending = false;
            if (eth_addr_is_zero(mac)) {
                VLOG_WARN("Received async devlink update without VF MAC "
                          "for pending request bus=%s dev=%s port-index=%"
                          PRIu32" requested-mac="ETH_ADDR_FMT,
                          pn->bus_name, pn->dev_name, pn->port_index,
                          ETH_ADDR_ARGS(requested_mac));
            } else if (eth_addr_equals(mac, requested_mac)) {
                VLOG_INFO("Confirmed async devlink VF MAC update "
                          "bus=%s dev=%s port-index=%"PRIu32" mac="
                          ETH_ADDR_FMT" in %lld ms.",
                          pn->bus_name, pn->dev_name, pn->port_index,
                          ETH_ADDR_ARGS(mac), request_age_ms);
            } else {
                VLOG_WARN("Async devlink VF MAC update mismatch "
                          "bus=%s dev=%s port-index=%"PRIu32
                          " requested-mac="ETH_ADDR_FMT" observed-mac="
                          ETH_ADDR_FMT" after %lld ms; retrying.",
                          pn->bus_name, pn->dev_name, pn->port_index,
                          ETH_ADDR_ARGS(requested_mac),
                          ETH_ADDR_ARGS(mac), request_age_ms);
                int error;
                if (vif_plug_representor_request_vf_mac(
                        pn, &requested_mac, time_msec(), &error)) {
                    keep_pending = true;
                } else {
                    vif_plug_representor_log_vf_mac_retry_error(
                        error, "async mismatch", pn, &requested_mac);
                }
            }
            pn->vf_mac_programming_pending = keep_pending;
        } else if (flavour == DEVLINK_PORT_FLAVOUR_PCI_VF
                   && !eth_addr_equals(old_mac, mac)) {
            VLOG_INFO("Observed devlink VF MAC change "
                      "bus=%s dev=%s port-index=%"PRIu32" old-mac="
                      ETH_ADDR_FMT" new-mac="ETH_ADDR_FMT,
                      pn->bus_name, pn->dev_name, pn->port_index,
                      ETH_ADDR_ARGS(old_mac), ETH_ADDR_ARGS(mac));
        }
    }
    return pn;
}

/* Inserts or updates an entry in the table. */
static struct port_node *
port_table_update_entry(struct port_table *tbl,
                        const char *bus_name, const char *dev_name,
                        uint32_t port_index, uint32_t netdev_ifindex,
                        const char *netdev_name,
                        uint32_t number, uint16_t pci_pf_number,
                        uint16_t pci_vf_number, uint16_t flavour,
                        struct eth_addr mac,
                        enum port_node_source port_node_source)
{
    if (flavour == DEVLINK_PORT_FLAVOUR_PHYSICAL
            || flavour == DEVLINK_PORT_FLAVOUR_PCI_PF) {
        return port_table_update_phy__(
            tbl, bus_name, dev_name, port_index, netdev_ifindex, netdev_name,
            flavour == DEVLINK_PORT_FLAVOUR_PHYSICAL ? number : pci_pf_number,
            flavour, mac, port_node_source);
    }

    struct port_node *phy;
    phy = port_table_lookup_phy_bus_dev(tbl, bus_name, dev_name,
                                        DEVLINK_PORT_FLAVOUR_PCI_PF,
                                        pci_pf_number);
    if (!phy) {
        VLOG_WARN("attempt to add function before having knowledge about PF");
        return NULL;
    }
    return port_table_update_function__(tbl, phy, bus_name, dev_name,
                                        port_index, netdev_ifindex,
                                        netdev_name,
                                        pci_vf_number, flavour, mac,
                                        port_node_source);
}

static void
port_table_delete_phy__(struct port_table *tbl,
                        const char *bus_name, const char *dev_name,
                        uint32_t number, uint16_t flavour)
{
    struct port_node *phy;

    phy = port_table_lookup_phy_bus_dev(tbl, bus_name, dev_name,
                                        flavour, number);
    if (!phy) {
        VLOG_WARN("attempt to remove non-existing device %s/%s %d",
                  bus_name, dev_name, number);
        return;
    }
    hmap_remove(&tbl->ifindex_table, &phy->ifindex_node);
    hmap_remove(&tbl->bus_dev_table, &phy->bus_dev_node);
    port_node_destroy(phy);
}

static void
port_table_delete_function__(struct port_table *tbl, struct port_node *pf,
                             uint16_t pci_vf_number)
{
    struct port_node *pn;

    pn = port_table_lookup_pf_mac_vf(tbl, pf->mac, pci_vf_number);
    if (!pn) {
        VLOG_WARN("attempt to remove non-existing function %s-%d",
                  pf->netdev_name, pci_vf_number);
        return;
    }
    hmap_remove(&tbl->ifindex_table, &pn->ifindex_node);
    hmap_remove(&tbl->mac_vf_table, &pn->mac_vf_node);
    port_node_destroy(pn);
}

static void
port_table_delete_entry(struct port_table *tbl,
                        const char *bus_name, const char *dev_name,
                        uint32_t number, uint16_t pci_pf_number,
                        uint16_t pci_vf_number, uint16_t flavour)
{
    if (flavour == DEVLINK_PORT_FLAVOUR_PHYSICAL
            || flavour == DEVLINK_PORT_FLAVOUR_PCI_PF) {
        port_table_delete_phy__(
            tbl, bus_name, dev_name,
            flavour == DEVLINK_PORT_FLAVOUR_PHYSICAL ? number : pci_pf_number,
            flavour);
    } else {
        struct port_node *phy;

        phy = port_table_lookup_phy_bus_dev(tbl, bus_name, dev_name,
                                            DEVLINK_PORT_FLAVOUR_PCI_PF,
                                            pci_pf_number);
        if (!phy) {
            VLOG_WARN("attempt to remove function with non-existing PF "
                      "bus_dev %s/%s pci_pf_number %d",
                      bus_name, dev_name, pci_pf_number);
            return;
        }
        port_table_delete_function__(tbl, phy, pci_vf_number);
    }
}

static struct nl_sock *devlink_monitor_sock;

#ifdef HAVE_UDEV
static struct udev *udev;
static struct udev_monitor *udev_monitor;
#endif /* HAVE_UDEV */

static bool compat_get_host_pf_mac(const char *, struct eth_addr *);

static void
port_table_update_devlink_port(struct dl_port *port_entry,
                               enum port_node_source port_node_source)
{
    if (port_entry->flavour != DEVLINK_PORT_FLAVOUR_PHYSICAL
            && port_entry->flavour != DEVLINK_PORT_FLAVOUR_PCI_PF
            && port_entry->flavour != DEVLINK_PORT_FLAVOUR_PCI_VF) {
        VLOG_WARN("Unsupported flavour for port '%s': %s",
            port_entry->netdev_name,
            port_entry->flavour == DEVLINK_PORT_FLAVOUR_CPU ? "CPU" :
            port_entry->flavour == DEVLINK_PORT_FLAVOUR_DSA ? "DSA" :
            port_entry->flavour == DEVLINK_PORT_FLAVOUR_PCI_PF ? "PCI_PF":
            port_entry->flavour == DEVLINK_PORT_FLAVOUR_PCI_VF ? "PCI_VF":
            port_entry->flavour == DEVLINK_PORT_FLAVOUR_VIRTUAL ? "VIRTUAL":
            port_entry->flavour == DEVLINK_PORT_FLAVOUR_UNUSED ? "UNUSED":
            port_entry->flavour == DEVLINK_PORT_FLAVOUR_PCI_SF ? "PCI_SF":
            "UNKNOWN");
        return;
    };

    struct eth_addr fallback_mac;
    if (port_entry->flavour == DEVLINK_PORT_FLAVOUR_PCI_PF
            && eth_addr_is_zero(port_entry->function.eth_addr)) {
        /* PF representor does not have host facing MAC address set.
         *
         * For kernel versions where the devlink-port infrastructure does
         * not provide MAC address for PCI_PF flavoured ports, there exists
         * a interim interface in sysfs which is relative to the name of a
         * PHYSICAL port netdev name.
         *
         * Note that there is not really any association between PHYSICAL and
         * PF representor ports from the devlink data structure point of view.
         * But we have found them to correlate on the devices where this is
         * necessary.
         *
         * Attempt to retrieve host facing MAC address from the compatibility
         * interface */
        struct port_node *phy;
        phy = port_table_lookup_phy_bus_dev(port_table,
                                            port_entry->bus_name,
                                            port_entry->dev_name,
                                            DEVLINK_PORT_FLAVOUR_PHYSICAL,
                                            port_entry->pci_pf_number);
        if (!phy) {
            VLOG_WARN("Unable to find PHYSICAL representor for fallback "
                      "lookup of host PF MAC address.");
            return;
        }
        if (!compat_get_host_pf_mac(phy->netdev_name, &fallback_mac)) {
            VLOG_WARN("Fallback lookup of host PF MAC address failed.");
            return;
        }
    }
    port_table_update_entry(
        port_table, port_entry->bus_name, port_entry->dev_name,
        port_entry->index,
        port_entry->netdev_ifindex, port_entry->netdev_name,
        port_entry->number, port_entry->pci_pf_number,
        port_entry->pci_vf_number, port_entry->flavour,
        port_entry->flavour == DEVLINK_PORT_FLAVOUR_PCI_PF
            && eth_addr_is_zero(port_entry->function.eth_addr) ?
        fallback_mac : port_entry->function.eth_addr,
        port_node_source);
}

static void
port_table_delete_devlink_port(struct dl_port *port_entry)
{
    port_table_delete_entry(port_table,
                            port_entry->bus_name, port_entry->dev_name,
                            port_entry->number, port_entry->pci_pf_number,
                            port_entry->pci_vf_number, port_entry->flavour);
}

static int
devlink_port_dump(void)
{
    struct nl_dl_dump_state *port_dump;
    struct dl_port port_entry;
    int error;

    port_table = port_table_create();

    port_dump = nl_dl_dump_init();
    if ((error = nl_dl_dump_init_error(port_dump))) {
        VLOG_WARN(
            "unable to start dump of ports from devlink-port interface");
        return error;
    }
    nl_dl_dump_start(DEVLINK_CMD_PORT_GET, port_dump);
    while (nl_dl_port_dump_next(port_dump, &port_entry)) {
        port_table_update_devlink_port(&port_entry, PORT_NODE_SOURCE_DUMP);
    }
    nl_dl_dump_finish(port_dump);
    nl_dl_dump_destroy(port_dump);

    return 0;
}

static int
devlink_monitor_init(void)
{
    unsigned int devlink_mcgroup;
    int error;

    error = nl_lookup_genl_mcgroup(DEVLINK_GENL_NAME,
                                   DEVLINK_GENL_MCGRP_CONFIG_NAME,
                                   &devlink_mcgroup);
    if (error) {
        return error;
    }

    error = nl_sock_create(NETLINK_GENERIC, &devlink_monitor_sock);
    if (error) {
        return error;
    }

    error = nl_sock_join_mcgroup(devlink_monitor_sock, devlink_mcgroup);
    if (error) {
        return error;
    }

    return 0;
}

static bool
devlink_monitor_run(void)
{
    uint64_t buf_stub[4096 / 64];
    struct ofpbuf buf;
    int error;
    bool changed = false;

    ofpbuf_use_stub(&buf, buf_stub, sizeof buf_stub);
    for (;;) {
        error = nl_sock_recv(devlink_monitor_sock, &buf, NULL, false);
        if (error == EAGAIN) {
            /* Nothing to do. */
            break;
        } else if (error == ENOBUFS) {
            VLOG_WARN("devlink monitor socket overflowed: %s",
                      ovs_strerror(error));
        } else if (error) {
            VLOG_ERR("error on devlink monitor socket: %s",
                     ovs_strerror(error));
            break;
        } else {
            struct genlmsghdr *genl;
            struct dl_port port_entry;

            genl = nl_msg_genlmsghdr(&buf);
            if (genl->cmd == DEVLINK_CMD_PORT_NEW
                    || genl->cmd == DEVLINK_CMD_PORT_DEL) {
                if (!nl_dl_parse_port_policy(&buf, &port_entry)) {
                    VLOG_WARN("could not parse devlink port entry");
                    continue;
                }
                if (genl->cmd == DEVLINK_CMD_PORT_NEW) {
                    if (port_entry.netdev_ifindex == UINT32_MAX) {
                        /* When ports are removed we receive both a NEW CMD
                         * without data, followed by a DEL CMD. Ignore the
                         * empty NEW CMD */
                        continue;
                    }
                    changed = true;
                    port_table_update_devlink_port(&port_entry,
                                                   PORT_NODE_SOURCE_RUNTIME);
                } else if (genl->cmd == DEVLINK_CMD_PORT_DEL) {
                    port_table_delete_devlink_port(&port_entry);
                }
            }
        }
    }
    port_table_expire_pending_vf_mac_programming(port_table);
    return changed;
}

#ifdef HAVE_UDEV
static void
udev_monitor_init(void)
{
    udev = udev_new();
    if (!udev) {
        VLOG_ERR("unable to initialize udev context: %s", ovs_strerror(errno));
        return;
    }

    udev_monitor = udev_monitor_new_from_netlink(udev, "kernel");
    if (!udev_monitor) {
        VLOG_ERR("unable to initialize udev monitor: %s", ovs_strerror(errno));
        return;
    }

    int err;
    err = udev_monitor_filter_add_match_subsystem_devtype(
        udev_monitor, "net", NULL);
    if (err < 0) {
        VLOG_WARN("unable to initialize udev monitor filter: %s",
                  ovs_strerror(-err));
    }
    err = udev_monitor_enable_receiving(udev_monitor);
    if (err < 0) {
        VLOG_ERR("unable to initialize udev monitor: %s", ovs_strerror(-err));
        return;
    }
    err = udev_monitor_set_receive_buffer_size(udev_monitor,
                                               128 * 1024 * 1024);
    if (err < 0) {
        VLOG_INFO("unable to set udev receive buffer size: %s",
                 ovs_strerror(-err));
        return;
    }
}
#endif /* HAVE_UDEV */

static bool
udev_monitor_run(void)
{
    bool changed = false;
#ifdef HAVE_UDEV
    int fd;
    char buf[1];
    size_t n_recv;
    struct udev_device *dev;

    fd = udev_monitor_get_fd(udev_monitor);

    for (;;) {
        n_recv = recv(fd, buf, 1, MSG_DONTWAIT | MSG_PEEK);
        if (n_recv == -1) {
            if (errno == EAGAIN) {
                /* Nothing to do. */
                break;
            } else {
                VLOG_ERR("error on udev monitor socket: %s",
                         ovs_strerror(errno));
            }
        } else {
            dev = udev_monitor_receive_device(udev_monitor);
            if (!dev) {
                /* Nothing to do. */
                break;
            }
            const char *udev_action = udev_device_get_action(dev);
            if (udev_action && !strcmp(udev_action, "move")) {
                const char *ifindex_str, *sysname;
                char *cp = NULL;
                uint32_t ifindex;
                struct port_node *pn;

                ifindex_str = udev_device_get_sysattr_value(dev, "ifindex");
                if (!ifindex_str) {
                    VLOG_WARN("udev: unable to get ifindex of moved netdev.");
                    goto next;
                }

                sysname = udev_device_get_sysname(dev);
                if (!sysname) {
                    VLOG_ERR("Unable to lookup netdev name from udev.");
                    goto next;
                }

                ifindex = strtol(ifindex_str, &cp, 10);
                if (cp && cp != ifindex_str && *cp != '\0') {
                    VLOG_WARN("udev provided malformed ifindex: '%s'",
                              ifindex_str);
                    goto next;
                }

                pn = port_table_lookup_ifindex(port_table, ifindex);
                if (!pn) {
                    VLOG_DBG("udev move event on port we do not know about "
                             "ifindex=%s", ifindex_str);
                    goto next;
                }

                port_node_update(pn, sysname);
                changed = true;
            }
next:
            udev_device_unref(dev);
        }
    }
#endif /* HAVE_UDEV */
    return changed;
}

static int
vif_plug_representor_init(void)
{
    int error;

    error = devlink_monitor_init();
    if (error) {
        return error;
    }

    error = devlink_port_dump();
    if (error) {
        return error;
    }

#ifdef HAVE_UDEV
    udev_monitor_init();
#endif /* HAVE_UDEV */

    return 0;
}

static bool
vif_plug_representor_run(struct vif_plug_class *plug_class OVS_UNUSED)
{
    return devlink_monitor_run() & udev_monitor_run();
}

static int
vif_plug_representor_destroy(void)
{
    port_table_destroy(port_table);

    return 0;
}

static bool
vif_plug_representor_port_prepare(const struct vif_plug_port_ctx_in *ctx_in,
                                 struct vif_plug_port_ctx_out *ctx_out)
{
    if (ctx_in->op_type == PLUG_OP_REMOVE) {
        return true;
    }
    const char *opt_pf_mac = smap_get(&ctx_in->lport_options,
                                   "vif-plug:representor:pf-mac");
    const char *opt_vf_num = smap_get(&ctx_in->lport_options,
                                   "vif-plug:representor:vf-num");
    if (!opt_pf_mac || !opt_vf_num) {
         return false;
    }

    /* Ensure lookup tables are up to date */
    vif_plug_representor_run(NULL);

    struct eth_addr pf_mac;
    if (!eth_addr_from_string(opt_pf_mac, &pf_mac)) {
        VLOG_WARN("Unable to parse option as Ethernet address for lport: %s "
                  "pf-mac: '%s' vf-num: '%s'",
                  ctx_in->lport_name, opt_pf_mac, opt_vf_num);
        return false;
    }

    char *cp = NULL;
    uint16_t vf_num = strtol(opt_vf_num, &cp, 10);
    if (cp && cp != opt_vf_num && *cp != '\0') {
        VLOG_WARN("Unable to parse option as VF number for lport: %s "
                  "pf-mac: '%s' vf-num: '%s'",
                  ctx_in->lport_name, opt_pf_mac, opt_vf_num);
    }

    struct port_node *pn;
    pn = port_table_lookup_pf_mac_vf(port_table, pf_mac, vf_num);

    if (!pn || !pn->netdev_name) {
        VLOG_INFO("No representor port found for "
                  "lport: %s pf-mac: '%s' vf-num: '%s'",
                  ctx_in->lport_name, opt_pf_mac, opt_vf_num);
        return false;
    } else if (port_node_rename_expected(pn)) {
        VLOG_INFO("Lookup of representor port successful, but we anticipate "
                  "the netdev name to change, refusing plug/update of "
                  "lport: %s current netdev_name: %s",
                  ctx_in->lport_name, pn->netdev_name);
        return false;
    }

    vif_plug_representor_program_vf_mac(ctx_in, pn, opt_pf_mac, opt_vf_num);

    if (ctx_out) {
        ctx_out->name = pn->netdev_name;
        ctx_out->type = NULL;
    }
    return true;
}

static void
vif_plug_representor_port_finish(
        const struct vif_plug_port_ctx_in *ctx_in OVS_UNUSED,
        struct vif_plug_port_ctx_out *ctx_out OVS_UNUSED)
{
    /* Nothing to be done here for now */
}

static void
vif_plug_representor_port_ctx_destroy(
        const struct vif_plug_port_ctx_in *ctx_in OVS_UNUSED,
        struct vif_plug_port_ctx_out *ctx_out OVS_UNUSED)
{
    /* Noting to be done here for now */
}

const struct vif_plug_class vif_plug_representor = {
    .type = "representor",
    .init = vif_plug_representor_init,
    .destroy = vif_plug_representor_destroy,
    .vif_plug_get_maintained_iface_options = NULL, /* TODO */
    .run = vif_plug_representor_run,
    .vif_plug_port_prepare = vif_plug_representor_port_prepare,
    .vif_plug_port_finish = vif_plug_representor_port_finish,
    .vif_plug_port_ctx_destroy = vif_plug_representor_port_ctx_destroy,
};

/* The kernel devlink-port interface provides a vendor neutral and standard way
 * of discovering host visible resources such as MAC address of interfaces from
 * a program running on the NIC SoC side.
 *
 * However a fairly recent kernel version is required for it to work, so until
 * this is widely available we provide this helper to retrieve the same
 * information from the interim sysfs solution. */
#ifndef OVSTEST
static bool
compat_get_host_pf_mac(const char *netdev_name, struct eth_addr *ea)
{
    char file_name[IFNAMSIZ + 35 + 1];
    FILE *stream;
    char line[128];
    bool retval = false;

    snprintf(file_name, sizeof(file_name),
             "/sys/class/net/%s/smart_nic/pf/config", netdev_name);
    stream = fopen(file_name, "r");
    if (!stream) {
        VLOG_WARN("%s: open failed (%s)",
                  file_name, ovs_strerror(errno));
        *ea = eth_addr_zero;
        return false;
    }
    while (fgets(line, sizeof(line), stream)) {
        char key[16];
        char *cp;
        if (ovs_scan(line, "%15[^:]: ", key)
            && key[0] == 'M' && key[1] == 'A' && key[2] == 'C')
        {
            /* strip any newline character */
            if ((cp = strchr(line, '\n')) != NULL) {
                *cp = '\0';
            }
            /* point cp at end of key + ': ', i.e. start of MAC address */
            cp = line + strnlen(key, sizeof(key)) + 2;
            retval = eth_addr_from_string(cp, ea);
            break;
        }
    }
    fclose(stream);
    return retval;
}
#endif /* OVSTEST */

#ifdef OVSTEST
#include "tests/ovstest.h"

static bool
compat_get_host_pf_mac(const char *netdev_name, struct eth_addr *ea)
{

    ovs_assert(!strcmp(netdev_name, "p0"));
    *ea = (struct eth_addr)ETH_ADDR_C(00,53,00,00,00,51);
    return true;
}

static void
reset_set_hw_addr_mock(int error)
{
    ovstest_set_hw_addr_error = error;
    ovstest_set_hw_addr_calls = 0;
    ovstest_set_hw_addr_last_bus_name = NULL;
    ovstest_set_hw_addr_last_dev_name = NULL;
    ovstest_set_hw_addr_last_port_index = UINT32_MAX;
    ovstest_set_hw_addr_last_mac = eth_addr_zero;
}

static void
reset_get_hw_addr_mock(bool found, struct eth_addr mac, int error)
{
    ovstest_get_hw_addr_found = found;
    ovstest_get_hw_addr_mac = mac;
    ovstest_get_hw_addr_error = error;
    ovstest_get_hw_addr_calls = 0;
}

static void
_init_store(void)
{
    port_table = port_table_create();

    port_table_update_entry(
        port_table, "pci", "0000:03:00.0", 1, 10, "p0", 0,
        UINT16_MAX, UINT16_MAX, DEVLINK_PORT_FLAVOUR_PHYSICAL,
        (struct eth_addr) ETH_ADDR_C(00,53,00,00,00,00),
        PORT_NODE_SOURCE_DUMP);
    port_table_update_entry(
        port_table, "pci", "0000:03:00.0", 2, 100, "p0hpf", UINT32_MAX,
        0, UINT16_MAX, DEVLINK_PORT_FLAVOUR_PCI_PF,
        (struct eth_addr) ETH_ADDR_C(00,53,00,00,00,42),
        PORT_NODE_SOURCE_DUMP);
}

static void
_destroy_store(void)
{
    port_table_destroy(port_table);
}

static void
test_phy_store(struct ovs_cmdl_context *ctx OVS_UNUSED)
{
    struct port_node *pn;

    _init_store();

    pn = port_table_lookup_phy_bus_dev(port_table, "pci", "0000:03:00.0",
                                       DEVLINK_PORT_FLAVOUR_PHYSICAL, 0);
    ovs_assert(pn);
    ovs_assert(pn->netdev_ifindex == 10);
    ovs_assert(!strcmp(pn->netdev_name, "p0"));
    ovs_assert(
        eth_addr_equals(pn->mac,
                        (struct eth_addr) ETH_ADDR_C(00,53,00,00,00,00)));
    ovs_assert(pn->flavour == DEVLINK_PORT_FLAVOUR_PHYSICAL);
    ovs_assert(pn->number == 0);

    ovs_assert(pn == port_table_lookup_ifindex(port_table, 10));

    pn = port_table_lookup_phy_bus_dev(port_table, "pci", "0000:03:00.0",
                                       DEVLINK_PORT_FLAVOUR_PCI_PF, 0);
    ovs_assert(pn);
    ovs_assert(pn->netdev_ifindex == 100);
    ovs_assert(!strcmp(pn->netdev_name, "p0hpf"));
    ovs_assert(
        eth_addr_equals(pn->mac,
                        (struct eth_addr) ETH_ADDR_C(00,53,00,00,00,42)));
    ovs_assert(pn == port_table_lookup_ifindex(port_table, 100));

    port_table_delete_entry(port_table, "pci", "0000:03:00.0",
                            UINT32_MAX, 0, UINT16_MAX,
                            DEVLINK_PORT_FLAVOUR_PCI_PF);

    pn = port_table_lookup_phy_bus_dev(port_table, "pci", "0000:03:00.0",
                                       DEVLINK_PORT_FLAVOUR_PCI_PF, 0);
    ovs_assert(!pn);

    port_table_delete_entry(port_table, "pci", "0000:03:00.0",
                            0, UINT16_MAX, UINT16_MAX,
                            DEVLINK_PORT_FLAVOUR_PHYSICAL);

    pn = port_table_lookup_phy_bus_dev(port_table, "pci", "0000:03:00.0",
                                       DEVLINK_PORT_FLAVOUR_PHYSICAL, 0);
    ovs_assert(!pn);

    /* confirm that we would not misbehave on attempt to delete non-existing
     * entries. */
    port_table_delete_entry(port_table, "nonexistent", "device",
                            UINT32_MAX, 0, UINT16_MAX,
                            DEVLINK_PORT_FLAVOUR_PCI_PF);
    port_table_delete_entry(port_table, "nonexistent", "device",
                            0, UINT16_MAX, UINT16_MAX,
                            DEVLINK_PORT_FLAVOUR_PHYSICAL);

    _destroy_store();
}

static void
test_port_store(struct ovs_cmdl_context *ctx OVS_UNUSED)
{
    struct port_node *pn;

    _init_store();

    port_table_update_entry(
        port_table, "pci", "0000:03:00.0", 3, 1000, "pf0vf0", UINT32_MAX,
        0, 0, DEVLINK_PORT_FLAVOUR_PCI_VF,
        (struct eth_addr) ETH_ADDR_C(00,53,00,00,10,00),
        PORT_NODE_SOURCE_RUNTIME);

    pn = port_table_lookup_ifindex(port_table, 1000);

    ovs_assert(pn);
    ovs_assert(pn->netdev_ifindex == 1000);
    ovs_assert(!strcmp(pn->netdev_name, "pf0vf0"));
    ovs_assert(
        eth_addr_equals(pn->mac,
                        (struct eth_addr) ETH_ADDR_C(00,53,00,00,10,00)));
    ovs_assert(pn->flavour == DEVLINK_PORT_FLAVOUR_PCI_VF);
    ovs_assert(pn->number == 0);
    ovs_assert(pn->port_node_source == PORT_NODE_SOURCE_RUNTIME);

    ovs_assert(pn->pf);
    ovs_assert(!strcmp(pn->pf->netdev_name, "p0hpf"));

    pn = port_table_lookup_pf_mac_vf(
        port_table,
        (struct eth_addr) ETH_ADDR_C(00,53,00,00,00,42),
        0);

    ovs_assert(pn);
    ovs_assert(pn->netdev_ifindex == 1000);
    ovs_assert(!strcmp(pn->netdev_name, "pf0vf0"));
    ovs_assert(
        eth_addr_equals(pn->mac,
                        (struct eth_addr) ETH_ADDR_C(00,53,00,00,10,00)));

    port_table_delete_entry(port_table, "pci", "0000:03:00.0", UINT32_MAX,
                            0, 0, DEVLINK_PORT_FLAVOUR_PCI_VF);

    pn = port_table_lookup_ifindex(port_table, 1000);
    ovs_assert(!pn);

    pn = port_table_lookup_pf_mac_vf(
        port_table,
        (struct eth_addr) ETH_ADDR_C(00,53,00,00,00,42),
        0);
    ovs_assert(!pn);

    /* confirm that we would not misbehave on attempt to delete non-existing
     * entries. */
    port_table_delete_entry(port_table, "non", "existing", UINT32_MAX,
                            0, 0, DEVLINK_PORT_FLAVOUR_PCI_VF);

    _destroy_store();
}

static void
test_program_vf_mac_success(struct ovs_cmdl_context *ctx OVS_UNUSED)
{
    struct vif_plug_port_ctx_in ctx_in;
    struct port_node *pn;
    struct eth_addr old_mac = (struct eth_addr)ETH_ADDR_C(00,53,00,00,10,00);
    struct eth_addr new_mac = (struct eth_addr)ETH_ADDR_C(00,53,00,00,10,be);

    _init_store();
    pn = port_table_update_entry(
        port_table, "pci", "0000:03:00.0", 3, 1000, "pf0vf0", UINT32_MAX,
        0, 0, DEVLINK_PORT_FLAVOUR_PCI_VF, old_mac, PORT_NODE_SOURCE_DUMP);
    ovs_assert(pn);

    memset(&ctx_in, 0, sizeof ctx_in);
    ctx_in.lport_name = "lp-vf-mac-success";
    smap_init(&ctx_in.lport_options);
    smap_init(&ctx_in.iface_options);
    smap_add(&ctx_in.lport_options, "mac", "00:53:00:00:10:be");

    reset_set_hw_addr_mock(0);
    vif_plug_representor_program_vf_mac(&ctx_in, pn, "00:53:00:00:00:42", "0");

    ovs_assert(ovstest_set_hw_addr_calls == 1);
    ovs_assert(!strcmp(ovstest_set_hw_addr_last_bus_name, "pci"));
    ovs_assert(!strcmp(ovstest_set_hw_addr_last_dev_name, "0000:03:00.0"));
    ovs_assert(ovstest_set_hw_addr_last_port_index == 3);
    ovs_assert(eth_addr_equals(ovstest_set_hw_addr_last_mac, new_mac));
    ovs_assert(eth_addr_equals(pn->mac, new_mac));

    smap_destroy(&ctx_in.lport_options);
    smap_destroy(&ctx_in.iface_options);
    _destroy_store();
}

static void
test_program_vf_mac_error(struct ovs_cmdl_context *ctx OVS_UNUSED)
{
    struct vif_plug_port_ctx_in ctx_in;
    struct port_node *pn;
    struct eth_addr old_mac = (struct eth_addr)ETH_ADDR_C(00,53,00,00,10,00);

    _init_store();
    pn = port_table_update_entry(
        port_table, "pci", "0000:03:00.0", 3, 1000, "pf0vf0", UINT32_MAX,
        0, 0, DEVLINK_PORT_FLAVOUR_PCI_VF, old_mac, PORT_NODE_SOURCE_DUMP);
    ovs_assert(pn);

    memset(&ctx_in, 0, sizeof ctx_in);
    ctx_in.lport_name = "lp-vf-mac-error";
    smap_init(&ctx_in.lport_options);
    smap_init(&ctx_in.iface_options);
    smap_add(&ctx_in.lport_options, "addresses",
             "00:53:00:00:10:be 192.168.1.2");

    reset_set_hw_addr_mock(EOPNOTSUPP);
    vif_plug_representor_program_vf_mac(&ctx_in, pn, "00:53:00:00:00:42", "0");

    ovs_assert(ovstest_set_hw_addr_calls == 1);
    ovs_assert(eth_addr_equals(pn->mac, old_mac));

    smap_destroy(&ctx_in.lport_options);
    smap_destroy(&ctx_in.iface_options);
    _destroy_store();
}

static void
test_program_vf_mac_async_confirm(struct ovs_cmdl_context *ctx OVS_UNUSED)
{
    struct vif_plug_port_ctx_in ctx_in;
    struct port_node *pn;
    struct eth_addr old_mac = (struct eth_addr)ETH_ADDR_C(00,53,00,00,10,00);
    struct eth_addr new_mac = (struct eth_addr)ETH_ADDR_C(00,53,00,00,10,be);
    struct dl_port dl_port = {
        .bus_name = "pci",
        .dev_name = "0000:03:00.0",
        .index = 3,
        .netdev_ifindex = 1000,
        .netdev_name = "pf0vf0",
        .number = UINT32_MAX,
        .pci_pf_number = 0,
        .pci_vf_number = 0,
        .flavour = DEVLINK_PORT_FLAVOUR_PCI_VF,
        .function.eth_addr = (struct eth_addr)ETH_ADDR_C(00,53,00,00,10,be),
    };

    _init_store();
    pn = port_table_update_entry(
        port_table, "pci", "0000:03:00.0", 3, 1000, "pf0vf0", UINT32_MAX,
        0, 0, DEVLINK_PORT_FLAVOUR_PCI_VF, old_mac, PORT_NODE_SOURCE_DUMP);
    ovs_assert(pn);

    memset(&ctx_in, 0, sizeof ctx_in);
    ctx_in.lport_name = "lp-vf-mac-async-confirm";
    smap_init(&ctx_in.lport_options);
    smap_init(&ctx_in.iface_options);
    smap_add(&ctx_in.lport_options, "addresses",
             "00:53:00:00:10:be 192.168.1.2");

    reset_set_hw_addr_mock(0);
    vif_plug_representor_program_vf_mac(&ctx_in, pn, "00:53:00:00:00:42", "0");

    ovs_assert(ovstest_set_hw_addr_calls == 1);
    ovs_assert(pn->vf_mac_programming_pending);
    ovs_assert(eth_addr_equals(pn->vf_mac_requested, new_mac));

    port_table_update_devlink_port(&dl_port, PORT_NODE_SOURCE_RUNTIME);

    pn = port_table_lookup_ifindex(port_table, 1000);
    ovs_assert(pn);
    ovs_assert(!pn->vf_mac_programming_pending);
    ovs_assert(eth_addr_equals(pn->mac, new_mac));

    smap_destroy(&ctx_in.lport_options);
    smap_destroy(&ctx_in.iface_options);
    _destroy_store();
}

static void
test_program_vf_mac_no_change(struct ovs_cmdl_context *ctx OVS_UNUSED)
{
    struct vif_plug_port_ctx_in ctx_in;
    struct port_node *pn;
    struct eth_addr old_mac = (struct eth_addr)ETH_ADDR_C(00,53,00,00,10,00);

    _init_store();
    pn = port_table_update_entry(
        port_table, "pci", "0000:03:00.0", 3, 1000, "pf0vf0", UINT32_MAX,
        0, 0, DEVLINK_PORT_FLAVOUR_PCI_VF, old_mac, PORT_NODE_SOURCE_DUMP);
    ovs_assert(pn);

    memset(&ctx_in, 0, sizeof ctx_in);
    ctx_in.lport_name = "lp-vf-mac-no-change";
    smap_init(&ctx_in.lport_options);
    smap_init(&ctx_in.iface_options);
    smap_add(&ctx_in.lport_options, "mac", "00:53:00:00:10:00");

    reset_set_hw_addr_mock(0);
    vif_plug_representor_program_vf_mac(&ctx_in, pn, "00:53:00:00:00:42", "0");

    ovs_assert(ovstest_set_hw_addr_calls == 0);
    ovs_assert(eth_addr_equals(pn->mac, old_mac));

    smap_destroy(&ctx_in.lport_options);
    smap_destroy(&ctx_in.iface_options);
    _destroy_store();
}

static void
test_program_vf_mac_async_mismatch_retry(
    struct ovs_cmdl_context *ctx OVS_UNUSED)
{
    struct vif_plug_port_ctx_in ctx_in;
    struct port_node *pn;
    struct eth_addr old_mac = (struct eth_addr)ETH_ADDR_C(00,53,00,00,10,00);
    struct eth_addr new_mac = (struct eth_addr)ETH_ADDR_C(00,53,00,00,10,be);
    struct dl_port dl_port = {
        .bus_name = "pci",
        .dev_name = "0000:03:00.0",
        .index = 3,
        .netdev_ifindex = 1000,
        .netdev_name = "pf0vf0",
        .number = UINT32_MAX,
        .pci_pf_number = 0,
        .pci_vf_number = 0,
        .flavour = DEVLINK_PORT_FLAVOUR_PCI_VF,
        .function.eth_addr = (struct eth_addr)ETH_ADDR_C(00,53,00,00,10,11),
    };

    _init_store();
    pn = port_table_update_entry(
        port_table, "pci", "0000:03:00.0", 3, 1000, "pf0vf0", UINT32_MAX,
        0, 0, DEVLINK_PORT_FLAVOUR_PCI_VF, old_mac, PORT_NODE_SOURCE_DUMP);
    ovs_assert(pn);

    memset(&ctx_in, 0, sizeof ctx_in);
    ctx_in.lport_name = "lp-vf-mac-async-mismatch-retry";
    smap_init(&ctx_in.lport_options);
    smap_init(&ctx_in.iface_options);
    smap_add(&ctx_in.lport_options, "addresses",
             "00:53:00:00:10:be 192.168.1.2");

    reset_set_hw_addr_mock(0);
    vif_plug_representor_program_vf_mac(&ctx_in, pn, "00:53:00:00:00:42", "0");

    ovs_assert(ovstest_set_hw_addr_calls == 1);
    ovs_assert(pn->vf_mac_programming_pending);
    ovs_assert(eth_addr_equals(pn->vf_mac_requested, new_mac));

    port_table_update_devlink_port(&dl_port, PORT_NODE_SOURCE_RUNTIME);

    pn = port_table_lookup_ifindex(port_table, 1000);
    ovs_assert(pn);
    ovs_assert(ovstest_set_hw_addr_calls == 2);
    ovs_assert(eth_addr_equals(ovstest_set_hw_addr_last_mac, new_mac));
    ovs_assert(pn->vf_mac_programming_pending);
    ovs_assert(eth_addr_equals(pn->vf_mac_requested, new_mac));
    ovs_assert(eth_addr_equals(pn->mac, new_mac));

    smap_destroy(&ctx_in.lport_options);
    smap_destroy(&ctx_in.iface_options);
    _destroy_store();
}

static void
test_program_vf_mac_expiry_validate_current(
    struct ovs_cmdl_context *ctx OVS_UNUSED)
{
    struct vif_plug_port_ctx_in ctx_in;
    struct port_node *pn;
    struct eth_addr old_mac = (struct eth_addr)ETH_ADDR_C(00,53,00,00,10,00);
    struct eth_addr new_mac = (struct eth_addr)ETH_ADDR_C(00,53,00,00,10,be);

    _init_store();
    pn = port_table_update_entry(
        port_table, "pci", "0000:03:00.0", 3, 1000, "pf0vf0", UINT32_MAX,
        0, 0, DEVLINK_PORT_FLAVOUR_PCI_VF, old_mac, PORT_NODE_SOURCE_DUMP);
    ovs_assert(pn);

    memset(&ctx_in, 0, sizeof ctx_in);
    ctx_in.lport_name = "lp-vf-mac-expiry-validate-current";
    smap_init(&ctx_in.lport_options);
    smap_init(&ctx_in.iface_options);
    smap_add(&ctx_in.lport_options, "addresses",
             "00:53:00:00:10:be 192.168.1.2");

    reset_set_hw_addr_mock(0);
    vif_plug_representor_program_vf_mac(&ctx_in, pn, "00:53:00:00:00:42", "0");
    ovs_assert(ovstest_set_hw_addr_calls == 1);
    ovs_assert(pn->vf_mac_programming_pending);

    reset_get_hw_addr_mock(true, new_mac, 0);
    pn->vf_mac_requested_at = time_msec() - VF_MAC_CONFIRM_TIMEOUT_MS - 1;
    port_table_expire_pending_vf_mac_programming(port_table);

    ovs_assert(ovstest_get_hw_addr_calls == 1);
    ovs_assert(ovstest_set_hw_addr_calls == 1);
    ovs_assert(!pn->vf_mac_programming_pending);
    ovs_assert(eth_addr_equals(pn->mac, new_mac));

    smap_destroy(&ctx_in.lport_options);
    smap_destroy(&ctx_in.iface_options);
    _destroy_store();
}

static void
test_program_vf_mac_expiry_retry(struct ovs_cmdl_context *ctx OVS_UNUSED)
{
    struct vif_plug_port_ctx_in ctx_in;
    struct port_node *pn;
    struct eth_addr old_mac = (struct eth_addr)ETH_ADDR_C(00,53,00,00,10,00);
    struct eth_addr new_mac = (struct eth_addr)ETH_ADDR_C(00,53,00,00,10,be);

    _init_store();
    pn = port_table_update_entry(
        port_table, "pci", "0000:03:00.0", 3, 1000, "pf0vf0", UINT32_MAX,
        0, 0, DEVLINK_PORT_FLAVOUR_PCI_VF, old_mac, PORT_NODE_SOURCE_DUMP);
    ovs_assert(pn);

    memset(&ctx_in, 0, sizeof ctx_in);
    ctx_in.lport_name = "lp-vf-mac-expiry-retry";
    smap_init(&ctx_in.lport_options);
    smap_init(&ctx_in.iface_options);
    smap_add(&ctx_in.lport_options, "addresses",
             "00:53:00:00:10:be 192.168.1.2");

    reset_set_hw_addr_mock(0);
    vif_plug_representor_program_vf_mac(&ctx_in, pn, "00:53:00:00:00:42", "0");
    ovs_assert(ovstest_set_hw_addr_calls == 1);
    ovs_assert(pn->vf_mac_programming_pending);

    reset_get_hw_addr_mock(true, old_mac, 0);
    pn->vf_mac_requested_at = time_msec() - VF_MAC_CONFIRM_TIMEOUT_MS - 1;
    port_table_expire_pending_vf_mac_programming(port_table);

    ovs_assert(ovstest_get_hw_addr_calls == 1);
    ovs_assert(ovstest_set_hw_addr_calls == 2);
    ovs_assert(pn->vf_mac_programming_pending);
    ovs_assert(eth_addr_equals(pn->vf_mac_requested, new_mac));

    smap_destroy(&ctx_in.lport_options);
    smap_destroy(&ctx_in.iface_options);
    _destroy_store();
}

static void
test_port_node_rename_expected(struct ovs_cmdl_context *ctx OVS_UNUSED)
{
    struct port_node *pn;

    _init_store();

    pn = port_table_lookup_ifindex(port_table, 100);
    ovs_assert(port_node_rename_expected(pn) == false);

    pn = port_table_update_entry(
            port_table, "pci", "0000:03:00.0", 3, 1000, "eth0", UINT32_MAX,
            0, 0, DEVLINK_PORT_FLAVOUR_PCI_VF,
            (struct eth_addr) ETH_ADDR_C(00,53,00,00,10,00),
            PORT_NODE_SOURCE_RUNTIME);
    ovs_assert(port_node_rename_expected(pn) == true);

    _destroy_store();
}

static void
test_port_table_update_devlink_port(struct ovs_cmdl_context *ctx OVS_UNUSED)
{
    struct dl_port dl_port = {
        .bus_name = "pci",
        .dev_name = "0000:03:00.0",
        .netdev_ifindex = 1000,
        .netdev_name = "pf0vf0",
        .number = UINT32_MAX,
        .pci_pf_number = 0,
        .pci_vf_number = 0,
        .flavour = DEVLINK_PORT_FLAVOUR_PCI_VF,
    };
    struct port_node *pn;

    _init_store();

    port_table_update_devlink_port(&dl_port, PORT_NODE_SOURCE_RUNTIME);

    pn = port_table_lookup_ifindex(port_table, 1000);
    ovs_assert(pn);
    ovs_assert(pn->pf);
    ovs_assert(
        eth_addr_equals(pn->pf->mac,
                        (struct eth_addr) ETH_ADDR_C(00,53,00,00,00,42)));
    ovs_assert(pn->port_node_source == PORT_NODE_SOURCE_RUNTIME);

    pn = port_table_lookup_pf_mac_vf(
        port_table,
        (struct eth_addr) ETH_ADDR_C(00,53,00,00,00,42),
        0);
    ovs_assert(pn);
    ovs_assert(pn->netdev_ifindex == 1000);

    _destroy_store();
}

static void
test_port_table_delete_devlink_port(struct ovs_cmdl_context *ctx OVS_UNUSED)
{
    struct dl_port dl_port = {
        .bus_name = "pci",
        .dev_name = "0000:03:00.0",
        .netdev_ifindex = 1000,
        .netdev_name = "pf0vf0",
        .number = UINT32_MAX,
        .pci_pf_number = 0,
        .pci_vf_number = 0,
        .flavour = DEVLINK_PORT_FLAVOUR_PCI_VF,
    };
    struct port_node *pn;

    _init_store();

    port_table_update_devlink_port(&dl_port, PORT_NODE_SOURCE_DUMP);

    pn = port_table_lookup_ifindex(port_table, 1000);
    ovs_assert(pn);
    ovs_assert(pn->port_node_source == PORT_NODE_SOURCE_DUMP);

    port_table_delete_devlink_port(&dl_port);
    pn = port_table_lookup_ifindex(port_table, 1000);
    ovs_assert(!pn);

    _destroy_store();
}

static void
test_port_table_update_devlink_port_compat(
    struct ovs_cmdl_context *ctx OVS_UNUSED)
{
    struct dl_port dl_pf_port = {
        .bus_name = "pci",
        .dev_name = "0000:03:00.0",
        .netdev_ifindex = 100,
        .netdev_name = "pf0hpf",
        .number = UINT32_MAX,
        .pci_pf_number = 0,
        .pci_vf_number = UINT16_MAX,
        .flavour = DEVLINK_PORT_FLAVOUR_PCI_PF,
    };
    struct port_node *pn;

    _init_store();

    port_table_delete_entry(port_table, "pci", "0000:03:00.0",
                            UINT32_MAX, 0, UINT16_MAX,
                            DEVLINK_PORT_FLAVOUR_PCI_PF);

    /* check that when we add a PF with zero MAC address, the compat sysfs
     * interface is used to retrieve the MAC. */
    port_table_update_devlink_port(&dl_pf_port, PORT_NODE_SOURCE_DUMP);

    pn = port_table_lookup_phy_bus_dev(port_table, "pci", "0000:03:00.0",
                                       DEVLINK_PORT_FLAVOUR_PCI_PF, 0);
    ovs_assert(pn);
    ovs_assert(
        eth_addr_equals(pn->mac,
                        (struct eth_addr) ETH_ADDR_C(00,53,00,00,00,51)));


    _destroy_store();
}

static void
test_vif_plug_representor_main(int argc, char **argv) {
    set_program_name(*argv);
    static const struct ovs_cmdl_command commands[] = {
        {"store-phy", NULL, 0, 0, test_phy_store, OVS_RO},
        {"store-port", NULL, 0, 0, test_port_store, OVS_RO},
        {"store-devlink-port-update", NULL, 0, 0,
         test_port_table_update_devlink_port, OVS_RO},
        {"store-devlink-port-delete", NULL, 0, 0,
         test_port_table_delete_devlink_port, OVS_RO},
        {"store-devlink-port-update-compat", NULL, 0, 0,
         test_port_table_update_devlink_port_compat, OVS_RO},
        {"store-rename-expected", NULL, 0, 0,
         test_port_node_rename_expected, OVS_RO},
        {"vf-mac-success", NULL, 0, 0, test_program_vf_mac_success, OVS_RO},
        {"vf-mac-error", NULL, 0, 0, test_program_vf_mac_error, OVS_RO},
        {"vf-mac-async-confirm", NULL, 0, 0,
         test_program_vf_mac_async_confirm, OVS_RO},
        {"vf-mac-no-change", NULL, 0, 0, test_program_vf_mac_no_change,
         OVS_RO},
        {"vf-mac-async-mismatch-retry", NULL, 0, 0,
         test_program_vf_mac_async_mismatch_retry, OVS_RO},
        {"vf-mac-expiry-validate-current", NULL, 0, 0,
         test_program_vf_mac_expiry_validate_current, OVS_RO},
        {"vf-mac-expiry-retry", NULL, 0, 0, test_program_vf_mac_expiry_retry,
         OVS_RO},
        {NULL, NULL, 0, 0, NULL, OVS_RO},
    };
    struct ovs_cmdl_context ctx;
    ctx.argc = argc - 1;
    ctx.argv = argv + 1;

    ovs_cmdl_run_command(&ctx, commands);
}

OVSTEST_REGISTER("test-vif-plug-representor", test_vif_plug_representor_main);
#endif /* OVSTEST */
