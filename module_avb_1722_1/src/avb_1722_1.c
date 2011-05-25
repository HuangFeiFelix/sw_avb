#include "xccompat.h"
#include "avb_conf.h"
#include "avb_1722_common.h"
#include "avb_1722_1.h"
#include "avb_1722_1_protocol.h"
#include "ethernet_tx_client.h"
#include "nettypes.h"
#include "misc_timer.h"
#include "simple_printf.h"
#include <print.h>

static unsigned char my_mac_addr[6];

static unsigned char avb_1722_1_sdp_dest_addr[6] =  {0x01, 0x50, 0x43, 0xff, 0x00, 0x00};
//static unsigned char avb_1722_1_scm_dest_addr[6] =  {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
//static unsigned char avb_1722_1_scm_dest_addr[6] =  {0x91, 0xe0, 0xf0, 0x00, 0xff, 0x01};
//static unsigned char avb_1722_1_sec_dest_addr[6] =  {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

//! Buffer for constructing 1722.1 transmit packets
static unsigned int avb_1722_1_buf[(sizeof(avb_1722_1_packet_t)+sizeof(ethernet_hdr_t)+3)/4];

//! Timers for various parts of the state machines
static avb_timer sdp_advertise_timer;
static avb_timer sdp_readvertise_timer;
static avb_timer sdp_discovery_timer;

//! Counts two second intervals
static unsigned sdp_two_second_counter = 0;

//! The GUID of this device
static unsigned my_guid[2];

//! The GUID whose information we are currently trying to discover
static unsigned discover_guid[2];

//! Enumerations for state variables
enum { SDP_ADVERTISE_IDLE,
	   SDP_ADVERTISE_ADVERTISE_1,
	   SDP_ADVERTISE_ADVERTISE_2,
	   SDP_ADVERTISE_WAITING,
	   SDP_ADVERTISE_DEPARTING_1,
	   SDP_ADVERTISE_DEPARTING_2
} sdp_advertise_state = SDP_ADVERTISE_IDLE;

enum { SDP_DISCOVERY_IDLE,
	   SDP_DISCOVERY_WAITING,
	   SDP_DISCOVERY_DISCOVER
} sdp_discovery_state = SDP_DISCOVERY_IDLE;

enum { SCM_TALKER_IDLE,
	   SCM_TALKER_WAITING
} scm_talker_state = SCM_TALKER_IDLE;

enum { SCM_LISTENER_IDLE,
	   SCM_LISTENER_WAITING
} scm_listener_state = SCM_LISTENER_IDLE;

//! Record for entries in the SRP entity database
typedef struct {
	unsigned guid[2];
	unsigned timeout;
} avb_1722_1_entity_record;

//! Data structures from the 1722.1 SCM section

typedef struct {
	unsigned guid[2];
	short unique_id;
	short padding;
} avb_1722_1_scm_listener_pair;

typedef struct {
	unsigned stream_id[2];
	short connection_count;
	char destination_mac[6];
	avb_1722_1_scm_listener_pair listeners[AVB_1722_1_MAX_LISTENERS_PER_TALKER];
} avb_1722_1_scm_talker_stream_info;

typedef struct {
	unsigned talker_guid[2];
	short talker_unique_id;
	short connected;
	unsigned stream_id[2];
	char destination_mac[6];
	short padding;
} avb_1722_1_scm_listener_stream_info;

typedef struct {
	unsigned timeout;
	short retried;
	short command;
	short original_sequence_id;
	short dummy;
} avb_1722_1_scm_inflight_command;


//! Entity database
avb_1722_1_entity_record entities[AVB_1722_1_MAX_ENTITIES];

//! Listener stream database
avb_1722_1_scm_listener_stream_info listener_streams[AVB_1722_1_MAX_LISTENERS];

//! Talker stream database
avb_1722_1_scm_talker_stream_info talker_streams[AVB_1722_1_MAX_TALKERS];


