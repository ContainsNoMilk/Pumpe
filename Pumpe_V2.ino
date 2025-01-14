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
const char* password = "1";
IPAddress local_ip(192,168,1,1);
IPAddress gateway(192,168,1,1);
IPAddress subnet(255,255,255,0);

DNSServer dnsServer;
WebServer server(80);

// Add these lines near the top of your file, with other constant definitions
const int ledpin = 2;
const int pump1 = 4;
const int pump2 = 16;
const int pump3 = 17;
const int pump4 = 5;

void triggerPump(int secs, int pump);

void setup() {
  Serial.begin(115200);
  pinMode(ledpin, OUTPUT);
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(local_ip, gateway, subnet);
  dnsServer.start(53, "*", local_ip);
  
  // Starte den Filesystem
  WiFi.softAP(ssid, password);
  delay(100); // Kurze Verzögerung
  
  Serial.println("Access Point gestartet");
  Serial.print("IP-Adresse: ");
  Serial.println(WiFi.softAPIP());
  
  // ... (restlicher Setup-Code)
}
/* --------------------------------------------------------------------------
   Pumpenstatus und Kalibrierung
   -------------------------------------------------------------------------- */
bool pumpStatus[4] = {false, false, false, false};
float pumpFlowRate[4] = {0,0,0,0}; // ml/s aus Kalibrierung
unsigned long calibrationStartTime[4] = {0,0,0,0};
bool calibrationRunning[4] = {false, false, false, false};

/* --------------------------------------------------------------------------
   Programmdatenstruktur
   Erweitert um lastRun (time_t) für Intervalle
   -------------------------------------------------------------------------- */
struct Program {
  String days;
  int interval;      // 1 = jede Woche, 2 = alle 2 Wochen, ...
  String time;       // HH:MM
  int amount;        // ml
  bool active;
  int pumpIndex;
  time_t lastRun;    // letzter Ausführungszeitpunkt (Unixzeit), 0 wenn noch nie
};
std::vector<Program> programs;

/* --------------------------------------------------------------------------
   Zeitanzeige
   --------------------------------------------------------------------------
   Wir speichern die Zeit als Unixzeit (time_t) in currentUnixTime.
   Jede Sekunde wird diese um 1 erhöht.
   -------------------------------------------------------------------------- */

time_t currentUnixTime = 0;
unsigned long lastUpdateMillis = 0;

// Wochentage-Kürzel (0=So, 1=Mo, ...)
const char* wdays[7] = {"So","Mo","Di","Mi","Do","Fr","Sa"};

