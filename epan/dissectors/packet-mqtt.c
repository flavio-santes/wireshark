/* packet-mqtt.c
 * Routines for MQTT Protocol dissection
 *
 * By Lakshmi Narayana Madala  <madalanarayana@outlook.com>
 *    Stig Bjorlykke  <stig@bjorlykke.org>
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/*
 * Protocol description:
 *
 * MQTT is a Client Server publish/subscribe messaging transport
 * protocol. The protocol runs over TCP/IP, or over other network
 * protocols that provide ordered, lossless, bi-directional
 * connections.
 *
 * MQTT v3.1 specification:
 * http://public.dhe.ibm.com/software/dw/webservices/ws-mqtt/mqtt-v3r1.html
 *
 * MQTT v3.1.1 specification:
 * http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/
 */

#include "config.h"
#include <epan/packet.h>
#include <epan/dwarf.h>
#include "packet-tcp.h"
#include "packet-ssl.h"

#define MQTT_DEFAULT_PORT     1883 /* IANA registered under service name as mqtt */
#define MQTT_SSL_DEFAULT_PORT 8883 /* IANA registered under service name secure-mqtt */

/* MQTT Protocol Versions */
#define MQTT_PROTO_V31      3
#define MQTT_PROTO_V311     4

#define MQTT_HDR_SIZE_BEFORE_LEN 1

/* MQTT Message Types */
#define MQTT_RESERVED        0
#define MQTT_CONNECT         1
#define MQTT_CONNACK         2
#define MQTT_PUBLISH         3
#define MQTT_PUBACK          4
#define MQTT_PUBREC          5
#define MQTT_PUBREL          6
#define MQTT_PUBCOMP         7
#define MQTT_SUBSCRIBE       8
#define MQTT_SUBACK          9
#define MQTT_UNSUBSCRIBE    10
#define MQTT_UNSUBACK       11
#define MQTT_PINGREQ        12
#define MQTT_PINGRESP       13
#define MQTT_DISCONNECT     14
#define MQTT_RESERVED_15    15

/* Flag Values to extract fields */
#define MQTT_MASK_MSG_TYPE          0xF0
#define MQTT_MASK_HDR_RESERVED      0x0F
#define MQTT_MASK_HDR_DUP_RESERVED  0x07
#define MQTT_MASK_QOS               0x06
#define MQTT_MASK_DUP_FLAG          0x08
#define MQTT_MASK_RETAIN            0x01

void proto_register_mqtt(void);
void proto_reg_handoff_mqtt(void);

static const value_string mqtt_protocol_version_vals[] = {
  { MQTT_PROTO_V31,        "MQTT v3.1" },
  { MQTT_PROTO_V311,       "MQTT v3.1.1" },
  { 0,                     NULL }
};

static const value_string mqtt_msgtype_vals[] = {
  { MQTT_RESERVED,          "Reserved" },
  { MQTT_CONNECT,           "Connect Command" },
  { MQTT_CONNACK,           "Connect Ack" },
  { MQTT_PUBLISH,           "Publish Message" },
  { MQTT_PUBACK,            "Publish Ack" },
  { MQTT_PUBREC,            "Publish Received" },
  { MQTT_PUBREL,            "Publish Release" },
  { MQTT_PUBCOMP,           "Publish Complete" },
  { MQTT_SUBSCRIBE,         "Subscribe Request" },
  { MQTT_SUBACK,            "Subscribe Ack" },
  { MQTT_UNSUBSCRIBE,       "Unsubscribe Request" },
  { MQTT_UNSUBACK,          "Unsubscribe Ack" },
  { MQTT_PINGREQ,           "Ping Request" },
  { MQTT_PINGRESP,          "Ping Response" },
  { MQTT_DISCONNECT,        "Disconnect Req" },
  { MQTT_RESERVED_15,       "Reserved" },
  { 0,                      NULL }
};
static value_string_ext mqtt_msgtype_vals_ext = VALUE_STRING_EXT_INIT(mqtt_msgtype_vals);

#define MQTT_QOS_ATMOST_ONCE      0
#define MQTT_QOS_ATLEAST_ONCE     1
#define MQTT_QOS_EXACTLY_ONCE     2
#define MQTT_QOS_RESERVED         3

static const value_string mqtt_qos_vals[] = {
  { MQTT_QOS_ATMOST_ONCE,       "At most once delivery (Fire and Forget)" },
  { MQTT_QOS_ATLEAST_ONCE,      "At least once delivery (Acknowledged deliver)" },
  { MQTT_QOS_EXACTLY_ONCE,      "Exactly once delivery (Assured Delivery)" },
  { MQTT_QOS_RESERVED,          "Reserved" },
  { 0,                          NULL }
};

