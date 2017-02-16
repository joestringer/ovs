/*
 * Copyright (c) 2017 Red Hat, Inc.
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

#include "dpif-rtnetlink.h"

#include <net/if.h>
#include <linux/ip.h>
#include <linux/rtnetlink.h>

#include "dpif-netlink.h"
#include "netdev-vport.h"
#include "netlink-socket.h"

/*
 * On some older systems, these enums are not defined.
 */
#ifndef IFLA_VXLAN_MAX
#define IFLA_VXLAN_MAX 0
#define IFLA_VXLAN_PORT 15
#endif
#if IFLA_VXLAN_MAX < 20
#define IFLA_VXLAN_UDP_ZERO_CSUM6_RX 20
#define IFLA_VXLAN_GBP 23
#define IFLA_VXLAN_COLLECT_METADATA 25
#endif

#if IFLA_GRE_MAX < 18
#define IFLA_GRE_COLLECT_METADATA 18
#endif

#ifndef IFLA_GENEVE_MAX
#define IFLA_GENEVE_MAX 0
#define IFLA_GENEVE_PORT 5
#endif

#if IFLA_GENEVE_MAX < 6
#define IFLA_GENEVE_COLLECT_METADATA 6
#endif
#if IFLA_GENEVE_MAX < 10
#define IFLA_GENEVE_UDP_ZERO_CSUM6_RX 10
#endif

static const struct nl_policy rtlink_policy[] = {
    [IFLA_LINKINFO] = { .type = NL_A_NESTED },
};
static const struct nl_policy linkinfo_policy[] = {
    [IFLA_INFO_KIND] = { .type = NL_A_STRING },
    [IFLA_INFO_DATA] = { .type = NL_A_NESTED },
};


static int
dpif_rtnetlink_destroy(const char *name)
{
    int err;
    struct ofpbuf request, *reply;

    ofpbuf_init(&request, 0);
    nl_msg_put_nlmsghdr(&request, 0, RTM_DELLINK,
                        NLM_F_REQUEST | NLM_F_ACK);
    ofpbuf_put_zeros(&request, sizeof(struct ifinfomsg));
    nl_msg_put_string(&request, IFLA_IFNAME, name);

    err = nl_transact(NETLINK_ROUTE, &request, &reply);

    if (!err) {
        ofpbuf_uninit(reply);
    }

    ofpbuf_uninit(&request);
    return err;
}

static int
dpif_rtnetlink_vxlan_destroy(const char *name)
{
    return dpif_rtnetlink_destroy(name);
}

static int
dpif_rtnetlink_gre_destroy(const char *name)
{
    return dpif_rtnetlink_destroy(name);
}

static int
dpif_rtnetlink_geneve_destroy(const char *name)
{
    return dpif_rtnetlink_destroy(name);
}

