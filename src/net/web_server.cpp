#include "web_server.h"
#include "wifi_manager.h"
#include "cal_sync.h"
#include "cloud_client.h"
#include "../util/waypoints.h"
#include "../nav_main.h"
#include <ArduinoJson.h>
#include <WebServer.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <DNSServer.h>
#include <ESPmDNS.h>

namespace web {

static WebServer server(80);
static DNSServer dnsServer;
static bool sReloadCalRequested = false;

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
  .waypoints { margin-top: 1.5rem; padding: 1rem; background: #16213e; border-radius: 8px; }
  .waypoints table { width: 100%; border-collapse: collapse; margin-bottom: .75rem; }
  .waypoints th, .waypoints td { text-align: left; padding: .4rem .5rem; border-bottom: 1px solid #333; font-size: .9rem; }
  .waypoints th { color: #4cc9f0; }
  .waypoints .wp-form { margin-top: .75rem; }
  .waypoints .wp-form label { display: inline-block; width: 3.5rem; font-size: .9rem; }
  .waypoints .wp-form input[type=text], .waypoints .wp-form input[type=number] {
    padding: .3rem .5rem; border: 1px solid #333; border-radius: 4px;
    background: #1a1a2e; color: #e0e0e0; font-size: .9rem; }
  .waypoints .wp-form input[type=text] { width: 10rem; }
  .waypoints .wp-form input[type=number] { width: 9rem; }
  .waypoints .wp-form .row { margin: .3rem 0; }
  .waypoints button { background: #4cc9f0; color: #1a1a2e; border: none; padding: .3rem .7rem;
    border-radius: 4px; cursor: pointer; font-weight: bold; margin-right: .4rem; font-size: .85rem; }
  .waypoints button:hover { background: #72efdd; }
  .waypoints .btn-del { background: #e63946; color: #fff; }
  .waypoints .btn-del:hover { background: #ff4d5a; }
  #wpstatus { margin-top: .5rem; font-size: .9rem; color: #72efdd; }
  .calsync { margin-top: 1rem; padding: 1rem; background: #16213e; border-radius: 8px; }
  .calsync button { background: #4cc9f0; color: #1a1a2e; border: none; padding: .3rem .7rem;
    border-radius: 4px; cursor: pointer; font-weight: bold; margin: 0 .4rem .4rem 0; font-size: .85rem; }
  .calsync button:hover { background: #72efdd; }
  .calsync .label { font-size: .85rem; color: #aaa; margin-top: .6rem; }
  #calsyncstatus { font-size: .9rem; color: #aaa; margin: .5rem 0; }
  #calsyncaction { font-size: .9rem; color: #72efdd; margin-top: .3rem; min-height: 1.2em; }
  #cloudstatus { font-size: .9rem; color: #aaa; margin: -.5rem 0 1rem; }
  th.sel, td.sel { width: 1.5rem; }
  .uploadbar { margin: -1rem 0 1.5rem; }
  .uploadbar button, .btn-upload { background: #4cc9f0; color: #1a1a2e; border: none;
    padding: .3rem .7rem; border-radius: 4px; cursor: pointer; font-weight: bold; font-size: .85rem; }
  .uploadbar button:hover, .btn-upload:hover { background: #72efdd; }
  .uploadbar button:disabled, .btn-upload:disabled { background: #333; color: #777; cursor: not-allowed; }
  .cloudresult { display: block; font-size: .8rem; color: #72efdd; margin-top: .25rem; }
  .wifisec { margin-top: 1.5rem; padding: 1rem; background: #16213e; border-radius: 8px; }
  .wifisec .row { margin: .4rem 0; }
  .wifisec label { display: inline-block; width: 3.5rem; }
  .wifisec input[type=text], .wifisec input[type=password] {
    width: 14rem; padding: .3rem .5rem; border: 1px solid #333;
    border-radius: 4px; background: #1a1a2e; color: #e0e0e0; font-size: .95rem; }
  .wifisec button { background: #4cc9f0; color: #1a1a2e; border: none; padding: .4rem 1rem;
    border-radius: 4px; cursor: pointer; font-weight: bold; margin-right: .5rem; }
  .wifisec button:hover { background: #72efdd; }
  #wifista { color: #aaa; font-size: .85rem; margin: .4rem 0 .6rem; }
  #wifistat2 { margin-top: .4rem; font-size: .9rem; color: #72efdd; }
</style>
</head>
<body>
<h1>DPV-Nav File Manager</h1>
<div class="info" id="fsinfo">Loading...</div>
<div class="info" id="cloudstatus">Checking cloud link...</div>
<table>
  <thead><tr>
    <th class="sel"><input type="checkbox" id="selall" onchange="toggleSelectAll()" title="Select all uploadable files"></th>
    <th>File</th><th>Size</th><th></th><th></th><th>Cloud</th>
  </tr></thead>
  <tbody id="files"><tr><td colspan="6">Loading...</td></tr></tbody>
</table>
<div class="uploadbar">
  <button id="uploadselected" onclick="uploadSelected()">Upload Selected to Cloud</button>
</div>
<div class="upload">
  <b>Upload File</b>
  <form id="upform">
    <input type="file" id="upfile" required><br>
    <button type="submit">Upload</button>
  </form>
  <div id="status"></div>
</div>
<div class="upload" style="margin-top:1rem">
  <b>Calibration</b>
  <div style="margin-top:.5rem">
    <button onclick="reloadCal()">Reload Cal Files</button>
  </div>
  <div id="calstatus"></div>
</div>
<div class="calsync">
  <b>Calibration Cloud Sync</b>
  <div id="calsyncstatus">Not checked yet.</div>
  <div>
    <button onclick="checkCalUpdates()">Check for updates</button>
    <button onclick="backupCalNow()">Back up calibration now</button>
  </div>
  <div id="calsyncaction"></div>
  <div class="label">Restore from most recent cloud backup:</div>
  <div>
    <button onclick="restoreCal('baseline')">Baseline</button>
    <button onclick="restoreCal('mounted')">Mounted</button>
    <button onclick="restoreCal('hdg')">Heading</button>
    <button onclick="restoreCal('accel')">Accel</button>
    <button onclick="restoreCal('gyro')">Gyro</button>
    <button onclick="restoreCal('speed')">Speed</button>
  </div>
</div>
<div class="waypoints">
  <b>Waypoints</b>
  <table>
    <thead><tr><th>Name</th><th>Lat</th><th>Lon</th><th></th></tr></thead>
    <tbody id="wptbody"><tr><td colspan="4">Loading...</td></tr></tbody>
  </table>
  <div class="wp-form">
    <b style="font-size:.9rem">Add / Update Waypoint</b>
    <div class="row"><label>Name:</label><input type="text" id="wpname" placeholder="e.g. Bomber Line" maxlength="19"></div>
    <div class="row"><label>Lat:</label><input type="number" id="wplat" step="any" placeholder="e.g. 42.3456"></div>
    <div class="row"><label>Lon:</label><input type="number" id="wplon" step="any" placeholder="e.g. -122.678"></div>
    <div class="row"><button onclick="saveWp()">Save</button></div>
  </div>
  <div id="wpstatus"></div>
</div>
<div class="wifisec">
  <b>WiFi Networks</b>
  <div id="wifista">Loading...</div>
  <table style="width:100%;border-collapse:collapse;margin-bottom:.5rem">
    <thead><tr><th style="text-align:left;color:#4cc9f0;padding:.4rem .5rem">SSID</th><th></th></tr></thead>
    <tbody id="netbody"><tr><td colspan="2">Loading...</td></tr></tbody>
  </table>
  <div style="margin-top:.6rem"><b style="font-size:.9rem">Add / Update Network</b></div>
  <div class="row"><label>SSID:</label><input type="text" id="netssid" placeholder="Network name" autocomplete="off"></div>
  <div class="row"><label>Pass:</label><input type="password" id="netpass" placeholder="Password" autocomplete="new-password"></div>
  <div class="row"><button onclick="addNet()">Save</button></div>
  <div id="wifistat2"></div>
</div>
<script>
// Files recoverable via the calibration retry-upload flow (see
// nav_main::retryCalibrationUpload) -- kept in sync with the whitelist in
// nav_main.cpp's retryCalibrationUpload().
const CAL_RECOVERABLE_FILES = new Set([
  '/mag_baseline_samples.csv', '/mag_mounted_samples.csv',
  '/mag_gapfill_samples.csv', '/hdg_samples.csv'
]);
let cloudLinked = false;

function classifyUpload(name) {
  if (name.startsWith('/logs/') && name.endsWith('.csv')) return 'dive_log';
  if (CAL_RECOVERABLE_FILES.has(name)) return 'cal';
  return null;
}

async function load() {
  const [files, info] = await Promise.all([
    fetch('/api/files').then(r => r.json()),
    fetch('/api/fs-info').then(r => r.json())
  ]);
  document.getElementById('fsinfo').textContent =
    `Storage: ${fmt(info.used)} / ${fmt(info.total)} used (${fmt(info.free)} free)`;
  const tb = document.getElementById('files');
  if (!files.length) { tb.innerHTML = '<tr><td colspan="6">No files</td></tr>'; return; }
  tb.innerHTML = files.map(f => {
    const kind = classifyUpload(f.name);
    const sel = kind
      ? `<input type="checkbox" class="upsel" data-name="${f.name}" data-kind="${kind}">` : '';
    const cloud = kind
      ? `<button class="btn-upload" onclick="uploadOne('${f.name}','${kind}')" ${cloudLinked ? '' : 'disabled'}>Upload to cloud</button>` +
        `<span class="cloudresult" data-name="${f.name}"></span>`
      : '';
    return `<tr>
      <td class="sel">${sel}</td>
      <td>${f.name}</td>
      <td>${fmt(f.size)}</td>
      <td><a class="btn btn-dl" href="/api/download?file=${encodeURIComponent(f.name)}">Download</a></td>
      <td><button class="btn btn-del" onclick="del('${f.name}')">Delete</button></td>
      <td>${cloud}</td>
    </tr>`;
  }).join('');
}
function toggleSelectAll() {
  const on = document.getElementById('selall').checked;
  document.querySelectorAll('#files .upsel').forEach(cb => cb.checked = on);
}
async function uploadOne(name, kind) {
  const status = document.querySelector(`.cloudresult[data-name="${CSS.escape(name)}"]`);
  if (status) status.textContent = 'Uploading...';
  try {
    if (kind === 'dive_log') {
      const r = await fetch('/api/dive-logs/upload?file=' + encodeURIComponent(name), { method: 'POST' });
      const text = await r.text();
      if (status) status.textContent = r.ok ? 'Uploaded' : 'Failed: ' + text;
      return;
    }
    const r = await fetch('/api/cal/retry-upload?file=' + encodeURIComponent(name), { method: 'POST' });
    const j = await r.json();
    if (!status) return;
    if (!j.ok) { status.textContent = 'Failed: ' + (j.error || 'unknown error'); return; }
    const unit = name === '/hdg_samples.csv' ? '°' : '%';
    const next = j.installable
      ? 'check the unit’s display to accept/reject.'
      : 'uploaded — merge it into a calibration on the Dive Map website.';
    status.textContent = `Fit ${j.quality_band} (${j.rms_pct.toFixed(1)}${unit} err) — ${next}`;
  } catch (e) {
    if (status) status.textContent = 'Failed: network error';
  }
}
async function uploadSelected() {
  const boxes = Array.from(document.querySelectorAll('#files .upsel:checked'));
  if (!boxes.length) { alert('Select at least one file to upload.'); return; }
  const btn = document.getElementById('uploadselected');
  btn.disabled = true;
  for (const cb of boxes) {
    await uploadOne(cb.dataset.name, cb.dataset.kind);
  }
  btn.disabled = false;
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
async function loadWaypoints() {
  try {
    const r = await fetch('/api/waypoints');
    if (!r.ok) return;
    const wps = await r.json();
    const tb = document.getElementById('wptbody');
    if (!wps.length) { tb.innerHTML = '<tr><td colspan="4" style="color:#aaa">No waypoints</td></tr>'; return; }
    tb.innerHTML = wps.map(w => {
      const isHome = w.name === 'HOME';
      const delBtn = isHome ? '' :
        `<button class="btn-del" onclick="delWp('${w.name.replace(/'/g,"\\'")}')">Delete</button>`;
      return `<tr>
        <td>${w.name}</td>
        <td>${w.lat.toFixed(6)}</td>
        <td>${w.lon.toFixed(6)}</td>
        <td>${delBtn}</td>
      </tr>`;
    }).join('');
  } catch(e) { document.getElementById('wptbody').innerHTML = '<tr><td colspan="4">Error</td></tr>'; }
}
async function saveWp() {
  const name = document.getElementById('wpname').value.trim();
  const lat  = parseFloat(document.getElementById('wplat').value);
  const lon  = parseFloat(document.getElementById('wplon').value);
  const ws   = document.getElementById('wpstatus');
  if (!name) { ws.textContent = 'Enter a name'; return; }
  if (name === 'HOME') { ws.textContent = 'HOME is managed automatically'; return; }
  if (isNaN(lat) || isNaN(lon)) { ws.textContent = 'Enter valid lat/lon'; return; }
  if (lat < -90 || lat > 90 || lon < -180 || lon > 180) { ws.textContent = 'Lat/lon out of range'; return; }
  const r = await fetch('/api/waypoints', {
    method: 'POST', headers: {'Content-Type':'application/json'},
    body: JSON.stringify({name, lat, lon})
  });
  ws.textContent = r.ok ? 'Saved' : 'Save failed';
  if (r.ok) {
    document.getElementById('wpname').value = '';
    document.getElementById('wplat').value  = '';
    document.getElementById('wplon').value  = '';
    loadWaypoints();
  }
}
async function delWp(name) {
  if (!confirm('Delete waypoint "' + name + '"?')) return;
  const r = await fetch('/api/waypoints?name=' + encodeURIComponent(name), { method: 'DELETE' });
  document.getElementById('wpstatus').textContent = r.ok ? 'Deleted' : 'Delete failed';
  if (r.ok) loadWaypoints();
}
['wplat','wplon'].forEach(id => {
  document.getElementById(id).addEventListener('paste', e => {
    const text = (e.clipboardData || window.clipboardData).getData('text').trim();
    const m = text.match(/^(-?\d+\.?\d*)[,\s]+(-?\d+\.?\d*)$/);
    if (!m) return;
    e.preventDefault();
    document.getElementById('wplat').value = m[1];
    document.getElementById('wplon').value = m[2];
  });
});
async function loadWifi() {
  try {
    const [nets, status] = await Promise.all([
      fetch('/api/wifi-networks').then(r => r.json()),
      fetch('/api/wifi-status').then(r => r.json())
    ]);
    const sta = document.getElementById('wifista');
    sta.textContent = status.sta_connected
      ? 'STA: connected to "' + status.sta_ssid + '" \u2014 ' + status.sta_ip
      : 'STA: not connected (AP: ' + status.ap_ip + ')';
    const tb = document.getElementById('netbody');
    if (!nets.length) {
      tb.innerHTML = '<tr><td colspan="2" style="padding:.4rem .5rem;color:#aaa">No networks configured</td></tr>';
      return;
    }
    tb.innerHTML = nets.map(n =>
      '<tr>' +
      '<td style="padding:.4rem .5rem">' + n.ssid + '</td>' +
      '<td style="padding:.4rem .5rem"><button class="btn btn-del" onclick="removeNet(\'' +
        n.ssid.replace(/'/g, "\\'") + '\')">Remove</button></td>' +
      '</tr>'
    ).join('');
  } catch(e) { document.getElementById('wifista').textContent = 'Error loading WiFi info'; }
}
async function addNet() {
  const ssid = document.getElementById('netssid').value.trim();
  const pass = document.getElementById('netpass').value;
  const s = document.getElementById('wifistat2');
  if (!ssid) { s.textContent = 'Enter an SSID'; return; }
  const r = await fetch('/api/wifi-networks', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({ssid, pass})
  });
  s.textContent = r.ok ? 'Network saved' : 'Save failed';
  if (r.ok) {
    document.getElementById('netssid').value = '';
    document.getElementById('netpass').value = '';
    loadWifi();
  }
}
async function removeNet(ssid) {
  if (!confirm('Remove network "' + ssid + '"?')) return;
  const r = await fetch('/api/wifi-networks?ssid=' + encodeURIComponent(ssid), {method: 'DELETE'});
  document.getElementById('wifistat2').textContent = r.ok ? 'Removed' : 'Remove failed';
  loadWifi();
}
async function reloadCal() {
  const s = document.getElementById('calstatus');
  s.textContent = 'Reloading...';
  const r = await fetch('/api/reload-cal', { method: 'POST' });
  s.textContent = r.ok ? 'Calibration reloaded' : 'Reload failed';
}
async function loadCalSyncStatus() {
  const el = document.getElementById('calsyncstatus');
  try {
    const s = await fetch('/api/cal-sync/status').then(r => r.json());
    if (!s.checked) { el.textContent = 'Not checked yet.'; return; }
    if (!s.modes.length) { el.textContent = 'In sync as of last check.'; return; }
    el.textContent = s.modes.map(m => m.mode + ': ' + (m.in_sync ? 'installed' : 'not installed')).join('  ·  ');
  } catch (e) { el.textContent = 'Not checked yet.'; }
}
async function checkCalUpdates() {
  const el = document.getElementById('calsyncaction');
  el.textContent = 'Checking...';
  const r = await fetch('/api/cal-sync/check', { method: 'POST' });
  el.textContent = await r.text();
  loadCalSyncStatus();
}
async function backupCalNow() {
  const el = document.getElementById('calsyncaction');
  el.textContent = 'Backing up...';
  const r = await fetch('/api/cal-sync/backup', { method: 'POST' });
  el.textContent = await r.text();
}
async function restoreCal(kind) {
  if (!confirm('Restore ' + kind + ' calibration from the most recent cloud backup?')) return;
  const el = document.getElementById('calsyncaction');
  el.textContent = 'Restoring...';
  const r = await fetch('/api/cal-sync/restore?type=' + encodeURIComponent(kind), { method: 'POST' });
  el.textContent = await r.text();
  loadCalSyncStatus();
}
async function loadCloudStatus() {
  const el = document.getElementById('cloudstatus');
  try {
    const s = await fetch('/api/cloud-status').then(r => r.json());
    cloudLinked = s.authorized;
    el.textContent = cloudLinked
      ? 'Linked to Dive Map account.'
      : 'Not linked — link this device from the CAL menu on the unit first.';
    document.querySelectorAll('.btn-upload').forEach(b => b.disabled = !cloudLinked);
    document.getElementById('uploadselected').disabled = !cloudLinked;
  } catch (e) { el.textContent = 'Could not check link status.'; }
}
load();
loadWaypoints();
loadWifi();
loadCalSyncStatus();
loadCloudStatus();
</script>
</body>
</html>
)rawliteral";

// --------------- captive portal handlers ---------------
// Return the exact success responses each OS expects so devices consider
// connectivity confirmed — no popup, no "weak security" warning, stays connected.

static void handleIosConnectivity() {
    // iOS/macOS: captive.apple.com/hotspot-detect.html and /library/test/success.html
    server.send(200, "text/html",
        "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
}

static void handleAndroidConnectivity() {
    // Android: connectivitycheck.gstatic.com/generate_204 (expects empty 204)
    server.send(204, "text/plain", "");
}

static void handleWindowsConnectivity() {
    // Windows NCSI: www.msftconnecttest.com/connecttest.txt
    server.send(200, "text/plain", "Microsoft Connect Test");
}

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
    // Extract just the filename (strip leading path components)
    String filename = path;
    int slash = path.lastIndexOf('/');
    if (slash >= 0) filename = path.substring(slash + 1);

    File f = LittleFS.open(path, "r");
    server.sendHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
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

static void handleReloadCal() {
    sReloadCalRequested = true;
    server.send(200, "text/plain", "OK");
}

// --------------- calibration install sync / backup ---------------
// Handlers here call straight into net/cal_sync.h, which itself makes the
// outbound HTTPS call(s) to the cloud and blocks until done -- same
// tradeoff cloud_client.h's own docs accept for CAL-menu-triggered uploads
// (diver-initiated, not mid-dive). See
// docs/architecture/calibration-install-sync-plan.md.

static void handleCalSyncStatus() {
    server.send(200, "application/json", cal_sync::statusJson());
}

static void handleCalSyncCheck() {
    server.send(200, "text/plain", cal_sync::checkForUpdates());
}

static void handleCalSyncBackup() {
    server.send(200, "text/plain", cal_sync::backUpNow());
}

static void handleCalSyncRestore() {
    if (!server.hasArg("type")) {
        server.send(400, "text/plain", "Missing 'type' parameter");
        return;
    }
    String result = cal_sync::restoreBackup(server.arg("type").c_str());
    server.send(200, "text/plain", result);
}

static void handleGetWaypoints() {
    server.send(200, "application/json", waypoints::toJson());
}

static void handleAddWaypoint() {
    if (!server.hasArg("plain")) {
        server.send(400, "text/plain", "Missing body");
        return;
    }
    String body = server.arg("plain");

    JsonDocument doc;
    if (deserializeJson(doc, body)) {
        server.send(400, "text/plain", "Bad JSON");
        return;
    }

    const char* nameC = doc["name"] | "";
    String name = String(nameC);
    float lat   = doc["lat"] | 999.0f;
    float lon   = doc["lon"] | 999.0f;

    if (name.isEmpty()) { server.send(400, "text/plain", "Missing name"); return; }
    if (name == "HOME") { server.send(400, "text/plain", "HOME is managed automatically"); return; }
    if (name.length() > waypoints::WP_NAME_LEN) { server.send(400, "text/plain", "Name too long"); return; }
    if (lat < -90.0f || lat > 90.0f || lon < -180.0f || lon > 180.0f) {
        server.send(400, "text/plain", "Lat/lon out of range");
        return;
    }

    if (!waypoints::addOrUpdate(name.c_str(), lat, lon)) {
        server.send(500, "text/plain", "Waypoint list full");
        return;
    }
    waypoints::save();
    Serial.printf("[Web] Waypoint saved: %s (%.6f, %.6f)\n", name.c_str(), lat, lon);
    server.send(200, "text/plain", "OK");
}

static void handleDeleteWaypoint() {
    if (!server.hasArg("name")) {
        server.send(400, "text/plain", "Missing 'name' parameter");
        return;
    }
    String name = server.arg("name");
    if (name == "HOME") {
        server.send(400, "text/plain", "Cannot delete HOME");
        return;
    }
    if (!waypoints::remove(name.c_str())) {
        server.send(404, "text/plain", "Not found");
        return;
    }
    waypoints::save();
    Serial.printf("[Web] Waypoint deleted: %s\n", name.c_str());
    server.send(200, "text/plain", "OK");
}

// --------------- WiFi network management ---------------

static void handleGetWifiNetworks() {
    server.send(200, "application/json", wifi::getNetworksJson());
}

static void handleAddWifiNetwork() {
    if (!server.hasArg("plain")) {
        server.send(400, "text/plain", "Missing body");
        return;
    }
    String body = server.arg("plain");

    // Extract ssid and pass from JSON body (simple manual parse — no full JSON lib needed here)
    auto extractField = [&](const char* key) -> String {
        String token = String("\"") + key + "\"";
        int idx = body.indexOf(token);
        if (idx < 0) return "";
        int colon = body.indexOf(':', idx + token.length());
        if (colon < 0) return "";
        int start = body.indexOf('"', colon + 1);
        if (start < 0) return "";
        int end = body.indexOf('"', start + 1);
        if (end < 0) return "";
        return body.substring(start + 1, end);
    };

    String ssid = extractField("ssid");
    String pass = extractField("pass");
    if (ssid.isEmpty()) {
        server.send(400, "text/plain", "Missing ssid");
        return;
    }
    if (ssid.length() > 63 || pass.length() > 63) {
        server.send(400, "text/plain", "ssid/pass too long");
        return;
    }
    if (!wifi::addNetwork(ssid.c_str(), pass.c_str())) {
        server.send(500, "text/plain", "Network list full");
        return;
    }
    server.send(200, "text/plain", "OK");
}

static void handleRemoveWifiNetwork() {
    if (!server.hasArg("ssid")) {
        server.send(400, "text/plain", "Missing 'ssid' parameter");
        return;
    }
    String ssid = server.arg("ssid");
    wifi::removeNetwork(ssid.c_str());
    server.send(200, "text/plain", "OK");
}

static void handleWifiStatus() {
    String json = "{\"ap_ip\":\"";
    json += wifi::ip().toString();
    json += "\",\"sta_connected\":";
    json += wifi::isStaConnected() ? "true" : "false";
    json += ",\"sta_ssid\":\"";
    json += wifi::staSSID();
    json += "\",\"sta_ip\":\"";
    json += wifi::isStaConnected() ? wifi::staIP().toString() : "";
    json += "\"}";
    server.send(200, "application/json", json);
}

// --------------- dive-log upload ---------------
// Diver-initiated from the "Upload Dive Logs" panel: one blocking call per
// selected file, same tradeoff already accepted for the other cloud_client
// calls in this file (calibration sync).

static void handleCloudStatus() {
    String json = "{\"authorized\":";
    json += cloud::isAuthorized() ? "true" : "false";
    json += "}";
    server.send(200, "application/json", json);
}

static void handleDiveLogUpload() {
    if (!server.hasArg("file")) {
        server.send(400, "text/plain", "Missing 'file' parameter");
        return;
    }
    String path = server.arg("file");
    if (!path.startsWith("/logs/") || path.indexOf("..") >= 0) {
        server.send(400, "text/plain", "Invalid file path");
        return;
    }
    if (!LittleFS.exists(path)) {
        server.send(404, "text/plain", "File not found");
        return;
    }
    String err;
    if (!cloud::uploadBackup("dive_log", path.c_str(), err)) {
        server.send(502, "text/plain", err);
        return;
    }
    server.send(200, "text/plain", "Uploaded");
}

// --------------- calibration retry-upload ---------------
// Diver-initiated recovery for a raw calibration sample CSV that's still on
// LittleFS after its automatic cloud upload failed (WiFi blip, deploy
// restart, etc. -- see dpvnav-http-minus3-causes). Runs the same
// upload+fit+notify flow a live CAL-menu run performs
// (nav_main::retryCalibrationUpload / runCalUploadAndNotify), so a
// successful retry shows the normal accept/reject screen on the unit's
// display exactly as a fresh cal would.

static void handleCalRetryUpload() {
    if (!server.hasArg("file")) {
        server.send(400, "text/plain", "Missing 'file' parameter");
        return;
    }
    CalRetryResult r = retryCalibrationUpload(server.arg("file").c_str());

    JsonDocument doc;
    doc["ok"] = r.ok;
    doc["installable"] = r.installable;
    if (r.ok) {
        doc["quality_band"] = r.qualityBand;
        doc["rms_pct"] = r.rmsPct;
        doc["recommendation"] = r.recommendation;
        doc["calibration_id"] = r.calibrationId;
    } else {
        doc["error"] = r.error;
    }
    String json;
    serializeJson(doc, json);
    server.send(r.ok ? 200 : 502, "application/json", json);
}

// --------------- public API ---------------

void init() {
    // Wildcard DNS: resolves every hostname to the AP IP.
    // Combined with the OS-specific handlers below, devices silently confirm
    // connectivity and stay connected.  Also makes tern.nav work in any browser.
    dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    dnsServer.start(53, "*", wifi::ip());

    // mDNS: makes tern.local resolve via the proper mDNS protocol on
    // iOS, macOS, and Windows 10+, including when connected via STA.
    if (MDNS.begin("tern")) {
        MDNS.addService("http", "tcp", 80);
        Serial.println("[Web] mDNS started: tern.local");
    } else {
        Serial.println("[Web] mDNS start failed");
    }

    // Captive-portal connectivity checks — each OS probes a well-known URL
    // when joining a network; return the exact expected response so the device
    // treats this as a normal internet connection (Option B: invisible, no popup).
    server.on("/hotspot-detect.html",       HTTP_GET, handleIosConnectivity);
    server.on("/library/test/success.html", HTTP_GET, handleIosConnectivity);
    server.on("/generate_204",              HTTP_GET, handleAndroidConnectivity);
    server.on("/gen_204",                   HTTP_GET, handleAndroidConnectivity);
    server.on("/connecttest.txt",           HTTP_GET, handleWindowsConnectivity);
    server.on("/ncsi.txt",                  HTTP_GET, handleWindowsConnectivity);

    server.on("/",            HTTP_GET,  handleIndex);
    server.on("/api/files",   HTTP_GET,  handleFileList);
    server.on("/api/fs-info", HTTP_GET,  handleFsInfo);
    server.on("/api/download",HTTP_GET,  handleDownload);
    server.on("/api/delete",  HTTP_GET,  handleDelete);
    server.on("/api/upload",     HTTP_POST, handleUploadComplete, handleUpload);
    server.on("/api/reload-cal", HTTP_POST, handleReloadCal);
    server.on("/api/cal-sync/status",  HTTP_GET,  handleCalSyncStatus);
    server.on("/api/cal-sync/check",   HTTP_POST, handleCalSyncCheck);
    server.on("/api/cal-sync/backup",  HTTP_POST, handleCalSyncBackup);
    server.on("/api/cal-sync/restore", HTTP_POST, handleCalSyncRestore);
    server.on("/api/waypoints",     HTTP_GET,    handleGetWaypoints);
    server.on("/api/waypoints",     HTTP_POST,   handleAddWaypoint);
    server.on("/api/waypoints",     HTTP_DELETE, handleDeleteWaypoint);
    server.on("/api/wifi-networks", HTTP_GET,    handleGetWifiNetworks);
    server.on("/api/wifi-networks", HTTP_POST,   handleAddWifiNetwork);
    server.on("/api/wifi-networks", HTTP_DELETE, handleRemoveWifiNetwork);
    server.on("/api/wifi-status",   HTTP_GET,    handleWifiStatus);
    server.on("/api/cloud-status",     HTTP_GET,  handleCloudStatus);
    server.on("/api/dive-logs/upload", HTTP_POST, handleDiveLogUpload);
    server.on("/api/cal/retry-upload", HTTP_POST, handleCalRetryUpload);
    server.onNotFound([]() {
        Serial.printf("[Web] 404: %s %s\n", server.method() == HTTP_GET ? "GET" : "POST",
                      server.uri().c_str());
        server.send(404, "text/plain", "Not Found");
    });
    server.begin();
    Serial.println("[Web] Server started on port 80");
}

void update() {
    dnsServer.processNextRequest();
    server.handleClient();
}

bool isReloadCalRequested() { return sReloadCalRequested; }
void clearReloadCalRequest() { sReloadCalRequested = false; }

}  // namespace web