#define MQTT_SUBACK_FAILURE  128

static const value_string mqtt_subqos_vals[] = {
  { MQTT_QOS_ATMOST_ONCE,       "At most once delivery (Fire and Forget)" },
  { MQTT_QOS_ATLEAST_ONCE,      "At least once delivery (Acknowledged deliver)" },
  { MQTT_QOS_EXACTLY_ONCE,      "Exactly once delivery (Assured Delivery)" },
  { MQTT_QOS_RESERVED,          "Reserved" },
  { MQTT_SUBACK_FAILURE,        "Failure" },
  { 0,                          NULL }
};

#define MQTT_CON_ACCEPTED                   0
#define MQTT_CON_REFUSED_VERSION_MISMATCH   1
#define MQTT_CON_REFUSED_ID_REJECTED        2
#define MQTT_CON_REFUSED_SERVER_UNAVAILABLE 3
#define MQTT_CON_REFUSED_BAD_USER_PASSWD    4
#define MQTT_CON_REFUSED_UNAUTHORIZED       5

static const value_string mqtt_conack_vals[] = {
  { MQTT_CON_ACCEPTED,                   "Connection Accepted" },
  { MQTT_CON_REFUSED_VERSION_MISMATCH,   "Connection Refused: unacceptable protocol version" },
  { MQTT_CON_REFUSED_ID_REJECTED,        "Connection Refused: identifier rejected" },
  { MQTT_CON_REFUSED_SERVER_UNAVAILABLE, "Connection Refused: server unavailable" },
  { MQTT_CON_REFUSED_BAD_USER_PASSWD,    "Connection Refused: bad user name or password" },
  { MQTT_CON_REFUSED_UNAUTHORIZED,       "Connection Refused: not authorized" },
  { 0,                                   NULL }
};

#define MQTT_CONMASK_USER        0x80
#define MQTT_CONMASK_PASSWD      0x40
#define MQTT_CONMASK_RETAIN      0x20
#define MQTT_CONMASK_QOS         0x18
#define MQTT_CONMASK_WILLFLAG    0x04
#define MQTT_CONMASK_CLEANSESS   0x02
#define MQTT_CONMASK_RESERVED    0x01

#define MQTT_CONACKMASK_RESERVED 0xFE
#define MQTT_CONACKMASK_SP       0x01

/* The protocol version is present in the CONNECT message. */
typedef struct {
    guint8 runtime_proto_version;
} mqtt_conv;

static dissector_handle_t mqtt_handle;

/* Initialize the protocol and registered fields */
static int proto_mqtt = -1;

/* Message */
static int hf_mqtt_hdrflags = -1;
static int hf_mqtt_msg_len = -1;
static int hf_mqtt_msg_type = -1;
static int hf_mqtt_reserved = -1;
static int hf_mqtt_dup_reserved = -1;
static int hf_mqtt_dup_flag = -1;
static int hf_mqtt_qos_level = -1;
static int hf_mqtt_retain = -1;
static int hf_mqtt_conack_flags = -1;
static int hf_mqtt_conackflag_reserved = -1;
static int hf_mqtt_conackflag_sp = -1;
static int hf_mqtt_conack_code = -1;
static int hf_mqtt_msgid = -1;
static int hf_mqtt_sub_qos = -1;
static int hf_mqtt_suback_qos = -1;
static int hf_mqtt_topic_len = -1;
static int hf_mqtt_topic = -1;
static int hf_mqtt_will_topic_len = -1;
static int hf_mqtt_will_topic = -1;
static int hf_mqtt_will_msg_len = -1;
static int hf_mqtt_will_msg = -1;
static int hf_mqtt_username_len = -1;
static int hf_mqtt_username = -1;
static int hf_mqtt_passwd_len = -1;
static int hf_mqtt_passwd = -1;
static int hf_mqtt_pubmsg = -1;
static int hf_mqtt_proto_len = -1;
static int hf_mqtt_proto_name = -1;
static int hf_mqtt_client_id_len = -1;
static int hf_mqtt_client_id = -1;
static int hf_mqtt_proto_ver = -1;
static int hf_mqtt_conflags = -1;
static int hf_mqtt_conflag_user = -1;
static int hf_mqtt_conflag_passwd = -1;
static int hf_mqtt_conflag_will_retain = -1;
static int hf_mqtt_conflag_will_qos = -1;
static int hf_mqtt_conflag_will_flag = -1;
static int hf_mqtt_conflag_clean_sess = -1;
static int hf_mqtt_conflag_reserved = -1;
static int hf_mqtt_keep_alive = -1;

