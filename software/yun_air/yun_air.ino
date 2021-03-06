#define DEBUG
#ifdef DEBUG
#include <Console.h>
#define emit(s) Console.print(s);
#define emitln(s) Console.println(s);
#endif
//
//  Air Quality Sensor Suite
//

//
// Bridge and Modbus functions
//
#include <Bridge.h>
Process p;
// write a float to the bridge
void setFloat(char *a, float v) {
  Bridge.put(a, String(v));
#ifdef DEBUG
  emit("PUT "); emit(a); emit(":"); emitln(v);
#endif
}
// read a float from the bridge
#define BUF_SIZE 128
char buf[BUF_SIZE];
float getFloat(char *a) {
  Bridge.get(a, buf, BUF_SIZE);
#ifdef DEBUG
  emit("GET "); emit(a); emit(":"); emitln(buf);
#endif  
  return atof(buf);
}
uint32_t getLong(char *a) {
  Bridge.get(a, buf, BUF_SIZE);
#ifdef DEBUG
  emit("GET "); emit(a); emit(":"); emitln(buf);
#endif
  return atoi(buf);
}

//
// Modbus register map
//
// modpoll -m tcp -0 -c7 -t 4:float -r 40001 [ip_addr]
// modpoll -m tcp -0 -c3 -t 4:float -r 40101 [ip_addr]
//
#define RO_START "40001" // ro block
#define RO_LENGTH "7"
#define S0_CurrentMillis         "40001"
#define S1_TemperatureF          "40003"
#define S1_Humidity              "40005"
#define S2_ConcentrationPM10     "40007"
#define S2_ConcentrationPM25     "40009"
#define S3_Concentration         "40011"
#define S3_Alarm                 "40013"

#define RW_START "40101" // rw block
#define RW_LENGTH "4"
#define S1_UpdateInterval        "40101"
#define S2_UpdateInterval        "40103"
#define S2_FilterWeight          "40105"
#define S3_UpdateInterval        "40107"

//
// S1 Adafruit DHT22
// https://learn.adafruit.com/dht/connecting-to-a-dhtxx-sensor
//
#include "DHT.h"
#define S1_Pin 11
#define S1_Type DHT22
DHT dht(S1_Pin, S1_Type);

//
// S2 - Shinyei Model PPD42NS Particle Sensor
// http://www.sca-shinyei.com/pdf/PPD42NS.pdf
// SW-PWM-01A
// http://www.mouser.com/pdfdocs/AAS920643ATelaireDustSensor081414web.PDF
// http://www.digikey.com/Web%20Export/Supplier%20Content/amphenol-advanced-sensors-45/pdf/amphenol-advanced-smart-sensor.pdf?redirected=1
//
#define S2_P1_Pin 10
#define S2_P2_Pin  9

//
// S3 - MQ-2
//
#define MQ2_D  12
#define MQ2_A  A0

// interrupt and timer storage
unsigned long S0StartTime = 0, S1StartTime = 0, S2StartTime = 0, S3StartTime = 0;
int state09 = HIGH, state10 = HIGH;
unsigned long start09, start10;
volatile unsigned long lpo09, lpo10;

