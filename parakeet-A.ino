#define NEW_PCB
#define NEW_PCB_V3
//#define DEBUG
#define GSM_MODEM
#define ARDUINO_SLEEP
//#define ARDUINO_DELAY
//#define INT_BLINK_LED
#define EXT_BLINK_LED
#define MODEM_SLEEP_DTR
#define CC2500_LEN_CONTROL
//#define NO_NEXT_TIME // Не пытаемся угадать время следующего пакета

#include <SPI.h>
#include <SoftwareSerial.h>
#include <EEPROM.h>
#include "cc2500_REG.h"

#ifdef ARDUINO_SLEEP
#include <avr/sleep.h>     //AVR MCU power management.
#include <avr/power.h>     //AVR MCU peripheries (Analog comparator, ADC, USI, Timers/Counters etc) management.
#include <avr/wdt.h>       //AVR MCU watchdog timer management.
#include <avr/io.h>        //AVR MCU IO ports management.
#include <avr/interrupt.h> //AVR MCU interrupt flags management.
#endif

#ifdef NEW_PCB
#ifdef NEW_PCB_V3
  #define GDO0_PIN A2           // Цифровой канал, к которму подключен контакт GD0 платы CC2500
#else
  #define GDO0_PIN 9            // Цифровой канал, к которму подключен контакт GD0 платы CC2500
#endif
  #define DTR_PIN  8            // Цифровой канал, к которму подключен контакт DTR платы GSM-модема
  #define TX_PIN   7            // Tx контакт для последовательного порта
  #define RX_PIN   6            // Rx контакт для последовательного порта
  #define RED_LED_PIN 4
  #define YELLOW_LED_PIN 5
#else
  #define GDO0_PIN 3            // Цифровой канал, к которму подключен контакт GD0 платы CC2500
  #define DTR_PIN  2            // Цифровой канал, к которму подключен контакт DTR платы GSM-модема
  #define TX_PIN   8            // Tx контакт для последовательного порта
  #define RX_PIN   9            // Rx контакт для последовательного порта

  #ifdef EXT_BLINK_LED
    #define RED_LED_PIN 5
    #define YELLOW_LED_PIN 4
  #endif
#endif
#define LEN_PIN    A1            // Цифровой канал, к которму подключен контакт LEN (усилитель слабого сигнала) платы CC2500


#define NUM_CHANNELS (4)      // Кол-во проверяемых каналов
#define FIVE_MINUTE 300000    // 5 минут

#ifdef DEBUG
#define SERIAL_BUUFER_LEN 110 // Размер буфера для приема данных от GSM модема
#else
#define SERIAL_BUUFER_LEN 200 // Размер буфера для приема данных от GSM модема
#endif
#define GSM_BUUFER_LEN 200 // Размер буфера для приема данных от GSM модема

#define GSM_DELAY 200         // Задержка между командами модема

#define my_webservice_url  "http://parakeet.esen.ru/receiver.cgi"
#define my_webservice_reply     "!ACK"
#define my_user_agent     "parakeet_A"
#define my_gprs_apn   "internet.mts.ru"
//#define my_gprs_apn   "internet.beeline.ru"
//#define my_gprs_apn   "vmi.velcom.by"
#define my_password_code  "12354"
#define WDTO_2S_MS 2371 // Время ожидания строжевого пса "примерно 2 секунды"
#define ERROR_STR "ERROR" // Сообщение об ошибке при работе с модемом

SoftwareSerial mySerial(RX_PIN, TX_PIN); // RX, TX

unsigned long dex_tx_id;
char transmitter_id[] = "ABCDE";
//char transmitter_id[] = "6518Y";
//char transmitter_id[] = "69NL1";
//char transmitter_id[] = "619MY";
//char transmitter_id[] = "62SAL";
//char transmitter_id[] = "607K5";

unsigned long packet_received = 0;

//byte fOffset[NUM_CHANNELS] = { 0x00, 0xD5, 0xE6, 0xE5 };
//byte defaultfOffset[NUM_CHANNELS] = { 0x00, 0xD5, 0xE6, 0xE5 };
//byte fOffset[NUM_CHANNELS] = { 0xCE, 0xD5, 0xE6, 0xE5 };
byte fOffset[NUM_CHANNELS] = { 0xE4, 0xE3, 0xE2, 0xE2 };
//byte defaultfOffset[NUM_CHANNELS] = { 0xCE, 0xD5, 0xE6, 0xE5 };
byte nChannels[NUM_CHANNELS] = { 0, 100, 199, 209 };
//unsigned long waitTimes[NUM_CHANNELS] = { 13500, 500, 500, 500 };
//unsigned long waitTimes[NUM_CHANNELS] = { 0, 500, 500, 500 };
unsigned long waitTimes[NUM_CHANNELS] = { 0, 600, 600, 600 };
//unsigned long catch_offsets[NUM_CHANNELS] = { 0, 0, 0, 0 };

byte sequential_missed_packets = 0;
byte wait_after_time = 100;
unsigned long next_time = 0; // Время ожидания следующего пакета на канале 0
unsigned long catch_time = 0; // Время последнего пойманного пакета (приведенное к пакету на канале 0)

byte misses_until_failure = 2;                                                   //
// after how many missed packets should we just start a nonstop scan?                               //
// a high value is better for conserving batter life if you go out of wixel range a lot             //
// but it could also mean missing packets for MUCH longer periods of time                           //
// a value of zero is best if you dont care at all about battery life                               //

boolean gsm_availible = false; // Доступность связи GSM
boolean modem_availible = false; // Доступность модема на порту
boolean internet_availible = false; // Флаг подключения мобильного интернета
char SerialBuffer[SERIAL_BUUFER_LEN] ; // Буффер для чтения данных их последовательного порта
char gsm_cmd[GSM_BUUFER_LEN]; // Буффер для формирования GSM команд
boolean low_battery = false;
byte gsm_error_count = 0;

#ifdef ARDUINO_SLEEP
volatile long watchdog_counter;
int wdto_2s_ms = WDTO_2S_MS;
#endif

// Коды ошибок мигают лампочкой в двоичной системе
// 1 (0001) - Неверный CRC в сохраненных настройках. Берем настройки по умолчанию
// 2 (0010) - Не подключен модем.
// 3 (0011) - Нет мобильной связи
// 4 (0100) - Нет мобильного интернета
// 

