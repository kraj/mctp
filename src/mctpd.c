/* SPDX-License-Identifier: GPL-2.0 */
/*
 * mctpd: bus owner for MCTP using Linux kernel
 *
 * Copyright (c) 2021 Code Construct
 * Copyright (c) 2021 Google
 */

#define _GNU_SOURCE

#include <sys/socket.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <err.h>
#include <errno.h>
#include <getopt.h>

#include <systemd/sd-event.h>
#include <systemd/sd-bus.h>
#include <systemd/sd-id128.h>

#include "mctp.h"
#include "mctp-util.h"
#include "mctp-netlink.h"
#include "mctp-control-spec.h"

#define max(a, b) ((a) > (b) ? (a) : (b))
#define min(a, b) ((a) < (b) ? (a) : (b))

#define MCTP_DBUS_PATH "/xyz/openbmc_project/mctp"
#define CC_MCTP_DBUS_IFACE "au.com.CodeConstruct.MCTP"
#define CC_MCTP_DBUS_IFACE_ENDPOINT "au.com.CodeConstruct.MCTP.Endpoint"
#define CC_MCTP_DBUS_IFACE_TESTING "au.com.CodeConstruct.MCTPTesting"
#define MCTP_DBUS_IFACE "xyz.openbmc_project.MCTP"
#define MCTP_DBUS_IFACE_ENDPOINT "xyz.openbmc_project.MCTP.Endpoint"
#define OPENBMC_IFACE_COMMON_UUID "xyz.openbmc_project.Common.UUID"

static const char* mctpd_appid = "67369c05-4b97-4b7e-be72-65cfd8639f10";

static mctp_eid_t eid_alloc_min = 0x08;
static mctp_eid_t eid_alloc_max = 0xfe;

// arbitrary sanity
static size_t MAX_PEER_SIZE = 1000000;

static const uint8_t RQDI_REQ = 1<<7;
static const uint8_t RQDI_RESP = 0x0;

struct dest_phys {
	int ifindex;
	const uint8_t *hwaddr;
	size_t hwaddr_len;
};
typedef struct dest_phys dest_phys;

/* Table of per-network details */
struct net_det {
	int net;
	// EID mappings, an index into ctx->peers. Value -1 is unused.
	ssize_t peeridx[0xff];
};
typedef struct net_det net_det;

struct ctx;

struct peer {
	int net;
	mctp_eid_t eid;

	dest_phys phys;

	enum {
		UNUSED = 0,
		NEW,
		// Assigned once EID and routing has been set up
		ASSIGNED,
		// Own address placeholder. Only (net, eid) are used.
		// Note that multiple interfaces in a network may have
		// the same local address.
		LOCAL,
		// CONFLICT,
	} state;

	bool have_neigh;
	bool have_route;

	struct ctx *ctx;

	// malloc()ed list of supported message types, from Get Message Type
	uint8_t *message_types;
	size_t num_message_types;

	// from Get Endpoint ID
	uint8_t endpoint_type;
	uint8_t medium_spec;

	// From Get Endpoint UUID. A malloced 16 bytes */
	uint8_t *uuid;
};
typedef struct peer peer;

struct ctx {
	sd_event *event;
	sd_bus *bus;
	mctp_nl *nl;

	// Whether we are running as the bus owner
	bool bus_owner;

	// An allocated array of peers, changes address (reallocated) during runtime
	peer *peers;
	size_t size_peers;

	struct net_det *nets;
	size_t num_nets;

	// Timeout in usecs for a MCTP response
	uint64_t mctp_timeout;

	uint8_t uuid[16];

	// Verbose logging
	bool verbose;
	bool testing;
};
typedef struct ctx ctx;

static int emit_endpoint_added(const peer *peer);
static int emit_endpoint_removed(const peer *peer);
static int query_peer_properties(peer *peer);
static int setup_added_peer(peer *peer);
static int peer_route_update(peer *peer, uint16_t type);
static int peer_neigh_update(peer *peer, uint16_t type);

mctp_eid_t local_addr(const ctx *ctx, int ifindex) {
	mctp_eid_t *eids, ret = 0;
	size_t num;

	eids = mctp_nl_addrs_byindex(ctx->nl, ifindex, &num);
	if (num)
		ret = eids[0];
	free(eids);
	return ret;
}

static void* dfree(void* ptr);

static net_det *lookup_net(ctx *ctx, int net)
{
	size_t i;
	for (i = 0; i < ctx->num_nets; i++)
		if (ctx->nets[i].net == net)
			return &ctx->nets[i];
	return NULL;
}

static bool match_phys(const dest_phys *d1, const dest_phys *d2) {
	return d1->ifindex == d2->ifindex &&
		d1->hwaddr_len == d2->hwaddr_len &&
		!memcmp(d1->hwaddr, d2->hwaddr, d1->hwaddr_len);
}

static peer * find_peer_by_phys(ctx *ctx, const dest_phys *dest)
{
	for (size_t i = 0; i < ctx->size_peers; i++) {
		peer *peer = &ctx->peers[i];
		if (peer->state != ASSIGNED)
			continue;
		if (match_phys(&peer->phys, dest))
			return peer;
	}
	return NULL;
}

static peer * find_peer_by_addr(ctx *ctx, mctp_eid_t eid, int net)
{
	net_det *n = lookup_net(ctx, net);

	if (eid != 0 && n && n->peeridx[eid] >= 0)
		return &ctx->peers[n->peeridx[eid]];
	return NULL;
}

/* Returns a deferred free pointer */
static const char* dest_phys_tostr(const dest_phys *dest)
{
	char hex[MAX_ADDR_LEN*4];
	char* buf;
	size_t l = 50 + sizeof(hex);
	buf = malloc(l);
	if (!buf) {
		return "Out of memory";
	}
	write_hex_addr(dest->hwaddr, dest->hwaddr_len, hex, sizeof(hex));
	snprintf(buf, l, "physaddr if %d hw len %zu 0x%s", dest->ifindex, dest->hwaddr_len, hex);
	return dfree(buf);
}

static const char* ext_addr_tostr(const struct sockaddr_mctp_ext *addr)
{
	char hex[MAX_ADDR_LEN*4];
	char* buf;
	size_t l = 256;
	buf = malloc(l);
	if (!buf) {
		return "Out of memory";
	}

	write_hex_addr(addr->smctp_haddr, addr->smctp_halen, hex, sizeof(hex));
	snprintf(buf, l, "sockaddr_mctp_ext eid %d net %d type 0x%02x if %d hw len %hhu 0x%s",
		addr->smctp_base.smctp_addr.s_addr,
		addr->smctp_base.smctp_network,
		addr->smctp_base.smctp_type,
		addr->smctp_ifindex,
		addr->smctp_halen, hex);
	return dfree(buf);
}

static const char* peer_tostr(const peer *peer)
{
	size_t l = 300;
	char *str = NULL;

	str = malloc(l);
	if (!str) {
		return "Out of memory";
	}
	snprintf(str, l, "peer eid %d net %d phys %s state %d",
		peer->eid, peer->net, dest_phys_tostr(&peer->phys),
		peer->state);
	return dfree(str);
}

static int defer_free_handler(sd_event_source *s, void *userdata)
{
	free(userdata);
	return 0;
}

/* Returns ptr, frees it on the next default event loop cycle (defer)*/
static void* dfree(void* ptr)
{
	sd_event *e;
	int rc;

	if (!ptr)
		return NULL;
	rc = sd_event_default(&e);
	if (rc < 0) {
		warnx("defer_free no event loop");
		return ptr;
	}
	rc = sd_event_add_defer(e, NULL, defer_free_handler, ptr);
	if (rc < 0) {
		warnx("defer_free failed adding");
		return ptr;
	}
	return ptr;
}

static int cb_exit_loop_io(sd_event_source *s, int fd, uint32_t revents, void *userdata)
{
	sd_event_exit(sd_event_source_get_event(s), 0);
	return 0;
}

static int cb_exit_loop_timeout(sd_event_source *s, uint64_t usec, void *userdata)
{
	sd_event_exit(sd_event_source_get_event(s), -ETIMEDOUT);
	return 0;
}

/* Events are EPOLLIN, EPOLLOUT etc.
   Returns 0 on ready, negative on error. -ETIMEDOUT on timeout */
static int wait_fd_timeout(int fd, short events, uint64_t timeout_usec)
{
	int rc;
	sd_event *ev = NULL;

	// Create a new event loop just for the event+timeout
	rc = sd_event_new(&ev);
	if (rc < 0)
		goto out;

	rc = sd_event_add_time_relative(ev, NULL, CLOCK_MONOTONIC,
		timeout_usec, 0, cb_exit_loop_timeout, NULL);
	if (rc < 0)
		goto out;

	rc = sd_event_add_io(ev, NULL, fd, events, cb_exit_loop_io, NULL);
	if (rc < 0)
		goto out;

	// TODO: maybe need to break the loop on SIGINT event too?
	rc = sd_event_loop(ev);

out:
	if (ev)
		sd_event_unref(ev);
	return rc;
}

static int peer_from_path(ctx *ctx, const char* path, peer **ret_peer)
{
	char *netstr = NULL, *eidstr = NULL;
	uint32_t tmp, net;
	mctp_eid_t eid;
	int rc;

	*ret_peer = NULL;
	rc = sd_bus_path_decode_many(path, MCTP_DBUS_PATH "/%/%",
		&netstr, &eidstr);
	if (rc == 0)
		return -ENOENT;
	if (rc < 0)
		return rc;
	dfree(netstr);
	dfree(eidstr);

	if (parse_uint32(eidstr, &tmp) < 0 || tmp > 0xff)
		return -EINVAL;
	eid = tmp & 0xff;

	if (parse_uint32(netstr, &net) < 0)
		return -EINVAL;

	*ret_peer = find_peer_by_addr(ctx, eid, net);
	if (!*ret_peer)
		return -ENOENT;
	return 0;
}

static int path_from_peer(const peer *peer, char ** ret_path) {
	size_t l;
	char* buf;

	if (peer->state == UNUSED || peer->state == NEW) {
		warnx("BUG: %s on peer %s", __func__, peer_tostr(peer));
		return -EPROTO;
	}

	l = strlen(MCTP_DBUS_PATH) + 60;
	buf = malloc(l);
	if (!buf)
		return -ENOMEM;
	/* can't use sd_bus_path_encode_many() since it escapes
	   leading digits */
	snprintf(buf, l, "%s/%d/%d", MCTP_DBUS_PATH,
		peer->net, peer->eid);
	*ret_path = buf;
	return 0;
}


/* Returns the message from a socket.
   ret_buf is allocated, should be freed by the caller */
static int read_message(ctx *ctx, int sd, uint8_t **ret_buf, size_t *ret_buf_size,
		struct sockaddr_mctp_ext *ret_addr)
{
	int rc;
	socklen_t addrlen;
	ssize_t len;
	uint8_t* buf = NULL;
	size_t buf_size;

