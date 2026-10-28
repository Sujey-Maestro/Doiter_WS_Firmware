#include <SPI.h>
#include <Ethernet.h>
#include <Wire.h>
 
// ---------------- WAVESHARE BOARD PINS ----------------
 
// Door sensors: Digital Input 1 and Digital Input 2
#define DOOR1 4
#define DOOR2 5
 
// W5500 Ethernet
#define ETH_CS   16
#define ETH_SCLK 15
#define ETH_MISO 14
#define ETH_MOSI 13
#define ETH_RST  39
 
// External status LEDs (connect each LED through a suitable resistor)
#define RED_LED_PIN   6
#define GREEN_LED_PIN 7
 
// ---------------- DIGITAL OUTPUT CONTROLLER ----------------
 
// TCA9554 I/O expander
#define TCA_ADDR       0x20
#define TCA_OUTPUT_REG 0x01
#define TCA_CONFIG_REG 0x03
 
// EXIO1 = P0, EXIO2 = P1
#define LOCK1_MASK 0x01
#define LOCK2_MASK 0x02
 
uint8_t outputState = 0;
 
// ---------------- NETWORK ----------------
 
// Change the IP and MAC for the second device.
byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };
IPAddress ip(192, 168, 1, 76);
 
EthernetServer server(5111);
EthernetClient client;
 
// ---------------- TIMING ----------------
 
const unsigned long LOCK_PULSE_TIMER1 = 2000;
const unsigned long LOCK_PULSE_TIMER2 = 5000;
 
const unsigned long IDLE_TIME_BEFORE_ALIVE = 60000;
const unsigned long ALIVE_INTERVAL = 30000;
 
// ---------------- ALIVE STATE ----------------
 
unsigned long lastRxTime = 0;
unsigned long aliveStartTime = 0;
unsigned long lastAliveTime = 0;
 
bool aliveActive = false;
 
// ---------------- DOOR STATE ----------------
 
bool door1State = false;
bool door2State = false;
 
bool prevDoor1State = false;
bool prevDoor2State = false;
 
bool redFlag = false;
bool greenFlag = false;
 
bool handshakeComplete = false;
bool initialStateSent = false;
 
// ---------------- LOCK TIMERS ----------------
 
unsigned long lock1Timer = 0;
unsigned long lock2Timer = 0;
 
bool lock1Active = false;
bool lock2Active = false;
 
// ---------------- FUNCTION DECLARATIONS ----------------
 
void writeTCA(uint8_t reg, uint8_t value);
void setOutput(uint8_t mask, bool on);
void pulseLock1();
void pulseLock2();
void handleTimers();
void handleClient();
void handleIdleAlive();
void sendMsg(const String &msg);
void sendDoorState(int door, bool closed);
void sendInitialDoorState();
bool isDoorClosed(int doorPin);
void setStatusLED(bool red, bool green);
 
// ---------------- TCA9554 OUTPUT CONTROL ----------------
 
void writeTCA(uint8_t reg, uint8_t value)
{
    Wire.beginTransmission(TCA_ADDR);
    Wire.write(reg);
    Wire.write(value);
    uint8_t err = Wire.endTransmission();
    if (err != 0)
    {
        Serial.print("[WARN] TCA9554 write error: ");
        Serial.println(err);
    }
}
 
void setOutput(uint8_t mask, bool on)
{
    if (on)
        outputState |= mask;
    else
        outputState &= ~mask;
 
    writeTCA(TCA_OUTPUT_REG, outputState);
}
 
// ---------------- LOCK CONTROL ----------------
 
void pulseLock1()
{
    setOutput(LOCK1_MASK, true);
    lock1Timer = millis();
    lock1Active = true;
}
 
void pulseLock2()
{
    setOutput(LOCK2_MASK, true);
    lock2Timer = millis();
    lock2Active = true;
}
 
// ---------------- EXTERNAL STATUS LEDs ----------------
 
void setStatusLED(bool red, bool green)
{
    digitalWrite(RED_LED_PIN, red ? HIGH : LOW);
    digitalWrite(GREEN_LED_PIN, green ? HIGH : LOW);
}
 
// ---------------- SETUP ----------------
 
