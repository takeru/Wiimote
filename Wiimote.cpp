#include <esp_bt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <esp32-hal-log.h>
#include <esp32-hal-bt.h>

#if !defined(CONFIG_BT_ENABLED) || !defined(CONFIG_BLUEDROID_ENABLED)
#error Bluetooth is not enabled! Please run `make menuconfig` to and enable it
#endif

#ifndef CONFIG_CLASSIC_BT_ENABLED
#error Need CLASSIC BT.
#endif

#include "wiimote_bt.h"
#include "Wiimote.h"

#define PSM_HID_Control_11   0x0011
#define PSM_HID_Interrupt_13 0x0013

static uint8_t tmp_data[256];
static bool hciInit = false;
static bool wiimoteConnected = false;
static wiimote_callback_t wiimote_callback[4];
static uint8_t _g_identifier = 1;
static uint16_t _g_local_cid = 0x0040;

/**
 * Queue
 */
typedef struct {
  size_t len;
  uint8_t data[];
} lendata_t;
#define RX_QUEUE_SIZE 32
#define TX_QUEUE_SIZE 32
static xQueueHandle _rx_queue = NULL;
static xQueueHandle _tx_queue = NULL;
static esp_err_t _queue_data(xQueueHandle queue, uint8_t *data, size_t len){
  if(!data || !len){
    log_w("No data provided");
    return ESP_OK;
  }
  lendata_t * lendata = (lendata_t*)malloc(sizeof(lendata_t) + len);
  if(!lendata){
    log_e("lendata Malloc Failed!");
    return ESP_FAIL;
  }
  lendata->len = len;
  memcpy(lendata->data, data, len);
  if (xQueueSend(queue, &lendata, portMAX_DELAY) != pdPASS) {
    log_e("xQueueSend failed");
    free(lendata);
    return ESP_FAIL;
  }
  return ESP_OK;
}

/**
 * Utils
 */
#define FORMAT_HEX_MAX_BYTES 30
static char formatHexBuffer[FORMAT_HEX_MAX_BYTES*3+3+1];
static char* formatHex(uint8_t* data, uint16_t len){
  for(uint16_t i=0; i<len && i<FORMAT_HEX_MAX_BYTES; i++){
    sprintf(formatHexBuffer+3*i, "%02X ", data[i]);
  }
  if(FORMAT_HEX_MAX_BYTES<len){
    sprintf(formatHexBuffer+3*FORMAT_HEX_MAX_BYTES, "...");
  }
  return formatHexBuffer;
}

/**
 * Scanned device list
 */
struct scanned_device_t {
  bd_addr_t bd_addr;
  uint8_t psrm;
  uint16_t clkofs;
};
static int scanned_device_list_size = 0;
#define SCANNED_DEVICE_LIST_SIZE 16
static scanned_device_t scanned_device_list[SCANNED_DEVICE_LIST_SIZE];
static int scanned_device_find(struct bd_addr_t *bd_addr){
  for(int i=0; i<scanned_device_list_size; i++){
    scanned_device_t *c = &scanned_device_list[i];
    if(memcmp(&bd_addr->addr, c->bd_addr.addr, BD_ADDR_LEN) == 0){
      return i;
    }
  }
  return -1;
}
static int scanned_device_add(struct scanned_device_t scanned_device){
  if(SCANNED_DEVICE_LIST_SIZE == scanned_device_list_size){
    return -1;
  }
  scanned_device_list[scanned_device_list_size++] = scanned_device;
  return scanned_device_list_size;
}
static void scanned_device_clear(void){
  scanned_device_list_size = 0;
}

/**
 * L2CAP Connection
 */
struct l2cap_connection_t {
  uint16_t connection_handle;
  uint16_t psm;
  uint16_t local_cid;
  uint16_t remote_cid;
};
static int l2cap_connection_size = 0;
#define L2CAP_CONNECTION_LIST_SIZE 8
static l2cap_connection_t l2cap_connection_list[L2CAP_CONNECTION_LIST_SIZE];
static int l2cap_connection_find_by_psm(uint16_t connection_handle, uint16_t psm){
  for(int i=0; i<l2cap_connection_size; i++){
    l2cap_connection_t *c = &l2cap_connection_list[i];
    if(connection_handle == c->connection_handle && psm == c->psm){
      return i;
    }
  }
  return -1;
}
static int l2cap_connection_find_by_local_cid(uint16_t connection_handle, uint16_t local_cid){
  for(int i=0; i<l2cap_connection_size; i++){
    l2cap_connection_t *c = &l2cap_connection_list[i];
    if(connection_handle == c->connection_handle && local_cid == c->local_cid){
      return i;
    }
  }
  return -1;
}

