#include "pch.h"
#include "WebRadar.h"
#include "Radar.h"
#include "RobRunReader.h"
#include "RobRunSDK.h"
#include "ConfigInstance.h"
#include "EspDraw.h"
#include <atomic>
#include <thread>
#include <mutex>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <shellapi.h>
#include <algorithm>
#include <cctype>

namespace WebRadar {

namespace {

std::atomic<bool> g_serverRunning{ false };
std::thread g_serverThread;
int g_serverPort = 8888;
SOCKET g_listenSocket = INVALID_SOCKET;

// ---------------------------------------------------------------------------
// Embedded HTML5 / Canvas Single-Page Application
// ---------------------------------------------------------------------------
const char* const kWebRadarHtml = R"raw_html(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
<title>VeoxDMA - Tactical Web Radar</title>
<style>
  :root {
    --bg-dark: #0a0c14;
    --panel-bg: rgba(14, 18, 30, 0.88);
    --panel-border: rgba(160, 100, 255, 0.22);
    --accent: #9d4edd;
    --accent-bright: #c77dff;
    --accent-glow: rgba(157, 78, 221, 0.35);
    --text: #f0f2fd;
    --text-dim: #9aa2c2;
    --text-faint: #5a6282;
    --green: #38ef7d;
    --cyan: #38bdf8;
    --red: #ff4d4d;
    --gold: #ffc107;
  }
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body, html {
    width: 100%; height: 100%;
    background-color: var(--bg-dark);
    color: var(--text);
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
    overflow: hidden;
    user-select: none;
    -webkit-user-select: none;
  }
  #radarCanvas {
    position: absolute;
    top: 0; left: 0;
    width: 100%; height: 100%;
    display: block;
    cursor: grab;
  }
  #radarCanvas:active { cursor: grabbing; }