typedef struct _Dexcom_packet
{
  byte len;
  unsigned long dest_addr;
  unsigned long src_addr;
  byte port;
  byte device_info;
  byte txId;
  unsigned int raw;
  unsigned int filtered;
  byte battery;
  byte unknown;
  byte checksum;
  byte RSSI;
  byte LQI2;
} Dexcom_packet;

Dexcom_packet Pkt;

typedef struct _parakeet_settings
{
  unsigned long dex_tx_id;     //4 bytes
  char http_url[56];
  char gsm_apn[32];
  char password_code[6];
  unsigned long checksum; // needs to be aligned

} parakeet_settings;

parakeet_settings settings;

char SrcNameTable[32] = { '0', '1', '2', '3', '4', '5', '6', '7',
                          '8', '9', 'A', 'B', 'C', 'D', 'E', 'F',
                          'G', 'H', 'J', 'K', 'L', 'M', 'N', 'P',
                          'Q', 'R', 'S', 'T', 'U', 'W', 'X', 'Y'
                        };


void dexcom_src_to_ascii(unsigned long src, char addr[6]) {
  addr[0] = SrcNameTable[(src >> 20) & 0x1F];
  addr[1] = SrcNameTable[(src >> 15) & 0x1F];
  addr[2] = SrcNameTable[(src >> 10) & 0x1F];
  addr[3] = SrcNameTable[(src >> 5) & 0x1F];
  addr[4] = SrcNameTable[(src >> 0) & 0x1F];
  addr[5] = 0;
}


unsigned long getSrcValue(char srcVal) {
  byte i = 0;
  for (i = 0; i < 32; i++) {
    if (SrcNameTable[i] == srcVal) break;
  }
  return i & 0xFF;
}

unsigned long asciiToDexcomSrc(char addr[6]) {
  unsigned long src = 0;
  src |= (getSrcValue(addr[0]) << 20);
  src |= (getSrcValue(addr[1]) << 15);
  src |= (getSrcValue(addr[2]) << 10);
  src |= (getSrcValue(addr[3]) << 5);
  src |= getSrcValue(addr[4]);
  return src;
}

byte bit_reverse_byte (byte in)
{
    byte bRet = 0;
    if (in & 0x01)
        bRet |= 0x80;
    if (in & 0x02)
        bRet |= 0x40;
    if (in & 0x04)
        bRet |= 0x20;
    if (in & 0x08)
        bRet |= 0x10;
    if (in & 0x10)
        bRet |= 0x08;
    if (in & 0x20)
        bRet |= 0x04;
    if (in & 0x40)
        bRet |= 0x02;
    if (in & 0x80)
        bRet |= 0x01;
    return bRet;
}

void bit_reverse_bytes (byte * buf, byte nLen)
{
    byte i = 0;
    for (; i < nLen; i++)
    {
        buf[i] = bit_reverse_byte (buf[i]);
    }
}

unsigned long dex_num_decoder (unsigned int usShortFloat)
{
    unsigned int usReversed = usShortFloat;
    byte usExponent = 0;
    unsigned long usMantissa = 0;
    bit_reverse_bytes ((byte *) & usReversed, 2);
    usExponent = ((usReversed & 0xE000) >> 13);
    usMantissa = (usReversed & 0x1FFF);
    return usMantissa << usExponent;
}

#ifdef EXT_BLINK_LED
void blink_sequence_red(const char *sequence) {
  byte i;

  digitalWrite(YELLOW_LED_PIN, HIGH);
  digitalWrite(RED_LED_PIN, LOW);
  delay(500); 
  for (i = 0; i < strlen(sequence); i++) {
    digitalWrite(RED_LED_PIN, HIGH);
    switch (sequence[i]) {
      case '0': 
        delay(500);
        break;
      case '1': 
        delay(1000);
        break;
      default:
        delay(2000);
        break;
    }
    digitalWrite(RED_LED_PIN, LOW);
    delay(500); 
  }  
  digitalWrite(YELLOW_LED_PIN, LOW);
}
#endif

#ifdef INT_BLINK_LED
void blink_sequence(const char *sequence) {
  byte i;

  digitalWrite(LED_BUILTIN, LOW);
  delay(500); 
  for (i = 0; i < strlen(sequence); i++) {
    digitalWrite(LED_BUILTIN, HIGH);
    switch (sequence[i]) {
      case '0': 
        delay(500);
        break;
      case '1': 
        delay(1000);
        break;
      default:
        delay(2000);
        break;
    }
    digitalWrite(LED_BUILTIN, LOW);
    delay(500); 
  }  
}
#endif

void clearSettings()
{
  memset (&settings, 0, sizeof (settings));
  settings.dex_tx_id = asciiToDexcomSrc (transmitter_id);
  dex_tx_id = settings.dex_tx_id;
  sprintf(settings.http_url, my_webservice_url);
  sprintf(settings.gsm_apn, my_gprs_apn);
  sprintf(settings.password_code, my_password_code);
}

unsigned long checksum_settings()
{
  char* flash_pointer;
  unsigned long chk = 0x12345678;
  byte i;
  //   flash_pointer = (char*)settings;
  flash_pointer = (char*)&settings;
  for (i = 0; i < sizeof(parakeet_settings) - 4; i++)
  {
    chk += (flash_pointer[i] * (i + 1));
    chk++;
  }
  return chk;
}

void saveSettingsToFlash()
{
  settings.checksum = checksum_settings();
  EEPROM.put(0, settings);
}

void loadSettingsFromFlash()
{
  EEPROM.get(0, settings);
  dex_tx_id = settings.dex_tx_id;
  if (settings.checksum != checksum_settings()) {
#ifdef DEBUG
    Serial.println("Settings checksum error. Load defaults");
#endif
    clearSettings();
#ifdef INT_BLINK_LED
    blink_sequence("0001");
#endif
#ifdef EXT_BLINK_LED
    blink_sequence_red("0001");
#endif
  }
#ifdef DEBUG
  Serial.print("Dexcom ID: ");
  Serial.println(settings.dex_tx_id);
#endif
}

#ifdef INT_BLINK_LED
void blink_builtin_led_quarter() {  // Blink quarter seconds
  if ((millis() / 250) % 2) {
    digitalWrite(LED_BUILTIN, HIGH);
  } else
  {
    digitalWrite(LED_BUILTIN, LOW);
  }
}