void avb_1722_1_init(unsigned char macaddr[6], unsigned serial_number)
{
  for (int i=0;i<6;i++)
    my_mac_addr[i] = macaddr[i];

  my_guid[0] = (macaddr[3] << 24) + (macaddr[2] << 16) + (macaddr[1] << 8) + macaddr[0];
  my_guid[1] = (serial_number << 16) + (macaddr[5] << 8) +(macaddr[4]);

  init_avb_timer(&sdp_advertise_timer, 1);
  init_avb_timer(&sdp_readvertise_timer, 100);
  init_avb_timer(&sdp_discovery_timer, 200);

  sdp_discovery_state = SDP_DISCOVERY_WAITING;
  start_avb_timer(&sdp_discovery_timer, 1);
}



static void avb_1722_1_entity_database_add(avb_1722_1_sdp_packet_t* pkt)
{
	unsigned guid[2];
	unsigned int found_slot_index = AVB_1722_1_MAX_ENTITIES;

	guid[0] = GET_WORD(pkt->entity_guid_lo);
	guid[1] = GET_WORD(pkt->entity_guid_hi);

	for (unsigned i=0; i<AVB_1722_1_MAX_ENTITIES; ++i) {
		if (entities[i].guid[0]==0 || entities[i].guid[1]==0) found_slot_index=i;
		if (entities[i].guid[0]==guid[0] && entities[i].guid[1]==guid[1]) {
			found_slot_index=i;
			break;
		}
	}

	if (found_slot_index != AVB_1722_1_MAX_ENTITIES) {
		entities[found_slot_index].guid[0] = guid[0];
		entities[found_slot_index].guid[1] = guid[1];
		entities[found_slot_index].timeout = GET_1722_1_VALID_TIME(&pkt->header) + sdp_two_second_counter;
		return;
	}
}

static void avb_1722_1_entity_database_remove(avb_1722_1_sdp_packet_t* pkt)
{
	unsigned guid[2];
	guid[0] = GET_WORD(pkt->entity_guid_lo);
	guid[1] = GET_WORD(pkt->entity_guid_hi);

	for (unsigned i=0; i<AVB_1722_1_MAX_ENTITIES; ++i) {
		if (entities[i].guid[0]==guid[0] && entities[i].guid[1]==guid[1]) {
			entities[i].guid[0]=0;
			entities[i].guid[1]=0;
		}
	}
}

static void avb_1722_1_entity_database_check_timeout()
{
	for (unsigned i=0; i<AVB_1722_1_MAX_ENTITIES; ++i) {
		if (entities[i].guid[0]==0 && entities[i].guid[1]==0) continue;

		if (entities[i].timeout < sdp_two_second_counter) {
			entities[i].guid[0]=0;
			entities[i].guid[1]=0;
		}
	}
}

static unsigned scm_listener_valid_listener_unique(avb_1722_1_scm_packet_t* pkt)
{
	short n = pkt->listener_unique_id;
	return n < AVB_1722_1_MAX_LISTENERS;
}

static unsigned scm_listener_listener_is_connected()
{
	return 0;
}

static avb_1722_1_scm_status_type scm_listener_connect_listener()
{
	return SCM_STATUS_SUCCESS;
}

static avb_1722_1_scm_status_type scm_listener_disconnect_listener()
{
	return SCM_STATUS_SUCCESS;
}

static avb_1722_1_scm_status_type scm_listener_get_state()
{
	return SCM_STATUS_SUCCESS;
}

static void scm_listener_tx_command()
{
}

static void scm_listener_tx_response()
{
}

static void scm_listener_cancel_timeout()
{
}

static void scm_listener_remove_inflight()
{
}



static unsigned scm_talker_valid_talker_unique()
{
	return 0;
}

static avb_1722_1_scm_status_type scm_talker_connect_talker()
{
	return SCM_STATUS_SUCCESS;
}