static int l2cap_connection_add(struct l2cap_connection_t l2cap_connection){
  if(L2CAP_CONNECTION_LIST_SIZE == l2cap_connection_size){
    return -1;
  }
  l2cap_connection_list[l2cap_connection_size++] = l2cap_connection;
  return l2cap_connection_size;
}
static void l2cap_connection_clear(void){
  l2cap_connection_size = 0;
}

static void start_scan(void);

/**
 * callback 
 */
static void _notify_host_send_available(void){
  if(!hciInit){
    start_scan();
    hciInit = true;
  }
}

static int _notify_host_recv(uint8_t *data, uint16_t len){
  if(ESP_OK == _queue_data(_rx_queue, data, len)){
    return ESP_OK;
  }else{
    return ESP_FAIL;
  }
}

static const esp_vhci_host_callback_t callback = {
  _notify_host_send_available,
  _notify_host_recv
};

static void start_scan(void){
  scanned_device_clear();
  l2cap_connection_clear();
  uint16_t len = make_cmd_reset(tmp_data);
  _queue_data(_tx_queue, tmp_data, len); // TODO: check return 
  log_d("queued reset.");
}

static void process_command_complete_event(uint8_t len, uint8_t* data){
  if(data[1]==0x03 && data[2]==0x0C){ // reset
    // data[0] Num_HCI_Command_Packets
    if(data[3]==0x00){ // OK
      log_d("reset OK.");
      uint16_t len = make_cmd_read_bd_addr(tmp_data);
      _queue_data(_tx_queue, tmp_data, len); // TODO: check return
      log_d("queued read_bd_addr.");
    }else{
      log_d("reset failed.");
    }
  }else
  if(data[1]==0x09 && data[2]==0x10){ // read_bd_addr
    // data[0] Num_HCI_Command_Packets
    if(data[3]==0x00){ // OK
      log_d("read_bd_addr OK. BD_ADDR=%s", formatHex(data+4, 6));

      char name[] = "ESP32-BT-L2CAP";
      log_d("sizeof(name)=%d", sizeof(name));
      uint16_t len = make_cmd_write_local_name(tmp_data, (uint8_t*)name, sizeof(name));
      _queue_data(_tx_queue, tmp_data, len); // TODO: check return
      log_d("queued write_local_name.");
    }else{
      log_d("read_bd_addr failed.");
    }
  }else
  if(data[1]==0x13 && data[2]==0x0C){ // write_local_name
    // data[0] Num_HCI_Command_Packets
    if(data[3]==0x00){ // OK
      log_d("write_local_name OK.");
      uint8_t cod[3] = {0x04, 0x05, 0x00};
      uint16_t len = make_cmd_write_class_of_device(tmp_data, cod);
      _queue_data(_tx_queue, tmp_data, len); // TODO: check return
      log_d("queued write_class_of_device.");
    }else{
      log_d("write_local_name failed.");
    }
  }else
  if(data[1]==0x24 && data[2]==0x0C){ // write_class_of_device
    // data[0] Num_HCI_Command_Packets
    if(data[3]==0x00){ // OK
      log_d("write_class_of_device OK.");
      uint16_t len = make_cmd_write_scan_enable(tmp_data, 3);
      _queue_data(_tx_queue, tmp_data, len); // TODO: check return
      log_d("queued write_scan_enable.");
    }else{
      log_d("write_class_of_device failed.");
    }
  }else
  if(data[1]==0x1A && data[2]==0x0C){ // write_scan_enable
    // data[0] Num_HCI_Command_Packets
    if(data[3]==0x00){ // OK
      log_d("write_scan_enable OK.");

      scanned_device_clear();
      uint16_t len = make_cmd_inquiry(tmp_data, 0x9E8B33, 0x05/*0x30*/, 0x00);
      _queue_data(_tx_queue, tmp_data, len); // TODO: check return
      log_d("queued inquiry.");
    }else{
      log_d("write_scan_enable failed.");
    }
  }else
  if(data[1]==0x02 && data[2]==0x04){ // inquiry_cancel
    // data[0] Num_HCI_Command_Packets
    if(data[3]==0x00){ // OK
      log_d("inquiry_cancel OK.");
    }else{
      log_d("inquiry_cancel failed.");
    }
  }else{
    log_d("!!! process_command_complete_event no impl !!!");
  }
}

