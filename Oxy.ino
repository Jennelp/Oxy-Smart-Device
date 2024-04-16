#include <Arduino.h>
#include "Adafruit_CCS811.h"
#include <Wire.h>
#include <WiFi.h>
#include <FirebaseESP32.h>
#include <MQUnifiedsensor.h>
#include <Bonezegei_HCSR04.h>

// Provide the token generation process info.
#include <addons/TokenHelper.h>
#include <addons/RTDBHelper.h>

/* 1. Define the WiFi credentials */
#define WIFI_SSID "ZTE_2.4G_a55nRF"
#define WIFI_PASSWORD "4DQX6s7K"

#define API_KEY "AIzaSyBQD6XsFAs_eZSAN2cMjiTj2VXxp_R89VE"
#define DATABASE_URL "oxy-accounts-default-rtdb.firebaseio.com"

#define USER_EMAIL "oxyd3v@gmail.com"
#define USER_PASSWORD "12345678"

// #define MQ2_PIN 32
#define buzzerPin 2
#define AASPin 15
#define foggerPin 16
#define soakingPin 12
#define exhaustPin 13
#define TRIGGER_PIN 26
#define ECHO_PIN 27

#define NOTE_WARN 550 // C4 note in Hz

#define Board ("ESP-32") 
#define Pin (32)  

#define Type ("MQ-2")
#define Voltage_Resolution (3.3) 
#define ADC_Bit_Resolution (12) 
#define RatioMQ2CleanAir (9.83)

MQUnifiedsensor MQ2(Board, Voltage_Resolution, ADC_Bit_Resolution, Pin, Type);

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

unsigned long sendDataPrevMillis = 0;
unsigned long count = 0;
bool signupOK = false;

int CO = 0;
int TVOC = 0;
int waterLevel = 0;

int liquidLevelPercentage = 0;

int COFromFirebase = 0;
int TVOCFromFirebase = 0;

String GLOBAL_STATE = "";
String INITIAL_DELAY_STATE = "";
String ATOM_STATE = "";  
String SOAKING_STATE = "";
String EXHAUST_STATE = "";

String isOngoing = "";

Adafruit_CCS811 ccs;

// INITIAL STATE WITH BUZZER AND LED
bool buzzerActive = false; // Flag to track if the buzzer is active
unsigned long buzzerStartTime = 0; // Timestamp when the buzzer started
unsigned long buzzerStartTimePattern = 0;
unsigned long buzzerPreviousRead = 0; 
unsigned long buzzerInterval = 2000; 
int buzzerState = 0;

// ATOM AND SANITIZE
bool AAS_Active = false; // Flag to track if the AAS is active
unsigned long AAS_StartTime = 0; // Timestamp when the AAS started

// SOAKING 
bool soaking_Active = false; // Flag to track if the SOAKING is active
unsigned long soaking_StartTime = 0; // Timestamp when the SOAKING started

// EXHAUST
bool exhaust_Active = false; // Flag to track if the AAS is active
unsigned long exhaust_StartTime = 0; // Timestamp when the AAS started

const int ledcChannel = 0; // Choose a suitable LEDC channel (0-15) for your ESP32 model
const int ledcFreq = 2000; // Set the desired frequency in Hz
const int ledcResolution = 8; // Set the LEDC resolution (8, 10, 12, or 15 bits)

Bonezegei_HCSR04 ultrasonic(TRIGGER_PIN, ECHO_PIN);

// TEST VOC BUG
const int numReadings = 10;
int readings[numReadings];
int index = 0;
int total = 0;
int average = 0;