void setup(void)
{
  // Start the bridge
  Bridge.begin();
#ifdef DEBUG
  Console.begin();
  delay(2000);
  emitln("Hello world!");
#endif  
  pinMode(13, OUTPUT);
  digitalWrite(13, LOW);

  // Initialize S1
  dht.begin();
  setFloat(S1_UpdateInterval, 10000.0);

  // Initialize S2
  pinMode(S2_P1_Pin, INPUT);
  pinMode(S2_P2_Pin, INPUT);
  // pin change interrupt
  PCMSK0 |= bit (PCINT5);  // want pin  9
  PCMSK0 |= bit (PCINT6);  // want pin 10
  PCIFR  |= bit (PCIF0);   // clear any outstanding interrupts
  PCICR  |= bit (PCIE0);   // enable pin change interrupts for D8 to D13
  setFloat(S2_UpdateInterval, 10000.0);
  setFloat(S2_FilterWeight, 30.0);

  // Initialize S3
  pinMode(MQ2_A, INPUT);
  pinMode(MQ2_D, INPUT);
  setFloat(S3_UpdateInterval, 10000.0);

  // Start the modbus server script
  // in case a previous connection needs to timeout
  
  // kill -9 $(ps | grep "modbus" | awk '{ print $1 }')
  // kill -9 $( pgrep -f "python /mnt/sd/modbus_tcp_slave.py")
//  p.runShellCommand("kill -9 $( pgrep -f \"python /mnt/sd/modbus_tcp_slave.py\")");  
//  while (p.available() > 0);
  delay(125000);

  p.begin("python");
  p.addParameter("/mnt/sda1/modbus_tcp_slave.py"); // modbus float holding registers
  p.addParameter(RO_START);                      // read only start address
  p.addParameter(RO_LENGTH);                     // read only number of holding registers
  p.addParameter(RW_START);                      // read/write start address (optional)
  p.addParameter(RW_LENGTH);                     // read/write number of holding registers (optional)
  p.runAsynchronously();
  delay(5000);
  // 
  lpo09 = lpo10 = 0;
}

// optional limiter for S2
unsigned long limit(unsigned long x) {
  //  return (x < 10000ul || x > 90000ul) ? 0 : x;
  return x;
}
// interrupt service routine
ISR (PCINT0_vect) {
  unsigned long us = micros();
  if (state09 != digitalRead(S2_P2_Pin))
    if ((state09 = digitalRead(S2_P2_Pin)) == LOW)
      start09 = us;
    else
      lpo09 += limit(us - start09);
  if (state10 != digitalRead(S2_P1_Pin))
    if ((state10 = digitalRead(S2_P1_Pin)) == LOW)
      start10 = us;
    else
      lpo10 += limit(us - start10);
}

//
// poll the sensors and update the bridge
//
void loop(void)
{
  unsigned long curMillis = millis();
  float x, y;

  // S0 - suite
  if (curMillis - S0StartTime > 1000) {
    setFloat(S0_CurrentMillis, curMillis);
    S0StartTime = curMillis;
  }

  // S1 - temperature and humidity
  if (curMillis - S1StartTime > getLong(S1_UpdateInterval)) {
    digitalWrite(13, HIGH);
    setFloat(S1_TemperatureF, dht.readTemperature(true) - 4.0); // correction for mounting too close to MQ-2
    setFloat(S1_Humidity, dht.readHumidity());

    S1StartTime = curMillis;
    digitalWrite(13, LOW);
  }

  // S2 - particulate dust
  if (curMillis - S2StartTime > getLong(S2_UpdateInterval)) {
    digitalWrite(13, HIGH);
    x = 0.1 * float(lpo10) / getFloat(S2_UpdateInterval);
    y = 1.1 * x * x * x  - 3.8 * x * x + 520 * x; // + 0.62;
    setFloat(S2_ConcentrationPM10, (y + getFloat(S2_ConcentrationPM10) * (getFloat(S2_FilterWeight) - 1.0)) / getFloat(S2_FilterWeight));
    lpo10 = 0;

    x = 0.1 * float(lpo09) / getFloat(S2_UpdateInterval);
    y = 1.1 * x * x * x  - 3.8 * x * x + 520 * x; // + 0.62;
    setFloat(S2_ConcentrationPM25, (y + getFloat(S2_ConcentrationPM25) * (getFloat(S2_FilterWeight) - 1.0)) / getFloat(S2_FilterWeight));

    lpo09 = 0;
    S2StartTime = curMillis;
    digitalWrite(13, LOW);
  }

  // S3 - volatile organic compounds
  if (curMillis - S3StartTime > getLong(S3_UpdateInterval)) {
    digitalWrite(13, HIGH);
    setFloat(S3_Concentration, (5.0 / 4.0) * (float(analogRead(MQ2_A)) / 1024.0));
    setFloat(S3_Alarm, float(digitalRead(MQ2_D)));
    S3StartTime = curMillis;
    digitalWrite(13, LOW);
  }
}


