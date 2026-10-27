#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <SD.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <ArduinoOTA.h>
#include <math.h>
#include "config.h"

// ============================================================
// Globals
// ============================================================
TFT_eSPI tft = TFT_eSPI();
Preferences prefs;
AsyncWebServer server(HTTP_PORT);
DNSServer dnsServer;

bool sdMounted = false;
bool wifiConnected = false;
bool wifiConnecting = false;
unsigned long wifiConnectStart = 0;
bool apMode = false;
bool otaReady = false;
String currentSSID = "";
String currentIP = "";
float sdTotalGB = 0.0f;
float sdFreeGB = 0.0f;

unsigned long lastSDPoll = 0;
unsigned long lastWifiRetry = 0;
unsigned long lastSDAnim = 0;
bool uiDirty = true;
int wifiFailCount = 0;
bool uploadSuccess = false;   // tracks actual write result

// SD fault diagnostics
unsigned long sdFaultSince = 0;   // millis() when fault first detected (0 = no fault)
int sdRetryCount = 0;             // mount retry attempts since fault

// ============================================================
// Forward declarations
// ============================================================
void initDisplay();
void initSD();
void initWiFi();
void startAPMode();
void startSTAMode(const String& ssid, const String& pass);
void setupWebServer();
void updateUI();
void drawStatusScreen();
void drawSDErrorScreenFull();
void tickSDErrorScreen();
void drawAPScreen();
void pollSD();
void handleWiFiReconnect();
void noteSDFault();
String formatBytes(uint64_t bytes);
String jsonEscape(const String& input);
String sanitizeFilename(const String& filename);
bool isPathSafe(const String& path);
bool deletePathRecursive(const String& path, uint8_t depth = 0);