/* Initialize the subtree pointers */
static gint ett_mqtt_hdr = -1;
static gint ett_mqtt_msg = -1;
static gint ett_mqtt_hdr_flags = -1;
static gint ett_mqtt_con_flags = -1;
static gint ett_mqtt_conack_flags = -1;

/* Reassemble SMPP TCP segments */
static gboolean reassemble_mqtt_over_tcp = TRUE;

#define GET_MQTT_PDU_LEN(msg_len, len_offset)    (msg_len + len_offset + MQTT_HDR_SIZE_BEFORE_LEN)

static guint get_mqtt_pdu_len(packet_info *pinfo _U_, tvbuff_t *tvb,
                              int offset, void *data _U_)
{
  guint64 msg_len;
  guint len_offset;

  len_offset = dissect_uleb128(tvb, (offset + MQTT_HDR_SIZE_BEFORE_LEN), &msg_len);

  /* Explicitly downcast the value, because the length can never be more than 4 bytes */
  return (guint)(GET_MQTT_PDU_LEN(msg_len, len_offset));
}

/* Dissect the MQTT message */
static int dissect_mqtt(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void* data _U_)
{
    guint8  mqtt_fixed_hdr;
    guint8  mqtt_msg_type;
    proto_item *ti;
    proto_item *ti_mqtt;

    proto_tree *mqtt_tree;
    proto_tree *mqtt_flag_tree;

    guint8      hdr_reserved;
    guint8      mqtt_con_flags;
    guint64     msg_len      = 0;
    gint        mqtt_msg_len = 0;
    guint16     mqtt_str_len;
    guint16     mqtt_len_offset;
    conversation_t *conv;
    mqtt_conv   *mqtt;

    int offset = 0;

    /* Extract the message ID */
    mqtt_fixed_hdr = tvb_get_guint8(tvb, offset);
    mqtt_msg_type = mqtt_fixed_hdr >> 4;

    col_set_str(pinfo->cinfo, COL_PROTOCOL, "MQTT");
    col_append_sep_str(pinfo->cinfo, COL_INFO, ", ", val_to_str_ext(mqtt_msg_type, &mqtt_msgtype_vals_ext, "Unknown (0x%02x)"));

    /* Add the MQTT branch to the main tree */
    ti = proto_tree_add_item(tree, proto_mqtt, tvb, 0, -1, ENC_NA);
    mqtt_tree = proto_item_add_subtree(ti, ett_mqtt_hdr);

    conv = find_or_create_conversation(pinfo);
    mqtt = (mqtt_conv *)conversation_get_proto_data(conv, proto_mqtt);
    if (mqtt == NULL)
    {
        mqtt = wmem_new0(wmem_file_scope(), mqtt_conv);
        conversation_add_proto_data(conv, proto_mqtt, mqtt);
    }

    mqtt_len_offset = dissect_uleb128(tvb, (offset + MQTT_HDR_SIZE_BEFORE_LEN), &msg_len);

    /* Explicit downcast, typically maximum length of message could be 4 bytes */
    mqtt_msg_len = (gint) msg_len;

    /* Add the type to the MQTT tree item */
    proto_item_append_text(mqtt_tree, ", %s", val_to_str_ext(mqtt_msg_type, &mqtt_msgtype_vals_ext, "Unknown (0x%02x)"));

    ti_mqtt = proto_tree_add_uint_format_value(mqtt_tree, hf_mqtt_hdrflags, tvb, offset, 1, mqtt_fixed_hdr, "0x%02x (%s)",
                                               mqtt_fixed_hdr, val_to_str_ext(mqtt_msg_type, &mqtt_msgtype_vals_ext, "Unknown (0x%02x)"));

    mqtt_flag_tree = proto_item_add_subtree(ti_mqtt, ett_mqtt_hdr_flags);
    proto_tree_add_item(mqtt_flag_tree, hf_mqtt_msg_type,  tvb, offset, 1, ENC_BIG_ENDIAN);

    if (mqtt_msg_type == MQTT_PUBLISH) {
      proto_tree_add_item(mqtt_flag_tree, hf_mqtt_dup_flag,  tvb, offset, 1, ENC_BIG_ENDIAN);
      proto_tree_add_item(mqtt_flag_tree, hf_mqtt_qos_level, tvb, offset, 1, ENC_BIG_ENDIAN);
      proto_tree_add_item(mqtt_flag_tree, hf_mqtt_retain,    tvb, offset, 1, ENC_BIG_ENDIAN);
    } else if (mqtt->runtime_proto_version == MQTT_PROTO_V31 &&
               (mqtt_msg_type == MQTT_PUBREL || mqtt_msg_type == MQTT_SUBSCRIBE ||
                mqtt_msg_type == MQTT_UNSUBSCRIBE)) {
        hdr_reserved = mqtt_fixed_hdr & MQTT_MASK_HDR_DUP_RESERVED;
        proto_tree_add_item(mqtt_flag_tree, hf_mqtt_dup_flag, tvb, offset, 1, ENC_BIG_ENDIAN);
        proto_tree_add_uint(mqtt_flag_tree, hf_mqtt_dup_reserved, tvb, offset, 1, hdr_reserved);
    } else {
      hdr_reserved = mqtt_fixed_hdr & MQTT_MASK_HDR_RESERVED;
      proto_tree_add_uint(mqtt_flag_tree, hf_mqtt_reserved, tvb, offset, 1, hdr_reserved);
    }

    offset += 1;

    /* Add the MQTT message length */
    proto_tree_add_uint64(mqtt_tree, hf_mqtt_msg_len, tvb, offset, mqtt_len_offset, msg_len);
    offset += mqtt_len_offset;

    switch(mqtt_msg_type)
    {
      case MQTT_CONNECT:
        mqtt_str_len = tvb_get_ntohs(tvb, offset);
        proto_tree_add_item(mqtt_tree, hf_mqtt_proto_len, tvb, offset, 2, ENC_BIG_ENDIAN);
        offset += 2;

        proto_tree_add_item(mqtt_tree, hf_mqtt_proto_name, tvb, offset, mqtt_str_len, ENC_UTF_8|ENC_NA);
        offset += mqtt_str_len;

        mqtt->runtime_proto_version = tvb_get_guint8(tvb, offset);

        proto_tree_add_item(mqtt_tree, hf_mqtt_proto_ver, tvb, offset, 1, ENC_BIG_ENDIAN);
        offset += 1;

        /* Create a new subtree for flags, and add all items under this tree */
        mqtt_con_flags = tvb_get_guint8(tvb, offset);
        ti_mqtt = proto_tree_add_item(mqtt_tree, hf_mqtt_conflags, tvb, offset, 1, ENC_BIG_ENDIAN);
        mqtt_flag_tree = proto_item_add_subtree(ti_mqtt, ett_mqtt_con_flags);
        proto_tree_add_item(mqtt_flag_tree, hf_mqtt_conflag_user,        tvb, offset, 1, ENC_BIG_ENDIAN);
        proto_tree_add_item(mqtt_flag_tree, hf_mqtt_conflag_passwd,      tvb, offset, 1, ENC_BIG_ENDIAN);
        proto_tree_add_item(mqtt_flag_tree, hf_mqtt_conflag_will_retain, tvb, offset, 1, ENC_BIG_ENDIAN);
        proto_tree_add_item(mqtt_flag_tree, hf_mqtt_conflag_will_qos,    tvb, offset, 1, ENC_BIG_ENDIAN);
        proto_tree_add_item(mqtt_flag_tree, hf_mqtt_conflag_will_flag,   tvb, offset, 1, ENC_BIG_ENDIAN);
        proto_tree_add_item(mqtt_flag_tree, hf_mqtt_conflag_clean_sess,  tvb, offset, 1, ENC_BIG_ENDIAN);
        proto_tree_add_item(mqtt_flag_tree, hf_mqtt_conflag_reserved,    tvb, offset, 1, ENC_BIG_ENDIAN);
        offset += 1;

        proto_tree_add_item(mqtt_tree, hf_mqtt_keep_alive, tvb, offset, 2, ENC_BIG_ENDIAN);
        offset += 2;

        mqtt_str_len = tvb_get_ntohs(tvb, offset);
        proto_tree_add_item(mqtt_tree, hf_mqtt_client_id_len, tvb, offset, 2, ENC_BIG_ENDIAN);
        offset += 2;

        proto_tree_add_item(mqtt_tree, hf_mqtt_client_id, tvb, offset, mqtt_str_len, ENC_UTF_8|ENC_NA);
        offset += mqtt_str_len;

        if(mqtt_con_flags & MQTT_CONMASK_WILLFLAG)
        {
          mqtt_str_len = tvb_get_ntohs(tvb, offset);
          proto_tree_add_item(mqtt_tree, hf_mqtt_will_topic_len, tvb, offset, 2, ENC_BIG_ENDIAN);
          offset += 2;

          proto_tree_add_item(mqtt_tree, hf_mqtt_will_topic, tvb, offset, mqtt_str_len, ENC_UTF_8|ENC_NA);
          offset += mqtt_str_len;
        }
        if(mqtt_con_flags & MQTT_CONMASK_WILLFLAG)
        {
          mqtt_str_len = tvb_get_ntohs(tvb, offset);
          proto_tree_add_item(mqtt_tree, hf_mqtt_will_msg_len, tvb, offset, 2, ENC_BIG_ENDIAN);
          offset += 2;

          proto_tree_add_item(mqtt_tree, hf_mqtt_will_msg, tvb, offset, mqtt_str_len, ENC_UTF_8|ENC_NA);
          offset += mqtt_str_len;
        }
        if((mqtt_con_flags & MQTT_CONMASK_USER) && (tvb_reported_length_remaining(tvb, offset) > 0))
        {
          mqtt_str_len = tvb_get_ntohs(tvb, offset);
          proto_tree_add_item(mqtt_tree, hf_mqtt_username_len, tvb, offset, 2, ENC_BIG_ENDIAN);
          offset += 2;

          proto_tree_add_item(mqtt_tree, hf_mqtt_username, tvb, offset, mqtt_str_len, ENC_UTF_8|ENC_NA);
          offset += mqtt_str_len;
        }
        if((mqtt_con_flags & MQTT_CONMASK_PASSWD) && (tvb_reported_length_remaining(tvb, offset) > 0))
        {
          mqtt_str_len = tvb_get_ntohs(tvb, offset);
          proto_tree_add_item(mqtt_tree, hf_mqtt_passwd_len, tvb, offset, 2, ENC_BIG_ENDIAN);
          offset += 2;

          proto_tree_add_item(mqtt_tree, hf_mqtt_passwd, tvb, offset, mqtt_str_len, ENC_UTF_8|ENC_NA);
        }
        break;

      case MQTT_CONNACK:
        /* v3.1 Connection Ack only contains a reserved byte and the Return Code.
         * v3.1.1 Conn Ack contains the Conn Ack Flags and the Return Code.
         */
        ti_mqtt = proto_tree_add_item(mqtt_tree, hf_mqtt_conack_flags, tvb, offset, 1, ENC_BIG_ENDIAN);
        mqtt_flag_tree = proto_item_add_subtree(ti_mqtt, ett_mqtt_conack_flags);
        proto_tree_add_item(mqtt_flag_tree, hf_mqtt_conackflag_reserved, tvb, offset, 1, ENC_BIG_ENDIAN);
        proto_tree_add_item(mqtt_flag_tree, hf_mqtt_conackflag_sp, tvb, offset, 1, ENC_BIG_ENDIAN);
        offset += 1;

        proto_tree_add_item(mqtt_tree, hf_mqtt_conack_code, tvb, offset, 1, ENC_BIG_ENDIAN);
        break;

      case MQTT_PUBLISH:
        /* TopicName|MsgID|Message| */
        mqtt_str_len = tvb_get_ntohs(tvb, offset);
        proto_tree_add_item(mqtt_tree, hf_mqtt_topic_len, tvb, offset, 2, ENC_BIG_ENDIAN);
        offset += 2;
        mqtt_msg_len -= 2;

        proto_tree_add_item(mqtt_tree, hf_mqtt_topic, tvb, offset, mqtt_str_len, ENC_UTF_8|ENC_NA);
        offset += mqtt_str_len;
        mqtt_msg_len -= mqtt_str_len;

        /* Message ID is included only when QoS > 0 */
        if(mqtt_fixed_hdr & MQTT_MASK_QOS)
        {
          proto_tree_add_item(mqtt_tree, hf_mqtt_msgid, tvb, offset, 2, ENC_BIG_ENDIAN);
          offset += 2;
          mqtt_msg_len -= 2;
        }
        proto_tree_add_item(mqtt_tree, hf_mqtt_pubmsg, tvb, offset, mqtt_msg_len, ENC_UTF_8|ENC_NA);
        break;

      case MQTT_SUBSCRIBE:
        /* After the Message Id field is found, the following fields must appear
         * at least once:
         * |TopicName|QoS|
         */
        proto_tree_add_item(mqtt_tree, hf_mqtt_msgid, tvb, offset, 2, ENC_BIG_ENDIAN);
        offset += 2;
        for(mqtt_msg_len -= 2; mqtt_msg_len > 0;)
        {
          mqtt_str_len = tvb_get_ntohs(tvb, offset);
          proto_tree_add_item(mqtt_tree, hf_mqtt_topic_len, tvb, offset, 2, ENC_BIG_ENDIAN);
          offset += 2;
          mqtt_msg_len -= 2;

          proto_tree_add_item(mqtt_tree, hf_mqtt_topic, tvb, offset, mqtt_str_len, ENC_UTF_8|ENC_NA);
          offset += mqtt_str_len;
          mqtt_msg_len -= mqtt_str_len;

          proto_tree_add_item(mqtt_tree, hf_mqtt_sub_qos, tvb, offset, 1, ENC_BIG_ENDIAN);
          offset += 1;
          mqtt_msg_len -= 1;
        }
        break;

      case MQTT_UNSUBSCRIBE:
        /* After the Message Id field is found, the following fields must appear
         * at least once:
         * |TopicName|
         */
        proto_tree_add_item(mqtt_tree, hf_mqtt_msgid, tvb, offset, 2, ENC_BIG_ENDIAN);
        offset += 2;
        for(mqtt_msg_len -= 2; mqtt_msg_len > 0;)
        {
          mqtt_str_len = tvb_get_ntohs(tvb, offset);
          proto_tree_add_item(mqtt_tree, hf_mqtt_topic_len, tvb, offset, 2, ENC_BIG_ENDIAN);
          offset += 2;
          mqtt_msg_len -= 2;

          proto_tree_add_item(mqtt_tree, hf_mqtt_topic, tvb, offset, mqtt_str_len, ENC_UTF_8|ENC_NA);
          offset += mqtt_str_len;
          mqtt_msg_len -= mqtt_str_len;
        }
        break;

      case MQTT_SUBACK:
        /* The SUBACK message contains a list of granted QoS levels that come
         * after the Message Id field. The size of each QoS entry is 1 byte.
         */
        proto_tree_add_item(mqtt_tree, hf_mqtt_msgid, tvb, offset, 2, ENC_BIG_ENDIAN);
        offset += 2;
        for(mqtt_msg_len -= 2; mqtt_msg_len > 0; mqtt_msg_len--)
        {
          proto_tree_add_item(mqtt_tree, hf_mqtt_suback_qos, tvb, offset, 1, ENC_BIG_ENDIAN);
          offset += 1;
        }
        break;

      /* The following response code contains only msg-id */
      case MQTT_PUBACK:
      case MQTT_PUBREC:
      case MQTT_PUBREL:
      case MQTT_PUBCOMP:
      case MQTT_UNSUBACK:
        proto_tree_add_item(mqtt_tree, hf_mqtt_msgid, tvb, offset, 2, ENC_BIG_ENDIAN);
        break;

      /* The following messages don't have variable header */
      case MQTT_PINGREQ:
      case MQTT_PINGRESP:
      case MQTT_DISCONNECT:
        break;
    }

    return tvb_captured_length(tvb);
}