	len = recvfrom(sd, NULL, 0, MSG_PEEK | MSG_TRUNC, NULL, 0);
	if (len < 0) {
		rc = -errno;
		goto out;
	}

	buf_size = len;
	buf = malloc(buf_size);
	if (!buf) {
		rc = -ENOMEM;
		goto out;
	}

	addrlen = sizeof(struct sockaddr_mctp_ext);
	memset(ret_addr, 0x0, addrlen);
	len = recvfrom(sd, buf, buf_size, MSG_TRUNC, (struct sockaddr *)ret_addr,
		&addrlen);
	if (len < 0) {
		rc = -errno;
		goto out;
	}
	if ((size_t)len != buf_size) {
		warnx("BUG: incorrect recvfrom %zd, expected %zu", len, buf_size);
		rc = -EPROTO;
		goto out;
	}
	if (addrlen != sizeof(struct sockaddr_mctp_ext)) {
		warnx("Unexpected address size %u.", addrlen);
		rc = -EPROTO;
		goto out;
	}

	if (ctx->verbose) {
		warnx("read_message got from %s len %zu",
			ext_addr_tostr(ret_addr),
			buf_size);
	}

	*ret_buf = buf;
	*ret_buf_size = buf_size;
	rc = 0;
out:
	if (rc < 0) {
		if (ctx->verbose) {
			warnx("read_message returned error: %s", strerror(-rc));
		}
		free(buf);
	}
	return rc;
}

/* Replies to a real EID, not physical addressing */
static int reply_message(ctx *ctx, int sd, const void *resp, size_t resp_len,
	const struct sockaddr_mctp_ext *addr)
{
	ssize_t len;
	struct sockaddr_mctp reply_addr;

	memcpy(&reply_addr, &addr->smctp_base, sizeof(reply_addr));
	reply_addr.smctp_tag &= ~MCTP_TAG_OWNER;

	if (reply_addr.smctp_addr.s_addr == 0 ||
		 reply_addr.smctp_addr.s_addr == 0xff) {
		warnx("BUG: reply_message can't take EID %d",
			reply_addr.smctp_addr.s_addr);
		return -EPROTO;
	}

	len = sendto(sd, resp, resp_len, 0,
		(struct sockaddr*)&reply_addr, sizeof(reply_addr));
	if (len < 0) {
		return -errno;
	}

	if ((size_t)len != resp_len) {
		warnx("BUG: short sendto %zd, expected %zu", len, resp_len);
		return -EPROTO;
	}
	return 0;
}

// Handles new Incoming Set Endpoint ID request
static int handle_control_set_endpoint_id(ctx *ctx,
	int sd, struct sockaddr_mctp_ext *addr,
	const uint8_t *buf, const size_t buf_size)
{
	struct mctp_ctrl_cmd_set_eid *req = NULL;
	struct mctp_ctrl_resp_set_eid respi = {0}, *resp = &respi;
	size_t resp_len;

	if (buf_size < sizeof(*req)) {
		warnx("short Set Endpoint ID message");
		return -ENOMSG;
	}
	req = (void*)buf;

	resp->ctrl_hdr.command_code = req->ctrl_hdr.command_code;
	resp->ctrl_hdr.rq_dgram_inst = RQDI_RESP;
	resp->completion_code = 0;
	resp->status = 0x01 << 4; // Already assigned, TODO
	resp->eid_set = local_addr(ctx, addr->smctp_ifindex);
	resp->eid_pool_size = 0;
	resp_len = sizeof(struct mctp_ctrl_resp_set_eid);

	// TODO: learn busowner route and neigh

	return reply_message(ctx, sd, resp, resp_len, addr);
}

static int handle_control_get_version_support(ctx *ctx,
	int sd, const struct sockaddr_mctp_ext *addr,
	const uint8_t *buf, const size_t buf_size)
{
	struct mctp_ctrl_cmd_get_mctp_ver_support *req = NULL;
	struct mctp_ctrl_resp_get_mctp_ver_support *resp = NULL;
	uint32_t *versions = NULL;
	// space for 4 versions
	uint8_t respbuf[sizeof(*resp) + 4*sizeof(*versions)];
	size_t resp_len;

	if (buf_size < sizeof(struct mctp_ctrl_cmd_get_mctp_ver_support)) {
		warnx("short Get Version Support message");
		return -ENOMSG;
	}

	req = (void*)buf;
	resp = (void*)respbuf;
	versions = (void*)(resp+1);
	switch (req->msg_type_number) {
		case 0xff: // Base Protocol
		case 0x00: // Control protocol
			// from DSP0236 1.3.1  section 12.6.2. Big endian.
			versions[0] = htonl(0xF1F0FF00);
			versions[1] = htonl(0xF1F1FF00);
			versions[2] = htonl(0xF1F2FF00);
			versions[3] = htonl(0xF1F3F100);
			resp->number_of_entries = 4;
			resp->completion_code = 0x00;
			resp_len = sizeof(*resp) + 4*sizeof(*versions);
			break;
		default:
			// Unsupported message type
			resp->completion_code = 0x80;
			resp_len = sizeof(*resp);
	}

	resp->ctrl_hdr.command_code = req->ctrl_hdr.command_code;
	resp->ctrl_hdr.rq_dgram_inst = RQDI_RESP;
	return reply_message(ctx, sd, resp, resp_len, addr);
}

static int handle_control_get_endpoint_id(ctx *ctx,
	int sd, const struct sockaddr_mctp_ext *addr,
	const uint8_t *buf, const size_t buf_size)
{
	struct mctp_ctrl_cmd_get_eid *req = NULL;
	struct mctp_ctrl_resp_get_eid respi = {0}, *resp = &respi;

	if (buf_size < sizeof(*req)) {
		warnx("short Get Endpoint ID message");
		return -ENOMSG;
	}

	req = (void*)buf;
	resp->ctrl_hdr.command_code = req->ctrl_hdr.command_code;
	resp->ctrl_hdr.rq_dgram_inst = RQDI_RESP;

	resp->eid = local_addr(ctx, addr->smctp_ifindex);
	if (ctx->bus_owner)
		SET_ENDPOINT_TYPE(resp->eid_type, MCTP_BUS_OWNER_BRIDGE);
	// 10b = 2 = static EID supported, matches currently assigned.
	SET_ENDPOINT_ID_TYPE(resp->eid_type, 2);
	// TODO: medium specific information

	return reply_message(ctx, sd, resp, sizeof(*resp), addr);
}

static int handle_control_get_endpoint_uuid(ctx *ctx,
	int sd, const struct sockaddr_mctp_ext *addr,
	const uint8_t *buf, const size_t buf_size)
{
	struct mctp_ctrl_cmd_get_uuid *req = NULL;;
	struct mctp_ctrl_resp_get_uuid respi = {0}, *resp = &respi;

	if (buf_size < sizeof(*req)) {
		warnx("short Get Endpoint UUID message");
		return -ENOMSG;
	}

	req = (void*)buf;
	resp->ctrl_hdr.command_code = req->ctrl_hdr.command_code;
	resp->ctrl_hdr.rq_dgram_inst = RQDI_RESP;
	memcpy(resp->uuid, ctx->uuid, sizeof(resp->uuid));
	return reply_message(ctx, sd, resp, sizeof(*resp), addr);
}


static int handle_control_get_message_type_support(ctx *ctx,
	int sd, const struct sockaddr_mctp_ext *addr,
	const uint8_t *buf, const size_t buf_size)
{
	struct mctp_ctrl_cmd_get_msg_type_support *req = NULL;;
	struct mctp_ctrl_resp_get_msg_type_support *resp = NULL;
	uint8_t resp_buf[sizeof(*resp) + 1];

	if (buf_size < sizeof(*req)) {
		warnx("short Get Message Type Support message");
		return -ENOMSG;
	}

	req = (void*)buf;
	resp = (void*)resp_buf;
	resp->ctrl_hdr.command_code = req->ctrl_hdr.command_code;
	resp->ctrl_hdr.rq_dgram_inst = RQDI_RESP;

	// Only control messages supported
	resp->msg_type_count = 1;
	*((uint8_t*)(resp+1)) = MCTP_CTRL_HDR_MSG_TYPE;

	return reply_message(ctx, sd, resp, sizeof(*resp), addr);
}

static int handle_control_resolve_endpoint_id(ctx *ctx,
	int sd, const struct sockaddr_mctp_ext *addr,
	const uint8_t *buf, const size_t buf_size)
{
	struct mctp_ctrl_cmd_resolve_endpoint_id *req = NULL;
	struct mctp_ctrl_resp_resolve_endpoint_id *resp = NULL;
	uint8_t resp_buf[sizeof(*resp) + MAX_ADDR_LEN];
	size_t resp_len;
	peer *peer = NULL;

	if (buf_size < sizeof(*req)) {
		warnx("short Resolve Endpoint ID message");
		return -ENOMSG;
	}

	req = (void*)buf;
	resp = (void*)resp_buf;
	memset(resp, 0x0, sizeof(*resp));
	resp->ctrl_hdr.command_code = req->ctrl_hdr.command_code;
	resp->ctrl_hdr.rq_dgram_inst = RQDI_RESP;

	peer = find_peer_by_addr(ctx, req->eid,
		addr->smctp_base.smctp_network);
	if (!peer) {
		resp->completion_code = 1;
		resp_len = sizeof(*resp);
	} else {
		// TODO: bridging
		resp->eid = req->eid;
		memcpy((void*)(resp+1),
			peer->phys.hwaddr, peer->phys.hwaddr_len);
		resp_len = sizeof(*resp) + peer->phys.hwaddr_len;
	}

	printf("resp_len %zu ... 0x%02x 0x%02x\n", resp_len,
		((uint8_t*)resp)[resp_len-2],
		((uint8_t*)resp)[resp_len-1]);
	return reply_message(ctx, sd, resp, resp_len, addr);
}

static int handle_control_unsupported(ctx *ctx,
	int sd, const struct sockaddr_mctp_ext *addr,
	const uint8_t *buf, const size_t buf_size)
{
	struct mctp_ctrl_msg_hdr *req = NULL;
	struct mctp_ctrl_generic {
		struct mctp_ctrl_msg_hdr ctrl_hdr;
		uint8_t completion_code;
	} __attribute__((__packed__));
	struct mctp_ctrl_generic respi = {0}, *resp = &respi;

	if (buf_size < sizeof(*req)) {
		warnx("short unsupported control message");
		return -ENOMSG;
	}

	req = (void*)buf;
	resp->ctrl_hdr.command_code = req->command_code;
	resp->ctrl_hdr.rq_dgram_inst = RQDI_RESP;
	resp->completion_code = MCTP_CTRL_CC_ERROR_UNSUPPORTED_CMD;
	return reply_message(ctx, sd, resp, sizeof(*resp), addr);
}