// Konvertierung von String zu Unixzeit
time_t stringToUnixTime(String dt) {
  int year, month, day, hour, minute, second;
  sscanf(dt.c_str(), "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second);
  struct tm t = {0};
  t.tm_year = year - 1900;
  t.tm_mon = month - 1;
  t.tm_mday = day;
  t.tm_hour = hour;
  t.tm_min = minute;
  t.tm_sec = second;
  time_t unixTime = mktime(&t);
  return unixTime;
}

// Unixzeit zu formatiertem String mit Wochentag
String unixTimeToDayString(time_t ut) {
  struct tm *tmStruct = localtime(&ut);

  int wday = tmStruct->tm_wday;        
  int day = tmStruct->tm_mday;        
  int month = tmStruct->tm_mon + 1;   
  int year = tmStruct->tm_year + 1900;
  int hour = tmStruct->tm_hour;
  int min = tmStruct->tm_min;

  char buf[40];
  snprintf(buf, sizeof(buf), "%s %d.%d.%d %02d:%02d", wdays[wday], day, month, year, hour, min);
  return String(buf);
}

String getCurrentDateTime() {
  return unixTimeToDayString(currentUnixTime);
}

/* --------------------------------------------------------------------------
   Konfiguration in SPIFFS speichern und laden
   -------------------------------------------------------------------------- */
void saveConfig() {
  StaticJsonDocument<4096> doc;
  doc["currentDateTime"] = getCurrentDateTime(); // als String speichern

  JsonArray pumpStatusArr = doc.createNestedArray("pumpStatus");
  for (int i=0; i<4; i++) {
    pumpStatusArr.add(pumpStatus[i]);
  }

  JsonArray pumpRateArr = doc.createNestedArray("pumpFlowRate");
  for (int i=0; i<4; i++) {
    pumpRateArr.add(pumpFlowRate[i]);
  }

  JsonArray programsArr = doc.createNestedArray("programs");
  for (auto &prog : programs) {
    JsonObject p = programsArr.createNestedObject();
    p["days"] = prog.days;
    p["interval"] = prog.interval;
    p["time"] = prog.time;
    p["amount"] = prog.amount;
    p["active"] = prog.active;
    p["pumpIndex"] = prog.pumpIndex;
    p["lastRun"] = (long)prog.lastRun;
  }

  File file = SPIFFS.open("/config.json", FILE_WRITE);
  if(!file) {
    Serial.println("Fehler beim Öffnen der config.json zum Schreiben!");
    return;
  }
  serializeJson(doc, file);
  file.close();
  Serial.println("Konfiguration gespeichert.");
}

void loadConfig() {
  if (!SPIFFS.exists("/config.json")) {
    Serial.println("Keine config.json vorhanden, Standardwerte verwendet.");
    return;
  }
  
  File file = SPIFFS.open("/config.json", FILE_READ);
  if (!file) {
    Serial.println("Fehler beim Öffnen der config.json zum Lesen!");
    return;
  }

  StaticJsonDocument<4096> doc;
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  if (error) {
    Serial.println("Fehler beim Parsen der config.json!");
    return;
  }

  if (doc.containsKey("currentDateTime")) {
    String dt = doc["currentDateTime"].as<String>();
    currentUnixTime = stringToUnixTime(dt);
  }

  if (doc.containsKey("pumpStatus")) {
    JsonArray arr = doc["pumpStatus"].as<JsonArray>();
    for (int i=0; i<4 && i<(int)arr.size(); i++) {
      pumpStatus[i] = arr[i].as<bool>();
    }
  }

  if (doc.containsKey("pumpFlowRate")) {
    JsonArray arr = doc["pumpFlowRate"].as<JsonArray>();
    for (int i=0; i<4 && i<(int)arr.size(); i++) {
      pumpFlowRate[i] = arr[i].as<float>();
    }
  }

  if (doc.containsKey("programs")) {
    programs.clear();
    JsonArray parr = doc["programs"].as<JsonArray>();
    for (JsonObject p : parr) {
      Program prog;
      prog.days = p["days"].as<String>();
      prog.interval = p["interval"].as<int>();
      prog.time = p["time"].as<String>();
      prog.amount = p["amount"].as<int>();
      prog.active = p["active"].as<bool>();
      prog.pumpIndex = p["pumpIndex"].as<int>();
      prog.lastRun = p["lastRun"] | 0L;
      programs.push_back(prog);
    }
  }

  Serial.println("Konfiguration geladen.");
}

/* --------------------------------------------------------------------------
   Setter-Funktionen mit automatischer Sicherung
   -------------------------------------------------------------------------- */
void setCurrentDateTime(String newDateTime) {
  currentUnixTime = stringToUnixTime(newDateTime);
  lastUpdateMillis = millis();
  saveConfig();
}

void togglePumpStatus(int idx) {
  if (idx < 0 || idx > 3) return;  // Ensure index is valid

  pumpStatus[idx] = !pumpStatus[idx];
  int pumpPin;
  switch(idx) {
    case 0: pumpPin = pump1; break;
    case 1: pumpPin = pump2; break;
    case 2: pumpPin = pump3; break;
    case 3: pumpPin = pump4; break;
  }
  
  digitalWrite(pumpPin, pumpStatus[idx] ? HIGH : LOW);
  digitalWrite(ledpin, pumpStatus[idx] ? HIGH : LOW);
  
  saveConfig();
}

void updateProgramActiveState(int idx, bool newState) {
  programs[idx].active = newState;
  saveConfig();
}

void addProgram(const Program &prog) {
  programs.push_back(prog);
  saveConfig();
}

void deleteProgram(int idx) {
  if (idx>=0 && idx<(int)programs.size()) {
    programs.erase(programs.begin()+idx);
    saveConfig();
  }
}

void updatePumpFlowRate(int pump, float rate) {
  pumpFlowRate[pump] = rate;
  saveConfig();
}

/* --------------------------------------------------------------------------
   HTML und CSS Erzeugung
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
    display:inline-block; margin:15px; padding:15px 30px; font-size:18px; background-color:green; color:white; border:none; border-radius:10px; text-decoration:none; cursor:pointer;
  }
  .menu-button:hover { background-color:darkgreen; }
  .pump-button {
    padding:15px 30px; font-size:18px; margin:15px; border:none; border-radius:10px; color:white; cursor:pointer; text-transform:uppercase; min-width:100px;
  }
  .pump-button.on { background-color:green; }
  .pump-button.off { background-color:red; }
  .program-block { border:1px solid #ccc; margin:10px; padding:10px; text-align:left; }
  input, select {
    font-size:16px; margin:5px 0; padding:5px; width:80%; max-width:300px; border-radius:5px; border:1px solid #ccc;
  }
  button, .button {
    margin:5px; padding:10px 20px; font-size:16px; border-radius:5px; border:none; cursor:pointer;
  }
  button:hover { opacity:0.9; }
  .delete-button { background-color:red; color:white; }
  .activate-button { background-color:blue; color:white; }
  .add-program-form { border:1px solid #ccc; padding:10px; margin:10px; text-align:center; }

  .day-button, .pump-select-button {
    background-color:#eee; border:1px solid #ccc; border-radius:5px; display:inline-block; margin:5px; padding:10px; cursor:pointer;
  }
  .day-button.active, .pump-select-button.active {
    background-color:green; color:white; 
  }

  .calibration-info { margin-top:20px; font-size:16px; }

  #datetime-overlay {
    position: fixed;
    top:0; left:0; right:0; bottom:0;
    background-color: rgba(0,0,0,0.5);
    display: none; justify-content: center; align-items: center;
    z-index: 9999;
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

String createHeader(String title) {
  return R"=====(<div class="header">
    <a href="/" class="home-button">
      <img src="https://img.icons8.com/ios-filled/50/ffffff/home.png" alt="Home" />
    </a>
    <span class="header-title">)=====" + title + R"=====(</span>
    <span id="datetime" class="header-datetime" onclick="showDateTimeForm()">)=====" + getCurrentDateTime() + R"=====(</span>
  </div>
  <div id="datetime-overlay">
    <div id="datetime-form">
      <h3>Datum und Uhrzeit einstellen</h3>
      <p>Bitte geben Sie Tag, Monat, Jahr, Stunde und Minute ein:</p>
      <div>
        <label>Tag: <input type="number" id="day" min="1" max="31"></label><br>
        <label>Monat: <input type="number" id="month" min="1" max="12"></label><br>
        <label>Jahr: <input type="number" id="year" min="2000" max="2100" value="2024"></label><br>
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
      const day = document.getElementById('day').value;
      const month = document.getElementById('month').value;
      const year = document.getElementById('year').value;
      const hour = document.getElementById('hour').value;
      const minute = document.getElementById('minute').value;

      if(!day || !month || !year || !hour || !minute) {
        alert("Bitte alle Felder ausfüllen!");
        return;
      }

      const dd = day.padStart(2,'0');
      const mm = month.padStart(2,'0');
      const hh = hour.padStart(2,'0');
      const min = minute.padStart(2,'0');
      const newDateTime = `${year}-${mm}-${dd} ${hh}:${min}:00`;

      const response = await fetch(`/set_datetime?datetime=${encodeURIComponent(newDateTime)}`);
      const data = await response.text();
      alert(data);
      location.reload();
    }

    // Jede Sekunde aktualisieren
    setInterval(async function () {
      const response = await fetch('/get_datetime');
      const data = await response.text();
      document.getElementById('datetime').innerText = data;
    }, 1000);
  </script>
  )=====";
}

String createHomePage() {
  return R"=====(<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<title>Startseite</title>
<meta name="viewport" content="width=device-width, initial-scale=1.0">
)=====" + createCSS() + R"=====(</head>
<body>)=====" + createHeader("Startseite") + R"=====(<h1>ESP32 Pumpensteuerung</h1>
<p>Bitte wählen Sie eine Funktion aus:</p>
<div class="section">
  <a href="/manual" class="menu-button">Manuelle Steuerung</a>
  <a href="/calibration" class="menu-button">Kalibrierung</a>
  <a href="/programs" class="menu-button">Programme</a>
</div>
</body>
</html>)=====";
}

String createManualPage() {
  String page = R"=====(<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<title>Manuelle Steuerung</title>
<meta name="viewport" content="width=device-width,initial-scale=1.0">
)=====" + createCSS() + R"=====(</head>
<body>)=====" + createHeader("Manuelle Steuerung") + R"=====(<h1>Manuelle Steuerung</h1>
<p>Tippen Sie auf einen Button, um die Pumpe ein- oder auszuschalten.</p>
<div class="section" id="pumpSection">
)=====";

  for(int i=0; i<4; i++){
    String state = pumpStatus[i] ? "on" : "off";
    String label = "Pumpe " + String(i+1) + " (" + (pumpStatus[i]?"ON":"OFF") + ")";
    page += "<button class='pump-button "+state+"' onclick='togglePump("+String(i)+")'>"+label+"</button><br>";
  }

  page += R"=====(</div>
<script>
async function togglePump(index) {
  const response = await fetch(`/toggle_pump?index=${index}`);
  const data = await response.json();
  updatePumps(data);
}

function updatePumps(data) {
  const section = document.getElementById('pumpSection');
  section.innerHTML = '';
  data.forEach((pump, i) => {
    const state = pump.on ? 'on' : 'off';
    const label = `Pumpe ${i+1} (${pump.on?'ON':'OFF'})`;
    section.innerHTML += `<button class='pump-button ${state}' onclick='togglePump(${i})'>${label}</button><br>`;
  });
}

setInterval(async () => {
  const response = await fetch('/get_pumps');
  const data = await response.json();
  updatePumps(data);
}, 5000);
</script>
</body>
</html>)=====";

  return page;
}

String createCalibrationPage() {
  String page = R"=====(<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<title>Kalibrierung</title>
<meta name="viewport" content="width=device-width,initial-scale=1.0">
)=====" + createCSS() + R"=====(</head>
<body>)=====" + createHeader("Kalibrierung") + R"=====(<h1>Kalibrierung</h1>
<p>Starten Sie die Pumpe und stoppen Sie nach exakt 100ml, um die Flussrate zu berechnen.</p>
<div class="section" id="calibrationSection">
)=====";

  for (int i=0; i<4; i++){
    page += "<h2>Pumpe " + String(i+1) + "</h2>";
    page += "<button class='button' onclick='startCal("+String(i)+")'>Start Kalibrierung</button>";
    page += "<button class='button' onclick='stopCal("+String(i)+")'>Stop Kalibrierung</button>";
    page += "<div class='calibration-info' id='info"+String(i)+"'>Aktuelle Rate: ";
    if(pumpFlowRate[i]>0) {
      page += String(pumpFlowRate[i],2) + " ml/s";
    } else {
      page += "Noch nicht kalibriert";
    }
    page += "</div>";
  }

  page += R"=====(</div>
<script>
async function startCal(i) {
  const r = await fetch(`/start_calibration?pump=${i}`);
  const t = await r.text();
  alert(t);
}

async function stopCal(i) {
  const r = await fetch(`/stop_calibration?pump=${i}`);
  const t = await r.text();
  alert(t);
  location.reload();
}
</script>
</body>
</html>)=====";

  return page;
}

String createProgramsPage() {
  String page = R"=====(<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<title>Programme</title>
<meta name="viewport" content="width=device-width,initial-scale=1.0">
)=====" + createCSS() + R"=====(</head>
<body>)=====" + createHeader("Programme verwalten") + R"=====(<h1>Programme</h1>
<p>Verwalten Sie hier Ihre Programme:</p>
<div class="section">
)=====";

  if (programs.empty()) {
    page += "<p>Es sind keine Programme verfügbar.</p>";
  } else {
    for (size_t i = 0; i < programs.size(); i++) {
      Program &prog = programs[i];
      page += "<div class='program-block'>";
      page += "<strong>Programm " + String(i+1) + ":</strong><br>";
      page += "Wochentage: " + prog.days + "<br>";
      page += "Intervall (Wochen): " + String(prog.interval) + "<br>";
      page += "Uhrzeit: " + prog.time + "<br>";
      page += "Menge: " + String(prog.amount) + " ml<br>";
      page += "Pumpe: Pumpe " + String(prog.pumpIndex+1) + "<br>";
      page += "Letzte Ausführung: " + (prog.lastRun>0 ? unixTimeToDayString(prog.lastRun) : "Noch nie") + "<br>";
      page += "<button class='activate-button' onclick='toggleProgram("+String(i)+")'>"
              + String(prog.active?"Deaktivieren":"Aktivieren") + "</button>";
      page += "<button class='delete-button' onclick='deleteProgram("+String(i)+")'>Löschen</button>";
      page += "</div>";
    }
  }

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
      <label>Intervall (1-4 Wochen):<br><input type="number" id="interval" min="1" max="4" required></label><br>
    </div>
    <div>
      <label>Uhrzeit (HH:MM):<br><input type="text" id="time" placeholder="HH:MM" required></label><br>
    </div>
    <div>
      <label>Menge (ml):<br><input type="number" id="amount" required></label><br>
    </div>
    <div>
      <h3>Pumpe wählen:</h3>
      <div id="pumpButtons">
        <span class="pump-select-button" data-pump="0">Pumpe 1</span>
        <span class="pump-select-button" data-pump="1">Pumpe 2</span>
        <span class="pump-select-button" data-pump="2">Pumpe 3</span>
        <span class="pump-select-button" data-pump="3">Pumpe 4</span>
      </div>
      <input type="hidden" id="pump" name="pump">
    </div>
    <button type="submit">Programm hinzufügen</button>
  </form>
</div>

<script>
const dayButtons = document.querySelectorAll('.day-button');
dayButtons.forEach(btn => {
  btn.addEventListener('click', () => {
    btn.classList.toggle('active');
  });
});

const pumpButtons = document.querySelectorAll('.pump-select-button');
pumpButtons.forEach(btn => {
  btn.addEventListener('click', () => {
    pumpButtons.forEach(b => b.classList.remove('active'));
    btn.classList.add('active');
  });
});

async function toggleProgram(index) {
  const response = await fetch(`/toggle_program?index=${index}`);
  alert(await response.text());
  location.reload();
}

async function deleteProgram(index) {
  if (confirm('Sind Sie sicher, dass Sie dieses Programm löschen möchten?')) {
    const response = await fetch(`/delete_program?index=${index}`);
    alert(await response.text());
    location.reload();
  }
}

async function addProgram(e) {
  e.preventDefault();
  
  const selectedDays = [];
  document.querySelectorAll('.day-button.active').forEach(b => selectedDays.push(b.getAttribute('data-day')));
  
  const pumpBtn = document.querySelector('.pump-select-button.active');
  if(!pumpBtn) {
    alert('Bitte eine Pumpe auswählen!');
    return false;
  }
  const pump = pumpBtn.getAttribute('data-pump');
  
  const days = selectedDays.join(",");
  const interval = document.getElementById('interval').value;
  const time = document.getElementById('time').value;
  const amount = document.getElementById('amount').value;

  const params = new URLSearchParams();
  params.append('days', days);
  params.append('interval', interval);
  params.append('time', time);
  params.append('amount', amount);
  params.append('pump', pump);

  const res = await fetch('/add_program', {
    method: 'POST',
    body: params
  });
  const txt = await res.text();
  alert(txt);
  location.reload();
}
</script>
)=====";

  page += "</div></body></html>";
  return page;
}

/* --------------------------------------------------------------------------
   Zeitgesteuerte Programmausführungen
   --------------------------------------------------------------------------

   Wir prüfen jede Minute, ob ein Programm ausgeführt werden soll:
   - Hole aktuellen Wochentag, Uhrzeit
   - Prüfe ob der aktuelle Wochentag in prog.days enthalten ist
   - Prüfe ob interval erfüllt ist (letzter Lauf + interval*7 Tage <= jetzt?)
   - Prüfe ob Uhrzeit erreicht ist
   - Wenn ja, Pumpe für amount/pumpFlowRate Sekunden einschalten.
   
   Die Pumpenlaufzeit wird dynamisch gesteuert. Wir benötigen dafür:
   - Eine Struktur um den aktuellen Lauf einer Pumpe zu verfolgen:
     startTime, endTime
   Bei Ende Pumpe aus.
   
   Da wir bis jetzt nur pumpStatus haben, erweitern wir die Logik einfach im loop().
   
   Wir führen den Check jede Minute aus (wenn sich die Minute ändert).
--------------------------------------------------------------------------*/

// Strukturen für laufende Pumpläufe
// Wir starten beim Programmlauf die Pumpe für X Sekunden
// endTime: Wann soll die Pumpe wieder aus?
time_t pumpRunEnd[4] = {0,0,0,0}; // 0 = kein lauf

bool isDayInList(String days, const char* dayShort) {
  // days: "Mo,Di,Fr"
  // Suchen wir dayShort z. B. "Mo"
  // Einfache Suche mit indexOf
  // Sichern: dass wir das nur als komplettes Wort finden z. B. "Mo," oder am Ende
  String search = String(dayShort);
  search += ","; // Damit wir Komma am Ende haben
  if (days.indexOf(search) >= 0) return true;
  // Evtl. am Ende ohne Komma? Dann fügen wir Komma an days an
  String daysWithComma = days + ",";
  return (daysWithComma.indexOf(search) >= 0);
}

// Hilfsfunktion um Wochentag, Stunde und Minute aus currentUnixTime zu holen
void getTimeComponents(time_t t, int &wday, int &hour, int &minute) {
  struct tm *tmStruct = localtime(&t);
  wday = tmStruct->tm_wday; // 0=So,...6=Sa
  hour = tmStruct->tm_hour;
  minute = tmStruct->tm_min;
}

time_t lastProgramCheck = 0; // Letzter Check-Zeitpunkt in Unixzeit (minütlich)

// Hilfsfunktion um Programm auszuführen

void runProgram(Program &prog) {
  int pIdx = prog.pumpIndex;
  if (pIdx < 0 || pIdx > 3) {
    Serial.println("Ungültiger Pumpenindex: " + String(pIdx));
    return;
  }
  
  if (pumpFlowRate[pIdx] <= 0) {
    Serial.println("Fehler: Durchflussrate für Pumpe " + String(pIdx + 1) + " ist 0 oder negativ. Bitte kalibrieren Sie die Pumpe.");
    return;
  }
  
  // Berechne Laufzeit basierend auf kalibrierter Durchflussrate
  float sec = prog.amount / pumpFlowRate[pIdx]; // amount ml / (ml/s) = s
  
  if (sec <= 0) {
    Serial.println("Fehler: Berechnete Laufzeit ist 0 oder negativ. Überprüfen Sie die Menge und Durchflussrate.");
    return;
  }
  
  Serial.println("Starte Programm für Pumpe " + String(pIdx + 1) + ":");
  Serial.println("  Menge: " + String(prog.amount) + " ml");
  Serial.println("  Durchflussrate: " + String(pumpFlowRate[pIdx], 2) + " ml/s");
  Serial.println("  Berechnete Laufzeit: " + String(sec, 2) + " s");
  
  triggerPump(sec, pIdx + 1);  // Hier rufen wir die triggerPump Funktion auf
  
  prog.lastRun = currentUnixTime;
  saveConfig();
}

void triggerPump(int secs, int pump) {
  if (pump < 1 || pump > 4) return;  // Ensure pump number is valid
  
  int pumpPin;
  switch(pump) {
    case 1: pumpPin = pump1; break;
    case 2: pumpPin = pump2; break;
    case 3: pumpPin = pump3; break;
    case 4: pumpPin = pump4; break;
  }

  unsigned long end_trigger = millis() + secs * 1000UL;
  while (millis() < end_trigger) {
    digitalWrite(pumpPin, HIGH);
    digitalWrite(ledpin, HIGH);
    pumpStatus[pump-1] = true;
    esp_task_wdt_reset(); // preventing ESP32 resets
    yield();  // Allow other tasks to run
  }
  digitalWrite(pumpPin, LOW);
  digitalWrite(ledpin, LOW);
  pumpStatus[pump-1] = false;
  delay(20);
}

const char* ssid = "Pumpe";
const char* password = "1";
  Serial.begin(115200);

  // WiFi-Initialisierung
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(local_ip, gateway, subnet);
  WiFi.softAP(ssid, password);

  Serial.println("Access Point gestartet");
  Serial.print("IP-Adresse: ");
  Serial.println(WiFi.softAPIP());
  IPAddress local_ip(192,168,1,1);
  IPAddress gateway(192,168,1,1);
  IPAddress subnet(255,255,255,0);
  // Rest des Setup-Codes...
}

