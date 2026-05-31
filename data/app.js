'use strict';

// ----------------------------------------------------------------
// Constants
// ----------------------------------------------------------------
const NUM_FANS  = 5;
const MAX_RPM   = 3000;
const POLL_MS   = 2000;

const G_R     = 46;
const G_CIRC  = 2 * Math.PI * G_R;
const G_ARC   = G_CIRC * 0.75;

// ----------------------------------------------------------------
// State
// ----------------------------------------------------------------
let errStreak = 0;

// ----------------------------------------------------------------
// Build initial DOM
// ----------------------------------------------------------------
function buildUI() {
  const grid  = document.getElementById('fanGrid');
  const tbody = document.getElementById('alarmBody');
  const dtbody = document.getElementById('defaultsBody');

  for (let n = 1; n <= NUM_FANS; n++) {
    grid.insertAdjacentHTML('beforeend', fanCardHTML(n));

    tbody.insertAdjacentHTML('beforeend', `
      <tr>
        <td>Fan ${n}</td>
        <td><input type="number" id="mn${n}" value="0" min="0" max="9999"></td>
        <td><input type="number" id="mx${n}" value="0" min="0" max="9999"></td>
      </tr>`);

    dtbody.insertAdjacentHTML('beforeend', `
      <tr>
        <td>Fan ${n}</td>
        <td>
          <select id="dm${n}" class="def-select" onchange="toggleDefaultRpm(${n})">
            <option value="pwm">PWM</option>
            <option value="rpm">RPM</option>
            <option value="off">Off</option>
          </select>
        </td>
        <td><input type="number" id="dp${n}" value="64"   min="0"   max="255"  title="PWM duty 0-255"></td>
        <td><input type="number" id="dr${n}" value="1200" min="200" max="9999" title="Target RPM for closed-loop mode"></td>
        <td><input type="number" id="dmin${n}" value="0"  min="0"   max="255"  title="EMC2305 MIN_DRIVE register — stall floor (0 = chip default)"></td>
        <td><input type="number" id="dppr${n}" value="1"  min="1"   max="4"    title="Tach pulses per revolution"></td>
        <td>
          <select id="dsp${n}" class="def-select" title="Spin-up kick: Soft = chip default, Hard = 65% drive extended">
            <option value="soft">Soft</option>
            <option value="hard">Hard</option>
          </select>
        </td>
      </tr>`);
  }

  for (let n = 1; n <= NUM_FANS; n++) {
    setGauge(n, 0);
    toggleDefaultRpm(n);
  }
}

function toggleDefaultRpm(n) {
  const mode = document.getElementById('dm' + n).value;
  const off  = (mode === 'off');
  document.getElementById('dp'   + n).disabled = off || mode === 'rpm';
  document.getElementById('dr'   + n).disabled = off || mode === 'pwm';
  document.getElementById('dmin' + n).disabled = off;
  document.getElementById('dppr' + n).disabled = off;
  document.getElementById('dsp'  + n).disabled = off;
}

function fanCardHTML(n) {
  return `
  <div class="fan-card" id="fc${n}">
    <div class="card-hdr">
      <span class="fan-label">Fan ${n}</span>
      <span class="badge badge--off" id="badge${n}">OFF</span>
    </div>

    <div class="gauge-wrap">
      <svg class="gauge-svg" viewBox="0 0 110 110" aria-hidden="true">
        <circle class="gauge-track" cx="55" cy="55" r="${G_R}"
                stroke-dasharray="${G_ARC.toFixed(2)} ${(G_CIRC - G_ARC).toFixed(2)}"/>
        <circle class="gauge-fill" id="gf${n}" cx="55" cy="55" r="${G_R}"
                stroke-dasharray="0 ${G_CIRC.toFixed(2)}"/>
      </svg>
      <div class="gauge-center">
        <span class="gauge-val" id="grpm${n}">0</span>
        <span class="gauge-unit">RPM</span>
      </div>
    </div>

    <div class="pwm-row">
      <div class="pwm-hdr">
        <span>PWM</span>
        <span class="pwm-num" id="gpwm${n}">0%</span>
      </div>
      <div class="pwm-track"><div class="pwm-fill" id="pf${n}"></div></div>
    </div>

    <div class="alarm-tags" id="at${n}"></div>

    <div class="mode-toggle">
      <button class="mode-btn active" id="bpwm${n}" onclick="setMode(${n},'pwm')">Direct PWM</button>
      <button class="mode-btn"        id="brpm${n}" onclick="setMode(${n},'rpm')">RPM Target</button>
    </div>

    <div class="fan-ctrl" id="ctrl${n}">${sliderHTML(n)}</div>
  </div>`;
}

