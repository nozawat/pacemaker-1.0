/* 
 * Copyright (C) 2004 Andrew Beekhof <andrew@beekhof.net>
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <crm_internal.h>
#include <bzlib.h>
#include <crm/ais.h>
#include <crm/common/cluster.h>
#include <openais/saAis.h>
#include <sys/utsname.h>
#include "stack.h"

enum crm_ais_msg_types text2msg_type(const char *text) 
{
	int type = crm_msg_none;

	CRM_CHECK(text != NULL, return type);
	if(safe_str_eq(text, "ais")) {
		type = crm_msg_ais;
	} else if(safe_str_eq(text, "crm_plugin")) {
		type = crm_msg_ais;
	} else if(safe_str_eq(text, CRM_SYSTEM_CIB)) {
		type = crm_msg_cib;
	} else if(safe_str_eq(text, CRM_SYSTEM_CRMD)) {
		type = crm_msg_crmd;
	} else if(safe_str_eq(text, CRM_SYSTEM_DC)) {
		type = crm_msg_crmd;
	} else if(safe_str_eq(text, CRM_SYSTEM_TENGINE)) {
		type = crm_msg_te;
	} else if(safe_str_eq(text, CRM_SYSTEM_PENGINE)) {
		type = crm_msg_pe;
	} else if(safe_str_eq(text, CRM_SYSTEM_LRMD)) {
		type = crm_msg_lrmd;
	} else {
		crm_debug_2("Unknown message type: %s", text);
	}
	return type;
}

char *get_ais_data(AIS_Message *msg)
{
    int rc = BZ_OK;
    char *uncompressed = NULL;
    unsigned int new_size = msg->size;
    
    if(msg->is_compressed == FALSE) {
	crm_debug_2("Returning uncompressed message data");
	uncompressed = strdup(msg->data);

    } else {
	crm_debug_2("Decompressing message data");
	crm_malloc0(uncompressed, new_size);
	
	rc = BZ2_bzBuffToBuffDecompress(
	    uncompressed, &new_size, msg->data, msg->compressed_size, 1, 0);
	
	CRM_ASSERT(rc = BZ_OK);
	CRM_ASSERT(new_size == msg->size);
    }
    
    return uncompressed;
}


#if SUPPORT_AIS
int ais_fd_sync = -1;
static int ais_fd_async = -1; /* never send messages via this channel */
GFDSource *ais_source = NULL;
GFDSource *ais_source_out = NULL;

gboolean
send_ais_text(int class, const char *data,
	      gboolean local, const char *node, enum crm_ais_msg_types dest)
{
    static int msg_id = 0;
    static int local_pid = 0;

    int rc = SA_AIS_OK;
    AIS_Message *ais_msg = NULL;
    enum crm_ais_msg_types sender = text2msg_type(crm_system_name);

    if(local_pid == 0) {
	local_pid = getpid();
    }

    CRM_CHECK(data != NULL, return FALSE);
    crm_malloc0(ais_msg, sizeof(AIS_Message));
    
    ais_msg->id = msg_id++;
    ais_msg->header.id = class;
    
    ais_msg->host.type = dest;
    ais_msg->host.local = local;
    if(node) {
	ais_msg->host.size = strlen(node);
	memset(ais_msg->host.uname, 0, MAX_NAME);
	memcpy(ais_msg->host.uname, node, ais_msg->host.size);
	ais_msg->host.id = 0;
	
    } else {
	ais_msg->host.size = 0;
	memset(ais_msg->host.uname, 0, MAX_NAME);
	ais_msg->host.id = 0;
    }
    
    ais_msg->sender.type = sender;
    ais_msg->sender.pid = local_pid;
    ais_msg->sender.size = 0;
    memset(ais_msg->sender.uname, 0, MAX_NAME);
    ais_msg->sender.id = 0;
    
    ais_msg->size = 1 + strlen(data);

    if(ais_msg->size < 5120) {
  failback:
	crm_realloc(ais_msg, sizeof(AIS_Message) + ais_msg->size);
	memcpy(ais_msg->data, data, ais_msg->size);
	
    } else {
	char *compressed = NULL;
	char *uncompressed = crm_strdup(data);
	unsigned int len = (ais_msg->size * 1.1) + 600; /* recomended size */
	
	crm_debug_5("Compressing message payload");
	crm_malloc0(compressed, len);
	
	rc = BZ2_bzBuffToBuffCompress(
	    compressed, &len, uncompressed, ais_msg->size, 3, 0, 30);

	crm_free(uncompressed);
	
	if(rc != BZ_OK) {
	    crm_err("Compression failed: %d", rc);
	    crm_free(compressed);
	    goto failback;  
	}

	crm_realloc(ais_msg, sizeof(AIS_Message) + len + 1);
	memcpy(ais_msg->data, compressed, len);
	crm_free(compressed);

	ais_msg->is_compressed = TRUE;
	ais_msg->compressed_size = len;

	crm_debug("Compression details: %d -> %d",
		  ais_msg->size, ais_data_len(ais_msg));
    } 

    ais_msg->header.size = sizeof(AIS_Message) + ais_data_len(ais_msg);

    crm_debug("Sending%s message %d to %s.%s (data=%d, total=%d)",
		ais_msg->is_compressed?" compressed":"",
		ais_msg->id, ais_dest(&(ais_msg->host)), msg_type2text(dest),
		ais_data_len(ais_msg), ais_msg->header.size);

    rc = saSendRetry(ais_fd_sync, ais_msg, ais_msg->header.size);
    if(rc != SA_AIS_OK) {    
	crm_err("Sending message %d: FAILED", ais_msg->id);
	ais_fd_async = -1;
    } else {
	crm_debug_4("Message %d: sent", ais_msg->id);
    }

    crm_free(ais_msg);
    return (rc == SA_AIS_OK);
}