static int
dpif_rtnetlink_vxlan_verify(struct netdev *netdev, const char *name,
                            const char *kind)
{
    int err;
    struct ofpbuf request, *reply;
    struct ifinfomsg *ifmsg;
    const struct netdev_tunnel_config *tnl_cfg;

    static const struct nl_policy vxlan_policy[] = {
        [IFLA_VXLAN_COLLECT_METADATA] = { .type = NL_A_U8 },
        [IFLA_VXLAN_LEARNING] = { .type = NL_A_U8 },
        [IFLA_VXLAN_UDP_ZERO_CSUM6_RX] = { .type = NL_A_U8 },
        [IFLA_VXLAN_PORT] = { .type = NL_A_U16 },
    };

    tnl_cfg = netdev_get_tunnel_config(netdev);
    if (!tnl_cfg) {
        return EINVAL;
    }

    ofpbuf_init(&request, 0);
    nl_msg_put_nlmsghdr(&request, 0, RTM_GETLINK,
                        NLM_F_REQUEST);
    ofpbuf_put_zeros(&request, sizeof(struct ifinfomsg));
    nl_msg_put_string(&request, IFLA_IFNAME, name);

    err = nl_transact(NETLINK_ROUTE, &request, &reply);
    if (!err) {
        struct nlattr *rtlink[ARRAY_SIZE(rtlink_policy)];
        struct nlattr *linkinfo[ARRAY_SIZE(linkinfo_policy)];
        struct nlattr *vxlan[ARRAY_SIZE(vxlan_policy)];

        ifmsg = ofpbuf_at(reply, NLMSG_HDRLEN, sizeof *ifmsg);
        if (!nl_policy_parse(reply, NLMSG_HDRLEN + sizeof *ifmsg,
                             rtlink_policy, rtlink,
                             ARRAY_SIZE(rtlink_policy)) ||
            !nl_parse_nested(rtlink[IFLA_LINKINFO], linkinfo_policy,
                             linkinfo, ARRAY_SIZE(linkinfo_policy)) ||
            strcmp(nl_attr_get_string(linkinfo[IFLA_INFO_KIND]), kind) ||
            !nl_parse_nested(linkinfo[IFLA_INFO_DATA], vxlan_policy, vxlan,
                             ARRAY_SIZE(vxlan_policy))) {
            err = EINVAL;
        }
        if (!err) {
            if (0 != nl_attr_get_u8(vxlan[IFLA_VXLAN_LEARNING]) ||
                1 != nl_attr_get_u8(vxlan[IFLA_VXLAN_COLLECT_METADATA]) ||
                1 != nl_attr_get_u8(vxlan[IFLA_VXLAN_UDP_ZERO_CSUM6_RX]) ||
                tnl_cfg->dst_port !=
                    nl_attr_get_be16(vxlan[IFLA_VXLAN_PORT])) {
                err = EINVAL;
            }
        }
        if (!err) {
            if ((tnl_cfg->exts & (1 << OVS_VXLAN_EXT_GBP)) &&
                !(vxlan[IFLA_VXLAN_GBP] &&
                  nl_attr_get_flag(vxlan[IFLA_VXLAN_GBP]))) {
                err = EINVAL;
            }
        }
        ofpbuf_uninit(reply);
    }
    ofpbuf_uninit(&request);
    return err;
}

static int
dpif_rtnetlink_vxlan_create_kind(struct netdev *netdev, const char *kind)
{
    int err;
    struct ofpbuf request, *reply;
    size_t linkinfo_off, infodata_off;
    char namebuf[NETDEV_VPORT_NAME_BUFSIZE];
    const char *name = netdev_vport_get_dpif_port(netdev,
                                                  namebuf, sizeof namebuf);
    struct ifinfomsg *ifinfo;
    const struct netdev_tunnel_config *tnl_cfg;
    tnl_cfg = netdev_get_tunnel_config(netdev);
    if (!tnl_cfg) {
        return EINVAL;
    }

    ofpbuf_init(&request, 0);
    nl_msg_put_nlmsghdr(&request, 0, RTM_NEWLINK,
                        NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE);
    ifinfo = ofpbuf_put_zeros(&request, sizeof(struct ifinfomsg));
    ifinfo->ifi_change = ifinfo->ifi_flags = IFF_UP;
    nl_msg_put_string(&request, IFLA_IFNAME, name);
    nl_msg_put_u32(&request, IFLA_MTU, UINT16_MAX);
    linkinfo_off = nl_msg_start_nested(&request, IFLA_LINKINFO);
        nl_msg_put_string(&request, IFLA_INFO_KIND, kind);
        infodata_off = nl_msg_start_nested(&request, IFLA_INFO_DATA);
            nl_msg_put_u8(&request, IFLA_VXLAN_LEARNING, 0);
            nl_msg_put_u8(&request, IFLA_VXLAN_COLLECT_METADATA, 1);
            nl_msg_put_u8(&request, IFLA_VXLAN_UDP_ZERO_CSUM6_RX, 1);
            if (tnl_cfg->exts & (1 << OVS_VXLAN_EXT_GBP)) {
                nl_msg_put_flag(&request, IFLA_VXLAN_GBP);
            }
            nl_msg_put_be16(&request, IFLA_VXLAN_PORT, tnl_cfg->dst_port);
        nl_msg_end_nested(&request, infodata_off);
    nl_msg_end_nested(&request, linkinfo_off);

    err = nl_transact(NETLINK_ROUTE, &request, &reply);

    if (!err) {
        ofpbuf_uninit(reply);
    }

    if (!err && (err = dpif_rtnetlink_vxlan_verify(netdev, name, kind))) {
        dpif_rtnetlink_vxlan_destroy(name);
    }

    ofpbuf_uninit(&request);
    return err;
}

