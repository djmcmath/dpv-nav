#include "web_server.h"
#include <WebServer.h>
#include <WiFi.h>
#include <LittleFS.h>

namespace web {

static WebServer server(80);

// --------------- embedded HTML page ---------------

static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>DPV-Nav Files</title>
<style>
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body { font-family: system-ui, sans-serif; background: #1a1a2e; color: #e0e0e0; padding: 1rem; }
  h1 { margin-bottom: .5rem; color: #4cc9f0; }
  .info { color: #aaa; margin-bottom: 1rem; font-size: .9rem; }
  table { width: 100%; border-collapse: collapse; margin-bottom: 1.5rem; }
  th, td { text-align: left; padding: .5rem .75rem; border-bottom: 1px solid #333; }
  th { color: #4cc9f0; }
  a { color: #72efdd; text-decoration: none; }
  a:hover { text-decoration: underline; }
  .btn { display: inline-block; padding: .3rem .7rem; border: none; border-radius: 4px;
         cursor: pointer; font-size: .85rem; color: #fff; }
  .btn-dl  { background: #0077b6; }
  .btn-del { background: #e63946; }
  .btn-dl:hover  { background: #0096c7; }
  .btn-del:hover { background: #ff4d5a; }
  .upload { margin-top: 1rem; padding: 1rem; background: #16213e; border-radius: 8px; }
  .upload input[type=file] { margin: .5rem 0; }
  .upload button { background: #4cc9f0; color: #1a1a2e; border: none; padding: .4rem 1rem;
                   border-radius: 4px; cursor: pointer; font-weight: bold; }
  .upload button:hover { background: #72efdd; }
  #status { margin-top: .5rem; font-size: .9rem; color: #72efdd; }
  .waypoint { margin-top: 1.5rem; padding: 1rem; background: #16213e; border-radius: 8px; }
  .waypoint label { display: inline-block; width: 3rem; }
  .waypoint input[type=number] { width: 10rem; padding: .3rem .5rem; border: 1px solid #333;
    border-radius: 4px; background: #1a1a2e; color: #e0e0e0; font-size: .95rem; }
  .waypoint .row { margin: .4rem 0; }
  .waypoint button { background: #4cc9f0; color: #1a1a2e; border: none; padding: .4rem 1rem;
    border-radius: 4px; cursor: pointer; font-weight: bold; margin-right: .5rem; }
  .waypoint button:hover { background: #72efdd; }
  .waypoint .btn-clear { background: #e63946; color: #fff; }
  .waypoint .btn-clear:hover { background: #ff4d5a; }
  #wpstatus { margin-top: .5rem; font-size: .9rem; color: #72efdd; }
</style>
</head>
<body>
<h1>DPV-Nav File Manager</h1>
<div class="info" id="fsinfo">Loading...</div>
<table>
  <thead><tr><th>File</th><th>Size</th><th></th><th></th></tr></thead>
  <tbody id="files"><tr><td colspan="4">Loading...</td></tr></tbody>
</table>
<div class="upload">
  <b>Upload File</b>
  <form id="upform">
    <input type="file" id="upfile" required><br>
    <button type="submit">Upload</button>
  </form>
  <div id="status"></div>
</div>
<div class="waypoint">
  <b>Waypoint</b>
  <div class="row"><label>Lat:</label><input type="number" id="wplat" step="any" placeholder="e.g. 42.3456"></div>
  <div class="row"><label>Lon:</label><input type="number" id="wplon" step="any" placeholder="e.g. -122.678"></div>
  <div class="row">
    <button onclick="saveWp()">Save</button>
    <button class="btn-clear" onclick="clearWp()">Clear</button>
  </div>
  <div id="wpstatus"></div>
</div>
<script>
async function load() {
  const [files, info] = await Promise.all([
    fetch('/api/files').then(r => r.json()),
    fetch('/api/fs-info').then(r => r.json())
  ]);
  document.getElementById('fsinfo').textContent =
    `Storage: ${fmt(info.used)} / ${fmt(info.total)} used (${fmt(info.free)} free)`;
  const tb = document.getElementById('files');
  if (!files.length) { tb.innerHTML = '<tr><td colspan="4">No files</td></tr>'; return; }
  tb.innerHTML = files.map(f =>
    `<tr>
      <td>${f.name}</td>
      <td>${fmt(f.size)}</td>
      <td><a class="btn btn-dl" href="/api/download?file=${encodeURIComponent(f.name)}">Download</a></td>
      <td><button class="btn btn-del" onclick="del('${f.name}')">Delete</button></td>
    </tr>`
  ).join('');
}
function fmt(b) {
  if (b < 1024) return b + ' B';
  if (b < 1048576) return (b/1024).toFixed(1) + ' KB';
  return (b/1048576).toFixed(1) + ' MB';
}
async function del(name) {
  if (!confirm('Delete ' + name + '?')) return;
  await fetch('/api/delete?file=' + encodeURIComponent(name));
  load();
}
document.getElementById('upform').addEventListener('submit', async e => {
  e.preventDefault();
  const file = document.getElementById('upfile').files[0];
  if (!file) return;
  const status = document.getElementById('status');
  status.textContent = 'Uploading...';
  const fd = new FormData();
  fd.append('file', file, '/' + file.name);
  const r = await fetch('/api/upload', { method: 'POST', body: fd });
  status.textContent = r.ok ? 'Upload complete!' : 'Upload failed.';
  if (r.ok) { document.getElementById('upfile').value = ''; load(); }
});
async function loadWp() {
  try {
    const r = await fetch('/api/waypoint');
    if (r.ok) {
      const wp = await r.json();
      document.getElementById('wplat').value = wp.lat;
      document.getElementById('wplon').value = wp.lon;
    }
  } catch(e) {}
}
async function saveWp() {
  const lat = parseFloat(document.getElementById('wplat').value);
  const lon = parseFloat(document.getElementById('wplon').value);
  const ws = document.getElementById('wpstatus');
  if (isNaN(lat) || isNaN(lon)) { ws.textContent = 'Enter valid lat/lon'; return; }
  if (lat < -90 || lat > 90 || lon < -180 || lon > 180) { ws.textContent = 'Lat/lon out of range'; return; }
  const r = await fetch('/api/waypoint', {
    method: 'POST', headers: {'Content-Type':'application/json'},
    body: JSON.stringify({lat, lon})
  });
  ws.textContent = r.ok ? 'Waypoint saved' : 'Save failed';
}
async function clearWp() {
  const r = await fetch('/api/waypoint', { method: 'DELETE' });
  if (r.ok) {
    document.getElementById('wplat').value = '';
    document.getElementById('wplon').value = '';
    document.getElementById('wpstatus').textContent = 'Waypoint cleared';
  }
}
load();
loadWp();
</script>
</body>
</html>
)rawliteral";

// --------------- route handlers ---------------

static void handleIndex() {
    server.send(200, "text/html", INDEX_HTML);
}

static void listDir(File dir, String& json, bool& first) {
    File f = dir.openNextFile();
    while (f) {
        if (f.isDirectory()) {
            listDir(f, json, first);
        } else {
            if (!first) json += ",";
            json += "{\"name\":\"";
            json += f.path();
            json += "\",\"size\":";
            json += String(f.size());
            json += "}";
            first = false;
        }
        f = dir.openNextFile();
    }
}

static void handleFileList() {
    String json = "[";
    File root = LittleFS.open("/");
    bool first = true;
    listDir(root, json, first);
    json += "]";
    server.send(200, "application/json", json);
}

static void handleFsInfo() {
    String json = "{\"total\":";
    json += String(LittleFS.totalBytes());
    json += ",\"used\":";
    json += String(LittleFS.usedBytes());
    json += ",\"free\":";
    json += String(LittleFS.totalBytes() - LittleFS.usedBytes());
    json += "}";
    server.send(200, "application/json", json);
}

static void handleDownload() {
    if (!server.hasArg("file")) {
        server.send(400, "text/plain", "Missing 'file' parameter");
        return;
    }
    String path = server.arg("file");
    if (!path.startsWith("/")) path = "/" + path;

    if (!LittleFS.exists(path)) {
        server.send(404, "text/plain", "File not found");
        return;
    }
    File f = LittleFS.open(path, "r");
    server.streamFile(f, "application/octet-stream");
    f.close();
}

static void handleDelete() {
    if (!server.hasArg("file")) {
        server.send(400, "text/plain", "Missing 'file' parameter");
        return;
    }
    String path = server.arg("file");
    if (!path.startsWith("/")) path = "/" + path;

    if (!LittleFS.exists(path)) {
        server.send(404, "text/plain", "File not found");
        return;
    }
    LittleFS.remove(path);
    server.send(200, "text/plain", "Deleted");
}

static String uploadPath;

static void handleUpload() {
    HTTPUpload& upload = server.upload();

    if (upload.status == UPLOAD_FILE_START) {
        uploadPath = upload.filename;
        if (!uploadPath.startsWith("/")) uploadPath = "/" + uploadPath;
        Serial.printf("[Web] Upload start: %s\n", uploadPath.c_str());
        File f = LittleFS.open(uploadPath, "w");
        f.close();
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        File f = LittleFS.open(uploadPath, "a");
        if (f) {
            f.write(upload.buf, upload.currentSize);
            f.close();
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        Serial.printf("[Web] Upload done: %s (%u bytes)\n", uploadPath.c_str(), upload.totalSize);
    }
}

static void handleUploadComplete() {
    server.send(200, "text/plain", "OK");
}

static const char* WAYPOINT_PATH = "/config/waypoint.json";

static void handleGetWaypoint() {
    if (!LittleFS.exists(WAYPOINT_PATH)) {
        server.send(404, "application/json", "{}");
        return;
    }
    File f = LittleFS.open(WAYPOINT_PATH, "r");
    if (!f) {
        server.send(500, "text/plain", "Read error");
        return;
    }
    String json = f.readString();
    f.close();
    server.send(200, "application/json", json);
}

static void handleSetWaypoint() {
    if (!server.hasArg("plain")) {
        server.send(400, "text/plain", "Missing body");
        return;
    }
    String body = server.arg("plain");

    // Parse lat and lon from JSON body
    int latIdx = body.indexOf("\"lat\"");
    int lonIdx = body.indexOf("\"lon\"");
    if (latIdx < 0 || lonIdx < 0) {
        server.send(400, "text/plain", "Missing lat/lon");
        return;
    }
    int latColon = body.indexOf(':', latIdx);
    int lonColon = body.indexOf(':', lonIdx);
    if (latColon < 0 || lonColon < 0) {
        server.send(400, "text/plain", "Bad JSON");
        return;
    }
    float lat = body.substring(latColon + 1).toFloat();
    float lon = body.substring(lonColon + 1).toFloat();

    if (lat < -90.0f || lat > 90.0f || lon < -180.0f || lon > 180.0f) {
        server.send(400, "text/plain", "Lat/lon out of range");
        return;
    }

    // Ensure parent directory exists
    LittleFS.mkdir("/config");
    File f = LittleFS.open(WAYPOINT_PATH, "w");
    if (!f) {
        server.send(500, "text/plain", "Write error");
        return;
    }
    f.printf("{\"lat\":%.8f,\"lon\":%.8f}\n", lat, lon);
    f.close();
    Serial.printf("[Web] Waypoint saved: %.8f, %.8f\n", lat, lon);
    server.send(200, "text/plain", "OK");
}

static void handleDeleteWaypoint() {
    if (LittleFS.exists(WAYPOINT_PATH)) {
        LittleFS.remove(WAYPOINT_PATH);
        Serial.println("[Web] Waypoint cleared");
    }
    server.send(200, "text/plain", "OK");
}

// --------------- public API ---------------

void init() {
    server.on("/",            HTTP_GET,  handleIndex);
    server.on("/api/files",   HTTP_GET,  handleFileList);
    server.on("/api/fs-info", HTTP_GET,  handleFsInfo);
    server.on("/api/download",HTTP_GET,  handleDownload);
    server.on("/api/delete",  HTTP_GET,  handleDelete);
    server.on("/api/upload",  HTTP_POST, handleUploadComplete, handleUpload);
    server.on("/api/waypoint",HTTP_GET,  handleGetWaypoint);
    server.on("/api/waypoint",HTTP_POST, handleSetWaypoint);
    server.on("/api/waypoint",HTTP_DELETE, handleDeleteWaypoint);
    server.onNotFound([]() {
        Serial.printf("[Web] 404: %s %s\n", server.method() == HTTP_GET ? "GET" : "POST",
                      server.uri().c_str());
        server.send(404, "text/plain", "Not Found");
    });
    server.begin();
    Serial.println("[Web] Server started on port 80");
}

void update() {
    server.handleClient();
}

}  // namespace web