static int cb_listen_control_msg(sd_event_source *s, int sd, uint32_t revents,
	void *userdata)
{
	struct sockaddr_mctp_ext addr = {0};
	ctx *ctx = userdata;
	uint8_t *buf = NULL;
	size_t buf_size;
	struct mctp_ctrl_msg_hdr *ctrl_msg = NULL;
	int rc;

	rc = read_message(ctx, sd, &buf, &buf_size, &addr);
	if (rc < 0)
		goto out;

	if (addr.smctp_base.smctp_type != MCTP_CTRL_HDR_MSG_TYPE) {
		warnx("BUG: Wrong message type for listen socket");
		rc = -EINVAL;
		goto out;
	}

	if (buf_size < sizeof(struct mctp_ctrl_msg_hdr)) {
		warnx("Short message %zu bytes from %s",
			buf_size, ext_addr_tostr(&addr));
		rc = -EINVAL;
		goto out;
	}

	ctrl_msg = (void*)buf;
	if (ctx->verbose) {
		warnx("Got control request command code %hhd",
			ctrl_msg->command_code);
	}
	switch (ctrl_msg->command_code) {
		case MCTP_CTRL_CMD_GET_VERSION_SUPPORT:
			rc = handle_control_get_version_support(ctx,
				sd, &addr, buf, buf_size);
			break;
		case MCTP_CTRL_CMD_SET_ENDPOINT_ID:
			rc = handle_control_set_endpoint_id(ctx,
				sd, &addr, buf, buf_size);
			break;
		case MCTP_CTRL_CMD_GET_ENDPOINT_ID:
			rc = handle_control_get_endpoint_id(ctx,
				sd, &addr, buf, buf_size);
			break;
		case MCTP_CTRL_CMD_GET_ENDPOINT_UUID:
			rc = handle_control_get_endpoint_uuid(ctx,
				sd, &addr, buf, buf_size);
			break;
		case MCTP_CTRL_CMD_GET_MESSAGE_TYPE_SUPPORT:
			rc = handle_control_get_message_type_support(ctx,
				sd, &addr, buf, buf_size);
			break;
		case MCTP_CTRL_CMD_RESOLVE_ENDPOINT_ID:
			rc = handle_control_resolve_endpoint_id(ctx,
				sd, &addr, buf, buf_size);
			break;
		default:
			if (ctx->verbose) {
				warnx("Ignoring unsupported command code 0x%02x",
					ctrl_msg->command_code);
				rc = -ENOTSUP;
			}
			rc = handle_control_unsupported(ctx,
				sd, &addr, buf, buf_size);
	}

	if (ctx->verbose && rc < 0) {
		warnx("Error handling command code %02x from %s: %s",
			ctrl_msg->command_code, ext_addr_tostr(&addr),
			strerror(-rc));
	}

out:
	free(buf);
	return 0;
}

static int listen_control_msg(ctx *ctx, int net)
{
	struct sockaddr_mctp addr = { 0 };
	int rc, sd = -1, val;

	sd = socket(AF_MCTP, SOCK_DGRAM, 0);
	if (sd < 0) {
		rc = -errno;
		warn("%s: socket() failed", __func__);
		goto out;
	}

	addr.smctp_family = AF_MCTP;
	addr.smctp_network = net;
	addr.smctp_addr.s_addr = MCTP_ADDR_ANY;
	addr.smctp_type = MCTP_CTRL_HDR_MSG_TYPE;
	addr.smctp_tag = MCTP_TAG_OWNER;

	rc = bind(sd, (struct sockaddr *)&addr, sizeof(addr));
	if (rc < 0) {
		rc = -errno;
		warn("%s: bind() failed", __func__);
		goto out;
	}

	val = 1;
	rc = setsockopt(sd, SOL_MCTP, MCTP_OPT_ADDR_EXT, &val, sizeof(val));
	if (rc < 0) {
		rc = -errno;
		warn("Kernel does not support MCTP extended addressing");
		goto out;
	}

	rc = sd_event_add_io(ctx->event, NULL, sd, EPOLLIN,
		cb_listen_control_msg, ctx);
	return rc;
out:
	if (rc < 0) {
		close(sd);
	}
	return rc;
}

/* Use endpoint_query_peer() or endpoint_query_phys() instead.
 *
 * resp buffer is allocated, caller to free.
 * Extended addressing is used optionally, depending on ext_addr arg. */
static int endpoint_query_addr(ctx *ctx,
	const struct sockaddr_mctp_ext *req_addr, bool ext_addr,
	const void* req, size_t req_len,
	uint8_t **resp, size_t *resp_len, struct sockaddr_mctp_ext *resp_addr)
{
	size_t req_addr_len;
	int sd = -1, val;
	ssize_t rc;
	size_t buf_size;

	uint8_t* buf = NULL;

	*resp = NULL;
	*resp_len = 0;

	sd = socket(AF_MCTP, SOCK_DGRAM, 0);
	if (sd < 0) {
		warn("socket");
		rc = -errno;
		goto out;
	}

	// We want extended addressing on all received messages
	val = 1;
	rc = setsockopt(sd, SOL_MCTP, MCTP_OPT_ADDR_EXT, &val, sizeof(val));
	if (rc < 0) {
		rc = -errno;
		warn("Kernel does not support MCTP extended addressing");
		goto out;
	}

	if (ext_addr) {
		req_addr_len = sizeof(struct sockaddr_mctp_ext);
	} else {
		req_addr_len = sizeof(struct sockaddr_mctp);
	}

	if (req_len == 0) {
		warnx("BUG: zero length request");
		rc = -EPROTO;
		goto out;
	}
	rc = sendto(sd, req, req_len, 0, (struct sockaddr*)req_addr, req_addr_len);
	if (rc < 0) {
		rc = -errno;
		if (ctx->verbose) {
			warnx("%s: sendto(%s) %zu bytes failed. %s", __func__,
				ext_addr_tostr(req_addr), req_len,
				strerror(errno));
		}
		goto out;
	}
	if ((size_t)rc != req_len) {
		warnx("BUG: incorrect sendto %zd, expected %zu", rc, req_len);
		rc = -EPROTO;
		goto out;
	}

	rc = wait_fd_timeout(sd, EPOLLIN, ctx->mctp_timeout);
	if (rc < 0) {
		if (rc == -ETIMEDOUT && ctx->verbose) {
			warnx("%s: receive timed out from %s", __func__,
				ext_addr_tostr(req_addr));
		}
		goto out;
	}

	rc = read_message(ctx, sd, &buf, &buf_size, resp_addr);
	if (rc < 0) {
		goto out;
	}

	if (resp_addr->smctp_base.smctp_type != req_addr->smctp_base.smctp_type) {
		warnx("Mismatching response type %d for request type %d. dest %s",
			resp_addr->smctp_base.smctp_type,
			req_addr->smctp_base.smctp_type,
			ext_addr_tostr(req_addr));
		rc = -ENOMSG;
	}

	rc = 0;
out:
	close(sd);
	if (rc) {
		free(buf);
	} else {
		*resp = buf;
		*resp_len = buf_size;
	}

	return rc;
}

/* Queries an endpoint peer. Addressing is standard eid/net.
 */
static int endpoint_query_peer(const peer *peer,
	uint8_t req_type, const void* req, size_t req_len,
	uint8_t **resp, size_t *resp_len, struct sockaddr_mctp_ext *resp_addr)
{
	struct sockaddr_mctp_ext addr = {0};

	if (peer->state != ASSIGNED) {
		warnx("BUG: %s bad peer %s", __func__, peer_tostr(peer));
		return -EPROTO;
	}

	addr.smctp_base.smctp_family = AF_MCTP;
	addr.smctp_base.smctp_network = peer->net;
	addr.smctp_base.smctp_addr.s_addr = peer->eid;

	addr.smctp_base.smctp_type = req_type;
	addr.smctp_base.smctp_tag = MCTP_TAG_OWNER;

	return endpoint_query_addr(peer->ctx, &addr, false, req, req_len,
		resp, resp_len, resp_addr);
}

/* Queries an endpoint using physical addressing, null EID.
 */
static int endpoint_query_phys(ctx *ctx, const dest_phys *dest,
	uint8_t req_type, const void* req, size_t req_len,
	uint8_t **resp, size_t *resp_len, struct sockaddr_mctp_ext *resp_addr)
{
	struct sockaddr_mctp_ext addr = {0};

	addr.smctp_base.smctp_family = AF_MCTP;
	addr.smctp_base.smctp_network = 0;
	// Physical addressed requests may receive a response where the
	// source-eid that isn't the same as the dest-eid of the request,
	// for example Set Endpoint Id.
	// The kernel mctp stack has special handling for eid=0 to make sure we
	// can recv a response on the socket, so it's important to set eid=0
	// here in the request.
	addr.smctp_base.smctp_addr.s_addr = 0;

	addr.smctp_ifindex = dest->ifindex;
	addr.smctp_halen = dest->hwaddr_len;
	memcpy(addr.smctp_haddr, dest->hwaddr, dest->hwaddr_len);

	addr.smctp_base.smctp_type = req_type;
	addr.smctp_base.smctp_tag = MCTP_TAG_OWNER;

	return endpoint_query_addr(ctx, &addr, true, req, req_len,
		resp, resp_len, resp_addr);
}

/* returns -ECONNREFUSED if the endpoint returns failure. */
static int endpoint_send_set_endpoint_id(const peer *peer, mctp_eid_t *new_eid)
{
	struct sockaddr_mctp_ext addr;
	struct mctp_ctrl_cmd_set_eid req = {0};
	struct mctp_ctrl_resp_set_eid *resp = NULL;
	int rc;
	uint8_t* buf = NULL;
	size_t buf_size;
	uint8_t stat, alloc;
	const dest_phys *dest = &peer->phys;

	rc = -1;

	req.ctrl_hdr.rq_dgram_inst = RQDI_REQ;
	req.ctrl_hdr.command_code = MCTP_CTRL_CMD_SET_ENDPOINT_ID;
	req.operation = 0; // 00b Set EID. TODO: do we want Force?
	req.eid = peer->eid;
	rc = endpoint_query_phys(peer->ctx, dest, MCTP_CTRL_HDR_MSG_TYPE, &req,
		sizeof(req), &buf, &buf_size, &addr);
	if (rc < 0)
		goto out;

	if (buf_size != sizeof(*resp)) {
		warnx("%s: wrong reply length %zu bytes. dest %s", __func__,
			buf_size, dest_phys_tostr(dest));
		rc = -ENOMSG;
		goto out;
	}
	resp = (void*)buf;

	if (resp->completion_code != 0) {
		// TODO: make this a debug message?
		warnx("Failure completion code 0x%02x from %s",
			resp->completion_code, dest_phys_tostr(dest));
		rc = -ECONNREFUSED;
		goto out;
	}

	stat = resp->status >> 4 & 0x3;
	if (stat == 0x01) {
		// changed eid
	} else if (stat == 0x00) {
		if (resp->eid_set != peer->eid) {
			warnx("%s eid %d replied with different eid %d, but 'accepted'",
				dest_phys_tostr(dest), peer->eid, resp->eid_set);
		}
	} else {
		warnx("%s unexpected status 0x%02x",
			dest_phys_tostr(dest), resp->status);
	}
	*new_eid = resp->eid_set;

	alloc = resp->status & 0x3;
	if (alloc != 0) {
		// TODO for bridges
		warnx("%s requested allocation pool, unimplemented",
			dest_phys_tostr(dest));
	}

	rc = 0;
out:
	free(buf);
	return rc;
}