void setup() {
  // put your setup code here, to run once:
  Serial.begin (115200);  
  pinMode(buzzerPin, OUTPUT);
  pinMode(AASPin, OUTPUT);
  pinMode(foggerPin, OUTPUT);
  pinMode(soakingPin, OUTPUT);
  pinMode(exhaustPin, OUTPUT);

  // Initialize the LEDC module for the buzzer
  ledcSetup(ledcChannel, ledcFreq, ledcResolution);
  ledcAttachPin(buzzerPin, ledcChannel);
 
  MQ2.setRegressionMethod(1); //_PPM =  a*ratio^b
  MQ2.setA(36974); MQ2.setB(-3.109); // Configure the equation to to calculate CO concentration

  MQ2.init();

  Serial.print("Calibrating please wait.");
  float calcR0 = 0;
  for(int i = 1; i<=10; i ++)
  {
    MQ2.update(); // Update data, the arduino will read the voltage from the analog pin
    calcR0 += MQ2.calibrate(RatioMQ2CleanAir);
    Serial.print(".");
  }
  MQ2.setR0(calcR0/10);
  Serial.println("  done!.");
  
  if(isinf(calcR0)) {Serial.println("Warning: Conection issue, R0 is infinite (Open circuit detected) please check your wiring and supply"); while(1);}
  if(calcR0 == 0){Serial.println("Warning: Conection issue found, R0 is zero (Analog pin shorts to ground) please check your wiring and supply"); while(1);}
  /*****************************  MQ CAlibration ********************************************/ 

  MQ2.serialDebug(true);
  
  Wire.begin ();   // sda= GPIO_21 /scl= GPIO_22

  if(!ccs.begin()){
    Serial.println("Failed to start sensor! Please check your wiring.");
    while(1);
  }
  
  // Wait for the sensor to be ready
  // delay(1000);
  while(!ccs.available());

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to Wi-Fi");
  while (WiFi.status() != WL_CONNECTED)
  {
    Serial.print(".");
    delay(300);
  }
  Serial.println();
  Serial.print("Connected with IP: ");
  Serial.println(WiFi.localIP());
  Serial.println();

  Serial.printf("Firebase Client v%s\n\n", FIREBASE_CLIENT_VERSION);

  /* Assign the api key (required) */
  config.api_key = API_KEY;

   /* Assign the user sign in credentials */
  auth.user.email = USER_EMAIL;
  auth.user.password = USER_PASSWORD;

  config.database_url = DATABASE_URL;

  /* Sign up */
  if (Firebase.signUp(&config, &auth, "", "")){
    Serial.println("ok");
    signupOK = true;
  }
  else{
    Serial.printf("%s\n", config.signer.signupError.message.c_str());
  }

  /* Assign the callback function for the long running token generation task */
  config.token_status_callback = tokenStatusCallback; //see addons/TokenHelper.h

  Firebase.reconnectWiFi(true);

  // fbdo.setBSSLBufferSize(4096 /* Rx buffer size in bytes from 512 - 16384 */, 1024 /* Tx buffer size in bytes from 512 - 16384 */);

  Firebase.begin(&config, &auth);

  Firebase.setDoubleDigits(5);

}

void getMQData(){
  MQ2.update();
  CO = MQ2.readSensor();
  Serial.print("CO: ");
  Serial.print(CO);
  Serial.println(" ");
}

void getWaterLevel(){
  waterLevel = ultrasonic.getDistance();

  int fullTankLevel = 5;
  int emptyTankLevel = 18;
  
  liquidLevelPercentage = map(waterLevel, fullTankLevel, emptyTankLevel, 100, 0);
  liquidLevelPercentage = constrain(liquidLevelPercentage, 0, 100);

  Serial.print("Water Level: ");
  Serial.print(waterLevel);
  Serial.print(" cm, Percentage: ");
  Serial.print(liquidLevelPercentage);
  Serial.println("%");
}