void blink_builtin_led_half() {  // Blink half seconds
  if ((millis() / 500) % 2) {
    digitalWrite(LED_BUILTIN, HIGH);
  } else
  {
    digitalWrite(LED_BUILTIN, LOW);
  }
}
#endif

#ifdef EXT_BLINK_LED
void blink_yellow_led_quarter() {  // Blink quarter seconds
  if ((millis() / 250) % 2) {
    digitalWrite(YELLOW_LED_PIN, HIGH);
  } else
  {
    digitalWrite(YELLOW_LED_PIN, LOW);
  }
}

void blink_yellow_led_half() {  // Blink half seconds
  if ((millis() / 500) % 2) {
    digitalWrite(YELLOW_LED_PIN, HIGH);
  } else
  {
    digitalWrite(YELLOW_LED_PIN, LOW);
  }
}

void blink_red_led_quarter() {  // Blink quarter seconds
  if ((millis() / 250) % 2) {
    digitalWrite(RED_LED_PIN, HIGH);
  } else
  {
    digitalWrite(RED_LED_PIN, LOW);
  }
}

void blink_red_led_quarter2() {  // Blink quarter seconds
  if ((millis() / 250) % 2) {
    digitalWrite(RED_LED_PIN, LOW);
  } else
  {
    digitalWrite(RED_LED_PIN, HIGH);
  }
}

void blink_red_led_half() {  // Blink half seconds
  if ((millis() / 500) % 2) {
    digitalWrite(RED_LED_PIN, HIGH);
  } else
  {
    digitalWrite(RED_LED_PIN, LOW);
  }
}

void blink_red_led_half2() {  // Blink half seconds
  if ((millis() / 500) % 2) {
    digitalWrite(RED_LED_PIN, LOW);
  } else
  {
    digitalWrite(RED_LED_PIN, HIGH);
  }
}
#endif

void WriteReg(char addr, char value) {
  digitalWrite(SS, LOW);
  while (digitalRead(MISO) == HIGH) {
  };
  SPI.transfer(addr);
  SPI.transfer(value);
  digitalWrite(SS, HIGH);
  //  delay(10);
}

char SendStrobe(char strobe)
{
  digitalWrite(SS, LOW);

  while (digitalRead(MISO) == HIGH) {
  };

  char result =  SPI.transfer(strobe);
  digitalWrite(SS, HIGH);
  //  delay(10);
  return result;
}

void init_CC2500() {
//FSCTRL1 and MDMCFG4 have the biggest impact on sensitivity...
   
   WriteReg(PATABLE, 0x00);
//   WriteReg(IOCFG0, 0x01);
   WriteReg(IOCFG0, 0x06);
   WriteReg(PKTLEN, 0xff);
   WriteReg(PKTCTRL1, 0x0C); // CRC_AUTOFLUSH = 1 & APPEND_STATUS = 1
//   WriteReg(PKTCTRL1, 0x04);
   WriteReg(PKTCTRL0, 0x05);
   WriteReg(ADDR, 0x00);
   WriteReg(CHANNR, 0x00);

   WriteReg(FSCTRL1, 0x0f); 
   WriteReg(FSCTRL0, 0x00);  
  
   WriteReg(FREQ2, 0x5d);
   WriteReg(FREQ1, 0x44);
   WriteReg(FREQ0, 0xeb);
   
   WriteReg(FREND1, 0xb6);  
   WriteReg(FREND0, 0x10);  

   // Bandwidth
   //0x4a = 406 khz
   //0x5a = 325 khz
   // 300 khz is supposedly what dex uses...
   //0x6a = 271 khz
   //0x7a = 232 khz
   WriteReg(MDMCFG4, 0x7a); //appear to get better sensitivity
   WriteReg(MDMCFG3, 0xf8);
   WriteReg(MDMCFG2, 0x73);
   WriteReg(MDMCFG1, 0x23);
   WriteReg(MDMCFG0, 0x3b);
   
   WriteReg(DEVIATN, 0x40);

   WriteReg(MCSM2, 0x07);
   WriteReg(MCSM1, 0x30);
   WriteReg(MCSM0, 0x18);  
   WriteReg(FOCCFG, 0x16); //36
   WriteReg(FSCAL3, 0xa9);
   WriteReg(FSCAL2, 0x0a);
   WriteReg(FSCAL1, 0x00);
   WriteReg(FSCAL0, 0x11);
  
   WriteReg(AGCCTRL2, 0x03);  
   WriteReg(AGCCTRL1, 0x00);
   WriteReg(AGCCTRL0, 0x91);
   //
   WriteReg(TEST2, 0x81);
   WriteReg(TEST1, 0x35); 
   WriteReg(TEST0, 0x0b);  
   
   WriteReg(FOCCFG, 0x0A);    // allow range of +/1 FChan/4 = 375000/4 = 93750.  No CS GATE
   WriteReg(BSCFG, 0x6C);
 
}

char ReadReg(char addr) {
  addr = addr + 0x80;
  digitalWrite(SS, LOW);
  while (digitalRead(MISO) == HIGH) {
  };
  SPI.transfer(addr);
  char y = SPI.transfer(0);
  digitalWrite(SS, HIGH);
  //  delay(10);
  return y;
}

char ReadStatus(char addr) {
  addr = addr + 0xC0;
  digitalWrite(SS, LOW);
  while (digitalRead(MISO) == HIGH) {
  };
  SPI.transfer(addr);
  char y = SPI.transfer(0);
  digitalWrite(SS, HIGH);
  //  delay(10);
  return y;
}

void(* resetFunc) (void) = 0; // объявляем функцию reset 

