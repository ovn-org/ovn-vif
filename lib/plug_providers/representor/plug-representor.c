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

#include "plug-provider.h"

#include "hash.h"
#include "openvswitch/hmap.h"
#include "openvswitch/vlog.h"
#include "netlink.h"
#include "netlink-devlink.h"
#include "openvswitch/shash.h"
#include "packets.h"

VLOG_DEFINE_THIS_MODULE(plug_representor);

/* Contains netdev name of ports known to devlink indexed by PF MAC
 * address and logical function number (if applicable).
 *
 * Examples:
 *     SR-IOV Physical Function: key "00:53:00:00:00:42"    value "pf0hpf"
 *     SR-IOV Virtual Function:  key "00:53:00:00:00:42-42" value "pf0vf42"
 */
static struct shash devlink_ports;

/* Max number of physical ports connected to a single NIC SoC. */
#define MAX_NIC_PHY_PORTS 64
/* string repr of eth MAC, '-', logical function number (uint32_t) */
#define MAX_KEY_LEN 17+1+10+1

static bool compat_get_host_pf_mac(const char *, struct eth_addr *);

static bool
fill_devlink_ports_key_from_strs(char *buf, size_t bufsiz,
                                const char *host_pf_mac,
                                const char *function)
{
    return snprintf(buf, bufsiz,
                    function != NULL ? "%s-%s": "%s",
                    host_pf_mac, function) < bufsiz;
}

/* We deliberately pass the struct eth_addr by value as we would have to copy
 * the data either way to make use of the ETH_ADDR_ARGS macro */
static bool
fill_devlink_ports_key_from_typed(char *buf, size_t bufsiz,
                    struct eth_addr host_pf_mac,
                    uint32_t function)
{
    return snprintf(
        buf, bufsiz,
        function < UINT32_MAX ? ETH_ADDR_FMT"-%"PRIu32 : ETH_ADDR_FMT,
        ETH_ADDR_ARGS(host_pf_mac), function) < bufsiz;
}

static void
devlink_port_add_function(struct dl_port *port_entry,
                          struct eth_addr *host_pf_mac)
{
    char keybuf[MAX_KEY_LEN];
    uint32_t function_number;