/* Returns the newly added peer.
 * Error is -EEXISTS if it exists */
static int add_peer(ctx *ctx, const dest_phys *dest, mctp_eid_t eid,
	int net, peer **ret_peer)
{
	ssize_t idx;
	size_t new_size;
	net_det *n;
	void *tmp = NULL;
	peer *peer;

	n = lookup_net(ctx, net);
	if (!n) {
		warnx("BUG: %s Bad net %d", __func__, net);
		return -EPROTO;
	}

	idx = n->peeridx[eid];
	if (n->peeridx[eid] >= 0) {
		if (idx >= (ssize_t)ctx->size_peers) {
			warnx("BUG: Bad index %zu", idx);
			return -EPROTO;
		}
		peer = &ctx->peers[idx];
		if (!match_phys(&peer->phys, dest)) {
			warnx("BUG: %s eid %hhu net %d peer already exists", __func__, eid, net);
			return -EEXIST;
		}
		*ret_peer = peer;
		return 0;
	}

	// Find a slot
	for (idx = 0; idx < (ssize_t)ctx->size_peers; idx++) {
		if (ctx->peers[idx].state == UNUSED) {
			break;
		}
	}
	if (idx == (ssize_t)ctx->size_peers) {
		// Allocate more entries
		new_size = max(20, ctx->size_peers*2);
		if (new_size > MAX_PEER_SIZE) {
			return -ENOSPC;
		}
		tmp = realloc(ctx->peers, new_size * sizeof(*ctx->peers));
		if (!tmp)
			return -ENOMEM;
		ctx->peers = tmp;
		// Zero the new entries
		memset(&ctx->peers[ctx->size_peers], 0x0,
			sizeof(*ctx->peers) * (new_size - ctx->size_peers));
		ctx->size_peers = new_size;
	}

	// Populate it
	peer = &ctx->peers[idx];
	peer->eid = eid;
	peer->net = net;
	memcpy(&peer->phys, dest, sizeof(*dest));
	peer->state = NEW;
	peer->ctx = ctx;

	// Update network eid map
	n->peeridx[eid] = idx;

	*ret_peer = peer;
	return 0;
}

static int check_peer_struct(const peer *peer, const struct net_det *n)
{
	ssize_t idx;
    ctx *ctx = peer->ctx;

	if (n->net != peer->net) {
		warnx("BUG: Mismatching net %d vs peer net %d", n->net, peer->net);
		return -1;
	}

	if (((void*)peer - (void*)ctx->peers) % sizeof(struct peer) != 0) {
		warnx("BUG: Bad address alignment");
		return -1;
	}

	idx = peer - ctx->peers;
	if (idx < 0 || idx > (ssize_t)ctx->size_peers) {
		warnx("BUG: Bad address index");
		return -1;
	}

	if (idx != n->peeridx[peer->eid]) {
		warnx("BUG: Bad net %d peeridx 0x%zx vs 0x%zx",
			peer->net, n->peeridx[peer->eid], idx);
		return -1;
	}

	return 0;
}

static int remove_peer(peer *peer)
{
	int rc;
	net_det *n = NULL;

	if (peer->state == UNUSED) {
		warnx("BUG: %s: unused peer", __func__);
		return -EPROTO;
	}

	n = lookup_net(peer->ctx, peer->net);
	if (!n) {
		warnx("BUG: %s: Bad net %d", __func__, peer->net);
		return -EPROTO;
	}

	if (check_peer_struct(peer, n) != 0) {
		warnx("BUG: %s: Inconsistent state", __func__);
		return -EPROTO;
	}

	if (peer->state == ASSIGNED) {
		if (peer->have_neigh) {
			rc = peer_neigh_update(peer, RTM_DELNEIGH);
			if (rc < 0)
				warnx("Failed removing neigh for %s: %s",
					peer_tostr(peer),
					strerror(-rc));
		}

		if (peer->have_route) {
			rc = peer_route_update(peer, RTM_DELROUTE);
			if (rc < 0)
				warnx("Failed removing route for %s: %s",
					peer_tostr(peer),
					strerror(-rc));
		}

		emit_endpoint_removed(peer);
	}

	// Clear it
	n->peeridx[peer->eid] = -1;
	free(peer->message_types);
	free(peer->uuid);
	memset(peer, 0x0, sizeof(struct peer));
	return 0;
}

/* Returns -EEXIST if the new_eid is already used */
static int change_peer_eid(peer *peer, mctp_eid_t new_eid) {
	net_det *n = NULL;

	if (peer->state == UNUSED) {
		warnx("BUG: %s: unused peer", __func__);
		return -EPROTO;
	}

	n = lookup_net(peer->ctx, peer->net);
	if (!n) {
		warnx("BUG: %s: Bad net %d", __func__, peer->net);
		return -EPROTO;
	}

	if (check_peer_struct(peer, n) != 0) {
		warnx("BUG: %s: Inconsistent state", __func__);
		return -EPROTO;
	}

	if (n->peeridx[new_eid] != -1)
		return -EEXIST;

	if (peer->state == ASSIGNED)
		emit_endpoint_removed(peer);
	n->peeridx[new_eid] = n->peeridx[peer->eid];
	n->peeridx[peer->eid] = -1;
	peer->eid = new_eid;
	if (peer->state == ASSIGNED)
		emit_endpoint_added(peer);

	return 0;
}

static int peer_set_mtu(ctx *ctx, peer *peer, uint32_t mtu) {
	const char* ifname = NULL;
	int rc;

	ifname = mctp_nl_if_byindex(ctx->nl, peer->phys.ifindex);
	if (!ifname) {
		warnx("BUG %s: no interface for ifindex %d",
			__func__, peer->phys.ifindex);
		return -EPROTO;
	}

	rc = mctp_nl_route_del(ctx->nl, peer->eid, ifname);
	if (rc < 0 && rc != -ENOENT) {
		warnx("%s, Failed removing existing route for eid %d %s",
			__func__,
			peer->phys.ifindex, ifname);
		// Continue regardless, route_add will likely fail with EEXIST
	}

	rc = mctp_nl_route_add(ctx->nl, peer->eid, ifname, mtu);
	return rc;
}

static int endpoint_assign_eid(ctx *ctx, sd_bus_error *berr, const dest_phys *dest,
	peer **ret_peer)
{
	mctp_eid_t e, new_eid;
	net_det *n = NULL;
	peer *peer = NULL;
	int net;
	int rc;

	net = mctp_nl_net_byindex(ctx->nl, dest->ifindex);
	if (net <= 0) {
		warnx("BUG: No net known for ifindex %d", dest->ifindex);
		return -EPROTO;
	}

	n = lookup_net(ctx, net);
	if (!n) {
		warnx("BUG: Unknown net %d", net);
		return -EPROTO;
	}

	/* Find an unused EID */
	for (e = eid_alloc_min; e <= eid_alloc_max; e++) {
		if (n->peeridx[e] == -1) {
			rc = add_peer(ctx, dest, e, net, &peer);
			if (rc < 0)
				return rc;
			break;
		}
	}
	if (e > eid_alloc_max) {
		warnx("Ran out of EIDs for net %d, allocating %s", net, dest_phys_tostr(dest));
		sd_bus_error_setf(berr, SD_BUS_ERROR_FAILED,
			"Ran out of EIDs");
		return -EADDRNOTAVAIL;
	}

	rc = endpoint_send_set_endpoint_id(peer, &new_eid);
	if (rc == -ECONNREFUSED)
		sd_bus_error_setf(berr, SD_BUS_ERROR_FAILED,
			"Endpoint returned failure to Set Endpoint ID");
	if (rc < 0) {
		remove_peer(peer);
		return rc;
	}

	if (new_eid != peer->eid) {
		rc = change_peer_eid(peer, new_eid);
		if (rc == -EEXIST) {
			sd_bus_error_setf(berr, SD_BUS_ERROR_FAILED,
				"Endpoint requested EID %d instead of assigned %d, already used",
				new_eid, peer->eid);
		}
		if (rc < 0) {
			remove_peer(peer);
			return rc;
		}
	}

	rc = setup_added_peer(peer);
	if (rc < 0)
		return rc;
	*ret_peer = peer;

	return 0;
}

/* Populates a sd_bus_error based on mctpd's convention for error codes.
 * Does nothing if berr is already set.
 */
static void set_berr(ctx *ctx, int errcode, sd_bus_error *berr) {
	bool existing = false;

	if (sd_bus_error_is_set(berr)) {
		existing = true;
	} else switch (errcode) {
		case 0:
			break;
		case -ETIMEDOUT:
			sd_bus_error_setf(berr, SD_BUS_ERROR_FAILED,
				"MCTP Endpoint did not respond");
			break;
		case -ECONNREFUSED:
			// MCTP_CTRL_CC_ERROR or others
			sd_bus_error_setf(berr, SD_BUS_ERROR_FAILED,
				"MCTP Endpoint replied with failure");
			break;
		case -EBUSY:
			// MCTP_CTRL_CC_ERROR_NOT_READY
			sd_bus_error_setf(berr, SD_BUS_ERROR_FAILED,
				"MCTP Endpoint busy");
			break;
		case -ENOTSUP:
			// MCTP_CTRL_CC_ERROR_UNSUPPORTED_CMD
			sd_bus_error_setf(berr, SD_BUS_ERROR_FAILED,
				"Endpoint replied 'unsupported'");
			break;
		case -EPROTO:
			// BUG
			sd_bus_error_setf(berr, SD_BUS_ERROR_FAILED,
				"Internal error");
			break;
		default:
			sd_bus_error_setf(berr, SD_BUS_ERROR_FAILED,
				"Request failed");
			break;
	}

	if (ctx->verbose && sd_bus_error_is_set(berr)) {
		if (existing) {
			warnx("Returning existing dbus error '%s'. ignored errcode=%d (%s)",
				berr->message, errcode, strerror(-errcode));
		} else {
			warnx("Returning dbus error '%s', errcode=%d (%s)",
				berr->message, errcode, strerror(-errcode));
		}
	}
}