#ifdef GSM_MODEM
boolean gsm_command(const char *command, const char *response, int timeout, boolean break_on_error) {
  boolean ret;
  unsigned long timeout_time; 
  int len = strlen (response);
  int len_error = strlen(ERROR_STR);
  int loop = 0;

#ifdef INT_BLINK_LED  
  digitalWrite(LED_BUILTIN, HIGH);
#endif
#ifdef EXT_BLINK_LED  
  digitalWrite(RED_LED_PIN, HIGH);
#endif

  if (len == 0) {
    ret = true;
  } 
  else {
    ret = false;
  }  
//  memset (&SerialBuffer,0,sizeof(SerialBuffer));
  memset (&SerialBuffer[0],0,sizeof(SerialBuffer));
//  memset (&settings, 0, sizeof (settings));
//  mySerial.write(command);
//  mySerial.write("\r\n"); // enter key
  mySerial.println(command);
  timeout_time = timeout;
  timeout_time = millis() + (timeout_time * 1000);
  while (millis() < timeout_time)
  {
    if (mySerial.available()) {
      delayMicroseconds(100);
      SerialBuffer[loop] = mySerial.read();
      loop++;
      if (loop == SERIAL_BUUFER_LEN) loop = 0; // Контролируем переполнение буфера
      if (loop > len) {
        if (strncmp(response,&SerialBuffer[loop-len],len) == 0) {
          ret = true;
          delay(100);
        }
      }  
      if (break_on_error && loop >= len_error && strncmp(ERROR_STR,&SerialBuffer[loop-len_error],len_error) == 0) {
#ifdef DEBUG
        Serial.println("ERROR");
#endif
        delay(GSM_DELAY);
        break;
      }
    } 
    else {
      if (ret) {
        delayMicroseconds(100);
        break;
      }
    }
  }
  SerialBuffer[loop] = '\0';
  while (mySerial.available()) {
    delayMicroseconds(100);
    mySerial.read();
  }
#ifdef DEBUG
  Serial.print("Cmd=");
  Serial.println(command);
  Serial.print("Exp.rep=");
  Serial.println(response);
  Serial.print("Resp=");
  Serial.println(SerialBuffer);
  Serial.print("Res=");
  Serial.println(ret);
#endif
#ifdef INT_BLINK_LED  
  digitalWrite(LED_BUILTIN, LOW);
#endif
#ifdef EXT_BLINK_LED  
  digitalWrite(RED_LED_PIN, LOW);
#endif
  return ret;
}

boolean gsm_command(const char *command, const char *response, int timeout) {
  return gsm_command(command,response,timeout,true);
}

boolean set_gprs_profile() {
  boolean ret;
  
  delay(GSM_DELAY);
  gsm_command("AT+SAPBR=0,1", "OK", 10); // Сбросим настроенный GPRS профиль
  delay(GSM_DELAY);
  ret = gsm_command("AT+SAPBR=3,1,\"Contype\",\"GPRS\"", "OK", 2); // Настраиваем мобильный интернет 2G
  if (ret) {
    sprintf(gsm_cmd,"AT+SAPBR=3,1,\"APN\",\"%s\"",settings.gsm_apn);   // Точка доступа
    delay(GSM_DELAY);
    ret = gsm_command(gsm_cmd,"OK",2);
    if (ret) {
      delay(GSM_DELAY);
      ret = gsm_command("AT+SAPBR=1,1", "OK", 90); // Применяем настройки
    }
  }
  return ret;    
}

void set_settings(char *settings_str,char *data, byte idx,byte max_len) {
  byte i1 = idx;  
  byte i2 = 0;

  while (data[i1] == ' ') {
    i1++;
    if (i1 == SERIAL_BUUFER_LEN) return;
  }
  while (isPrintable(data[i1]) and data[i1] != ' ') {
    settings_str[i2] = data[i1];
    i1++;
    if (i1 == SERIAL_BUUFER_LEN) break;
    i2++;
    if (i2 == max_len-1) break;
  }
  settings_str[i2] = '\0';
}

void send_sms(char *phone, char *cmd, char *data) {
#ifdef DEBUG
  Serial.print("SMS to number = ");
  Serial.println(phone);
  Serial.print(cmd);
  Serial.println(data);
#endif
  mySerial.print("AT+CMGS=");
  delay(GSM_DELAY);
  if (gsm_command(phone,">",2)) {
    delay(GSM_DELAY);
    mySerial.print(cmd);
    delay(GSM_DELAY);
    mySerial.print(data);
    delay(GSM_DELAY);
    gsm_command("\x1A","OK",20);
  }
}

void extract_phone_number(char *phone, char *data, byte idx) {
  byte i1 = idx;
  byte i2 = 0;

  while (i1 > 0) {
    if (data[i1] == '"' && data[i1+1] == '+') break;
    i1--;
  }
  if (i1 == 0) return;
  while (data[i1] != ',') {
    phone[i2] = data[i1];
    i1++;
    i2++;
    if (i2 == 14) break;
  }
  phone[i2] = '\0';
}

void read_sms() {
  boolean ret;
  byte i;
  boolean reboot = false;
  boolean new_apn = false;
  char phone_number[15];
  char ascii_trans_id[6];

  delay(GSM_DELAY);
  gsm_command("AT+CMGL=\"REC UNREAD\"" ,"OK",5); // Читаем все новые смс-ки в буфер
  memset(&gsm_cmd,0,sizeof(gsm_cmd));
  strcpy(gsm_cmd,SerialBuffer);
  for (i = 0; i < SERIAL_BUUFER_LEN - 4; i++) {
    if (strncmp("APN ",&gsm_cmd[i],4) == 0) {
      set_settings(settings.gsm_apn,gsm_cmd,i+4,32);
      saveSettingsToFlash();
      extract_phone_number(phone_number,gsm_cmd,i);
      send_sms(phone_number,"APN:",settings.gsm_apn);
      new_apn = true;
    }
    if (strncmp("DEFAULTS",&gsm_cmd[i],8) == 0) {
      clearSettings();
      saveSettingsToFlash();
      extract_phone_number(phone_number,gsm_cmd,i);
      send_sms(phone_number,"DEFAULTS:","OK");
      new_apn = true;
    }
    if (strncmp("TRANSMIT ",&SerialBuffer[i],9) == 0) {
      set_settings(transmitter_id,gsm_cmd,i+9,6);
      settings.dex_tx_id = asciiToDexcomSrc (transmitter_id);
      dex_tx_id = settings.dex_tx_id;
      saveSettingsToFlash();
      extract_phone_number(phone_number,gsm_cmd,i);
      send_sms(phone_number,"TRANSMIT:",transmitter_id);
    }
    if (strncmp("HTTP ",&gsm_cmd[i],5) == 0) {
      set_settings(settings.http_url,gsm_cmd,i+5,56);
      saveSettingsToFlash();
      extract_phone_number(phone_number,gsm_cmd,i);
      send_sms(phone_number,"HTTP:",settings.http_url);
    }
    if (strncmp("PWD ",&gsm_cmd[i],4) == 0) {
      set_settings(settings.password_code,gsm_cmd,i+4,6);
      saveSettingsToFlash();
      extract_phone_number(phone_number,gsm_cmd,i);
      send_sms(phone_number,"PWD:",settings.password_code);
    }
    if (strncmp("REBOOT",&gsm_cmd[i],6) == 0) {
      reboot = true;
      extract_phone_number(phone_number,gsm_cmd,i);
      send_sms(phone_number,"REBOOT:","OK");
    }
    if (strncmp("SETTINGS",&gsm_cmd[i],8) == 0) {
      extract_phone_number(phone_number,gsm_cmd,i);
      dexcom_src_to_ascii(dex_tx_id,ascii_trans_id);
      send_sms(phone_number,"TRANSMIT:",ascii_trans_id);
      send_sms(phone_number,"APN:",settings.gsm_apn);
      send_sms(phone_number,"HTTP:",settings.http_url);
      send_sms(phone_number,"PWD:",settings.password_code);     
    }
  }
  delay(GSM_DELAY);
  gsm_command("AT+CMGDA=\"DEL READ\"","OK",5); // Удалить прочитанные смс-ки
  delay(GSM_DELAY);
  gsm_command("AT+CMGDA=\"DEL SENT\"","OK",5); // Удалить отправленные смс-ки
  if (new_apn) set_gprs_profile();      
  if (reboot) resetFunc();
}