static void process_command_status_event(uint8_t len, uint8_t* data){
  if(data[2]==0x01 && data[3]==0x04){ // inquiry
    // data[1] Num_HCI_Command_Packets
    if(data[0]==0x00){ // 0x00=pending
      log_d("inquiry pending!");
    }else{
      log_d("inquiry failed. error=%02X", data[0]);
    }
  }else
  if(data[2]==0x19 && data[3]==0x04){
    // data[1] Num_HCI_Command_Packets
    if(data[0]==0x00){ // 0x00=pending
      log_d("remote_name_request pending!");
    }else{
      log_d("remote_name_request failed. error=%02X", data[0]);
    }
  }else
  if(data[2]==0x05 && data[3]==0x04){
    // data[1] Num_HCI_Command_Packets
    if(data[0]==0x00){ // 0x00=pending
      log_d("create_connection pending!");
    }else{
      log_d("create_connection failed. error=%02X", data[0]);
    }
  }else{
      log_d("!!! process_command_status_event no impl !!!");
  }
}

static void process_inquiry_result_event(uint8_t len, uint8_t* data){
  uint8_t num = data[0];
  //log_d("inquiry_result num=%d", num);

  for(int i=0; i<num; i++){
    int pos = 1 + (6+1+2+3+2) * i;

    struct bd_addr_t bd_addr;
    STREAM_TO_BDADDR(bd_addr.addr, data+pos);

    log_d("**** inquiry_result BD_ADDR(%d/%d) = %s", i, num, formatHex((uint8_t*)&bd_addr.addr, BD_ADDR_LEN));

    int idx = scanned_device_find(&bd_addr);
    if(idx == -1){
      log_d("    Page_Scan_Repetition_Mode = %02X", data[pos+6]);
      // data[pos+7] data[pos+8] // Reserved
      log_d("    Class_of_Device = %02X %02X %02X", data[pos+9], data[pos+10], data[pos+11]);
      log_d("    Clock_Offset = %02X %02X", data[pos+12], data[pos+13]);

      struct scanned_device_t scanned_device;
      scanned_device.bd_addr = bd_addr;
      scanned_device.psrm    = data[pos+6];
      scanned_device.clkofs  = ((0x80 | data[pos+12]) << 8) | (data[pos+13]);

      idx = scanned_device_add(scanned_device);
      if(0<=idx){
        if(data[pos+9]==0x04 && data[pos+10]==0x25 && data[pos+11]==0x00){ // Filter for Wiimote [04 25 00] 
          uint16_t len = make_cmd_remote_name_request(tmp_data, scanned_device.bd_addr, scanned_device.psrm, scanned_device.clkofs);
          _queue_data(_tx_queue, tmp_data, len); // TODO: check return
          log_d("queued remote_name_request.");
        }else{
          log_d("skiped to remote_name_request. (not Wiimote COD)");
        }
      }else{
        log_d("failed to scanned_list_add.");
      }
    }else{
      log_d(" (dup idx=%d)", idx);
    }
  }
}

static void process_inquiry_complete_event(uint8_t len, uint8_t* data){
  uint8_t status = data[0];
  log_d("inquiry_complete status=%02X", status);
  start_scan();
}

static void process_remote_name_request_complete_event(uint8_t len, uint8_t* data){
  uint8_t status = data[0];
  log_d("remote_name_request_complete status=%02X", status);
  struct bd_addr_t bd_addr;
  STREAM_TO_BDADDR(bd_addr.addr, data+1);
  log_d("  BD_ADDR = %s", formatHex((uint8_t*)&bd_addr.addr, BD_ADDR_LEN));

  char* name = (char*)(data+7);
  log_d("  REMOTE_NAME = %s", name);

  int idx = scanned_device_find(&bd_addr);
  if(0<=idx && strcmp("Nintendo RVL-CNT-01", name)==0){
    {
      uint16_t len = make_cmd_inquiry_cancel(tmp_data);
      _queue_data(_tx_queue, tmp_data, len); // TODO: check return
      log_d("queued inquiry_cancel.");
    }

    struct scanned_device_t scanned_device = scanned_device_list[idx];

    uint16_t pt = 0x0008;
    uint8_t ars = 0x00;
    uint16_t len = make_cmd_create_connection(tmp_data, scanned_device.bd_addr, pt, scanned_device.psrm, scanned_device.clkofs, ars);
    _queue_data(_tx_queue, tmp_data, len); // TODO: check return
    log_d("queued create_connection.");
  }
}

