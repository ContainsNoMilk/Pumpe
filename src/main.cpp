#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <vector>
#include <FS.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <time.h> // Für struct tm, mktime, localtime
#include <esp_task_wdt.h>

/* --------------------------------------------------------------------------
   Wi-Fi Einstellungen
   -------------------------------------------------------------------------- */
const char* ssid = "Pumpe";
const char* password = "12345678";

IPAddress local_ip(192,168,1,1);
IPAddress gateway(192,168,1,1);
IPAddress subnet(255,255,255,0);

DNSServer dnsServer;
WebServer server(80);

/* --------------------------------------------------------------------------
   Hardware-Pins
   -------------------------------------------------------------------------- */
const int ledpin = 2;
const int pump1  = 4;
const int pump2  = 16;
const int pump3  = 17;
const int pump4  = 5;

/* --------------------------------------------------------------------------
   Tank-Pins
   -------------------------------------------------------------------------- */
// Neuer globaler Wert für deinen Tank (ml)
float currentTankLevel = 0.0f;


/* --------------------------------------------------------------------------
   Pumpenstatus und Kalibrierung
   -------------------------------------------------------------------------- */
bool pumpStatus[4]    = {false, false, false, false};
float pumpFlowRate[4] = {0,0,0,0}; // ml/s
unsigned long calibrationStartTime[4] = {0,0,0,0};
bool calibrationRunning[4] = {false, false, false, false};

/* --------------------------------------------------------------------------
   Programmdatenstruktur
   --------------------------------------------------------------------------
   Jeder Eintrag kann mehrere Pumpen gleichzeitig steuern.
   -------------------------------------------------------------------------- */
struct Program {
  String days;       
  int interval;      
  String time;       
  int amount;        
  bool active;
  bool pumps[4];     // beibehalten
  time_t lastRun;
};


std::vector<Program> programs;

/* --------------------------------------------------------------------------
   Zeitverwaltung
   -------------------------------------------------------------------------- */
time_t currentUnixTime = 0;
unsigned long lastUpdateMillis = 0;

// Wochentage-Kürzel (0=So, 1=Mo, ...)
const char* wdays[7] = {"So","Mo","Di","Mi","Do","Fr","Sa"};

// String -> Unixzeit (Format "YYYY-MM-DD HH:MM:SS")
time_t stringToUnixTime(const String &dt) {
  int year, month, day, hour, minute, second;
  sscanf(dt.c_str(), "%d-%d-%d %d:%d:%d",
         &year, &month, &day, &hour, &minute, &second);
  struct tm t = {0};
  t.tm_year = year - 1900;
  t.tm_mon  = month - 1;
  t.tm_mday = day;
  t.tm_hour = hour;
  t.tm_min  = minute;
  t.tm_sec  = second;
  return mktime(&t);
}

// Unixzeit -> formatierter String mit Wochentag
String unixTimeToDayString(time_t ut) {
  struct tm* tmStruct = localtime(&ut);
  int wday   = tmStruct->tm_wday;        
  int day    = tmStruct->tm_mday;        
  int month  = tmStruct->tm_mon + 1;   
  int year   = tmStruct->tm_year + 1900;
  int hour   = tmStruct->tm_hour;
  int minute = tmStruct->tm_min;

  char buf[40];
  snprintf(buf, sizeof(buf), "%s %d.%d.%d %02d:%02d",
           wdays[wday], day, month, year, hour, minute);
  return String(buf);
}

String getCurrentDateTime() {
  return unixTimeToDayString(currentUnixTime);
}


// Check, ob time in HH:MM format gültig ist
String calculateTankEmptyDate() {
  float usagePerWeek = 0.0f;

  // Hilfsfunktion: Tage zählen in "Mo,Di,Fr"
  auto countDays = [](const String &days) {
    String temp = days + ",";
    int count=0, start=0;
    while(true){
      int idx = temp.indexOf(',', start);
      if(idx<0) break;
      count++;
      start=idx+1;
    }
    return count;
  };

  // Programme durchgehen
  for(auto &pr : programs){
    if(!pr.active) continue;
    int dayCount = countDays(pr.days); 
    float weeklyAmount = dayCount * pr.amount;
    usagePerWeek += weeklyAmount;
  }

  if(usagePerWeek<=0.0f) {
    // Kein Verbrauch => kein LeerDatum
    return "";
  }

  // Wieviel Wochen reicht currentTankLevel?
  float weeks = currentTankLevel / usagePerWeek;
  time_t nowSec   = currentUnixTime;
  time_t deltaSec = (time_t)(weeks * 7 * 24 * 3600);
  time_t emptySec = nowSec + deltaSec;

  struct tm *tmStruct = localtime(&emptySec);
  int d  = tmStruct->tm_mday;
  int mo = tmStruct->tm_mon +1;
  int y  = tmStruct->tm_year+1900;
  int hh = tmStruct->tm_hour;
  int mm = tmStruct->tm_min;
  char buf[40];
  snprintf(buf,sizeof(buf),"%02d.%02d.%04d %02d:%02d", d, mo, y, hh, mm);
  return String(buf);
}

/* --------------------------------------------------------------------------
   Speichern/Laden der Konfiguration in SPIFFS
   -------------------------------------------------------------------------- */