  /* Floating Glassmorphism Toolbar */
  .toolbar {
    position: absolute;
    top: 14px; left: 14px;
    background: var(--panel-bg);
    backdrop-filter: blur(14px);
    -webkit-backdrop-filter: blur(14px);
    border: 1px solid var(--panel-border);
    border-radius: 12px;
    padding: 10px 16px;
    display: flex;
    align-items: center;
    gap: 14px;
    box-shadow: 0 8px 32px rgba(0, 0, 0, 0.6);
    z-index: 100;
    flex-wrap: wrap;
    max-width: calc(100vw - 28px);
  }
  .brand {
    display: flex;
    align-items: center;
    gap: 8px;
    font-weight: 800;
    letter-spacing: 1.5px;
    font-size: 14px;
    color: var(--text);
  }
  .brand-badge {
    background: linear-gradient(135deg, #7b2cbf, #9d4edd);
    color: #fff;
    padding: 3px 7px;
    border-radius: 6px;
    font-size: 11px;
    font-weight: 900;
  }
  .status-badge {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    font-size: 11px;
    font-weight: 700;
    padding: 3px 10px;
    border-radius: 20px;
    text-transform: uppercase;
    letter-spacing: 0.5px;
  }
  .status-badge.online {
    background: rgba(56, 239, 125, 0.14);
    color: var(--green);
    border: 1px solid rgba(56, 239, 125, 0.4);
  }
  .status-badge.offline {
    background: rgba(255, 77, 77, 0.14);
    color: var(--red);
    border: 1px solid rgba(255, 77, 77, 0.4);
  }
  .status-dot {
    width: 7px; height: 7px;
    border-radius: 50%;
    background-color: currentColor;
    box-shadow: 0 0 8px currentColor;
  }
  .control-group {
    display: flex;
    align-items: center;
    gap: 8px;
  }
  select, button {
    background: rgba(255, 255, 255, 0.07);
    color: var(--text);
    border: 1px solid rgba(255, 255, 255, 0.15);
    border-radius: 6px;
    padding: 5px 10px;
    font-size: 12px;
    font-weight: 600;
    outline: none;
    transition: all 0.2s ease;
  }
  select:hover, button:hover {
    background: rgba(157, 78, 221, 0.25);
    border-color: var(--accent);
    color: #fff;
  }
  button { cursor: pointer; }
  button:active { transform: scale(0.96); }
  .btn-primary {
    background: rgba(157, 78, 221, 0.35);
    border-color: var(--accent);
    color: #f3e8ff;
  }
  .btn-primary:hover {
    background: var(--accent);
    box-shadow: 0 0 14px var(--accent-glow);
  }
  label.toggle-label {
    display: inline-flex;
    align-items: center;
    gap: 5px;
    font-size: 12px;
    color: var(--text-dim);
    cursor: pointer;
    font-weight: 500;
  }
  label.toggle-label input {
    accent-color: var(--accent);
    cursor: pointer;
  }

  /* HUD Readouts */
  .hud-stats {
    position: absolute;
    bottom: 14px; left: 14px;
    background: var(--panel-bg);
    backdrop-filter: blur(12px);
    -webkit-backdrop-filter: blur(12px);
    border: 1px solid var(--panel-border);
    border-radius: 10px;
    padding: 8px 14px;
    font-size: 12px;
    color: var(--text-dim);
    display: flex;
    gap: 16px;
    z-index: 100;
  }
  .hud-val { color: var(--text); font-weight: 700; }

  /* Map Legend */
  .legend {
    position: absolute;
    bottom: 14px; right: 14px;
    background: var(--panel-bg);
    backdrop-filter: blur(12px);
    -webkit-backdrop-filter: blur(12px);
    border: 1px solid var(--panel-border);
    border-radius: 10px;
    padding: 8px 14px;
    font-size: 11px;
    display: flex;
    gap: 12px;
    z-index: 100;
  }
  .legend-item { display: flex; align-items: center; gap: 5px; font-weight: 600; }
  .legend-dot { width: 8px; height: 8px; border-radius: 50%; }

  /* Compass Rose */
  .compass {
    position: absolute;
    top: 14px; right: 14px;
    width: 48px; height: 48px;
    border-radius: 50%;
    background: var(--panel-bg);
    border: 1px solid var(--panel-border);
    display: flex;
    align-items: center;
    justify-content: center;
    font-weight: 800;
    font-size: 13px;
    color: var(--red);
    box-shadow: 0 4px 16px rgba(0, 0, 0, 0.4);
    z-index: 100;
    pointer-events: none;
    transition: transform 0.1s linear;
  }
</style>
</head>
<body>

<canvas id="radarCanvas"></canvas>

<!-- Top Toolbar -->
<div class="toolbar">
  <div class="brand">
    <span class="brand-badge">VEOX</span>
    <span>RADAR</span>
  </div>

  <div id="statusBadge" class="status-badge offline">
    <span class="status-dot"></span>
    <span id="statusText">WAITING</span>
  </div>

  <div class="control-group">
    <label class="toggle-label" title="Active 2D BVH map">MAP:</label>
    <select id="mapSelect"></select>
  </div>

  <div class="control-group">
    <label class="toggle-label"><input type="checkbox" id="chkFollow" checked> Follow Me</label>
    <label class="toggle-label"><input type="checkbox" id="chkRotate"> Rotate View</label>
  </div>

  <div class="control-group">
    <label class="toggle-label"><input type="checkbox" id="chkPlayers" checked> Players</label>
    <label class="toggle-label"><input type="checkbox" id="chkLoot" checked> Loot</label>
    <label class="toggle-label"><input type="checkbox" id="chkVehicles" checked> Vehicles</label>
    <label class="toggle-label"><input type="checkbox" id="chkBvh" checked> BVH Walls</label>
  </div>

  <div class="control-group">
    <button id="btnZoomIn" title="Zoom in (or Mouse Wheel)">+</button>
    <button id="btnZoomOut" title="Zoom out (or Mouse Wheel)">-</button>
    <button id="btnReset" class="btn-primary" title="Reset view">RESET</button>
  </div>
</div>

<!-- Compass Rose -->
<div id="compass" class="compass">N</div>

<!-- Bottom Stats HUD -->
<div class="hud-stats">
  <div>LOC: <span id="hudLoc" class="hud-val">-</span></div>
  <div>POS: <span id="hudPos" class="hud-val">0, 0</span></div>
  <div>ENTITIES: <span id="hudPlayers" class="hud-val">0</span> P | <span id="hudLoot" class="hud-val">0</span> L</div>
  <div>ZOOM: <span id="hudZoom" class="hud-val">1.0x</span></div>
</div>

<!-- Map Legend -->
<div class="legend">
  <div class="legend-item"><div class="legend-dot" style="background:var(--green)"></div> YOU</div>
  <div class="legend-item"><div class="legend-dot" style="background:#00f5d4"></div> PARTY</div>
  <div class="legend-item"><div class="legend-dot" style="background:var(--cyan)"></div> POLICE</div>
  <div class="legend-item"><div class="legend-dot" style="background:var(--red)"></div> CRIMINAL</div>
  <div class="legend-item"><div class="legend-dot" style="background:var(--gold)"></div> CIVILIAN</div>
  <div class="legend-item"><div class="legend-dot" style="background:#e0aaff"></div> SAFE / CRATE</div>
</div>

<script>
(() => {
  const canvas = document.getElementById("radarCanvas");
  const ctx = canvas.getContext("2d");

  // DOM Controls
  const statusBadge = document.getElementById("statusBadge");
  const statusText = document.getElementById("statusText");
  const mapSelect = document.getElementById("mapSelect");
  const chkFollow = document.getElementById("chkFollow");
  const chkRotate = document.getElementById("chkRotate");
  const chkPlayers = document.getElementById("chkPlayers");
  const chkLoot = document.getElementById("chkLoot");
  const chkVehicles = document.getElementById("chkVehicles");
  const chkBvh = document.getElementById("chkBvh");
  const btnZoomIn = document.getElementById("btnZoomIn");
  const btnZoomOut = document.getElementById("btnZoomOut");
  const btnReset = document.getElementById("btnReset");
  const compass = document.getElementById("compass");
  const hudLoc = document.getElementById("hudLoc");
  const hudPos = document.getElementById("hudPos");
  const hudPlayers = document.getElementById("hudPlayers");
  const hudLoot = document.getElementById("hudLoot");
  const hudZoom = document.getElementById("hudZoom");

  // State
  let state = {
    locationId: "",
    mapName: "",
    mapUrl: "",
    bounds: { minX: -600, maxX: 600, minZ: -400, maxZ: 400 },
    local: { valid: false, x: 0, y: 0, z: 0, yaw: 0, pitch: 0 },
    players: [],
    vehicles: [],
    loot: [],
    availableMaps: []
  };

  // View state
  let view = {
    zoom: 1.0,
    panX: 0,
    panY: 0,
    isDragging: false,
    dragStartX: 0,
    dragStartY: 0,
    panStartX: 0,
    panStartY: 0
  };

  // Map image cache
  let currentMapUrl = "";
  let mapImage = new Image();
  let mapLoaded = false;

  // BVH wireframe segments cache
  let bvhLines = [];
  let lastBvhFetch = 0;

  // Resize canvas
  function resize() {
    canvas.width = window.innerWidth;
    canvas.height = window.innerHeight;
  }
  window.addEventListener("resize", resize);
  resize();

  // Mouse / Touch Pan & Zoom
  canvas.addEventListener("wheel", (e) => {
    e.preventDefault();
    const factor = e.deltaY < 0 ? 1.15 : 0.87;
    const newZoom = Math.min(Math.max(view.zoom * factor, 0.2), 12.0);
    // Zoom toward mouse pointer
    const mouseX = e.clientX - canvas.width * 0.5;
    const mouseY = e.clientY - canvas.height * 0.5;
    view.panX -= mouseX / view.zoom - mouseX / newZoom;
    view.panY -= mouseY / view.zoom - mouseY / newZoom;
    view.zoom = newZoom;
    hudZoom.textContent = view.zoom.toFixed(1) + "x";
  }, { passive: false });

  canvas.addEventListener("mousedown", (e) => {
    if (e.button === 0) {
      view.isDragging = true;
      view.dragStartX = e.clientX;
      view.dragStartY = e.clientY;
      view.panStartX = view.panX;
      view.panStartY = view.panY;
    }
  });

  window.addEventListener("mousemove", (e) => {
    if (view.isDragging) {
      const dx = (e.clientX - view.dragStartX) / view.zoom;
      const dy = (e.clientY - view.dragStartY) / view.zoom;
      view.panX = view.panStartX - dx;
      view.panY = view.panStartY - dy;
      chkFollow.checked = false; // Disable follow on manual pan
    }
  });

  window.addEventListener("mouseup", () => { view.isDragging = false; });

  // Mobile Touch Pan / Pinch-to-zoom
  let initialTouchDist = 0;
  let initialZoom = 1.0;
  canvas.addEventListener("touchstart", (e) => {
    if (e.touches.length === 1) {
      view.isDragging = true;
      view.dragStartX = e.touches[0].clientX;
      view.dragStartY = e.touches[0].clientY;
      view.panStartX = view.panX;
      view.panStartY = view.panY;
    } else if (e.touches.length === 2) {
      view.isDragging = false;
      initialTouchDist = Math.hypot(
        e.touches[0].clientX - e.touches[1].clientX,
        e.touches[0].clientY - e.touches[1].clientY
      );
      initialZoom = view.zoom;
    }
  }, { passive: true });

  canvas.addEventListener("touchmove", (e) => {
    if (e.touches.length === 1 && view.isDragging) {
      const dx = (e.touches[0].clientX - view.dragStartX) / view.zoom;
      const dy = (e.touches[0].clientY - view.dragStartY) / view.zoom;
      view.panX = view.panStartX - dx;
      view.panY = view.panStartY - dy;
      chkFollow.checked = false;
    } else if (e.touches.length === 2 && initialTouchDist > 0) {
      const dist = Math.hypot(
        e.touches[0].clientX - e.touches[1].clientX,
        e.touches[0].clientY - e.touches[1].clientY
      );
      view.zoom = Math.min(Math.max(initialZoom * (dist / initialTouchDist), 0.2), 12.0);
      hudZoom.textContent = view.zoom.toFixed(1) + "x";
    }
  }, { passive: true });

  canvas.addEventListener("touchend", () => {
    view.isDragging = false;
    initialTouchDist = 0;
  });

  // Toolbar button handlers
  btnZoomIn.onclick = () => {
    view.zoom = Math.min(view.zoom * 1.25, 12.0);
    hudZoom.textContent = view.zoom.toFixed(1) + "x";
  };
  btnZoomOut.onclick = () => {
    view.zoom = Math.max(view.zoom * 0.8, 0.2);
    hudZoom.textContent = view.zoom.toFixed(1) + "x";
  };
  btnReset.onclick = () => {
    view.zoom = 1.0;
    view.panX = 0;
    view.panY = 0;
    chkFollow.checked = true;
    hudZoom.textContent = "1.0x";
  };

  mapSelect.onchange = () => {
    const chosen = mapSelect.value;
    if (chosen) {
      fetch("/api/setmap?name=" + encodeURIComponent(chosen));
    }
  };

  // Convert World Coordinate (X, Z) to Map Pixel (mx, my)
  function worldToMapPixel(wx, wz) {
    const b = state.bounds;
    const w = b.maxX - b.minX || 1.0;
    const h = b.maxZ - b.minZ || 1.0;
    const imgW = mapLoaded ? mapImage.width : 2048;
    const imgH = mapLoaded ? mapImage.height : 2048;
    const nx = (wx - b.minX) / w;
    const nz = (wz - b.minZ) / h;
    return { x: nx * imgW, y: nz * imgH };
  }

  // Fetch BVH 2D Wall Contours
  async function fetchBvhLines() {
    const now = Date.now();
    if (now - lastBvhFetch < 3000) return;
    lastBvhFetch = now;
    try {
      const res = await fetch("/api/bvh");
      if (res.ok) {
        bvhLines = await res.json();
      }
    } catch (_) {}
  }

  // Data Fetch Loop
  let isFetching = false;
  async function pollState() {
    if (isFetching) return;
    isFetching = true;
    try {
      const res = await fetch("/api/state");
      if (res.ok) {
        const data = await res.json();
        state = data;

        // Status badge
        statusBadge.className = "status-badge online";
        statusText.textContent = "CONNECTED";

        // HUD Readouts
        hudLoc.textContent = data.locationId || "In Battle";
        if (data.local && data.local.valid) {
          hudPos.textContent = Math.round(data.local.x) + ", " + Math.round(data.local.z);
        } else {
          hudPos.textContent = "-";
        }
        hudPlayers.textContent = data.players ? data.players.length : 0;
        hudLoot.textContent = data.loot ? data.loot.length : 0;

        // Populate Map Dropdown once or when changed
        if (data.availableMaps && data.availableMaps.length > 0) {
          if (mapSelect.options.length !== data.availableMaps.length) {
            mapSelect.innerHTML = "";
            for (const m of data.availableMaps) {
              const opt = document.createElement("option");
              opt.value = m;
              opt.textContent = m.replace(".png", "").replace("_map", " (BVH)").replace(/_/g, " ").toUpperCase();
              mapSelect.appendChild(opt);
            }
          }
          if (data.mapName && mapSelect.value !== data.mapName) {
            mapSelect.value = data.mapName;
          }
        }

        // Load / change map texture
        if (data.mapUrl && data.mapUrl !== currentMapUrl) {
          currentMapUrl = data.mapUrl;
          mapLoaded = false;
          mapImage = new Image();
          mapImage.onload = () => { mapLoaded = true; };
          mapImage.src = data.mapUrl;
        }

        // Follow local player
        if (chkFollow.checked && data.local && data.local.valid) {
          const lp = worldToMapPixel(data.local.x, data.local.z);
          view.panX = lp.x;
          view.panY = lp.y;
        }
      } else {
        statusBadge.className = "status-badge offline";
        statusText.textContent = "WAITING";
      }
    } catch (e) {
      statusBadge.className = "status-badge offline";
      statusText.textContent = "OFFLINE";
    } finally {
      isFetching = false;
    }
  }

  setInterval(pollState, 33); // 30 Hz polling

  // Main Render Loop
  function draw() {
    requestAnimationFrame(draw);

    const w = canvas.width;
    const h = canvas.height;
    ctx.clearRect(0, 0, w, h);

    // Dark grid background
    ctx.fillStyle = "#0c0e18";
    ctx.fillRect(0, 0, w, h);

    const cx = w * 0.5;
    const cy = h * 0.5;

    // Player rotation angle
    const playerYaw = (chkRotate.checked && state.local && state.local.valid)
      ? state.local.yaw
      : 0;

    // Update compass needle
    compass.style.transform = `rotate(${-playerYaw * (180 / Math.PI)}deg)`;

    ctx.save();
    ctx.translate(cx, cy);
    if (playerYaw !== 0) ctx.rotate(-playerYaw);
    ctx.scale(view.zoom, view.zoom);
    ctx.translate(-view.panX, -view.panY);

    // 1. Draw Map Image
    const imgW = mapLoaded ? mapImage.width : 2048;
    const imgH = mapLoaded ? mapImage.height : 2048;

    if (mapLoaded) {
      ctx.drawImage(mapImage, 0, 0, imgW, imgH);
    } else {
      ctx.fillStyle = "#141824";
      ctx.fillRect(0, 0, imgW, imgH);
      ctx.strokeStyle = "rgba(157, 78, 221, 0.4)";
      ctx.strokeRect(0, 0, imgW, imgH);
      ctx.fillStyle = "rgba(255, 255, 255, 0.2)";
      ctx.font = "24px sans-serif";
      ctx.textAlign = "center";
      ctx.fillText("Loading Map...", imgW * 0.5, imgH * 0.5);
    }

    // 2. Draw Subtle World Grid
    ctx.strokeStyle = "rgba(255, 255, 255, 0.05)";
    ctx.lineWidth = 1.0 / view.zoom;
    const step = 100;
    for (let x = 0; x <= imgW; x += step) {
      ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, imgH); ctx.stroke();
    }
    for (let y = 0; y <= imgH; y += step) {
      ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(imgW, y); ctx.stroke();
    }

    // 3. Draw BVH 2D Wall Contours (if enabled and loaded)
    if (chkBvh.checked) {
      fetchBvhLines();
      if (bvhLines && bvhLines.length > 0) {
        ctx.strokeStyle = "rgba(120, 210, 255, 0.45)";
        ctx.lineWidth = 1.5 / view.zoom;
        ctx.beginPath();
        for (const line of bvhLines) {
          const p1 = worldToMapPixel(line[0], line[1]);
          const p2 = worldToMapPixel(line[2], line[3]);
          ctx.moveTo(p1.x, p1.y);
          ctx.lineTo(p2.x, p2.y);
        }
        ctx.stroke();
      }
    }

    // 4. Draw Vehicles
    if (chkVehicles.checked && state.vehicles) {
      for (const v of state.vehicles) {
        const p = worldToMapPixel(v.x, v.z);
        ctx.fillStyle = "rgba(56, 189, 248, 0.85)";
        ctx.strokeStyle = "#fff";
        ctx.lineWidth = 1.5 / view.zoom;
        ctx.beginPath();
        ctx.rect(p.x - 7, p.y - 7, 14, 14);
        ctx.fill();
        ctx.stroke();

        if (view.zoom >= 0.7 && v.label) {
          ctx.fillStyle = "#e0f2fe";
          ctx.font = "bold 11px sans-serif";
          ctx.textAlign = "center";
          ctx.fillText(v.label, p.x, p.y + 18);
        }
      }
    }

    // 5. Draw Loot & Containers
    if (chkLoot.checked && state.loot) {
      for (const item of state.loot) {
        const p = worldToMapPixel(item.x, item.z);
        let col = "#22c55e"; // default cash green
        let radius = 3.5;

        if (item.isSafe) {
          col = "#e0aaff";
          radius = 6.0;
        } else if (item.isValuable) {
          col = "#f59e0b";
          radius = 4.5;
        } else if (item.isContainer) {
          col = "#38bdf8";
          radius = 4.0;
        }

        ctx.fillStyle = col;
        ctx.beginPath();
        ctx.arc(p.x, p.y, radius, 0, Math.PI * 2);
        ctx.fill();

        if (item.isSafe) {
          ctx.strokeStyle = "#fff";
          ctx.lineWidth = 1.2 / view.zoom;
          ctx.stroke();
        }

        if (view.zoom >= 1.2 && item.label) {
          ctx.fillStyle = col;
          ctx.font = "10px sans-serif";
          ctx.textAlign = "center";
          ctx.fillText(item.label, p.x, p.y + radius + 10);
        }
      }
    }

    // 6. Draw Other Players
    if (chkPlayers.checked && state.players) {
      for (const p of state.players) {
        if (p.isLocal) continue; // Drawn last
        const sp = worldToMapPixel(p.x, p.z);

        // Role color
        let col = "#fde047"; // Civilian / default yellow
        if (p.isParty) col = "#00f5d4";
        else if (p.roleName === "Police Officer" || p.role === 1) col = "#38bdf8";
        else if (p.roleName === "Criminal" || p.role === 0) col = "#ff4d4d";

        // Direction tick
        if (p.yaw !== undefined) {
          const tickLen = 14;
          const tx = sp.x + Math.sin(p.yaw) * tickLen;
          const ty = sp.y - Math.cos(p.yaw) * tickLen;
          ctx.strokeStyle = col;
          ctx.lineWidth = 2.0 / view.zoom;
          ctx.beginPath();
          ctx.moveTo(sp.x, sp.y);
          ctx.lineTo(tx, ty);
          ctx.stroke();
        }

        // Player dot
        ctx.fillStyle = col;
        ctx.strokeStyle = "#000";
        ctx.lineWidth = 1.5 / view.zoom;
        ctx.beginPath();
        ctx.arc(sp.x, sp.y, 6.0, 0, Math.PI * 2);
        ctx.fill();
        ctx.stroke();

        // Player Name & Distance Label
        if (view.zoom >= 0.6) {
          ctx.font = "bold 11px sans-serif";
          ctx.textAlign = "center";

          // Text shadow
          ctx.fillStyle = "#000";
          const label = p.name ? p.name : (p.roleName || "Player");
          ctx.fillText(label, sp.x + 1, sp.y - 10 + 1);
          ctx.fillText(label, sp.x - 1, sp.y - 10 - 1);

          ctx.fillStyle = col;
          ctx.fillText(label, sp.x, sp.y - 10);

          if (p.distance) {
            ctx.fillStyle = "#94a3b8";
            ctx.font = "9px sans-serif";
            ctx.fillText(Math.round(p.distance) + "m", sp.x, sp.y + 16);
          }
        }
      }
    }

    // 7. Draw Local Player (YOU)
    if (state.local && state.local.valid) {
      const lp = worldToMapPixel(state.local.x, state.local.z);

      // FOV Cone (90-degree translucent arc forward)
      const coneDist = 80;
      const fovHalf = Math.PI * 0.25;
      const angle = state.local.yaw || 0;
      ctx.fillStyle = "rgba(56, 239, 125, 0.12)";
      ctx.beginPath();
      ctx.moveTo(lp.x, lp.y);
      ctx.arc(lp.x, lp.y, coneDist, angle - Math.PI * 0.5 - fovHalf, angle - Math.PI * 0.5 + fovHalf);
      ctx.closePath();
      ctx.fill();

      // Pulsing outer beacon
      const pulseTime = (Date.now() % 1500) / 1500;
      const pulseR = 8 + pulseTime * 18;
      ctx.strokeStyle = `rgba(56, 239, 125, ${1.0 - pulseTime})`;
      ctx.lineWidth = 2.0 / view.zoom;
      ctx.beginPath();
      ctx.arc(lp.x, lp.y, pulseR, 0, Math.PI * 2);
      ctx.stroke();

      // Local player chevron / pointer
      ctx.save();
      ctx.translate(lp.x, lp.y);
      ctx.rotate(angle);

      ctx.fillStyle = "#38ef7d";
      ctx.strokeStyle = "#000";
      ctx.lineWidth = 2.0 / view.zoom;
      ctx.beginPath();
      ctx.moveTo(0, -12);
      ctx.lineTo(8, 8);
      ctx.lineTo(0, 4);
      ctx.lineTo(-8, 8);
      ctx.closePath();
      ctx.fill();
      ctx.stroke();
      ctx.restore();

      // "YOU" Label
      ctx.font = "bold 12px sans-serif";
      ctx.fillStyle = "#38ef7d";
      ctx.textAlign = "center";
      ctx.fillText("YOU", lp.x, lp.y - 18);
    }

    ctx.restore();
  }

  requestAnimationFrame(draw);
})();
</script>

</body>
</html>
)raw_html";