function sliderHTML(n) {
  return `
    <div class="ctrl-label">
      <span>PWM duty</span>
      <span class="ctrl-label-val" id="slv${n}">0%</span>
    </div>
    <input type="range" min="0" max="100" value="0" id="sl${n}"
           oninput="document.getElementById('slv${n}').textContent=this.value+'%'"
           onchange="sendPwm(${n}, this.value)">`;
}

function rpmCtrlHTML(n) {
  return `
    <div class="ctrl-label"><span>Target RPM</span></div>
    <div class="rpm-row">
      <input class="rpm-input" type="number" id="ri${n}"
             min="200" max="${MAX_RPM}" value="1200" placeholder="RPM">
      <button class="set-btn" onclick="sendTarget(${n})">SET</button>
    </div>`;
}

// ----------------------------------------------------------------
// Gauge
// ----------------------------------------------------------------
function gaugeColor(pct) {
  if (pct < 0.60) return '#00d4ff';
  if (pct < 0.80) return '#ffab40';
  return '#ff4444';
}

function setGauge(n, rpm) {
  const pct  = Math.min(rpm / MAX_RPM, 1);
  const fill = G_ARC * pct;
  const el   = document.getElementById('gf' + n);
  if (!el) return;
  el.style.strokeDasharray = `${fill.toFixed(2)} ${(G_CIRC - fill).toFixed(2)}`;
  el.style.stroke = gaugeColor(pct);

  const valEl = document.getElementById('grpm' + n);
  if (valEl) valEl.textContent = rpm > 0 ? rpm : '0';
  if (valEl) valEl.style.color = pct > 0.79 ? '#ff4444' : '';
}

// ----------------------------------------------------------------
// Fan controls
// ----------------------------------------------------------------
function sendPwm(n, pct) {
  const pwm = Math.round(parseInt(pct, 10) * 255 / 100);
  post('/api/fan/pwm', { fan: n, pwm });
}

function setMode(n, mode) {
  document.getElementById('bpwm' + n).classList.toggle('active', mode === 'pwm');
  document.getElementById('brpm' + n).classList.toggle('active', mode === 'rpm');
  document.getElementById('ctrl' + n).innerHTML =
    mode === 'rpm' ? rpmCtrlHTML(n) : sliderHTML(n);
  post('/api/fan/mode', { fan: n, mode });
}

function sendTarget(n) {
  const rpm = parseInt(document.getElementById('ri' + n).value, 10) || 1200;
  post('/api/fan/target', { fan: n, rpm });
}

// ----------------------------------------------------------------
// Poll /api/status
// ----------------------------------------------------------------
function applyStatus(data) {
  errStreak = 0;
  setConnState(true, data.ip);

  // ESP32 die temperature
  const tempEl = document.getElementById('espTemp');
  if (tempEl && data.espTempC !== undefined) {
    tempEl.textContent = data.espTempC.toFixed(1) + '°C';
    tempEl.title = `ESP32 die: ${data.espTempC.toFixed(1)}°C`;
  }

  // MQTT status badge
  const mqttLed   = document.getElementById('mqttLed');
  const mqttBadge = document.getElementById('mqttBadge');
  const mqttLabel = document.getElementById('mqttLabel');
  if (mqttLed) {
    const on = !!data.mqttOnline;
    mqttLed.className   = 'led ' + (on ? 'ok' : 'err');
    mqttBadge.className = 'conn-badge ' + (on ? 'online' : 'offline');
    mqttLabel.textContent = 'MQTT';
  }

  data.fans.forEach((f, i) => {
    const n = i + 1;

    setGauge(n, f.rpm);

    const pct = Math.round(f.pwm / 2.55);
    document.getElementById('gpwm' + n).textContent = `${pct}%  (${f.pwm})`;
    document.getElementById('pf'   + n).style.width = pct + '%';

    const tags = [];
    if (f.stall)     tags.push('Stall');
    if (f.spinFail)  tags.push('Spin Fail');
    if (f.driveFail) tags.push('Drive Fail');
    if (f.rpmLow)    tags.push('RPM Low');
    if (f.rpmHigh)   tags.push('RPM High');

    const atEl = document.getElementById('at' + n);
    atEl.innerHTML = tags.map(t => `<span class="alarm-tag">${t}</span>`).join('');

    const hasAlarm = tags.length > 0;
    const card  = document.getElementById('fc'    + n);
    const badge = document.getElementById('badge' + n);
    card.classList.toggle('alarm',        hasAlarm);
    card.classList.toggle('disabled-fan', !!f.disabled);

    if (hasAlarm) {
      badge.textContent = 'ALARM';
      badge.className   = 'badge badge--alarm';
    } else if (f.rpm > 0) {
      badge.textContent = 'OK';
      badge.className   = 'badge badge--ok';
    } else {
      badge.textContent = 'OFF';
      badge.className   = 'badge badge--off';
    }
  });

  document.getElementById('lastUpdate').textContent =
    'Updated ' + new Date().toLocaleTimeString();
}

