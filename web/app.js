'use strict';

/* ======================================================================
   State
   ====================================================================== */
const S = {
  segments: [],
  presets: [],
  config: {},
  status: {},
  diag: {},
  ota: { available: false, current: '—', latest: '—' },
};

/* ======================================================================
   API Helpers
   ====================================================================== */
async function apiGet(url) {
  const r = await fetch(url);
  if (!r.ok) throw new Error(`${r.status} ${r.statusText}`);
  return r.json();
}

async function apiPost(url, body) {
  const r = await fetch(url, {
    method: 'POST',
    headers: body !== undefined ? { 'Content-Type': 'application/json' } : {},
    body: body !== undefined ? JSON.stringify(body) : undefined,
  });
  if (!r.ok) {
    let msg = `${r.status}`;
    try { const j = await r.json(); msg = j.error || msg; } catch(_) {}
    throw new Error(msg);
  }
  return r.json();
}

/* Debounce helper — one timer per key */
const _dtimers = {};
function debounce(key, fn, ms = 250) {
  clearTimeout(_dtimers[key]);
  _dtimers[key] = setTimeout(fn, ms);
}

/* ======================================================================
   Toast notifications
   ====================================================================== */
function toast(msg, type = '') {
  const el = document.createElement('div');
  el.className = `toast ${type}`;
  el.textContent = msg;
  document.getElementById('toast-container').appendChild(el);
  setTimeout(() => el.remove(), 3200);
}

/* ======================================================================
   Color Helpers
   ====================================================================== */

/* Convert segment state → CSS hsl() for glow bar */
function segToColor(seg) {
  if (!seg.on) return null;
  const lvl = Math.max(0.2, seg.level / 254);
  if (seg.mode === 'ct') {
    /* mireds 153 (6500K cool) → 500 (2000K warm), mapped to hue 210→25 */
    const t = (seg.ct - 153) / (500 - 153);
    const h = Math.round(210 - t * 185);
    const s = Math.round(55 + t * 35);
    const l = Math.round((40 + t * 20) * lvl + 12);
    return `hsl(${h},${s}%,${Math.min(l, 75)}%)`;
  } else {
    const s = Math.round(seg.sat / 254 * 90 + 10);
    const l = Math.round(50 * lvl + 8);
    return `hsl(${seg.hue},${s}%,${Math.min(l, 65)}%)`;
  }
}

/* For preset visualization: color for one segment slot */
function presetSegColor(seg) {
  if (!seg.on) return 'var(--border)';
  const c = segToColor(seg);
  return c || '#f59e0b44';
}

/* ======================================================================
   Header — status
   ====================================================================== */
function applyStatus(s) {
  S.status = s;
  /* hostname from URL */
  document.getElementById('host-label').textContent = window.location.hostname || 'led-ctrl.local';

  /* firmware badge */
  const fwEl = document.getElementById('fw-badge');
  fwEl.textContent = s.firmware || '—';

  /* wifi badge */
  const dot  = document.getElementById('wifi-dot');
  const txt  = document.getElementById('wifi-text');
  const badge = document.getElementById('wifi-badge');
  const wMap = {
    connected:  { cls: 'ok',   dot: 'ok',   label: 'connected' },
    ap:         { cls: 'warn', dot: 'warn',  label: 'AP mode' },
    connecting: { cls: 'warn', dot: 'warn',  label: 'connecting' },
    init:       { cls: 'dim',  dot: '',      label: 'init' },
    failed:     { cls: 'err',  dot: 'err',   label: 'failed' },
  };
  const w = wMap[s.wifi] || { cls: 'dim', dot: '', label: s.wifi || '—' };
  badge.className = `badge badge-${w.cls}`;
  dot.className   = `dot ${w.dot}`;
  txt.textContent = w.label;
}

/* ======================================================================
   Segments Tab
   ====================================================================== */
function buildSegCards() {
  const grid = document.getElementById('seg-grid');
  grid.innerHTML = '';
  S.segments.forEach(seg => grid.appendChild(makeSegCard(seg)));
}

function refreshSegCard(seg) {
  const card = document.getElementById(`seg-${seg.index}`);
  if (!card) return;
  updateSegCardUI(card, seg);
}

