#include <Arduino.h>
#include <Wiimote.h>
#include <vector>

Wiimote wii;
std::vector<uint16_t> connections(0, 0);
bool is_scanning = false;
#define pair_button_gpio 21

void setup()
{
  Serial.begin(115200);
  pinMode(pair_button_gpio, INPUT_PULLUP);
  pinMode(LED_BUILTIN, OUTPUT);
  wii.init(wiimote_callback);
}

void loop()
{
  wii.handle();
  if (digitalRead(pair_button_gpio) == LOW && !is_scanning)
  {
    // disconnect all
    for (auto &wiimote : connections)
    {
      wii.disconnect(wiimote);
    }
    // scan for new
    wii.scan(true);
  }
}

void wiimote_callback(wiimote_event_type_t event_type, uint16_t wiimote, uint8_t *data, size_t len)
{
  printf("len:%02X ", len);

  if (wiimote != 0)
  {
    printf("Wiimote:%04X ", wiimote);
  }

  if (event_type == WIIMOTE_EVENT_DATA)
  {
    if (data[1] == 0x32)
    {
      printf("🪇 ");
      for (int i = 0; i < 4; i++)
      {
        printf("%02X ", data[i]);
      }
      // http://wiibrew.org/wiki/Wiimote/Extension_Controllers/Nunchuck
      uint8_t *ext = data + 4;
      printf(" ... Nunchuk: sx=%3d sy=%3d c=%d z=%d\n",
             ext[0],
             ext[1],
             0 == (ext[5] & 0x02),
             0 == (ext[5] & 0x01));
    }
    else if (data[1] == 0x34)
    {
      printf("👟 ");

      // balance boards must have LED set to 1
      wii.set_led(wiimote, 1);

      for (int i = 0; i < 4; i++)
      {
        printf("%02X ", data[i]);
      }
      // https://wiibrew.org/wiki/Wii_Balance_Board#Data_Format
      uint8_t *ext = data + 4;
      /*printf(" ... Wii Balance Board: TopRight=%d BottomRight=%d TopLeft=%d BottomLeft=%d Temperature=%d BatteryLevel=0x%02x\n",
        ext[0] * 256 + ext[1],
        ext[2] * 256 + ext[3],
        ext[4] * 256 + ext[5],
        ext[6] * 256 + ext[7],
        ext[8],
        ext[10]
      );*/

      float weight[4];
      wii.get_balance_weight(data, weight);

      printf(" ... Wii Balance Board: TopRight=%f BottomRight=%f TopLeft=%f BottomLeft=%f\n",
             weight[BALANCE_POSITION_TOP_RIGHT],
             weight[BALANCE_POSITION_BOTTOM_RIGHT],
             weight[BALANCE_POSITION_TOP_LEFT],
             weight[BALANCE_POSITION_BOTTOM_LEFT]);
    }
    else
    {
      printf("🪄 ");
      for (int i = 0; i < len; i++)
      {
        printf("%02X ", data[i]);
      }
      printf("\n");
    }

    bool wiimote_button_down = (data[2] & 0x01) != 0;
    bool wiimote_button_up = (data[2] & 0x02) != 0;
    bool wiimote_button_right = (data[2] & 0x04) != 0;
    bool wiimote_button_left = (data[2] & 0x08) != 0;
    bool wiimote_button_plus = (data[2] & 0x10) != 0;
    bool wiimote_button_2 = (data[3] & 0x01) != 0;
    bool wiimote_button_1 = (data[3] & 0x02) != 0;
    bool wiimote_button_B = (data[3] & 0x04) != 0;
    bool wiimote_button_A = (data[3] & 0x08) != 0;
    bool wiimote_button_minus = (data[3] & 0x10) != 0;
    bool wiimote_button_home = (data[3] & 0x80) != 0;
    static bool rumble = false;
    if (wiimote_button_plus && !rumble)
    {
      wii.set_rumble(wiimote, true);
      rumble = true;
    }
    if (wiimote_button_minus && rumble)
    {
      wii.set_rumble(wiimote, false);
      rumble = false;
    }
  }
  else if (event_type == WIIMOTE_EVENT_INITIALIZE)
  {
    printf("🛜 Bluetooth initialize. Accepting previously paired devices.\n");
  }
  else if (event_type == WIIMOTE_EVENT_SCAN_START)
  {
    printf("🫱 Scan started. Accepting new devices for pairing.\n", wiimote);
    is_scanning = true;
    digitalWrite(LED_BUILTIN, HIGH);
  }
  else if (event_type == WIIMOTE_EVENT_SCAN_STOP)
  {
    printf("✋ Scan stop\n");
    is_scanning = false;
    digitalWrite(LED_BUILTIN, LOW);
  }
  else if (event_type == WIIMOTE_EVENT_NEW)
  {
    printf("🤝 Authenticating Wiimote.\n");
    wii.initiate_auth(wiimote);
  }
  else if (event_type == WIIMOTE_EVENT_CONNECT)
  {
    connections.push_back(wiimote);
    wii.set_led(wiimote, 1 << connections.size());
    printf("✅ Connected Wiimote. Connections:%d\n", connections.size());
  }
  else if (event_type == WIIMOTE_EVENT_DISCONNECT)
  {
    // remove wiimote from connected list
    auto iterator = std::find(connections.begin(), connections.end(), wiimote);
    if (iterator != connections.end())
    {
      connections.erase(iterator);
    }
    printf("❌ Disconnected Wiimote. Connections:%d\n", connections.size());
  }
  else
  {
    printf("🤷🏻‍♂️ event_type=%d\n", event_type);
  }
}