// ---------------------------------------------------------------------------
// Worker Thread: Lightweight HTTP 1.1 Web Server
// ---------------------------------------------------------------------------
void RunServerLoop(int port)
{
	WSADATA wsaData{};
	WSAStartup(MAKEWORD(2, 2), &wsaData);

	g_listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (g_listenSocket == INVALID_SOCKET) {
		g_serverRunning.store(false);
		return;
	}

	int opt = 1;
	setsockopt(g_listenSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

	sockaddr_in serverAddr{};
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_addr.s_addr = INADDR_ANY; // Listen on all interfaces (localhost + LAN)
	serverAddr.sin_port = htons(static_cast<u_short>(port));

	if (bind(g_listenSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
		closesocket(g_listenSocket);
		g_listenSocket = INVALID_SOCKET;
		g_serverRunning.store(false);
		return;
	}

	if (listen(g_listenSocket, 10) == SOCKET_ERROR) {
		closesocket(g_listenSocket);
		g_listenSocket = INVALID_SOCKET;
		g_serverRunning.store(false);
		return;
	}

	printf("[WebRadar] Server listening on http://localhost:%d\n", port);

	while (g_serverRunning.load()) {
		fd_set readSet;
		FD_ZERO(&readSet);
		FD_SET(g_listenSocket, &readSet);

		timeval tv{};
		tv.tv_sec = 0;
		tv.tv_usec = 100000; // 100ms select timeout for clean thread shutdown

		int sel = select(0, &readSet, nullptr, nullptr, &tv);
		if (sel <= 0) continue;

		sockaddr_in clientAddr{};
		int clientAddrLen = sizeof(clientAddr);
		SOCKET client = accept(g_listenSocket, (sockaddr*)&clientAddr, &clientAddrLen);
		if (client == INVALID_SOCKET) continue;

		// Set client timeouts to 2 seconds
		DWORD timeoutMs = 2000;
		setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs));
		setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs));

		char reqBuf[4096] = {};
		int bytesRead = recv(client, reqBuf, sizeof(reqBuf) - 1, 0);
		if (bytesRead <= 0) {
			closesocket(client);
			continue;
		}

		std::string request(reqBuf, bytesRead);
		std::istringstream reqStream(request);
		std::string method, path, proto;
		reqStream >> method >> path >> proto;

		// Handle CORS preflight
		if (method == "OPTIONS") {
			std::string resp =
				"HTTP/1.1 204 No Content\r\n"
				"Access-Control-Allow-Origin: *\r\n"
				"Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
				"Access-Control-Allow-Headers: Content-Type\r\n"
				"Content-Length: 0\r\n"
				"Connection: close\r\n\r\n";
			send(client, resp.data(), (int)resp.size(), 0);
			closesocket(client);
			continue;
		}

		// Routing: Root / HTML SPA
		if (path == "/" || path == "/index.html" || path.empty()) {
			std::string body = kWebRadarHtml;
			std::string header =
				"HTTP/1.1 200 OK\r\n"
				"Content-Type: text/html; charset=utf-8\r\n"
				"Content-Length: " + std::to_string(body.size()) + "\r\n"
				"Access-Control-Allow-Origin: *\r\n"
				"Connection: close\r\n\r\n";
			send(client, header.data(), (int)header.size(), 0);
			send(client, body.data(), (int)body.size(), 0);
		}
		// Routing: /api/state
		else if (path.rfind("/api/state", 0) == 0) {
			json j;
			j["status"] = "ok";
			const auto snapshot = RobRunReader::GetSnapshot();
			const std::string loc = RobRunReader::GetLocationId();
			j["locationId"] = loc;

			const std::string activeMap = Radar::GetCurrentMapName();
			j["mapName"] = activeMap;
			j["mapUrl"] = "/maps/" + activeMap;

			const auto& cfg = ConfigInstance.Radar;
			j["bounds"] = {
				{ "minX", cfg.WorldMinX },
				{ "maxX", cfg.WorldMaxX },
				{ "minZ", cfg.WorldMinZ },
				{ "maxZ", cfg.WorldMaxZ }
			};

			if (snapshot && !snapshot->localPosition.IsZero()) {
				j["local"] = {
					{ "valid", true },
					{ "x", snapshot->localPosition.x },
					{ "y", snapshot->localPosition.y },
					{ "z", snapshot->localPosition.z },
					{ "yaw", snapshot->cameraYaw },
					{ "pitch", snapshot->cameraPitch },
					{ "entityId", snapshot->localEntityId }
				};
			} else {
				j["local"] = { { "valid", false } };
			}

			// Players
			json players = json::array();
			if (snapshot) {
				for (const auto& ent : snapshot->entities) {
					if (ent.isDead) continue;
					const bool isLocal = (ent.entityId == snapshot->localEntityId);
					float dist = 0.f;
					if (!snapshot->localPosition.IsZero())
						dist = Distance(ent.position, snapshot->localPosition);

					players.push_back({
						{ "id", ent.entityId },
						{ "name", ent.nicknameUtf8 },
						{ "isLocal", isLocal },
						{ "isParty", ent.isPartyMember },
						{ "role", ent.role },
						{ "roleName", ent.roleName },
						{ "x", ent.position.x },
						{ "y", ent.position.y },
						{ "z", ent.position.z },
						{ "yaw", ent.yaw },
						{ "distance", dist }
					});
				}
			}
			j["players"] = players;

			// Vehicles & Loot
			json vehicles = json::array();
			json loot = json::array();
			if (snapshot && snapshot->world && snapshot->world->valid) {
				for (const auto& ent : snapshot->world->entities) {
					if (ent.kind == RobRunWorld::Kind::Car ||
						ent.kind == RobRunWorld::Kind::Helicopter ||
						ent.kind == RobRunWorld::Kind::Vehicle) {
						vehicles.push_back({
							{ "kind", (int)ent.kind },
							{ "label", ent.label },
							{ "x", ent.position.x },
							{ "y", ent.position.y },
							{ "z", ent.position.z }
						});
					} else {
						bool isSafe = (ent.kind == RobRunWorld::Kind::SafeDoor);
						bool isContainer = (ent.kind == RobRunWorld::Kind::StorageBox ||
											ent.kind == RobRunWorld::Kind::VehicleTrunk ||
											ent.kind == RobRunWorld::Kind::FirstAidCabinet);
						bool isValuable = (ent.kind == RobRunWorld::Kind::GoldBar ||
										   ent.kind == RobRunWorld::Kind::CryptoWorkbench ||
										   ent.kind == RobRunWorld::Kind::MoneyWorkbench ||
										   ent.kind == RobRunWorld::Kind::Cash);

						loot.push_back({
							{ "kind", (int)ent.kind },
							{ "label", ent.label },
							{ "x", ent.position.x },
							{ "y", ent.position.y },
							{ "z", ent.position.z },
							{ "isSafe", isSafe },
							{ "isContainer", isContainer },
							{ "isValuable", isValuable }
						});
					}
				}
			}
			j["vehicles"] = vehicles;
			j["loot"] = loot;
			j["availableMaps"] = Radar::GetMapFiles();

			std::string body = j.dump();
			std::string header =
				"HTTP/1.1 200 OK\r\n"
				"Content-Type: application/json; charset=utf-8\r\n"
				"Content-Length: " + std::to_string(body.size()) + "\r\n"
				"Access-Control-Allow-Origin: *\r\n"
				"Connection: close\r\n\r\n";
			send(client, header.data(), (int)header.size(), 0);
			send(client, body.data(), (int)body.size(), 0);
		}
		// Routing: /api/setmap?name=...
		else if (path.rfind("/api/setmap", 0) == 0) {
			auto pos = path.find("name=");
			if (pos != std::string::npos) {
				std::string name = path.substr(pos + 5);
				// URL decode simple spaces
				for (size_t i = 0; i < name.size(); ++i) {
					if (name[i] == '+' || (name[i] == '%' && i + 2 < name.size() && name[i+1] == '2' && name[i+2] == '0')) {
						name[i] = ' ';
					}
				}
				const auto& files = Radar::GetMapFiles();
				for (int i = 0; i < (int)files.size(); ++i) {
					if (files[i] == name) {
						Radar::SetMap(i);
						break;
					}
				}
			}
			std::string body = "{\"status\":\"ok\"}";
			std::string header =
				"HTTP/1.1 200 OK\r\n"
				"Content-Type: application/json\r\n"
				"Content-Length: " + std::to_string(body.size()) + "\r\n"
				"Access-Control-Allow-Origin: *\r\n"
				"Connection: close\r\n\r\n";
			send(client, header.data(), (int)header.size(), 0);
			send(client, body.data(), (int)body.size(), 0);
		}
		// Routing: /api/bvh (2D collision wall contours)
		else if (path == "/api/bvh") {
			json lines = json::array();
			const auto& scene = RobRunReader::GetBvhScene();
			const auto snap = RobRunReader::GetSnapshot();

			if (scene.valid && !scene.tris.empty()) {
				float centerY = snap ? snap->localPosition.y : 0.f;
				float range = 180.f;
				float rangeSq = range * range;
				float cx = snap ? snap->localPosition.x : 0.f;
				float cz = snap ? snap->localPosition.z : 0.f;

				int count = 0;
				for (const auto& tri : scene.tris) {
					// Check height range (near player Y)
					float midY = (tri.v0.y + tri.v1.y + tri.v2.y) * 0.3333f;
					if (std::abs(midY - centerY) > 8.0f) continue;

					// Distance check
					float midX = (tri.v0.x + tri.v1.x + tri.v2.x) * 0.3333f;
					float midZ = (tri.v0.z + tri.v1.z + tri.v2.z) * 0.3333f;
					float dx = midX - cx, dz = midZ - cz;
					if (dx*dx + dz*dz > rangeSq) continue;

					// Normal test: skip horizontal floors/ceilings
					CollisionBVH::Float3 e1 = tri.v1 - tri.v0;
					CollisionBVH::Float3 e2 = tri.v2 - tri.v0;
					CollisionBVH::Float3 n = CollisionBVH::cross(e1, e2);
					float len = std::sqrt(CollisionBVH::dot(n, n));
					if (len < 1e-6f) continue;
					if (std::abs(n.y / len) > 0.65f) continue; // Skip floors

					lines.push_back({ tri.v0.x, tri.v0.z, tri.v1.x, tri.v1.z });
					lines.push_back({ tri.v1.x, tri.v1.z, tri.v2.x, tri.v2.z });
					lines.push_back({ tri.v2.x, tri.v2.z, tri.v0.x, tri.v0.z });

					if (++count > 1200) break; // Limit payload size
				}
			}

			std::string body = lines.dump();
			std::string header =
				"HTTP/1.1 200 OK\r\n"
				"Content-Type: application/json\r\n"
				"Content-Length: " + std::to_string(body.size()) + "\r\n"
				"Access-Control-Allow-Origin: *\r\n"
				"Connection: close\r\n\r\n";
			send(client, header.data(), (int)header.size(), 0);
			send(client, body.data(), (int)body.size(), 0);
		}
		// Routing: /maps/<filename> (Map image binary)
		else if (path.rfind("/maps/", 0) == 0) {
			std::string filename = path.substr(6);
			// Remove query strings if any
			auto q = filename.find('?');
			if (q != std::string::npos) filename = filename.substr(0, q);

			// Security: prevent path traversal
			if (filename.find("..") == std::string::npos && !filename.empty()) {
				std::filesystem::path fullPath = Radar::GetMapsDirectory() / filename;
				std::ifstream f(fullPath, std::ios::binary);
				if (f.is_open()) {
					f.seekg(0, std::ios::end);
					size_t size = (size_t)f.tellg();
					f.seekg(0, std::ios::beg);

					std::vector<char> buf(size);
					f.read(buf.data(), size);

					std::string header =
						"HTTP/1.1 200 OK\r\n"
						"Content-Type: image/png\r\n"
						"Content-Length: " + std::to_string(size) + "\r\n"
						"Cache-Control: public, max-age=600\r\n"
						"Access-Control-Allow-Origin: *\r\n"
						"Connection: close\r\n\r\n";
					send(client, header.data(), (int)header.size(), 0);
					send(client, buf.data(), (int)buf.size(), 0);
					closesocket(client);
					continue;
				}
			}

			std::string notFound = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
			send(client, notFound.data(), (int)notFound.size(), 0);
		}
		else {
			std::string notFound = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
			send(client, notFound.data(), (int)notFound.size(), 0);
		}

		closesocket(client);
	}

	if (g_listenSocket != INVALID_SOCKET) {
		closesocket(g_listenSocket);
		g_listenSocket = INVALID_SOCKET;
	}
}

} // namespace

void Init()
{
	if (ConfigInstance.Radar.WebRadarEnabled) {
		Start(ConfigInstance.Radar.WebRadarPort);
	}
}

bool Start(int port)
{
	Stop();
	if (port <= 0 || port > 65535) port = 8888;
	g_serverPort = port;
	g_serverRunning.store(true);
	g_serverThread = std::thread(RunServerLoop, port);
	return true;
}

void Stop()
{
	if (g_serverRunning.load()) {
		g_serverRunning.store(false);
		if (g_listenSocket != INVALID_SOCKET) {
			closesocket(g_listenSocket);
			g_listenSocket = INVALID_SOCKET;
		}
		if (g_serverThread.joinable()) {
			g_serverThread.join();
		}
	}
}

bool IsRunning()
{
	return g_serverRunning.load();
}

int GetPort()
{
	return g_serverPort;
}

std::string GetUrl()
{
	return "http://localhost:" + std::to_string(g_serverPort);
}

void OpenInBrowser()
{
	if (!IsRunning()) {
		Start(ConfigInstance.Radar.WebRadarPort);
	}
	std::string url = GetUrl();
	ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

} // namespace WebRadar