void saveConfig() {
  DynamicJsonDocument doc(4096);

  // Beispiel: doc["currentDateTime"] = ...
  doc["currentDateTime"] = getCurrentDateTime();

  // Neuer Tanklevel
  doc["tankLevel"] = currentTankLevel;

  // Pumpenstatus in ein Array
  {
    // statt createNestedArray("pumpStatus"):
    JsonArray arr = doc["pumpStatus"].to<JsonArray>();
    arr.clear();  // sicherheitshalber leeren, bevor wir einfügen
    for (int i = 0; i < 4; i++) {
      arr.add(pumpStatus[i]);
    }
  }

  // PumpFlowRate in ein Array
  {
    JsonArray arr = doc["pumpFlowRate"].to<JsonArray>();
    arr.clear();
    for (int i = 0; i < 4; i++) {
      arr.add(pumpFlowRate[i]);
    }
  }

  // Programme
  {
    // statt createNestedArray("programs"):
    JsonArray arr = doc["programs"].to<JsonArray>();
    arr.clear();
    for (auto &prog : programs) {
      // statt createNestedObject():
      JsonObject p = arr.createNestedObject();

      p["days"]     = prog.days;
      p["interval"] = prog.interval;
      p["time"]     = prog.time;
      p["amount"]   = prog.amount;
      p["active"]   = prog.active;
      p["lastRun"]  = (long)prog.lastRun;

      // pumps
      JsonArray pa = p["pumps"].to<JsonArray>();
      pa.clear();
      for (int i = 0; i < 4; i++) {
        pa.add(prog.pumps[i]);
      }
    }
  }

  File file = SPIFFS.open("/config.json", FILE_WRITE);
  if(!file) {
    Serial.println("Fehler beim Öffnen /config.json zum Schreiben!");
    return;
  }
  serializeJson(doc, file);
  file.close();
  Serial.println("Konfiguration gespeichert.");
}

void loadConfig() {
  if(!SPIFFS.exists("/config.json")){
    Serial.println("Keine config.json, Standardwerte");
    return;
  }
  File file = SPIFFS.open("/config.json", FILE_READ);
  if(!file){
    Serial.println("Fehler beim Öffnen /config.json zum Lesen!");
    return;
  }

  DynamicJsonDocument doc(4096);
  DeserializationError err = deserializeJson(doc, file);
  file.close();
  if(err) {
    Serial.println("Fehler beim Parsen der config.json!");
    return;
  }

  // TankLevel
  // Keine Notwendigkeit für containsKey("tankLevel"), 
  // man kann direkt checken ob doc["tankLevel"].is<float>()
  if (doc["tankLevel"].is<float>()) {
    currentTankLevel = doc["tankLevel"].as<float>();
  } else {
    currentTankLevel = 0.0f;
  }

  // currentDateTime
  if (doc["currentDateTime"].is<const char*>()) {
    String dtStr = doc["currentDateTime"].as<const char*>();
    currentUnixTime = stringToUnixTime(dtStr);
  }

  // pumpStatus
  {
    JsonArray arr = doc["pumpStatus"].as<JsonArray>();
    if(!arr.isNull()) {
      for(int i=0; i<4 && i<(int)arr.size(); i++){
        pumpStatus[i] = arr[i].as<bool>();
      }
    }
  }

  // pumpFlowRate
  {
    JsonArray arr = doc["pumpFlowRate"].as<JsonArray>();
    if(!arr.isNull()) {
      for(int i=0; i<4 && i<(int)arr.size(); i++){
        pumpFlowRate[i] = arr[i].as<float>();
      }
    }
  }

  // programmes
  {
    JsonArray parr = doc["programs"].as<JsonArray>();
    if(!parr.isNull()) {
      programs.clear();
      for (JsonObject p : parr) {
        Program prog;
        prog.days     = p["days"]    .as<String>();
        prog.interval = p["interval"].as<int>();
        prog.time     = p["time"]    .as<String>();
        prog.amount   = p["amount"]  .as<int>();
        prog.active   = p["active"]  .as<bool>();
        prog.lastRun  = p["lastRun"] | 0L;

        // pumps
        JsonArray pa = p["pumps"].as<JsonArray>();
        if(!pa.isNull()) {
          for(int i=0; i<4 && i<(int)pa.size(); i++){
            prog.pumps[i] = pa[i].as<bool>();
          }
        }
        programs.push_back(prog);
      }
    }
  }

  Serial.println("Konfiguration geladen.");
}


/* --------------------------------------------------------------------------
   Setter-Funktionen mit automatischer Sicherung
   -------------------------------------------------------------------------- */
void setCurrentDateTime(const String &dt) {
  currentUnixTime = stringToUnixTime(dt);
  lastUpdateMillis = millis();
  saveConfig();
}

void togglePumpStatus(int idx) {
  if(idx<0 || idx>3) return;

  pumpStatus[idx] = !pumpStatus[idx];
  int pin;
  switch(idx){
    case 0: pin = pump1; break;
    case 1: pin = pump2; break;
    case 2: pin = pump3; break;
    default: pin = pump4; break;
  }
  digitalWrite(pin, pumpStatus[idx]?HIGH:LOW);

  // LED an, falls mind. eine Pumpe an
  bool anyOn = false;
  for(int i=0; i<4; i++){
    if(pumpStatus[i]) { anyOn=true; break; }
  }
  digitalWrite(ledpin, anyOn ? HIGH : LOW);

  saveConfig();
}

