// Teensy 4.1: USB Serial and Serial1 UART (RX=0, TX=1), 115200 baud.
// Supports existing USB clients and blink_teensy29.py over Pi GPIO UART.
// Protocol: VERSION, ON/OFF, ON29/OFF29, MOTORB 0..255, STOPB, STATUSB, ADC14, ADC3, MOTORS -255..255 -255..255, STOPALL, STATUSAB.
#include <Arduino.h>
#include <IntervalTimer.h>
#if !defined(ARDUINO_TEENSY41)
#error Select Teensy 4.1 as the target board.
#endif

constexpr uint8_t EXTERNAL_LED = 29;
constexpr uint8_t CLOCK_EDGE_PIN = 31;
bool edgeSessionActive = false;
uint64_t edgeSession = 0;
uint32_t edgeSequence = 0, edgeLastCommand = 0;
bool jobLedActive = false;
uint32_t jobLedStarted = 0, jobLedDuration = 0, jobLedRefresh = 0;

constexpr uint8_t PHOTO_A = A11, PHOTO_B = A12, PHOTO_C = A13;
constexpr uint8_t PHOTO_ADC = A0;  // Teensy pin 14, input range 0..3.3 V.
constexpr uint8_t PWMA = 3, AIN1 = 4, AIN2 = 5, STBY = 6;
constexpr uint8_t BIN1 = 7, BIN2 = 8, PWMB = 9;
constexpr uint32_t MOTOR_TIMEOUT_MS = 750;
// Each transport needs its own parser so partial lines cannot mix.
struct CommandBuffer {
  char command[64];
  size_t used = 0;
  bool overflow = false;
};
CommandBuffer usbBuffer, uartBuffer;
bool lastCommandViaUsb = true;
bool lastMotorCommandViaUsb = true;
uint32_t lastCommand = 0;
uint32_t lastMotorCommand = 0;
int motorDuty = 0;  // right / B, signed in dual-motor mode
int leftDuty = 0;

void stopMotor() {
  digitalWrite(STBY, LOW);
  analogWrite(PWMA, 0); analogWrite(PWMB, 0);
  digitalWrite(AIN1, LOW); digitalWrite(AIN2, LOW);
  digitalWrite(BIN1, LOW); digitalWrite(BIN2, LOW);
  leftDuty = 0; motorDuty = 0;
}


void stopJobLed() { jobLedActive = false; digitalWriteFast(EXTERNAL_LED, LOW); }

void stopEdges() {
  stopJobLed();
  digitalWriteFast(CLOCK_EDGE_PIN, LOW);
  digitalWriteFast(EXTERNAL_LED, LOW);
  edgeSessionActive = false;
}