function makeSegCard(seg) {
  const idx = seg.index;
  const div = document.createElement('div');
  div.className = `seg-card${seg.on ? ' seg-on' : ''}`;
  div.id = `seg-${idx}`;

  div.innerHTML = `
    <div class="seg-glow-bar" id="glow-${idx}"></div>
    <div class="seg-body">
      <div class="seg-header">
        <div class="seg-title-group">
          <span class="seg-title">Segment ${idx + 1}</span>
          <span class="seg-num">#${idx}</span>
        </div>
        <label class="toggle" title="On/Off">
          <input type="checkbox" id="seg-on-${idx}" ${seg.on ? 'checked' : ''}>
          <span class="toggle-track"></span>
        </label>
      </div>

      <div class="ctrl-row">
        <span class="ctrl-label">Level</span>
        <input type="range" class="sl-level" id="seg-lvl-${idx}" min="1" max="254" value="${seg.level}">
        <span class="ctrl-val" id="seg-lvl-val-${idx}">${seg.level}</span>
      </div>

      <div class="ctrl-row">
        <span class="ctrl-label">Mode</span>
        <div class="mode-toggle">
          <button class="mode-btn${seg.mode === 'hs' ? ' active' : ''}" data-mode="hs" data-idx="${idx}">HS</button>
          <button class="mode-btn${seg.mode === 'ct' ? ' active' : ''}" data-mode="ct" data-idx="${idx}">CT</button>
        </div>
      </div>

      <div id="seg-hs-${idx}" class="${seg.mode === 'hs' ? '' : 'hidden'}">
        <div class="ctrl-row">
          <span class="ctrl-label">Hue</span>
          <input type="range" class="sl-hue" id="seg-hue-${idx}" min="0" max="360" value="${seg.hue}">
          <span class="ctrl-val" id="seg-hue-val-${idx}">${seg.hue}°</span>
        </div>
        <div class="ctrl-row">
          <span class="ctrl-label">Sat</span>
          <input type="range" id="seg-sat-${idx}" min="0" max="254" value="${seg.sat}">
          <span class="ctrl-val" id="seg-sat-val-${idx}">${seg.sat}</span>
        </div>
      </div>

      <div id="seg-ct-${idx}" class="${seg.mode === 'ct' ? '' : 'hidden'}">
        <div class="ctrl-row">
          <span class="ctrl-label">Temp</span>
          <input type="range" class="sl-ct" id="seg-ct-${idx}-r" min="153" max="500" value="${seg.ct}">
          <span class="ctrl-val" id="seg-ct-val-${idx}">${Math.round(1000000 / seg.ct)}K</span>
        </div>
      </div>

      <div class="seg-startup-row">
        <span class="ctrl-label">Start</span>
        <select id="seg-startup-${idx}">
          <option value="off"      ${seg.startup === 'off'      ? 'selected' : ''}>Off</option>
          <option value="on"       ${seg.startup === 'on'       ? 'selected' : ''}>On</option>
          <option value="toggle"   ${seg.startup === 'toggle'   ? 'selected' : ''}>Toggle</option>
          <option value="previous" ${seg.startup === 'previous' ? 'selected' : ''}>Previous</option>
        </select>
      </div>

      <div class="seg-geom">
        <button class="seg-geom-toggle" id="geom-toggle-${idx}">
          <svg class="chevron" width="10" height="10" viewBox="0 0 10 10" fill="none">
            <path d="M2 3.5L5 6.5L8 3.5" stroke="currentColor" stroke-width="1.5" stroke-linecap="round"/>
          </svg>
          Geometry
        </button>
        <div class="seg-geom-body" id="geom-body-${idx}">
          <div class="geom-field">
            <label>Start</label>
            <input type="number" id="seg-gstart-${idx}" min="0" max="999" value="${seg.start}">
          </div>
          <div class="geom-field">
            <label>Count</label>
            <input type="number" id="seg-gcount-${idx}" min="1" max="600" value="${seg.count}">
          </div>
          <div class="geom-field">
            <label>Strip</label>
            <select id="seg-gstrip-${idx}">
              <option value="0" ${seg.strip === 0 ? 'selected' : ''}>1</option>
              <option value="1" ${seg.strip === 1 ? 'selected' : ''}>2</option>
            </select>
          </div>
        </div>
      </div>
    </div>`;

  updateGlowBar(div, seg);
  bindSegCard(div, idx, seg);
  return div;
}

