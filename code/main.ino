/**
 * @file main.ino
 * @brief Wearable gait-analysis device integrating 3 IMUs, piezo step sensors, GPS, SD logging, and OLED display.
 * 
 * This firmware:
 *  - Reads and calibrates data from three MPU6050 sensors via TCA9548A multiplexer
 *  - Detects left/right foot steps using piezo sensors with strength differentiation
 *  - Filters GPS data and calculates total distance traveled using Haversine formula
 *  - Logs timestamped sensor data to SD card in CSV format
 *  - Displays real-time metrics on 128×64 OLED screen
 *  - Implements sensor fusion and data validation checks
 *
 * Hardware Architecture:
 *  - ESP32 microcontroller
 *  - 3x MPU6050 IMUs through TCA9548A I2C multiplexer
 *  - GPS NEO-6M module
 *  - SD card module (SPI interface)
 *  - 1.3" SH1106 OLED display
 *  - Piezo sensors on analog inputs
 *
 * Dependencies: Adafruit_MPU6050, TinyGPS++, SD, SPI, EEPROM, FS, Wire, Adafruit_SH110X
 * License: MIT
 */

// ====================== Include Libraries ======================
#include <Adafruit_MPU6050.h>  // IMU sensor library
#include <TinyGPS++.h>         // GPS parsing library
#include <SD.h>                // SD card operations
#include <SPI.h>               // SPI communication
#include <EEPROM.h>            // For potential calibration storage
#include "FS.h"                // Filesystem operations
#include <Wire.h>              // I2C communication
#include <Adafruit_SH110X.h>   // OLED display

// ====================== Hardware Configuration ======================
#define TCA_ADDR           0x70    // I2C multiplexer address
#define NUM_IMUS           3       // Number of IMU sensors
const uint8_t IMU_CHANNELS[NUM_IMUS] = {0,1,2};  // Multiplexer channels for IMUs

// IMU Calibration Offsets {AccX, AccY, AccZ, GyroX, GyroY, GyroZ}
const float CALIBRATION_OFFSETS[NUM_IMUS][6] = {
  // Right Leg IMU (empirically determined)
  {0.12-1.2280, -0.15, -0.81+0.0525, 0.005-0.0694, -0.002+0.0585, 0.003-0.0363},
  // Left Leg IMU
  {-0.08-1.1284, 0.15, -0.76+1.4918-1, 0.002-0.0655, 0.004-0.0116, -0.001-0.0273},
  // Chest IMU
  {0.30, -0.02-0.3162, -0.83+1.7669-1, -0.003+0.0201, 0.001-0.0122, 0.002+0.0077}
};

// GPS Configuration
#define RXD2          16        // GPIO16 for GPS RX
#define TXD2          17        // GPIO17 for GPS TX
#define GPS_BAUD      9600      // GPS module baud rate

// SD Card Configuration
#define pinCS         5         // Chip-select pin for SD card

// OLED Configuration
#define OLED_SDA      26        // I2C1 SDA
#define OLED_SCL      25        // I2C1 SCL
#define SCREEN_WIDTH  128       // OLED display resolution
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1        // Reset pin not used

// Piezo Sensor Configuration
const int PIEZO_LEFT_PIN = 33;  // Analog pin for left foot
const int PIEZO_RIGHT_PIN = 2;  // Analog pin for right foot

// Timing Constants (milliseconds)
const unsigned long DEBOUNCE_TIME = 200;        // Minimum step interval
const unsigned long GPS_UPDATE_INTERVAL = 1000; // GPS polling interval
const unsigned long SENSOR_UPDATE_INTERVAL = 100; // IMU sampling rate
const unsigned long PIEZO_UPDATE_INTERVAL = 20; // Piezo sampling rate

// GPS Processing Parameters
const double GPS_FILTER_ALPHA = 0.2;           // Exponential smoothing factor
const double GPS_DISTANCE_THRESHOLD = 50.0;    // Movement threshold in meters

