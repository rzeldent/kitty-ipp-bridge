/**
 * @file WebUiPage.h
 * @brief Embedded single-page dashboard (Bootstrap 5 from CDN).
 *
 * The page is self-contained: it talks to the REST API documented in
 * docs/REST_API.md and listens to Server-Sent Events on /api/events for live
 * updates (with a polling fallback). All dynamic text is HTML-escaped before
 * being inserted into the DOM.
 *
 * The string lives in flash (.rodata) and is served without copying.
 */
#pragma once

static const char kIndexHtml[] = R"HTML(<!doctype html>
<html lang="en" data-bs-theme="auto">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>CatPrinter IPP Bridge</title>
<link href="https://cdn.jsdelivr.net/npm/bootstrap@5.3.3/dist/css/bootstrap.min.css" rel="stylesheet">
<style>
  body { padding-bottom: 3rem; }
  .stat-value { font-size: 1.6rem; font-weight: 600; }
  .mono { font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace; font-size: .85rem; }
  #logs { max-height: 22rem; overflow-y: auto; white-space: pre-wrap; }
  .dot { display:inline-block; width:.6rem; height:.6rem; border-radius:50%; margin-right:.35rem; }
  .dot-on { background: var(--bs-success); } .dot-off { background: var(--bs-secondary); }
</style>
</head>
<body>
<nav class="navbar navbar-expand bg-body-tertiary border-bottom mb-4">
  <div class="container">
    <span class="navbar-brand fw-semibold">&#128049; CatPrinter IPP Bridge</span>
    <span class="navbar-text small" id="conn">connecting&hellip;</span>
  </div>
</nav>