function updateSegCardUI(card, seg) {
  card.className = `seg-card${seg.on ? ' seg-on' : ''}`;

  const onEl = document.getElementById(`seg-on-${seg.index}`);
  if (onEl) onEl.checked = seg.on;

  const lvlEl = document.getElementById(`seg-lvl-${seg.index}`);
  if (lvlEl) { lvlEl.value = seg.level; document.getElementById(`seg-lvl-val-${seg.index}`).textContent = seg.level; }

  const modeHs = document.getElementById(`seg-hs-${seg.index}`);
  const modeCt = document.getElementById(`seg-ct-${seg.index}`);
  if (modeHs) modeHs.classList.toggle('hidden', seg.mode !== 'hs');
  if (modeCt) modeCt.classList.toggle('hidden', seg.mode !== 'ct');

  card.querySelectorAll('.mode-btn').forEach(b => {
    b.classList.toggle('active', b.dataset.mode === seg.mode);
  });

  ['hue', 'sat'].forEach(k => {
    const el = document.getElementById(`seg-${k}-${seg.index}`);
    const vEl = document.getElementById(`seg-${k}-val-${seg.index}`);
    if (el) {
      el.value = seg[k];
      vEl.textContent = k === 'hue' ? `${seg[k]}°` : seg[k];
    }
  });

  const ctEl = document.getElementById(`seg-ct-${seg.index}-r`);
  const ctValEl = document.getElementById(`seg-ct-val-${seg.index}`);
  if (ctEl) { ctEl.value = seg.ct; ctValEl.textContent = `${Math.round(1000000/seg.ct)}K`; }

  const startupEl = document.getElementById(`seg-startup-${seg.index}`);
  if (startupEl) startupEl.value = seg.startup;

  ['gstart', 'gcount'].forEach(k => {
    const el = document.getElementById(`seg-${k}-${seg.index}`);
    if (el) el.value = k === 'gstart' ? seg.start : seg.count;
  });
  const gstripEl = document.getElementById(`seg-gstrip-${seg.index}`);
  if (gstripEl) gstripEl.value = seg.strip;

  updateGlowBar(card, seg);
}

function updateGlowBar(card, seg) {
  const bar = card.querySelector('.seg-glow-bar');
  if (!bar) return;
  const color = segToColor(seg);
  if (color) {
    bar.style.background = color;
    bar.style.boxShadow = `0 0 12px ${color}`;
  } else {
    bar.style.background = 'var(--border)';
    bar.style.boxShadow = 'none';
  }
}

function bindSegCard(card, idx, initSeg) {
  /* local mutable copy of this segment */
  const patch = () => {
    const seg = S.segments[idx];
    if (!seg) return;
    debounce(`seg-${idx}`, async () => {
      try {
        await apiPost('/api/segments', { index: idx, ...seg });
      } catch (e) {
        toast(`Segment ${idx + 1}: ${e.message}`, 'error');
      }
    }, 250);
  };

  const mutate = (fields) => {
    Object.assign(S.segments[idx], fields);
    updateSegCardUI(card, S.segments[idx]);
    patch();
  };

  /* on/off */
  const onEl = document.getElementById(`seg-on-${idx}`);
  onEl.addEventListener('change', () => mutate({ on: onEl.checked }));

  /* level */
  const lvlEl = document.getElementById(`seg-lvl-${idx}`);
  lvlEl.addEventListener('input', () => mutate({ level: +lvlEl.value }));

  /* mode buttons */
  card.querySelectorAll('.mode-btn').forEach(btn => {
    btn.addEventListener('click', () => mutate({ mode: btn.dataset.mode }));
  });

  /* hue */
  const hueEl = document.getElementById(`seg-hue-${idx}`);
  hueEl.addEventListener('input', () => mutate({ hue: +hueEl.value }));

  /* sat */
  const satEl = document.getElementById(`seg-sat-${idx}`);
  satEl.addEventListener('input', () => mutate({ sat: +satEl.value }));

  /* ct */
  const ctEl = document.getElementById(`seg-ct-${idx}-r`);
  ctEl.addEventListener('input', () => mutate({ ct: +ctEl.value }));

  /* startup */
  const suEl = document.getElementById(`seg-startup-${idx}`);
  suEl.addEventListener('change', () => mutate({ startup: suEl.value }));

  /* geometry */
  const geomToggle = document.getElementById(`geom-toggle-${idx}`);
  const geomBody   = document.getElementById(`geom-body-${idx}`);
  geomToggle.addEventListener('click', () => {
    const open = geomBody.classList.toggle('open');
    geomToggle.classList.toggle('open', open);
  });

  const gsEl = document.getElementById(`seg-gstart-${idx}`);
  const gcEl = document.getElementById(`seg-gcount-${idx}`);
  const gstEl = document.getElementById(`seg-gstrip-${idx}`);
  gsEl.addEventListener('change', () => mutate({ start: +gsEl.value }));
  gcEl.addEventListener('change', () => mutate({ count: +gcEl.value }));
  gstEl.addEventListener('change', () => mutate({ strip: +gstEl.value }));
}