    switch (port_entry->flavour) {
    case DEVLINK_PORT_FLAVOUR_PCI_PF:
        /* for Physical Function representor ports we only add the MAC address
         * and no logical function number */
        function_number = -1;
        break;
    case DEVLINK_PORT_FLAVOUR_PCI_VF:
        function_number = port_entry->pci_vf_number;
        break;
    default:
        VLOG_WARN("Unsupported flavour for port '%s': %s",
            port_entry->netdev_name,
            port_entry->flavour == DEVLINK_PORT_FLAVOUR_PHYSICAL ? "PHYSICAL" :
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
    /* Failure to fill key from typed values means calculation of the max key
     * length is wrong, i.e. a bug. */
    ovs_assert(fill_devlink_ports_key_from_typed(
                            keybuf, sizeof(keybuf),
                            *host_pf_mac, function_number));
    shash_add(&devlink_ports, keybuf, xstrdup(port_entry->netdev_name));
}


static int
plug_representor_init(void)
{
    struct nl_dl_dump_state *port_dump;
    struct dl_port port_entry;
    int error;
    struct eth_addr host_pf_macs[MAX_NIC_PHY_PORTS];

    shash_init(&devlink_ports);

    port_dump = nl_dl_dump_init();
    if ((error = nl_dl_dump_init_error(port_dump))) {
        VLOG_WARN(
            "unable to start dump of ports from devlink-port interface");
        return error;
    }
    /* The core devlink infrastructure in the kernel keeps a linked list of
     * the devices and each of those has a linked list of ports. These are
     * populated by each device driver as devices are enumerated, and as such
     * we can rely on ports being dumped in a consistent order on a device
     * by device basis with logical numbering for each port flavour starting
     * on 0 for each new device.
     */
    nl_dl_dump_start(DEVLINK_CMD_PORT_GET, port_dump);
    while (nl_dl_port_dump_next(port_dump, &port_entry)) {
        switch (port_entry.flavour) {
        case DEVLINK_PORT_FLAVOUR_PHYSICAL:
            /* The PHYSICAL flavoured port represent a network facing port on
             * the NIC.
             *
             * For kernel versions where the devlink-port infrastructure does
             * not provide MAC address for PCI_PF flavoured ports, there exist
             * a interface in sysfs which is relative to the name of the
             * PHYSICAL port netdev name.
             *
             * Since we at this point in the dump do not know if the MAC will
             * be provided for the PCI_PF or not, proactively store the MAC
             * address by looking up through the sysfs interface.
             *
             * If MAC address is available once we get to the PCI_PF we will
             * overwrite the stored value.
             */
            if (port_entry.number > MAX_NIC_PHY_PORTS) {
                VLOG_WARN("physical port number out of range for port '%s': "
                          "%"PRIu32,
                          port_entry.netdev_name, port_entry.number);
                continue;
            }
            compat_get_host_pf_mac(port_entry.netdev_name,
                                   &host_pf_macs[port_entry.number]);
            break;
        case DEVLINK_PORT_FLAVOUR_PCI_PF: /* FALL THROUGH */
            /* The PCI_PF flavoured port represent a host facing port.
             *
             * For function flavours other than PHYSICAL pci_pf_number will be
             * set to the logical number of which physical port the function
             * belongs.
             */
            if (!eth_addr_is_zero(port_entry.function.eth_addr)) {
                host_pf_macs[port_entry.pci_pf_number] =
                    port_entry.function.eth_addr;
            }
            /* FALL THROUGH */
        case DEVLINK_PORT_FLAVOUR_PCI_VF:
            /* The PCI_VF flavoured port represent a host facing
             * PCI Virtual Function.
             *
             * For function flavours other than PHYSICAL pci_pf_number will be
             * set to the logical number of which physical port the function
             * belongs.
             */
            if (port_entry.pci_pf_number > MAX_NIC_PHY_PORTS) {
                VLOG_WARN("physical port number out of range for port '%s': "
                          "%"PRIu32,
                          port_entry.netdev_name, port_entry.pci_pf_number);
                continue;
            }
            devlink_port_add_function(&port_entry,
                                      &host_pf_macs[port_entry.pci_pf_number]);
            break;
        };
    }
    nl_dl_dump_finish(port_dump);
    nl_dl_dump_destroy(port_dump);

    return 0;
}

static int
plug_representor_destroy(void)
{
    shash_destroy_free_data(&devlink_ports);

    return 0;
}

static bool
plug_representor_port_prepare(const struct plug_port_ctx_in *ctx_in,
                              struct plug_port_ctx_out *ctx_out)
{
    if (ctx_in->op_type == PLUG_OP_REMOVE) {
        return true;
    }
    char keybuf[MAX_KEY_LEN];
    const char *pf_mac = smap_get(&ctx_in->lport_options,
                                  "plug:representor:pf-mac");
    const char *vf_num = smap_get(&ctx_in->lport_options,
                                  "plug:representor:vf-num");
    if (!pf_mac || !vf_num) {
        return false;
    }
    if (!fill_devlink_ports_key_from_strs(keybuf, sizeof(keybuf),
                                          pf_mac, vf_num))
    {
        /* Overflow, most likely incorrect input data from database */
        VLOG_WARN("Southbound DB port plugging options out of range for "
                  "lport: %s pf-mac: '%s' vf-num: '%s'",
                  ctx_in->lport_name, pf_mac, vf_num);
        return false;
    }

    char *rep_port;
    rep_port = shash_find_data(&devlink_ports, keybuf);
    if (!rep_port) {
        VLOG_INFO("No representor port found for "
                  "lport: %s pf-mac: '%s' vf-num: '%s'",
                  ctx_in->lport_name, pf_mac, vf_num);
        return false;
    }
    if (ctx_out) {
        ctx_out->name = rep_port;
        ctx_out->type = NULL;
    }
    return true;
}

static void
plug_representor_port_finish(const struct plug_port_ctx_in *ctx_in OVS_UNUSED,
                             struct plug_port_ctx_out *ctx_out OVS_UNUSED)
{
    /* Nothing to be done here for now */
}

static void
plug_representor_port_ctx_destroy(
                const struct plug_port_ctx_in *ctx_in OVS_UNUSED,
                struct plug_port_ctx_out *ctx_out OVS_UNUSED)
{
    /* Noting to be done here for now */
}

const struct plug_class plug_representor = {
    .type = "representor",
    .init = plug_representor_init,
    .destroy = plug_representor_destroy,
    .plug_get_maintained_iface_options = NULL, /* TODO */
    .run = NULL, /* TODO */
    .plug_port_prepare = plug_representor_port_prepare,
    .plug_port_finish = plug_representor_port_finish,
    .plug_port_ctx_destroy = plug_representor_port_ctx_destroy,
};

/* The kernel devlink-port interface provides a vendor neutral and standard way
 * of discovering host visible resources such as MAC address of interfaces from
 * a program running on the NIC SoC side.
 *
 * However a fairly recent kernel version is required for it to work, so until
 * this is widely available we provide this helper to retrieve the same
 * information from the interim sysfs solution. */
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