void updateProgramActiveState(int idx, bool newState) {
  if(idx<0 || idx>=(int)programs.size()) return;
  programs[idx].active = newState;
  saveConfig();
}

void addProgram(const Program &prog) {
  programs.push_back(prog);
  saveConfig();
}

void deleteProgram(int idx) {
  if(idx>=0 && idx<(int)programs.size()){
    programs.erase(programs.begin()+idx);
    saveConfig();
  }
}

void updatePumpFlowRate(int p, float rate) {
  pumpFlowRate[p] = rate;
  saveConfig();
}

/* --------------------------------------------------------------------------
   HTML und CSS (unverändert)
   -------------------------------------------------------------------------- */
String createCSS() {
  return R"=====(
<style>
  body { font-family: Arial, sans-serif; margin:0; padding:0; text-align:center; }
  .header {
    display:flex; justify-content:space-between; align-items:center;
    background-color:#333; color:white; padding:10px; position:sticky; top:0;
  }
  .home-button img { width:30px; height:30px; }
  .header-title { font-size:18px; font-weight:bold; }
  .header-datetime { font-size:16px; cursor:pointer; }
  .section { padding:20px; }
  .menu-button {
    display:inline-block; margin:15px; padding:15px 30px; font-size:18px;
    background-color:green; color:white; border:none; border-radius:10px;
    text-decoration:none; cursor:pointer;
  }
  .menu-button:hover { background-color:darkgreen; }
  .pump-button {
    padding:15px 30px; font-size:18px; margin:15px; border:none; border-radius:10px; 
    color:white; cursor:pointer; text-transform:uppercase; min-width:100px;
  }
  .pump-button.on  { background-color:green; }
  .pump-button.off { background-color:red; }
  .program-block { border:1px solid #ccc; margin:10px; padding:10px; text-align:left; }
  input, select {
    font-size:16px; margin:5px 0; padding:5px; width:80%; max-width:300px; 
    border-radius:5px; border:1px solid #ccc;
  }
  button, .button {
    margin:5px; padding:10px 20px; font-size:16px; border-radius:5px; border:none; 
    cursor:pointer;
  }
  button:hover { opacity:0.9; }
  .delete-button   { background-color:red;   color:white; }
  .activate-button { background-color:blue;  color:white; }
  .add-program-form { border:1px solid #ccc; padding:10px; margin:10px; text-align:center; }
  .day-button, .pump-select-button {
    background-color:#eee; border:1px solid #ccc; border-radius:5px; 
    display:inline-block; margin:5px; padding:10px; cursor:pointer;
  }
  .day-button.active, .pump-select-button.active {
    background-color:green; color:white;
  }
  .calibration-info { margin-top:20px; font-size:16px; }
  #datetime-overlay {
    position: fixed; top:0; left:0; right:0; bottom:0; background-color: rgba(0,0,0,0.5);
    display: none; justify-content: center; align-items: center; z-index: 9999;
  }
  #datetime-form {
    background: white; padding: 20px; border-radius:5px; text-align:center;
  }
  #datetime-form input {
    margin: 5px; padding:5px; width:80px;
  }
  #datetime-form button {
    margin: 5px; padding: 10px 20px;
  }
  @media (max-width: 400px) {
    .menu-button, .pump-button { width:100%; box-sizing:border-box; margin:10px 0; }
    input, select { width:90%; }
  }
</style>
)=====";
}

// Kopfbereich mit Datum/Uhrzeit-Einblendung
String createHeader(String title) {
  return R"=====(<div class="header">
    <a href="/" class="home-button">
      <svg xmlns="http://www.w3.org/2000/svg" width="30" height="30" viewBox="0 0 24 24" 
           fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" 
           stroke-linejoin="round">
        <path d="M3 9L12 2L21 9V22H14V15H10V22H3V9Z"></path>
      </svg>
    </a>
    <span class="header-title">)=====" + title + R"=====(</span>
    <span id="datetime" class="header-datetime" onclick="showDateTimeForm()">)====="
    + getCurrentDateTime() + 
    R"=====(</span>
  </div>
  <div id="datetime-overlay">
    <div id="datetime-form">
      <h3>Datum und Uhrzeit einstellen</h3>
      <p>Bitte geben Sie Tag, Monat, Jahr, Stunde und Minute ein:</p>
      <div>
        <label>Tag: <input type="number" id="day" min="1" max="31"></label><br>
        <label>Monat: <input type="number" id="month" min="1" max="12"></label><br>
        <label>Jahr: <input type="number" id="year" min="2000" max="2100" value="2025"></label><br>
        <label>Stunde: <input type="number" id="hour" min="0" max="23"></label><br>
        <label>Minute: <input type="number" id="minute" min="0" max="59"></label>
      </div>
      <button onclick="submitDateTimeForm()">Setzen</button>
      <button onclick="cancelDateTimeForm()">Abbrechen</button>
    </div>
  </div>
  <script>
    function showDateTimeForm() {
      document.getElementById('datetime-overlay').style.display = 'flex';
    }
    function cancelDateTimeForm() {
      document.getElementById('datetime-overlay').style.display = 'none';
    }
    async function submitDateTimeForm() {
      const day    = document.getElementById('day').value;
      const month  = document.getElementById('month').value;
      const year   = document.getElementById('year').value;
      const hour   = document.getElementById('hour').value;
      const minute = document.getElementById('minute').value;
      if(!day||!month||!year||!hour||!minute){
        alert("Bitte alle Felder ausfüllen!");
        return;
      }
      const dd  = day.padStart(2,'0');
      const mm  = month.padStart(2,'0');
      const hh  = hour.padStart(2,'0');
      const min = minute.padStart(2,'0');
      const newDateTime = `${year}-${mm}-${dd} ${hh}:${min}:00`;
      const r = await fetch(`/set_datetime?datetime=${encodeURIComponent(newDateTime)}`);
      alert(await r.text());
      location.reload();
    }
    // Jede Sekunde aktualisieren
    setInterval(async ()=>{
      const r = await fetch('/get_datetime');
      const d = await r.text();
      document.getElementById('datetime').innerText = d;
    },1000);
  </script>
  )=====";
}