static int query_get_endpoint_id(ctx *ctx, const dest_phys *dest,
	mctp_eid_t *ret_eid, uint8_t *ret_ep_type, uint8_t *ret_media_spec)
{
	struct sockaddr_mctp_ext addr;
	struct mctp_ctrl_cmd_get_eid req = {0};
	struct mctp_ctrl_resp_get_eid *resp = NULL;
	uint8_t *buf = NULL;
	size_t buf_size;
	int rc;

	req.ctrl_hdr.rq_dgram_inst = RQDI_REQ;
	req.ctrl_hdr.command_code = MCTP_CTRL_CMD_GET_ENDPOINT_ID;
	rc = endpoint_query_phys(ctx, dest, MCTP_CTRL_HDR_MSG_TYPE, &req,
		sizeof(req), &buf, &buf_size, &addr);
	if (rc < 0)
		goto out;

	if (buf_size != sizeof(*resp)) {
		warnx("%s: wrong reply length %zu bytes. dest %s", __func__, buf_size,
			dest_phys_tostr(dest));
		rc = -ENOMSG;
		goto out;
	}
	resp = (void*)buf;

	if (resp->completion_code != 0) {
		warnx("Failure completion code 0x%02x from %s",
			resp->completion_code, dest_phys_tostr(dest));
		rc = -ECONNREFUSED;
		goto out;
	}

	*ret_eid = resp->eid;
	*ret_ep_type = resp->eid_type;
	*ret_media_spec = resp->medium_data;
out:
	free(buf);
	return rc;
}

/* Returns the peer associated with the endpoint.
 * Returns NULL if the endpoint successfully replies "not yet assigned".
 */
static int get_endpoint_peer(ctx *ctx, sd_bus_error *berr,
	const dest_phys *dest, peer **ret_peer)
{
	mctp_eid_t eid;
	uint8_t ep_type, medium_spec;
	peer *peer = NULL;
	int net;
	int rc;

	*ret_peer = NULL;
	rc = query_get_endpoint_id(ctx, dest, &eid, &ep_type, &medium_spec);
	if (rc < 0)
		return rc;

	net = mctp_nl_net_byindex(ctx->nl, dest->ifindex);
	if (net < 1) {
		return -EPROTO;
	}

	peer = find_peer_by_phys(ctx, dest);
	if (peer) {
		/* Existing entry */
		if (peer->state != ASSIGNED) {
			warnx("BUG: Bad state %d for peer, eid %d",
				peer->state, peer->eid);
			return -EPROTO;
		}

		if (eid == 0) {
			// EID not yet assigned
			remove_peer(peer);
			return 0;
		} else if (peer->eid != eid) {
			rc = change_peer_eid(peer, eid);
			if (rc == -EEXIST)
				return sd_bus_error_setf(berr, SD_BUS_ERROR_FAILED,
					"Endpoint previously EID %d claimed EID %d which is already used",
					peer->eid, eid);
			else if (rc < 0)
				return rc;
		}
	} else {
		if (eid == 0) {
			// Not yet assigned.
			return 0;
		}
		/* New endpoint */
		rc = add_peer(ctx, dest, eid, net, &peer);
		if (rc == -EEXIST)
			return sd_bus_error_setf(berr, SD_BUS_ERROR_FAILED,
					"Endpoint claimed EID %d which is already used",
					eid);
		else if (rc < 0)
			return rc;
	}

	peer->endpoint_type = ep_type;
	peer->medium_spec = medium_spec;
	rc = setup_added_peer(peer);
	if (rc < 0)
		return rc;

	*ret_peer = peer;
	return 0;
}

static int query_get_peer_msgtypes(peer *peer) {
	struct sockaddr_mctp_ext addr;
	struct mctp_ctrl_cmd_get_msg_type_support req;
	struct mctp_ctrl_resp_get_msg_type_support *resp = NULL;
	uint8_t* buf = NULL;
	size_t buf_size, expect_size;
	int rc;

	peer->num_message_types = 0;
	free(peer->message_types);
	peer->message_types = NULL;

	req.ctrl_hdr.rq_dgram_inst = RQDI_REQ;
	req.ctrl_hdr.command_code = MCTP_CTRL_CMD_GET_MESSAGE_TYPE_SUPPORT;

	rc = endpoint_query_peer(peer, MCTP_CTRL_HDR_MSG_TYPE,
		&req, sizeof(req), &buf, &buf_size, &addr);
	if (rc < 0)
		goto out;

	if (buf_size < sizeof(*resp)) {
		warnx("%s: short reply %zu bytes. dest %s", __func__, buf_size,
			peer_tostr(peer));
		rc = -ENOMSG;
		goto out;
	}
	resp = (void*)buf;
	expect_size = sizeof(resp) + resp->msg_type_count;
	if (buf_size != expect_size) {
		warnx("%s: bad reply length. got %zu, expected %zu, %d entries. dest %s",
			__func__, buf_size, expect_size, resp->msg_type_count,
			peer_tostr(peer));
		rc = -ENOMSG;
		goto out;
	}

	if (resp->completion_code != 0x00) {
		rc = -ECONNREFUSED;
		goto out;
	}

	peer->num_message_types = resp->msg_type_count;
	peer->message_types = malloc(resp->msg_type_count);
	if (!peer->message_types) {
		rc = -ENOMEM;
		goto out;
	}
	memcpy(peer->message_types, (void*)(resp+1), resp->msg_type_count);
	rc = 0;
out:
	free(buf);
	return rc;
}

static int peer_set_uuid(peer *peer, const uint8_t uuid[16])
{
	if (!peer->uuid) {
		peer->uuid = malloc(16);
		if (!peer->uuid)
			return -ENOMEM;
	}
	memcpy(peer->uuid, uuid, 16);
	return 0;
}

static int query_get_peer_uuid(peer *peer) {
	struct sockaddr_mctp_ext addr;
	struct mctp_ctrl_cmd_get_uuid req;
	struct mctp_ctrl_resp_get_uuid *resp = NULL;
	uint8_t* buf = NULL;
	size_t buf_size;
	int rc;

	if (peer->state != ASSIGNED) {
		warnx("%s: Wrong state for peer %s", __func__, peer_tostr(peer));
		return -EPROTO;
	}

	req.ctrl_hdr.rq_dgram_inst = RQDI_REQ;
	req.ctrl_hdr.command_code = MCTP_CTRL_CMD_GET_ENDPOINT_UUID;

	rc = endpoint_query_peer(peer, MCTP_CTRL_HDR_MSG_TYPE,
		&req, sizeof(req), &buf, &buf_size, &addr);
	if (rc < 0)
		goto out;

	if (buf_size != sizeof(*resp)) {
		warnx("%s: wrong reply %zu bytes. dest %s", __func__, buf_size,
			peer_tostr(peer));
		rc = -ENOMSG;
		goto out;
	}
	resp = (void*)buf;

	if (resp->completion_code != 0x00) {
		rc = -ECONNREFUSED;
		goto out;
	}

	rc = peer_set_uuid(peer, resp->uuid);
	if (rc < 0)
		goto out;
	rc = 0;

out:
	free(buf);
	return rc;
}

static int validate_dest_phys(ctx *ctx, const dest_phys *dest)
{
	if (dest->hwaddr_len > MAX_ADDR_LEN) {
		warnx("bad hwaddr_len %zu", dest->hwaddr_len);
		return -EINVAL;
	}
	if (dest->ifindex <= 0) {
		warnx("bad ifindex %d", dest->ifindex);
		return -EINVAL;
	}
	if (mctp_nl_net_byindex(ctx->nl, dest->ifindex) <= 0) {
		warnx("unknown ifindex %d", dest->ifindex);
		return -EINVAL;
	}
	return 0;
}

/* SetupEndpoint method tries the following in order:
  - return a peer that already is known
  - request Get Endpoint ID to add to the known table, return that
  - request Set Endpoint ID, return that */
static int method_setup_endpoint(sd_bus_message *call, void *data, sd_bus_error *berr)
{
	int rc;
	const char *ifname = NULL;
	dest_phys desti, *dest = &desti;
	char *peer_path = NULL;
	ctx *ctx = data;
	peer *peer = NULL;

	rc = sd_bus_message_read(call, "s", &ifname);
	if (rc < 0)
		goto err;

	rc = sd_bus_message_read_array(call, 'y',
		(const void**)&dest->hwaddr, &dest->hwaddr_len);
	if (rc < 0)
		goto err;

	dest->ifindex = mctp_nl_ifindex_byname(ctx->nl, ifname);
	if (dest->ifindex <= 0)
		return sd_bus_error_setf(berr, SD_BUS_ERROR_INVALID_ARGS,
			"Unknown MCTP ifname '%s'", ifname);

	rc = validate_dest_phys(ctx, dest);
	if (rc < 0)
		return sd_bus_error_setf(berr, SD_BUS_ERROR_INVALID_ARGS,
			"Bad physaddr");

	/* Check for existing record */
	peer = find_peer_by_phys(ctx, dest);
	if (peer) {
		if (peer->state != ASSIGNED) {
			warnx("BUG: Bad state for peer %d, eid %d",
				peer->state, peer->eid);
			rc = -EPROTO;
			goto err;
		}

		rc = path_from_peer(peer, &peer_path);
		if (rc < 0)
			goto err;
		return sd_bus_reply_method_return(call, "yisb",
			peer->eid, peer->net, dfree(peer_path), 0);
	}

	/* Get Endpoint ID */
	rc = get_endpoint_peer(ctx, berr, dest, &peer);
	if (rc < 0)
		goto err;

	if (peer) {
		rc = path_from_peer(peer, &peer_path);
		if (rc < 0)
			goto err;
		return sd_bus_reply_method_return(call, "yisb",
			peer->eid, peer->net, dfree(peer_path), 0);
	}


	/* Set Endpoint ID */
	rc = endpoint_assign_eid(ctx, berr, dest, &peer);
	if (rc < 0)
		goto err;

	rc = path_from_peer(peer, &peer_path);
	if (rc < 0)
		goto err;
	return sd_bus_reply_method_return(call, "yisb",
		peer->eid, peer->net, dfree(peer_path), 1);

err:
	set_berr(ctx, rc, berr);
	return rc;
}

static int method_assign_endpoint(sd_bus_message *call, void *data, sd_bus_error *berr)
{
	int rc;
	const char *ifname = NULL;
	dest_phys desti, *dest = &desti;
	char *peer_path = NULL;
	ctx *ctx = data;
	peer *peer = NULL;

	rc = sd_bus_message_read(call, "s", &ifname);
	if (rc < 0)
		goto err;

	rc = sd_bus_message_read_array(call, 'y',
		(const void**)&dest->hwaddr, &dest->hwaddr_len);
	if (rc < 0)
		goto err;

	dest->ifindex = mctp_nl_ifindex_byname(ctx->nl, ifname);
	if (dest->ifindex <= 0)
		return sd_bus_error_setf(berr, SD_BUS_ERROR_INVALID_ARGS,
			"Unknown MCTP ifname '%s'", ifname);

	rc = validate_dest_phys(ctx, dest);
	if (rc < 0)
		return sd_bus_error_setf(berr, SD_BUS_ERROR_INVALID_ARGS,
			"Bad physaddr");

	peer = find_peer_by_phys(ctx, dest);
	if (peer) {
		// Return existing record.
		if (peer->state != ASSIGNED) {
			warnx("BUG: Bad state for peer %d, eid %d",
				peer->state, peer->eid);
			rc = -EPROTO;
			goto err;
		}

		rc = path_from_peer(peer, &peer_path);
		if (rc < 0)
			goto err;
		dfree(peer_path);

		return sd_bus_reply_method_return(call, "yisb",
			peer->eid, peer->net, peer_path, 0);
	}

	rc = endpoint_assign_eid(ctx, berr, dest, &peer);
	if (rc < 0)
		goto err;

	rc = path_from_peer(peer, &peer_path);
	if (rc < 0)
		goto err;
	dfree(peer_path);

	return sd_bus_reply_method_return(call, "yisb",
		peer->eid, peer->net, peer_path, 1);
err:
	set_berr(ctx, rc, berr);
	return rc;
}