void gsm_wake_up() {
#ifdef MODEM_SLEEP_DTR  
  digitalWrite(DTR_PIN, LOW); // Будим GSM-модем
#else  
  mySerial.write(27);
  delay(GSM_DELAY); 
  gsm_command("AT+CSCLK=0", "OK", 2); // Отключаем на модеме режим сна
#endif  
  delay(GSM_DELAY); 
   // Включаем мигание модема
  if (!gsm_command("AT+CNETLIGHT=1", "OK", 2))
  {
    modem_availible = false;
    init_gsm_modem();
    if (modem_availible) {
      gsm_command("AT+CNETLIGHT=1", "OK", 2);
    }    
  };
}

void gsm_goto_sleep() {
//  gsm_command("AT+CSCLK=1", "OK", 2); // Переводим модем в режим сна в режиме управления сигналом DTR
  gsm_command("AT+CNETLIGHT=0", "OK", 2); // Отключаем мигание модема
  delay(GSM_DELAY);
#ifdef MODEM_SLEEP_DTR  
  digitalWrite(DTR_PIN, HIGH);
#else  
  gsm_command("AT+CSCLK=2", "OK", 2); // Переводим модем в режим сна 2
  delay(GSM_DELAY);
#endif  
}

void init_base_gsm()
{
//  gsm_command("AT+IPR=9600","OK",2); // Установить скорость порта 9600
//  gsm_command("AT+IFC=0,0","OK",2); 
  gsm_command("ATZ","OK",10); // Установить параметры по умолчанию 
  gsm_command("ATE0","OK", 2); // Выключить эхо 
  gsm_command("AT+CFUN=0", "OK",10); // Отключаем мобильную связь
}

boolean init_gsm_modem()
{
  if (!modem_availible) {
//    gsm_command("AT+IPR=9600","OK",2); // Установить скорость порта 9600
//    gsm_command("AT+IFC=0,0","OK",2); 
    if (!gsm_command("AT","OK",2)) {
      digitalWrite(DTR_PIN, HIGH);
      delay(200);
      digitalWrite(DTR_PIN, LOW);
      if (!gsm_command("AT","OK",2)) {
        mySerial.write(27);
        delay(300);
        if (!gsm_command("AT","OK",2)) {
#ifdef INT_BLINK_LED
          blink_sequence("0010");
#endif
#ifdef EXT_BLINK_LED
          blink_sequence_red("0010");
#endif
          return false;
        }
      }
    }
    modem_availible = true;
  } 
  return true;
}

void init_GSM(boolean sleep_after_init) {
  if (!init_gsm_modem()) return;
  init_base_gsm();
  delay(200);
#ifdef MODEM_SLEEP_DTR  
  gsm_command("AT+CSCLK=1", "OK", 2); // Переводим модем в режим сна в режиме управления сигналом DTR
  delay(200);
#endif  
  gsm_availible = gsm_command("AT+CFUN=1", "Call Ready", 30); // Подключаемся к сети  
  if (gsm_availible) {
    delay(GSM_DELAY);
//    gsm_command("AT+CMGF?","OK",10); // Устанавливаем текстовый режим чтения смс
    gsm_command("AT+CPMS?","OK",2);
// Устанавливаем текстовый режим чтения смс
    if (gsm_command("AT+CMGF=1","OK",10)) { 
      read_sms();
    }  
    else {
      gsm_command("AT+CMGL=0","OK",10);
    }
    internet_availible = set_gprs_profile();
    if (!internet_availible) {
#ifdef INT_BLINK_LED
      blink_sequence("0100");
#endif
#ifdef EXT_BLINK_LED
      blink_sequence_red("0100");
#endif
      
    }
    if (sleep_after_init) {
      gsm_goto_sleep();
    }  
  }  
  else {
#ifdef INT_BLINK_LED
    blink_sequence("0011");
#endif
#ifdef EXT_BLINK_LED
    blink_sequence_red("0011");
#endif
  }
}

void gsm_get_location(char *location) {
  byte i;
  byte i1 = 0;
  byte i2 = 0;
  byte i3 = 0;

  location[0]='\0';
  if (gsm_command("AT+CIPGSMLOC=1,1","OK",15)) {
    if (strlen(SerialBuffer)>16){
      for (i = 0; i < strlen(SerialBuffer); i++) {
        if (SerialBuffer[i] == ',') {
          if (i1 == 0) {
            i1 = i;
          }
          else if (i2 == 0) {
            i2 = i;
          }
          else {
            i3 = i;
            break;
          }
        }
      }
      if (i1 != 0 && i2 != 0 && i3 != 0) {
        strncpy(location,&SerialBuffer[i2+1],i3-i2-1);
        strncpy(&location[i3-i2-1],&SerialBuffer[i1],i2-i1);
        location[i3-i1-1] = '\0';
      }
#ifdef DEBUG
      Serial.print("Location = ");
      Serial.println(location);
#endif
//      if ((longitudeMajor==0)&&(captureBuffer[2]=='-')) longitudeMajor=255;
    }
  }
}