<main class="container">
  <div id="alert" class="alert d-none" role="alert"></div>

  <!-- Statistics -->
  <div class="row g-3 mb-4" id="stats">
    <div class="col-6 col-md"><div class="card h-100"><div class="card-body">
      <div class="text-body-secondary small">Uptime</div><div class="stat-value" id="s-uptime">-</div></div></div></div>
    <div class="col-6 col-md"><div class="card h-100"><div class="card-body">
      <div class="text-body-secondary small">Free heap</div><div class="stat-value" id="s-heap">-</div></div></div></div>
    <div class="col-6 col-md"><div class="card h-100"><div class="card-body">
      <div class="text-body-secondary small">Printers (online / known)</div><div class="stat-value" id="s-printers">-</div></div></div></div>
    <div class="col-6 col-md"><div class="card h-100"><div class="card-body">
      <div class="text-body-secondary small">Jobs printed</div><div class="stat-value text-success" id="s-printed">-</div></div></div></div>
    <div class="col-6 col-md"><div class="card h-100"><div class="card-body">
      <div class="text-body-secondary small">Jobs failed</div><div class="stat-value text-danger" id="s-failed">-</div></div></div></div>
  </div>

  <!-- Printers -->
  <div class="card mb-4">
    <div class="card-header d-flex align-items-center">
      <h5 class="mb-0 me-auto">Printers</h5>
      <button class="btn btn-sm btn-outline-secondary me-2" id="btn-scan" title="Start a BLE scan now">Scan now</button>
      <button class="btn btn-sm btn-primary" id="btn-refresh">Refresh</button>
    </div>
    <div class="table-responsive">
      <table class="table table-hover align-middle mb-0">
        <thead><tr><th>Status</th><th>Name</th><th>MAC</th><th>RSSI</th><th>Last seen</th><th>IPP URI</th><th class="text-end">Actions</th></tr></thead>
        <tbody id="printers"><tr><td colspan="7" class="text-center text-body-secondary py-4">Loading&hellip;</td></tr></tbody>
      </table>
    </div>
  </div>

  <!-- Jobs -->
  <div class="card mb-4">
    <div class="card-header"><h5 class="mb-0">Jobs</h5></div>
    <div class="table-responsive">
      <table class="table table-sm align-middle mb-0">
        <thead><tr><th>#</th><th>Printer</th><th>Name</th><th>Type</th><th>State</th><th>Progress</th><th>Message</th><th></th></tr></thead>
        <tbody id="jobs"></tbody>
      </table>
    </div>
  </div>

  <div class="row g-4">
    <!-- Settings -->
    <div class="col-lg-6">
      <div class="card mb-4">
        <div class="card-header"><h5 class="mb-0">Bridge settings</h5></div>
        <form class="card-body" id="f-config">
          <div class="row g-2">
            <div class="col-6"><label class="form-label small">Scan interval (s)</label><input class="form-control" type="number" name="bridge.scanIntervalSec" min="10" max="3600"></div>
            <div class="col-6"><label class="form-label small">Scan duration (s)</label><input class="form-control" type="number" name="bridge.scanDurationSec" min="1" max="30"></div>
            <div class="col-6"><label class="form-label small">Offline timeout (s)</label><input class="form-control" type="number" name="bridge.offlineTimeoutSec" min="30" max="86400"></div>
            <div class="col-6"><label class="form-label small">Log level</label>
              <select class="form-select" name="bridge.logLevel"><option value="0">DEBUG</option><option value="1">INFO</option><option value="2">WARN</option><option value="3">ERROR</option></select></div>
            <div class="col-6"><label class="form-label small">Queue size (reboot)</label><input class="form-control" type="number" name="bridge.queueSize" min="1" max="32"></div>
            <div class="col-6"><label class="form-label small">Attempts per job</label><input class="form-control" type="number" name="bridge.maxRetries" min="1" max="10"></div>
            <div class="col-6 form-check ms-2"><input class="form-check-input" type="checkbox" name="bridge.probeUnknownDevices" id="probe"><label class="form-check-label small" for="probe">Probe devices without full UUID list</label></div>
            <div class="col-5 form-check ms-2"><input class="form-check-input" type="checkbox" name="bridge.trimBlankRows" id="trim"><label class="form-check-label small" for="trim">Trim blank rows</label></div>
            <div class="col-12"><hr class="my-2"><div class="small text-body-secondary">Printer protocol</div></div>
            <div class="col-4"><label class="form-label small">Energy</label><input class="form-control" type="number" name="protocol.energy" min="0" max="65535"></div>
            <div class="col-4"><label class="form-label small">Speed</label><input class="form-control" type="number" name="protocol.speed" min="1" max="255"></div>
            <div class="col-4"><label class="form-label small">Feed after job</label><input class="form-control" type="number" name="protocol.feedAfterJobLines" min="0" max="800"></div>
            <div class="col-6"><label class="form-label small">BLE chunk size</label><input class="form-control" type="number" name="protocol.chunkSize" min="20" max="512"></div>
            <div class="col-6"><label class="form-label small">Packet delay (ms)</label><input class="form-control" type="number" name="protocol.packetDelayMs" min="0" max="100"></div>
          </div>
          <button class="btn btn-primary mt-3" type="submit">Save</button>
        </form>
      </div>

      <div class="card mb-4">
        <div class="card-header"><h5 class="mb-0">Network</h5></div>
        <form class="card-body" id="f-network">
          <div class="row g-2">
            <div class="col-6"><label class="form-label small">Wi-Fi SSID</label><input class="form-control" name="ssid" maxlength="32"></div>
            <div class="col-6"><label class="form-label small">Wi-Fi password</label><input class="form-control" type="password" name="password" maxlength="63" placeholder="unchanged"></div>
            <div class="col-6"><label class="form-label small">Hostname</label><input class="form-control" name="hostname" maxlength="32" pattern="[A-Za-z0-9-]+"></div>
            <div class="col-6"><label class="form-label small">Setup AP password</label><input class="form-control" type="password" name="apPassword" maxlength="63" placeholder="unchanged"></div>
          </div>
          <button class="btn btn-primary mt-3" type="submit">Save</button>
          <button class="btn btn-outline-danger mt-3 ms-2" type="button" id="btn-reboot">Reboot</button>
        </form>
      </div>
    </div>

    <!-- Logs -->
    <div class="col-lg-6">
      <div class="card mb-4">
        <div class="card-header d-flex"><h5 class="mb-0 me-auto">Log</h5>
          <button class="btn btn-sm btn-outline-secondary" id="btn-clear-log">Clear view</button></div>
        <div class="card-body"><div id="logs" class="mono"></div></div>
      </div>
      <div class="card mb-4">
        <div class="card-header"><h5 class="mb-0">Details</h5></div>
        <div class="card-body"><dl class="row mb-0 small" id="details"></dl></div>
      </div>
    </div>
  </div>