static int method_learn_endpoint(sd_bus_message *call, void *data, sd_bus_error *berr)
{
	int rc;
	const char *ifname = NULL;
	char *peer_path = NULL;
	dest_phys desti, *dest = &desti;
	ctx *ctx = data;
	peer *peer = NULL;

	rc = sd_bus_message_read(call, "s", &ifname);
	if (rc < 0)
		goto err;

	rc = sd_bus_message_read_array(call, 'y',
		(const void**)&dest->hwaddr, &dest->hwaddr_len);
	if (rc < 0)
		goto err;

	dest->ifindex = mctp_nl_ifindex_byname(ctx->nl, ifname);
	if (dest->ifindex <= 0)
		return sd_bus_error_setf(berr, SD_BUS_ERROR_INVALID_ARGS,
			"Unknown MCTP ifname '%s'", ifname);

	rc = validate_dest_phys(ctx, dest);
	if (rc < 0)
		return sd_bus_error_setf(berr, SD_BUS_ERROR_INVALID_ARGS,
			"Bad physaddr");

	rc = get_endpoint_peer(ctx, berr, dest, &peer);
	if (rc < 0)
		goto err;
	if (!peer)
		return sd_bus_reply_method_return(call, "yisb", 0, 0, "", 0);

	rc = path_from_peer(peer, &peer_path);
	if (rc < 0)
		goto err;
	dfree(peer_path);
	return sd_bus_reply_method_return(call, "yisb", peer->eid, peer->net,
		peer_path, 1);
err:
	set_berr(ctx, rc, berr);
	return rc;
}

// Query various properties of a peer.
// To be called when a new peer is discovered/assigned, once an EID is known
// and routable.
static int query_peer_properties(peer *peer)
{
	int rc;

	rc = query_get_peer_msgtypes(peer);
	if (rc < 0) {
		// Warn here, it's a mandatory command code.
		// It might be too noisy if some devices don't implement it.
		warnx("Error getting endpoint types for %s. Ignoring error %d %s",
			peer_tostr(peer), rc, strerror(-rc));
		rc = 0;
	}

	rc = query_get_peer_uuid(peer);
	if (rc < 0) {
		if (peer->ctx->verbose)
			warnx("Error getting UUID for %s. Ignoring error %d %s",
				peer_tostr(peer), rc, strerror(-rc));
		rc = 0;
	}

	// TODO: emit property changed? Though currently they are all const.
	return rc;
}

static int peer_neigh_update(peer *peer, uint16_t type)
{
	struct {
		struct nlmsghdr		nh;
		struct ndmsg		ndmsg;
		uint8_t			rta_buff[RTA_SPACE(1) + RTA_SPACE(MAX_ADDR_LEN)];
	} msg = {0};
	size_t rta_len = sizeof(msg.rta_buff);
	struct rtattr *rta = (void*)msg.rta_buff;

	msg.nh.nlmsg_type = type;
	msg.nh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
	msg.ndmsg.ndm_ifindex = peer->phys.ifindex;
	msg.ndmsg.ndm_family = AF_MCTP;
	msg.nh.nlmsg_len = NLMSG_LENGTH(sizeof(msg.ndmsg));
	msg.nh.nlmsg_len += mctp_put_rtnlmsg_attr(&rta, &rta_len,
		NDA_DST, &peer->eid, sizeof(peer->eid));
	msg.nh.nlmsg_len += mctp_put_rtnlmsg_attr(&rta, &rta_len,
		NDA_LLADDR, peer->phys.hwaddr, peer->phys.hwaddr_len);
	return mctp_nl_send(peer->ctx->nl, &msg.nh);
}

static int peer_route_update(peer *peer, uint16_t type)
{
	struct {
		struct nlmsghdr		nh;
		struct rtmsg		rtmsg;
		uint8_t			rta_buff[
					RTA_SPACE(sizeof(mctp_eid_t)) + // eid
					RTA_SPACE(sizeof(int)) + // ifindex
					100 // space for MTU, nexthop etc
					];
	} msg = {0};
	size_t rta_len = sizeof(msg.rta_buff);
	struct rtattr *rta = (void*)msg.rta_buff;

	msg.nh.nlmsg_type = type;
	msg.nh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
	msg.rtmsg.rtm_family = AF_MCTP;
	msg.rtmsg.rtm_type = RTN_UNICAST;
	// TODO add eid range handling?
	msg.rtmsg.rtm_dst_len = 0;
	msg.nh.nlmsg_len = NLMSG_LENGTH(sizeof(msg.rtmsg));
	msg.nh.nlmsg_len += mctp_put_rtnlmsg_attr(&rta, &rta_len,
		RTA_DST, &peer->eid, sizeof(peer->eid));
	msg.nh.nlmsg_len += mctp_put_rtnlmsg_attr(&rta, &rta_len,
		RTA_OIF, &peer->phys.ifindex, sizeof(peer->phys.ifindex));
	// TODO: mtu, metric?
	return mctp_nl_send(peer->ctx->nl, &msg.nh);
}

/* Called when a new peer is discovered. Sets up routes and properties */
static int setup_added_peer(peer *peer)
{
	int rc;

	rc = peer_neigh_update(peer, RTM_NEWNEIGH);
	if (rc < 0 && rc != -EEXIST)
		warnx("Failed adding neigh for %s: %s", peer_tostr(peer),
			strerror(-rc));
	else
		peer->have_neigh = true;

	rc = peer_route_update(peer, RTM_NEWROUTE);
	if (rc < 0 && rc != -EEXIST)
		warnx("Failed adding route for %s: %s", peer_tostr(peer),
			strerror(-rc));
	else
		peer->have_route = true;

	// Ready to send ordinary messages
	peer->state = ASSIGNED;

	rc = query_peer_properties(peer);
	if (rc < 0)
		goto out;

	rc = emit_endpoint_added(peer);
out:
	if (rc < 0) {
		remove_peer(peer);
	}
	return rc;
}

// Testing code
static int method_sendto_phys(sd_bus_message *call, void *data, sd_bus_error *berr)
{
	int rc;
	const char *ifname = NULL;
	struct sockaddr_mctp_ext addr;
	dest_phys desti, *dest = &desti;
	ctx *ctx = data;
	uint8_t type;
	uint8_t *resp = NULL;
	const uint8_t *req = NULL;
	size_t req_len, resp_len;
	sd_bus_message *m = NULL;

	rc = sd_bus_message_read(call, "s", &ifname);
	if (rc < 0)
		goto err;

	rc = sd_bus_message_read_array(call, 'y',
		(const void**)&dest->hwaddr, &dest->hwaddr_len);
	if (rc < 0)
		goto err;

	rc = sd_bus_message_read(call, "y", &type);
	if (rc < 0)
		goto err;

	rc = sd_bus_message_read_array(call, 'y', (const void**)&req, &req_len);
	if (rc < 0)
		goto err;

	dest->ifindex = mctp_nl_ifindex_byname(ctx->nl, ifname);
	if (dest->ifindex <= 0)
		return sd_bus_error_setf(berr, SD_BUS_ERROR_INVALID_ARGS,
			"Unknown MCTP ifname '%s'", ifname);

	rc = validate_dest_phys(ctx, dest);
	if (rc < 0)
		return sd_bus_error_setf(berr, SD_BUS_ERROR_INVALID_ARGS,
			"Bad physaddr");

	rc = endpoint_query_phys(ctx, dest, type, req,
		req_len, &resp, &resp_len, &addr);
	if (rc < 0)
		goto err;

	dfree(resp);
	rc = sd_bus_message_new_method_return(call, &m);
	if (rc < 0)
		goto err;

	rc = sd_bus_message_append(m, "yi",
		addr.smctp_base.smctp_addr,
		addr.smctp_base.smctp_network);
	if (rc < 0)
		goto err;

	rc = sd_bus_message_append_array(m, 'y', resp, resp_len);
	if (rc < 0)
		goto err;

	rc = sd_bus_send(sd_bus_message_get_bus(m), m, NULL);
	sd_bus_message_unref(m);
	return rc;

err:
	set_berr(ctx, rc, berr);
	return rc;
}

static int method_sendto_addr(sd_bus_message *call, void *data, sd_bus_error *berr)
{
	int rc;
	struct sockaddr_mctp_ext req_addr = {0};
	struct sockaddr_mctp_ext addr;
	ctx *ctx = data;
	uint8_t *req = NULL, *resp = NULL;
	size_t req_len, resp_len;
	sd_bus_message *m = NULL;

	req_addr.smctp_base.smctp_family = AF_MCTP;
	req_addr.smctp_base.smctp_tag = MCTP_TAG_OWNER;

	rc = sd_bus_message_read(call, "y", &req_addr.smctp_base.smctp_addr);
	if (rc < 0)
		goto err;

	rc = sd_bus_message_read(call, "i", &req_addr.smctp_base.smctp_network);
	if (rc < 0)
		goto err;

	rc = sd_bus_message_read(call, "y", &req_addr.smctp_base.smctp_type);
	if (rc < 0)
		goto err;

	rc = sd_bus_message_read_array(call, 'y', (const void**)&req, &req_len);
	if (rc < 0)
		goto err;

	rc = endpoint_query_addr(ctx, &req_addr, false, req, req_len,
		&resp, &resp_len, &addr);
	if (rc < 0)
		goto err;

	dfree(resp);
	rc = sd_bus_message_new_method_return(call, &m);
	if (rc < 0)
		goto err;

	rc = sd_bus_message_append_array(m, 'y', resp, resp_len);
	if (rc < 0)
		goto err;

	rc = sd_bus_send(sd_bus_message_get_bus(m), m, NULL);
	sd_bus_message_unref(m);
	return rc;

err:
	set_berr(ctx, rc, berr);
	return rc;
}

static int method_endpoint_remove(sd_bus_message *call, void *data,
	sd_bus_error *berr)
{
	peer *peer = data;
	int rc;
	ctx *ctx = peer->ctx;

	if (peer->state == LOCAL)
		return sd_bus_error_setf(berr, SD_BUS_ERROR_FAILED,
			"Cannot remove mctpd-local endpoint");
	if (peer->state != ASSIGNED) {
		rc = -EPROTO;
		goto out;
	}

	rc = remove_peer(peer);
	if (rc < 0)
		goto out;

	rc = sd_bus_reply_method_return(call, "");
out:
	set_berr(ctx, rc, berr);
	return rc;
}