void setup()
{
    Serial.begin(115200);
 
    // Give Native USB CDC time to connect with the PC host
    unsigned long startWait = millis();
    while (!Serial && (millis() - startWait < 3000))
    {
        delay(10);
    }
    delay(500);
 
    Serial.println();
    Serial.println("=========================================");
    Serial.println("  ESP32-S3 PoE System Starting Up...     ");
    Serial.println("=========================================");
 
    // Door inputs
    pinMode(DOOR1, INPUT);
    pinMode(DOOR2, INPUT);
 
    // External status LEDs
    pinMode(RED_LED_PIN, OUTPUT);
    pinMode(GREEN_LED_PIN, OUTPUT);
    setStatusLED(false, false);
 
    // I2C for TCA9554
    Serial.println("[INIT] Starting I2C (SDA=42, SCL=41)...");
    Wire.begin(42, 41);
 
    // Configure TCA9554 pins as outputs.
    writeTCA(TCA_CONFIG_REG, 0x00);
 
    // Initialize all outputs OFF
    outputState = 0;
    writeTCA(TCA_OUTPUT_REG, outputState);
 
    // Reset W5500
    Serial.println("[INIT] Resetting W5500 Ethernet...");
    pinMode(ETH_RST, OUTPUT);
    digitalWrite(ETH_RST, LOW);
    delay(100);
    digitalWrite(ETH_RST, HIGH);
    delay(200);
 
    // Initialize Ethernet SPI
    Serial.println("[INIT] Initializing Ethernet SPI...");
    SPI.begin(ETH_SCLK, ETH_MISO, ETH_MOSI, ETH_CS);
    Ethernet.init(ETH_CS);
 
    // Static IP
    Serial.print("[INIT] Configuring static IP: ");
    Serial.println(ip);
    Ethernet.begin(mac, ip);
 
    delay(500);
 
    if (Ethernet.hardwareStatus() == EthernetNoHardware)
    {
        Serial.println("[ERROR] W5500 Ethernet controller not found! Check SPI connections.");
    }
    else if (Ethernet.linkStatus() == LinkOFF)
    {
        Serial.println("[WARN] Ethernet cable is not connected (Link OFF).");
    }
    else
    {
        Serial.println("[OK] Ethernet cable connected (Link ON).");
    }
 
    server.begin();
    Serial.print("[OK] Server listening on port 5111, IP: ");
    Serial.println(Ethernet.localIP());
 
    prevDoor1State = digitalRead(DOOR1);
    prevDoor2State = digitalRead(DOOR2);
 
    door1State = prevDoor1State;
    door2State = prevDoor2State;
 
    Serial.println("=========================================");
    Serial.println("  System Ready!                          ");
    Serial.println("=========================================");
}
 
// ---------------- MAIN LOOP ----------------
 
void loop()
{
    static unsigned long lastHeartbeat = 0;
    if (millis() - lastHeartbeat >= 10000)
    {
        lastHeartbeat = millis();
        if (!client || !client.connected())
        {
            Serial.println("[Heartbeat] Waiting for client connection on port 5111...");
        }
    }
    if (!client || !client.connected())
    {
        EthernetClient newClient = server.available();
 
        if (newClient)
        {
            client = newClient;
 
            handshakeComplete = false;
            initialStateSent = false;
            aliveActive = false;
            lastRxTime = millis();
 
            Serial.println("Client connected");
        }
    }
 
    if (client && client.connected())
    {
        handleClient();
        handleIdleAlive();
    }
 
    handleTimers();
}
 
// ---------------- CLIENT HANDLING ----------------
 