</main>

<!-- Rename dialog -->
<div class="modal fade" id="m-rename" tabindex="-1" aria-hidden="true">
  <div class="modal-dialog"><form class="modal-content" id="f-rename">
    <div class="modal-header"><h5 class="modal-title">Rename printer</h5>
      <button type="button" class="btn-close" data-bs-dismiss="modal" aria-label="Close"></button></div>
    <div class="modal-body">
      <p class="small text-body-secondary mb-2" id="rename-mac"></p>
      <input class="form-control" id="rename-alias" maxlength="48" placeholder="e.g. Desk Label Printer">
      <div class="form-text">Leave empty to use the default name. The IPP queue name follows the alias.</div>
    </div>
    <div class="modal-footer">
      <button type="button" class="btn btn-secondary" data-bs-dismiss="modal">Cancel</button>
      <button type="submit" class="btn btn-primary">Save</button></div>
  </form></div>
</div>

<script src="https://cdn.jsdelivr.net/npm/bootstrap@5.3.3/dist/js/bootstrap.bundle.min.js"></script>
<script>
"use strict";
const $ = (id) => document.getElementById(id);
const esc = (v) => String(v ?? "").replace(/[&<>"']/g, (c) => ({"&":"&amp;","<":"&lt;",">":"&gt;","\"":"&quot;","'":"&#39;"}[c]));
const printers = new Map();
const jobs = new Map();
let renameMac = null;

function notify(msg, kind = "success") {
  const a = $("alert");
  a.className = "alert alert-" + kind;
  a.textContent = msg;
  clearTimeout(notify.t);
  notify.t = setTimeout(() => a.classList.add("d-none"), 4000);
}

async function api(path, method = "GET", body) {
  const opt = { method, headers: {} };
  if (body !== undefined) { opt.headers["Content-Type"] = "application/json"; opt.body = JSON.stringify(body); }
  const res = await fetch(path, opt);
  let data = null;
  try { data = await res.json(); } catch (e) { /* empty body */ }
  if (!res.ok || (data && data.ok === false)) throw new Error((data && data.error) || res.statusText);
  return data;
}

function ago(sec) {
  if (sec < 0) return "never";
  if (sec < 60) return sec + " s ago";
  if (sec < 3600) return Math.floor(sec / 60) + " min ago";
  if (sec < 86400) return Math.floor(sec / 3600) + " h ago";
  return Math.floor(sec / 86400) + " d ago";
}

function renderPrinters() {
  const tb = $("printers");
  if (printers.size === 0) {
    tb.innerHTML = '<tr><td colspan="7" class="text-center text-body-secondary py-4">No printers discovered yet. Switch on a Cat Printer nearby.</td></tr>';
    return;
  }
  const rows = [...printers.values()].sort((a, b) => (b.online - a.online) || a.name.localeCompare(b.name));
  tb.innerHTML = rows.map((p) => {
    const seen = p.lastSeenEpoch ? new Date(p.lastSeenEpoch * 1000).toLocaleString() : "";
    return `<tr>
      <td><span class="dot ${p.online ? "dot-on" : "dot-off"}"></span>${p.online ? "Online" : "Offline"}${p.busy ? ' <span class="badge text-bg-warning">busy</span>' : ""}</td>
      <td><div class="fw-semibold">${esc(p.name)}</div><div class="small text-body-secondary">${esc(p.deviceName)} &middot; ${esc(p.profile)}</div></td>
      <td class="mono">${esc(p.mac)}</td>
      <td>${p.online ? esc(p.rssi) + " dBm" : "-"}</td>
      <td title="${esc(seen)}">${esc(ago(p.lastSeenAgo))}</td>
      <td class="mono small">${esc(p.ippUri)}</td>
      <td class="text-end text-nowrap">
        <button class="btn btn-sm btn-outline-primary" data-act="rename" data-mac="${esc(p.mac)}">Rename</button>
        <button class="btn btn-sm btn-outline-secondary" data-act="identify" data-mac="${esc(p.mac)}">Identify</button>
        <button class="btn btn-sm btn-outline-secondary" data-act="print-test" data-mac="${esc(p.mac)}">Test page</button>
        <button class="btn btn-sm btn-outline-danger" data-act="delete" data-mac="${esc(p.mac)}" title="Forget printer">&times;</button>
      </td></tr>`;
  }).join("");
}