gboolean
send_ais_message(crm_data_t *msg, 
		 gboolean local, const char *node, enum crm_ais_msg_types dest)
{
    gboolean rc = TRUE;
    char *data = NULL;

    if(ais_fd_async < 0 || ais_source == NULL) {
	crm_err("Not connected to AIS");
	return FALSE;
    }

    if(cl_get_string(msg, F_XML_TAGNAME) == NULL) {
	ha_msg_add(msg, F_XML_TAGNAME, "ais_msg");
    }
    
    data = dump_xml_unformatted(msg);
    rc = send_ais_text(0, data, local, node, dest);
    crm_free(data);
    return rc;
}

void terminate_ais_connection(void) 
{
    close(ais_fd_sync);
    close(ais_fd_async);
    crm_notice("Disconnected from AIS");
/*     G_main_del_fd(ais_source); */
/*     G_main_del_fd(ais_source_out);     */
}

static gboolean ais_dispatch(int sender, gpointer user_data)
{
    /* Grab the header */
    int data_len = 0;
    char *data = NULL;
    char *header = NULL;
    AIS_Message *msg = NULL;
    SaAisErrorT rc = SA_AIS_OK;
    static int header_len = sizeof(AIS_Message);
    gboolean (*dispatch)(AIS_Message*,char*,int) = user_data;

    crm_malloc0(header, header_len);
    
    crm_debug_5("Start");
    rc = saRecvRetry(sender, header, header_len);
    if (rc != SA_AIS_OK) {
	crm_warn("Receiving message header failed");
	goto bail;
    }

    msg = (void*)header;

    crm_debug_3("Got new%s message indication (size=%d, %d, %d)",
		msg->is_compressed?" compressed":"",
		ais_data_len(msg), msg->size, msg->compressed_size);

    if(check_message_sanity(msg, NULL) == FALSE) {
	goto badmsg;
    }
    
    data_len = ais_data_len(msg);

#if 0
    crm_malloc0(data, data_len);
#else    
    data = cl_malloc(data_len);
    CRM_CHECK(data != NULL,
	      crm_err("Failed allocation of %d bytes", data_len);
	      goto badmsg);
    memset(data, 0, data_len);
#endif
    rc = saRecvRetry(sender, data, data_len);
    
    if (rc != SA_AIS_OK) {
	crm_err("Receiving message body failed: %d", rc);
	goto bail;
    }
    
    crm_debug_5("Read data");
    
    if(msg->is_compressed) {
	int rc = BZ_OK;
	char *uncompressed = NULL;
	unsigned int new_size = msg->size;

	crm_debug_5("Decompressing message data");
	crm_malloc0(uncompressed, new_size);
	rc = BZ2_bzBuffToBuffDecompress(
	    uncompressed, &new_size, data, msg->compressed_size, 1, 0);

	if(rc != BZ_OK) {
	    crm_err("Decompression failed: %d", rc);
	    crm_free(uncompressed);
	    goto badmsg;
	}
	
	CRM_ASSERT(rc == BZ_OK);
	CRM_ASSERT(new_size == msg->size);

	crm_free(data);
	data = uncompressed;

    } else if(check_message_sanity(msg, data) == FALSE) {
	goto badmsg;
    }

    if(safe_str_eq("identify", data)) {
	int pid = getpid();
	char *pid_s = crm_itoa(pid);
	send_ais_text(0, pid_s, TRUE, NULL, crm_msg_ais);
	crm_free(pid_s);

    } else if(msg->header.id == crm_class_members) {
	crm_data_t *xml = string2xml(data);

	if(xml != NULL) {
	    const char *seq_s = crm_element_value(xml, "id");
	    unsigned long seq = crm_int_helper(seq_s, NULL);
	    crm_info("Processing membership %ld/%s", seq, seq_s);
	    crm_log_xml_debug(xml, __PRETTY_FUNCTION__);
	    xml_child_iter(xml, node, crm_update_ais_node(node, seq));
	    crm_calculate_quorum();
	    
	} else {
	    crm_warn("Invalid peer update: %s", data);
	}

	free_xml(xml);
	if(dispatch != NULL) {
	    dispatch(msg, data, sender);
	}	

    } else if(dispatch != NULL) {
	dispatch(msg, data, sender);
    }
    crm_free(data);
    crm_free(msg);
    return TRUE;

  badmsg:
    crm_err("Invalid message (id=%d, dest=%s:%s, from=%s:%s.%d):"
	    " min=%d, total=%d, size=%d, bz2_size=%d",
	    msg->id, ais_dest(&(msg->host)), msg_type2text(msg->host.type),
	    ais_dest(&(msg->sender)), msg_type2text(msg->sender.type),
	    msg->sender.pid, (int)sizeof(AIS_Message),
	    msg->header.size, msg->size, msg->compressed_size);
    crm_free(data);
    crm_free(msg);
    return TRUE;
    
  bail:
    crm_err("AIS connection failed");
    return FALSE;
}