static int
dpif_rtnetlink_vxlan_create(struct netdev *netdev)
{
    return dpif_rtnetlink_vxlan_create_kind(netdev, "vxlan");
}

static int
dpif_rtnetlink_gre_verify(struct netdev *netdev OVS_UNUSED, const char *name,
                          const char *kind)
{
    int err;
    struct ofpbuf request, *reply;
    struct ifinfomsg *ifmsg;

    static const struct nl_policy gre_policy[] = {
        [IFLA_GRE_COLLECT_METADATA] = { .type = NL_A_FLAG },
    };

    ofpbuf_init(&request, 0);
    nl_msg_put_nlmsghdr(&request, 0, RTM_GETLINK,
                        NLM_F_REQUEST);
    ofpbuf_put_zeros(&request, sizeof(struct ifinfomsg));
    nl_msg_put_string(&request, IFLA_IFNAME, name);

    err = nl_transact(NETLINK_ROUTE, &request, &reply);
    if (!err) {
        struct nlattr *rtlink[ARRAY_SIZE(rtlink_policy)];
        struct nlattr *linkinfo[ARRAY_SIZE(linkinfo_policy)];
        struct nlattr *gre[ARRAY_SIZE(gre_policy)];

        ifmsg = ofpbuf_at(reply, NLMSG_HDRLEN, sizeof *ifmsg);
        if (!nl_policy_parse(reply, NLMSG_HDRLEN + sizeof *ifmsg,
                             rtlink_policy, rtlink,
                             ARRAY_SIZE(rtlink_policy)) ||
            !nl_parse_nested(rtlink[IFLA_LINKINFO], linkinfo_policy,
                             linkinfo, ARRAY_SIZE(linkinfo_policy)) ||
            strcmp(nl_attr_get_string(linkinfo[IFLA_INFO_KIND]), kind) ||
            !nl_parse_nested(linkinfo[IFLA_INFO_DATA], gre_policy, gre,
                             ARRAY_SIZE(gre_policy))) {
            err = EINVAL;
        }
        if (!err) {
            if (!nl_attr_get_flag(gre[IFLA_GRE_COLLECT_METADATA])) {
                err = EINVAL;
            }
        }
        ofpbuf_uninit(reply);
    }
    ofpbuf_uninit(&request);
    return err;
}

static int
dpif_rtnetlink_gre_create_kind(struct netdev *netdev, const char *kind)
{
    int err;
    struct ofpbuf request, *reply;
    size_t linkinfo_off, infodata_off;
    char namebuf[NETDEV_VPORT_NAME_BUFSIZE];
    const char *name = netdev_vport_get_dpif_port(netdev,
                                                  namebuf, sizeof namebuf);
    struct ifinfomsg *ifinfo;
    const struct netdev_tunnel_config *tnl_cfg;
    tnl_cfg = netdev_get_tunnel_config(netdev);
    if (!tnl_cfg) {
        return EINVAL;
    }

    ofpbuf_init(&request, 0);
    nl_msg_put_nlmsghdr(&request, 0, RTM_NEWLINK,
                        NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE);
    ifinfo = ofpbuf_put_zeros(&request, sizeof(struct ifinfomsg));
    ifinfo->ifi_change = ifinfo->ifi_flags = IFF_UP;
    nl_msg_put_string(&request, IFLA_IFNAME, name);
    nl_msg_put_u32(&request, IFLA_MTU, UINT16_MAX);
    linkinfo_off = nl_msg_start_nested(&request, IFLA_LINKINFO);
        nl_msg_put_string(&request, IFLA_INFO_KIND, kind);
        infodata_off = nl_msg_start_nested(&request, IFLA_INFO_DATA);
            nl_msg_put_flag(&request, IFLA_GRE_COLLECT_METADATA);
        nl_msg_end_nested(&request, infodata_off);
    nl_msg_end_nested(&request, linkinfo_off);

    err = nl_transact(NETLINK_ROUTE, &request, &reply);

    if (!err) {
        ofpbuf_uninit(reply);
    }

    if (!err && (err = dpif_rtnetlink_gre_verify(netdev, name, kind))) {
        dpif_rtnetlink_gre_destroy(name);
    }

    ofpbuf_uninit(&request);
    return err;
}