function renderJobs() {
  const rows = [...jobs.values()].sort((a, b) => b.id - a.id).slice(0, 20);
  const badge = { pending: "secondary", processing: "primary", completed: "success", failed: "danger", cancelled: "warning" };
  $("jobs").innerHTML = rows.length ? rows.map((j) => {
    const p = printers.get(j.printer);
    const pct = j.rowsTotal ? Math.round(100 * j.rowsDone / j.rowsTotal) : (j.state === "completed" ? 100 : 0);
    const active = j.state === "pending" || j.state === "processing";
    return `<tr><td>${esc(j.id)}</td><td>${esc(p ? p.name : j.printer)}</td><td>${esc(j.name)}</td><td>${esc(j.type)}</td>
      <td><span class="badge text-bg-${badge[j.state] || "secondary"}">${esc(j.state)}</span></td>
      <td style="min-width:7rem"><div class="progress" role="progressbar" style="height:.6rem"><div class="progress-bar" style="width:${pct}%"></div></div></td>
      <td class="small">${esc(j.message || "")}</td>
      <td>${active ? `<button class="btn btn-sm btn-link text-danger p-0" data-cancel="${esc(j.id)}">Cancel</button>` : ""}</td></tr>`;
  }).join("") : '<tr><td colspan="8" class="text-center text-body-secondary">No jobs yet</td></tr>';
}

function renderStats(s) {
  $("s-uptime").textContent = s.uptime;
  $("s-heap").textContent = Math.round(Number(s.heapFree) / 1024) + " KB";
  $("s-printers").textContent = (s.details ? s.details.printersOnline : "?") + " / " + s.printersDiscovered;
  $("s-printed").textContent = s.jobsPrinted;
  $("s-failed").textContent = s.jobsFailed;
  if (s.details) {
    $("details").innerHTML = Object.entries(s.details).map(([k, v]) =>
      `<dt class="col-6 fw-normal text-body-secondary">${esc(k)}</dt><dd class="col-6 mb-1 mono">${esc(v)}</dd>`).join("");
  }
}

function addLog(line) {
  const box = $("logs");
  const atBottom = box.scrollTop + box.clientHeight >= box.scrollHeight - 10;
  const div = document.createElement("div");
  div.textContent = line;
  if (/\bE\b|ERROR/.test(line)) div.className = "text-danger";
  else if (/\bW\b|WARN/.test(line)) div.className = "text-warning";
  box.appendChild(div);
  while (box.childElementCount > 300) box.removeChild(box.firstChild);
  if (atBottom) box.scrollTop = box.scrollHeight;
}

async function loadPrinters() {
  const list = await api("/api/printers");
  printers.clear();
  list.forEach((p) => printers.set(p.mac, p));
  renderPrinters();
}
async function loadJobs() {
  const list = await api("/api/jobs");
  jobs.clear();
  list.forEach((j) => jobs.set(j.id, j));
  renderJobs();
}
async function loadStats() { renderStats(await api("/api/stats")); }
async function loadLogs() {
  const lines = await api("/api/logs");
  $("logs").innerHTML = "";
  lines.forEach(addLog);
}
async function loadConfig() {
  const c = await api("/api/config");
  for (const el of $("f-config").elements) {
    if (!el.name) continue;
    const [sec, key] = el.name.split(".");
    const v = c[sec] ? c[sec][key] : undefined;
    if (el.type === "checkbox") el.checked = !!v; else if (v !== undefined) el.value = v;
  }
  const f = $("f-network").elements;
  f.ssid.value = c.network.ssid || "";
  f.hostname.value = c.network.hostname || "";
  f.password.placeholder = c.network.passwordSet ? "unchanged" : "not set";
}
async function refreshAll() {
  try { await Promise.all([loadPrinters(), loadJobs(), loadStats()]); }
  catch (e) { notify("Refresh failed: " + e.message, "danger"); }
}