static void _l2cap_connect(uint16_t connection_handle, uint16_t psm, uint16_t source_cid){
  uint8_t  packet_boundary_flag = 0b10; // Packet_Boundary_Flag
  uint8_t  broadcast_flag       = 0b00; // Broadcast_Flag
  uint16_t channel_id           = 0x0001;
  uint8_t data[] = {
    0x02,                               // CONNECTION REQUEST
    _g_identifier++,                    // Identifier
    0x04, 0x00,                         // Length:     0x0004
    psm        & 0xFF, psm        >> 8, // PSM: HID_Control=0x0011, HID_Interrupt=0x0013
    source_cid & 0xFF, source_cid >> 8  // Source CID: 0x0040+
  };
  uint16_t data_len = 8;
  uint16_t len = make_acl_l2cap_single_packet(tmp_data, connection_handle, packet_boundary_flag, broadcast_flag, channel_id, data, data_len);
  _queue_data(_tx_queue, tmp_data, len); // TODO: check return
  log_d("queued acl_l2cap_single_packet(CONNECTION REQUEST)");

  struct l2cap_connection_t l2cap_connection;
  l2cap_connection.connection_handle = connection_handle;
  l2cap_connection.psm               = psm;
  l2cap_connection.local_cid         = source_cid;
  l2cap_connection.remote_cid        = 0;
  int idx = l2cap_connection_add(l2cap_connection);
  if(idx == -1){
    log_d("!!! l2cap_connection_add failed.");
  }
}

static void _set_rumble(uint16_t connection_handle, bool rumble){
  int idx = l2cap_connection_find_by_psm(connection_handle, PSM_HID_Interrupt_13);
  struct l2cap_connection_t l2cap_connection = l2cap_connection_list[idx];

  uint8_t  packet_boundary_flag = 0b10; // Packet_Boundary_Flag
  uint8_t  broadcast_flag       = 0b00; // Broadcast_Flag
  uint16_t channel_id           = l2cap_connection.remote_cid;
  uint8_t data[] = {
    0xA2,
    0x10,
    rumble ? 0x01 : 0x00 // 0x0? - 0xF?
  };
  uint16_t data_len = 3;
  uint16_t len = make_acl_l2cap_single_packet(tmp_data, connection_handle, packet_boundary_flag, broadcast_flag, channel_id, data, data_len);
  _queue_data(_tx_queue, tmp_data, len); // TODO: check return
  log_d("queued acl_l2cap_single_packet(Set Rumble)");
}

static void _set_led(uint16_t connection_handle, uint8_t leds){
  int idx = l2cap_connection_find_by_psm(connection_handle, PSM_HID_Interrupt_13);
  struct l2cap_connection_t l2cap_connection = l2cap_connection_list[idx];

  uint8_t  packet_boundary_flag = 0b10; // Packet_Boundary_Flag
  uint8_t  broadcast_flag       = 0b00; // Broadcast_Flag
  uint16_t channel_id           = l2cap_connection.remote_cid;
  uint8_t data[] = {
    0xA2,
    0x11,
    leds << 4 // 0x0? - 0xF?
  };
  uint16_t data_len = 3;
  uint16_t len = make_acl_l2cap_single_packet(tmp_data, connection_handle, packet_boundary_flag, broadcast_flag, channel_id, data, data_len);
  _queue_data(_tx_queue, tmp_data, len); // TODO: check return
  log_d("queued acl_l2cap_single_packet(Set LEDs)");
}

static void _set_reporting_mode(uint16_t connection_handle, uint8_t reporting_mode, bool continuous){
  int idx = l2cap_connection_find_by_psm(connection_handle, PSM_HID_Interrupt_13);
  struct l2cap_connection_t l2cap_connection = l2cap_connection_list[idx];

  uint8_t  packet_boundary_flag = 0b10; // Packet_Boundary_Flag
  uint8_t  broadcast_flag       = 0b00; // Broadcast_Flag
  uint16_t channel_id           = l2cap_connection.remote_cid;
  uint8_t data[] = {
    0xA2,
    0x12,
    continuous ? 0x04 : 0x00, // 0x00, 0x04
    reporting_mode
  };
  uint16_t data_len = 4;
  uint16_t len = make_acl_l2cap_single_packet(tmp_data, connection_handle, packet_boundary_flag, broadcast_flag, channel_id, data, data_len);
  _queue_data(_tx_queue, tmp_data, len); // TODO: check return
  log_d("queued acl_l2cap_single_packet(Set reporting mode)");
}

enum address_space_t {
  EEPROM_MEMORY,
  CONTROL_REGISTER
};

static uint8_t _address_space(address_space_t as)
{
  switch(as){
    case EEPROM_MEMORY   : return 0x00;
    case CONTROL_REGISTER: return 0x04;
  }
  return 0xFF;
}