static avb_1722_1_scm_status_type scm_talker_disconnect_talker()
{
	return SCM_STATUS_SUCCESS;
}

static avb_1722_1_scm_status_type scm_talker_get_state()
{
	return SCM_STATUS_SUCCESS;
}

static avb_1722_1_scm_status_type scm_talker_get_connection()
{
	return SCM_STATUS_SUCCESS;
}

static void scm_talker_tx_response()
{
}



static unsigned compare_guid(short* a, short* b)
{
	return a[0]==b[0] && a[1]==b[1] && a[2]==b[2] && a[3]==b[3];
}

static void avb_1722_1_create_1722_1_header(unsigned char* dest_addr, int subtype, int message_type, char valid_time_status, unsigned data_len, ethernet_hdr_t *hdr)
{
	avb_1722_1_packet_header_t *pkt = (avb_1722_1_packet_header_t *) (hdr + 1);

	for (int i=0;i<6;i++) {
	  hdr->src_addr[i] = my_mac_addr[i];
	  hdr->dest_addr[i] = dest_addr[i];
	}

	hdr->ethertype[0] = AVB_ETYPE >> 8;
	hdr->ethertype[1] = AVB_ETYPE & 0xff;

	SET_1722_1_CD_FLAG(pkt, 1);
	SET_1722_1_SUBTYPE(pkt, subtype);
	SET_1722_1_SV(pkt, 0);
	SET_1722_1_AVB_VERSION(pkt, 0);
	SET_1722_1_MSG_TYPE(pkt, message_type);
	SET_1722_1_VALID_TIME(pkt, valid_time_status);
	SET_1722_1_DATALENGTH(pkt, data_len);
}


static void avb_1722_1_create_sdp_packet(int message_type, unsigned guid[2])
{
	  ethernet_hdr_t *hdr = (ethernet_hdr_t*) &avb_1722_1_buf[0];
	  avb_1722_1_sdp_packet_t *pkt = (avb_1722_1_sdp_packet_t*) (hdr + 1);

	  avb_1722_1_create_1722_1_header(avb_1722_1_sdp_dest_addr, DEFAULT_1722_1_SDP_SUBTYPE, message_type,
			  (message_type==ENTITY_AVAILABLE)?AVB_1722_1_SDP_VALID_TIME:0, 40, hdr);

	  SET_WORD(pkt->entity_guid_lo, guid[0]);
	  SET_WORD(pkt->entity_guid_hi, guid[1]);

	  if (message_type==ENTITY_DISCOVER) {
		  pkt->vendor_id[0] = 0;
		  pkt->vendor_id[1] = 0;
		  pkt->model_id[0] = 0;
		  pkt->model_id[1] = 0;
		  pkt->entity_capabilities[0] = 0;
		  pkt->entity_capabilities[1] = 0;
		  pkt->talker_stream_sources = 0;
		  pkt->talker_capabilities = 0;
		  pkt->listener_stream_sinks = 0;
		  pkt->listener_capabilites = 0;
		  pkt->controller_capabilities[0] = 0;
		  pkt->controller_capabilities[1] = 0;
		  pkt->boot_id[0] = 0;
		  pkt->boot_id[1] = 0;
	  } else {
		  SET_WORD(pkt->vendor_id, AVB_1722_1_SDP_VENDOR_ID);
		  SET_WORD(pkt->model_id, AVB_1722_1_SDP_MODEL_ID);
		  SET_WORD(pkt->entity_capabilities, AVB_1722_1_SDP_ENTITY_CAPABILITIES);
		  pkt->talker_stream_sources = AVB_1722_1_SDP_TALKER_STREAM_SOURCES;
		  pkt->talker_capabilities = AVB_1722_1_SDP_TALKER_CAPABILITIES;
		  pkt->listener_stream_sinks = AVB_1722_1_SDP_LISTENER_STREAM_SINKS;
		  pkt->listener_capabilites = AVB_1722_1_SDP_LISTENER_CAPABILITIES;
		  SET_WORD(pkt->controller_capabilities, AVB_1722_1_SDP_CONTROLLER_CAPABILITIES);
		  SET_WORD(pkt->boot_id, AVB_1722_1_SDP_BOOT_ID);
	  }
	  pkt->reserved[0] = 0;
	  pkt->reserved[1] = 0;
	  pkt->reserved[2] = 0;
	  pkt->reserved[3] = 0;
	  pkt->reserved[4] = 0;
	  pkt->reserved[5] = 0;
}

