#include "storage.h"
#include <LittleFS.h>
#include <Arduino.h>
#include <cstdio>
#include <math.h>

namespace storage {

// Forward declarations for static helpers defined later in the file
static bool parseJsonMagCalib(const char* json, MagCalib& cal);
static bool parseJsonCalib3(const char* json, Calib3& cal);

// Helper: Ensure LittleFS is mounted
static bool ensureLittleFS() {
  if (!LittleFS.begin(true)) {  // true = format if mount fails
    Serial.println("[STORAGE] Error: LittleFS mount failed");
    return false;
  }
  return true;
}

// Helper: Create parent directory for a given file path
static void ensureParentDir(const char* filePath) {
  char dir[64];
  strncpy(dir, filePath, sizeof(dir) - 1);
  dir[sizeof(dir) - 1] = '\0';
  // Find last '/' to isolate directory portion
  char* lastSlash = strrchr(dir, '/');
  if (lastSlash && lastSlash != dir) {
    *lastSlash = '\0';
    LittleFS.mkdir(dir);
  }
}

bool saveMagCalibration(const char* filename, const MagCalib& cal) {
  if (!ensureLittleFS()) {
    return false;
  }

  // Ensure parent directory exists and open file for writing
  ensureParentDir(filename);
  File f = LittleFS.open(filename, FILE_WRITE);
  if (!f) {
    Serial.print("[STORAGE] Error: Could not open ");
    Serial.println(filename);
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
  Serial.println(filename);
  return true;
}

bool loadMagCalibration(const char* filename, MagCalib& cal) {
    Serial.print("[STORAGE] Loading magnetometer calibration from ");
    Serial.println(filename);
  if (!ensureLittleFS()) {
    return false;
  }

  // Check if file exists
  if (!LittleFS.exists(filename)) {
    Serial.print("[STORAGE] File not found: ");
    Serial.println(filename);
    return false;
  }

  // Open file for reading
  File f = LittleFS.open(filename, FILE_READ);
  if (!f) {
    Serial.print("[STORAGE] Error: Could not open ");
    Serial.println(filename);
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
    Serial.println(filename);
  } else {
    Serial.print("[STORAGE] Error parsing JSON from ");
    Serial.println(filename);
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

bool loadMagCalibrationChain(MagCalib& effectiveCal, bool& hasBase, bool& hasMount) {
    hasBase  = false;
    hasMount = false;

    MagCalib base{};
    // Identity defaults
    base.bias = {0, 0, 0};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            base.softIron[i][j] = (i == j) ? 1.0f : 0.0f;

    MagCalib mount{};
    mount.bias = {0, 0, 0};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            mount.softIron[i][j] = (i == j) ? 1.0f : 0.0f;

    // Try to load base calibration
    if (loadMagCalibration(MAG_BASE_FILE, base)) {
        hasBase = true;
        Serial.println("[STORAGE] mag_base.json loaded");
    } else {
        // Fall back to legacy file
        if (loadMagCalibration(MAG_LEGACY_FILE, base)) {
            hasBase = true;
            Serial.println("[STORAGE] Legacy mag_cal.json loaded as base");
        }
    }

    // Try to load mounted correction (only meaningful if base loaded)
    if (hasBase && loadMagCalibration(MAG_MOUNT_FILE, mount)) {
        hasMount = true;
        Serial.println("[STORAGE] mag_mount.json loaded");
    }

    if (!hasBase) {
        Serial.println("[STORAGE] No mag calibration files found — uncalibrated");
        // Return identity (no calibration)
        effectiveCal = base;
        return false;
    }

    if (!hasMount) {
        // Only base: effective cal is just base
        effectiveCal = base;
        return true;
    }

    // Compose: corrected = M_mount * (M_base * (raw - b_base) - b_mount)
    // Rewrite as: corrected = M_mount*M_base * raw - M_mount*M_base*b_base - M_mount*b_mount
    // Define:  M_eff = M_mount * M_base
    //          b_eff = b_base + M_base_inv * b_mount   (so M_eff*(raw-b_eff) = full chain)
    //
    // To pre-compose: we store M_eff and b_eff in a single MagCalib.

    // M_eff = M_mount * M_base  (row-vector convention: result[i][j] = sum_k M_mount[i][k]*M_base[k][j])
    float M_eff[3][3] = {};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            for (int k = 0; k < 3; k++)
                M_eff[i][j] += mount.softIron[i][k] * base.softIron[k][j];

    // b_mount is in base-corrected space.  Convert to raw space:
    // b_eff = b_base + M_base_inv * b_mount
    // Solve M_base * delta = b_mount  →  delta = M_base_inv * b_mount
    // Simple 3×3 Cramer's rule inversion:
    float A[3][3];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            A[i][j] = base.softIron[i][j];

    float det = A[0][0]*(A[1][1]*A[2][2]-A[1][2]*A[2][1])
              - A[0][1]*(A[1][0]*A[2][2]-A[1][2]*A[2][0])
              + A[0][2]*(A[1][0]*A[2][1]-A[1][1]*A[2][0]);

    float bm[3] = { mount.bias.x, mount.bias.y, mount.bias.z };
    float delta[3] = {};
    if (fabsf(det) > 1e-9f) {
        float invDet = 1.0f / det;
        float inv[3][3];
        inv[0][0] =  (A[1][1]*A[2][2] - A[1][2]*A[2][1]) * invDet;
        inv[0][1] = -(A[0][1]*A[2][2] - A[0][2]*A[2][1]) * invDet;
        inv[0][2] =  (A[0][1]*A[1][2] - A[0][2]*A[1][1]) * invDet;
        inv[1][0] = -(A[1][0]*A[2][2] - A[1][2]*A[2][0]) * invDet;
        inv[1][1] =  (A[0][0]*A[2][2] - A[0][2]*A[2][0]) * invDet;
        inv[1][2] = -(A[0][0]*A[1][2] - A[0][2]*A[1][0]) * invDet;
        inv[2][0] =  (A[1][0]*A[2][1] - A[1][1]*A[2][0]) * invDet;
        inv[2][1] = -(A[0][0]*A[2][1] - A[0][1]*A[2][0]) * invDet;
        inv[2][2] =  (A[0][0]*A[1][1] - A[0][1]*A[1][0]) * invDet;
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++)
                delta[i] += inv[i][j] * bm[j];
    }

    effectiveCal.bias.x = base.bias.x + delta[0];
    effectiveCal.bias.y = base.bias.y + delta[1];
    effectiveCal.bias.z = base.bias.z + delta[2];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            effectiveCal.softIron[i][j] = M_eff[i][j];

    Serial.printf("[STORAGE] Composed cal chain: base+mount → b_eff=(%.2f,%.2f,%.2f)\n",
                  effectiveCal.bias.x, effectiveCal.bias.y, effectiveCal.bias.z);
    return true;
}

bool saveCalib3(const char* filename, const Calib3& cal) {
  if (!ensureLittleFS()) {
    return false;
  }

  // Ensure parent directory exists and open file for writing
  ensureParentDir(filename);
  File f = LittleFS.open(filename, FILE_WRITE);
  if (!f) {
    Serial.print("[STORAGE] Error: Could not open ");
    Serial.println(filename);
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
  Serial.println(filename);
  return true;
}

bool loadCalib3(const char* filename, Calib3& cal) {
  if (!ensureLittleFS()) {
    return false;
  }

  // Check if file exists
  if (!LittleFS.exists(filename)) {
    Serial.print("[STORAGE] File not found: ");
    Serial.println(filename);
    return false;
  }

  // Open file for reading
  File f = LittleFS.open(filename, FILE_READ);
  if (!f) {
    Serial.print("[STORAGE] Error: Could not open ");
    Serial.println(filename);
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
    Serial.println(filename);
  } else {
    Serial.print("[STORAGE] Error parsing JSON from ");
    Serial.println(filename);
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