// IntervalTimer uses PIT, independently of the motor PWM timers. Serial I/O and
// formatting stay in loop(); the ISR only converts three channels into a ring.
IntervalTimer sampleTimer;
struct SensorSample {
  uint64_t sequence, started, completed;
  uint16_t a, b, c;
};
SensorSample samples[16];
volatile unsigned sampleHead = 0, sampleTail = 0;
volatile uint32_t sampleDrops = 0;
volatile bool streaming = false, adcFault = false;
uint64_t sampleSequence = 0;
uint8_t streamTxMemory[256];
uint64_t clockHigh = 0;
uint32_t clockLow = 0;
uint64_t clockUs() {
  uint32_t mask;
  __asm__ volatile("mrs %0, primask" : "=r" (mask) :: "memory");
  __disable_irq();
  const uint32_t low = micros();
  if (low < clockLow) clockHigh += (uint64_t(1) << 32);
  clockLow = low;
  const uint64_t result = clockHigh | low;
  __asm__ volatile("msr primask, %0" :: "r" (mask) : "memory");
  return result;
}
// Core analogRead() calls yield(). Avoid that inside an ISR; setup() first
// completes core ADC calibration. Channels are from Teensy 4.1 core analog.c:
// A11/pin25 = ADC1 channel2, A12/pin26 = ADC2 channel3, A13/pin27 = ADC2 channel4.
// No other ADC calls are permitted while streaming. Each conversion is bounded.
bool convertAdc(bool second, uint32_t channel, uint16_t &value) {
  const uint32_t start = micros();
  if (second) {
    ADC2_HC0 = channel;
    while (!(ADC2_HS & ADC_HS_COCO0))
      if (uint32_t(micros() - start) >= 100) return false;
    value = ADC2_R0;
  } else {
    ADC1_HC0 = channel;
    while (!(ADC1_HS & ADC_HS_COCO0))
      if (uint32_t(micros() - start) >= 100) return false;
    value = ADC1_R0;
  }
  return true;
}
void sampleTick() {
  if (!streaming || adcFault) return;
  SensorSample s;
  s.sequence = ++sampleSequence;
  s.started = clockUs();
  if (!convertAdc(false, 2, s.a) || !convertAdc(true, 3, s.b) ||
      !convertAdc(true, 4, s.c)) {
    adcFault = true;
    digitalWrite(STBY, LOW); // immediate hardware disable on conversion fault
    return;
  }
  s.completed = clockUs();
  unsigned next = (sampleHead + 1) % 16;
  if (next == sampleTail) { ++sampleDrops; return; }
  samples[sampleHead] = s;
  __asm__ volatile("dmb" ::: "memory");
  sampleHead = next;
}
void endStream() {
  streaming = false;
  sampleTimer.end();
  sampleHead = sampleTail = 0;
}
void transmitSample() {
  if (adcFault) {
    endStream();
    stopMotor();
    adcFault = false;
    stopJobLed();
    Serial1.println("ERROR ADC timeout");
    return;
  }
  if (!streaming || sampleTail == sampleHead) return;
  SensorSample s = samples[sampleTail]; // ISR never overwrites the unread slot
  char line[128];
  const int n = snprintf(line, sizeof line, "S %llu %llu %llu %lu %u %u %u\n",
      (unsigned long long)s.sequence, (unsigned long long)s.started,
      (unsigned long long)s.completed, (unsigned long)sampleDrops, s.a, s.b, s.c);
  if (n > 0 && n < int(sizeof line) && Serial1.availableForWrite() >= n) {
    Serial1.write((const uint8_t *)line, n);
    __asm__ volatile("dmb" ::: "memory");
    sampleTail = (sampleTail + 1) % 16;
  }
}

int direction(int duty) { return (duty > 0) - (duty < 0); }

void setChannel(uint8_t in1, uint8_t in2, uint8_t pwm, int duty) {
  digitalWrite(in1, duty > 0 ? HIGH : LOW);
  digitalWrite(in2, duty < 0 ? HIGH : LOW);
  // At zero, IN1=IN2=LOW/PWM=HIGH is high impedance, even with STBY high.
  analogWrite(pwm, duty == 0 ? 255 : abs(duty));
}

void driveMotors(int left, int right) {
  if (left == 0 && right == 0) { stopMotor(); return; }
  // Disable outputs while changing channel direction; duty-only updates stay smooth.
  bool changing = direction(left) != direction(leftDuty) ||
                  direction(right) != direction(motorDuty);
  if (changing) { digitalWrite(STBY, LOW); delayMicroseconds(100); }
  setChannel(AIN1, AIN2, PWMA, left);
  setChannel(BIN1, BIN2, PWMB, right);
  leftDuty = left; motorDuty = right;
  lastMotorCommand = millis();
  lastMotorCommandViaUsb = lastCommandViaUsb;
  digitalWrite(STBY, HIGH);
}

void forwardMotor(uint8_t duty) { driveMotors(0, duty); }

bool parseDuty(const char *&p, int &duty) {
  bool negative = *p == '-';
  if (negative) ++p;
  if (*p < '0' || *p > '9') return false;
  unsigned value = 0;
  while (*p >= '0' && *p <= '9') {
    value = value * 10 + unsigned(*p++ - '0');
    if (value > 255) return false;
  }
  duty = negative ? -int(value) : int(value);
  return true;
}

