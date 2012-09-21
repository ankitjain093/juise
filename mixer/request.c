/*
 * $Id$
 *
 * Copyright (c) 2012, Juniper Networks, Inc.
 * All rights reserved.
 * This SOFTWARE is licensed under the LICENSE provided in the
 * ../Copyright file. By downloading, installing, copying, or otherwise
 * using the SOFTWARE, you agree to be bound by the terms of that
 * LICENSE.
 */

#include "local.h"
#include "request.h"
#include "session.h"
#include "channel.h"
#include "netconf.h"

static unsigned mx_request_id; /* Monotonically increasing ID number */

char mx_netconf_tag_open_rpc[] = "<rpc format=\"html\">";
unsigned mx_netconf_tag_open_rpc_len = sizeof(mx_netconf_tag_open_rpc) - 1;
char mx_netconf_tag_close_rpc[] = "</rpc>";
unsigned  mx_netconf_tag_close_rpc_len = sizeof(mx_netconf_tag_close_rpc) - 1;

mx_request_t *
mx_request_create (mx_sock_websocket_t *mswp, mx_buffer_t *mbp UNUSED,
		     const char *tag, const char **attrs)
{
    mx_request_t *mrp;
    char *cp;

    mrp = calloc(1, sizeof(*mrp));
    if (mrp == NULL)
	return NULL;

    mrp->mr_id = ++mx_request_id;

    mrp->mr_muxid = nstrdup(xml_get_attribute(attrs, "muxid"));
    mrp->mr_name = strdup(tag);
    mrp->mr_target = nstrdup(xml_get_attribute(attrs, "target"));
    mrp->mr_user = nstrdup(xml_get_attribute(attrs, "user"));
    mrp->mr_password = nstrdup(xml_get_attribute(attrs, "password"));

    if (mrp->mr_user == NULL && mrp->mr_target) {
	cp = index(mrp->mr_target, '@');
	if (cp) {
	    mrp->mr_user = mrp->mr_target;
	    *cp++ = '\0';
	    mrp->mr_target = strdup(cp);
	}
    }

    mx_log("R%u request %s '%s' from S%u, target %s, user %s",
	   mrp->mr_id, mrp->mr_name, mrp->mr_muxid,
	   mswp->msw_base.ms_id, mrp->mr_target, mrp->mr_user);

    return mrp;

#if 0
 fatal:
    free(mrp);
    return NULL;
#endif
}

/*
 * We need to add framing to the front and end of the RPC, but we really
 * want to put all the content into one mx_buffer_t.  So if it fits, that's
 * what we do.
 */
static mx_buffer_t *
mx_netconf_insert_framing (mx_buffer_t *mbp)
{
    const unsigned close_len
	= mx_netconf_tag_close_rpc_len + mx_netconf_marker_len;
    mx_buffer_t *newp = mbp;
    int fresh = FALSE, blen, len;

    if (mbp->mb_next != NULL)
	fresh = TRUE;
    else if (mbp->mb_start >= mx_netconf_tag_open_rpc_len) {
	fresh =  (mbp->mb_size - (mbp->mb_start + mbp->mb_len) < close_len);
    } else {
	fresh =  (mbp->mb_size - (mbp->mb_start + mbp->mb_len)
		  < close_len + mx_netconf_tag_open_rpc_len);
    }

    if (fresh) {
	blen = mx_netconf_tag_open_rpc_len;
	blen += mbp->mb_len;
	blen += mx_netconf_tag_close_rpc_len;
	blen += mx_netconf_marker_len;

	newp = mx_buffer_create(blen);
	if (newp == NULL)
	    return NULL;

	len = mx_netconf_tag_open_rpc_len;
	memcpy(newp->mb_data, mx_netconf_tag_open_rpc, len);
	blen = len;

	len = mbp->mb_len;
	memcpy(newp->mb_data + blen, mbp->mb_data + mbp->mb_start, len);
	blen += len;

	len = mx_netconf_tag_close_rpc_len;
	memcpy(newp->mb_data + blen, mx_netconf_tag_close_rpc, len);
	blen += len;

	len = mx_netconf_marker_len;
	memcpy(newp->mb_data + blen, mx_netconf_marker, len);
	blen += len;

	newp->mb_len = blen;

	return newp;
    }

    /* Find or make room for the open <rpc> tag */
    if (mbp->mb_start < mx_netconf_tag_open_rpc_len) {
	memmove(mbp->mb_data + mbp->mb_start,
		mbp->mb_data + mx_netconf_tag_open_rpc_len,
		mbp->mb_len);
	mbp->mb_start = 0;
    } else {
	mbp->mb_start -= mx_netconf_tag_open_rpc_len;
	mbp->mb_len += mx_netconf_tag_open_rpc_len;
    }

    /* Prepend the open <rpc> tag */
    memcpy(mbp->mb_data + mbp->mb_start, mx_netconf_tag_open_rpc,
	   mx_netconf_tag_open_rpc_len);

    /* Append the close </rpc> tag and the ]]>]]> framing marker */
    memcpy(mbp->mb_data + mbp->mb_start + mbp->mb_len,
	   mx_netconf_tag_close_rpc,
	   mx_netconf_tag_close_rpc_len);
    mbp->mb_len += mx_netconf_tag_close_rpc_len;

    memcpy(mbp->mb_data + mbp->mb_start + mbp->mb_len, mx_netconf_marker,
	   mx_netconf_marker_len);
    mbp->mb_len += mx_netconf_marker_len;

    return newp;
}