static void _write_memory(uint16_t connection_handle, address_space_t as, uint32_t offset, uint8_t size, const uint8_t* d){
  int idx = l2cap_connection_find_by_psm(connection_handle, PSM_HID_Interrupt_13);
  struct l2cap_connection_t l2cap_connection = l2cap_connection_list[idx];

  uint8_t  packet_boundary_flag = 0b10; // Packet_Boundary_Flag
  uint8_t  broadcast_flag       = 0b00; // Broadcast_Flag
  uint16_t channel_id           = l2cap_connection.remote_cid;
  // (a2) 16 MM FF FF FF SS DD DD DD DD DD DD DD DD DD DD DD DD DD DD DD DD
  uint8_t data[] = {
    0xA2,
    0x16, // Write
    _address_space(as),    // MM 0x00=EEPROM, 0x04=ControlRegister
    (offset >> 16) & 0xFF, // FF
    (offset >>  8) & 0xFF, // FF
    (offset      ) & 0xFF, // FF
    size,                  // SS size 1..16
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
  };
  memcpy(data+7, d, size);

  uint16_t data_len = 7 + 16;
  uint16_t len = make_acl_l2cap_single_packet(tmp_data, connection_handle, packet_boundary_flag, broadcast_flag, channel_id, data, data_len);
  _queue_data(_tx_queue, tmp_data, len); // TODO: check return
  log_d("queued acl_l2cap_single_packet(write memory)");
}

static void _read_memory(uint16_t connection_handle, address_space_t as, uint32_t offset, uint16_t size){
  int idx = l2cap_connection_find_by_psm(connection_handle, PSM_HID_Interrupt_13);
  struct l2cap_connection_t l2cap_connection = l2cap_connection_list[idx];

  uint8_t  packet_boundary_flag = 0b10; // Packet_Boundary_Flag
  uint8_t  broadcast_flag       = 0b00; // Broadcast_Flag
  uint16_t channel_id           = l2cap_connection.remote_cid;
  // (a2) 17 MM FF FF FF SS SS
  uint8_t data[] = {
    0xA2,
    0x17, // Read
    _address_space(as),    // MM 0x00=EEPROM, 0x04=ControlRegister
    (offset >> 16) & 0xFF, // FF
    (offset >>  8) & 0xFF, // FF
    (offset      ) & 0xFF, // FF
    (size >> 8   ) & 0xFF, // SS
    (size        ) & 0xFF  // SS
  };
  uint16_t data_len = 8;
  uint16_t len = make_acl_l2cap_single_packet(tmp_data, connection_handle, packet_boundary_flag, broadcast_flag, channel_id, data, data_len);
  _queue_data(_tx_queue, tmp_data, len); // TODO: check return
  log_d("queued acl_l2cap_single_packet(read memory)");
}

static void process_connection_complete_event(uint8_t len, uint8_t* data){
  uint8_t status = data[0];
  log_d("connection_complete status=%02X", status);

  uint16_t connection_handle = data[2] << 8 | data[1];
  struct bd_addr_t bd_addr;
  STREAM_TO_BDADDR(bd_addr.addr, data+3);
  uint8_t lt = data[9];  // Link_Type
  uint8_t ee = data[10]; // Encryption_Enabled

  log_d("  Connection_Handle  = 0x%04X", connection_handle);
  log_d("  BD_ADDR            = %s", formatHex((uint8_t*)&bd_addr.addr, BD_ADDR_LEN));
  log_d("  Link_Type          = %02X", lt);
  log_d("  Encryption_Enabled = %02X", ee);

  _l2cap_connect(connection_handle, PSM_HID_Control_11, _g_local_cid++);
}

static void process_disconnection_complete_event(uint8_t len, uint8_t* data){
  uint8_t status = data[0];
  log_d("disconnection_complete status=%02X", status);

  uint16_t ch = data[2] << 8 | data[1]; //Connection_Handle
  uint8_t reason = data[3];  // Reason

  log_d("  Connection_Handle  = 0x%04X", ch);
  log_d("  Reason             = %02X", reason);

  wiimoteConnected = false;
  start_scan();
}