// Standalone diagnostic: one identified pulse per request, with motors disabled.
// Normal ADC streaming is stopped before entering this mode.
bool edgeCommand(const char *command, Stream &port) {
  if (strncmp(command, "EDGE ", 5) != 0) return false;
  if (!strcmp(command, "EDGE STOP")) {
    stopEdges(); stopMotor(); port.println("OK"); return true;
  }
  unsigned long long session = 0;
  unsigned long seq = 0;
  int used = 0;
  if (sscanf(command, "EDGE BEGIN %16llx%n", &session, &used) == 1 &&
      used > 0 && command[used] == '\0' && session != 0) {
    endStream(); stopMotor(); stopEdges();
    digitalWriteFast(CLOCK_EDGE_PIN, LOW); pinMode(CLOCK_EDGE_PIN, OUTPUT);
    edgeSession = session; edgeSequence = 0;
    edgeSessionActive = true; edgeLastCommand = millis();
    char reply[40]; snprintf(reply, sizeof reply, "EDGE READY %016llx", session);
    port.println(reply); return true;
  }
  used = 0;
  unsigned flashes = 2;
  int fields = sscanf(command, "EDGE BLINK %16llx %u%n", &session, &flashes, &used);
  if (fields != 2) { used = 0; flashes = 2; fields = sscanf(command, "EDGE BLINK %16llx%n", &session, &used); }
  if (fields >= 1 && flashes >= 1 && flashes <= 3 &&
      used > 0 && command[used] == '\0' && edgeSessionActive && session == edgeSession) {
    stopMotor();
    for (unsigned i = 0; i < flashes; ++i) {
      digitalWriteFast(EXTERNAL_LED, HIGH);
      digitalWriteFast(CLOCK_EDGE_PIN, HIGH);
      delay(500);
      digitalWriteFast(EXTERNAL_LED, LOW);
      digitalWriteFast(CLOCK_EDGE_PIN, LOW);
      delay(500);
    }
    edgeLastCommand = millis();
    char reply[48]; snprintf(reply, sizeof reply, "EDGE BLINKED %016llx", session);
    port.println(reply); return true;
  }
  used = 0;
  if (sscanf(command, "EDGE %16llx %lu%n", &session, &seq, &used) != 2 ||
      used <= 0 || command[used] != '\0' || !edgeSessionActive ||
      session != edgeSession || seq == 0 || seq > 1000000 || seq != edgeSequence + 1) {
    stopEdges(); stopMotor(); port.println("ERROR EDGE session/sequence"); return true;
  }
  edgeSequence = seq; edgeLastCommand = millis(); stopMotor();
  uint32_t mask;
  __asm__ volatile("mrs %0, primask" : "=r" (mask) :: "memory");
  __disable_irq();
  const uint64_t before = clockUs();
  digitalWriteFast(CLOCK_EDGE_PIN, HIGH);
  __asm__ volatile("dsb" ::: "memory");
  const bool readback = digitalReadFast(CLOCK_EDGE_PIN);
  const uint64_t after = clockUs();
  __asm__ volatile("msr primask, %0" :: "r" (mask) : "memory");
  // Deliberately wide pulse; no interrupts are masked during the hold.
  // UART delivery is after the physical pulse and is not a timing reference.
  delayMicroseconds(2000);
  digitalWriteFast(CLOCK_EDGE_PIN, LOW);
  __asm__ volatile("dsb" ::: "memory");
  char reply[112];
  snprintf(reply, sizeof reply, "EDGE %016llx %lu %llu %llu %u", session, seq,
           (unsigned long long)before, (unsigned long long)after, unsigned(readback));
  port.println(reply);
  return true;
}