void gsm_get_battery(byte *percent,int *millivolts) {
  byte charging;  
  char *ptr1;

  if (gsm_command("AT+CBC","OK",2)) {
    memset(gsm_cmd,0,10);
    charging = 0;
    *percent = 0;
    *millivolts = 0;
// Состояние зарядки    
    ptr1 = strchr(SerialBuffer,',');
    if (ptr1 > 0) {
      strncpy(gsm_cmd,ptr1-1,1);
      charging = atoi(gsm_cmd);
// Процент зарядки    
      strncpy(gsm_cmd,ptr1+1,2);
      if (ptr1[3] != ',') {
        gsm_cmd[2] = ptr1[3];
      }  
      *percent = atoi(gsm_cmd);
// Напряжение аккумулятора    
      ptr1 = strchr(ptr1+1,',');
      if (ptr1 > 0) {
        strncpy(gsm_cmd,ptr1+1,4);
        *millivolts = atoi(gsm_cmd);
      }  
    }
//    sscanf(&SerialBuffer[8],"%d,%d,%d",&charging,percent,millivolts);
#ifdef DEBUG
    sprintf(gsm_cmd,"Charg=%d",charging);
    Serial.println(gsm_cmd);
    sprintf(gsm_cmd,"%=%d",*percent);
    Serial.println(gsm_cmd);
    sprintf(gsm_cmd,"mv=%d",*millivolts);
    Serial.println(gsm_cmd);
#endif
  }  
}
#endif

void setup() {
#ifdef DEBUG
  byte b1;
#endif
 
  pinMode(GDO0_PIN, INPUT);
  pinMode(DTR_PIN, OUTPUT);
  digitalWrite(DTR_PIN, LOW);
//  analogReference(INTERNAL);
#ifdef DEBUG
  Serial.begin(9600);
  while (!Serial) {
    ; // wait for serial port to connect. Needed for native USB port only
  }
#endif
  // initialize digital pin LED_BUILTIN as an output.
#ifdef INT_BLINK_LED
  pinMode(LED_BUILTIN, OUTPUT);
#endif
#ifdef EXT_BLINK_LED
  pinMode(RED_LED_PIN, OUTPUT);
  pinMode(YELLOW_LED_PIN, OUTPUT);
#endif
#ifdef CC2500_LEN_CONTROL
  pinMode(LEN_PIN, OUTPUT);
#endif

  SPI.begin();
  //  SPI.setClockDivider(SPI_CLOCK_DIV2);  // max SPI speed, 1/2 F_CLOCK
  digitalWrite(SS, HIGH);

  init_CC2500();  // initialise CC2500 registers
#ifdef DEBUG
  Serial.print("CC2500 PARTNUM=");
  b1 = ReadStatus(PARTNUM);
  Serial.println(b1,HEX);
  Serial.print("CC2500 VERSION=");
  b1 = ReadStatus(VERSION);
  Serial.println(b1,HEX);
#endif
  mySerial.begin(9600);
  loadSettingsFromFlash(); 
#ifdef GSM_MODEM
  digitalWrite(DTR_PIN, LOW);
  init_GSM(true);
#endif
#ifdef ARDUINO_SLEEP
  calibrate_watchdog();
#endif
}

/*
void reset_offsets() {
  int i;
  for (i = 0; i < 4; i++) {
    fOffset[i] = defaultfOffset[i];
  }
}
*/
void swap_channel(unsigned long channel, byte newFSCTRL0) {

  SendStrobe(SIDLE);
  SendStrobe(SFRX);
//  WriteReg(FSCTRL0,newFSCTRL0);
  WriteReg(CHANNR, channel);
  SendStrobe(SRX);  //RX
  while (ReadStatus(MARCSTATE) != 0x0d) {
    // Подождем пока включится режим приема
  }
}

byte ReadRadioBuffer() {
  byte len;
  byte i;
  byte rxbytes;

  memset (&gsm_cmd, 0, sizeof (Dexcom_packet));
  len = ReadStatus(RXBYTES);
#ifdef DEBUG
  Serial.print("Bytes in buffer: ");
  Serial.println(len);
#endif
  if (len > 0 && len < 65) {
    for (i = 0; i < len; i++) {
      if (i < sizeof (Dexcom_packet)) {
        gsm_cmd[i] = ReadReg(RXFIFO);
      }
    }
  }
  memcpy(&Pkt, &gsm_cmd, sizeof (Dexcom_packet));
#ifdef DEBUG
  Serial.print("Dexcom ID: ");
  Serial.println(Pkt.src_addr);
#endif
  return len;
}