// ====================== Global Objects ======================
Adafruit_MPU6050 imus[NUM_IMUS];    // IMU sensor array
TinyGPSPlus gps;                    // GPS parser object
HardwareSerial gpsSerial(2);        // UART2 for GPS
Adafruit_SH1106G display(SCREEN_WIDTH, SCREEN_HEIGHT, &oledWire, OLED_RESET);
File seminarFile;                   // SD card file handle
TwoWire oledWire = TwoWire(1);      // Separate I2C bus for OLED

// ====================== Data Structures ======================
/**
 * @struct SensorData
 * @brief Container for IMU sensor readings
 * 
 * Stores both raw and calibrated values from accelerometer, gyroscope
 * sensor. Calibration is applied using predefined offsets.
 */
struct SensorData {
  float accel[3];           // Raw accelerometer readings (x,y,z) in m/s²
  float gyro[3];            // Raw gyroscope readings (x,y,z) in rad/s
  float calibratedAccel[3]; // Calibrated accelerometer values
  float calibratedGyro[3];  // Calibrated gyroscope values
};

/**
 * @struct GPSData
 * @brief Container for processed GPS information
 * 
 * Stores current position, speed, and cumulative distance traveled.
 * Implements filtering to reduce GPS noise.
 */
struct GPSData {
  double latitude;         // Current latitude in degrees
  double longitude;        // Current longitude in degrees
  float speed;             // Current speed in km/h
  double totalDistance;    // Cumulative distance in meters
};

/**
 * @struct StepData
 * @brief Container for step detection metrics
 * 
 * Tracks different types of steps detected by piezo sensors:
 * - Normal and strong steps for both feet
 * - Time-based debouncing to prevent false positives
 */
struct StepData {
  uint32_t totalSteps;     // Total steps detected
  uint32_t normal_L_steps; // Left foot normal steps
  uint32_t normal_R_steps; // Right foot normal steps
  uint32_t strong_L_steps; // Left foot strong steps
  uint32_t strong_R_steps; // Right foot strong steps
  unsigned long lastLeftStep;  // Timestamp of last left step
  unsigned long lastRightStep; // Timestamp of last right step
};

// ====================== Global Variables ======================
SensorData imuData[NUM_IMUS];   // Array of IMU data containers
GPSData gpsData = {0, 0, 0, 0}; // GPS data initialized to zero
StepData stepData = {0,0,0,0,0,0,0}; // Step counter initialized

// Timing control variables
unsigned long lastGpsUpdate = 0;     // GPS update timer
unsigned long lastSensorUpdate = 0;  // IMU sampling timer
unsigned long lastPiezoUpdate = 0;   // Piezo sampling timer
unsigned long lastLogTime = 0;       // Data logging timer
bool headerWritten = false;          // CSV header state flag

// ====================== Function Prototypes ===================
double calculateDistance(double lat1, double lon1, double lat2, double lon2);
void selectImuChannel(uint8_t channel);
void readPiezoSensors();
void initializeIMUs();
void readAllIMUs();
void updateGPS();
void printData();
void logData();
void updateDisplay();

// ====================== Core Functions ========================
void setup() {
  Serial.begin(115200);  // Initialize debug serial
  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, RXD2, TXD2); // Start GPS serial
  
  Wire.begin();  // Initialize I2C0 for IMUs and multiplexer
  initializeIMUs();

  // OLED Display Initialization
  oledWire.begin(OLED_SDA, OLED_SCL);
  if(!display.begin(0x3C, true)) {
    Serial.println("OLED initialization failed!");
    while(1); // Halt on critical failure
  }
  display.display(); // Show initial buffer
  delay(1000);

  // SD Card Initialization
  if (!SD.begin(pinCS)) {
    Serial.println("SD card initialization failed!");
    display.println("SD Init Fail!");
    display.display();
  }

  Serial.println("System Initialization Complete");
}