/* ======================================================================
   Presets Tab
   ====================================================================== */
function buildPresetCards() {
  const grid = document.getElementById('preset-grid');
  grid.innerHTML = '';
  S.presets.forEach(p => grid.appendChild(makePresetCard(p)));
}

function makePresetCard(p) {
  const div = document.createElement('div');
  div.className = `preset-card${p.occupied ? ' occupied' : ''}`;
  div.id = `preset-${p.slot}`;

  const vizHTML = S.segments.map(seg =>
    `<div class="preset-viz-seg" style="background:${presetSegColor(seg)}"></div>`
  ).join('');

  div.innerHTML = `
    <div class="preset-viz">${vizHTML}</div>
    <div class="preset-body">
      <div class="preset-name ${p.occupied ? '' : 'preset-empty-name'}">${p.occupied ? (p.name || `Preset ${p.slot + 1}`) : 'Empty'}</div>
      <div class="preset-slot">Slot ${p.slot}</div>
      <div class="preset-actions">
        ${p.occupied ? `<button class="btn btn-sm btn-acc" data-action="apply" data-slot="${p.slot}">Apply</button>` : ''}
        <button class="btn btn-sm" data-action="save" data-slot="${p.slot}">Save</button>
        ${p.occupied ? `<button class="btn btn-sm btn-danger" data-action="delete" data-slot="${p.slot}">Delete</button>` : ''}
      </div>
    </div>`;

  div.querySelectorAll('button[data-action]').forEach(btn => {
    btn.addEventListener('click', () => handlePresetAction(btn.dataset.action, +btn.dataset.slot));
  });

  return div;
}

async function handlePresetAction(action, slot) {
  try {
    if (action === 'apply') {
      await apiPost('/api/presets/apply', { slot });
      toast(`Preset ${slot} applied`, 'ok');
      const segs = await apiGet('/api/segments');
      S.segments = segs.segments || [];
      S.segments.forEach(s => refreshSegCard(s));
    } else if (action === 'save') {
      const name = prompt(`Name for slot ${slot}:`, `Preset ${slot + 1}`);
      if (name === null) return;
      await apiPost('/api/presets/save', { slot, name });
      toast(`Preset ${slot} saved`, 'ok');
      await loadPresets();
    } else if (action === 'delete') {
      if (!confirm(`Delete preset slot ${slot}?`)) return;
      await apiPost('/api/presets/delete', { slot });
      toast(`Preset ${slot} deleted`, 'warning');
      await loadPresets();
    }
  } catch (e) {
    toast(`Preset ${slot}: ${e.message}`, 'error');
  }
}

async function loadPresets() {
  const data = await apiGet('/api/presets');
  S.presets = data.presets || [];
  buildPresetCards();
}

/* ======================================================================
   Config Tab
   ====================================================================== */
function buildConfigUI(cfg) {
  S.config = cfg;

  /* Strip groups */
  const grid = document.getElementById('strip-config-grid');
  grid.innerHTML = '';
  for (let i = 1; i <= 2; i++) {
    const g = document.createElement('div');
    g.className = 'strip-group';
    g.innerHTML = `
      <div class="strip-group-title">Strip ${i}</div>
      <div class="form-row">
        <label>LED Count</label>
        <input type="number" id="cfg-count-${i}" min="1" max="1024" value="${cfg[`strip${i}_count`] || 0}">
      </div>
      <div class="form-row">
        <label>LED Type</label>
        <select id="cfg-type-${i}">
          <option value="0" ${cfg[`strip${i}_type`] === 0 ? 'selected' : ''}>SK6812 (RGBW)</option>
          <option value="1" ${cfg[`strip${i}_type`] === 1 ? 'selected' : ''}>WS2812B (RGB)</option>
        </select>
      </div>
      <div class="form-row">
        <label>Max Current (mA, 0=unlimited)</label>
        <input type="number" id="cfg-ma-${i}" min="0" max="20000" step="100" value="${cfg[`strip${i}_max_current`] || 0}">
      </div>`;
    grid.appendChild(g);
  }

  /* Transition slider */
  const transEl = document.getElementById('cfg-transition');
  const transValEl = document.getElementById('cfg-transition-val');
  transEl.value = cfg.transition_ms || 100;
  transValEl.textContent = `${cfg.transition_ms || 100}ms`;
  transEl.addEventListener('input', () => {
    transValEl.textContent = `${transEl.value}ms`;
  });

  /* Save button */
  document.getElementById('btn-save-config').onclick = async () => {
    const body = {
      strip1_count:       +document.getElementById('cfg-count-1').value,
      strip2_count:       +document.getElementById('cfg-count-2').value,
      strip1_type:        +document.getElementById('cfg-type-1').value,
      strip2_type:        +document.getElementById('cfg-type-2').value,
      strip1_max_current: +document.getElementById('cfg-ma-1').value,
      strip2_max_current: +document.getElementById('cfg-ma-2').value,
      transition_ms:      +document.getElementById('cfg-transition').value,
    };
    try {
      await apiPost('/api/config', body);
      toast('Configuration saved', 'ok');
    } catch (e) {
      toast(`Config error: ${e.message}`, 'error');
    }
  };
}