// Startseite
String createHomePage() {
  return String(R"=====(<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<title>Startseite</title>
<meta name="viewport" content="width=device-width, initial-scale=1.0">
)=====") 
  + createCSS() 
  + R"=====(</head>
<body>)====="
  + createHeader("Startseite")
  + R"=====(<h1>ESP32 Pumpensteuerung</h1>
<p>Bitte wählen Sie eine Funktion aus:</p>
<div class="section">
  <a href="/manual" class="menu-button">Manuelle Steuerung</a>
  <a href="/calibration" class="menu-button">Kalibrierung</a>
  <a href="/programs" class="menu-button">Programme</a>
  <a href="/tank" class="menu-button">Tankstatus</a> <!-- NEU -->
</div>
<script>
window.onload = async () => {
  let now = new Date();
  let yyyy = now.getFullYear();
  let MM = String(now.getMonth() + 1).padStart(2, '0');
  let dd = String(now.getDate()).padStart(2, '0');
  let hh = String(now.getHours()).padStart(2, '0');
  let mm = String(now.getMinutes()).padStart(2, '0');
  let ss = String(now.getSeconds()).padStart(2, '0');
  let datetime = `${yyyy}-${MM}-${dd} ${hh}:${mm}:${ss}`;
  await fetch(`/set_datetime?datetime=${encodeURIComponent(datetime)}`);
  console.log("Zeit synchronisiert:", datetime);
};
</script>
</body>
</html>)=====";
}


// Manuelle Steuerung
String createManualPage() {
  String page = R"=====(<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<title>Manuelle Steuerung</title>
<meta name="viewport" content="width=device-width,initial-scale=1.0">
)=====" + createCSS() + R"=====(</head>
<body>)====="
  + createHeader("Manuelle Steuerung")
  + R"=====(<h1>Manuelle Steuerung</h1>
<p>Tippen Sie auf einen Button, um die Pumpe ein- oder auszuschalten.</p>
<div class="section" id="pumpSection">
)=====";

  for(int i=0; i<4; i++){
    String state = pumpStatus[i] ? "on" : "off";
    String label= "Pumpe " + String(i+1) + " (" + (pumpStatus[i]?"ON":"OFF") + ")";
    page += "<button class='pump-button "+state+"' onclick='togglePump("+String(i)+")'>"
          + label + "</button><br>";
  }

  page += R"=====(</div>
<script>
async function togglePump(index){
  const response = await fetch(`/toggle_pump?index=${index}`);
  const data = await response.json();
  updatePumps(data);
}
function updatePumps(data){
  const section = document.getElementById('pumpSection');
  section.innerHTML = '';
  data.forEach((p, i)=>{
    const st = p.on ? 'on':'off';
    const label = `Pumpe ${i+1} (${p.on?'ON':'OFF'})`;
    section.innerHTML += `<button class='pump-button ${st}' onclick='togglePump(${i})'>${label}</button><br>`;
  });
}
// Polling alle 5s
setInterval(async ()=>{
  const r = await fetch('/get_pumps');
  const d = await r.json();
  updatePumps(d);
},5000);
</script>
</body></html>)=====";
  return page;
}

// Kalibrierung
String createCalibrationPage() {
  String page = R"=====(<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<title>Kalibrierung</title>
<meta name="viewport" content="width=device-width,initial-scale=1.0">
)=====" + createCSS() 
  + R"=====(</head>
<body>)====="
  + createHeader("Kalibrierung")
  + R"=====(<h1>Kalibrierung</h1>
<p>Starten Sie die Pumpe und stoppen Sie nach exakt 100 ml, um die Flussrate zu berechnen.</p>
<div class="section" id="calibrationSection">
)=====";

  for(int i=0; i<4; i++){
    page += "<h2>Pumpe "+String(i+1)+"</h2>";
    page += "<button class='button' onclick='startCal("+String(i)+")'>Start Kalibrierung</button>";
    page += "<button class='button' onclick='stopCal("+String(i)+")'>Stop Kalibrierung</button>";
    page += "<div class='calibration-info' id='info"+String(i)+"'>Aktuelle Rate: ";
    if(pumpFlowRate[i]>0){
      page += String(pumpFlowRate[i],2)+" ml/s";
    } else {
      page += "Noch nicht kalibriert";
    }
    page += "</div>";
  }

  page += R"=====(</div>
