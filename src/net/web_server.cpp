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
  .uploadbar .btn-delsel { background: #e63946; color: #fff; }
  .uploadbar .btn-delsel:hover { background: #ff4d5a; }
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
  #wifistat2 { margin-top: .4rem; font-size: .9rem; color: #72efdd; min-height: 1.2em; }
  .wifisec .tag { font-size: .7rem; background: #333; color: #aaa; border-radius: 3px;
    padding: .05rem .3rem; margin-left: .4rem; vertical-align: middle; }
  .wifisec .btn { margin-left: .4rem; }
  #scanout { margin: .4rem 0 .2rem; }
  .scanrow { display: flex; justify-content: space-between; align-items: center; width: 100%;
    background: #1a1a2e; color: #e0e0e0; border: 1px solid #2a2a44; border-radius: 4px;
    padding: .45rem .6rem; margin: 0 0 .25rem; cursor: pointer; font-size: .9rem;
    text-align: left; font-weight: normal; }
  .scanrow:hover { background: #232347; border-color: #4cc9f0; }
  .scanrow .sig { color: #888; font-size: .8rem; font-family: monospace; margin-left: .75rem;
    white-space: nowrap; }
  .scannote, .hotspothelp { color: #aaa; font-size: .8rem; line-height: 1.4; margin: .4rem 0; }
  .hotspothelp b { color: #ffd166; font-weight: bold; }
  .wifisec .chk { font-size: .85rem; color: #ccc; }
  .wifisec .chk input { vertical-align: middle; margin-right: .3rem; }
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
  <button id="deleteselected" class="btn-delsel" onclick="deleteSelected()">Delete Selected</button>
  <span class="cloudresult" id="delselstatus"></span>
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
    <thead><tr><th style="text-align:left;color:#4cc9f0;padding:.4rem .5rem">Saved network</th><th></th></tr></thead>
    <tbody id="netbody"><tr><td colspan="2">Loading...</td></tr></tbody>
  </table>

  <div style="margin-top:.8rem"><b style="font-size:.9rem">Add a network</b></div>
  <div class="row"><button id="scanbtn" onclick="scanWifi()">Scan for networks</button></div>
  <div id="scanout"></div>

  <div class="row"><label>SSID:</label><input type="text" id="netssid" placeholder="Pick one above, or type it" autocomplete="off"></div>
  <div class="row"><label>Pass:</label><input type="password" id="netpass" placeholder="Password" autocomplete="new-password"></div>
  <div class="row"><label></label><label class="chk"><input type="checkbox" id="nethidden">Hidden network / phone hotspot</label></div>
  <div class="row"><button onclick="addNet()">Save</button></div>
  <div id="wifistat2"></div>

  <div class="hotspothelp">
    <b>Pairing with a phone hotspot.</b>
    A hotspot often will not show up in a scan &mdash; it stops broadcasting when nothing is
    connected to it. Type its name in exactly, tick the box above, and save; the unit then
    tries it by name whether or not it is beaconing.
    <br><br>
    Two things that stop it working, both on the phone:
    <br>&bull; <b>2.4 GHz.</b> The unit has no 5 GHz radio. On iPhone turn on
    <i>Personal Hotspot &rarr; Maximize Compatibility</i>; on Android set the hotspot band to 2.4 GHz.
    <br>&bull; <b>The hotspot has to be awake.</b> Leave the Personal Hotspot screen open while pairing.
    <br><br>
    A phone cannot serve its hotspot and stay joined to the Tern AP at the same time, so save the
    credentials first, then leave this page, switch the hotspot on, and the unit picks it up within
    about a minute. From a second device, <i>Try now</i> forces the attempt immediately.
  </div>
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
async function deleteSelected() {
  const boxes = Array.from(document.querySelectorAll('#files .upsel:checked'));
  const status = document.getElementById('delselstatus');
  status.textContent = '';
  if (!boxes.length) { alert('Select at least one file to delete.'); return; }
  const names = boxes.map(cb => cb.dataset.name);
  const shown = names.slice(0, 25).map(n => '  ' + n).join('\n');
  const more = names.length > 25 ? '\n  ... and ' + (names.length - 25) + ' more' : '';
  const msg = 'Permanently delete ' + names.length +
    (names.length === 1 ? ' file' : ' files') + ' from the unit?\n\n' + shown + more +
    '\n\nThis cannot be undone.';
  if (!confirm(msg)) return;
  const upBtn = document.getElementById('uploadselected');
  const delBtn = document.getElementById('deleteselected');
  upBtn.disabled = true;
  delBtn.disabled = true;
  let ok = 0;
  const failed = [];
  for (const name of names) {
    status.textContent = `Deleting ${ok + failed.length + 1} of ${names.length}...`;
    try {
      const r = await fetch('/api/delete?file=' + encodeURIComponent(name));
      if (r.ok) ok++; else failed.push(name);
    } catch (e) {
      failed.push(name);
    }
  }
  upBtn.disabled = false;
  delBtn.disabled = false;
  document.getElementById('selall').checked = false;
  status.textContent = failed.length
    ? `Deleted ${ok}, failed ${failed.length}: ${failed.join(', ')}`
    : `Deleted ${ok} file${ok === 1 ? '' : 's'}.`;
  load();
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
    // Google Maps wraps copied coords in invisible bidi marks (U+200E, U+202A..)
    // and may use U+2212 for minus, so pull out the two numbers rather than
    // anchoring on the whole string. Hemisphere letters (47.7° N, 122.6° W) flip sign.
    const text = (e.clipboardData || window.clipboardData).getData('text')
      .replace(/[−–]/g, '-');
    const nums = [...text.matchAll(/(-?\d+(?:\.\d+)?)[°\s]*([NSEW])?(?![\d.])/gi)];
    if (nums.length !== 2) return;
    const val = n => (/[SW]/i.test(n[2] || '') ? -Math.abs(+n[1]) : +n[1]);
    e.preventDefault();
    document.getElementById('wplat').value = val(nums[0]);
    document.getElementById('wplon').value = val(nums[1]);
  });
});
const sleep = ms => new Promise(r => setTimeout(r, ms));

// Rows are built with DOM calls, not innerHTML: an SSID is arbitrary text the
// unit read off the air, and the old string-concatenated onclick broke on any
// name holding a quote.
function savedNetRow(n) {
  const tr = document.createElement('tr');
  const name = document.createElement('td');
  name.style.padding = '.4rem .5rem';
  name.textContent = n.ssid;
  if (n.hidden) {
    const tag = document.createElement('span');
    tag.className = 'tag';
    tag.textContent = 'hidden';
    name.appendChild(tag);
  }
  const acts = document.createElement('td');
  acts.style.cssText = 'padding:.4rem .5rem;text-align:right';
  const tryb = document.createElement('button');
  tryb.className = 'btn';
  tryb.textContent = 'Try now';
  tryb.title = 'Connect to this network right away instead of waiting for the next retry';
  tryb.addEventListener('click', () => connectNow(n.ssid));
  const del = document.createElement('button');
  del.className = 'btn btn-del';
  del.textContent = 'Remove';
  del.addEventListener('click', () => removeNet(n.ssid));
  acts.appendChild(tryb);
  acts.appendChild(del);
  tr.appendChild(name);
  tr.appendChild(acts);
  return tr;
}
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
    tb.textContent = '';
    if (!nets.length) {
      const tr = document.createElement('tr');
      const td = document.createElement('td');
      td.colSpan = 2;
      td.style.cssText = 'padding:.4rem .5rem;color:#aaa';
      td.textContent = 'No networks configured';
      tr.appendChild(td);
      tb.appendChild(tr);
      return;
    }
    nets.forEach(n => tb.appendChild(savedNetRow(n)));
  } catch(e) { document.getElementById('wifista').textContent = 'Error loading WiFi info'; }
}
function rssiBars(r) {
  const n = r >= -55 ? 4 : r >= -67 ? 3 : r >= -75 ? 2 : 1;
  return '\u2588'.repeat(n) + '\u2591'.repeat(4 - n);
}
function pickSsid(n) {
  document.getElementById('netssid').value = n.ssid;
  document.getElementById('nethidden').checked = false;  // it was just seen, so it beacons
  const pass = document.getElementById('netpass');
  pass.value = '';
  const s = document.getElementById('wifistat2');
  if (n.open) {
    s.textContent = 'Open network \u2014 leave the password blank and press Save.';
  } else {
    s.textContent = 'Enter the password for "' + n.ssid + '" and press Save.';
    pass.focus();
  }
}
function renderScan(res) {
  const out = document.getElementById('scanout');
  out.textContent = '';
  if (res.state === 'failed') { out.textContent = 'Scan failed \u2014 try again.'; return; }
  if (res.state !== 'done')   { out.textContent = 'Scan did not finish \u2014 try again.'; return; }
  (res.nets || []).forEach(n => {
    const b = document.createElement('button');
    b.type = 'button';
    b.className = 'scanrow';
    const name = document.createElement('span');
    name.textContent = n.ssid + (n.open ? '' : ' \u{1F512}');
    const sig = document.createElement('span');
    sig.className = 'sig';
    sig.textContent = rssiBars(n.rssi) + '  ' + n.rssi + ' dBm';
    b.appendChild(name);
    b.appendChild(sig);
    b.addEventListener('click', () => pickSsid(n));
    out.appendChild(b);
  });
  const note = document.createElement('div');
  note.className = 'scannote';
  if (!(res.nets || []).length && !res.hidden) {
    note.textContent = 'No networks in range.';
  } else if (res.hidden) {
    note.textContent = res.hidden + ' network(s) in range are not broadcasting a name. If one is '
      + 'your hotspot, type its name below and tick "Hidden network / phone hotspot".';
  } else {
    note.textContent = 'Hotspot missing from the list? See the note below \u2014 you can still add it by name.';
  }
  out.appendChild(note);
}
async function scanWifi() {
  const btn = document.getElementById('scanbtn');
  const out = document.getElementById('scanout');
  btn.disabled = true;
  btn.textContent = 'Scanning\u2026';
  out.textContent = 'Sweeping the band. This page may freeze for a few seconds \u2014 the radio has '
    + 'to leave the Tern AP to listen, and comes back on its own.';
  try {
    let res = await fetch('/api/wifi-scan', {method: 'POST'}).then(r => r.json());
    // The AP is off-channel for most of the scan, so individual polls are
    // expected to fail; keep asking rather than treating one loss as an error.
    for (let i = 0; i < 25 && res.state === 'running'; i++) {
      await sleep(1000);
      try { res = await fetch('/api/wifi-scan').then(r => r.json()); } catch(e) { /* retry */ }
    }
    renderScan(res);
  } catch(e) {
    out.textContent = 'Could not reach the unit \u2014 rejoin the Tern network and try again.';
  }
  btn.disabled = false;
  btn.textContent = 'Scan for networks';
}
async function addNet() {
  const ssid = document.getElementById('netssid').value.trim();
  const pass = document.getElementById('netpass').value;
  const hidden = document.getElementById('nethidden').checked;
  const s = document.getElementById('wifistat2');
  if (!ssid) { s.textContent = 'Enter an SSID'; return; }
  const r = await fetch('/api/wifi-networks', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({ssid, pass, hidden})
  });
  s.textContent = r.ok
    ? (hidden ? 'Saved. Switch the hotspot on \u2014 the unit retries about once a minute.'
              : 'Network saved')
    : 'Save failed';
  if (r.ok) {
    document.getElementById('netssid').value = '';
    document.getElementById('netpass').value = '';
    document.getElementById('nethidden').checked = false;
    loadWifi();
  }
}
async function connectNow(ssid) {
  const s = document.getElementById('wifistat2');
  s.textContent = 'Trying "' + ssid + '"\u2026';
  let r;
  try { r = await fetch('/api/wifi-connect?ssid=' + encodeURIComponent(ssid), {method: 'POST'}); }
  catch(e) { s.textContent = 'Could not reach the unit.'; return; }
  if (!r.ok) { s.textContent = 'Could not start the attempt.'; return; }
  for (let i = 0; i < 12; i++) {
    await sleep(1000);
    try {
      const st = await fetch('/api/wifi-status').then(x => x.json());
      if (st.sta_connected && st.sta_ssid === ssid) {
        s.textContent = 'Connected \u2014 ' + st.sta_ip;
        loadWifi();
        return;
      }
    } catch(e) { /* radio busy mid-association */ }
  }
  s.textContent = 'Not connected yet. The unit keeps retrying about once a minute \u2014 '
    + 'leave the hotspot switched on.';
  loadWifi();
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

    // Parsed properly rather than scanned for quotes: SSIDs picked out of a
    // scan routinely carry characters the old hand-rolled parser mangled --
    // a curly apostrophe is standard in "<name>'s iPhone", and an embedded
    // quote or backslash would have truncated the credential silently.
    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain"))) {
        server.send(400, "text/plain", "Bad JSON");
        return;
    }

    const char* ssid   = doc["ssid"];
    const char* pass   = doc["pass"] | "";
    bool        hidden = doc["hidden"] | false;

    if (!ssid || strlen(ssid) == 0) {
        server.send(400, "text/plain", "Missing ssid");
        return;
    }
    if (strlen(ssid) > 63 || strlen(pass) > 63) {
        server.send(400, "text/plain", "ssid/pass too long");
        return;
    }
    if (!wifi::addNetwork(ssid, pass, hidden)) {
        server.send(500, "text/plain", "Network list full");
        return;
    }
    server.send(200, "text/plain", "OK");
}

// Kick off an async SSID scan.  Returns immediately with the current state --
// the radio leaves the softAP channel for the duration of a scan, so a handler
// that waited for results would stall the connection the diver is using.
static void handleStartWifiScan() {
    wifi::startScan();
    server.send(200, "application/json", wifi::getScanJson());
}

static void handleGetWifiScan() {
    server.send(200, "application/json", wifi::getScanJson());
}

// Try one saved network right now instead of waiting out the reconnect cycle.
// This is the hotspot path: save the credentials, switch the phone's hotspot
// on, press Try now.
static void handleWifiConnectNow() {
    if (!server.hasArg("ssid")) {
        server.send(400, "text/plain", "Missing 'ssid' parameter");
        return;
    }
    if (!wifi::connectNow(server.arg("ssid").c_str())) {
        server.send(404, "text/plain", "Not a saved network");
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
    server.on("/api/wifi-scan",     HTTP_POST,   handleStartWifiScan);
    server.on("/api/wifi-scan",     HTTP_GET,    handleGetWifiScan);
    server.on("/api/wifi-connect",  HTTP_POST,   handleWifiConnectNow);
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