void loop() {
  unsigned long currentTime = millis();

  // GPS Update Task
  if (currentTime - lastGpsUpdate >= GPS_UPDATE_INTERVAL) {
    updateGPS();
    lastGpsUpdate = currentTime;
  }

  // IMU Sampling Task
  if (currentTime - lastSensorUpdate >= SENSOR_UPDATE_INTERVAL) {
    readAllIMUs();
    printData();  // Debug output
    lastSensorUpdate = currentTime;
  }

  // Step Detection Task
  if (currentTime - lastPiezoUpdate >= PIEZO_UPDATE_INTERVAL) {
    readPiezoSensors();
    lastPiezoUpdate = currentTime;
  }

  // Data Logging Task (1Hz)
  if (currentTime - lastLogTime >= 1000) {
    logData();
    lastLogTime = currentTime;
  }

  // Display Update Task (1Hz)
  static unsigned long lastDisplayUpdate = 0;
  if (currentTime - lastDisplayUpdate >= 1000) {
    updateDisplay();
    lastDisplayUpdate = currentTime;
  }

  delay(10);  // Minimal delay for background tasks
}

// ====================== IMU Management Functions ======================
/**
 * @brief Selects channel on I2C multiplexer
 * @param channel Multiplexer channel (0-7)
 * 
 * This function enables communication with the IMU connected to
 * the specified multiplexer channel. Must be called before
 * accessing any IMU.
 */
void selectImuChannel(uint8_t channel) {
  Wire.beginTransmission(TCA_ADDR);
  Wire.write(1 << channel);  // Bitmask for channel selection
  Wire.endTransmission();
}

/**
 * @brief Initializes all IMU sensors
 * 
 * Configures each MPU6050 with appropriate ranges and filter settings.
 * Verifies I2C communication with each sensor.
 */
void initializeIMUs() {
  for (uint8_t i = 0; i < NUM_IMUS; i++) {
    selectImuChannel(IMU_CHANNELS[i]);
    
    if (!imus[i].begin()) {
      Serial.print("IMU "); Serial.print(i); Serial.println(" not found!");
      while(1); // Halt on critical failure
    }
    
    // Configure sensor ranges
    imus[i].setAccelerometerRange(MPU6050_RANGE_8_G);
    imus[i].setGyroRange(MPU6050_RANGE_500_DEG);
    imus[i].setFilterBandwidth(MPU6050_BAND_10_HZ);
  }
}

/**
 * @brief Reads data from all IMUs and applies calibration
 * 
 * Iterates through all IMUs, reads their sensors, and applies
 * predefined calibration offsets. Stores both raw and calibrated
 * values in the imuData array.
 */
void readAllIMUs() {
  for (uint8_t i = 0; i < NUM_IMUS; i++) {
    selectImuChannel(IMU_CHANNELS[i]);
    
    sensors_event_t accel, gyro;
    if (!imus[i].getEvent(&accel, &gyro)) {
      Serial.print("IMU "); Serial.print(i); Serial.println(" read error!");
      continue;
    }

    // Store raw sensor data
    imuData[i].accel[0] = accel.acceleration.x;
    imuData[i].accel[1] = accel.acceleration.y;
    imuData[i].accel[2] = accel.acceleration.z;
    imuData[i].gyro[0] = gyro.gyro.x;
    imuData[i].gyro[1] = gyro.gyro.y;
    imuData[i].gyro[2] = gyro.gyro.z;

    // Apply calibration offsets
    for(int j = 0; j < 3; j++) {
      imuData[i].calibratedAccel[j] = 
        imuData[i].accel[j] - CALIBRATION_OFFSETS[i][j];
      imuData[i].calibratedGyro[j] = 
        imuData[i].gyro[j] - CALIBRATION_OFFSETS[i][j+3];
    }
  }
}

// ====================== GPS Processing Functions ======================
/**
 * @brief Updates GPS data and calculates movement
 * 
 * Reads NMEA sentences from GPS module, parses them using TinyGPS++,
 * and updates position/speed data. Implements distance thresholding
 * to filter out GPS noise and small movements.
 */