<script>
async function startCal(i){
  let r = await fetch(`/start_calibration?pump=${i}`);
  alert(await r.text());
}
async function stopCal(i){
  let r = await fetch(`/stop_calibration?pump=${i}`);
  alert(await r.text());
  location.reload();
}
</script>
</body></html>)=====";
  return page;
}


// Tank
String createTankPage() {
  String page = R"=====(<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<title>Tankstatus</title>
<meta name="viewport" content="width=device-width,initial-scale=1.0">
)=====";
  page += createCSS();
  page += R"=====(</head><body>)=====";
  page += createHeader("Tankstatus");

  page += "<h1>Aktueller Wasserstand</h1>";
  page += "<p>Derzeitiger Inhalt: " + String(currentTankLevel,1) + " ml</p>";

  // Leer-Datum berechnen
  String emptyDate = calculateTankEmptyDate();
  page += "<p>Voraussichtlich leer: ";
  if(emptyDate.isEmpty()) {
    page += "(keine aktiven Programme oder kein Verbrauch)";
  } else {
    page += emptyDate;
  }
  page += "</p>";

  // Formular zum Setzen eines neuen Wasserstands
  page += R"=====(<h2>Wasserstand aktualisieren</h2>
<form onsubmit="return setTankLevel(event)">
  <label>Neuer Wasserstand (ml):<br>
    <input type="number" id="tankInput" placeholder="z.B. 1000" required>
  </label><br><br>
  <button type="submit">Setzen</button>
</form>
<script>
async function setTankLevel(e){
  e.preventDefault();
  let val = document.getElementById('tankInput').value;
  if(!val){alert("Bitte Wert eingeben.");return false;}
  let resp = await fetch('/update_tank?level='+encodeURIComponent(val));
  let txt  = await resp.text();
  alert(txt);
  location.reload();
}
</script>
)=====";

  page += "</body></html>";
  return page;
}

// Programme-Seite
String createProgramsPage() {
  String page = R"=====(<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<title>Programme</title>
<meta name="viewport" content="width=device-width,initial-scale=1.0">
)=====" + createCSS() + R"=====(</head>
<body>)====="
  + createHeader("Programme verwalten")
  + R"=====(<h1>Programme</h1>
<p>Verwalten Sie hier Ihre Programme:</p>
<div class="section">
)=====";

  if(programs.empty()){
    page += "<p>Es sind keine Programme verfügbar.</p>";
  } else {
    for(size_t i=0; i<programs.size(); i++){
      Program &prog = programs[i];
      page += "<div class='program-block'>";
      page += "<strong>Programm "+String(i+1)+":</strong><br>";
      page += "Wochentage: "+prog.days+"<br>";
      page += "Intervall (Wochen): "+String(prog.interval)+"<br>";
      page += "Uhrzeit: "+prog.time+"<br>";
      page += "Menge: "+String(prog.amount)+" ml<br>";

      // Mehrere Pumpen auflisten
      String pumpList;
      for(int k=0; k<4; k++){
        if(prog.pumps[k]){
          if(!pumpList.isEmpty()) pumpList+=", ";
          pumpList += "Pumpe "+String(k+1);
        }
      }
      if(pumpList.isEmpty()) pumpList="Keine Pumpe ausgewählt";
      page += "Pumpen: "+pumpList+"<br>";

      page += "Letzte Ausführung: "
            + (prog.lastRun>0 ? unixTimeToDayString(prog.lastRun) : "Noch nie")
            + "<br>";

      page += "<button class='activate-button' onclick='toggleProgram("+String(i)+")'>"
            + String(prog.active?"Deaktivieren":"Aktivieren")+"</button>";
      page += "<button class='delete-button' onclick='deleteProgram("+String(i)+")'>Löschen</button>";
      page += "</div>";
    }
  }

  // Formular zum Neuanlegen
  page += R"=====(<div class="add-program-form">
  <h2>Neues Programm hinzufügen</h2>
  <form onsubmit="return addProgram(event)">
    <div>
      <h3>Wochentage wählen:</h3>
      <div id="dayButtons">
        <span class="day-button" data-day="Mo">Mo</span>
        <span class="day-button" data-day="Di">Di</span>
        <span class="day-button" data-day="Mi">Mi</span>
        <span class="day-button" data-day="Do">Do</span>
        <span class="day-button" data-day="Fr">Fr</span>
        <span class="day-button" data-day="Sa">Sa</span>
        <span class="day-button" data-day="So">So</span>
      </div>
      <input type="hidden" id="days" name="days">
    </div>
    <div>
      <label>Intervall (1-4 Wochen):<br>
        <input type="number" id="interval" min="1" max="4" required>
      </label><br>
    </div>
    <div>
      <label>Uhrzeit (HH:MM):<br>
        <input type="text" id="time" placeholder="HH:MM" required>
      </label><br>
    </div>
    <div>
      <label>Menge (ml):<br>
        <input type="number" id="amount" required>
      </label><br>
    </div>
    <div>
      <h3>Pumpe(n) wählen:</h3>
      <div id="pumpButtons">
        <span class="pump-select-button" data-pump="0">Pumpe 1</span>
        <span class="pump-select-button" data-pump="1">Pumpe 2</span>
        <span class="pump-select-button" data-pump="2">Pumpe 3</span>
        <span class="pump-select-button" data-pump="3">Pumpe 4</span>
      </div>
      <input type="hidden" id="pumps">
    </div>
    <button type="submit">Programm hinzufügen</button>
  </form>