static int method_endpoint_set_mtu(sd_bus_message *call, void *data,
	sd_bus_error *berr)
{
	peer *peer = data;
	ctx *ctx = peer->ctx;
	int rc;
	uint32_t mtu;

	if (peer->state == LOCAL)
		return sd_bus_error_setf(berr, SD_BUS_ERROR_FAILED,
			"Cannot set local endpoint MTU");
	if (peer->state != ASSIGNED) {
		rc = -EPROTO;
		goto out;
	}

	rc = sd_bus_message_read(call, "u", &mtu);
	if (rc < 0)
		goto out;

	rc = peer_set_mtu(ctx, peer, mtu);
	if (rc < 0)
		goto out;

	rc = sd_bus_reply_method_return(call, "");
out:
	set_berr(ctx, rc, berr);
	return rc;
}

// Testing code
static int cb_test_timer(sd_event_source *s, uint64_t t, void* data)
{
	sd_bus_message *call = data;
	// sd_bus *bus = sd_bus_message_get_bus(call);
	int rc;

	rc = sd_bus_reply_method_return(call, "i", (int)(t % 11111));
	sd_bus_message_unref(call);
	if (rc < 0)
		return rc;
	return 0;
}

static int method_test_timer_async(sd_bus_message *call, void *data, sd_bus_error *sderr)
{
	int rc;
	int seconds;
	ctx *ctx = data;

	rc = sd_bus_message_read(call, "i", &seconds);
	if (rc < 0)
		return rc;

	rc = sd_event_add_time_relative(ctx->event, NULL,
		CLOCK_MONOTONIC, 1000000ULL * seconds, 0,
		cb_test_timer, call);
	if (rc < 0)
		return rc;

	sd_bus_message_ref(call);

	// reply later
	return 1;
}

// Testing code
static int method_test_timer(sd_bus_message *call, void *data, sd_bus_error *sderr)
{
	int rc;
	int seconds;
	// struct ctx *ctx = data;

	rc = sd_bus_message_read(call, "i", &seconds);
	if (rc < 0)
		return rc;

	sleep(seconds);

	rc = sd_bus_reply_method_return(call, "i", seconds*10);
	return rc;
}

static const sd_bus_vtable bus_mctpd_vtable[] = {
	SD_BUS_VTABLE_START(0),

	SD_BUS_METHOD_WITH_NAMES("SetupEndpoint",
		"say",
		SD_BUS_PARAM(ifname)
		SD_BUS_PARAM(physaddr),
		"yisb",
		SD_BUS_PARAM(eid)
		SD_BUS_PARAM(net)
		SD_BUS_PARAM(path)
		SD_BUS_PARAM(new),
		method_setup_endpoint,
		0),

	SD_BUS_METHOD_WITH_NAMES("AssignEndpoint",
		"say",
		SD_BUS_PARAM(ifname)
		SD_BUS_PARAM(physaddr),
		"yisb",
		SD_BUS_PARAM(eid)
		SD_BUS_PARAM(net)
		SD_BUS_PARAM(path)
		SD_BUS_PARAM(new),
		method_assign_endpoint,
		0),

	SD_BUS_METHOD_WITH_NAMES("LearnEndpoint",
		"say",
		SD_BUS_PARAM(ifname)
		SD_BUS_PARAM(physaddr),
		"yisb",
		SD_BUS_PARAM(eid)
		SD_BUS_PARAM(net)
		SD_BUS_PARAM(path)
		SD_BUS_PARAM(found),
		method_learn_endpoint,
		0),
	SD_BUS_VTABLE_END,

};

static const sd_bus_vtable testing_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD_WITH_NAMES("SendToPhys",
		"sayyay",
		SD_BUS_PARAM(ifname)
		SD_BUS_PARAM(physaddr)
		SD_BUS_PARAM(type)
		SD_BUS_PARAM(req),
		"yiay",
		SD_BUS_PARAM(eid)
		SD_BUS_PARAM(net)
		SD_BUS_PARAM(resp),
		method_sendto_phys,
		0),
	SD_BUS_METHOD_WITH_NAMES("SendTo",
		"yiyay",
		SD_BUS_PARAM(eid)
		SD_BUS_PARAM(net)
		SD_BUS_PARAM(type)
		SD_BUS_PARAM(req),
		"ay",
		SD_BUS_PARAM(resp),
		method_sendto_addr,
		0),
	SD_BUS_METHOD_WITH_NAMES("TestTimer",
		"i",
		SD_BUS_PARAM(seconds),
		"i",
		SD_BUS_PARAM(secondsx10),
		method_test_timer,
		0),
	SD_BUS_METHOD_WITH_NAMES("TestTimerAsync",
		"i",
		SD_BUS_PARAM(seconds),
		"i",
		SD_BUS_PARAM(secondsx10),
		method_test_timer_async,
		0),

	SD_BUS_VTABLE_END
};

static int bus_endpoint_get_prop(sd_bus *bus,
		const char *path, const char *interface, const char *property,
		sd_bus_message *reply, void *userdata, sd_bus_error *berr)
{
	peer *peer = userdata;
	int rc;

	if (strcmp(property, "NetworkId") == 0) {
		rc = sd_bus_message_append(reply, "i", peer->net);
	} else if (strcmp(property, "EID") == 0) {
		rc = sd_bus_message_append(reply, "y", peer->eid);
	} else if (strcmp(property, "SupportedMessageTypes") == 0) {
		rc = sd_bus_message_append_array(reply, 'y',
			peer->message_types, peer->num_message_types);
	} else if (strcmp(property, "UUID") == 0 && peer->uuid) {
		const char *s = dfree(bytes_to_uuid(peer->uuid));
		rc = sd_bus_message_append(reply, "s", s);
	} else {
		printf("Unknown property '%s' for %s iface %s\n", property, path, interface);
		rc = -ENOENT;
	}

	return rc;
}