/**
"The minimum size of MQTT Packet is 2 bytes(Ping Req, Ping Rsp,
Disconnect), and the maximum size is 256MB.  Hence minimum fixed
length should be 2 bytes for tcp_dissect_pdu.

If the length filed is spread across two TCP segments, then we have a
problem, because exception will be raised.  So long as MQTT length
field(although spread over 4 bytes) is present within single TCP
segment we shouldn't have any issue by calling tcp_dissect_pdu with
minimum length set to 2."

XXX: ToDo: Commit a fix for the case of the length field spread across TCP segments.
**/

static int dissect_mqtt_data(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void* data)
{
  col_clear(pinfo->cinfo, COL_INFO);

  tcp_dissect_pdus(tvb, pinfo, tree,
                   reassemble_mqtt_over_tcp,
                   2,                           /* Length can be determined within 5 bytes */
                   get_mqtt_pdu_len,
                   dissect_mqtt, data);

  return tvb_captured_length(tvb);
}

/*
 * Register the protocol with Wireshark
 */
void proto_register_mqtt(void)
{
  static hf_register_info hf_mqtt[] = {
    { &hf_mqtt_msg_len,
      { "Msg Len", "mqtt.len",
        FT_UINT64, BASE_DEC, NULL, 0,
        NULL, HFILL }},
    { &hf_mqtt_hdrflags,
      { "Header Flags", "mqtt.hdrflags",
        FT_UINT8, BASE_HEX, NULL, 0,
        NULL, HFILL }},
    { &hf_mqtt_msg_type,
      { "Message Type", "mqtt.msgtype",
        FT_UINT8, BASE_DEC | BASE_EXT_STRING, &mqtt_msgtype_vals_ext, MQTT_MASK_MSG_TYPE,
        NULL, HFILL }},
    { &hf_mqtt_reserved,
      { "Reserved", "mqtt.hdr_reserved",
        FT_UINT8, BASE_DEC, NULL, MQTT_MASK_HDR_RESERVED,
        "Fixed Header Reserved Field", HFILL }},
    { &hf_mqtt_dup_reserved,
      { "Reserved", "mqtt.hdr_dup_reserved",
        FT_UINT8, BASE_DEC, NULL, MQTT_MASK_HDR_DUP_RESERVED,
        "Fixed Header Reserved Field", HFILL }},
    { &hf_mqtt_dup_flag,
      { "DUP Flag", "mqtt.dupflag",
        FT_BOOLEAN, 8, TFS(&tfs_set_notset), MQTT_MASK_DUP_FLAG,
        NULL, HFILL }},
    { &hf_mqtt_qos_level,
      { "QoS Level", "mqtt.qos",
        FT_UINT8, BASE_DEC, VALS(mqtt_qos_vals), MQTT_MASK_QOS,
        NULL, HFILL }},
    { &hf_mqtt_retain,
      { "Retain", "mqtt.retain",
        FT_BOOLEAN, 8, TFS(&tfs_set_notset), MQTT_MASK_RETAIN,
        NULL, HFILL }},
    /* Conn-Ack */
    { &hf_mqtt_conack_flags,
      { "Acknowledge Flags", "mqtt.conack.flags",
        FT_UINT8, BASE_HEX, NULL, 0,
        NULL, HFILL }},
    { &hf_mqtt_conackflag_reserved,
      { "Reserved", "mqtt.conack.flags.reserved",
        FT_BOOLEAN, 8, TFS(&tfs_set_notset), MQTT_CONACKMASK_RESERVED,
        NULL, HFILL }},
    { &hf_mqtt_conackflag_sp,
      { "Session Present", "mqtt.conack.flags.sp",
        FT_BOOLEAN, 8, TFS(&tfs_set_notset), MQTT_CONACKMASK_SP,
        "Session Present (version 3.1.1)", HFILL }},
    { &hf_mqtt_conack_code,
      { "Return Code", "mqtt.conack.val",
        FT_UINT8, BASE_DEC, VALS(mqtt_conack_vals), 0,
        NULL, HFILL }},
    /* Publish-Ack / Publish-Rec / Publish-Rel / Publish-Comp / Unsubscribe-Ack */
    { &hf_mqtt_msgid,
      { "Message Identifier", "mqtt.msgid",
        FT_UINT16, BASE_DEC, NULL, 0,
        NULL, HFILL }},
    { &hf_mqtt_sub_qos,
      { "Requested QoS", "mqtt.sub.qos",
        FT_UINT8, BASE_DEC, VALS(mqtt_qos_vals), 0,
        NULL, HFILL }},
    { &hf_mqtt_suback_qos,
      { "Granted QoS", "mqtt.suback.qos",
        FT_UINT8, BASE_DEC, VALS(mqtt_subqos_vals), 0,
        NULL, HFILL }},
      { &hf_mqtt_topic_len,
      { "Topic Length", "mqtt.topic_len",
        FT_UINT16, BASE_DEC, NULL, 0,
        NULL, HFILL }},
    { &hf_mqtt_topic,
      { "Topic", "mqtt.topic",
        FT_STRING, BASE_NONE, NULL, 0,
        NULL, HFILL }},
    { &hf_mqtt_will_topic_len,
      { "Will Topic Length", "mqtt.willtopic_len",
        FT_UINT16, BASE_DEC, NULL, 0,
        NULL, HFILL }},
    { &hf_mqtt_will_topic,
      { "Will Topic", "mqtt.willtopic",
        FT_STRING, BASE_NONE, NULL, 0,
        NULL, HFILL }},
    { &hf_mqtt_will_msg,
      { "Will Message", "mqtt.willmsg",
        FT_STRING, BASE_NONE, NULL, 0,
        NULL, HFILL }},
    { &hf_mqtt_will_msg_len,
      { "Will Message Length", "mqtt.willmsg_len",
        FT_UINT16, BASE_DEC, NULL, 0,
        NULL, HFILL }},
    { &hf_mqtt_username_len,
      { "User Name Length", "mqtt.username_len",
        FT_UINT16, BASE_DEC, NULL, 0,
        NULL, HFILL }},
    { &hf_mqtt_username,
      { "User Name", "mqtt.username",
        FT_STRING, BASE_NONE, NULL, 0,
        NULL, HFILL }},
    { &hf_mqtt_passwd_len,
      { "Password Length", "mqtt.passwd_len",
        FT_UINT16, BASE_DEC, NULL, 0,
        NULL, HFILL }},
    { &hf_mqtt_passwd,
      { "Password", "mqtt.passwd",
        FT_STRING, BASE_NONE, NULL, 0,
        NULL, HFILL }},
    { &hf_mqtt_pubmsg,
      { "Message", "mqtt.msg",
        FT_STRING, BASE_NONE, NULL, 0,
        NULL, HFILL }},
    { &hf_mqtt_proto_len,
      { "Protocol Name Length", "mqtt.proto_len",
        FT_UINT16, BASE_DEC, NULL, 0,
        NULL, HFILL }},
    { &hf_mqtt_proto_name,
      { "Protocol Name", "mqtt.protoname",
        FT_STRING, BASE_NONE, NULL, 0,
        NULL, HFILL }},
    { &hf_mqtt_client_id_len,
      { "Client ID Length", "mqtt.clientid_len",
        FT_UINT16, BASE_DEC, NULL, 0,
        NULL, HFILL }},
    { &hf_mqtt_client_id,
      { "Client ID", "mqtt.clientid",
        FT_STRING, BASE_NONE, NULL, 0,
        NULL, HFILL }},
    { &hf_mqtt_proto_ver,
      { "Version", "mqtt.ver",
        FT_UINT8, BASE_DEC, VALS(mqtt_protocol_version_vals), 0,
        "MQTT version", HFILL }},
    /* Connect Flags */
    { &hf_mqtt_conflags,
      { "Connect Flags", "mqtt.conflags",
        FT_UINT8, BASE_HEX, NULL, 0,
        NULL, HFILL }},
    { &hf_mqtt_conflag_user,
      { "User Name Flag", "mqtt.conflag.uname",
        FT_BOOLEAN, 8, TFS(&tfs_set_notset), MQTT_CONMASK_USER,
        NULL, HFILL }},
    { &hf_mqtt_conflag_passwd,
      { "Password Flag", "mqtt.conflag.passwd",
        FT_BOOLEAN, 8, TFS(&tfs_set_notset), MQTT_CONMASK_PASSWD,
        NULL, HFILL }},
    { &hf_mqtt_conflag_will_retain,
      { "Will Retain", "mqtt.conflag.retain",
        FT_BOOLEAN, 8, TFS(&tfs_set_notset), MQTT_CONMASK_RETAIN,
        NULL, HFILL }},
    { &hf_mqtt_conflag_will_qos,
      { "QoS Level", "mqtt.conflag.qos",
        FT_UINT8, BASE_DEC, VALS(mqtt_qos_vals), MQTT_CONMASK_QOS,
        NULL, HFILL }},
    { &hf_mqtt_conflag_will_flag,
      { "Will Flag", "mqtt.conflag.willflag",
        FT_BOOLEAN, 8, TFS(&tfs_set_notset), MQTT_CONMASK_WILLFLAG,
        NULL, HFILL }},
    { &hf_mqtt_conflag_clean_sess,
      { "Clean Session Flag", "mqtt.conflag.cleansess",
        FT_BOOLEAN, 8, TFS(&tfs_set_notset), MQTT_CONMASK_CLEANSESS,
        NULL, HFILL }},
    { &hf_mqtt_conflag_reserved,
      { "(Reserved)", "mqtt.conflag.reserved",
        FT_BOOLEAN, 8, TFS(&tfs_set_notset), MQTT_CONMASK_RESERVED,
        NULL, HFILL }},
    { &hf_mqtt_keep_alive,
      { "Keep Alive", "mqtt.kalive",
        FT_UINT16, BASE_DEC, NULL, 0,
        NULL, HFILL }}
  };

  /* Setup protocol subtree arrays */
  static gint* ett_mqtt[] = {
    &ett_mqtt_hdr,
    &ett_mqtt_msg,
    &ett_mqtt_hdr_flags,
    &ett_mqtt_con_flags,
    &ett_mqtt_conack_flags
  };

  /* Register protocol names and descriptions */
  proto_mqtt = proto_register_protocol("MQ Telemetry Transport Protocol", "MQTT", "mqtt");

  /* Register the dissector */
  mqtt_handle = register_dissector("mqtt", dissect_mqtt_data, proto_mqtt);

  proto_register_field_array(proto_mqtt, hf_mqtt, array_length(hf_mqtt));
  proto_register_subtree_array(ett_mqtt, array_length(ett_mqtt));
}

/*
 *  Dissector Handoff
 */
void proto_reg_handoff_mqtt(void)
{
  dissector_add_uint_with_preference("tcp.port", MQTT_DEFAULT_PORT, mqtt_handle);
  ssl_dissector_add(MQTT_SSL_DEFAULT_PORT, mqtt_handle);
}

/*
 * Editor modelines  -  http://www.wireshark.org/tools/modelines.html
 *
 * Local variables:
 * c-basic-offset: 2
 * tab-width: 8
 * indent-tabs-mode: nil
 * End:
 *
 * vi: set shiftwidth=2 tabstop=8 expandtab:
 * :indentSize=2:tabSize=8:noTabs=true:
 */