</div>

<script>
const dayBtns = document.querySelectorAll('.day-button');
dayBtns.forEach(btn=>{
  btn.addEventListener('click', ()=>{
    btn.classList.toggle('active');
  });
});
const pumpBtns = document.querySelectorAll('.pump-select-button');
pumpBtns.forEach(btn=>{
  btn.addEventListener('click', ()=>{
    btn.classList.toggle('active');
  });
});

async function toggleProgram(idx){
  let r = await fetch(`/toggle_program?index=${idx}`);
  alert(await r.text());
  location.reload();
}
async function deleteProgram(idx){
  if(confirm("Wirklich löschen?")){
    let r = await fetch(`/delete_program?index=${idx}`);
    alert(await r.text());
    location.reload();
  }
}

async function addProgram(e){
  e.preventDefault();
  // Wochentage
  const selectedDays=[];
  document.querySelectorAll('.day-button.active')
    .forEach(b=>selectedDays.push(b.getAttribute('data-day')));
  const daysStr = selectedDays.join(",");

  // Pumpen
  const selectedPumps=[];
  document.querySelectorAll('.pump-select-button.active')
    .forEach(b=>selectedPumps.push(b.getAttribute('data-pump')));
  if(!selectedPumps.length){
    alert("Bitte mindestens eine Pumpe auswählen!");
    return false;
  }
  const pumpStr = selectedPumps.join(",");

  const interval= document.getElementById('interval').value;
  const time   = document.getElementById('time').value;
  const amount = document.getElementById('amount').value;

  const params = new URLSearchParams();
  params.append('days',daysStr);
  params.append('interval', interval);
  params.append('time', time);
  params.append('amount', amount);
  params.append('pumps', pumpStr);

  let r = await fetch('/add_program', {method:'POST', body:params});
  let txt=await r.text();
  alert(txt);
  location.reload();
}
</script>
)=====";

  page += "</div></body></html>";
  return page;
}

/* --------------------------------------------------------------------------
   Nicht-blockierender Pumpenlauf
   --------------------------------------------------------------------------*/
time_t pumpRunEnd[4] = {0,0,0,0};

// Pumpe i starten für durationSec Sekunden
void startPumpTimed(int i, float durationSec){
  if(i<0||i>3||durationSec<=0) return;

  pumpStatus[i] = true;
  int pin;
  switch(i){
    case 0: pin=pump1; break;
    case 1: pin=pump2; break;
    case 2: pin=pump3; break;
    default: pin=pump4; break;
  }
  digitalWrite(pin, HIGH);

  // LED an
  digitalWrite(ledpin, HIGH);

  time_t endTime = currentUnixTime + (time_t)round(durationSec);
  if(endTime>pumpRunEnd[i]){
    pumpRunEnd[i] = endTime;
  }
}

// Pumpe i ausschalten
void stopPump(int i){
  if(i<0||i>3) return;
  pumpStatus[i] = false;
  pumpRunEnd[i] = 0;

  int pin;
  switch(i){
    case 0: pin=pump1; break;
    case 1: pin=pump2; break;
    case 2: pin=pump3; break;
    default: pin=pump4; break;
  }
  digitalWrite(pin, LOW);

  // LED aus, wenn keine Pumpe mehr an
  bool anyOn=false;
  for(int k=0;k<4;k++){
    if(pumpStatus[k]) { anyOn=true; break; }
  }
  if(!anyOn) digitalWrite(ledpin, LOW);
}

// Programm ausführen (alle angehakten Pumpen)
void runProgram(Program &prog){
  Serial.println("Starte Programm: "+prog.days
    +", time="+prog.time
    +", amount="+String(prog.amount));
  for(int i=0; i<4; i++){
    if(prog.pumps[i]){
      if(pumpFlowRate[i]<=0){
        Serial.println("WARNUNG: Pumpe "+String(i+1)+" Flow=0 => skip");
        continue;
      }
      float sec = prog.amount / pumpFlowRate[i];
      
      // NEU: Tankstand verringern
      currentTankLevel -= prog.amount;
      if(currentTankLevel<0) currentTankLevel=0;
      Serial.println(" -> Pumpe "+String(i+1)+" "+String(sec,1)+"s, Tank="+String(currentTankLevel,1)+" ml");

      // Pumpe starten
      startPumpTimed(i, sec);
    }
  }
  prog.lastRun = currentUnixTime;
  saveConfig();
}

/* --------------------------------------------------------------------------
   Hilfsfunktionen
   --------------------------------------------------------------------------*/