static void process_l2cap_connection_response(uint16_t connection_handle, uint8_t* data){
  uint8_t identifier       =  data[ 1];
  uint16_t len             = (data[ 3] << 8) | data[ 2];
  uint16_t destination_cid = (data[ 5] << 8) | data[ 4];
  uint16_t source_cid      = (data[ 7] << 8) | data[ 6];
  uint16_t result          = (data[ 9] << 8) | data[ 8];
  uint16_t status          = (data[11] << 8) | data[10];

  log_d("L2CAP CONNECTION RESPONSE");
  log_d("  identifier      = %02X", identifier);
  log_d("  destination_cid = %04X", destination_cid);
  log_d("  source_cid      = %04X", source_cid);
  log_d("  result          = %04X", result);
  log_d("  status          = %04X", status);

  if(result == 0x0000){
    int idx = l2cap_connection_find_by_local_cid(connection_handle, source_cid);
    struct l2cap_connection_t *l2cap_connection = &l2cap_connection_list[idx];
    l2cap_connection->remote_cid = destination_cid;

    uint8_t  packet_boundary_flag = 0b10; // Packet_Boundary_Flag
    uint8_t  broadcast_flag       = 0b00; // Broadcast_Flag
    uint16_t channel_id           = 0x0001;
    uint8_t data[] = {
      0x04,       // CONFIGURATION REQUEST
      _g_identifier++, // Identifier
      0x08, 0x00, // Length: 0x0008
      destination_cid & 0xFF, destination_cid >> 8, // Destination CID
      0x00, 0x00, // Flags
      0x01, 0x02, 0x40, 0x00 // type=01 len=02 value=00 40
    };
    uint16_t data_len = 12;
    uint16_t len = make_acl_l2cap_single_packet(tmp_data, connection_handle, packet_boundary_flag, broadcast_flag, channel_id, data, data_len);
    _queue_data(_tx_queue, tmp_data, len); // TODO: check return
    log_d("queued acl_l2cap_single_packet(CONFIGURATION REQUEST)");
  }
}

static void process_l2cap_configuration_response(uint16_t connection_handle, uint8_t* data){
  uint8_t identifier       =  data[ 1];
  uint16_t len             = (data[ 3] << 8) | data[ 2];
  uint16_t source_cid      = (data[ 5] << 8) | data[ 4];
  uint16_t flags           = (data[ 7] << 8) | data[ 6];
  uint16_t result          = (data[ 9] << 8) | data[ 8];
  // config = data[10..]

  log_d("L2CAP CONFIGURATION RESPONSE");
  log_d("  identifier      = %02X", identifier);
  log_d("  len             = %04X", len);
  log_d("  source_cid      = %04X", source_cid);
  log_d("  flags           = %04X", flags);
  log_d("  result          = %04X", result);
  log_d("  config          = %s", formatHex(data+10, len-6));
}

static void process_l2cap_configuration_request(uint16_t connection_handle, uint8_t* data){
  uint8_t identifier       =  data[ 1];
  uint16_t len             = (data[ 3] << 8) | data[ 2];
  uint16_t destination_cid = (data[ 5] << 8) | data[ 4];
  uint16_t flags           = (data[ 7] << 8) | data[ 6];
  // config = data[8..]

  log_d("L2CAP CONFIGURATION REQUEST");
  log_d("  identifier      = %02X", identifier);
  log_d("  len             = %02X", len);
  log_d("  destination_cid = %04X", destination_cid);
  log_d("  flags           = %04X", flags);
  log_d("  config          = %s", formatHex(data+8, len-4));

  if(flags != 0x0000){
    log_d("!!! flags!=0x0000");
    return;
  }
  if(len != 0x08){
    log_d("!!! len!=0x08");
    return;
  }
  if(data[8]==0x01 && data[9]==0x02){ // MTU
    uint16_t mtu = (data[11] << 8) | data[10];
    log_d("  MTU=%d", mtu);

    int idx = l2cap_connection_find_by_local_cid(connection_handle, destination_cid);
    struct l2cap_connection_t l2cap_connection = l2cap_connection_list[idx];

    uint8_t  packet_boundary_flag = 0b10; // Packet_Boundary_Flag
    uint8_t  broadcast_flag       = 0b00; // Broadcast_Flag
    uint16_t channel_id           = 0x0001;
    uint16_t source_cid           = l2cap_connection.remote_cid;
    uint8_t data[] = {
      0x05,       // CONFIGURATION RESPONSE
      identifier, // Identifier
      0x0A, 0x00, // Length: 0x000A
      source_cid & 0xFF, source_cid >> 8, // Source CID
      0x00, 0x00, // Flags
      0x00, 0x00, // Res
      0x01, 0x02, mtu & 0xFF, mtu >> 8 // type=01 len=02 value=xx xx
    };
    uint16_t data_len = 14;
    uint16_t len = make_acl_l2cap_single_packet(tmp_data, connection_handle, packet_boundary_flag, broadcast_flag, channel_id, data, data_len);
    _queue_data(_tx_queue, tmp_data, len); // TODO: check return
    log_d("queued acl_l2cap_single_packet(CONFIGURATION RESPONSE)");

    if(l2cap_connection.psm == PSM_HID_Control_11){
      _l2cap_connect(connection_handle, PSM_HID_Interrupt_13, _g_local_cid++);
    }
  }
}

