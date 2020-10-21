/*
  Simulate433TemperatureSensor

  This program requests the current temperature, for given coordinates, from
  OpenWeatherMap (App ID required, it's free!) and sends it with a 433MHz
  transmitter to a nearby (and cheap) weather station.

  Hardware used:
   - ESP32 from AZDelivery
   - XY-FST 433MHz transmitter
   - Jumper wires
   - (Technoline) WS 7014 weather station from 2008

  created September-October 2020
  by Kelvin Reichenbach
  https://github.com/KLVN

*/

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>

#define TRANSMIT_PIN 4  // Pin that is connected to DATA-Pin of Transmitter

// OOK Modulation "converts" each bit to a HIGH pulse for a certain amount of
// time, followed by a fixed LOW pulse (Gap). The original transmitter
// (La Crosse TX, Temperature only) is sending bits as followed:
//  0 -> |‾‾‾‾|__ where the HIGH pulse lasts for 1300us (microseconds)
//  1 -> |‾‾|__  where the HIGH pulse lasts for 550us (microseconds)
//  both HIGHs are followed by the same LOW that lasts for 920us

// Timings are a bit off and brute-forced (trial and error) to achieve a signal
// that is as close to the original one as possible
#define OOK_ZERO 1270
#define OOK_ONE 515
#define OOK_GAP 950

// Change these to your coordinates and app id
#define COORD_LAT "53.563886"
#define COORD_LNG "10.007173"
#define OPEN_WEATHER_MAP_APP_ID "XXXXXX"

// Define built-in LED for visual feedback while connecting to WiFi
int LED_BUILTIN = 2;

// Insert your WiFi credentials
const char* ssid = "XXXXXX";
const char* password = "XXXXXX";

// Initialize a mutex for critical sections
static portMUX_TYPE spinlock = portMUX_INITIALIZER_UNLOCKED;

// Function to connect to a given WiFi SSID with corresponding password
int connectWiFi(const char* ssid, const char* password) {
  // Always try to connect to WiFi
  while (WiFi.status() != WL_CONNECTED) {
    digitalWrite(LED_BUILTIN, HIGH);
    Serial.println("WiFi: Connecting...");
    WiFi.begin(ssid, password);
    delay(500);
    digitalWrite(LED_BUILTIN, LOW);
  }
  Serial.println("WiFi: Connected");
  return 1;
}

// Check for current WiFi status and reconnect if no connection is established
int checkWiFi() {
  if (WiFi.status() != WL_CONNECTED) {
    return connectWiFi(ssid, password);
  } else {
    return 1;
  }
}

// "Convert" a String of bits to an OOK package
void sendCode(String final_code) {
  // Variable for a more precise delay
  unsigned long time_now = 0;

  // Lock mutex during critical section
  portENTER_CRITICAL(&spinlock);

  // Iterate through every bit. Pull a pin HIGH and LOW for the specified
  // amounts of time
  for (int i = 0; i < final_code.length(); i++) {
    if (final_code[i] == '0') {
      // This is a much faster method than digitalWrite(PIN, HIGH);
      GPIO.out_w1ts = 0b10000;

      // This is a delay without blocking and is also used in other parts of
      // the code.
      time_now = micros();
      while (micros() - time_now < OOK_ZERO) {
      }

    } else if (final_code[i] == '1') {
      GPIO.out_w1ts = 0b10000;
      time_now = micros();
      while (micros() - time_now < OOK_ONE) {
      }
    }

    // After the HIGH pulses, set pin to LOW and hold it for the time of a gap.
    // This is equal for 0 (zeros) and 1 (ones)
    GPIO.out_w1tc = 0b10000;
    time_now = micros();
    while (micros() - time_now < OOK_GAP) {
    }
  }

  // Just in case something failed, set pin to LOW before leaving the function
  GPIO.out_w1tc = 0b10000;

  // Unlock mutex
  portEXIT_CRITICAL(&spinlock);
}

//////////////////////

void setup() {
  Serial.begin(115200);
  pinMode(LED_BUILTIN, OUTPUT);

  // Configure a pin bit mask
  gpio_config_t io_conf;
  io_conf.mode = GPIO_MODE_OUTPUT;

  // We will use pin 4 for the transmitter and therefore our bit mask looks like
  // this (right to left, start counting from 0)
  io_conf.pin_bit_mask = 0b10000;
  gpio_config(&io_conf);

  // Small delay before trying to connect to WiFi
  delay(3000);
  connectWiFi(ssid, password);
}