// ----------------------------------------------------------------
// Connection indicator
// ----------------------------------------------------------------
function setConnState(online, ip) {
  const led   = document.getElementById('connLed');
  const lbl   = document.getElementById('connLabel');
  const badge = document.getElementById('connBadge');
  const ipEl  = document.getElementById('ipChip');

  led.className   = 'led ' + (online ? 'ok' : 'err');
  lbl.textContent = online ? 'Online' : 'Offline';
  badge.className = 'conn-badge ' + (online ? 'online' : 'offline');
  if (ip) ipEl.textContent = ip;
}

function poll() {
  fetch('/api/status')
    .then(r => r.json())
    .then(applyStatus)
    .catch(() => {
      if (++errStreak >= 3) setConnState(false, null);
    });
}

// ----------------------------------------------------------------
// Config load / save — MQTT + Alarms
// ----------------------------------------------------------------
function loadConfig() {
  fetch('/api/config')
    .then(r => r.json())
    .then(d => {
      document.getElementById('hostname').value    = d.hostname   ?? 'fancontrol';
      document.getElementById('mqttHost').value   = d.mqttHost   ?? '';
      document.getElementById('mqttPort').value   = d.mqttPort   ?? 1883;
      document.getElementById('mqttUser').value   = d.mqttUser   ?? '';
      document.getElementById('mqttPrefix').value = d.mqttPrefix ?? 'fancontrol';
      if (Array.isArray(d.alarms)) {
        d.alarms.forEach((a, i) => {
          const n = i + 1;
          document.getElementById('mn' + n).value = a.minRpm ?? 0;
          document.getElementById('mx' + n).value = a.maxRpm ?? 0;
        });
      }
      // OTA flags
      if (d.otaWebEnabled    !== undefined) document.getElementById('otaWeb').checked     = d.otaWebEnabled;
      if (d.otaArduinoEnabled !== undefined) document.getElementById('otaArduino').checked = d.otaArduinoEnabled;
    })
    .catch(() => {});
}

function saveMqtt() {
  post('/api/config/mqtt', {
    mqttHost:   document.getElementById('mqttHost').value.trim(),
    mqttPort:   parseInt(document.getElementById('mqttPort').value, 10) || 1883,
    mqttUser:   document.getElementById('mqttUser').value.trim(),
    mqttPass:   document.getElementById('mqttPass').value,
    mqttPrefix: document.getElementById('mqttPrefix').value.trim() || 'fancontrol',
  }).then(d => flash('mqttMsg', d.ok ? '✓ Saved' : '✗ Error'));
}

function saveAlarms() {
  const alarms = [];
  for (let n = 1; n <= NUM_FANS; n++) {
    alarms.push({
      minRpm: parseInt(document.getElementById('mn' + n).value, 10) || 0,
      maxRpm: parseInt(document.getElementById('mx' + n).value, 10) || 0,
    });
  }
  post('/api/config/alarms', { alarms })
    .then(d => flash('alarmMsg', d.ok ? '✓ Saved' : '✗ Error'));
}

// ----------------------------------------------------------------
// Fan defaults
// ----------------------------------------------------------------
function loadFanDefaults() {
  fetch('/api/fan/defaults')
    .then(r => r.json())
    .then(d => {
      if (!Array.isArray(d.fans)) return;
      d.fans.forEach((f, i) => {
        const n = i + 1;
        document.getElementById('dm'   + n).value = f.mode          ?? 'pwm';
        document.getElementById('dp'   + n).value = f.pwm           ?? 64;
        document.getElementById('dr'   + n).value = f.targetRpm     ?? 1200;
        document.getElementById('dmin' + n).value = f.minDrive      ?? 0;
        document.getElementById('dppr' + n).value = f.pulsesPerRev  ?? 1;
        document.getElementById('dsp'  + n).value = f.hardSpinup    ? 'hard' : 'soft';
        toggleDefaultRpm(n);
      });
    })
    .catch(() => {});
}