static void process_report(uint8_t* data, uint16_t len){
  log_d("REPORT len=%d data=%s", len, formatHex(data, len));
  // TODO callback[1,2,3]
  uint8_t number = 0;
  wiimote_callback_t cb = wiimote_callback[number];
  if(cb){
    cb(number, data, len);
  }
}

static void process_extension_controller_reports(uint16_t connection_handle, uint16_t channel_id, uint8_t* data, uint16_t len){
  static int controller_query_state = 0;

  switch(controller_query_state){
  case 0:
    // 0x20 Status
    // (a1) 20 BB BB LF 00 00 VV
    if(data[1] == 0x20){
      if(data[4] & 0x02){ // extension controller is connected
        _write_memory(connection_handle, CONTROL_REGISTER, 0xA400F0, 1, (const uint8_t[]){0x55});
        controller_query_state = 1;
      }else{ // extension controller is NOT connected
        _set_reporting_mode(connection_handle, 0x30, false); // 0x30: Core Buttons : 30 BB BB
        //_set_reporting_mode(connection_handle, 0x31, false); // 0x31: Core Buttons and Accelerometer : 31 BB BB AA AA AA
      }
    }
    break;
  case 1:
    // A1 22 00 00 16 00 => OK
    // A1 22 00 00 16 04 => NG
    if(data[1]==0x22 && data[4]==0x16){
      if(data[5]==0x00){
        _write_memory(connection_handle, CONTROL_REGISTER, 0xA400FB, 1, (const uint8_t[]){0x00});
        controller_query_state = 2;
      }else{
        controller_query_state = 0;
      }
    }
    break;
  case 2:
    if(data[1]==0x22 && data[4]==0x16){
      if(data[5]==0x00){
        _read_memory(connection_handle, CONTROL_REGISTER, 0xA400FA, 6); // read controller type
        controller_query_state = 3;
      }else{
        controller_query_state = 0;
      }
    }
    break;
  case 3:
    // 0x21 Read response
    // (a1) 21 BB BB SE FF FF DD DD DD DD DD DD DD DD DD DD DD DD DD DD DD DD
    if(data[1] == 0x21){
      if(memcmp(data+5, (const uint8_t[]){0x00, 0xFA}, 2)==0){
        if(memcmp(data+7, (const uint8_t[]){0x00, 0x00, 0xA4, 0x20, 0x00, 0x00}, 6)==0){ // Nunchuck
          _set_reporting_mode(connection_handle, 0x32, false); // 0x32: Core Buttons with 8 Extension bytes : 32 BB BB EE EE EE EE EE EE EE EE
        }
        controller_query_state = 0;
      }
    }
    break;
  }
}

static void process_l2cap_data(uint16_t connection_handle, uint16_t channel_id, uint8_t* data, uint16_t len){
  if(data[0]==0x03){ // CONNECTION RESPONSE
    process_l2cap_connection_response(connection_handle, data);
  }else
  if(data[0]==0x05){ // CONFIGURATION RESPONSE
    process_l2cap_configuration_response(connection_handle, data);
  }else
  if(data[0]==0x04){ // CONFIGURATION REQUEST
    process_l2cap_configuration_request(connection_handle, data);
  }else
  if(data[0]==0xA1){ // HID 0xA1
    if(!wiimoteConnected){
      _set_led(connection_handle, 0b0001);
      wiimoteConnected = true;
    }
    process_extension_controller_reports(connection_handle, channel_id, data, len);
    process_report(data, len);

    // DEBUG
    bool wiimote_button_down  = (data[2] & 0x01) != 0;
    bool wiimote_button_up    = (data[2] & 0x02) != 0;
    bool wiimote_button_right = (data[2] & 0x04) != 0;
    bool wiimote_button_left  = (data[2] & 0x08) != 0;
    bool wiimote_button_plus  = (data[2] & 0x10) != 0;
    bool wiimote_button_2     = (data[3] & 0x01) != 0;
    bool wiimote_button_1     = (data[3] & 0x02) != 0;
    bool wiimote_button_B     = (data[3] & 0x04) != 0;
    bool wiimote_button_A     = (data[3] & 0x08) != 0;
    bool wiimote_button_minus = (data[3] & 0x10) != 0;
    bool wiimote_button_home  = (data[3] & 0x80) != 0;
    static bool rumble = false;
    if(wiimote_button_plus && !rumble){
      _set_rumble(connection_handle, true);
      rumble = true;
    }
    if(wiimote_button_minus && rumble){
      _set_rumble(connection_handle, false);
      rumble = false;
    }
  }else{
    log_d("!!! process_l2cap_data no impl !!!");
    log_d("  L2CAP len=%d data=%s", len, formatHex(data, len));
  }
}