/*
static void avb_1722_1_create_scm_packet(int message_type)
{
	  struct ethernet_hdr_t *hdr = (ethernet_hdr_t*) &avb_1722_1_buf[0];
	  struct avb_1722_1_scm_packet_t *pkt = (avb_1722_1_scm_packet_t*) (hdr + 1);

	  avb_1722_1_create_1722_1_header(avb_1722_1_scm_dest_addr, DEFAULT_1722_1_SCM_SUBTYPE, message_type, 0, 40, hdr);
}

static void avb_1722_1_create_sec_packet(int message_type)
{
	  struct ethernet_hdr_t *hdr = (ethernet_hdr_t*) &avb_1722_1_buf[0];
	  struct avb_1722_1_sec_packet_t *pkt = (avb_1722_1_sec_packet_t*) (hdr + 1);

	  avb_1722_1_create_1722_1_header(avb_1722_1_sec_dest_addr, DEFAULT_1722_1_SEC_SUBTYPE, message_type, 0, 40, hdr);
}
*/

avb_status_t process_avb_1722_1_sdp_packet(avb_1722_1_sdp_packet_t* pkt)
{
	unsigned message_type = GET_1722_1_MSG_TYPE(((avb_1722_1_packet_header_t*)pkt));
	short zero_guid[4] = { 0,0,0,0 };

	switch (message_type) {
	case ENTITY_DISCOVER: {
		if ( compare_guid(pkt->entity_guid_lo, (short*)my_guid) || compare_guid(pkt->entity_guid_lo, zero_guid) )
		{
			if (sdp_advertise_state == SDP_ADVERTISE_WAITING)
				sdp_advertise_state = SDP_ADVERTISE_ADVERTISE_1;
		}
		return AVB_1722_1_OK;
	}

	case ENTITY_AVAILABLE: {
		avb_1722_1_entity_database_add(pkt);
		return AVB_1722_1_ENTITY_ADDED;
	}

	case ENTITY_DEPARTING: {
		avb_1722_1_entity_database_remove(pkt);
		return AVB_1722_1_ENTITY_REMOVED;
	}

	}

	return AVB_1722_1_OK;
}

static avb_status_t process_avb_1722_1_sec_packet(avb_1722_1_sec_packet_t* pkt)
{
	unsigned message_type = GET_1722_1_MSG_TYPE(((avb_1722_1_packet_header_t*)pkt));
	switch (message_type) {
	}
	return AVB_1722_1_OK;
}

