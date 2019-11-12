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
  Serial.printf("wiimote handle=%04X len=%d ", handle, len);
  if(event_type == WIIMOTE_EVENT_DATA){
    if(data[1]==0x32){
      for (int i = 0; i < 4; i++) {
        Serial.printf("%02X ", data[i]);
      }

      // http://wiibrew.org/wiki/Wiimote/Extension_Controllers/Nunchuck
      uint8_t* ext = data+4;
      Serial.printf(" ... Nunchuk: sx=%3d sy=%3d c=%d z=%d\n",
        ext[0],
        ext[1],
        0==(ext[5]&0x02),
        0==(ext[5]&0x01)
      );
    }else{
      for (int i = 0; i < len; i++) {
        Serial.printf("%02X ", data[i]);
      }
      Serial.print("\n");
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
    Serial.print("  event_type=WIIMOTE_EVENT_INITIALIZE\n");
    wiimote.scan(true);
  }else if(event_type == WIIMOTE_EVENT_SCAN_START){
    Serial.print("  event_type=WIIMOTE_EVENT_SCAN_START\n");
  }else if(event_type == WIIMOTE_EVENT_SCAN_STOP){
    Serial.print("  event_type=WIIMOTE_EVENT_SCAN_STOP\n");
  }else if(event_type == WIIMOTE_EVENT_CONNECT){
    Serial.print("  event_type=WIIMOTE_EVENT_CONNECT\n");
    wiimote.set_led(handle, 1<<connection_count);
    connection_count++;
  }else if(event_type == WIIMOTE_EVENT_DISCONNECT){
    Serial.print("  event_type=WIIMOTE_EVENT_DISCONNECT\n");
    connection_count--;
    wiimote.scan(true);
  }else{
    Serial.printf("  event_type=%d\n", event_type);
  }
  Serial.flush();
  delay(100);
}
```

## See Also
- http://wiibrew.org/wiki/Wiimote
- http://www.yts.rdy.jp/pic/GB002/GB002.html
	- http://www.yts.rdy.jp/pic/GB002/hcip.html
	- http://www.yts.rdy.jp/pic/GB002/hcic.html
	- http://www.yts.rdy.jp/pic/GB002/l2cap.html
- https://qiita.com/jp-96/items/ff3822ab81f7696172c0
- https://www.wdic.org/w/WDIC/Bluetooth
	- https://www.wdic.org/w/WDIC/HCI%20%28Bluetooth%29
	- https://www.wdic.org/w/WDIC/L2CAP