void handleCommand(const char *command, Stream &port, bool viaUsb, uint64_t receivedUs) {
  lastCommand = millis();
  lastCommandViaUsb = viaUsb;
  if (strcmp(command, "JOB CAL") == 0) {
    endStream(); stopMotor(); stopEdges();
    for (unsigned i=0;i<2;++i) {
      digitalWriteFast(EXTERNAL_LED,HIGH); delay(500);
      digitalWriteFast(EXTERNAL_LED,LOW); delay(500);
    }
    port.println("OK"); return;
  }
  if (strncmp(command,"JOB RUN ",8)==0) {
    unsigned long duration=0; int used=0;
    if (sscanf(command,"JOB RUN %lu%n",&duration,&used)!=1 || command[used]!='\0' ||
        !streaming || edgeSessionActive || duration<1 || duration>3600000) {
      stopJobLed(); stopMotor(); port.println("ERROR JOB duration"); return;
    }
    jobLedStarted=jobLedRefresh=millis();jobLedDuration=duration;jobLedActive=true;
    digitalWriteFast(EXTERNAL_LED,HIGH);port.println("OK");return;
  }
  if (!strcmp(command,"JOB STATUS")) {
    port.print("JOB active=");port.print(jobLedActive?1:0);
    port.print("; led=");port.println(digitalReadFast(EXTERNAL_LED)?1:0);return;
  }
  if (edgeCommand(command, port)) return;
  if (edgeSessionActive && (strncmp(command, "MOTOR", 5) == 0 ||
                            strncmp(command, "STREAM ", 7) == 0)) {
    stopMotor(); port.println("ERROR EDGE diagnostic active"); return;
  }
  if (strcmp(command, "VERSION") == 0) {
    // Preserve the version prefix accepted by the existing halo_teensy.py.
    port.print("HALO_TEENSY/1.0; board=Teensy 4.1; OS=none (bare metal); Teensyduino=");
    port.print(TEENSYDUINO);
    port.println("; features=GPIO29,MOTOR_B,MOTOR_AB,ADC14,ADC3,STREAM1,CLOCK,MOTOR_TIME,TIME_SYNC,GPIO_EDGE_CLOCK,JOB_LED; revision=15; LED_PIN=29; UART1=115200,8N1; RX1=0; TX1=1; AIN1=4; AIN2=5; PWMA=3; BIN1=7; BIN2=8; PWMB=9; STBY=6; ADC14=A0; ADC3=25,26,27; ADC_BITS=10");
  } else if (strcmp(command, "TIMESYNC 00000000000000000000 00000000000000000000") == 0) {
    // Complete-line receive stamp, then reply-construction stamp. Both clocks
    // keep running; this diagnostic does not touch ADC scheduling or motor refresh.
    // Request and reply are exactly 51 bytes, including LF, to balance UART time.
    char line[64];
    const uint64_t replyUs = clockUs();
    const int n = snprintf(line, sizeof line, "TIMESYNC %020llu %020llu\n",
                           (unsigned long long)receivedUs, (unsigned long long)replyUs);
    if (n == 51) port.write((const uint8_t *)line, n);
  } else if (strcmp(command, "CLOCK") == 0) {
    char line[40];
    snprintf(line, sizeof line, "CLOCK %llu", (unsigned long long)clockUs());
    port.println(line);
  } else if (strcmp(command, "STREAM 0") == 0) {
    endStream(); stopMotor(); stopJobLed(); port.println("OK");
  } else if (strncmp(command, "STREAM ", 7) == 0) {
    const char *q = command + 7;
    int rate = 0;
    bool valid = parseDuty(q, rate) && *q == '\0' && rate >= 1 && rate <= 100 && !viaUsb;
    endStream(); stopMotor();
    if (!valid) { port.println("ERROR STREAM rate 1..100 on UART only"); return; }
    sampleSequence = 0; sampleDrops = 0; adcFault = false;
    streaming = true;
    if (!sampleTimer.begin(sampleTick, 1000000.0 / rate)) {
      streaming = false; port.println("ERROR timer unavailable"); return;
    }
    sampleTimer.priority(64);
    port.println("OK");
  } else if (streaming && (strcmp(command, "ADC3") == 0 || strcmp(command, "ADC14") == 0)) {
    port.println("ERROR stop stream before manual ADC");
  } else if (strcmp(command, "ADC3") == 0) {
    const int a = analogRead(PHOTO_A);
    const int b = analogRead(PHOTO_B);
    const int c = analogRead(PHOTO_C);
    port.print("ADC3 "); port.print(a);
    port.print(' '); port.print(b);
    port.print(' '); port.println(c);
  } else if (strcmp(command, "ADC14") == 0) {
    port.print("ADC14 "); port.println(analogRead(PHOTO_ADC));
  } else if (strncmp(command, "MOTORS ", 7) == 0 || strncmp(command, "MOTORST ", 8) == 0) {
    const bool timed = strncmp(command, "MOTORST ", 8) == 0;
    const char *p = command + (timed ? 8 : 7);
    int left, right;
    bool valid = parseDuty(p, left);
    if (valid && *p == ' ') { ++p; valid = parseDuty(p, right) && *p == '\0'; }
    else valid = false;
    if (valid) {
      if (jobLedActive) jobLedRefresh=millis();
      driveMotors(left, right);
      if (timed) {
        char reply[40];
        snprintf(reply, sizeof reply, "OK T %llu", (unsigned long long)receivedUs);
        port.println(reply);
      } else port.println("OK");
    }
    else { stopMotor(); port.println("ERROR expected MOTORS -255..255 -255..255"); }
  } else if (strcmp(command, "STATUSAB") == 0) {
    port.print("MOTORS left="); port.print(leftDuty);
    port.print("; right="); port.print(motorDuty);
    port.print("; standby="); port.println(leftDuty == 0 && motorDuty == 0 ? 1 : 0);
  } else if (strcmp(command, "STOPB") == 0 || strcmp(command, "STOPALL") == 0) {
    stopMotor(); stopJobLed(); port.println("OK");
  } else if (strcmp(command, "STATUSB") == 0) {
    port.print("MOTORB duty="); port.print(motorDuty);
    port.print("; standby="); port.println(leftDuty == 0 && motorDuty == 0 ? 1 : 0);
  } else if (strncmp(command, "MOTORB ", 7) == 0) {
    unsigned value = 0;
    bool valid = command[7] != '\0';
    for (const char *p = command + 7; *p; ++p) {
      if (*p < '0' || *p > '9') { valid = false; break; }
      value = value * 10 + unsigned(*p - '0');
      if (value > 255) { valid = false; break; }
    }
    if (valid) { forwardMotor(uint8_t(value)); port.println("OK"); }
    else { stopMotor(); port.println("ERROR duty must be 0..255"); }
  } else if (strcmp(command, "ON") == 0) {
    digitalWrite(LED_BUILTIN, HIGH);
    port.println("OK");
  } else if (strcmp(command, "OFF") == 0) {
    digitalWrite(LED_BUILTIN, LOW);
    port.println("OK");
  } else if (strcmp(command, "ON29") == 0) {
    digitalWrite(EXTERNAL_LED, HIGH);
    port.println("OK");
  } else if (strcmp(command, "OFF29") == 0) {
    digitalWrite(EXTERNAL_LED, LOW);
    port.println("OK");
  } else {
    stopMotor();
    port.println("ERROR unknown command");
  }
}