void loop()
{

  if(INITIAL_DELAY_STATE.equals("ON")){
    buzzerStartTimePattern = millis();
    if(buzzerStartTimePattern - buzzerPreviousRead > buzzerInterval){
      buzzerPreviousRead = buzzerStartTimePattern;
      if (buzzerState==0) buzzerState=1; else buzzerState=0;
      tone(buzzerPin, NOTE_WARN, buzzerInterval);
    }
  } else {
    noTone(buzzerPin);
  }

  if(ATOM_STATE.equals("ON")){
    digitalWrite(AASPin, HIGH);
    digitalWrite(foggerPin, HIGH);
  } else {
    digitalWrite(AASPin, LOW);
    digitalWrite(foggerPin, LOW);
  }

  if(SOAKING_STATE.equals("ON")){
    digitalWrite(soakingPin, HIGH);
  } else {
    digitalWrite(soakingPin, LOW);
  }

  if(isOngoing.equals("YES") && EXHAUST_STATE.equals("OFF")){
    digitalWrite(exhaustPin, LOW);
  } else if (isOngoing.equals("YES") && EXHAUST_STATE.equals("ON")){
    digitalWrite(exhaustPin, HIGH);
  } else if (isOngoing.equals("NO") && EXHAUST_STATE.equals("OFF")){
    digitalWrite(exhaustPin, HIGH);
  }

  if (Firebase.ready() && (millis() - sendDataPrevMillis > 5000 || sendDataPrevMillis == 0)){
    sendDataPrevMillis = millis();

    GLOBAL_STATE = Firebase.getString(fbdo, "Room_1/EXEC-PROCESS/GLOBAL-STATE") ? fbdo.to<const char *>() : "";
    INITIAL_DELAY_STATE = Firebase.getString(fbdo, "Room_1/EXEC-PROCESS/INITIAL-DELAY-STATE") ? fbdo.to<const char *>() : "";
    ATOM_STATE = Firebase.getString(fbdo, "Room_1/EXEC-PROCESS/ATOM-SANI-STATE") ? fbdo.to<const char *>() : "";
    SOAKING_STATE = Firebase.getString(fbdo, "Room_1/EXEC-PROCESS/SOAKING-STATE") ? fbdo.to<const char *>() : "";
    EXHAUST_STATE = Firebase.getString(fbdo, "Room_1/EXEC-PROCESS/EXHAUST-STATE") ? fbdo.to<const char *>() : "";
    isOngoing = Firebase.getString(fbdo, "Room_1/isOngoing") ? fbdo.to<const char *>() : "";

    Serial.printf("Get CO from FB:  %s\n", Firebase.getInt(fbdo, F("/T-CO"), &COFromFirebase) ? String(COFromFirebase).c_str() : fbdo.errorReason().c_str());
    Serial.printf("Get TVOC from FB:  %s\n", Firebase.getInt(fbdo, F("/T-TVOC"), &TVOCFromFirebase) ? String(TVOCFromFirebase).c_str() : fbdo.errorReason().c_str());

    if(CO >= 100 || TVOC >= 100){
      if(isOngoing == "NO"){
        GLOBAL_STATE = Firebase.setString(fbdo, "Room_1/EXEC-PROCESS/GLOBAL-STATE", "ON") ? "ON" : "";
        INITIAL_DELAY_STATE = Firebase.setString(fbdo, "Room_1/EXEC-PROCESS/INITIAL-DELAY-STATE", "ON") ? "ON" : "";

        isOngoing = Firebase.setString(fbdo, "Room_1/isOngoing", "YES") ? "YES" : "";
      }
    }
    
    if(GLOBAL_STATE.equals("ON") && isOngoing == "YES"){
      // EXECUTE THE INITIAL DELAY
      if(INITIAL_DELAY_STATE.equals("ON")){
        if(buzzerActive == false){
          buzzerStartTime = millis();
          buzzerActive = true;
        }
        
        // CHECK IF 1 minute
        if(millis() - buzzerStartTime >= 1 * 60 * 1000){
          buzzerActive = false;
          INITIAL_DELAY_STATE = Firebase.setString(fbdo, "Room_1/EXEC-PROCESS/INITIAL-DELAY-STATE", "OFF") ? "OFF" : "";
          // ON THE ATOM OR SANITIZE
          ATOM_STATE = Firebase.setString(fbdo, "Room_1/EXEC-PROCESS/ATOM-SANI-STATE", "ON") ? "ON" : "";
        }
      }

      // EXECUTE SANITIZE AND ATOMZER
      if(ATOM_STATE.equals("ON")){
        if(AAS_Active == false){
          AAS_StartTime = millis();
          AAS_Active = true;
        }

        if(millis() - AAS_StartTime >= 2 * 60 * 1000){
          AAS_Active = false;
          ATOM_STATE = Firebase.setString(fbdo, "Room_1/EXEC-PROCESS/ATOM-SANI-STATE", "OFF") ? "OFF" : "";

          SOAKING_STATE = Firebase.setString(fbdo, "Room_1/EXEC-PROCESS/SOAKING-STATE", "ON") ? "ON" : "";
        }
      }

      // EXECUTE SOAKING
      if(SOAKING_STATE.equals("ON")){
        if(soaking_Active == false){
          soaking_StartTime = millis();
          soaking_Active = true;
        }

        if(millis() - soaking_StartTime >= 2 * 60 * 1000){
          soaking_Active = false;
          SOAKING_STATE = Firebase.setString(fbdo, "Room_1/EXEC-PROCESS/SOAKING-STATE", "OFF") ? "OFF" : "";

          EXHAUST_STATE = Firebase.setString(fbdo, "Room_1/EXEC-PROCESS/EXHAUST-STATE", "ON") ? "ON" : "";
        }
      }

      // EXECUTE EXHAUST
      if(EXHAUST_STATE.equals("ON")){
        if(exhaust_Active == false){
          exhaust_StartTime = millis();
          exhaust_Active = true;
        }

        if(millis() - exhaust_StartTime >= 2 * 60 * 1000){
          exhaust_Active = false;
          EXHAUST_STATE = Firebase.setString(fbdo, "Room_1/EXEC-PROCESS/EXHAUST-STATE", "OFF") ? "OFF" : "";

          GLOBAL_STATE = Firebase.setString(fbdo, "Room_1/EXEC-PROCESS/GLOBAL-STATE", "OFF") ? "OFF" : "";
          isOngoing = Firebase.setString(fbdo, "Room_1/isOngoing", "NO")? "NO" : "";
        }
      }
    }

    getWaterLevel();

    if(ccs.available()){
      if(!ccs.readData()){
        Serial.print("TVOC: ");
        Serial.print(ccs.getTVOC());
        TVOC = ccs.getTVOC();
        Serial.print(", ");
        getMQData();
        total = total - readings[index];
        total = total + TVOC;
        readings[index] = TVOC;
        index = (index + 1) % numReadings;
        average = total / numReadings;
      } else {
        Serial.println("ERROR!");
      }
    }

    Serial.printf("Set TANK LEVEL: %s\n", Firebase.setInt(fbdo, F("Room_1/TANK-LEVEL"), liquidLevelPercentage) ? "ok" : fbdo.errorReason().c_str());

    Serial.printf("Set CO: %s\n", Firebase.setInt(fbdo, F("Room_1/CO"), CO) ? "ok" : fbdo.errorReason().c_str());

    Serial.printf("Get CO: %s\n", Firebase.getInt(fbdo, F("Room_1/CO")) ? String(fbdo.to<int>()).c_str() : fbdo.errorReason().c_str());
   
    Serial.printf("Set TVOC: %s\n", Firebase.setInt(fbdo, F("Room_1/TVOC"), TVOC) ? "ok" : fbdo.errorReason().c_str());

    Serial.printf("Get TVOC: %s\n", Firebase.getInt(fbdo, F("Room_1/TVOC")) ? String(fbdo.to<int>()).c_str() : fbdo.errorReason().c_str());

    Serial.print("isOngoing: "); 
    Serial.print(isOngoing);
    Serial.println();
    Serial.println();
    Serial.print("TVOC Average: ");
    Serial.print(average);
    Serial.println();

  }
}