document.addEventListener("click", async (ev) => {
  const b = ev.target.closest("button[data-act], button[data-cancel]");
  if (!b) return;
  if (b.dataset.cancel) {
    try { await api(`/api/jobs/${b.dataset.cancel}/cancel`, "POST"); notify("Job cancelled"); }
    catch (e) { notify(e.message, "danger"); }
    return;
  }
  const mac = b.dataset.mac, act = b.dataset.act, p = printers.get(mac);
  if (act === "rename") {
    renameMac = mac;
    $("rename-mac").textContent = mac;
    $("rename-alias").value = p ? p.alias : "";
    bootstrap.Modal.getOrCreateInstance($("m-rename")).show();
    return;
  }
  try {
    if (act === "delete") {
      if (!confirm("Forget this printer? It will be re-added when discovered again.")) return;
      await api(`/api/printers/${encodeURIComponent(mac)}`, "DELETE");
      printers.delete(mac); renderPrinters(); notify("Printer removed");
    } else {
      b.disabled = true;
      const r = await api(`/api/printers/${encodeURIComponent(mac)}/${act}`, "POST");
      notify(`Job #${r.jobId} queued`);
    }
  } catch (e) { notify(e.message, "danger"); }
  finally { b.disabled = false; }
});

$("f-rename").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  try {
    const r = await api(`/api/printers/${encodeURIComponent(renameMac)}/alias`, "POST", { alias: $("rename-alias").value });
    printers.set(r.printer.mac, r.printer); renderPrinters();
    bootstrap.Modal.getOrCreateInstance($("m-rename")).hide();
    notify("Printer renamed");
  } catch (e) { notify(e.message, "danger"); }
});

$("f-config").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  const body = { bridge: {}, protocol: {} };
  for (const el of ev.target.elements) {
    if (!el.name) continue;
    const [sec, key] = el.name.split(".");
    body[sec][key] = el.type === "checkbox" ? el.checked : Number(el.value);
  }
  try { await api("/api/config", "POST", body); notify("Settings saved"); }
  catch (e) { notify(e.message, "danger"); }
});

$("f-network").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  const f = ev.target.elements, body = { ssid: f.ssid.value, hostname: f.hostname.value };
  if (f.password.value) body.password = f.password.value;
  if (f.apPassword.value) body.apPassword = f.apPassword.value;
  try { await api("/api/network", "POST", body); notify("Network settings saved. Reboot to apply.", "warning"); }
  catch (e) { notify(e.message, "danger"); }
});

$("btn-refresh").onclick = refreshAll;
$("btn-scan").onclick = async () => { try { await api("/api/scan", "POST"); notify("Scan started"); } catch (e) { notify(e.message, "danger"); } };
$("btn-clear-log").onclick = () => { $("logs").innerHTML = ""; };
$("btn-reboot").onclick = async () => {
  if (!confirm("Reboot the bridge?")) return;
  try { await api("/api/reboot", "POST"); notify("Rebooting\u2026", "warning"); setTimeout(() => location.reload(), 8000); }
  catch (e) { notify(e.message, "danger"); }
};

let pollTimer = null;
function startPolling() {
  if (!pollTimer) pollTimer = setInterval(refreshAll, 10000);
}
function connectEvents() {
  if (!window.EventSource) { startPolling(); return; }
  const es = new EventSource("/api/events");
  es.onopen = () => { $("conn").textContent = "live"; clearInterval(pollTimer); pollTimer = null; };
  es.onerror = () => { $("conn").textContent = "reconnecting\u2026"; startPolling(); };
  es.addEventListener("printer", (e) => {
    const p = JSON.parse(e.data);
    if (p.removed) printers.delete(p.mac); else printers.set(p.mac, p);
    renderPrinters();
  });
  es.addEventListener("job", (e) => { const j = JSON.parse(e.data); jobs.set(j.id, j); renderJobs(); });
  es.addEventListener("log", (e) => addLog(e.data));
  es.addEventListener("stats", (e) => renderStats(JSON.parse(e.data)));
}

// Keep "last seen" ages fresh between updates.
setInterval(() => { printers.forEach((p) => { if (p.lastSeenAgo >= 0) p.lastSeenAgo += 5; }); renderPrinters(); }, 5000);

refreshAll();
loadLogs().catch(() => {});
loadConfig().catch(() => {});
connectEvents();
</script>
</body>
</html>
)HTML";