/* --------------------------------------------------------------------------
   Loop
   -------------------------------------------------------------------------- */
void loop() {
  dnsServer.processNextRequest();
  server.handleClient();

  // Jede Sekunde Uhr hochzählen
  unsigned long now = millis();
  if (now - lastUpdateMillis >= 1000) {
    lastUpdateMillis += 1000;
    currentUnixTime++;
  }

  // Pumpen abschalten wenn Zeit um ist
  for (int i=0; i<4; i++) {
    if (pumpRunEnd[i] > 0 && currentUnixTime >= pumpRunEnd[i]) {
      pumpStatus[i] = false;
      pumpRunEnd[i] = 0;
      Serial.println("Pumpe "+String(i+1)+" ausgeschaltet, Dosierung beendet.");
      saveConfig();
    }
  }

  // Programme minütlich checken (wenn sich die Minute ändert)
  // Wir vergleichen die aktuelle Minute mit der letzten Check-Minute
  time_t nowMin = currentUnixTime/60; // Einfache Heuristik
  if (nowMin != lastProgramCheck) {
    lastProgramCheck = nowMin;
    // Prüfe Programme
    int wday,hour,minute;
    getTimeComponents(currentUnixTime,wday,hour,minute);

    // Wochentagskürzel:
    const char* dayMap[7] = {"So","Mo","Di","Mi","Do","Fr","Sa"};
    const char* currentDay = dayMap[wday];

    // HH:MM String aus current time bauen
    char hmBuf[6];
    snprintf(hmBuf,sizeof(hmBuf),"%02d:%02d",hour,minute);

    for (auto &prog : programs) {
      if (!prog.active) continue;

      // Prüfen ob heutiger Wochentag enthalten
      if (!isDayInList(prog.days, currentDay)) continue;

      // Prüfe Intervall
      // lastRun + (interval * 7 * 24h *3600) <= currentUnixTime ?
      time_t intervalSec = prog.interval * 7 * 24 * 3600;
      if (prog.lastRun!=0 && (currentUnixTime < (prog.lastRun + intervalSec))) {
        // Noch nicht Zeit für nächsten Lauf
        continue;
      }

      // Uhrzeit check
      if (String(hmBuf) == prog.time) {
        // Passende Uhrzeit!
        // Programm ausführen
        runProgram(prog);
      }
    }
  }
}
