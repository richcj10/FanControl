'use strict';

// ----------------------------------------------------------------
// Constants
// ----------------------------------------------------------------
const NUM_FANS  = 5;
const MAX_RPM   = 3000;    // full-scale RPM on gauge
const POLL_MS   = 2000;

// Gauge geometry (matches SVG viewBox "0 0 110 110", cx=55 cy=55)
const G_R     = 46;
const G_CIRC  = 2 * Math.PI * G_R;   // ~289.03
const G_ARC   = G_CIRC * 0.75;       // 270° sweep  ~216.77

// ----------------------------------------------------------------
// State
// ----------------------------------------------------------------
let errStreak = 0;

// ----------------------------------------------------------------
// Build initial DOM
// ----------------------------------------------------------------
function buildUI() {
  const grid = document.getElementById('fanGrid');
  const tbody = document.getElementById('alarmBody');

  for (let n = 1; n <= NUM_FANS; n++) {
    grid.insertAdjacentHTML('beforeend', fanCardHTML(n));
    tbody.insertAdjacentHTML('beforeend', `
      <tr>
        <td>Fan ${n}</td>
        <td><input type="number" id="mn${n}" value="0" min="0" max="9999"></td>
        <td><input type="number" id="mx${n}" value="0" min="0" max="9999"></td>
      </tr>`);
  }

  // Initialise gauges to zero
  for (let n = 1; n <= NUM_FANS; n++) setGauge(n, 0);
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

  // Colour the RPM number too when hot
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

  data.fans.forEach((f, i) => {
    const n = i + 1;

    // Gauge + RPM
    setGauge(n, f.rpm);

    // PWM bar
    const pct = Math.round(f.pwm / 2.55);
    document.getElementById('gpwm' + n).textContent = `${pct}%  (${f.pwm})`;
    document.getElementById('pf'   + n).style.width = pct + '%';

    // Alarm tags
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
    card.classList.toggle('alarm', hasAlarm);

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
// Config load / save
// ----------------------------------------------------------------
function loadConfig() {
  fetch('/api/config')
    .then(r => r.json())
    .then(d => {
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
// Boot
// ----------------------------------------------------------------
buildUI();
loadConfig();
poll();
setInterval(poll, POLL_MS);