static int
dpif_rtnetlink_gre_create(struct netdev *netdev)
{
    return dpif_rtnetlink_gre_create_kind(netdev, "gretap");
}

static int
dpif_rtnetlink_geneve_verify(struct netdev *netdev, const char *name,
                             const char *kind)
{
    int err;
    struct ofpbuf request, *reply;
    struct ifinfomsg *ifmsg;
    const struct netdev_tunnel_config *tnl_cfg;

    static const struct nl_policy geneve_policy[] = {
        [IFLA_GENEVE_COLLECT_METADATA] = { .type = NL_A_FLAG },
        [IFLA_GENEVE_UDP_ZERO_CSUM6_RX] = { .type = NL_A_U8 },
        [IFLA_GENEVE_PORT] = { .type = NL_A_U16 },
    };

    tnl_cfg = netdev_get_tunnel_config(netdev);
    if (!tnl_cfg) {
        return EINVAL;
    }

    ofpbuf_init(&request, 0);
    nl_msg_put_nlmsghdr(&request, 0, RTM_GETLINK,
                        NLM_F_REQUEST);
    ofpbuf_put_zeros(&request, sizeof(struct ifinfomsg));
    nl_msg_put_string(&request, IFLA_IFNAME, name);

    err = nl_transact(NETLINK_ROUTE, &request, &reply);
    if (!err) {
        struct nlattr *rtlink[ARRAY_SIZE(rtlink_policy)];
        struct nlattr *linkinfo[ARRAY_SIZE(linkinfo_policy)];
        struct nlattr *geneve[ARRAY_SIZE(geneve_policy)];

        ifmsg = ofpbuf_at(reply, NLMSG_HDRLEN, sizeof *ifmsg);
        if (!nl_policy_parse(reply, NLMSG_HDRLEN + sizeof *ifmsg,
                             rtlink_policy, rtlink,
                             ARRAY_SIZE(rtlink_policy)) ||
            !nl_parse_nested(rtlink[IFLA_LINKINFO], linkinfo_policy,
                             linkinfo, ARRAY_SIZE(linkinfo_policy)) ||
            strcmp(nl_attr_get_string(linkinfo[IFLA_INFO_KIND]), kind) ||
            !nl_parse_nested(linkinfo[IFLA_INFO_DATA], geneve_policy, geneve,
                             ARRAY_SIZE(geneve_policy))) {
            err = EINVAL;
        }
        if (!err) {
            if (!nl_attr_get_flag(geneve[IFLA_GENEVE_COLLECT_METADATA]) ||
                1 != nl_attr_get_u8(geneve[IFLA_GENEVE_UDP_ZERO_CSUM6_RX]) ||
                tnl_cfg->dst_port !=
                    nl_attr_get_be16(geneve[IFLA_GENEVE_PORT])) {
                err = EINVAL;
            }
        }
        ofpbuf_uninit(reply);
    }
    ofpbuf_uninit(&request);
    return err;
}

static int
dpif_rtnetlink_geneve_create_kind(struct netdev *netdev, const char *kind)
{
    int err;
    struct ofpbuf request, *reply;
    size_t linkinfo_off, infodata_off;
    char namebuf[NETDEV_VPORT_NAME_BUFSIZE];
    const char *name = netdev_vport_get_dpif_port(netdev,
                                                  namebuf, sizeof namebuf);
    struct ifinfomsg *ifinfo;
    const struct netdev_tunnel_config *tnl_cfg;
    tnl_cfg = netdev_get_tunnel_config(netdev);
    if (!tnl_cfg) {
        return EINVAL;
    }

    ofpbuf_init(&request, 0);
    nl_msg_put_nlmsghdr(&request, 0, RTM_NEWLINK,
                        NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE);
    ifinfo = ofpbuf_put_zeros(&request, sizeof(struct ifinfomsg));
    ifinfo->ifi_change = ifinfo->ifi_flags = IFF_UP;
    nl_msg_put_string(&request, IFLA_IFNAME, name);
    nl_msg_put_u32(&request, IFLA_MTU, UINT16_MAX);
    linkinfo_off = nl_msg_start_nested(&request, IFLA_LINKINFO);
        nl_msg_put_string(&request, IFLA_INFO_KIND, kind);
        infodata_off = nl_msg_start_nested(&request, IFLA_INFO_DATA);
            nl_msg_put_flag(&request, IFLA_GENEVE_COLLECT_METADATA);
            nl_msg_put_u8(&request, IFLA_GENEVE_UDP_ZERO_CSUM6_RX, 1);
            nl_msg_put_be16(&request, IFLA_GENEVE_PORT, tnl_cfg->dst_port);
        nl_msg_end_nested(&request, infodata_off);
    nl_msg_end_nested(&request, linkinfo_off);

    err = nl_transact(NETLINK_ROUTE, &request, &reply);

    if (!err) {
        ofpbuf_uninit(reply);
    }

    if (!err && (err = dpif_rtnetlink_geneve_verify(netdev, name, kind))) {
        dpif_rtnetlink_geneve_destroy(name);
    }

    ofpbuf_uninit(&request);
    return err;
}

