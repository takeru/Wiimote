# Wiimote Bluetooth Connection Library for Arduino core for ESP32

## Example

```WiimoteDemo.ino.cpp
#include <Wiimote.h>

Wiimote wiimote;

void setup() {
  Serial.begin(115200);
  wiimote.init(wiimote_callback);
}

void loop() {
  wiimote.handle();
}

void wiimote_callback(wiimote_event_type_t event_type, uint16_t handle, uint8_t *data, size_t len) {
  static int connection_count = 0;
  printf("wiimote handle=%04X len=%d ", handle, len);
  if(event_type == WIIMOTE_EVENT_DATA){
    if(data[1]==0x32){
      for (int i = 0; i < 4; i++) {
        printf("%02X ", data[i]);
      }
      // http://wiibrew.org/wiki/Wiimote/Extension_Controllers/Nunchuck
      uint8_t* ext = data+4;
      printf(" ... Nunchuk: sx=%3d sy=%3d c=%d z=%d\n",
        ext[0],
        ext[1],
        0==(ext[5]&0x02),
        0==(ext[5]&0x01)
      );
    }else if(data[1]==0x34){
      for (int i = 0; i < 4; i++) {
        printf("%02X ", data[i]);
      }
      // https://wiibrew.org/wiki/Wii_Balance_Board#Data_Format
      uint8_t* ext = data+4;
      /*printf(" ... Wii Balance Board: TopRight=%d BottomRight=%d TopLeft=%d BottomLeft=%d Temperature=%d BatteryLevel=0x%02x\n",
        ext[0] * 256 + ext[1],
        ext[2] * 256 + ext[3],
        ext[4] * 256 + ext[5],
        ext[6] * 256 + ext[7],
        ext[8],
        ext[10]
      );*/
      
      float weight[4];
      wiimote.get_balance_weight(data, weight);

      printf(" ... Wii Balance Board: TopRight=%f BottomRight=%f TopLeft=%f BottomLeft=%f\n",
        weight[BALANCE_POSITION_TOP_RIGHT],
        weight[BALANCE_POSITION_BOTTOM_RIGHT],
        weight[BALANCE_POSITION_TOP_LEFT],
        weight[BALANCE_POSITION_BOTTOM_LEFT]
      );  
    }else{
      for (int i = 0; i < len; i++) {
        printf("%02X ", data[i]);
      }
      printf("\n");
    }

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
      wiimote.set_rumble(handle, true);
      rumble = true;
    }
    if(wiimote_button_minus && rumble){
      wiimote.set_rumble(handle, false);
      rumble = false;
    }
  }else if(event_type == WIIMOTE_EVENT_INITIALIZE){
    printf("  event_type=WIIMOTE_EVENT_INITIALIZE\n");
    wiimote.scan(true);
  }else if(event_type == WIIMOTE_EVENT_SCAN_START){
    printf("  event_type=WIIMOTE_EVENT_SCAN_START\n");
  }else if(event_type == WIIMOTE_EVENT_SCAN_STOP){
    printf("  event_type=WIIMOTE_EVENT_SCAN_STOP\n");
    if(connection_count==0){
      wiimote.scan(true);
    }
  }else if(event_type == WIIMOTE_EVENT_CONNECT){
    printf("  event_type=WIIMOTE_EVENT_CONNECT\n");
    wiimote.set_led(handle, 1<<connection_count);
    connection_count++;
  }else if(event_type == WIIMOTE_EVENT_DISCONNECT){
    printf("  event_type=WIIMOTE_EVENT_DISCONNECT\n");
    connection_count--;
    wiimote.scan(true);
  }else{
    printf("  event_type=%d\n", event_type);
  }
  delay(100);
}
```

## See Also
- https://wiibrew.org/wiki/Wiimote
  - https://wiibrew.org/wiki/Wiimote/Extension_Controllers
  - https://wiibrew.org/wiki/Wii_Balance_Board
- http://www.yts.rdy.jp/pic/GB002/GB002.html
	- http://www.yts.rdy.jp/pic/GB002/hcip.html
	- http://www.yts.rdy.jp/pic/GB002/hcic.html
	- http://www.yts.rdy.jp/pic/GB002/l2cap.html
- https://qiita.com/jp-96/items/ff3822ab81f7696172c0
- https://www.wdic.org/w/WDIC/Bluetooth
	- https://www.wdic.org/w/WDIC/HCI%20%28Bluetooth%29
	- https://www.wdic.org/w/WDIC/L2CAP