void updateGPS() {
  while (gpsSerial.available() > 0) {
    if (gps.encode(gpsSerial.read())) {
      if (gps.location.isValid() && gps.location.isUpdated()) {
        double newLat = gps.location.lat();
        double newLng = gps.location.lng();
        
        if (gpsData.latitude != 0 && gpsData.longitude != 0) {
          double distance = calculateDistance(
            gpsData.latitude, gpsData.longitude, newLat, newLng);
          
          // Update position only if significant movement detected
          if (distance > GPS_DISTANCE_THRESHOLD && 
              gps.speed.isValid() && 
              gps.speed.kmph() > 2.0) {
            gpsData.totalDistance += distance;
            gpsData.latitude = newLat;
            gpsData.longitude = newLng;
          }
        } else {
          // Initial position fix
          gpsData.latitude = newLat;
          gpsData.longitude = newLng;
        }
        
        // Always update speed if valid
        if (gps.speed.isValid()) {
          gpsData.speed = gps.speed.kmph();
        }
      }
    }
  }
}

/**
 * @brief Calculates distance between two coordinates using Haversine formula
 * @return Distance in meters between two points
 */
double calculateDistance(double lat1, double lon1, double lat2, double lon2) {
  const double R = 6371e3; // Earth radius in meters
  double φ1 = lat1 * DEG_TO_RAD;
  double φ2 = lat2 * DEG_TO_RAD;
  double Δφ = (lat2-lat1) * DEG_TO_RAD;
  double Δλ = (lon2-lon1) * DEG_TO_RAD;

  double a = sin(Δφ/2) * sin(Δφ/2) +
             cos(φ1) * cos(φ2) *
             sin(Δλ/2) * sin(Δλ/2);
  double c = 2 * atan2(sqrt(a), sqrt(1-a));
  
  return R * c;
}

// ====================== Step Detection Functions ======================
/**
 * @brief Reads piezo sensors and detects steps
 * 
 * Implements analog threshold detection with temporal debouncing.
 * Differentiates between normal and strong steps based on amplitude.
 * Updates step counters and timestamps.
 */
void readPiezoSensors() {
  unsigned long currentMillis = millis();
  
  // Left foot detection
  int leftValue = analogRead(PIEZO_LEFT_PIN);
  if (leftValue > 600 && (currentMillis - stepData.lastLeftStep) > DEBOUNCE_TIME) {
    if (leftValue < 700) {
      stepData.normal_L_steps++;
    } else {
      stepData.strong_L_steps++;
    }
    stepData.totalSteps++;
    stepData.lastLeftStep = currentMillis;
  }

  // Right foot detection
  int rightValue = analogRead(PIEZO_RIGHT_PIN);
  if (rightValue > 80 && (currentMillis - stepData.lastRightStep) > DEBOUNCE_TIME) {
    if (rightValue < 120) {
      stepData.normal_R_steps++;
    } else {
      stepData.strong_R_steps++;
    }
    stepData.totalSteps++;
    stepData.lastRightStep = currentMillis;
  }
}

// ====================== Data Logging Functions ======================
/**
 * @brief Logs system data to SD card in CSV format
 * 
 * Creates/opens a CSV file and writes sensor readings with timestamp.
 * Implements header writing on first run. Uses buffered writes for
 * better SD card longevity.
 */