static int
dpif_rtnetlink_geneve_create(struct netdev *netdev)
{
    return dpif_rtnetlink_geneve_create_kind(netdev, "geneve");
}

int
dpif_rtnetlink_port_create(struct netdev *netdev)
{
    switch (netdev_to_ovs_vport_type(netdev_get_type(netdev))) {
    case OVS_VPORT_TYPE_VXLAN:
        return dpif_rtnetlink_vxlan_create(netdev);
    case OVS_VPORT_TYPE_GRE:
        return dpif_rtnetlink_gre_create(netdev);
    case OVS_VPORT_TYPE_GENEVE:
        return dpif_rtnetlink_geneve_create(netdev);
    case OVS_VPORT_TYPE_NETDEV:
    case OVS_VPORT_TYPE_INTERNAL:
    case OVS_VPORT_TYPE_LISP:
    case OVS_VPORT_TYPE_STT:
    case OVS_VPORT_TYPE_UNSPEC:
    case __OVS_VPORT_TYPE_MAX:
    default:
        return EOPNOTSUPP;
    }
    return 0;
}

int
dpif_rtnetlink_port_destroy(const char *name, const char *type)
{
    switch (netdev_to_ovs_vport_type(type)) {
    case OVS_VPORT_TYPE_VXLAN:
        return dpif_rtnetlink_vxlan_destroy(name);
    case OVS_VPORT_TYPE_GRE:
        return dpif_rtnetlink_gre_destroy(name);
    case OVS_VPORT_TYPE_GENEVE:
        return dpif_rtnetlink_geneve_destroy(name);
    case OVS_VPORT_TYPE_NETDEV:
    case OVS_VPORT_TYPE_INTERNAL:
    case OVS_VPORT_TYPE_LISP:
    case OVS_VPORT_TYPE_STT:
    case OVS_VPORT_TYPE_UNSPEC:
    case __OVS_VPORT_TYPE_MAX:
    default:
        return EOPNOTSUPP;
    }
    return 0;
}

/**
 * This is to probe for whether the modules are out-of-tree (openvswitch) or
 * in-tree (upstream kernel).
 *
 * We probe for "ovs_geneve" via rtnetlink. As long as this returns something
 * other than EOPNOTSUPP we know that the module in use is the out-of-tree one.
 * This will be used to determine what netlink interface to use when creating
 * ports; rtnetlink or compat/genetlink.
 *
 * See ovs_tunnels_out_of_tree
 */
bool
dpif_rtnetlink_probe_oot_tunnels(void)
{
    struct netdev *netdev = NULL;
    bool out_of_tree = false;
    int error;

    error = netdev_open("ovs-system-probe", "geneve", &netdev);
    if (!error) {
        error = dpif_rtnetlink_geneve_create_kind(netdev, "ovs_geneve");
        if (error != EOPNOTSUPP) {
            if (!error) {
                char namebuf[NETDEV_VPORT_NAME_BUFSIZE];
                const char *dp_port;

                dp_port = netdev_vport_get_dpif_port(netdev, namebuf,
                                                     sizeof namebuf);
                dpif_rtnetlink_geneve_destroy(dp_port);
            }
            out_of_tree = true;
        }
        netdev_close(netdev);
        error = 0;
    }

    return out_of_tree;
}