/* ======================================================================
   WiFi Tab
   ====================================================================== */
function renderWifiStatus(status) {
  const panel = document.getElementById('wifi-status-panel');
  const w = status.wifi || 'unknown';
  const labels = {
    connected: `Connected`,
    ap:        `AP mode — connect to set up`,
    connecting:`Connecting…`,
    failed:    `Connection failed`,
    init:      `Initialising…`,
  };
  panel.innerHTML = `
    <span class="dot ${w === 'connected' ? 'ok' : w === 'ap' ? 'warn' : 'err'}" style="display:inline-block;margin-right:8px"></span>
    <span class="mono">${labels[w] || w}</span>
    ${w === 'connected' ? `<br><span class="mono" style="color:var(--text-muted);font-size:11px;margin-top:4px;display:block">
      host: ${window.location.hostname}</span>` : ''}`;
}

/* ======================================================================
   System Tab
   ====================================================================== */
function renderStatGrid(containerId, items) {
  const grid = document.getElementById(containerId);
  grid.innerHTML = items.map(({ label, value }) => `
    <div class="stat-item">
      <div class="stat-label">${label}</div>
      <div class="stat-value">${value}</div>
    </div>`).join('');
}

function fmtUptime(sec) {
  sec = Math.round(sec);
  const h = Math.floor(sec / 3600);
  const m = Math.floor((sec % 3600) / 60);
  const s = sec % 60;
  if (h > 0) return `${h}h ${m}m`;
  if (m > 0) return `${m}m ${s}s`;
  return `${s}s`;
}

function fmtBytes(b) {
  if (b >= 1024) return `${(b / 1024).toFixed(1)}k`;
  return `${b}B`;
}

function renderSystemStatus(status) {
  renderStatGrid('stat-grid', [
    { label: 'Firmware',  value: status.firmware || '—' },
    { label: 'Uptime',    value: fmtUptime(status.uptime_sec || 0) },
    { label: 'Free Heap', value: fmtBytes(status.free_heap || 0) },
    { label: 'WiFi',      value: status.wifi || '—' },
  ]);
}

function renderDiag(diag) {
  S.diag = diag;
  renderStatGrid('diag-grid', [
    { label: 'Boot Count',    value: String(diag.boot_count ?? '—') },
    { label: 'Reset Reason',  value: diag.reset_reason_str || '—' },
    { label: 'Last Uptime',   value: diag.last_uptime_sec != null ? fmtUptime(diag.last_uptime_sec) : '—' },
    { label: 'Min Heap',      value: diag.min_free_heap != null ? fmtBytes(diag.min_free_heap) : '—' },
  ]);
}

function renderOtaStatus(ota) {
  S.ota = ota;
  const panel = document.getElementById('ota-status-panel');
  panel.innerHTML = `current: ${ota.current || '—'}   latest: ${ota.latest || '—'}   available: ${ota.available ? 'YES' : 'no'}`;

  const banner = document.getElementById('ota-banner');
  if (ota.available) {
    banner.classList.remove('hidden');
    document.getElementById('ota-ver').textContent = ota.latest;
  } else {
    banner.classList.add('hidden');
  }
}

async function doOtaCheck() {
  try {
    const r = await apiPost('/api/ota/check');
    renderOtaStatus(r);
    toast(r.available ? `Update available: ${r.latest}` : 'Already up to date', r.available ? 'warning' : 'ok');
  } catch (e) {
    toast(`OTA check failed: ${e.message}`, 'error');
  }
}