// Check, ob dayShort in days (z.B. "Mo,Di,Fr") enthalten ist
bool isDayInList(const String &days, const char* dayShort){
  String withComma = days + ",";
  String look = String(dayShort)+",";
  return (withComma.indexOf(look)>=0);
}



// Wochentag, Stunde, Minute aus currentUnixTime
void getTimeComponents(time_t t, int &wday, int &hour, int &minute){
  struct tm *tmStruct = localtime(&t);
  wday   = tmStruct->tm_wday; // 0=So,...6=Sa
  hour   = tmStruct->tm_hour;
  minute = tmStruct->tm_min;
}

/* --------------------------------------------------------------------------
   setup()
   --------------------------------------------------------------------------*/
time_t lastProgramCheck = 0;

void setup() {
  Serial.begin(115200);
  delay(100);

  pinMode(ledpin, OUTPUT);
  pinMode(pump1,  OUTPUT);
  pinMode(pump2,  OUTPUT);
  pinMode(pump3,  OUTPUT);
  pinMode(pump4,  OUTPUT);

  // TZ
  setenv("TZ","UTC0",1);
  tzset();

  // SPIFFS mounten
  if(!SPIFFS.begin(true)){
    Serial.println("SPIFFS konnte nicht gemountet werden!");
  }
  loadConfig();

  // Alte AP-Daten ignorieren
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ssid, password);
  WiFi.softAPConfig(local_ip, gateway, subnet);
  Serial.println("Access Point SSID: " + String(WiFi.softAPSSID())
    + " / IP: "+WiFi.softAPIP().toString());

  dnsServer.start(53, "*", local_ip);

  // Routen
  server.on("/", [](){
    server.send(200, "text/html; charset=UTF-8", createHomePage());
  });
  server.on("/manual", [](){
    server.send(200, "text/html; charset=UTF-8", createManualPage());
  });
  server.on("/calibration", [](){
    server.send(200, "text/html; charset=UTF-8", createCalibrationPage());
  });
  server.on("/programs", [](){
    server.send(200, "text/html; charset=UTF-8", createProgramsPage());
  });
  server.on("/tank", [](){
  server.send(200,"text/html; charset=UTF-8", createTankPage());
  });
  server.on("/update_tank", [](){
  if(!server.hasArg("level")){
    server.send(400,"text/plain","Missing level");
    return;
  }
  float newLevel = server.arg("level").toFloat();
  if(newLevel<0) newLevel=0;
  
  currentTankLevel = newLevel;
  saveConfig();
  server.send(200,"text/plain","Wasserstand aktualisiert auf "+
              String(currentTankLevel,1)+" ml");
});


  // AJAX Endpoints
  server.on("/get_pumps", [](){
    String json="[";
    for(int i=0;i<4;i++){
      if(i>0) json+=",";
      json+="{\"on\":"+String(pumpStatus[i]?"true":"false")+"}";
    }
    json+="]";
    server.send(200,"application/json",json);
  });

  server.on("/toggle_pump", [](){
    if(!server.hasArg("index")){
      server.send(400,"text/plain","Missing index");
      return;
    }
    int idx=server.arg("index").toInt();
    if(idx<0||idx>3){
      server.send(400,"text/plain","Invalid index");
      return;
    }
    togglePumpStatus(idx);

    // Rückgabe
    String json="[";
    for(int i=0;i<4;i++){
      if(i>0) json+=",";
      json+="{\"on\":"+String(pumpStatus[i]?"true":"false")+"}";
    }
    json+="]";
    server.send(200,"application/json",json);
  });

  // Kalibrierung
  server.on("/start_calibration", [](){
    if(!server.hasArg("pump")){
      server.send(400,"text/plain","Missing pump");
      return;
    }
    int p = server.arg("pump").toInt();
    if(p<0||p>3){
      server.send(400,"text/plain","Invalid pump");
      return;
    }
    calibrationStartTime[p] = millis();
    calibrationRunning[p] = true;

    // Pumpe an
    int pin;
    switch(p){
      case 0: pin=pump1; break;
      case 1: pin=pump2; break;
      case 2: pin=pump3; break;
      default: pin=pump4; break;
    }
    digitalWrite(pin, HIGH);
    pumpStatus[p] = true;

    server.send(200,"text/plain","Kalibrierung für Pumpe "+String(p+1)+" gestartet.");
  });

  server.on("/stop_calibration", [](){
    if(!server.hasArg("pump")){
      server.send(400,"text/plain","Missing pump");
      return;
    }
    int p=server.arg("pump").toInt();
    if(p<0||p>3){
      server.send(400,"text/plain","Invalid pump");
      return;
    }
    if(!calibrationRunning[p]){
      server.send(400,"text/plain","Kalibrierung wurde nicht gestartet.");
      return;
    }
    unsigned long duration = millis() - calibrationStartTime[p];
    calibrationRunning[p] = false;

    // Pumpe aus
    int pin;
    switch(p){
      case 0: pin=pump1; break;
      case 1: pin=pump2; break;
      case 2: pin=pump3; break;
      default: pin=pump4; break;
    }
    digitalWrite(pin, LOW);
    pumpStatus[p] = false;

    float durationSec = (float)duration/1000.0;
    float rate = 100.0 / durationSec; // 100 ml / Dauer
    updatePumpFlowRate(p, rate);

    server.send(200,"text/plain",
      "Kalibrierung für Pumpe "+String(p+1)+" gestoppt. Dauer: "
      +String(durationSec,2)+" s. Rate: "+String(rate,2)+" ml/s.");
  });

  // Programme
  server.on("/toggle_program", [](){
    if(!server.hasArg("index")){
      server.send(400,"text/plain","Missing index");
      return;
    }
    int idx=server.arg("index").toInt();
    if(idx<0||idx>=(int)programs.size()){
      server.send(400,"text/plain","Invalid index");
      return;
    }
    bool newState = !programs[idx].active;
    updateProgramActiveState(idx,newState);
    server.send(200,"text/plain","Programm "+String(idx+1)
      +" ist jetzt "+(newState?"aktiv":"inaktiv")+".");
  });

  server.on("/delete_program", [](){
    if(!server.hasArg("index")){
      server.send(400,"text/plain","Missing index");
      return;
    }
    int idx=server.arg("index").toInt();
    if(idx<0||idx>=(int)programs.size()){
      server.send(400,"text/plain","Invalid index");
      return;
    }
    deleteProgram(idx);
    server.send(200,"text/plain","Programm "+String(idx+1)+" wurde gelöscht.");
  });

  // add_program => Mehrere Pumpen
  server.on("/add_program", HTTP_POST, [](){
    if(!server.hasArg("days")||!server.hasArg("interval")||!server.hasArg("time")
       ||!server.hasArg("amount")||!server.hasArg("pumps")){
      server.send(400,"text/plain","Fehlende Parameter");
      return;
    }
    Program prog;
    prog.days     = server.arg("days");
    prog.interval = server.arg("interval").toInt();
    prog.time     = server.arg("time");
    prog.amount   = server.arg("amount").toInt();
    prog.active   = false;
    prog.lastRun  = 0;

    // pumps[] reset
    for(int i=0;i<4;i++){
      prog.pumps[i] = false;
    }
    // Beispiel: "0,2" => Pumpe1+3
    String pStr = server.arg("pumps");
    int start=0;
    while(true){
      int commaIndex = pStr.indexOf(',', start);
      String val = (commaIndex==-1)
                    ? pStr.substring(start)
                    : pStr.substring(start, commaIndex);
      val.trim();
      if(val.length()>0){
        int idx = val.toInt();
        if(idx>=0 && idx<4){
          prog.pumps[idx] = true;
        }
      }
      if(commaIndex==-1) break;
      start = commaIndex+1;
    }

    addProgram(prog);
    server.send(200,"text/plain","Programm hinzugefügt.");
  });

  // Datum/Uhrzeit
  server.on("/get_datetime", [](){
    server.send(200,"text/plain", getCurrentDateTime());
  });
  server.on("/set_datetime", [](){
    if(!server.hasArg("datetime")){
      server.send(400,"text/plain","Missing datetime");
      return;
    }
    setCurrentDateTime(server.arg("datetime"));
    server.send(200,"text/plain","Datum und Uhrzeit wurden gesetzt.");
  });

  // Not-Found-Handler: Leitet unbekannte Anfragen auf die Startseite um
  server.onNotFound([](){
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
  });
  
  server.begin();
  Serial.println("HTTP Server gestartet.");
}