static avb_status_t process_avb_1722_1_scm_talker_packet(avb_1722_1_scm_packet_t* pkt)
{
	unsigned response_type=0;
	avb_1722_1_scm_status_type error = SCM_STATUS_SUCCESS;

	if (compare_guid(pkt->talker_guid, (short*)my_guid)==0) return AVB_1722_1_OK;
	if (!scm_talker_valid_talker_unique(pkt)) {
		error = SCM_STATUS_TALKER_UNKNOWN_ID;
	}

	switch (GET_1722_1_MSG_TYPE(((avb_1722_1_packet_header_t*)pkt))) {

	case SCM_CMD_CONNECT_TX_COMMAND:
		if (error != SCM_STATUS_TALKER_UNKNOWN_ID) {
			error = scm_talker_connect_talker();
		}
		response_type=SCM_CMD_CONNECT_TX_RESPONSE;
		break;

	case SCM_CMD_DISCONNECT_TX_COMMAND:
		if (error != SCM_STATUS_TALKER_UNKNOWN_ID) {
			error = scm_talker_disconnect_talker();
		}
		response_type=SCM_CMD_DISCONNECT_TX_RESPONSE;
		break;

	case SCM_CMD_GET_TX_STATE_COMMAND:
		if (error != SCM_STATUS_TALKER_UNKNOWN_ID) {
			error = scm_talker_get_state();
		}
		response_type=SCM_CMD_GET_TX_STATE_RESPONSE;
		break;

	case SCM_CMD_GET_TX_CONNECTION_COMMAND:
		if (error != SCM_STATUS_TALKER_UNKNOWN_ID) {
			error = scm_talker_get_connection();
		}
		response_type=SCM_CMD_GET_TX_CONNECTION_RESPONSE;
		break;
	}

	scm_talker_tx_response();
	return AVB_1722_1_OK;
}

static avb_status_t process_avb_1722_1_scm_listener_packet(avb_1722_1_scm_packet_t* pkt)
{
	unsigned response_type=0;
	avb_1722_1_scm_status_type error = SCM_STATUS_SUCCESS;

	if (compare_guid(pkt->listener_guid, (short*)my_guid)==0) return AVB_1722_1_OK;
	if (!scm_listener_valid_listener_unique(pkt)) {
		error = SCM_STATUS_LISTENER_UNKNOWN_ID;
	}

	switch (GET_1722_1_MSG_TYPE(((avb_1722_1_packet_header_t*)pkt))) {

	case SCM_CMD_CONNECT_TX_RESPONSE:
		if (error != SCM_STATUS_LISTENER_UNKNOWN_ID) {
			error = scm_listener_connect_listener();
		}
		scm_listener_cancel_timeout();
		scm_listener_remove_inflight();
		response_type=SCM_CMD_CONNECT_TX_RESPONSE;
		break;
	case SCM_CMD_DISCONNECT_TX_RESPONSE:
		if (error != SCM_STATUS_LISTENER_UNKNOWN_ID) {
			error = scm_listener_disconnect_listener();
		}
		scm_listener_cancel_timeout();
		scm_listener_remove_inflight();
		response_type=SCM_CMD_DISCONNECT_TX_RESPONSE;
		break;
	case SCM_CMD_CONNECT_RX_COMMAND:
		if (error != SCM_STATUS_LISTENER_UNKNOWN_ID) {
			if (!scm_listener_listener_is_connected()) {
				scm_listener_tx_command();
				return AVB_1722_1_OK;
			} else {
				error = SCM_STATUS_LISTENER_EXCLUSIVE;
			}
		}
		response_type=SCM_CMD_CONNECT_RX_RESPONSE;
		break;
	case SCM_CMD_DISCONNECT_RX_COMMAND:
		if (error != SCM_STATUS_LISTENER_UNKNOWN_ID) {
			if (scm_listener_listener_is_connected()) {
				scm_listener_tx_command();
				return AVB_1722_1_OK;
			} else {
				error = SCM_STATUS_NOT_CONNECTED;
			}
		}
		response_type=SCM_CMD_DISCONNECT_RX_RESPONSE;
		break;
	case SCM_CMD_GET_RX_STATE_COMMAND:
		if (error != SCM_STATUS_LISTENER_UNKNOWN_ID) {
			error = scm_listener_get_state();
		}
		response_type=SCM_CMD_GET_RX_STATE_RESPONSE;
		break;
	}


	scm_listener_tx_response();

	return AVB_1722_1_OK;
}

