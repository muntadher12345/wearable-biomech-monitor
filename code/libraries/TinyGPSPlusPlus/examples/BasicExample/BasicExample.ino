#include <SoftwareSerial.h>
#include <TinyGPS++.h>

// Create TinyGPS++ object
TinyGPSPlus gps;

// Set up software serial
SoftwareSerial ss(4, 3); // RX, TX (adjust pins as needed)

double lastLat = 0.0;    // Last known latitude
double lastLng = 0.0;    // Last known longitude
double totalDistance = 0.0; // Total distance traveled (in kilometers)

const double DISTANCE_THRESHOLD = 0.0017; // Minimum distance to consider (in degrees, adjust if needed)
const double MIN_MOVEMENT = 0.00001; // Minimum movement (in degrees) to ignore small GPS errors
const unsigned long SPEED_AVG_PERIOD = 10000; // Average speed over 10 seconds

unsigned long lastSpeedTime = 0;
float averageSpeed = 0;

void setup() {
  // Start serial communication
  Serial.begin(9600);
  ss.begin(9600);

  // Wait for GPS fix
  Serial.println("Waiting for GPS fix...");
}

void loop() {
  // Read data from GPS
  while (ss.available() > 0) {
    gps.encode(ss.read());
    
    if (gps.location.isUpdated()) {
      // GPS location updated, get current latitude and longitude
      double currentLat = gps.location.lat();
      double currentLng = gps.location.lng();
      
      // If this is not the first reading, calculate the distance
      if (lastLat != 0.0 && lastLng != 0.0) {
        // Calculate distance from last position to current position
        double distance = calculateDistance(lastLat, lastLng, currentLat, currentLng);
        
        // Only add to total distance if the distance is above the minimum threshold
        if (distance > DISTANCE_THRESHOLD) {
          totalDistance += distance;
          
          // Print the total distance
          Serial.print("Total Distance Traveled (km): ");
          Serial.println(totalDistance, 6);
        }
      }
      
      // Get the speed from GPS in km/h
      float speed = gps.speed.kmph(); // Speed in kilometers per hour

      // Average speed over the defined period
      if (millis() - lastSpeedTime > SPEED_AVG_PERIOD) {
        averageSpeed = speed;
        lastSpeedTime = millis();
      }
Serial.print("Total Distance Traveled (km): ");
          Serial.println(totalDistance, 6);
      // Print the current speed (averaged)
      Serial.print("Current Speed (km/h): ");
      Serial.println(averageSpeed);

      // Update the last known position
      lastLat = currentLat;
      lastLng = currentLng;
    }
  }
}

// Haversine formula to calculate distance between two GPS coordinates
double calculateDistance(double lat1, double lon1, double lat2, double lon2) {
  const double R = 6371.0; // Radius of Earth in kilometers
  double lat1Rad = radians(lat1);
  double lon1Rad = radians(lon1);
  double lat2Rad = radians(lat2);
  double lon2Rad = radians(lon2);
  
  double dlat = lat2Rad - lat1Rad;
  double dlon = lon2Rad - lon1Rad;
  
  double a = sin(dlat / 2) * sin(dlat / 2) + cos(lat1Rad) * cos(lat2Rad) * sin(dlon / 2) * sin(dlon / 2);
  double c = 2 * atan2(sqrt(a), sqrt(1 - a));
  
  return R * c; // Distance in kilometers
}