void loop() {
  // Variable for a more precise delay
  unsigned long time_now = 0;

  // Variables for temperature and offset. La Crosse TX has an offset of 50°C
  // that needs to be added to the real temperature
  float main_temp = 0;
  float temp = 0;
  float offset = 50;

  // Check WiFi, before requesting temperature from the internet
  if (checkWiFi()) {
    HTTPClient http;

    // GET JSON data from OpenWeatherMap. Change your coordinates and appid!
    http.begin(
        "http://api.openweathermap.org/data/2.5/"
        "weather?lat=" COORD_LAT "&lon=" COORD_LNG
        "&units=metric&appid=" OPEN_WEATHER_MAP_APP_ID);
    int httpCode = http.GET();

    if (httpCode > 0) {
      // I used "ArduinoJson Assistant" for this part:
      // https://arduinojson.org/v6/assistant/
      const size_t capacity = JSON_ARRAY_SIZE(1) + JSON_OBJECT_SIZE(1) +
                              2 * JSON_OBJECT_SIZE(2) + JSON_OBJECT_SIZE(4) +
                              JSON_OBJECT_SIZE(5) + JSON_OBJECT_SIZE(6) +
                              JSON_OBJECT_SIZE(13) + 280;
      DynamicJsonDocument doc(capacity);

      String json = http.getString();
      deserializeJson(doc, json);

      JsonObject main = doc["main"];
      main_temp = main["temp"];

      // Print current temperature that was fetched from OpenWeatherMap
      Serial.println(main_temp);

      // Free the resources
      http.end();

      // Turn off WiFi for less interruptions
      WiFi.mode(WIFI_OFF);

      // Add offset to current temperature
      temp = main_temp + offset;

      // times 10 and add 0.5 for rounding
      temp = temp * 10 + 0.5;

      // cast to int to delete decimal point
      int int_temp = temp;

      // fill up to 3 digits with leading zeros and save each digits in array
      char data[4];
      sprintf(data, "%03d", int_temp);

      // Array with all binary codes
      int bin_code[11] = {
          [0] = 0b0000,         // Start sequence
          [1] = 0b1010,         // Start sequence
          [2] = 0b0000,         // TYP
          [3] = 0b1100,         // ADDR
          [4] = 0b1010,         // ADDR
          [5] = data[0] - '0',  // temperature, digit one
          [6] = data[1] - '0',  // temperature, digit two
          [7] = data[2] - '0',  // temperature, digit three
          [8] = data[0] - '0',  // temperature, digit one
          [9] = data[1] - '0',  // temperature, digit two
          [10] = 0b0000,  // CRC (sum of all elements before -> in binary ->
                          // take only last 4 bits)
      };

      // Calculate CRC
      int crc = 0b0000;
      for (int i = 0; i <= 10; i++) {
        crc += (int)bin_code[i];
      }
      // Add CRC to array
      bin_code[10] = crc;

      // Write final binary code as String
      String final_code = "";
      for (int i = 0; i <= 10; i++) {
        for (int j = 3; j >= 0; j--) {
          final_code += bitRead(bin_code[i], j);
        }
      }

      // Transmit binary code in bursts of 4 with a delay of 26ms between them
      // and repeat this for 40 times with a delay of 8 seconds between each
      // iteration. Visual representation:
      // +.+.+.--------+.+.+.--------+.+.+.-------- and so on
      // (+ = transmit code; . = delay 26ms; - = delay 8 seconds)

      // Transmitting one burst will take about ~83ms. So 4 bursts with delay
      // take 436ms and 40 iterations with delay will take 5.6 minutes. So about
      // every 5.6 minutes loop() will run again, get the current temperature
      // and transmit it to the weather station.
      for (int i = 0; i < 40; i++) {
        for (int j = 0; j < 4; j++) {
          sendCode(final_code);
          time_now = millis();
          while (millis() - time_now < 26) {
          }
        }

        // Just for debugging
        Serial.println(final_code);
        Serial.print("Sent iteration ");
        Serial.println(i);

        // Wait 8 seconds
        time_now = millis();
        while (millis() - time_now < 8000) {
        }
      }

    } else {
      // Abort on HTTP error
      Serial.println("Error on HTTP request");
      http.end();  // Free the resources
    }
  }

  // Wait for 10 seconds after each cycle
  delay(10000);
}