static avb_status_t process_avb_1722_1_scm_packet(avb_1722_1_scm_packet_t* pkt)
{
	unsigned message_type = GET_1722_1_MSG_TYPE(((avb_1722_1_packet_header_t*)pkt));

	switch (message_type) {

	// Talker messages
	case SCM_CMD_CONNECT_TX_COMMAND:
	case SCM_CMD_DISCONNECT_TX_COMMAND:
	case SCM_CMD_GET_TX_STATE_COMMAND:
	case SCM_CMD_GET_TX_CONNECTION_COMMAND:
		return process_avb_1722_1_scm_talker_packet(pkt);

	// Listener messages
	case SCM_CMD_CONNECT_TX_RESPONSE:
	case SCM_CMD_DISCONNECT_TX_RESPONSE:
	case SCM_CMD_CONNECT_RX_COMMAND:
	case SCM_CMD_DISCONNECT_RX_COMMAND:
	case SCM_CMD_GET_RX_STATE_COMMAND:
		return process_avb_1722_1_scm_listener_packet(pkt);
	}

	return AVB_1722_1_OK;
}

avb_status_t avb_1722_1_process_packet(unsigned int buf0[], int len, chanend c_tx)
{
	  unsigned char *buf = (unsigned char *) buf0;

	  struct ethernet_hdr_t *ethernet_hdr = (ethernet_hdr_t *) &buf[0];
	  struct tagged_ethernet_hdr_t *tagged_ethernet_hdr = (tagged_ethernet_hdr_t *) &buf[0];

	  int has_qtag = ethernet_hdr->ethertype[1]==0x18;
	  int ethernet_pkt_size = has_qtag ? 18 : 14;

	  struct avb_1722_1_packet_header_t *pkt =
	    (struct avb_1722_1_packet_header_t *) &buf[ethernet_pkt_size];

	  if (has_qtag) {
	    if (tagged_ethernet_hdr->ethertype[1] != (AVB_ETYPE & 0xff) ||
	        tagged_ethernet_hdr->ethertype[0] != (AVB_ETYPE >> 8)) {
	        // not a 1722 packet
	        return AVB_NO_STATUS;
	      }
	  } else {
	    if (ethernet_hdr->ethertype[1] != (AVB_ETYPE & 0xff) ||
	        ethernet_hdr->ethertype[0] != (AVB_ETYPE >> 8)) {
	        // not a 1722 packet
	        return AVB_NO_STATUS;
	      }
	  }

	  if (GET_1722_1_CD_FLAG(pkt) != 1)
		    // not a 1722.1 packet
		    return AVB_NO_STATUS;

	  {
		  unsigned subtype = GET_1722_1_SUBTYPE(pkt);
		  switch (subtype) {
		  case DEFAULT_1722_1_SDP_SUBTYPE:
			  return process_avb_1722_1_sdp_packet((avb_1722_1_sdp_packet_t*)pkt);
		  case DEFAULT_1722_1_SEC_SUBTYPE:
			  return process_avb_1722_1_sec_packet((avb_1722_1_sec_packet_t*)pkt);
		  case DEFAULT_1722_1_SCM_SUBTYPE:
			  return process_avb_1722_1_scm_packet((avb_1722_1_scm_packet_t*)pkt);
		  default:
			  return AVB_NO_STATUS;
		  }
	  }

	  return AVB_NO_STATUS;
}

static void avb_1722_1_scm_talker_periodic(chanend c_tx)
{
	switch (scm_talker_state) {
	case SCM_TALKER_IDLE:
		break;
	case SCM_TALKER_WAITING:
		break;
	}
}

static void avb_1722_1_scm_listener_periodic(chanend c_tx)
{
	switch (scm_listener_state) {
	case SCM_LISTENER_IDLE:
		break;
	case SCM_LISTENER_WAITING:
		break;
	}
}

