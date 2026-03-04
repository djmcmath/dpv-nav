#include "storage.h"
#include <SPIFFS.h>
#include <Arduino.h>
#include <cstdio>

namespace storage {

// Helper: Ensure SPIFFS is mounted
static bool ensureSPIFFS() {
  if (!SPIFFS.begin(true)) {  // true = format if mount fails
    Serial.println("[STORAGE] Error: SPIFFS mount failed");
    return false;
  }
  return true;
}

// Helper: Create directory structure if needed (SPIFFS doesn't have true dirs, but we use path prefixes)
static void ensureDir(const char* dirPath) {
  // SPIFFS doesn't require directory creation, just use path prefixes
  // Verify a known file in the directory exists or will be created
  (void)dirPath;  // Suppress unused warning
}

bool saveMagCalibration(const char* filename, const MagCalib& cal) {
  if (!ensureSPIFFS()) {
    return false;
  }

  // Build full path
  char fullPath[64] = "/";
  strncat(fullPath, filename, sizeof(fullPath) - strlen(fullPath) - 1);

  // Open file for writing
  File f = SPIFFS.open(fullPath, FILE_WRITE);
  //FILE* f = fopen(fullPath, "w");
  if (!f) {
    Serial.print("[STORAGE] Error: Could not open ");
    Serial.println(fullPath);
    return false;
  }

  // Write JSON
  f.printf("{\n");
  f.printf("  \"type\": \"MagCalib\",\n");
  f.printf("  \"bias\": {\n");
  f.printf("    \"x\": %.6f,\n", cal.bias.x);
  f.printf("    \"y\": %.6f,\n", cal.bias.y);
  f.printf("    \"z\": %.6f\n", cal.bias.z);
  f.printf("  },\n");
  f.printf("  \"softIron\": [\n");
  for (int i = 0; i < 3; i++) {
    f.printf("    [%.6f, %.6f, %.6f]", cal.softIron[i][0], cal.softIron[i][1], cal.softIron[i][2]);
    if (i < 2) f.printf(",");
    f.printf("\n");
  }
  f.printf("  ]\n");
  f.printf("}\n");

  f.close();

  Serial.print("[STORAGE] Saved magnetometer calibration to ");
  Serial.println(fullPath);
  return true;
}

bool loadMagCalibration(const char* filename, MagCalib& cal) {
    Serial.print("[STORAGE] Loading magnetometer calibration from ");
    Serial.println(filename);
  if (!ensureSPIFFS()) {
    return false;
  }

  // Build full path
  char fullPath[64] = "/";
  strncat(fullPath, filename, sizeof(fullPath) - strlen(fullPath) - 1);

  //Serial.print("[STORAGE] Full path: ");
  //Serial.println(fullPath);

  // Check if file exists
  if (!SPIFFS.exists(fullPath)) {
    Serial.print("[STORAGE] File not found: ");
    Serial.println(fullPath);
    return false;
  } //else {
    //Serial.print("[STORAGE] File found: ");
    //Serial.println(fullPath);
  //}

  // Open file for reading
  File f = SPIFFS.open(fullPath, FILE_READ);
  if (!f) {
    Serial.print("[STORAGE] Error: Could not open ");
    Serial.println(fullPath);
    return false;
  } //else {
   // Serial.print("[STORAGE] Successfully opened ");
    //Serial.println(fullPath);
  //}

  // Read file into buffer
  f.seek(0, SeekMode::SeekEnd);
  long fsize = f.position();
  f.seek(0, SeekMode::SeekSet);

  //Serial.print("[STORAGE] File size: ");
  //Serial.print(fsize);
  //Serial.println(" bytes");

  if (fsize <= 0 || fsize > 2048) {
    Serial.print("[STORAGE] Invalid file size: ");
    Serial.println(fsize);
    f.close();
    return false;
  }

  char* buffer = (char*)malloc(fsize + 1);
  if (!buffer) {
    Serial.println("[STORAGE] Error: Could not allocate buffer");
    f.close();
    return false;
  }

  size_t read = f.readBytes(buffer, fsize);
  f.close();
  buffer[read] = '\0';

  //Serial.print("[STORAGE] Read ");  Serial.print(read);
  //Serial.println(" bytes from file");   
  Serial.println("[STORAGE] File content:");
  Serial.println(buffer);

  // Simple JSON parsing (manual, no dependency on ArduinoJson)
  bool success = parseJsonMagCalib(buffer, cal);
  free(buffer);

  if (success) {
    Serial.print("[STORAGE] Loaded magnetometer calibration from ");
    Serial.println(fullPath);
  } else {
    Serial.print("[STORAGE] Error parsing JSON from ");
    Serial.println(fullPath);
  }

  return success;
}

// Helper function: Parse magnetometer calibration JSON
static bool parseJsonMagCalib(const char* json, MagCalib& cal) {
  //Serial.println("[STORAGE] Parsing magnetometer calibration JSON...");
  
  if (!json) {
    Serial.println("[STORAGE] ERROR: json pointer is NULL");
    return false;
  }
  
  //Serial.print("[STORAGE] JSON buffer length: ");
  //Serial.println(strlen(json));
  
  // Initialize to defaults
  cal.bias = {0, 0, 0};
  cal.softIron[0][0] = 1; cal.softIron[0][1] = 0; cal.softIron[0][2] = 0;
  cal.softIron[1][0] = 0; cal.softIron[1][1] = 1; cal.softIron[1][2] = 0;
  cal.softIron[2][0] = 0; cal.softIron[2][1] = 0; cal.softIron[2][2] = 1;
  //Serial.println("[STORAGE] Initialized defaults");

  // Very simple JSON parser - look for key patterns
  const char* ptr = json;

  // Find "bias"
  //Serial.println("[STORAGE] Searching for \"bias\"...");
  const char* biasStart = strstr(ptr, "\"bias\"");
  if (!biasStart) {
    Serial.println("[STORAGE] ERROR: Could not find \"bias\" in JSON");
    return false;
  }
  //Serial.println("[STORAGE] Found \"bias\"");

  // Parse bias.x
  //Serial.println("[STORAGE] Searching for bias.x...");
  const char* xStr = strstr(biasStart, "\"x\"");
  if (!xStr) {
    Serial.println("[STORAGE] ERROR: Could not find bias.x in JSON");
    return false;
  }
  const char* colonPtr = strstr(xStr, ":");
  if (!colonPtr) {
    Serial.println("[STORAGE] ERROR: Could not find ':' after x key");
    return false;
  }
  cal.bias.x = atof(colonPtr + 1);
  //Serial.print("[STORAGE] Parsed bias.x = ");
  //Serial.println(cal.bias.x);

  // Parse bias.y
  //Serial.println("[STORAGE] Searching for bias.y...");
  const char* yStr = strstr(xStr, "\"y\"");
  if (!yStr) {
    Serial.println("[STORAGE] ERROR: Could not find bias.y in JSON");
    return false;
  }
  colonPtr = strstr(yStr, ":");
  if (!colonPtr) {
    Serial.println("[STORAGE] ERROR: Could not find ':' after y key");
    return false;
  }
  cal.bias.y = atof(colonPtr + 1);
  //Serial.print("[STORAGE] Parsed bias.y = ");
  //Serial.println(cal.bias.y);

  // Parse bias.z
  //Serial.println("[STORAGE] Searching for bias.z...");
  const char* zStr = strstr(yStr, "\"z\"");
  if (!zStr) {
    Serial.println("[STORAGE] ERROR: Could not find bias.z in JSON");
    return false;
  }
  colonPtr = strstr(zStr, ":");
  if (!colonPtr) {
    Serial.println("[STORAGE] ERROR: Could not find ':' after z key");
    return false;
  }
  cal.bias.z = atof(colonPtr + 1);
  //Serial.print("[STORAGE] Parsed bias.z = ");
  //Serial.println(cal.bias.z);

  // Parse softIron matrix (3x3)
  //Serial.println("[STORAGE] Searching for \"softIron\"...");
  const char* matStart = strstr(zStr, "\"softIron\"");
  if (!matStart) {
    Serial.println("[STORAGE] ERROR: Could not find \"softIron\" in JSON");
    return false;
  }
  //Serial.println("[STORAGE] Found \"softIron\"");

  //Serial.println("[STORAGE] Searching for array start...");
  const char* arrayStart = strstr(matStart, "[");
  if (!arrayStart) {
    Serial.println("[STORAGE] ERROR: Could not find '[' for softIron array");
    return false;
  }
  //Serial.println("[STORAGE] Found array start");

  // Parse 3 rows
  ptr = arrayStart;
  for (int row = 0; row < 3; row++) {
    //Serial.print("[STORAGE] Parsing softIron row ");
    //Serial.println(row);
    
    // Find opening bracket for this row
    ptr = strstr(ptr, "[");
    if (!ptr) {
      Serial.print("[STORAGE] ERROR: Could not find '[' for row ");
      Serial.println(row);
      return false;
    }
    ptr++;  // skip outer opening '['
    
    // Skip whitespace to inner bracket
    while (*ptr && (*ptr == ' ' || *ptr == '\n' || *ptr == '\r' || *ptr == '\t')) {
      ptr++;
    }
    
    // Skip inner opening bracket
    if (*ptr == '[') {
      ptr++;
    }
    
    // Skip whitespace after inner bracket
    while (*ptr && (*ptr == ' ' || *ptr == '\n' || *ptr == '\r' || *ptr == '\t')) {
      ptr++;
    }
    
    //Serial.print("[STORAGE] Found row ");
    //Serial.print(row);
    //Serial.println(" start");

    for (int col = 0; col < 3; col++) {
      /*Serial.print("[STORAGE] Parsing softIron[");
      Serial.print(row);
      Serial.print("][");
      Serial.print(col);
      Serial.println("]");*/
      
      cal.softIron[row][col] = (float)atof(ptr);
      /*Serial.print("[STORAGE] softIron[");
      Serial.print(row);
      Serial.print("][");
      Serial.print(col);
      Serial.print("] = ");
      Serial.println(cal.softIron[row][col]);*/
      
      // Move to next number
      if (col < 2) {  // Not the last column, must find comma
        const char* commaPtr = strstr(ptr, ",");
        if (!commaPtr) {
          /*Serial.print("[STORAGE] ERROR: Could not find ',' after softIron[");
          Serial.print(row);
          Serial.print("][");
          Serial.print(col);
          Serial.println("]");*/
          return false;
        }
        ptr = commaPtr + 1;  // Advance past comma
      }
      // For last column (col=2), don't look for comma; row ends with ]
    }

    ptr = strstr(ptr, "]");
    if (!ptr) {
      Serial.print("[STORAGE] ERROR: Could not find ']' to close row ");
      Serial.println(row);
      return false;
    }
    //Serial.print("[STORAGE] Completed row ");
    //Serial.println(row);
  }

  //Serial.println("[STORAGE] Successfully parsed magnetometer calibration JSON");
  return true;
}

bool saveCalib3(const char* filename, const Calib3& cal) {
  if (!ensureSPIFFS()) {
    return false;
  }

  // Build full path
  char fullPath[64] = "/";
  strncat(fullPath, filename, sizeof(fullPath) - strlen(fullPath) - 1);

  // Open file for writing
  File f = SPIFFS.open(fullPath, FILE_WRITE);
  if (!f) {
    Serial.print("[STORAGE] Error: Could not open ");
    Serial.println(fullPath);
    return false;
  }

  // Write JSON
  f.printf("{\n");
  f.printf("  \"type\": \"Calib3\",\n");
  f.printf("  \"bias\": {\n");
  f.printf("    \"x\": %.6f,\n", cal.bias.x);
  f.printf("    \"y\": %.6f,\n", cal.bias.y);
  f.printf("    \"z\": %.6f\n", cal.bias.z);
  f.printf("  },\n");
  f.printf("  \"scale\": {\n");
  f.printf("    \"x\": %.6f,\n", cal.scale.x);
  f.printf("    \"y\": %.6f,\n", cal.scale.y);
  f.printf("    \"z\": %.6f\n", cal.scale.z);
  f.printf("  }\n");
  f.printf("}\n");

  f.close();

  Serial.print("[STORAGE] Saved Calib3 to ");
  Serial.println(fullPath);
  return true;
}

bool loadCalib3(const char* filename, Calib3& cal) {
  if (!ensureSPIFFS()) {
    return false;
  }

  // Build full path
  char fullPath[64] = "/";
  strncat(fullPath, filename, sizeof(fullPath) - strlen(fullPath) - 1);

  // Check if file exists
  if (!SPIFFS.exists(fullPath)) {
    Serial.print("[STORAGE] File not found: ");
    Serial.println(fullPath);
    return false;
  }

  // Open file for reading
  File f = SPIFFS.open(fullPath, FILE_READ);
  if (!f) {
    Serial.print("[STORAGE] Error: Could not open ");
    Serial.println(fullPath);
    return false;
  }

  // Read file into buffer
  f.seek(0, SeekMode::SeekEnd);
  long fsize = f.position();
  f.seek(0, SeekMode::SeekSet);

  if (fsize <= 0 || fsize > 2048) {
    Serial.print("[STORAGE] Invalid file size: ");
    Serial.println(fsize);
    f.close();
    return false;
  }

  char* buffer = (char*)malloc(fsize + 1);
  if (!buffer) {
    Serial.println("[STORAGE] Error: Could not allocate buffer");
    f.close();
    return false;
  }

  size_t read = f.readBytes(buffer, fsize);
  f.close();
  buffer[read] = '\0';

  // Simple JSON parsing
  bool success = parseJsonCalib3(buffer, cal);
  free(buffer);

  if (success) {
    Serial.print("[STORAGE] Loaded Calib3 from ");
    Serial.println(fullPath);
  } else {
    Serial.print("[STORAGE] Error parsing JSON from ");
    Serial.println(fullPath);
  }

  return success;
}

// Helper function: Parse Calib3 JSON
static bool parseJsonCalib3(const char* json, Calib3& cal) {
  // Initialize to defaults
  cal.bias = {0, 0, 0};
  cal.scale = {1, 1, 1};

  // Very simple JSON parser
  const char* ptr = json;

  // Find "bias"
  const char* biasStart = strstr(ptr, "\"bias\"");
  if (!biasStart) return false;

  // Parse bias.x
  const char* xStr = strstr(biasStart, "\"x\"");
  if (!xStr) return false;
  cal.bias.x = atof(strstr(xStr, ":") + 1);

  // Parse bias.y
  const char* yStr = strstr(xStr, "\"y\"");
  if (!yStr) return false;
  cal.bias.y = atof(strstr(yStr, ":") + 1);

  // Parse bias.z
  const char* zStr = strstr(yStr, "\"z\"");
  if (!zStr) return false;
  cal.bias.z = atof(strstr(zStr, ":") + 1);

  // Find "scale"
  const char* scaleStart = strstr(zStr, "\"scale\"");
  if (!scaleStart) return false;

  // Parse scale.x
  xStr = strstr(scaleStart, "\"x\"");
  if (!xStr) return false;
  cal.scale.x = atof(strstr(xStr, ":") + 1);

  // Parse scale.y
  yStr = strstr(xStr, "\"y\"");
  if (!yStr) return false;
  cal.scale.y = atof(strstr(yStr, ":") + 1);

  // Parse scale.z
  zStr = strstr(yStr, "\"z\"");
  if (!zStr) return false;
  cal.scale.z = atof(strstr(zStr, ":") + 1);

  return true;
}

}  // namespace storage