boolean WaitForPacket(unsigned int milliseconds_wait, byte channel_index)
{
  unsigned long start_time;
  unsigned long current_time;
  boolean nRet = false;
  boolean packet_on_board;
  byte packet_len;

  start_time = millis();
  swap_channel(nChannels[channel_index], fOffset[channel_index]);

#ifdef CC2500_LEN_CONTROL
  digitalWrite(LEN_PIN, HIGH);
#endif
#ifdef DEBUG
  Serial.print("Ch=");
  Serial.print(nChannels[channel_index]);
  Serial.print(" Tm=");
  Serial.print(start_time);
  Serial.print(" N Tm=");
  Serial.println(next_time);
#endif
  while (true) {
    current_time = millis();
    if (milliseconds_wait != 0 && current_time - start_time > milliseconds_wait) {
      break; // Если превысыли время ожидания на канале - выход
    }
    if (channel_index == 0 && next_time != 0 && current_time > (next_time + wait_after_time)) {
      break; // Если превысыли время следующего пакета на канале 0 - выход
    }
#ifdef INT_BLINK_LED
    blink_builtin_led_quarter();
#endif
#ifdef EXT_BLINK_LED
    if (dex_tx_id == 10858926)   // ABCDE
    {
      blink_yellow_led_half();
      if (low_battery) {
        blink_red_led_half2();
      }
    } else
    {
      blink_yellow_led_quarter();
      if (low_battery) {
        blink_red_led_quarter2();
      }
    }  
#endif
    packet_on_board = false;
    while (digitalRead(GDO0_PIN) == HIGH) {
      packet_on_board = true;
      // Идет прием пакета
    }
    if (packet_on_board) {
      packet_len = ReadRadioBuffer();
      if (Pkt.src_addr == dex_tx_id) {
#ifdef DEBUG
        Serial.print("Catched.Ch=");
        Serial.print(nChannels[channel_index]);
        Serial.print(" Int=");
        if (catch_time != 0) {
          Serial.println(current_time - 500 * channel_index - catch_time);
        }
        else {
          Serial.println("unkn");
        }
#endif
        fOffset[channel_index] += ReadStatus(FREQEST);
        catch_time = current_time - 500 * channel_index; // Приводим к каналу 0
        nRet = true;
      } 
//      if (next_time != 0 && !nRet && channel_index == 0 && current_time < next_time && next_time-current_time < 2000) {
      if (next_time != 0 && !nRet && packet_len != 0) {
#ifdef DEBUG
        Serial.print("Try.Ch=");
        Serial.print(nChannels[channel_index]);
        Serial.print(" Time=");
        Serial.println(current_time);
#endif
        swap_channel(nChannels[channel_index], fOffset[channel_index]);
      }
      else {
        break;
      }
    }
  }

#ifdef CC2500_LEN_CONTROL
  digitalWrite(LEN_PIN, LOW);
#endif
#ifdef INT_BLINK_LED
  digitalWrite(LED_BUILTIN, LOW);
#endif
#ifdef EXT_BLINK_LED
  digitalWrite(YELLOW_LED_PIN, LOW);
  digitalWrite(RED_LED_PIN, LOW);
#endif
  return nRet;
}

boolean get_packet (void) {
  byte nChannel;
  boolean nRet;

  nRet = false;
  for (nChannel = 0; nChannel < NUM_CHANNELS; nChannel++)
  {
    if (WaitForPacket (waitTimes[nChannel], nChannel)) {
      sequential_missed_packets = 0; // Сбрасываем счетчик непойманных пакетов
      nRet = true;
      break;
    }
  }
  if (!nRet) {
    if (next_time > 0) {
      sequential_missed_packets++;
    }
#ifdef DEBUG
    Serial.print("Missed-");
    Serial.println(sequential_missed_packets);
#endif
    if (sequential_missed_packets >= misses_until_failure && next_time > 0) { // Кол-во непойманных пакетов превысило заданное кол-во. Будем ловить пакеты непрерывно
#ifdef ARDUINO_SLEEP
      calibrate_watchdog();
#endif
      next_time = 0;
      sequential_missed_packets = 0; // Сбрасываем счетчик непойманных пакетов
    }  
  }
  else {
    next_time = catch_time; 
  }

  if (next_time != 0) {
    next_time += FIVE_MINUTE;
  }
  SendStrobe(SIDLE);
  SendStrobe(SFRX);

  return nRet;
}

#ifdef GSM_MODEM
boolean send_gprs_data() {
  char lastLocation[30];  
  byte batteryPercent = 0;
  int batteryMillivolts = 0; 
  boolean res1;

  gsm_get_location(lastLocation);
  gsm_get_battery(&batteryPercent, &batteryMillivolts);
  if (batteryPercent > 0 && batteryPercent < 30)
  {
    low_battery = true;
  } else
  {
    low_battery = false;   
  }
  gsm_command("AT+HTTPTERM", "OK", 2); // Завершить сессию на вскяий случай
  gsm_command("AT+HTTPINIT", "OK", 10); // Начинаем http сессию
  gsm_command("AT+HTTPPARA=\"CID\",1", "OK", 2) ;  
  gsm_command("AT+HTTPPARA=\"UA\",\"" my_user_agent "\"", "OK", 2);  // User agent для http запроса
// Адрес сервера паракита
  sprintf(gsm_cmd,"AT+HTTPPARA=\"URL\",\"%s?rr=%lu&zi=%lu&pc=%s&lv=%lu&lf=%lu&db=%hhu&ts=%lu&bp=%d&bm=%d&ct=%d&gl=%s\" ",settings.http_url,millis(),dex_tx_id,settings.password_code,
                                                                                                                         dex_num_decoder(Pkt.raw),dex_num_decoder(Pkt.filtered)*2,
                                                                                                                         Pkt.battery,millis()-catch_time,batteryPercent, batteryMillivolts, 
                                                                                                                         37, lastLocation);         
  gsm_command(gsm_cmd,"OK",2) ;
  res1 = gsm_command("AT+HTTPACTION=0", "+HTTPACTION: 0,200,", 60); // Отправляем запрос на сервер
  gsm_command("AT+HTTPREAD", my_webservice_reply , 20) ;    // Читаем ответ вэб-сервиса
  gsm_command("AT+HTTPTERM", "OK", 2); // Завершаем http сессию
  if (res1) {
    gsm_error_count = 0;
  }
  return res1;
}
#endif

void print_packet() {
  
#ifdef GSM_MODEM
  gsm_wake_up(); // Будим GSM-модем
  if (!modem_availible) {
    init_GSM(false);
  }
  if (internet_availible) {
    if (!send_gprs_data()) {
      internet_availible = set_gprs_profile();
      if (internet_availible) {
        if (!send_gprs_data()) {      
          gsm_availible = false;
          gsm_error_count++;
        }
      }
      else {
#ifdef INT_BLINK_LED
        blink_sequence("0100");
#endif
#ifdef EXT_BLINK_LED
        blink_sequence_red("0100");
#endif      
      }
        
    }
    
  }  
  else {
    modem_availible = false;
#ifdef INT_BLINK_LED
    blink_sequence("0100");
#endif
#ifdef EXT_BLINK_LED
    blink_sequence_red("0100");
#endif      
  }
#endif
/*
#ifdef DEBUG
  Serial.print(Pkt.len, HEX);
  Serial.print("\t");
  Serial.print(Pkt.dest_addr, HEX);
  Serial.print("\t");
  Serial.print(Pkt.src_addr, HEX);
  Serial.print("\t");
  Serial.print(Pkt.port, HEX);
  Serial.print("\t");
  Serial.print(Pkt.device_info, HEX);
  Serial.print("\t");
  Serial.print(Pkt.txId, HEX);
  Serial.print("\t");
  Serial.print(dex_num_decoder(Pkt.raw));
  Serial.print("\t");
  Serial.print(dex_num_decoder(Pkt.filtered)*2);
  Serial.print("\t");
  Serial.print(Pkt.battery, HEX);
  Serial.print("\t");
  Serial.print(Pkt.unknown, HEX);
  Serial.print("\t");
  Serial.print(Pkt.checksum, HEX);
  Serial.print("\t");
  Serial.print(Pkt.RSSI, HEX);
  Serial.print("\t");
  Serial.print(Pkt.LQI2, HEX);
  Serial.println(" OK");
#endif
*/
}