static void avb_1722_1_sdp_discovery_periodic(chanend c_tx)
{
	switch (sdp_discovery_state) {
	case SDP_DISCOVERY_IDLE:
		break;

	case SDP_DISCOVERY_WAITING: {
			if (avb_timer_expired(&sdp_discovery_timer)) {
				sdp_two_second_counter++;
				avb_1722_1_entity_database_check_timeout();
				start_avb_timer(&sdp_discovery_timer, 1);
			}
		}
		break;

	case SDP_DISCOVERY_DISCOVER: {
			avb_1722_1_create_sdp_packet(ENTITY_DISCOVER, discover_guid);
			mac_tx(c_tx, avb_1722_1_buf, 60, ETH_BROADCAST);
			sdp_discovery_state = SDP_DISCOVERY_WAITING;
		}
		break;
	}
}

static void avb_1722_1_sdp_advertising_periodic(chanend c_tx)
{
	switch (sdp_advertise_state) {
	case SDP_ADVERTISE_IDLE:
		break;

	case SDP_ADVERTISE_ADVERTISE_1:
		avb_1722_1_create_sdp_packet(ENTITY_AVAILABLE, my_guid);
		mac_tx(c_tx, avb_1722_1_buf, 60, ETH_BROADCAST);
		start_avb_timer(&sdp_advertise_timer, 3); // 3 centiseconds
		sdp_advertise_state = SDP_ADVERTISE_ADVERTISE_2;
		break;

	case SDP_ADVERTISE_ADVERTISE_2:
		if (avb_timer_expired(&sdp_advertise_timer)) {
			avb_1722_1_create_sdp_packet(ENTITY_AVAILABLE, my_guid);
			mac_tx(c_tx, avb_1722_1_buf, 60, ETH_BROADCAST);
			start_avb_timer(&sdp_readvertise_timer, AVB_1722_1_SDP_VALID_TIME);
			sdp_advertise_state = SDP_ADVERTISE_WAITING;
		}
		break;

	case SDP_ADVERTISE_WAITING:
		if (avb_timer_expired(&sdp_readvertise_timer)) {
			sdp_advertise_state = SDP_ADVERTISE_ADVERTISE_1;
		}
		break;

	case SDP_ADVERTISE_DEPARTING_1:
		avb_1722_1_create_sdp_packet(ENTITY_DEPARTING, my_guid);
		mac_tx(c_tx, avb_1722_1_buf, 60, ETH_BROADCAST);
		start_avb_timer(&sdp_advertise_timer, 3); // 3 centiseconds
		sdp_advertise_state = SDP_ADVERTISE_DEPARTING_2;
		break;

	case SDP_ADVERTISE_DEPARTING_2:
		if (avb_timer_expired(&sdp_advertise_timer)) {
			avb_1722_1_create_sdp_packet(ENTITY_DEPARTING, my_guid);
			mac_tx(c_tx, avb_1722_1_buf, 60, ETH_BROADCAST);
			sdp_advertise_state = SDP_ADVERTISE_IDLE;
		}
		break;

	default:
		break;
	}
}

void avb_1722_1_periodic(chanend c_tx)
{
	avb_1722_1_sdp_advertising_periodic(c_tx);
	avb_1722_1_sdp_discovery_periodic(c_tx);
	avb_1722_1_scm_listener_periodic(c_tx);
	avb_1722_1_scm_talker_periodic(c_tx);
}


void avb_1722_1_sdp_announce()
{
	if (sdp_advertise_state == SDP_ADVERTISE_IDLE) sdp_advertise_state = SDP_ADVERTISE_ADVERTISE_1;
}


void avb_1722_1_sdp_depart()
{
	if (sdp_advertise_state == SDP_ADVERTISE_IDLE) sdp_advertise_state = SDP_ADVERTISE_DEPARTING_1;
}

void avb_1722_1_sdp_discover(unsigned guid[])
{
	if (sdp_discovery_state == SDP_DISCOVERY_WAITING) {
		sdp_discovery_state = SDP_DISCOVERY_DISCOVER;
		discover_guid[0] = guid[0];
		discover_guid[1] = guid[1];
	}
}

void avb_1722_1_sdp_discover_all()
{
	unsigned guid[2] = {0,0};
	avb_1722_1_sdp_discover(guid);
}