void setup() {
  digitalWriteFast(CLOCK_EDGE_PIN, LOW); pinMode(CLOCK_EDGE_PIN, OUTPUT);
  digitalWrite(STBY, LOW); pinMode(STBY, OUTPUT);
  const uint8_t motorPins[] = {PWMA, AIN1, AIN2, BIN1, BIN2, PWMB};
  for (uint8_t pin : motorPins) {
    digitalWrite(pin, LOW); pinMode(pin, OUTPUT);
  }
  analogWriteResolution(8);
  analogWriteFrequency(PWMA, 20000);
  analogWriteFrequency(PWMB, 20000);
  stopMotor();
  pinMode(PHOTO_ADC, INPUT);
  pinMode(PHOTO_A, INPUT);
  pinMode(PHOTO_B, INPUT);
  pinMode(PHOTO_C, INPUT);
  analogReadResolution(10);
  analogRead(PHOTO_A); analogRead(PHOTO_B); analogRead(PHOTO_C); // calibrate before ISR
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);
  pinMode(EXTERNAL_LED, OUTPUT);
  digitalWrite(EXTERNAL_LED, LOW);
  Serial.begin(115200);
  Serial1.begin(115200);
  Serial1.addMemoryForWrite(streamTxMemory, sizeof streamTxMemory);
}

// Preserve USB disconnect protection; UART has no connection signal and uses
// the same command timeouts (750 ms motors, 3 s LEDs).
void checkWatchdogs() {
  if (jobLedActive && (uint32_t(millis()-jobLedStarted)>=jobLedDuration ||
      uint32_t(millis()-jobLedRefresh)>MOTOR_TIMEOUT_MS)) { stopJobLed(); stopMotor(); }
  if (edgeSessionActive && uint32_t(millis() - edgeLastCommand) > 2000) stopEdges();
  if ((lastMotorCommandViaUsb && !Serial) ||
      uint32_t(millis() - lastMotorCommand) > MOTOR_TIMEOUT_MS) stopMotor();
  if ((lastCommandViaUsb && !Serial) || uint32_t(millis() - lastCommand) > 3000) {
    digitalWrite(LED_BUILTIN, LOW);
    digitalWrite(EXTERNAL_LED, LOW);
  }
}

void readCommands(Stream &port, CommandBuffer &buffer, bool viaUsb) {
  // Bound each pass so continuous traffic cannot starve the other port.
  unsigned budget = 64;
  while (budget-- && port.available()) {
    char c = port.read();
    if (c == '\n') {
      if (!buffer.overflow) {
        buffer.command[buffer.used] = '\0';
        handleCommand(buffer.command, port, viaUsb, clockUs());
      } else {
        stopMotor(); port.println("ERROR command too long");
      }
      buffer.used = 0;
      buffer.overflow = false;
    } else if (c != '\r') {
      if (!buffer.overflow && buffer.used < sizeof(buffer.command) - 1)
        buffer.command[buffer.used++] = c;
      else { buffer.overflow = true; stopMotor(); }
    }
    checkWatchdogs();
  }
}

void loop() {
  clockUs(); // extend micros across wrap, even while idle
  readCommands(Serial, usbBuffer, true);
  readCommands(Serial1, uartBuffer, false);
  transmitSample();
  checkWatchdogs();
}