static void
ais_destroy(gpointer user_data)
{
    crm_err("AIS connection terminated");
    ais_fd_sync = -1;
    exit(1);
}

gboolean init_ais_connection(
    gboolean (*dispatch)(AIS_Message*,char*,int),
    void (*destroy)(gpointer), char **our_uuid, char **our_uname)
{
    int rc = SA_AIS_OK;
    struct utsname name;

    if(our_uname != NULL) {
	if(uname(&name) < 0) {
	    cl_perror("uname(2) call failed");
	    exit(100);
	}
	*our_uname = crm_strdup(name.nodename);
	crm_notice("Local node name: %s", *our_uname);
    }
    
    if(our_uuid != NULL) {
	*our_uuid = crm_strdup(name.nodename);
    }

    /* 16 := CRM_SERVICE */
    crm_info("Creating connection to our AIS plugin");
    rc = saServiceConnect (&ais_fd_sync, &ais_fd_async, 16);
    if (rc != SA_AIS_OK) {
	crm_info("Connection to our AIS plugin failed");
	return FALSE;
    }

    if(destroy == NULL) {
	crm_debug("Using the default destroy handler");
	destroy = ais_destroy;
    } 
   
    crm_info("AIS connection established");
    ais_source = G_main_add_fd(
	G_PRIORITY_HIGH, ais_fd_sync, FALSE, ais_dispatch, dispatch, destroy);
    ais_source_out = G_main_add_fd(
 	G_PRIORITY_HIGH, ais_fd_async, FALSE, ais_dispatch, dispatch, destroy);
    return TRUE;
}

gboolean check_message_sanity(AIS_Message *msg, char *data) 
{
    gboolean sane = TRUE;
    gboolean repaired = FALSE;
    int dest = msg->host.type;
    int tmp_size = msg->header.size - sizeof(AIS_Message);

    if(sane && msg->header.size == 0) {
	crm_err("Message with no size");
	sane = FALSE;
    }

    if(sane && ais_data_len(msg) != tmp_size) {
	int cur_size = ais_data_len(msg);

	repaired = TRUE;
	if(msg->is_compressed) {
	    msg->compressed_size = tmp_size;
	    
	} else {
	    msg->size = tmp_size;
	}
	
	crm_err("Repaired message payload size %d -> %d", cur_size, tmp_size);
    }

    if(sane && ais_data_len(msg) == 0) {
	crm_err("Message with no payload");
	sane = FALSE;
    }

    if(sane && data && msg->is_compressed == FALSE) {
	int str_size = strlen(data) + 1;
	if(ais_data_len(msg) != str_size) {
	    int lpc = 0;
	    crm_err("Message payload is corrupted: expected %d bytes, got %d",
		    ais_data_len(msg), str_size);
	    sane = FALSE;
	    for(lpc = (str_size - 10); lpc < msg->size; lpc++) {
		if(lpc < 0) {
		    lpc = 0;
		}
		crm_warn("bad_data[%d]: %d / '%c'", lpc, data[lpc], data[lpc]);
	    }
	}
    }
    
    if(sane == FALSE) {
	crm_err("Invalid message %d: (dest=%s:%s, from=%s:%s.%d, compressed=%d, size=%d, total=%d)",
		msg->id, ais_dest(&(msg->host)), msg_type2text(dest),
		ais_dest(&(msg->sender)), msg_type2text(msg->sender.type),
		msg->sender.pid, msg->is_compressed, ais_data_len(msg),
		msg->header.size);
	
    } else if(repaired) {
	crm_err("Repaired message %d: (dest=%s:%s, from=%s:%s.%d, compressed=%d, size=%d, total=%d)",
		msg->id, ais_dest(&(msg->host)), msg_type2text(dest),
		ais_dest(&(msg->sender)), msg_type2text(msg->sender.type),
		msg->sender.pid, msg->is_compressed, ais_data_len(msg),
		msg->header.size);
    } else {
	crm_debug_3("Verfied message %d: (dest=%s:%s, from=%s:%s.%d, compressed=%d, size=%d, total=%d)",
		    msg->id, ais_dest(&(msg->host)), msg_type2text(dest),
		    ais_dest(&(msg->sender)), msg_type2text(msg->sender.type),
		    msg->sender.pid, msg->is_compressed, ais_data_len(msg),
		    msg->header.size);
    }
    
    return sane;
}
#endif