static int
mx_request_rpc_send (mx_sock_websocket_t *mswp, mx_buffer_t *mbp,
		     mx_request_t *mrp, mx_channel_t *mcp)
{
    mx_log("R%u S%u/C%u sending rpc %.*s",
	   mrp->mr_id, mswp->msw_base.ms_id, mcp->mc_id,
	   (int) mbp->mb_len, mbp->mb_data + mbp->mb_start);

    ssize_t len;
    ssize_t blen = 0;

    mx_buffer_t *newp = mx_netconf_insert_framing(mbp);

    len = libssh2_channel_write(mcp->mc_channel,
				newp->mb_data + newp->mb_start,
				newp->mb_len + blen);
    mx_log("R%u S%u/C%u send rpc, len %d",
	   mrp->mr_id, mswp->msw_base.ms_id, mcp->mc_id, (int) len);

    if (newp != mbp)
	mx_buffer_free(newp);

    return FALSE;
}

int
mx_request_start_rpc (mx_sock_websocket_t *mswp, mx_buffer_t *mbp,
		      mx_request_t *mrp)
{
    mx_log("R%u %s '%s' on S%u, target '%s'",
	   mrp->mr_id, mrp->mr_name, mrp->mr_muxid,
	   mswp->msw_base.ms_id, mrp->mr_target);

    mx_sock_session_t *mssp = mx_session(&mswp->msw_base, mrp);
    if (mssp == NULL) {
	/* XXX send failure message */
	return TRUE;
    }

    if (mssp->mss_base.ms_state != MSS_ESTABLISHED)
	return TRUE;

    mx_channel_t *mcp = mx_channel_netconf(mssp, &mswp->msw_base, TRUE);
    if (mcp) {
	mx_log("C%u running R%u '%s' target '%s'",
	       mcp->mc_id, mrp->mr_id, mrp->mr_name, mrp->mr_target);
	mx_request_rpc_send(mswp, mbp, mrp, mcp);
    }

    return TRUE;
}

mx_request_t *
mx_request_find (mx_sock_websocket_t *mswp, const char *muxid)
{
    mx_request_t *mrp;

    TAILQ_FOREACH(mrp, &mswp->msw_requests, mr_link) {
	if (streq(mrp->mr_muxid, muxid))
	    return mrp;
    }

    return NULL;
}

void
mx_request_free (mx_request_t *mrp)
{
    if (mrp->mr_muxid) free(mrp->mr_muxid);
    if (mrp->mr_name) free(mrp->mr_name);
    if (mrp->mr_target) free(mrp->mr_target);
    if (mrp->mr_user) free(mrp->mr_user);
    if (mrp->mr_password) free(mrp->mr_password);

    free(mrp);
}