void handleClient()
{
    door1State = digitalRead(DOOR1);
    door2State = digitalRead(DOOR2);
 
    if (client.available())
    {
        String cmd = client.readStringUntil('\n');
        cmd.trim();
 
        Serial.println("RX: " + cmd);
 
        lastRxTime = millis();
        aliveActive = false;
 
        // HANDSHAKE
        if (cmd == "MAE" && !handshakeComplete)
        {
            sendMsg("MAE WELCOME");
            handshakeComplete = true;
            initialStateSent = false;
            Serial.println("MAE WELCOME");
        }
 
        // OPEN DOORS
        else if (cmd == "1DoorOpen")
        {
            sendMsg("1DoorOpened");
            pulseLock1();
        }
        else if (cmd == "2DoorOpen")
        {
            sendMsg("2DoorOpened");
            pulseLock2();
        }
        else if (cmd == "3DoorOpen")
        {
            sendMsg("3DoorOpened");
            pulseLock1();
            pulseLock2();
        }
 
        // CLOSE DOORS
        else if (cmd == "1DoorClose")
        {
            setOutput(LOCK1_MASK, false);
            lock1Active = false;
            lock1Timer = 0;
 
            if (!isDoorClosed(DOOR1))
                sendMsg("1DoorClosed");
        }
        else if (cmd == "2DoorClose")
        {
            setOutput(LOCK2_MASK, false);
            lock2Active = false;
            lock2Timer = 0;
 
            if (!isDoorClosed(DOOR2))
                sendMsg("2DoorClosed");
        }
        else if (cmd == "3DoorClose")
        {
            setOutput(LOCK1_MASK, false);
            setOutput(LOCK2_MASK, false);
 
            lock1Active = false;
            lock2Active = false;
            lock1Timer = 0;
            lock2Timer = 0;
 
            if (!isDoorClosed(DOOR1) &&
                !isDoorClosed(DOOR2))
            {
                sendMsg("3DoorClosed");
            }
        }
 
        // STATUS
        else if (cmd == "status")
        {
            int sta1 = digitalRead(DOOR1);
            int sta2 = digitalRead(DOOR2);
 
            if (sta1 == LOW && sta2 == LOW)
            {
                sendMsg("3DoorOpened");
            }
            else if (sta1 == HIGH && sta2 == LOW)
            {
                sendMsg("1DoorClosed");
                sendMsg("2DoorOpened");
            }
            else if (sta1 == LOW && sta2 == HIGH)
            {
                sendMsg("1DoorOpened");
                sendMsg("2DoorClosed");
            }
            else
            {
                sendMsg("3DoorClosed");
            }
        }
 
        // STATUS LED
        else if (cmd == "OK")
        {
            setStatusLED(false, true);
            pulseLock2();
            greenFlag = true;
            redFlag =  false;
        }
        else if (cmd.indexOf("REJ") != -1)
        {
            setStatusLED(true, false);
            pulseLock1();
            redFlag = true;
            greenFlag = false;
        }
    }
 
    if (handshakeComplete && !initialStateSent)
    {
        sendInitialDoorState();
        initialStateSent = true;
    }
 
    // Refresh input readings
    door1State = digitalRead(DOOR1);
    door2State = digitalRead(DOOR2);
 
    if (door1State != prevDoor1State)
    {
        prevDoor1State = door1State;
        sendDoorState(1, door1State);
 
        if (redFlag && door1State == HIGH)
        {
            redFlag = false;
            (digitalWrite(RED_LED_PIN, LOW));

        }
    }
 
    if (door2State != prevDoor2State)
    {
        prevDoor2State = door2State;
        sendDoorState(2, door2State);
 
        if (greenFlag && door2State == HIGH)
        {
            greenFlag = false;
            digitalWrite(GREEN_LED_PIN, LOW);
        }
    }
 
    if (!client.connected())
    {
        client.stop();
        aliveActive = false;
        Serial.println("Client disconnected");
    }
}
 
// ---------------- ALIVE ----------------
 
void handleIdleAlive()
{
    if (!client || !client.connected())
        return;
 
    unsigned long now = millis();
 
    if (!aliveActive &&
        now - lastRxTime >= IDLE_TIME_BEFORE_ALIVE)
    {
        aliveActive = true;
        aliveStartTime = now;
        lastAliveTime = 0;
 
        Serial.println("Alive cycle started");
    }
 
    if (aliveActive)
    {
        if (now - aliveStartTime >= IDLE_TIME_BEFORE_ALIVE)
        {
            aliveActive = false;
            Serial.println("Alive cycle stopped");
            return;
        }
 
        if (now - lastAliveTime >= ALIVE_INTERVAL)
        {
            client.println("Alive");
            Serial.println("TX: Alive");
            lastAliveTime = now;
        }
    }
}
 
// ---------------- TIMERS ----------------
 
void handleTimers()
{
    unsigned long now = millis();
 
    if (lock1Active &&
        now - lock1Timer >= LOCK_PULSE_TIMER1)
    {
        setOutput(LOCK1_MASK, false);
        lock1Active = false;
        lock1Timer = 0;
    }
 
    if (lock2Active &&
        now - lock2Timer >= LOCK_PULSE_TIMER2)
    {
        setOutput(LOCK2_MASK, false);
        lock2Active = false;
        lock2Timer = 0;
    }
}
 
// ---------------- HELPERS ----------------
 
void sendMsg(const String &msg)
{
    if (client && client.connected())
    {
        client.println(msg);
        Serial.println("TX: " + msg);
    }
}
 
void sendDoorState(int door, bool closed)
{
    sendMsg(
        String(door) + "Door" +
        (closed ? "Closed" : "Opened")
    );
}
 
void sendInitialDoorState()
{
    if (prevDoor1State && prevDoor2State)
        sendMsg("3DoorClosed");
    else if (!prevDoor1State && !prevDoor2State)
        sendMsg("3DoorOpened");
    else if (!prevDoor1State && prevDoor2State)
        sendMsg("1DoorOpened");
    else
        sendMsg("2DoorOpened");
}
 
bool isDoorClosed(int doorPin)
{
    return digitalRead(doorPin) == HIGH;
}