static void process_acl_data(uint8_t* data, size_t len){
  if(!wiimoteConnected){
    log_d("**** ACL_DATA len=%d data=%s", len, formatHex(data, len));
  }

  uint16_t connection_handle    = ((data[1] & 0x0F) << 8) | data[0];
  uint8_t  packet_boundary_flag =  (data[1] & 0x30) >> 4; // Packet_Boundary_Flag
  uint8_t  broadcast_flag       =  (data[1] & 0xC0) >> 6; // Broadcast_Flag
  uint16_t acl_len              =  (data[3] << 8) | data[2];
  if(packet_boundary_flag != 0b10){
    log_d("!!! packet_boundary_flag = 0b%02B", packet_boundary_flag);
    return;
  }
  if(broadcast_flag != 0b00){
    log_d("!!! broadcast_flag = 0b%02B", broadcast_flag);
    return;
  }
  uint16_t l2cap_len            =  (data[5] << 8) | data[4];
  uint16_t channel_id           =  (data[7] << 8) | data[6];

  process_l2cap_data(connection_handle, channel_id, data + 8, l2cap_len);
}

static void process_hci_event(uint8_t event_code, uint8_t len, uint8_t* data){
  if(event_code != 0x02){ // suppress inquiry_result_event
    log_d("**** HCI_EVENT code=%02X len=%d data=%s", event_code, len, formatHex(data, len));
  }

  if(event_code == 0x0E){
    process_command_complete_event(len, data);
  }else if(event_code == 0x0F){
    process_command_status_event(len, data);
  }else if(event_code == 0x02){
    process_inquiry_result_event(len, data);
  }else if(event_code == 0x01){
    process_inquiry_complete_event(len, data);
  }else if(event_code == 0x07){
    process_remote_name_request_complete_event(len, data);
  }else if(event_code == 0x03){
    process_connection_complete_event(len, data);
  }else if(event_code == 0x05){
    process_disconnection_complete_event(len, data);
  }else if(event_code == 0x13){
    log_d("  (Number Of Completed Packets Event)");
  }else if(event_code == 0x0D){
    log_d("  (QoS Setup Complete Event)");
  }else{
    log_d("!!! process_hci_event no impl !!!");
  }
}

void Wiimote::init(){
  for(int i; i<4; i++){ wiimote_callback[i] = NULL; }

  _tx_queue = xQueueCreate(TX_QUEUE_SIZE, sizeof(lendata_t*));
  if (_tx_queue == NULL){
    log_e("xQueueCreate(_tx_queue) failed");
    return;
  }
  _rx_queue = xQueueCreate(RX_QUEUE_SIZE, sizeof(lendata_t*));
  if (_rx_queue == NULL){
    log_e("xQueueCreate(_rx_queue) failed");
    return;
  }

  if(!btStart()){
    log_e("btStart failed");
    return;
  }

  esp_err_t ret;
  ret = esp_vhci_host_register_callback(&callback);
  if (ret != ESP_OK) {
    log_e("esp_vhci_host_register_callback failed: %d %s", ret, esp_err_to_name(ret));
    return;
  }
}

void Wiimote::handle(){
  if(!btStarted()){
    return;
  }

  if(uxQueueMessagesWaiting(_tx_queue)){
    bool ok = esp_vhci_host_check_send_available();
    //log_d("esp_vhci_host_check_send_available=%d", ok);
    if(ok){
      lendata_t *lendata = NULL;
      if(xQueueReceive(_tx_queue, &lendata, 0) == pdTRUE){
        esp_vhci_host_send_packet(lendata->data, lendata->len);
        log_d("SEND => %s", formatHex(lendata->data, lendata->len));
        free(lendata);
      }
    }
  }

  if(uxQueueMessagesWaiting(_rx_queue)){
    lendata_t *lendata = NULL;
    if(xQueueReceive(_rx_queue, &lendata, 0) == pdTRUE){
      switch(lendata->data[0]){
      case 0x04:
        process_hci_event(lendata->data[1], lendata->data[2], lendata->data+3);
        break;
      case 0x02:
        process_acl_data(lendata->data+1, lendata->len-1);
        break;
      default:
        log_d("**** !!! Not HCI Event !!! ****");
        log_d("len=%d data=%s", lendata->len, formatHex(lendata->data, lendata->len));
      }
      free(lendata);
    }
  }
}

void Wiimote::register_callback(uint8_t number, wiimote_callback_t cb){
  if(number < 1 || 4 < number){
    return;
  }
  wiimote_callback[number-1] = cb;
}