function saveFanDefaults() {
  const fans = [];
  for (let n = 1; n <= NUM_FANS; n++) {
    fans.push({
      mode:         document.getElementById('dm'   + n).value,
      pwm:          parseInt(document.getElementById('dp'   + n).value, 10) || 64,
      targetRpm:    parseInt(document.getElementById('dr'   + n).value, 10) || 1200,
      minDrive:     parseInt(document.getElementById('dmin' + n).value, 10) || 0,
      pulsesPerRev: parseInt(document.getElementById('dppr' + n).value, 10) || 1,
      hardSpinup:   document.getElementById('dsp' + n).value === 'hard',
    });
  }
  post('/api/fan/defaults', { fans })
    .then(d => flash('defaultsMsg', d.ok ? '✓ Saved' : '✗ Error'));
}

function uploadFanDefaults(input) {
  const file = input.files[0];
  if (!file) return;
  const reader = new FileReader();
  reader.onload = e => {
    let parsed;
    try { parsed = JSON.parse(e.target.result); } catch { flash('defaultsMsg', '✗ Invalid JSON'); return; }
    // Full settings.json upload goes to /api/settings/upload;
    // a fans-only payload falls back to /api/fan/defaults.
    const url = parsed.fans && Object.keys(parsed).length > 1
      ? '/api/settings/upload'
      : '/api/fan/defaults';
    post(url, parsed)
      .then(d => {
        flash('defaultsMsg', d.ok ? '✓ Uploaded' : '✗ Error');
        if (d.ok) { loadConfig(); loadFanDefaults(); }
      });
  };
  reader.readAsText(file);
  input.value = '';
}

function downloadFanDefaults() {
  // Download the full settings.json directly from LittleFS.
  const a = document.createElement('a');
  a.href = '/settings.json';
  a.download = 'settings.json';
  a.click();
}

// ----------------------------------------------------------------
// General settings
// ----------------------------------------------------------------
function saveGeneral() {
  const hostname = document.getElementById('hostname').value.trim().replace(/[^a-zA-Z0-9-]/g, '-') || 'fancontrol';
  post('/api/config/general', { hostname })
    .then(d => {
      if (d.ok) {
        flash('generalMsg', '✓ Saved — rebooting…');
        setTimeout(() => post('/api/reboot', {}), 800);
      } else {
        flash('generalMsg', '✗ Error');
      }
    });
}

// ----------------------------------------------------------------
// OTA settings
// ----------------------------------------------------------------
function saveOta() {
  const body = {
    otaWebEnabled:     document.getElementById('otaWeb').checked,
    otaArduinoEnabled: document.getElementById('otaArduino').checked,
    otaPassword:       document.getElementById('otaPass').value,
  };
  post('/api/config/ota', body)
    .then(d => flash('otaMsg', d.ok ? '✓ Saved — reboot to apply' : '✗ Error'));
}

function rebootDevice() {
  if (!confirm('Reboot the fan controller?')) return;
  post('/api/reboot', {}).catch(() => {});
  flash('otaMsg', 'Rebooting…');
}

// ----------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------
function post(url, body) {
  return fetch(url, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
  })
  .then(r => r.json())
  .catch(() => ({ ok: false }));
}

function flash(id, msg) {
  const el = document.getElementById(id);
  el.textContent = msg;
  el.classList.add('show');
  clearTimeout(el._t);
  el._t = setTimeout(() => { el.textContent = ''; el.classList.remove('show'); }, 3000);
}

// ----------------------------------------------------------------
// Tab switching
// ----------------------------------------------------------------
function switchTab(name) {
  ['dashboard', 'settings'].forEach(t => {
    document.getElementById('tab-' + t).style.display      = (t === name) ? '' : 'none';
    document.getElementById('tab-btn-' + t).classList.toggle('active', t === name);
  });
}

// ----------------------------------------------------------------
// Boot
// ----------------------------------------------------------------
buildUI();
loadConfig();
loadFanDefaults();
poll();
setInterval(poll, POLL_MS);