const char INDEX_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html><html><head><meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'><title>ESP32 Drive</title>
<style>
*{box-sizing:border-box}body{margin:0;font-family:'Segoe UI',Roboto,Arial,sans-serif;background:#101418;color:#e8eaed}
header{display:flex;align-items:center;gap:10px;padding:10px 18px;background:#1a1f26;box-shadow:0 1px 0 #2a313a;position:sticky;top:0;z-index:5}
.logo{display:flex;align-items:center;gap:8px;font-size:20px;font-weight:600;color:#8ab4f8;white-space:nowrap}
.logo b{background:#8ab4f8;color:#101418;padding:2px 7px;border-radius:6px}
#search{flex:1;min-width:120px;padding:10px 16px;background:#101418;border:1px solid #2a313a;border-radius:10px;color:#e8eaed;font-size:14px;outline:none}
#search:focus{border-color:#8ab4f8}
#mkdirBtn{background:#222a33;color:#8ab4f8;border:1px solid #2a313a;padding:9px 14px;border-radius:10px;font-size:14px;font-weight:600;cursor:pointer;white-space:nowrap}
#mkdirBtn:hover{border-color:#8ab4f8}
#uploadBtn{background:#8ab4f8;color:#101418;border:none;padding:9px 18px;border-radius:10px;font-size:14px;font-weight:600;cursor:pointer;white-space:nowrap}
#uploadBtn:hover{filter:brightness(1.1)}
#layout{display:flex;min-height:calc(100vh - 54px)}
nav{width:250px;background:#161b21;padding:18px 12px}
.navitem{display:block;padding:10px 14px;border-radius:8px;cursor:pointer;color:#e8eaed;font-size:14px;text-decoration:none;margin-bottom:4px}
.navitem:hover{background:#1f262e}.navitem.active{background:#2a3442;color:#8ab4f8}
.card{background:#1a1f26;border:1px solid #2a313a;border-radius:12px;padding:14px;margin-top:16px}
.card h3{margin:0 0 10px;font-size:12px;letter-spacing:.8px;text-transform:uppercase;color:#9aa0a6}
.card p{margin:4px 0;font-size:13px;color:#c5cbd3;word-break:break-all}
.bar{height:7px;background:#2a313a;border-radius:4px;overflow:hidden;margin:8px 0}
.bar i{display:block;height:100%;background:#8ab4f8;border-radius:4px}
main{flex:1;padding:14px 22px;min-width:0}
#crumbs{font-size:15px;color:#9aa0a6;margin:6px 0 14px}
#crumbs a{color:#8ab4f8;text-decoration:none;cursor:pointer}
#crumbs a:hover{text-decoration:underline}
#selbar{display:flex;align-items:center;gap:10px;margin:0 0 12px;padding:9px 14px;background:#243040;border:1px solid #2a313a;border-radius:10px;font-size:13px}
#selbar b{color:#8ab4f8}
#selbar button{background:#8ab4f8;color:#101418;border:none;padding:6px 12px;border-radius:8px;font-size:13px;font-weight:600;cursor:pointer}
#selbar button.del{background:#f28b82}
#selbar button.clr{background:none;color:#9aa0a6;font-weight:400;padding:6px 6px}
#selbar button:hover{filter:brightness(1.1)}
table{width:100%;border-collapse:collapse}
th{text-align:left;font-size:12px;color:#9aa0a6;padding:8px;border-bottom:1px solid #2a313a;white-space:nowrap}
tr{border-bottom:1px solid #1f262e}
tr.folder{cursor:pointer}
tbody tr:hover{background:#1a2027}
tbody tr.sel{background:#243040}
td{padding:9px 8px;font-size:14px;vertical-align:middle}
.tname{display:flex;align-items:center;gap:12px;min-width:0}
.nm{overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.fname{color:#e8eaed;cursor:pointer}
.icon{width:22px;text-align:center;font-size:17px}
.chk{width:30px}
input[type=checkbox]{accent-color:#8ab4f8;width:15px;height:15px;cursor:pointer;display:block}
.size{text-align:right;color:#9aa0a6;width:110px;white-space:nowrap;font-variant-numeric:tabular-nums}
th.act,td.act{text-align:right;width:170px;white-space:nowrap}
.actbtn{background:#222a33;border:1px solid #2a313a;color:#c5cbd3;border-radius:8px;font-size:12px;padding:5px 10px;margin-left:6px;cursor:pointer}
.actbtn:hover{border-color:#8ab4f8;color:#8ab4f8}
.actbtn.danger:hover{border-color:#f28b82;color:#f28b82}
#empty{padding:60px 20px;text-align:center;color:#5f6368}
.hidden{display:none!important}
#upOv{position:fixed;inset:0;background:rgba(6,10,14,.7);display:flex;align-items:center;justify-content:center;z-index:20}
#upPanel{background:#1a1f26;border:1px solid #2a313a;border-radius:14px;padding:26px 32px;width:min(420px,90vw);text-align:center}
#upBar{height:8px;background:#2a313a;border-radius:5px;margin:16px 0;overflow:hidden}
#upBar i{display:block;height:100%;width:0;background:#8ab4f8;border-radius:5px;transition:width .15s}
#toast{position:fixed;bottom:20px;left:50%;transform:translateX(-50%);background:#2a3442;color:#e8eaed;padding:10px 20px;border-radius:10px;font-size:14px;z-index:30}
#toast.err{border-left:3px solid #f28b82}
input[type=text],input[type=password]{padding:9px 10px;background:#101418;border:1px solid #2a313a;border-radius:8px;color:#e8eaed;width:100%;font-size:13px;margin-top:6px}
.b{background:#8ab4f8;color:#101418;border:none;padding:8px 14px;border-radius:8px;cursor:pointer;font-size:13px;margin-top:10px}
@media(max-width:820px){nav{display:none}th.act,td.act{width:auto}}
</style></head>
<body>
<header><div class='logo'><b>NAS</b> ESP32 Drive</div>
<input id='search' placeholder='Search files' oninput='render()'>
<button id='mkdirBtn' onclick='newFolder()'>+ New folder</button>
<button id='uploadBtn' onclick="document.getElementById('fileInput').click()">&#8593; Upload</button></header>
<div id='layout'>
<nav>
<span class='navitem active'>My Files</span>
<div class='card'><h3>Storage</h3><div class='bar'><i id='stoBar'></i></div><p id='stoTxt'></p></div>
<div class='card'><h3>Transfers</h3><p id='upSp'>&#8593; idle</p><p id='dnSp'>&#8595; idle</p></div>
<div class='card'><h3>Device</h3><p id='netTxt'></p></div>
<div class='card hidden' id='wifiCard'><h3>WiFi Setup</h3>
<input type='text' id='wS' placeholder='SSID'>
<input type='password' id='wP' placeholder='Password'>
<button class='b' onclick='saveWifi()'>Save &amp; Connect</button></div>
</nav>
<main>
<div id='crumbs'></div>
<div id='selbar' class='hidden'>
<b id='selCount'></b>
<button onclick='bulkDownload()'>&#8595; Download</button>
<button class='del' onclick='bulkDelete()'>Delete</button>
<button class='clr' onclick='clearSel()'>Clear</button>
</div>
<table><thead><tr>
<th class='chk'><input type='checkbox' id='selAll' onchange='toggleAll(this.checked)'></th>
<th>Name</th>
<th class='size'>Size</th>
<th class='act'>Actions</th>
</tr></thead>
<tbody id='rows'></tbody></table>
<div id='empty' class='hidden'>This folder is empty</div>
</main></div>
<div id='upOv' class='hidden'><div id='upPanel'><b id='upTxt'>Uploading...</b><div id='upBar'><i></i></div></div></div>
<div id='toast' class='hidden'></div>
<input type='file' id='fileInput' hidden>
<script>
let cur='/',items=[];
const enc=p=>encodeURIComponent(p);
const $=id=>document.getElementById(id);
function toast(msg,err){const t=$('toast');t.textContent=msg;t.className=err?'err':'';t.classList.remove('hidden');setTimeout(()=>t.classList.add('hidden'),3000)}
async function jget(u){const r=await fetch(u);if(!r.ok)throw 0;return r.json()}
function join(p,n){return p==='/'?'/'+n:p+'/'+n}
function parentOf(p){const s=p.slice(0,p.lastIndexOf('/'));return s===''?'/':s}
function fmtSize(b){if(b<1024)return b+' B';if(b<1048576)return(b/1024).toFixed(1)+' KB';return(b/1048576).toFixed(1)+' MB'}
function fmtSp(bps){if(!isFinite(bps)||bps<=0)return'0 KB/s';if(bps<1048576)return(bps/1024).toFixed(1)+' KB/s';return(bps/1048576).toFixed(1)+' MB/s'}
function iconFor(n){const e=(n.split('.').pop()||'').toLowerCase();
if(['png','jpg','jpeg','gif','bmp','webp'].includes(e))return'\u{1F5BC}';
if(['mp4','avi','mkv','mov'].includes(e))return'\u{1F39E}';
if(['mp3','wav','flac'].includes(e))return'\u{1F3B5}';
if(['zip','rar','7z','gz','tar'].includes(e))return'\u{1F5DC}';
if(['txt','md','log','cpp','h','ino','json','csv','xml','ini','py'].includes(e))return'\u{1F4D3}';
if(['pdf'].includes(e))return'\u{1F4C4}';
return'\u{1F4C1}'}
function paint(s){
const used=s.totalGB-s.freeGB,pct=s.totalGB>0?Math.min(100,used*100/s.totalGB):0;
const b=$('stoBar');b.style.width=pct+'%';b.style.background=pct>90?'#f28b82':pct>75?'#fdd663':'#8ab4f8';
$('stoTxt').textContent=s.sdMounted?s.totalGB.toFixed(2)+' GB total \u00B7 '+s.freeGB.toFixed(2)+' GB free':'Not mounted';
$('netTxt').textContent=s.apMode?'AP mode \u00B7 '+s.ip:(s.wifiConnected?'WiFi \u00B7 '+s.ssid:'Connecting...');
$('wifiCard').classList.toggle('hidden',!s.apMode)}
function crumbs(p){const c=$('crumbs');c.textContent='';
if(p!=='/'){const b=document.createElement('a');b.textContent='\u2039 Back';b.onclick=()=>nav(parentOf(p));c.appendChild(b)}
const h=document.createElement('a');h.textContent=' My Files';h.onclick=()=>nav('/');c.appendChild(h);
let acc='';p.split('/').filter(Boolean).forEach(seg=>{acc+='/'+seg;
const a=document.createElement('a');a.textContent=' \u203A '+seg;a.onclick=()=>nav(acc);c.appendChild(a)})}
let sel=new Set();
function clearSel(){sel.clear();updateSel();$('selAll').checked=false;render()}
function updateSel(){const n=sel.size;$('selbar').classList.toggle('hidden',n===0);
$('selCount').textContent=n+' item'+(n>1?'s':'')+' selected'}
function toggleAll(on){items.forEach(i=>{if(!i.d){const p=join(cur,i.n);if(on&&!sel.has(p))sel.add(p);if(!on)sel.delete(p)}});
$('selAll').checked=on;render()}
function syncSelAll(){const files=items.filter(i=>!i.d);
$('selAll').checked=files.length>0&&files.every(i=>sel.has(join(cur,i.n)))}
async function bulkDelete(){if(!sel.size)return;
if(!confirm('Delete '+sel.size+' item(s) permanently?'))return;
const list=[...sel];let ok=0;
for(const p of list){try{const r=await fetch('/delete?file='+enc(p));if(r.ok){ok++;sel.delete(p)}}catch(e){}}
sel.clear();updateSel();
toast(ok===list.length?('Deleted '+ok+' item(s)'):(ok+'/'+list.length+' deleted \u00B7 '+(list.length-ok)+' failed'),ok!==list.length);
nav(cur)}
async function bulkDownload(){
const files=[...sel].filter(p=>{const n=p==='/'?'':p.split('/').pop();
const it=items.find(i=>join(cur,i.n)===p);return it&&!it.d});
if(!files.length){toast('No files selected (folders cannot be downloaded)',true);return}
for(let k=0;k<files.length;k++){
const it=items.find(i=>join(cur,i.n)===files[k]);
await downloadFile(files[k],it.n);sel.delete(files[k]);
if(k<files.length-1)await new Promise(r=>setTimeout(r,500))}
toast('Downloaded '+files.length+' file(s)');updateSel();render()}
function render(){const tb=$('rows');tb.textContent='';
const q=$('search').value.toLowerCase();
const list=items.filter(i=>i.n.toLowerCase().includes(q));
list.sort((a,b)=>(b.d?1:0)-(a.d?1:0)||a.n.localeCompare(b.n));
list.forEach(i=>{const full=join(cur,i.n);const selected=sel.has(full);
const tr=document.createElement('tr');
if(i.d){tr.className='folder';if(selected)tr.classList.add('sel');tr.onclick=()=>nav(full)}
else{if(selected)tr.classList.add('sel')}
const c0=document.createElement('td');c0.className='chk';
const cb=document.createElement('input');cb.type='checkbox';cb.checked=selected;
if(i.d)cb.disabled=true;
cb.onclick=e=>e.stopPropagation();
cb.onchange=()=>{if(cb.checked)sel.add(full);else sel.delete(full);tr.classList.toggle('sel',cb.checked);updateSel();syncSelAll()};
c0.append(cb);
const t1=document.createElement('td');const tn=document.createElement('div');tn.className='tname';
const ic=document.createElement('span');ic.className='icon';ic.textContent=i.d?'\u{1F4C1}':iconFor(i.n);
const nm=document.createElement('span');nm.className='nm'+(i.d?'':' fname');
nm.textContent=i.n;if(!i.d)nm.onclick=()=>downloadFile(full,i.n);
tn.append(ic,nm);t1.append(tn);
const t2=document.createElement('td');t2.className='size';t2.textContent=i.d?'':i.s;
const t3=document.createElement('td');t3.className='act';
if(!i.d){const db=document.createElement('button');db.className='actbtn';db.textContent='Download';
db.onclick=e=>{e.stopPropagation();downloadFile(full,i.n)};t3.append(db)}
const ab=document.createElement('button');ab.className='actbtn danger';ab.textContent='Delete';
ab.onclick=async e=>{e.stopPropagation();
const message=i.d?'Permanently delete folder "'+i.n+'" and ALL of its contents? This cannot be undone.':'Permanently delete "'+i.n+'"?';
if(!confirm(message))return;
try{const r=await fetch('/delete?file='+enc(full));if(r.ok){sel.delete(full);toast(i.d?'Folder and contents deleted':'Deleted');nav(cur)}else toast(await r.text(),true)}catch(e){toast('Delete failed',true)}};
t3.append(ab)
tr.append(c0,t1,t2,t3);tb.append(tr)});
$('empty').classList.toggle('hidden',list.length>0);syncSelAll()}
async function nav(p){cur=p;sel.clear();updateSel();$('selAll').checked=false;
try{const d=await jget('/api/list?dir='+enc(p));
items=d.items||[];crumbs(d.path)}catch(e){toast('Not mounted',true);items=[];crumbs(p)}render()}
function newFolder(){const n=prompt('Folder name:');if(!n)return;
fetch('/api/mkdir',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
body:'dir='+enc(cur)+'&name='+enc(n)}).then(async r=>{
if(r.ok){toast('Folder created');nav(cur)}else{toast(await r.text()||'Failed',true)}}).catch(()=>toast('Failed',true))}
async function downloadFile(full,name){
const sp=$('dnSp');
if(!window.ReadableStream){location.href='/download?file='+enc(full);return}
const t0=performance.now();let recv=0,shown=t0,shownRecv=0;
try{
const res=await fetch('/download?file='+enc(full));
if(!res.ok){toast('Download failed: '+name,true);return}
const reader=res.body.getReader();const parts=[];
while(true){const r=await reader.read();if(r.done)break;parts.push(r.value);recv+=r.value.length;
const now=performance.now();
if(now-shown>=250){sp.textContent='\u2193 '+fmtSp((recv-shownRecv)*1000/(now-shown));shown=now;shownRecv=recv}}
const avg=recv*1000/(performance.now()-t0);
sp.textContent='\u2193 '+fmtSp(avg)+' \u00B7 done ('+fmtSize(recv)+')';
const blob=new Blob(parts);const url=URL.createObjectURL(blob);
const a=document.createElement('a');a.href=url;a.download=name;document.body.appendChild(a);a.click();a.remove();
setTimeout(()=>URL.revokeObjectURL(url),4000);
}catch(e){toast('Download failed: '+name,true)}}
async function init(){try{const s=await jget('/api/status');paint(s);
const q=new URLSearchParams(location.search).get('dir');
await nav(q||'/')}catch(e){toast('Device not reachable',true)}
setInterval(async()=>{try{paint(await jget('/api/status'))}catch(e){}},15000)}
function saveWifi(){const s=$('wS').value.trim();if(!s)return toast('SSID required',true);
fetch('/setwifi',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
body:'ssid='+enc(s)+'&pass='+enc($('wP').value)}).then(()=>toast('Saved! Rebooting...'))}
$('fileInput').onchange=e=>{const f=e.target.files[0];if(!f)return;
const fd=new FormData();fd.append('file',f,f.name);
$('upOv').classList.remove('hidden');$('upTxt').textContent='Uploading '+f.name;
const t0=performance.now();
const x=new XMLHttpRequest();x.open('POST','/upload');
x.upload.onprogress=ev=>{if(ev.lengthComputable){
$('upBar').firstElementChild.style.width=Math.round(ev.loaded*100/ev.total)+'%';
const dt=(performance.now()-t0)/1000;if(dt>0.2){const spd=ev.loaded/dt;
$('upTxt').textContent='Uploading '+f.name+' \u00B7 '+fmtSp(spd);
$('upSp').textContent='\u2191 '+fmtSp(spd)}}};
x.onload=()=>{$('upOv').classList.add('hidden');
if(x.status===200){$('upSp').textContent='\u2191 '+fmtSp(f.size*1000/(performance.now()-t0))+' \u00B7 done';
toast('Upload complete');e.target.value='';nav(cur)}else toast('Upload failed',true)};
x.onerror=()=>{$('upOv').classList.add('hidden');toast('Upload failed',true)};
x.send(fd)};
init();
</script></body></html>)rawliteral";

// ============================================================
// Setup
// ============================================================
void setup() {
    Serial.begin(115200);
    Serial.println("\n=== ESP32 NAS with LCD - Starting ===");

    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);

    initDisplay();
    initSD();
    initWiFi();
    setupWebServer();

    Serial.println("=== Setup complete ===");
}

// ============================================================
// Loop
// ============================================================
void loop() {
    unsigned long now = millis();

    if (now - lastSDPoll >= SD_POLL_INTERVAL) {
        lastSDPoll = now;
        pollSD();
    }

    // SD error screen: localized animation + retry stats every 500ms
    if (!sdMounted && now - lastSDAnim >= SD_RETRY_ANIM_INTERVAL) {
        lastSDAnim = now;
        tickSDErrorScreen();
    }

    // Non-blocking WiFi connect/reconnect check
    if (!apMode && !wifiConnected) {
        if (WiFi.status() == WL_CONNECTED) {
            wifiConnected = true;
            wifiConnecting = false;
            wifiFailCount = 0;
            currentIP = WiFi.localIP().toString();
            Serial.printf("[WIFI] Connected! IP: %s\n", currentIP.c_str());
            if (!otaReady) {
                ArduinoOTA.begin();
                otaReady = true;
                Serial.println("[OTA] Ready - upload via PlatformIO or Arduino IDE OTA");
            }
            uiDirty = true;
        } else if (wifiConnecting && millis() - wifiConnectStart >= WIFI_CONNECT_TIMEOUT) {
            wifiConnecting = false;
            Serial.println("[WIFI] Connection timeout, falling back to AP");
            startAPMode();
        } else if (!wifiConnecting && now - lastWifiRetry >= WIFI_RETRY_INTERVAL) {
            lastWifiRetry = now;
            handleWiFiReconnect();
        }
    }

    if (!apMode && wifiConnected && WiFi.status() != WL_CONNECTED) {
        wifiConnected = false;
        otaReady = false;
        currentIP = "";
        Serial.println("[WIFI] Connection lost, will retry...");
        uiDirty = true;
    }

    if (otaReady) {
        ArduinoOTA.handle();
    }

    if (apMode) {
        dnsServer.processNextRequest();
    }

    // Draw ONLY on state change — no periodic repaint, no flicker
    if (uiDirty) {
        updateUI();
        uiDirty = false;
    }

    delay(10);
}

// ============================================================
// Display Init
// ============================================================
void initDisplay() {
    Serial.println("[DISPLAY] Initializing TFT_eSPI...");
    tft.init();
    tft.setRotation(1); // Landscape 320x240
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(2);
    tft.setCursor(10, 10);
    tft.println("ESP32 NAS Starting...");
    Serial.println("[DISPLAY] TFT initialized (320x240, rotation 1)");
}

// ============================================================
// SD Card
// ============================================================
void noteSDFault() {
    if (sdFaultSince == 0) {
        sdFaultSince = millis();
        sdRetryCount = 0;
    }
    uiDirty = true;
}

void initSD() {
    Serial.println("[SD] Initializing SD card on VSPI...");
    SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

    if (SD.begin(SD_CS, SPI, 4000000)) {
        sdMounted = true;
        sdFaultSince = 0;
        sdRetryCount = 0;
        uint64_t totalBytes = SD.totalBytes();
        uint64_t usedBytes = SD.usedBytes();
        sdTotalGB = (float)totalBytes / (1024.0f * 1024.0f * 1024.0f);
        sdFreeGB = (usedBytes <= totalBytes)
            ? (float)(totalBytes - usedBytes) / (1024.0f * 1024.0f * 1024.0f)
            : 0.0f;
        Serial.printf("[SD] Mounted OK - Total: %.2f GB, Free: %.2f GB\n", sdTotalGB, sdFreeGB);
    } else {
        sdMounted = false;
        noteSDFault();
        Serial.println("[SD] Mount FAILED - will retry in loop");
    }
    uiDirty = true;
}

void pollSD() {
    bool wasMounted = sdMounted;

    if (!sdMounted) {
        SD.end();
        SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
        sdRetryCount++;
        if (SD.begin(SD_CS, SPI, 4000000)) {
            sdMounted = true;
            sdFaultSince = 0;
            sdRetryCount = 0;
            uint64_t totalBytes = SD.totalBytes();
            uint64_t usedBytes = SD.usedBytes();
            sdTotalGB = (float)totalBytes / (1024.0f * 1024.0f * 1024.0f);
            sdFreeGB = (usedBytes <= totalBytes)
                ? (float)(totalBytes - usedBytes) / (1024.0f * 1024.0f * 1024.0f)
                : 0.0f;
            Serial.printf("[SD] Re-mounted OK - Total: %.2f GB, Free: %.2f GB\n", sdTotalGB, sdFreeGB);
            uiDirty = true;
        }
    } else {
        uint64_t totalBytes = SD.totalBytes();
        if (totalBytes == 0) {
            sdMounted = false;
            SD.end();
            sdTotalGB = 0.0f;
            sdFreeGB = 0.0f;
            noteSDFault();
            Serial.println("[SD] Card removed or error");
        } else {
            uint64_t usedBytes = SD.usedBytes();
            float newTotal = (float)totalBytes / (1024.0f * 1024.0f * 1024.0f);
            float newFree = (usedBytes <= totalBytes)
                ? (float)(totalBytes - usedBytes) / (1024.0f * 1024.0f * 1024.0f)
                : 0.0f;
            if (fabsf(newTotal - sdTotalGB) > 0.01f || fabsf(newFree - sdFreeGB) > 0.01f) {
                sdTotalGB = newTotal;
                sdFreeGB = newFree;
                uiDirty = true;
            }
        }
    }

    if (wasMounted != sdMounted) {
        uiDirty = true;
    }
}

// ============================================================
// WiFi
// ============================================================
void initWiFi() {
    prefs.begin(PREF_NAMESPACE, true);
    String savedSSID = prefs.getString(PREF_SSID, "");
    String savedPass = prefs.getString(PREF_PASS, "");
    prefs.end();

    WiFi.setHostname(WIFI_HOSTNAME);
    WiFi.setAutoReconnect(false);

    if (savedSSID.length() > 0) {
        Serial.printf("[WIFI] Saved SSID found: %s, connecting...\n", savedSSID.c_str());
        startSTAMode(savedSSID, savedPass);
    } else if (HAS_DEFAULT_CREDENTIALS && strlen(DEFAULT_SSID) > 0) {
        Serial.printf("[WIFI] No saved credentials, trying default SSID: %s\n", DEFAULT_SSID);
        startSTAMode(String(DEFAULT_SSID), String(DEFAULT_PASS));
    } else {
        Serial.println("[WIFI] No saved credentials, starting AP mode");
        startAPMode();
    }
}

void startSTAMode(const String& ssid, const String& pass) {
    apMode = false;
    currentSSID = ssid;
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());
    wifiConnected = false;
    wifiConnecting = true;
    wifiConnectStart = millis();
    wifiFailCount = 0;
    Serial.printf("[WIFI] Connecting to %s...\n", ssid.c_str());
    uiDirty = true;
}

void startAPMode() {
    apMode = true;
    wifiConnected = false;
    currentIP = "";

    currentSSID = AP_SSID;

    WiFi.mode(WIFI_AP);
    WiFi.softAP(currentSSID.c_str());
    currentIP = WiFi.softAPIP().toString();

    dnsServer.start(53, "*", WiFi.softAPIP());

    Serial.printf("[WIFI] AP Mode - SSID: %s, IP: %s\n",
                  currentSSID.c_str(), currentIP.c_str());
    uiDirty = true;
}

void handleWiFiReconnect() {
    if (apMode) return;

    if (wifiFailCount == 0) {
        Serial.println("[WIFI] Attempting reconnect...");
        WiFi.disconnect();
        WiFi.reconnect();
    }

    wifiFailCount++;
    Serial.printf("[WIFI] Waiting for reconnect (attempt %d/%d)\n", wifiFailCount, MAX_WIFI_FAIL);

    if (wifiFailCount >= MAX_WIFI_FAIL) {
        wifiFailCount = 0;
        Serial.println("[WIFI] Max retries reached, falling back to AP");
        startAPMode();
    }
    uiDirty = true;
}

// ============================================================
// Helpers
// ============================================================
String jsonEscape(const String& input) {
    String out = input;
    out.replace("\\", "\\\\");
    out.replace("\"", "\\\"");
    String clean = "";
    for (size_t i = 0; i < out.length(); i++) {
        char c = out.charAt(i);
        if (c >= 0x20 && c != 0x7F) clean += c;
    }
    return clean;
}

String sanitizeFilename(const String& filename) {
    String clean = filename;
    int lastSlash = clean.lastIndexOf('/');
    if (lastSlash >= 0) clean = clean.substring(lastSlash + 1);
    lastSlash = clean.lastIndexOf('\\');
    if (lastSlash >= 0) clean = clean.substring(lastSlash + 1);
    String result = "";
    for (size_t i = 0; i < clean.length(); i++) {
        char c = clean.charAt(i);
        if (c >= 0x20 && c != 0x7F) result += c;
    }
    if (result.length() > 127) result = result.substring(0, 127);
    return result;
}

bool isPathSafe(const String& path) {
    if (path.indexOf("..") >= 0) return false;
    if (path.length() == 0 || path.charAt(0) != '/') return false;
    return true;
}

// Recursively remove a directory's contents, then remove the directory itself.
// The recursion limit and root guard prevent runaway traversal and root deletion.
bool deletePathRecursive(const String& path, uint8_t depth) {
    if (path == "/" || !isPathSafe(path) || depth >= 16) return false;

    File target = SD.open(path, FILE_READ);
    if (!target) return false;
    if (!target.isDirectory()) {
        target.close();
        return SD.remove(path);
    }

    File entry = target.openNextFile();
    while (entry) {
        String childName = String(entry.name());
        const bool isDirectory = entry.isDirectory();
        entry.close();

        // ESP32 FS may return an absolute entry name or a path relative to the parent.
        String childPath;
        if (childName.startsWith("/")) {
            childPath = childName;
        } else {
            childPath = path.endsWith("/") ? path + childName : path + "/" + childName;
        }
        if (!deletePathRecursive(childPath, depth + 1)) {
            target.close();
            return false;
        }
        entry = target.openNextFile();
    }
    target.close();
    return SD.rmdir(path);
}

// ============================================================
// Web Server (Drive-style SPA + JSON API)
// ============================================================
void setupWebServer() {
    server.onNotFound([](AsyncWebServerRequest* request) {
        if (apMode) {
            request->redirect("http://" + WiFi.softAPIP().toString() + "/");
        } else {
            request->send(404, "text/plain", "Not Found");
        }
    });

    server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
        AsyncWebServerResponse* res = request->beginResponse_P(200, "text/html", INDEX_HTML);
        res->addHeader("Cache-Control", "no-cache");
        request->send(res);
    });

    server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* request) {
        String json = "{\"sdMounted\":";
        json += (sdMounted ? "true" : "false");
        json += ",\"totalGB\":" + String(sdTotalGB, 2);
        json += ",\"freeGB\":" + String(sdFreeGB, 2);
        json += ",\"apMode\":";
        json += (apMode ? "true" : "false");
        json += ",\"wifiConnected\":";
        json += (wifiConnected ? "true" : "false");
        json += ",\"ssid\":\"" + jsonEscape(currentSSID) + "\"";
        json += ",\"ip\":\"" + currentIP + "\"";
        json += ",\"hostname\":\"" WIFI_HOSTNAME "\"";
        json += "}";
        request->send(200, "application/json", json);
    });

    server.on("/api/list", HTTP_GET, [](AsyncWebServerRequest* request) {
        if (!sdMounted) {
            request->send(503, "application/json", "{\"error\":\"sd not mounted\"}");
            return;
        }

        String path = "/";
        if (request->hasParam("dir")) {
            path = request->getParam("dir")->value();
        }
        if (!isPathSafe(path)) {
            request->send(403, "application/json", "{\"error\":\"invalid path\"}");
            return;
        }

        File dir = SD.open(path);
        if (!dir || !dir.isDirectory()) {
            request->send(404, "application/json", "{\"error\":\"not found\"}");
            return;
        }

        String json = "{\"path\":\"" + jsonEscape(path) + "\",\"items\":[";
        bool first = true;
        File entry = dir.openNextFile();
        while (entry) {
            String name = entry.name();
            if (name != "") {
                if (!first) json += ",";
                first = false;
                json += "{\"d\":";
                json += (entry.isDirectory() ? "true" : "false");
                json += ",\"n\":\"" + jsonEscape(name) + "\"";
                if (!entry.isDirectory()) {
                    json += ",\"s\":\"" + formatBytes(entry.size()) + "\"";
                }
                json += "}";
            }
            entry.close();
            entry = dir.openNextFile();
        }
        dir.close();
        json += "]}";
        request->send(200, "application/json", json);
    });

    server.on("/api/mkdir", HTTP_POST, [](AsyncWebServerRequest* request) {
        if (!sdMounted) {
            request->send(503, "text/plain", "SD card not mounted");
            return;
        }
        String dir = "/";
        String name = "";
        if (request->hasParam("dir", true)) {
            dir = request->getParam("dir", true)->value();
        }
        if (request->hasParam("name", true)) {
            name = sanitizeFilename(request->getParam("name", true)->value());
        }
        if (name.length() == 0) {
            request->send(400, "text/plain", "Invalid folder name");
            return;
        }
        if (!isPathSafe(dir)) {
            request->send(403, "text/plain", "Invalid path");
            return;
        }
        String fullPath = (dir == "/") ? ("/" + name) : (dir + "/" + name);
        if (SD.exists(fullPath)) {
            request->send(409, "text/plain", "Name already exists");
            return;
        }
        if (SD.mkdir(fullPath)) {
            Serial.printf("[MKDIR] %s\n", fullPath.c_str());
            request->send(200, "text/plain", "Folder created");
        } else {
            request->send(500, "text/plain", "Could not create folder");
        }
    });

    server.on("/setwifi", HTTP_POST, [](AsyncWebServerRequest* request) {
        if (!apMode) {
            request->send(403, "text/plain", "WiFi config only available in setup mode");
            return;
        }

        String ssid = "";
        String pass = "";
        if (request->hasParam("ssid", true)) {
            ssid = request->getParam("ssid", true)->value();
        }
        if (request->hasParam("pass", true)) {
            pass = request->getParam("pass", true)->value();
        }

        if (ssid.length() > 0 && ssid.length() <= 32) {
            prefs.begin(PREF_NAMESPACE, false);
            prefs.putString(PREF_SSID, ssid);
            prefs.putString(PREF_PASS, pass);
            prefs.end();
            Serial.printf("[WIFI] Credentials saved for SSID: %s\n", ssid.c_str());
            request->send(200, "application/json", "{\"ok\":true}");
            delay(2000);
            ESP.restart();
        } else {
            request->send(400, "text/plain", "Invalid SSID (1-32 chars required)");
        }
    });

    server.on("/download", HTTP_GET, [](AsyncWebServerRequest* request) {
        if (!sdMounted || !request->hasParam("file")) {
            request->send(400, "text/plain", "Bad request");
            return;
        }
        String filePath = request->getParam("file")->value();
        if (!isPathSafe(filePath)) {
            request->send(403, "text/plain", "Invalid path");
            return;
        }
        File f = SD.open(filePath, FILE_READ);
        if (!f || f.isDirectory()) {
            request->send(404, "text/plain", "File not found");
            return;
        }
        request->send(f, filePath, "application/octet-stream");
    });

    server.on("/delete", HTTP_GET, [](AsyncWebServerRequest* request) {
        if (!sdMounted || !request->hasParam("file")) {
            request->send(400, "text/plain", "Bad request");
            return;
        }
        String filePath = request->getParam("file")->value();
        if (!isPathSafe(filePath) || filePath == "/") {
            request->send(403, "text/plain", "Invalid path");
            return;
        }
        File target = SD.open(filePath, FILE_READ);
        if (!target) {
            request->send(404, "text/plain", "File or folder not found");
            return;
        }
        bool isDir = target.isDirectory();
        target.close();

        if (deletePathRecursive(filePath)) {
            Serial.printf("[DELETE] Removed %s: %s\n", isDir ? "folder tree" : "file", filePath.c_str());
            request->send(200, "text/plain", isDir ? "Folder and contents deleted" : "File deleted");
        } else {
            request->send(500, "text/plain", "Delete failed; check SD card and folder contents");
        }
    });

    server.on("/upload", HTTP_POST,
        [](AsyncWebServerRequest* request) {
            if (uploadSuccess) {
                request->send(200, "text/plain", "Upload complete");
            } else {
                request->send(500, "text/plain", "Upload failed - SD not mounted or write error");
            }
            uploadSuccess = false;
        },
        [](AsyncWebServerRequest* request, String filename, size_t index, uint8_t* data, size_t len, bool final) {
            static File uploadFile;
            static bool fileOpened = false;

            if (!sdMounted) {
                uploadSuccess = false;
                return;
            }

            if (!index) {
                String safeName = sanitizeFilename(filename);
                if (safeName.length() == 0) {
                    uploadSuccess = false;
                    fileOpened = false;
                    Serial.println("[UPLOAD] Rejected: invalid filename");
                    return;
                }
                String path = "/" + safeName;
                uploadFile = SD.open(path, FILE_WRITE);
                fileOpened = (bool)uploadFile;
                uploadSuccess = fileOpened;
                Serial.printf("[UPLOAD] Start: %s (opened=%d)\n", path.c_str(), fileOpened);
            }

            if (fileOpened && uploadFile && len) {
                size_t written = uploadFile.write(data, len);
                if (written != len) {
                    uploadSuccess = false;
                    Serial.println("[UPLOAD] Write error: partial write");
                }
            }

            if (final) {
                if (uploadFile) uploadFile.close();
                fileOpened = false;
                Serial.printf("[UPLOAD] Done (success=%d)\n", uploadSuccess);
                uiDirty = true;
            }
        }
    );

    server.begin();
    Serial.println("[HTTP] Web server started (ESP32 Drive UI)");
}

// ============================================================
// UI Rendering
// ============================================================
void updateUI() {
    if (!sdMounted) {
        drawSDErrorScreenFull();
    } else if (apMode) {
        drawAPScreen();
    } else {
        drawStatusScreen();
    }
}

void drawStatusScreen() {
    // Neutral graphite surfaces with restrained instrument-panel accents.
    const uint16_t bg = 0x0000;
    const uint16_t panel = 0x18E3;
    const uint16_t panelHi = 0x2945;
    const uint16_t cyan = 0x07FF;
    const uint16_t muted = 0x9CF3;
    const bool online = wifiConnected && WiFi.status() == WL_CONNECTED;
    const int16_t w = tft.width();
    const int16_t h = tft.height();
    const int16_t margin = 6;
    const int16_t innerW = w - margin * 2;
    tft.fillScreen(bg);
    // Subtle grid and edge markers create a technical, non-decorative backdrop.
    for (int16_t x = 16; x < w; x += 32) tft.drawFastVLine(x, 32, h - 52, 0x0841);
    for (int16_t y = 48; y < h - 20; y += 24) tft.drawFastHLine(6, y, w - 12, 0x0841);
    tft.drawRect(2, 2, w - 4, h - 4, 0x2945);

    // Compact HUD-style header with a live system indicator.
    tft.fillRect(0, 0, w, 30, panel);
    tft.fillRect(0, 0, 4, 30, cyan);
    tft.setTextSize(2);
    tft.setTextColor(cyan, panel);
    tft.setCursor(10, 7);
    tft.print("NAS // NODE 01");
    tft.fillCircle(w - 66, 15, 3, online ? TFT_GREEN : TFT_ORANGE);
    tft.setTextSize(1);
    tft.setTextColor(online ? TFT_GREEN : TFT_ORANGE, panel);
    tft.setCursor(w - 57, 11);
    tft.print(online ? "ONLINE" : "LINKING");
    tft.drawFastHLine(6, 31, w - 12, cyan);

    // Network panel.
    const int16_t netY = 37;
    const int16_t netH = 58;
    tft.fillRoundRect(margin, netY, innerW, netH, 5, panel);
    tft.drawRoundRect(margin, netY, innerW, netH, 5, panelHi);
    tft.setTextSize(1);
    tft.setTextColor(cyan, panel);
    tft.setCursor(12, netY + 5);
    tft.print("01 / NETWORK");
    tft.setTextColor(muted, panel);
    tft.setCursor(w - 78, netY + 5);
    tft.print("WLAN STATUS");
    String ssidLine = online ? currentSSID : (wifiConnecting ? "Connecting..." : "Not connected");
    const int16_t textMax = w - 24;
    while (ssidLine.length() && tft.textWidth(ssidLine, 2) > textMax) ssidLine.remove(ssidLine.length() - 1);
    tft.setTextSize(2);
    tft.setTextColor(online ? TFT_GREEN : TFT_ORANGE, panel);
    tft.setCursor(12, netY + 17);
    tft.print(ssidLine);
    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE, panel);
    tft.setCursor(12, netY + 42);
    tft.print("IP  ");
    tft.setTextColor(cyan, panel);
    tft.print(currentIP.length() ? currentIP : "---.---.---.---");

    // Storage telemetry panel with clear numeric hierarchy and segmented meter.
    const int16_t storageY = 101;
    const int16_t storageH = h - storageY - 28;
    float usedGB = sdTotalGB - sdFreeGB;
    if (usedGB < 0) usedGB = 0;
    tft.fillRoundRect(margin, storageY, innerW, storageH, 5, panel);
    tft.drawRoundRect(margin, storageY, innerW, storageH, 5, panelHi);
    tft.setTextSize(1);
    tft.setTextColor(cyan, panel);
    tft.setCursor(12, storageY + 6);
    tft.print("02 / MICROSD STORAGE");
    tft.setTextColor(sdMounted ? TFT_GREEN : TFT_RED, panel);
    tft.setCursor(w - 68, storageY + 6);
    tft.print(sdMounted ? "MOUNTED" : "FAULT");

    const int16_t colW = (innerW - 20) / 3;
    const int16_t colX[3] = {12, (int16_t)(12 + colW), (int16_t)(12 + 2 * colW)};
    const char* labels[3] = {"CAPACITY", "IN USE", "AVAILABLE"};
    const float values[3] = {sdTotalGB, usedGB, sdFreeGB};
    for (int i = 0; i < 3; ++i) {
        if (i > 0) tft.drawFastVLine(colX[i] - 6, storageY + 24, 34, panelHi);
        tft.setTextColor(muted, panel);
        tft.setCursor(colX[i], storageY + 23);
        tft.print(labels[i]);
        tft.setTextSize(2);
        tft.setTextColor(i == 1 ? cyan : TFT_WHITE, panel);
        tft.setCursor(colX[i], storageY + 35);
        tft.printf("%.2f", values[i]);
        tft.setTextSize(1);
    }

    float usedPct = sdTotalGB > 0 ? (usedGB / sdTotalGB) * 100.0f : 0;
    if (usedPct < 0) usedPct = 0;
    if (usedPct > 100) usedPct = 100;
    uint16_t usageColor = usedPct >= 90 ? TFT_RED : (usedPct >= 75 ? TFT_ORANGE : cyan);
    const int16_t barX = 12;
    const int16_t barY = storageY + storageH - 21;
    const int16_t barW = w - 24;
    const int16_t usableW = barW - 4;
    const int16_t fillW = (int16_t)(usableW * usedPct / 100.0f);
    tft.setTextColor(muted, panel);
    tft.setCursor(barX, barY - 11);
    tft.print("VOLUME UTILIZATION");
    tft.setTextColor(usageColor, panel);
    tft.setCursor(w - 45, barY - 11);
    tft.printf("%d%%", (int)(usedPct + 0.5f));
    tft.drawRoundRect(barX, barY, barW, 8, 2, muted);
    if (fillW > 0) tft.fillRoundRect(barX + 2, barY + 2, fillW, 4, 1, usageColor);
    for (int16_t x = barX + 2; x < barX + usableW; x += 12) {
        tft.drawFastVLine(x, barY + 2, 4, bg);
    }

    // Web endpoint stays visible at the bottom edge.
    tft.setTextSize(1);
    tft.setTextColor(muted, bg);
    tft.setCursor(8, h - 16);
    tft.print(online ? String("WEB // http://") + currentIP : String("WEB // WAITING FOR NETWORK"));
    Serial.printf("[UI] Tech status dashboard drawn (%dx%d)\n", w, h);
}

void drawSDErrorScreenFull() {
    const uint16_t bg = 0x0841;
    const uint16_t panel = 0x10A2;
    const uint16_t red = 0xF986;
    const uint16_t muted = 0x9CF3;
    tft.fillScreen(bg);
    tft.fillRect(0, 0, 320, 31, panel);
    tft.fillRect(0, 0, 4, 31, red);
    tft.setTextColor(TFT_WHITE, panel);
    tft.setTextSize(2);
    tft.setCursor(12, 7);
    tft.print("STORAGE // FAULT");

    tft.drawRoundRect(10, 42, 300, 108, 8, panel);
    tft.drawRoundRect(21, 54, 54, 54, 8, red);
    tft.setTextColor(red, panel);
    tft.setTextSize(4);
    tft.setCursor(36, 65);
    tft.print("!");
    tft.setTextColor(TFT_WHITE, bg);
    tft.setTextSize(2);
    tft.setCursor(88, 57);
    tft.print("SD CARD FAILED");
    tft.setTextColor(muted, bg);
    tft.setTextSize(1);
    tft.setCursor(88, 80);
    tft.print("Card not detected or mount rejected");
    tft.setTextColor(TFT_ORANGE, bg);
    tft.setCursor(88, 100);
    tft.print("1. Remove + reinsert the card");
    tft.setCursor(88, 114);
    tft.print("2. Check FAT32 format");
    tft.setCursor(88, 128);
    tft.print("3. Try a different card");

    tft.drawRoundRect(10, 158, 300, 44, 7, panel);
    tft.setTextColor(muted, bg);
    tft.setCursor(22, 167);
    tft.print("RECOVERY AGENT");
    tft.setTextColor(TFT_CYAN, bg);
    tft.setCursor(22, 186);
    tft.print("Retrying every 5 s");

    tft.drawFastHLine(10, 210, 300, panel);
    tft.setTextColor(muted, bg);
    tft.setCursor(12, 220);
    tft.print("NETWORK  ");
    tft.setTextColor(wifiConnected ? TFT_GREEN : TFT_ORANGE, bg);
    tft.print(wifiConnected ? "ONLINE" : (apMode ? "AP" : "LINKING"));

    for (int i = 0; i < 4; ++i) {
        tft.fillCircle(248 + i * 12, 190, 3, panel);
    }
    Serial.println("[UI] SD fault screen painted");
}

void tickSDErrorScreen() {
    const uint16_t bg = 0x0841;
    const uint16_t panel = 0x10A2;

    static uint8_t phase = 0;
    phase = (phase + 1) % 4;
    for (int i = 0; i < 4; ++i) {
        tft.fillCircle(248 + i * 12, 190, 3, i == (int)phase ? TFT_CYAN : panel);
    }

    unsigned long secs = 0;
    if (sdFaultSince > 0) secs = (millis() - sdFaultSince) / 1000UL;
    char line[32];
    snprintf(line, sizeof(line), "ATT %-4d ELAPSED %02lu:%02lu",
             sdRetryCount, (unsigned long)(secs / 60), secs % 60);
    tft.fillRect(10, 205, 300, 6, bg);
    tft.fillRect(120, 216, 190, 9, bg);
    tft.setTextColor(TFT_WHITE, bg);
    tft.setTextSize(1);
    tft.setCursor(150, 220);
    tft.print(line);
}

void drawAPScreen() {
    const uint16_t bg = 0x0841;
    const uint16_t panel = 0x10A2;
    const uint16_t cyan = 0x07FF;
    const uint16_t muted = 0x9CF3;
    tft.fillScreen(bg);
    tft.fillRect(0, 0, 320, 38, panel);
    tft.fillRect(0, 0, 4, 38, cyan);
    tft.setTextColor(cyan, panel);
    tft.setTextSize(2);
    tft.setCursor(12, 11);
    tft.print("NETWORK // SETUP");
    tft.setTextColor(muted, bg);
    tft.setTextSize(1);
    tft.setCursor(14, 50);
    tft.print("CONNECT A DEVICE TO THIS ACCESS POINT");

    String apSsid = currentSSID;
    if (apSsid.length() > 16) apSsid = apSsid.substring(0, 16); // fits at size 3
    tft.drawRoundRect(6, 62, 308, 56, 7, panel);
    tft.setTextColor(muted, bg);
    tft.setCursor(14, 70);
    tft.print("SSID");
    tft.setTextColor(TFT_CYAN, bg);
    tft.setTextSize(3);
    tft.setCursor(14, 84);
    tft.print(apSsid);

    tft.setTextSize(1);
    tft.drawRoundRect(6, 124, 308, 50, 7, panel);
    tft.setTextColor(muted, bg);
    tft.setCursor(14, 132);
    tft.print("PASSWORD");
    tft.setTextColor(TFT_WHITE, bg);
    tft.setTextSize(2);
    tft.setCursor(14, 144);
    tft.print("OPEN - NO KEY");

    tft.drawRoundRect(6, 180, 308, 44, 7, panel);
    tft.setTextColor(muted, bg);
    tft.setTextSize(1);
    tft.setCursor(14, 188);
    tft.print("OPEN CONFIGURATION PORTAL");
    tft.setTextColor(TFT_GREEN, bg);
    tft.setTextSize(2);
    tft.setCursor(14, 200);
    tft.printf("http://%s", currentIP.c_str());

    tft.setTextSize(1);
    tft.setTextColor(muted, bg);
    tft.setCursor(14, 231);
    tft.printf("SD STORAGE  %s", sdMounted ? "READY" : "FAULT / RETRYING");
    Serial.println("[UI] Tech setup screen drawn");
}

// ============================================================
// Utilities
// ============================================================
String formatBytes(uint64_t bytes) {
    if (bytes < 1024) return String(bytes) + " B";
    else if (bytes < 1024ULL * 1024) return String(bytes / 1024.0, 1) + " KB";
    else if (bytes < 1024ULL * 1024 * 1024) return String(bytes / (1024.0 * 1024.0), 1) + " MB";
    else return String((double)bytes / (1024.0 * 1024.0 * 1024.0), 2) + " GB";
}