static const sd_bus_vtable bus_endpoint_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_PROPERTY("NetworkId",
			"i",
			bus_endpoint_get_prop,
			0,
			SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("EID",
			"y",
			bus_endpoint_get_prop,
			0,
			SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("SupportedMessageTypes",
			"ay",
			bus_endpoint_get_prop,
			0,
			SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_VTABLE_END
};

static const sd_bus_vtable bus_endpoint_uuid_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_PROPERTY("UUID",
			"s",
			bus_endpoint_get_prop,
			0,
			SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_VTABLE_END
};

static const sd_bus_vtable bus_endpoint_cc_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD_WITH_ARGS("SetMTU",
		SD_BUS_ARGS("u", mtu),
		SD_BUS_NO_RESULT,
		method_endpoint_set_mtu,
		0),
	SD_BUS_METHOD_WITH_ARGS("Remove",
		SD_BUS_NO_ARGS,
		SD_BUS_NO_RESULT,
		method_endpoint_remove,
		0),
	SD_BUS_VTABLE_END
};

static int bus_endpoint_find(sd_bus *bus, const char *path,
	const char *interface, void *userdata, void **ret_found,
	sd_bus_error *ret_error)
{
	ctx *ctx = userdata;
	peer *peer = NULL;
	int rc;

	rc = peer_from_path(ctx, path, &peer);
	if (rc >= 0) {
		*ret_found = peer;
		return 1;
	}
	return 0;
}

/* Common.UUID interface is only added for peers that have a UUID. */
static int bus_endpoint_find_uuid(sd_bus *bus, const char *path,
	const char *interface, void *userdata, void **ret_found,
	sd_bus_error *ret_error)
{
	ctx *ctx = userdata;
	peer *peer = NULL;
	int rc;

	rc = peer_from_path(ctx, path, &peer);
	if (rc >= 0) {
		if (peer->uuid) {
			*ret_found = peer;
			return 1;
		}
	}
	return 0;
}

static int emit_endpoint_added(const peer *peer) {
	char *path = NULL;
	int rc;

	rc = path_from_peer(peer, &path);
	if (rc < 0)
		return rc;
	if (peer->ctx->verbose)
		warnx("%s: %s", __func__, path);
	rc = sd_bus_emit_object_added(peer->ctx->bus, dfree(path));
	// rc = sd_bus_emit_interfaces_added(ctx->bus, dfree(path),
	// 	MCTP_DBUS_IFACE_ENDPOINT, NULL);
	if (rc < 0)
		warnx("%s: error emitting, %s", __func__, strerror(errno));
	return rc;
}

static int emit_endpoint_removed(const peer *peer) {
	char *path = NULL;
	int rc;

	rc = path_from_peer(peer, &path);
	if (rc < 0)
		return rc;
	if (peer->ctx->verbose)
		warnx("%s: %s", __func__, path);
	rc = sd_bus_emit_object_removed(peer->ctx->bus, dfree(path));
	if (rc < 0)
		warnx("%s: error emitting, %s", __func__, strerror(errno));
	return rc;
}

static int bus_mctpd_find(sd_bus *bus, const char *path,
	const char *interface, void *userdata, void **ret_found,
	sd_bus_error *ret_error)
{
	if (strcmp(path, MCTP_DBUS_PATH) == 0) {
		*ret_found = userdata;
		return 1;
	}
	return 0;
}

static int mctpd_dbus_enumerate(sd_bus *bus, const char* path,
	void *data, char ***out, sd_bus_error *err)
{
	ctx *ctx = data;
	size_t num_nodes, i, j;
	char **nodes = NULL;
	int rc;

	// NULL terminator
	num_nodes = 1;
	// .../mctp object
	num_nodes++;

	for (i = 0; i < ctx->size_peers; i++)
		if (ctx->peers[i].state != UNUSED)
			num_nodes++;

	num_nodes += ctx->num_nets;

	nodes = malloc(sizeof(*nodes) * num_nodes);
	if (!nodes) {
		rc = -ENOMEM;
		goto out;
	}

	j = 0;
	nodes[j] = strdup(MCTP_DBUS_PATH);
	if (!nodes[j]) {
		rc = -ENOMEM;
		goto out;
	}
	j++;

	// Peers
	for (i = 0; i < ctx->size_peers; i++) {
		peer *peer = &ctx->peers[i];

		if (peer->state == UNUSED || peer->state == NEW)
			continue;
		// TODO do we need to exclude LOCAL?

		rc = path_from_peer(peer, &nodes[j]);
		if (rc < 0)
			goto out;
		j++;
	}

	// Nets
	for (i = 0; i < ctx->num_nets; i++) {
		size_t l;
		char *buf = NULL;

		l = strlen(MCTP_DBUS_PATH) + 30;
		buf = malloc(l);
		if (!buf) {
			rc = -ENOMEM;
			goto out;
		}
		/* can't use sd_bus_path_encode_many() since it escapes
		   leading digits */
		snprintf(buf, l, "%s/%d", MCTP_DBUS_PATH, ctx->nets[i].net);
		nodes[j] = buf;
		j++;
	}

	// NULL terminator
	nodes[j] = NULL;
	j++;
	rc = 0;
	*out = nodes;
out:
	if (rc < 0) {
		for (i = 0; nodes && i < j; i++) {
			free(nodes[i]);
		}
		free(nodes);
	}

	return rc;
}

static int setup_bus(ctx *ctx)
{
	int rc;

	rc = sd_event_new(&ctx->event);
	if (rc < 0) {
		warnx("sd_event failed");
		goto out;
	}

	rc = sd_bus_default(&ctx->bus);
	if (rc < 0) {
		warnx("Couldn't get bus");
		goto out;
	}

	rc = sd_bus_attach_event(ctx->bus, ctx->event,
		SD_EVENT_PRIORITY_NORMAL);
	if (rc < 0) {
		warnx("Failed attach");
		goto out;
	}

	/* mctp object needs to use _fallback_vtable() since we can't
	   mix non-fallback and fallback vtables on MCTP_DBUS_PATH */
	rc = sd_bus_add_fallback_vtable(ctx->bus, NULL,
					MCTP_DBUS_PATH,
					CC_MCTP_DBUS_IFACE,
					bus_mctpd_vtable,
					bus_mctpd_find,
					ctx);
	if (rc < 0) {
		warnx("Failed dbus object");
		goto out;
	}

	rc = sd_bus_add_fallback_vtable(ctx->bus, NULL,
					MCTP_DBUS_PATH,
					MCTP_DBUS_IFACE_ENDPOINT,
					bus_endpoint_vtable,
					bus_endpoint_find,
					ctx);
	if (rc < 0) {
		warnx("Failed dbus fallback endpoint %s", strerror(-rc));
		goto out;
	}

	rc = sd_bus_add_fallback_vtable(ctx->bus, NULL,
					MCTP_DBUS_PATH,
					CC_MCTP_DBUS_IFACE_ENDPOINT,
					bus_endpoint_cc_vtable,
					bus_endpoint_find,
					ctx);
	if (rc < 0) {
		warnx("Failed dbus fallback endpoint %s", strerror(-rc));
		goto out;
	}


	rc = sd_bus_add_fallback_vtable(ctx->bus, NULL,
					MCTP_DBUS_PATH,
					OPENBMC_IFACE_COMMON_UUID,
					bus_endpoint_uuid_vtable,
					bus_endpoint_find_uuid,
					ctx);
	if (rc < 0) {
		warnx("Failed dbus fallback endpoint uuid %s", strerror(-rc));
		goto out;
	}

	rc = sd_bus_add_object_manager(ctx->bus, NULL, MCTP_DBUS_PATH);
	if (rc < 0) {
		warnx("%s failed %s", __func__, strerror(-rc));
		goto out;
	}

	rc = sd_bus_add_node_enumerator(ctx->bus, NULL,
		MCTP_DBUS_PATH, mctpd_dbus_enumerate, ctx);
	if (rc < 0) {
		warnx("Failed add enumerator");
		goto out;
	}

	// All setup must be complete by here, we might immediately
	// get requests from waiting clients.
	rc = sd_bus_request_name(ctx->bus, MCTP_DBUS_IFACE, 0);
	if (rc < 0) {
		warnx("Failed requesting name %s", MCTP_DBUS_IFACE);
		goto out;
	}

	rc = 0;
out:
	return rc;
}

static int setup_nets(ctx *ctx)
{
	int *netlist = NULL;
	size_t num_nets, i, j, num_ifs;
	int *ifs;
	int rc = -1;

	netlist = mctp_nl_net_list(ctx->nl, &num_nets);
	ctx->nets = calloc(num_nets, sizeof(net_det));
	if (!ctx->nets) {
		warnx("Allocation failed");
		goto out;
	}

	if (num_nets == 0) {
		warnx("No MCTP interfaces");
		goto out;
	}

	for (i = 0; i < num_nets; i++) {
		ctx->nets[i].net = netlist[i];
		for (j = 0; j < 0xff; j++) {
			ctx->nets[i].peeridx[j] = -1;
		}
	}
	ctx->num_nets = num_nets;

	/* Set up local addresses */
	ifs = mctp_nl_if_list(ctx->nl, &num_ifs);
	for (i = 0; i < num_ifs; i++) {
		mctp_eid_t *eids = NULL;
		size_t num;
		peer *peer = NULL;

		if (!mctp_nl_up_byindex(ctx->nl, ifs[i]))
			warnx("Warning, interface %s is down",
				mctp_nl_if_byindex(ctx->nl, ifs[i]));

		eids = mctp_nl_addrs_byindex(ctx->nl, ifs[i], &num);
		for (j = 0; j < num; j++) {
			int net = mctp_nl_net_byindex(ctx->nl, ifs[i]);
			dest_phys dest = { .ifindex = 0 };

			if (net == 0) {
				warnx("No net for ifindex %d", ifs[i]);
				continue;
			}

			peer = find_peer_by_addr(ctx, eids[j], net);
			if (peer) {
				if (peer->state != LOCAL)
					warnx("BUG: Local eid %d net %d already exists?",
						eids[j], net);
				continue;
			}

			rc = add_peer(ctx, &dest, eids[j], net, &peer);
			if (rc < 0) {
				warn("BUG: Error adding local eid %d net %d for ifindex %d",
					eids[j], net, ifs[i]);
				continue;
			}
			peer->state = LOCAL;
			rc = peer_set_uuid(peer, ctx->uuid);
			if (rc < 0) {
				warnx("Failed setting local UUID: %s",
					strerror(-rc));
			}

			// Only advertise supporting control messages
			peer->message_types = malloc(1);
			if (peer->message_types) {
				peer->num_message_types = 1;
				peer->message_types[0] = MCTP_CTRL_HDR_MSG_TYPE;
			} else {
				warnx("Out of memory");
			}
		}
		free(eids);
	}
	free(ifs);

	if (ctx->verbose) {
		mctp_nl_linkmap_dump(ctx->nl);
	}

	rc = 0;
out:
	free(netlist);
	return rc;
}

static int setup_testing(ctx *ctx) {
	int rc;
	dest_phys dest = {};
	peer *peer;
	size_t i, j;

	if (!ctx->testing)
		return 0;

	warnx("Running in development testing mode. Not safe for production");

	if (ctx->num_nets > 0) {
		warnx("Not populating fake MCTP nets, real ones exist");
	} else {
		warnx("Populating fake MCTP nets");

		ctx->num_nets = 2;
		ctx->nets = calloc(ctx->num_nets, sizeof(net_det));
		ctx->nets[0].net = 10;
		ctx->nets[1].net = 12;
		for (j = 0; j < ctx->num_nets; j++)
			for (i = 0; i < 0xff; i++)
				ctx->nets[j].peeridx[i] = -1;

		rc = add_peer(ctx, &dest, 7, 10, &peer);
		if (rc < 0) {
			warnx("%s failed add_peer, %s", __func__, strerror(-rc));
			return rc;
		}
		peer->state = ASSIGNED;
		peer->uuid = malloc(16);
		sd_id128_randomize((void*)peer->uuid);

		rc = add_peer(ctx, &dest, 7, 12, &peer);
		if (rc < 0) {
			warnx("%s failed add_peer, %s", __func__, strerror(-rc));
			return rc;
		}
		peer->state = ASSIGNED;
		peer->num_message_types = 3;
		peer->message_types = malloc(3);
		peer->message_types[0] = 0x00;
		peer->message_types[1] = 0x03;
		peer->message_types[2] = 0x04;

		rc = add_peer(ctx, &dest, 9, 12, &peer);
		if (rc < 0) {
			warnx("%s failed add_peer, %s", __func__, strerror(-rc));
			return rc;
		}
		peer->state = ASSIGNED;
		peer->uuid = malloc(16);
		// a UUID that remains constant across runs
		memcpy(peer->uuid, ctx->uuid, 16);
	}

	/* Add extra interface with test methods */
	rc = sd_bus_add_fallback_vtable(ctx->bus, NULL,
					MCTP_DBUS_PATH,
					CC_MCTP_DBUS_IFACE_TESTING,
					testing_vtable,
					bus_mctpd_find,
					ctx);
	if (rc < 0) {
		warnx("Failed testing dbus object");
		return rc;
	}


	return 0;
}

static void print_usage(ctx *ctx)
{
	fprintf(stderr, "mctpd [-v] [-N]\n");
	fprintf(stderr, "      -v verbose\n");
	fprintf(stderr, "      -N testing mode. Not safe for production\n");
}

static int parse_args(ctx *ctx, int argc, char **argv)
{
	struct option options[] = {
		{ .name = "help", .has_arg = no_argument, .val = 'h' },
		{ .name = "verbose", .has_arg = no_argument, .val = 'v' },
		{ .name = "testing", .has_arg = no_argument, .val = 'N' },
		{ 0 },
	};
	int c;

	for (;;) {
		c = getopt_long(argc, argv, "+hvN", options, NULL);
		if (c == -1)
			break;

		switch (c) {
		case 'N':
			ctx->testing = true;
			break;
		case 'v':
			ctx->verbose = true;
			break;
		case 'h':
		default:
			print_usage(ctx);
			return 255;
		}
	}
	return 0;
}

static int fill_uuid(ctx *ctx)
{
	int rc;
	sd_id128_t appid;
	sd_id128_t *u = (void*)ctx->uuid;

	rc = sd_id128_from_string(mctpd_appid, &appid);
	if (rc < 0) {
		warnx("Failed to get appid");
		return rc;
	}

	rc = sd_id128_get_machine_app_specific(appid, u);
	if (rc >= 0)
		return 0;

	warnx("No machine-id, fallback to boot ID");
	rc = sd_id128_get_boot_app_specific(appid, u);
	if (rc < 0)
		warnx("Failed to get boot ID");

	return rc;
}

static int setup_config(ctx *ctx)
{
	int rc;
	// TODO: this will go in a config file or arguments.
	ctx->mctp_timeout = 250000; // 250ms
	ctx->bus_owner = true;
	rc = fill_uuid(ctx);
	if (rc < 0)
		return rc;
	return 0;
}

int main(int argc, char **argv)
{
	int rc;
	ctx ctxi = {0}, *ctx = &ctxi;

	setlinebuf(stdout);

	setup_config(ctx);

	rc = parse_args(ctx, argc, argv);
	if (rc != 0) {
		return rc;
	}

	ctx->nl = mctp_nl_new(false);
	if (!ctx->nl) {
		warnx("Failed creating netlink object");
		return 1;
	}

	rc = setup_nets(ctx);
	if (rc < 0 && !ctx->testing)
		return 1;

	rc = setup_bus(ctx);
	if (rc < 0) {
		warnx("Error in setup, returned %s %d", strerror(-rc), rc);
		return 1;
	}

	// TODO add net argument
	rc = listen_control_msg(ctx, MCTP_NET_ANY);
	if (rc < 0) {
		warnx("Error in listen, returned %s %d", strerror(-rc), rc);
		if (!ctx->testing)
			return 1;
	}

	rc = setup_testing(ctx);
	if (rc < 0)
		return 1;

	rc = sd_event_loop(ctx->event);
	if (rc < 0) {
		warnx("Error in loop, returned %s %d", strerror(-rc), rc);
		return 1;
	}

	return 0;
}