/* --------------------------------------------------------------------------
   loop()
   --------------------------------------------------------------------------*/
void loop() {
  dnsServer.processNextRequest();
  server.handleClient();

  // Sekundentakt
  unsigned long nowMs = millis();
  if(nowMs - lastUpdateMillis >= 1000){
    lastUpdateMillis += 1000;
    currentUnixTime++;
  }

  // Pumpen abschalten, deren Zeit vorbei ist
  for(int i=0; i<4; i++){
    if(pumpStatus[i] && pumpRunEnd[i]>0){
      if(currentUnixTime >= pumpRunEnd[i]){
        Serial.println("Pumpe "+String(i+1)+" Lauf abgelaufen => AUS");
        stopPump(i);
      }
    }
  }

  // Programme minütlich checken
  time_t nowMin = currentUnixTime/60;
  if(nowMin != lastProgramCheck){
    lastProgramCheck = nowMin;

    int wday,hour,minute;
    getTimeComponents(currentUnixTime, wday, hour, minute);
    const char* dayMap[7] = {"So","Mo","Di","Mi","Do","Fr","Sa"};
    const char* currentDay = dayMap[wday];

    char hmBuf[6];
    snprintf(hmBuf,sizeof(hmBuf),"%02d:%02d",hour,minute);

    // Serial.println("== Program check: " + String(currentDay)+" "+String(hmBuf));
    for(auto &prog : programs){
      if(!prog.active) continue;
      if(!isDayInList(prog.days, currentDay)) continue;

      time_t intervalSec = prog.interval * 7 * 24 * 3600;
      if(prog.lastRun!=0 && currentUnixTime<(prog.lastRun+intervalSec)){
        continue;
      }
      if(String(hmBuf)==prog.time){
        runProgram(prog);
      }
    }
  }

  esp_task_wdt_reset();
  yield();
}