#ifdef ARDUINO_SLEEP
void setup_watchdog(byte sleep_time)
{
  wdt_enable(sleep_time);
  WDTCSR|= _BV(WDIE);     // enable interrupts instead of MCU reset when watchdog is timed out
}

/************************************************************************************************************/
/*
    Puts MCU into the sleep state

    NOTE: There are 6 different sleeps modes:
          * SLEEP_MODE_IDLE..........The least power savings state. CPU stopped but Analog
                                     comparator, ADC, USI, Timer/Counter, Watchdog (if enabled),
                                     & the interrupt system continues operating. (by default in "sleep.h")
          * SLEEP_MODE_ADC...........ADC Noise Reduction. CPU stopped but the ADC, the external
                                     interrupts, & the Watchdog (if enabled) continue operating.
          * SLEEP_MODE_PWR_SAVE......Supported by Atiny25, Atiny45, Atiny85.
          * SLEEP_MODE_EXT_STANDBY...Not supported by Atiny25, Atiny45, Atiny85.
          * SLEEP_MODE_STANDBY.......Not supported by Atiny25, Atiny45, Atiny85.
          * SLEEP_MODE_PWR_DOWN......The most power savings state. All oscillators are stopped, only an
                                     External Reset, Watchdog Reset, Brown-out Reset, USI start condition
                                     interupt & external level interrupt on INT0 or a pin change interrupt
                                     can wake up the MCU.      
*/
/************************************************************************************************************/
void arduino_sleep()
{
  set_sleep_mode(SLEEP_MODE_PWR_DOWN); //set the sleep type
//  set_sleep_mode(SLEEP_MODE_PWR_SAVE); //set the sleep type
//  set_sleep_mode(SLEEP_MODE_STANDBY); //set the sleep type
  sleep_mode();                        /*system stops & sleeps here (automatically sets the SE (Sleep Enable) bit
                                         (so the sleep is possible), goes to sleep, wakes-up from sleep after an
                                         interrupt (if interrupts are enabled) or WDT timed out (if enabled) and
                                         clears the SE (Sleep Enable) bit afterwards).
                                         the sketch will continue from this point after interrupt or WDT timed out
                                       */
}

void arduino_wake_up() {
  power_all_enable();       //enable all peripheries (timer0, timer1, Universal Serial Interface, ADC)
  delay(5);                 //to settle down the ADC and peripheries
}

void calibrate_watchdog() {
  unsigned long start_time;
  unsigned long end_time;

  watchdog_counter = 0;     //reset watchdog_counter
  start_time = millis();
  setup_watchdog(WDTO_2S); // Включаем строжевого пса
  while (watchdog_counter < 4) 
  {
//    arduino_sleep();
  }
  wdt_disable(); // Выключим строжевого пса
  end_time = millis();
  wdto_2s_ms = (end_time - start_time) / 4;
#ifdef DEBUG
  Serial.println("WDT_2S=");
  Serial.println(wdto_2s_ms);
#endif
}
#endif

void loop() {
  unsigned long current_time;
  unsigned long watchdog_counter_max;
  
  if (next_time != 0) {
#ifdef DEBUG
    Serial.print("n_tm-");
    Serial.print(next_time);
    Serial.print(" c_tm-");
    Serial.print(millis());
    Serial.print(" int-");
    Serial.println(next_time - millis() - 3000);
#endif
    current_time = millis();
    if  (next_time > current_time && (next_time - current_time) < FIVE_MINUTE)  {
#ifdef ARDUINO_SLEEP
#ifdef DEBUG
      Serial.println("Sleep");
      Serial.print(" c_tm-");
      Serial.println(millis());
      delay(200);
#endif
      watchdog_counter = 0;     //reset watchdog_counter
      watchdog_counter_max = ((next_time - current_time - 2000) / wdto_2s_ms); // Будем включаться за 2 секунды + остаток таймера до пакета
#ifdef NO_NEXT_TIME      
      watchdog_counter_max--;
#endif
      next_time -= wdto_2s_ms*watchdog_counter_max;
      power_all_disable();                 //disable all peripheries (timer0, timer1, Universal Serial Interface, ADC)
      setup_watchdog(WDTO_2S); // Включаем строжевого пса
      while (watchdog_counter < watchdog_counter_max) 
      {
        arduino_sleep();
      }
//      arduino_sleep(WDTO_4S);
      wdt_disable(); // Выключим строжевого пса
      arduino_wake_up();
#endif

#ifdef ARDUINO_DELAY
#ifdef DEBUG
      Serial.println("Delay");
#endif
      delay(next_time - current_time - 2000); // Можно спать до следующего пакета. С режимом сна будем разбираться позже
#endif
      
#ifdef DEBUG
      Serial.println("WakeUp");
#endif
    }
    else {
#ifdef DEBUG
      Serial.println("Timer overflow");
#endif
      next_time = 0;
    }
  }
#ifdef NO_NEXT_TIME
  next_time = 0;
#endif
  if (get_packet ())
  {
    print_packet ();
/*    
    if (gsm_error_count > 3) {
      resetFunc();
    }
*/    
  }
#ifdef GSM_MODEM
  if (gsm_availible) {
    gsm_wake_up(); // Будим GSM-модем
    read_sms(); // Прочитаем полученные смс-ки
    gsm_goto_sleep();
  }  
#endif

}

#ifdef ARDUINO_SLEEP
/************************************************************************************************************/
/*
    ISR(WDT_vect)
    
    Watchdog Interrupt Service (automatically executed when watchdog is timed out)
*/
/************************************************************************************************************/
ISR(WDT_vect)
{
  watchdog_counter++;
  WDTCSR|= _BV(WDIE);     // enable interrupts instead of MCU reset when watchdog is timed out
}
#endif