void logData() { 
  seminarFile = SD.open("/gait_data.csv", FILE_APPEND);
  
  if (seminarFile) {
    // Write CSV header if first run
    if (!headerWritten) {
      seminarFile.println("timestamp,RL_AX,RL_AY,RL_AZ,RL_GX,RL_GY,RL_GZ,"
                          "LL_AX,LL_AY,LL_AZ,LL_GX,LL_GY,LL_GZ,"
                          "CH_AX,CH_AY,CH_AZ,CH_GX,CH_GY,CH_GZ,"
                          "latitude,longitude,speed_kmh,total_distance,"
                          "total_steps,normal_R,normal_L,strong_R,strong_L");
      headerWritten = true;
    }

    // Write timestamp (seconds since startup)
    seminarFile.print(millis()/1000); seminarFile.print(',');

    // Write IMU data (3 sensors × 6 values each)
    for (int i = 0; i < NUM_IMUS; i++) {
      seminarFile.print(imuData[i].calibratedAccel[0], 2); seminarFile.print(',');
      seminarFile.print(imuData[i].calibratedAccel[1], 2); seminarFile.print(',');
      seminarFile.print(imuData[i].calibratedAccel[2], 2); seminarFile.print(',');
      seminarFile.print(imuData[i].calibratedGyro[0], 2); seminarFile.print(',');
      seminarFile.print(imuData[i].calibratedGyro[1], 2); seminarFile.print(',');
      seminarFile.print(imuData[i].calibratedGyro[2], 2); seminarFile.print(',');
    }

    // Write GPS data
    seminarFile.print(gpsData.latitude, 6); seminarFile.print(',');
    seminarFile.print(gpsData.longitude, 6); seminarFile.print(',');
    seminarFile.print(gpsData.speed, 1); seminarFile.print(',');
    seminarFile.print(gpsData.totalDistance, 2); seminarFile.print(',');

    // Write step data
    seminarFile.print(stepData.totalSteps); seminarFile.print(',');
    seminarFile.print(stepData.normal_R_steps); seminarFile.print(',');
    seminarFile.print(stepData.normal_L_steps); seminarFile.print(',');
    seminarFile.print(stepData.strong_R_steps); seminarFile.print(',');
    seminarFile.println(stepData.strong_L_steps);
    
    seminarFile.close();
  } else {
    Serial.println("Error opening log file!");
  }
}

// ====================== Display Functions ======================
/**
 * @brief Updates OLED display with current system metrics
 * 
 * Formats and displays time, speed, distance, and step count.
 * Implements automatic unit conversion (m/km) based on distance.
 */
void updateDisplay() {
  display.clearDisplay();
  display.setCursor(0,0);
  
  // Display runtime in HH:MM:SS format
  unsigned long totalSeconds = millis()/1000;
  uint8_t hours = totalSeconds / 3600;
  uint8_t mins = (totalSeconds % 3600) / 60;
  uint8_t secs = totalSeconds % 60;
  display.printf("Time: %02d:%02d:%02d\n", hours, mins, secs);
  
  // Display speed and distance
  display.println("------------");
  display.printf("Speed: %.1f km/h\n", gpsData.speed);
  display.println("------------");
  if(gpsData.totalDistance < 1000) {
    display.printf("Dist: %.1fm\n", gpsData.totalDistance);
  } else {
    display.printf("Dist: %.2fkm\n", gpsData.totalDistance/1000);
  }
  
  // Display step count
  display.println("------------");
  display.printf("Steps: %d\n", stepData.totalSteps);
  
  display.display();
}

// ====================== Debug Functions ======================
/**
 * @brief Prints sensor data to serial monitor
 * 
 * Used for debugging purposes. Displays formatted sensor readings
 * and system status information.
 */
void printData() {
  // Print IMU data
  for (uint8_t i = 0; i < NUM_IMUS; i++) {
    Serial.printf("IMU%d:\n", i);
    Serial.printf(" Accel: %.2f,%.2f,%.2f\n", 
      imuData[i].calibratedAccel[0],
      imuData[i].calibratedAccel[1],
      imuData[i].calibratedAccel[2]);
    Serial.printf(" Gyro: %.2f,%.2f,%.2f\n",
      imuData[i].calibratedGyro[0],
      imuData[i].calibratedGyro[1],
      imuData[i].calibratedGyro[2]);
  }

  // Print GPS data
  Serial.printf("Lat: %.6f, Lon: %.6f\n", gpsData.latitude, gpsData.longitude);
  Serial.printf("Speed: %.1f km/h, Dist: %.1f m\n", gpsData.speed, gpsData.totalDistance);

  // Print step data
  Serial.printf("Steps: %d (L:%d/%d R:%d/%d)\n",
    stepData.totalSteps,
    stepData.normal_L_steps, stepData.strong_L_steps,
    stepData.normal_R_steps, stepData.strong_R_steps);
  Serial.println("==================================");
}