async function doOtaApply(url) {
  try {
    await apiPost('/api/ota', { url });
    toast('OTA update started — device will restart', 'ok');
  } catch (e) {
    toast(`OTA failed: ${e.message}`, 'error');
  }
}

async function doOtaUpload() {
  const fileInput = document.getElementById('ota-file');
  const file = fileInput.files[0];
  if (!file) { toast('Select a .bin or .ota file first', 'warning'); return; }

  const progress = document.getElementById('ota-progress');
  progress.classList.remove('hidden');
  progress.textContent = `Uploading ${(file.size / 1024).toFixed(1)} KB…`;

  try {
    const r = await fetch('/api/ota/upload', {
      method: 'POST',
      headers: { 'Content-Type': 'application/octet-stream', 'Content-Length': String(file.size) },
      body: file,
    });
    const json = await r.json();
    if (!r.ok) throw new Error(json.error || r.statusText);
    progress.textContent = 'Flash complete — restarting…';
    toast('Firmware flashed — device restarting', 'ok');
  } catch (e) {
    progress.textContent = `Error: ${e.message}`;
    toast(`Upload failed: ${e.message}`, 'error');
  }
}

async function doDiagReset() {
  if (!confirm('Reset boot counter to 0?')) return;
  try {
    await apiPost('/api/diag/reset');
    toast('Boot counter reset', 'ok');
    const d = await apiGet('/api/diag');
    renderDiag(d);
  } catch (e) {
    toast(`Reset failed: ${e.message}`, 'error');
  }
}

/* ======================================================================
   Tab Switching
   ====================================================================== */
function switchTab(name) {
  document.querySelectorAll('.tab-btn').forEach(b => b.classList.toggle('active', b.dataset.tab === name));
  document.querySelectorAll('.tab-content').forEach(s => s.classList.toggle('active', s.id === `tab-${name}`));
}

/* ======================================================================
   Wire-up
   ====================================================================== */
function bindUI() {
  /* Tabs */
  document.querySelectorAll('.tab-btn').forEach(btn => {
    btn.addEventListener('click', () => switchTab(btn.dataset.tab));
  });

  /* System tab */
  document.getElementById('btn-ota-check').addEventListener('click', doOtaCheck);
  document.getElementById('btn-ota-apply').addEventListener('click', () => {
    if (S.ota.available) doOtaApply('');  /* empty url = server uses cached latest url */
  });
  document.getElementById('btn-ota-upload').addEventListener('click', doOtaUpload);
  document.getElementById('btn-diag-reset').addEventListener('click', doDiagReset);

  document.getElementById('btn-restart').addEventListener('click', async () => {
    if (!confirm('Restart device?')) return;
    try { await apiPost('/api/restart'); toast('Restarting…', 'warning'); }
    catch (e) { toast(e.message, 'error'); }
  });
  document.getElementById('btn-zb-reset').addEventListener('click', async () => {
    if (!confirm('Reset Zigbee network? The device will leave the network and rejoin.')) return;
    try { await apiPost('/api/zb-reset'); toast('Zigbee reset initiated', 'warning'); }
    catch (e) { toast(e.message, 'error'); }
  });
  document.getElementById('btn-factory-reset').addEventListener('click', async () => {
    if (!confirm('Factory reset? All settings will be erased.')) return;
    try { await apiPost('/api/factory-reset'); toast('Factory reset initiated — device restarting', 'warning'); }
    catch (e) { toast(e.message, 'error'); }
  });
}

/* ======================================================================
   Init
   ====================================================================== */
async function init() {
  bindUI();

  try {
    const [status, segData, presetData, cfg, ota, diag] = await Promise.all([
      apiGet('/api/status'),
      apiGet('/api/segments'),
      apiGet('/api/presets'),
      apiGet('/api/config'),
      apiGet('/api/ota/status'),
      apiGet('/api/diag'),
    ]);

    applyStatus(status);
    renderSystemStatus(status);
    renderWifiStatus(status);
    renderDiag(diag);
    renderOtaStatus(ota);

    S.segments = segData.segments || [];
    buildSegCards();

    S.presets = presetData.presets || [];
    buildPresetCards();

    buildConfigUI(cfg);

  } catch (e) {
    toast(`Failed to load: ${e.message}`, 'error');
    console.error('init error:', e);
  }

  /* Refresh status every 10s */
  setInterval(async () => {
    try {
      const s = await apiGet('/api/status');
      applyStatus(s);
      renderSystemStatus(s);
      renderWifiStatus(s);
    } catch (_) {}
  }, 10000);
}

document.addEventListener('DOMContentLoaded', init);
