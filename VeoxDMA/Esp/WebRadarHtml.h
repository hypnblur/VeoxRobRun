#pragma once
#include <string>

namespace WebRadar {

inline const std::string& GetWebRadarHtml()
{
	static const std::string kHtml = std::string(R"raw_html(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
<title>VeoxDMA — Web Radar</title>
<style>
  :root {
    --bg: #101016;
    --card: #14141c;
    --elevated: #1a1a24;
    --control: #1c1c27;
    --stroke: #1e1e29;
    --stroke-soft: #191923;
    --track: #232330;
    --text: #e4e4ee;
    --text-mid: #b6b6c8;
    --text-dim: #68687c;
    --text-faint: #4c4c5e;
    --accent: #8b5cf6;
    --accent-bright: #a884ff;
    --green: #46c278;
    --amber: #e6b84c;
    --red: #e55353;
    --teal: #2ec4b6;
    --radius-card: 10px;
    --radius-ctrl: 6px;
  }

  * { box-sizing: border-box; margin: 0; padding: 0; }
  html, body {
    width: 100%; height: 100%;
    background: var(--bg);
    color: var(--text);
    font-family: "Maven Pro", "Segoe UI", -apple-system, BlinkMacSystemFont, Roboto, sans-serif;
    font-size: 12px;
    overflow: hidden;
    user-select: none;
    -webkit-user-select: none;
  }

  /* App Shell: Topbar + Main (Canvas + Collapsible Drawer) + Footer */
  .app {
    display: flex;
    flex-direction: column;
    width: 100%;
    height: 100%;
    background: var(--bg);
  }

  /* Top Bar (matches VeoxMenu kTop header) */
  .topbar {
    height: 50px;
    min-height: 50px;
    background: var(--card);
    border-bottom: 1px solid var(--stroke);
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 0 14px;
    gap: 12px;
    z-index: 50;
  }

  .topbar-left, .topbar-center, .topbar-right {
    display: flex;
    align-items: center;
    gap: 10px;
  }

  .brand {
    display: flex;
    align-items: center;
    gap: 9px;
    padding-right: 12px;
    border-right: 1px solid var(--stroke);
  }
  .brand-mark {
    width: 28px; height: 28px;
    border-radius: 7px;
    background: var(--elevated);
    border: 1px solid var(--stroke);
    display: flex;
    align-items: center;
    justify-content: center;
    color: var(--accent);
    font-weight: 800;
    font-size: 14px;
    letter-spacing: -0.5px;
  }
  .brand-title {
    display: flex;
    flex-direction: column;
    line-height: 1.1;
  }
  .brand-name {
    font-size: 13px;
    font-weight: 700;
    color: var(--text);
    letter-spacing: 0.4px;
  }
  .brand-sub {
    font-size: 10px;
    font-weight: 600;
    color: var(--text-dim);
    letter-spacing: 0.8px;
    text-transform: uppercase;
  }

  /* Status Pill */
  .status-pill {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    height: 26px;
    padding: 0 10px;
    border-radius: 999px;
    background: var(--elevated);
    border: 1px solid var(--stroke);
    font-size: 11px;
    font-weight: 600;
    color: var(--text-dim);
  }
  .status-dot {
    width: 7px; height: 7px;
    border-radius: 50%;
    background: var(--red);
  }
  .status-pill.online { color: var(--text); border-color: rgba(70, 194, 120, 0.35); }
  .status-pill.online .status-dot { background: var(--green); }
  .status-pill.waiting .status-dot { background: var(--amber); }

  /* Segmented Mode Switch (Full Map / Minimap) */
  .segmented {
    display: inline-flex;
    background: var(--bg);
    border: 1px solid var(--stroke);
    border-radius: 7px;
    padding: 2px;
    gap: 2px;
  }
  .seg-btn {
    border: none;
    background: transparent;
    color: var(--text-dim);
    font-family: inherit;
    font-size: 11.5px;
    font-weight: 600;
    padding: 5px 12px;
    border-radius: 5px;
    cursor: pointer;
    transition: background 0.14s, color 0.14s;
  }
  .seg-btn:hover { color: var(--text-mid); }
  .seg-btn.active {
    background: var(--accent);
    color: #fff;
  }

  /* Flat Matte Select & Buttons (VeoxMenu style) */
  .veox-select {
    background: var(--control);
    color: var(--text);
    border: 1px solid var(--stroke);
    border-radius: var(--radius-ctrl);
    padding: 5px 10px;
    font-family: inherit;
    font-size: 11.5px;
    font-weight: 600;
    outline: none;
    cursor: pointer;
  }
  .veox-select:hover, .veox-select:focus {
    border-color: var(--accent);
  }
  .veox-select option {
    background: var(--card);
    color: var(--text);
  }

  .veox-btn {
    background: var(--control);
    color: var(--text-mid);
    border: 1px solid var(--stroke);
    border-radius: var(--radius-ctrl);
    height: 28px;
    padding: 0 10px;
    font-family: inherit;
    font-size: 11.5px;
    font-weight: 600;
    cursor: pointer;
    display: inline-flex;
    align-items: center;
    justify-content: center;
    gap: 5px;
    transition: background 0.12s, border-color 0.12s, color 0.12s;
  }
  .veox-btn:hover {
    background: var(--elevated);
    border-color: var(--accent);
    color: var(--text);
  }
  .veox-btn.accent {
    background: var(--accent);
    border-color: var(--accent);
    color: #fff;
  }
  .veox-btn.accent:hover {
    background: var(--accent-bright);
    border-color: var(--accent-bright);
  }
  .veox-btn.icon-only {
    width: 28px;
    padding: 0;
  }
)raw_html") + R"raw_html(
  /* Workspace */
  .workspace {
    position: relative;
    flex: 1;
    display: flex;
    min-height: 0;
    overflow: hidden;
  }

  .canvas-wrap {
    position: relative;
    flex: 1;
    min-width: 0;
    background: var(--bg);
    overflow: hidden;
  }

  #radarCanvas {
    position: absolute;
    inset: 0;
    width: 100%;
    height: 100%;
    display: block;
    cursor: grab;
  }
  #radarCanvas:active { cursor: grabbing; }

  /* Floating Compass Badge */
  .compass {
    position: absolute;
    top: 12px; left: 12px;
    width: 40px; height: 40px;
    border-radius: 50%;
    background: rgba(20, 20, 28, 0.92);
    border: 1px solid var(--stroke);
    display: flex;
    align-items: center;
    justify-content: center;
    font-size: 12px;
    font-weight: 800;
    color: var(--red);
    pointer-events: none;
    z-index: 10;
  }
  .compass::after {
    content: "";
    position: absolute;
    top: 3px;
    width: 0; height: 0;
    border-left: 4px solid transparent;
    border-right: 4px solid transparent;
    border-bottom: 5px solid var(--red);
  }

  /* Quick Zoom Stack (bottom-left of canvas) */
  .zoom-stack {
    position: absolute;
    bottom: 12px; left: 12px;
    display: flex;
    flex-direction: column;
    gap: 4px;
    z-index: 10;
  }
  .zoom-stack .veox-btn {
    width: 30px; height: 30px;
    background: rgba(20, 20, 28, 0.92);
    font-size: 14px;
  }

  /* In-Menu / Lobby Overlay (VeoxMenu flat matte card) */
  .menu-overlay {
    position: absolute;
    inset: 0;
    background: rgba(16, 16, 22, 0.88);
    backdrop-filter: blur(6px);
    -webkit-backdrop-filter: blur(6px);
    display: flex;
    align-items: center;
    justify-content: center;
    z-index: 15;
    transition: opacity 0.2s ease;
  }
  .menu-overlay.hidden {
    opacity: 0;
    pointer-events: none;
  }
  .menu-card {
    width: 360px;
    max-width: calc(100% - 32px);
    background: var(--card);
    border: 1px solid var(--stroke);
    border-radius: 12px;
    padding: 26px 24px 22px;
    display: flex;
    flex-direction: column;
    align-items: center;
    text-align: center;
    gap: 12px;
    box-shadow: 0 18px 48px rgba(0, 0, 0, 0.55);
  }
  .menu-emblem {
    width: 52px; height: 52px;
    border-radius: 12px;
    background: var(--elevated);
    border: 1px solid var(--stroke);
    display: flex;
    align-items: center;
    justify-content: center;
    font-size: 24px;
    font-weight: 800;
    color: var(--accent);
    position: relative;
  }
  .menu-emblem::after {
    content: "";
    position: absolute;
    inset: -5px;
    border-radius: 15px;
    border: 1px solid var(--accent);
    opacity: 0.35;
    animation: veoxPulse 2.2s ease-in-out infinite;
  }
  @keyframes veoxPulse {
    0%, 100% { transform: scale(0.96); opacity: 0.25; }
    50% { transform: scale(1.04); opacity: 0.6; }
  }
  .menu-title {
    font-size: 18px;
    font-weight: 700;
    color: var(--text);
    letter-spacing: 0.3px;
  }
  .menu-sub {
    font-size: 12px;
    color: var(--text-dim);
    line-height: 1.45;
  }
  .menu-divider {
    width: 100%;
    height: 1px;
    background: var(--stroke-soft);
    margin: 4px 0;
  }
  .menu-actions {
    width: 100%;
    display: flex;
    align-items: center;
    gap: 8px;
  }
  .menu-actions .veox-select {
    flex: 1;
    height: 30px;
  }
  .menu-actions .veox-btn {
    height: 30px;
    padding: 0 14px;
  }
  .menu-meta {
    display: flex;
    align-items: center;
    gap: 8px;
    font-size: 11px;
    color: var(--text-dim);
  }
  .menu-meta b { color: var(--text-mid); font-weight: 600; }

  /* Right Control Panel (VeoxMenu flat matte cards) */
  .drawer {
    width: 286px;
    min-width: 286px;
    background: var(--bg);
    border-left: 1px solid var(--stroke);
    padding: 10px;
    display: flex;
    flex-direction: column;
    gap: 10px;
    overflow-y: auto;
    z-index: 20;
    transition: margin-right 0.16s ease;
  }
  .drawer.collapsed {
    margin-right: -286px;
    border-left: none;
  }
  .drawer::-webkit-scrollbar { width: 5px; }
  .drawer::-webkit-scrollbar-track { background: transparent; }
  .drawer::-webkit-scrollbar-thumb { background: var(--track); border-radius: 4px; }

  .card {
    background: var(--card);
    border: 1px solid var(--stroke);
    border-radius: var(--radius-card);
    overflow: hidden;
  }
  .card-header {
    height: 32px;
    padding: 0 12px;
    display: flex;
    align-items: center;
    justify-content: space-between;
    border-bottom: 1px solid var(--stroke-soft);
    font-size: 11px;
    font-weight: 700;
    letter-spacing: 0.6px;
    text-transform: uppercase;
    color: var(--text-mid);
  }
  .card-badge {
    font-size: 10px;
    font-weight: 700;
    color: var(--accent-bright);
    background: var(--elevated);
    padding: 1px 7px;
    border-radius: 999px;
    border: 1px solid var(--stroke);
  }
  .card-body {
    padding: 8px 12px;
    display: flex;
    flex-direction: column;
    gap: 7px;
  }

  /* Pill-only Toggle Row (matches VeoxMenu::ToggleRow) */
  .toggle-row {
    display: flex;
    align-items: center;
    justify-content: space-between;
    min-height: 22px;
    cursor: pointer;
    gap: 8px;
  }
  .toggle-row span {
    color: var(--text-mid);
    font-size: 12px;
    font-weight: 500;
    transition: color 0.12s;
  }
  .toggle-row:hover span { color: var(--text); }
  .pill {
    position: relative;
    width: 34px;
    height: 18px;
    border-radius: 999px;
    background: var(--track);
    border: 1px solid var(--stroke);
    transition: background 0.15s, border-color 0.15s;
    flex-shrink: 0;
  }
  .pill::after {
    content: "";
    position: absolute;
    top: 2px; left: 2px;
    width: 12px; height: 12px;
    border-radius: 50%;
    background: var(--text-dim);
    transition: transform 0.15s, background 0.15s;
  }
  .toggle-row input { display: none; }
  .toggle-row input:checked + .pill {
    background: var(--accent);
    border-color: var(--accent);
  }
  .toggle-row input:checked + .pill::after {
    transform: translateX(16px);
    background: #fff;
  }

  /* Inline Slider Row (matches VeoxMenu::SliderFloatRow) */
  .slider-row {
    display: flex;
    flex-direction: column;
    gap: 4px;
  }
  .slider-head {
    display: flex;
    justify-content: space-between;
    font-size: 11.5px;
    color: var(--text-mid);
  }
  .slider-val {
    color: var(--text);
    font-weight: 700;
    font-variant-numeric: tabular-nums;
  }
  input[type="range"] {
    -webkit-appearance: none;
    appearance: none;
    width: 100%;
    height: 5px;
    border-radius: 999px;
    background: var(--track);
    outline: none;
    cursor: pointer;
  }
  input[type="range"]::-webkit-slider-thumb {
    -webkit-appearance: none;
    appearance: none;
    width: 13px; height: 13px;
    border-radius: 50%;
    background: var(--accent);
    border: 2px solid var(--card);
    cursor: pointer;
  }

  .select-row {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 8px;
  }
  .select-row label {
    color: var(--text-mid);
    font-size: 11.5px;
    font-weight: 500;
  }
  .select-row .veox-select {
    min-width: 120px;
    padding: 3px 8px;
    font-size: 11px;
  }

  /* Live Players List */
  .player-list {
    display: flex;
    flex-direction: column;
    gap: 3px;
    max-height: 210px;
    overflow-y: auto;
  }
  .player-list::-webkit-scrollbar { width: 4px; }
  .player-list::-webkit-scrollbar-thumb { background: var(--track); border-radius: 4px; }
  .player-item {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 4px 7px;
    border-radius: 6px;
    background: var(--elevated);
    border: 1px solid transparent;
    cursor: pointer;
    gap: 6px;
    font-size: 11.5px;
  }
  .player-item:hover { border-color: var(--accent); }
  .player-left {
    display: flex;
    align-items: center;
    gap: 6px;
    min-width: 0;
  }
  .role-dot {
    width: 7px; height: 7px;
    border-radius: 50%;
    flex-shrink: 0;
  }
  .player-name {
    color: var(--text);
    font-weight: 600;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .player-meta {
    color: var(--text-dim);
    font-size: 10.5px;
    font-variant-numeric: tabular-nums;
    flex-shrink: 0;
  }

  /* Footer (matches VeoxMenu kFooter) */
  .footer {
    height: 30px;
    min-height: 30px;
    background: var(--card);
    border-top: 1px solid var(--stroke);
    padding: 0 14px;
    display: flex;
    align-items: center;
    justify-content: space-between;
    font-size: 11px;
    color: var(--text-dim);
    z-index: 50;
  }
  .footer-group {
    display: flex;
    align-items: center;
    gap: 14px;
  }
  .footer-val {
    color: var(--text);
    font-weight: 600;
    font-variant-numeric: tabular-nums;
  }
  .legend-inline {
    display: flex;
    align-items: center;
    gap: 10px;
  }
  .legend-chip {
    display: inline-flex;
    align-items: center;
    gap: 4px;
    font-size: 10.5px;
    color: var(--text-mid);
  }
  .legend-chip i {
    display: inline-block;
    width: 7px; height: 7px;
    border-radius: 50%;
  }

  @media (max-width: 780px) {
    .legend-inline { display: none; }
    .drawer { position: absolute; right: 0; top: 0; bottom: 0; box-shadow: -8px 0 24px rgba(0,0,0,0.5); }
  }
</style>
</head>
)raw_html" + R"raw_html(<body>
<div class="app">
  <!-- Top Header -->
  <header class="topbar">
    <div class="topbar-left">
      <div class="brand">
        <div class="brand-mark">V</div>
        <div class="brand-title">
          <span class="brand-name">VeoxDMA</span>
          <span class="brand-sub">Web Radar</span>
        </div>
      </div>
      <div id="statusPill" class="status-pill waiting">
        <span class="status-dot"></span>
        <span id="statusText">CONNECTING</span>
      </div>
    </div>

    <div class="topbar-center">
      <select id="mapSelect" class="veox-select" title="Active Map"></select>
    </div>

    <div class="topbar-right">
      <button id="btnCenter" class="veox-btn" title="Center on Local Player [Space]">Center</button>
      <button id="btnReset" class="veox-btn" title="Reset View">Reset</button>
      <button id="btnDrawer" class="veox-btn accent" title="Toggle Controls Panel">Panel</button>
    </div>
  </header>

  <!-- Main Workspace -->
  <div class="workspace">
    <div class="canvas-wrap">
      <canvas id="radarCanvas"></canvas>
      <div id="compass" class="compass">N</div>
      <div class="zoom-stack">
        <button id="btnZoomIn" class="veox-btn" title="Zoom In (+)">+</button>
        <button id="btnZoomOut" class="veox-btn" title="Zoom Out (-)">-</button>
      </div>

      <!-- In-Menu / Lobby Screen -->
      <div id="menuOverlay" class="menu-overlay">
        <div class="menu-card">
          <div class="menu-emblem">V</div>
          <div id="menuTitle" class="menu-title">In Menu</div>
          <div id="menuSub" class="menu-sub">Waiting for a battle to start. The map will switch and lock onto your position automatically on spawn.</div>
          <div class="menu-meta">
            <span>Status: <b id="menuConn">Connecting...</b></span>
            <span>·</span>
            <span>Location: <b id="menuLoc">Lobby</b></span>
          </div>
          <div class="menu-divider"></div>
          <div class="menu-actions">
            <select id="menuMapSelect" class="veox-select" title="Preview Map"></select>
            <button id="btnPreviewMap" class="veox-btn accent">Preview Map</button>
          </div>
        </div>
      </div>
    </div>

    <!-- Flat Matte Control Drawer -->
    <aside id="drawer" class="drawer">
      <div class="card">
        <div class="card-header"><span>View &amp; Tracking</span></div>
        <div class="card-body">
          <label class="toggle-row"><span>Follow Local Player</span><input type="checkbox" id="chkFollow" checked><span class="pill"></span></label>
          <label class="toggle-row"><span>Rotate With Camera</span><input type="checkbox" id="chkRotate"><span class="pill"></span></label>
          <label class="toggle-row"><span>Show FOV Cone</span><input type="checkbox" id="chkFov" checked><span class="pill"></span></label>
          <label class="toggle-row"><span>BVH Wall Contours</span><input type="checkbox" id="chkBvh"><span class="pill"></span></label>
          <div class="slider-row">
            <div class="slider-head"><span>Icon Scale</span><span id="valScale" class="slider-val">1.0x</span></div>
            <input type="range" id="rngScale" min="0.5" max="2.0" step="0.05" value="1.0">
          </div>
        </div>
      </div>

      <div class="card">
        <div class="card-header"><span>In-Game Map Layers</span></div>
        <div class="card-body">
          <label class="toggle-row"><span>Players</span><input type="checkbox" id="chkPlayers" checked><span class="pill"></span></label>
          <label class="toggle-row"><span>Player &amp; Driver Names</span><input type="checkbox" id="chkNames" checked><span class="pill"></span></label>
          <label class="toggle-row"><span>Vehicles (Cars &amp; Helis)</span><input type="checkbox" id="chkVehicles" checked><span class="pill"></span></label>
          <label class="toggle-row"><span>Dropped Bags</span><input type="checkbox" id="chkBags"><span class="pill"></span></label>
          <label class="toggle-row"><span>Valuables &amp; Loot</span><input type="checkbox" id="chkLoot"><span class="pill"></span></label>
          <label class="toggle-row"><span>Cargo</span><input type="checkbox" id="chkCargo"><span class="pill"></span></label>
          <div class="select-row">
            <label for="selMinRarity">Loot Rarity</label>
            <select id="selMinRarity" class="veox-select">
              <option value="0">All</option>
              <option value="1">Uncommon+</option>
              <option value="2">Rare+</option>
              <option value="3" selected>Epic+</option>
              <option value="4">Legendary+</option>
              <option value="5">Mythical</option>
            </select>
          </div>
          <div class="select-row">
            <label for="selCargoRarity">Cargo Rarity</label>
            <select id="selCargoRarity" class="veox-select">
              <option value="0">All</option>
              <option value="1" selected>Uncommon+</option>
              <option value="2">Rare+</option>
              <option value="3">Epic+</option>
              <option value="4">Legendary+</option>
              <option value="5">Mythical</option>
            </select>
          </div>
        </div>
      </div>

      <div class="card">
        <div class="card-header">
          <span>Live Players</span>
          <span id="playerCountBadge" class="card-badge">0</span>
        </div>
        <div class="card-body">
          <div id="playerList" class="player-list"></div>
        </div>
      </div>
    </aside>
  </div>

  <!-- Footer -->
  <footer class="footer">
    <div class="footer-group">
      <span>LOC <span id="hudLoc" class="footer-val">-</span></span>
      <span>POS <span id="hudPos" class="footer-val">-</span></span>
      <span>ZOOM <span id="hudZoom" class="footer-val">1.0x</span></span>
      <span>FEED <span id="hudRate" class="footer-val">0 Hz</span></span>
    </div>
    <div class="legend-inline">
      <span class="legend-chip"><i style="background:#ffffff;box-shadow:0 0 0 1px #4c4c5e"></i>You</span>
      <span class="legend-chip"><i style="background:#46e15a"></i>Civilian</span>
      <span class="legend-chip"><i style="background:#4bebeb"></i>Team</span>
      <span class="legend-chip"><i style="background:#3791ff"></i>Police</span>
      <span class="legend-chip"><i style="background:#ff4444"></i>Criminal</span>
      <span class="legend-chip"><i style="background:#ffd02a"></i>Safe / Bag</span>
    </div>
  </footer>
</div>
)raw_html" + R"raw_html(<script>
(() => {
  const canvas = document.getElementById("radarCanvas");
  const ctx = canvas.getContext("2d", { alpha: false });

  // DOM refs
  const statusPill = document.getElementById("statusPill");
  const statusText = document.getElementById("statusText");
  const mapSelect = document.getElementById("mapSelect");
  const btnCenter = document.getElementById("btnCenter");
  const btnReset = document.getElementById("btnReset");
  const btnDrawer = document.getElementById("btnDrawer");
  const drawer = document.getElementById("drawer");
  const btnZoomIn = document.getElementById("btnZoomIn");
  const btnZoomOut = document.getElementById("btnZoomOut");
  const compass = document.getElementById("compass");
  const menuOverlay = document.getElementById("menuOverlay");
  const menuTitle = document.getElementById("menuTitle");
  const menuSub = document.getElementById("menuSub");
  const menuConn = document.getElementById("menuConn");
  const menuLoc = document.getElementById("menuLoc");
  const menuMapSelect = document.getElementById("menuMapSelect");
  const btnPreviewMap = document.getElementById("btnPreviewMap");

  const chkFollow = document.getElementById("chkFollow");
  const chkRotate = document.getElementById("chkRotate");
  const chkFov = document.getElementById("chkFov");
  const chkBvh = document.getElementById("chkBvh");
  const rngScale = document.getElementById("rngScale");
  const valScale = document.getElementById("valScale");

  const chkPlayers = document.getElementById("chkPlayers");
  const chkNames = document.getElementById("chkNames");
  const chkVehicles = document.getElementById("chkVehicles");
  const chkBags = document.getElementById("chkBags");
  const chkLoot = document.getElementById("chkLoot");
  const chkCargo = document.getElementById("chkCargo");
  const selMinRarity = document.getElementById("selMinRarity");
  const selCargoRarity = document.getElementById("selCargoRarity");

  const playerListEl = document.getElementById("playerList");
  const playerCountBadge = document.getElementById("playerCountBadge");
  const hudLoc = document.getElementById("hudLoc");
  const hudPos = document.getElementById("hudPos");
  const hudZoom = document.getElementById("hudZoom");
  const hudRate = document.getElementById("hudRate");

  // -------------------------------------------------------------------------
  // Exact 2-pass embedded sprite loader + offscreen tint cache
  // Matches Radar.cpp::DrawMapSprite / DrawMapSpriteRotated (128x128 PNG pairs)
  // -------------------------------------------------------------------------
  const SPRITE_SLOTS = [
    "player_arrow", "car", "police_car", "helicopter", "dropped_bag",
    "weapon", "armor", "valuable", "cash", "safe", "cargo", "medicine", "key", "generic"
  ];
  const rawSprites = {};
  const tintedCache = new Map();

  for (const slot of SPRITE_SLOTS) {
    const entry = { base: new Image(), fill: new Image(), baseOk: false, fillOk: false };
    entry.base.onload = () => { entry.baseOk = true; tintedCache.clear(); };
    entry.fill.onload = () => { entry.fillOk = true; tintedCache.clear(); };
    entry.base.src = `/sprites/${slot}_base.png`;
    entry.fill.src = `/sprites/${slot}_fill.png`;
    rawSprites[slot] = entry;
  }

  function getTintedSprite(slot, color) {
    const sp = rawSprites[slot];
    if (!sp || !sp.baseOk || !sp.fillOk) return null;
    const key = slot + "|" + color;
    let cached = tintedCache.get(key);
    if (cached) return cached;

    const sz = 64;
    const c = document.createElement("canvas");
    c.width = sz; c.height = sz;
    const g = c.getContext("2d");

    // Pass 1: charcoal outline silhouette (#1c1e24)
    const tmp = document.createElement("canvas");
    tmp.width = sz; tmp.height = sz;
    const tg = tmp.getContext("2d");
    tg.drawImage(sp.base, 0, 0, sz, sz);
    tg.globalCompositeOperation = "source-in";
    tg.fillStyle = "#1c1e24";
    tg.fillRect(0, 0, sz, sz);
    g.drawImage(tmp, 0, 0);

    // Pass 2: vibrant role/rarity tinted interior
    tg.globalCompositeOperation = "source-over";
    tg.clearRect(0, 0, sz, sz);
    tg.drawImage(sp.fill, 0, 0, sz, sz);
    tg.globalCompositeOperation = "source-in";
    tg.fillStyle = color;
    tg.fillRect(0, 0, sz, sz);
    g.drawImage(tmp, 0, 0);

    if (tintedCache.size > 256) tintedCache.clear();
    tintedCache.set(key, c);
    return c;
  }

  // -------------------------------------------------------------------------
  // State + Smooth 60fps Interpolation
  // -------------------------------------------------------------------------
  let state = {
    locationId: "",
    mapName: "",
    mapUrl: "",
    accent: "#8b5cf6",
    accentBright: "#a884ff",
    bounds: { minX: -1232.33, maxX: 1131.41, minZ: -460.78, maxZ: 914.29 },
    local: { valid: false, x: 0, y: 0, z: 0, yaw: 0 },
    inGameMap: { valid: false, fullscreenOpen: false, minimapCamSize: 0 },
    players: [],
    vehicles: [],
    loot: [],
    lootVer: 0,
    availableMaps: []
  };

  let defaultsApplied = false;
  let iconScale = 1.0;
  let previewingInMenu = false;
  let wasInMatch = false;

  const view = {
    zoom: 1.0,
    panX: 1095,
    panY: 637,
    isDragging: false,
    dragStartX: 0,
    dragStartY: 0,
    panStartX: 0,
    panStartY: 0,
    hasFitted: false
  };

  // Smooth entity interpolation map: key -> { x, z, yaw, tx, tz, tyaw }
  const interp = new Map();
  function lerpAngle(a, b, t) {
    let d = b - a;
    while (d > Math.PI) d -= Math.PI * 2;
    while (d < -Math.PI) d += Math.PI * 2;
    return a + d * t;
  }
  function updateTarget(key, x, z, yaw) {
    let e = interp.get(key);
    if (!e) {
      e = { x, z, yaw: yaw || 0, tx: x, tz: z, tyaw: yaw || 0 };
      interp.set(key, e);
    } else {
      const dx = x - e.tx, dz = z - e.tz;
      if (dx * dx + dz * dz > 2500) { // > 50m teleport snap
        e.x = x; e.z = z; e.yaw = yaw || 0;
      }
      e.tx = x; e.tz = z;
      if (yaw !== undefined) e.tyaw = yaw;
    }
    return e;
  }

  // Map image & BVH cache
  let currentMapUrl = "";
  let mapImage = new Image();
  let mapLoaded = false;
  let mapBgColor = "#101016";
  let bvhLines = [];
  let lastBvhFetch = 0;

  // Sample border pixels of the loaded map so the canvas extends seamlessly
  // with the exact ocean/sand/background color of whichever map is active.
  function detectMapEdgeColor(img) {
    try {
      const c = document.createElement("canvas");
      const s = 32;
      c.width = s; c.height = s;
      const g = c.getContext("2d", { willReadFrequently: true });
      g.drawImage(img, 0, 0, s, s);
      const d = g.getImageData(0, 0, s, s).data;
      const rs = [], gs = [], bs = [];
      for (let i = 0; i < s; i++) {
        const pts = [[i, 0], [i, s - 1], [0, i], [s - 1, i]];
        for (const [x, y] of pts) {
          const idx = (y * s + x) * 4;
          if (d[idx + 3] > 200) {
            rs.push(d[idx]); gs.push(d[idx + 1]); bs.push(d[idx + 2]);
          }
        }
      }
      if (rs.length > 0) {
        rs.sort((a, b) => a - b);
        gs.sort((a, b) => a - b);
        bs.sort((a, b) => a - b);
        const m = rs.length >> 1;
        return `rgb(${rs[m]}, ${gs[m]}, ${bs[m]})`;
      }
    } catch (_) {}
    return "#101016";
  }

  // Frame rate telemetry
  let msgCount = 0;
  let lastRateTick = performance.now();

  // Resize with HiDPI support
  let dpr = 1;
  function resize() {
    dpr = Math.min(window.devicePixelRatio || 1, 2);
    const rect = canvas.parentElement.getBoundingClientRect();
    canvas.width = Math.max(1, Math.floor(rect.width * dpr));
    canvas.height = Math.max(1, Math.floor(rect.height * dpr));
    if (!view.hasFitted && mapLoaded) fitMapToCanvas();
  }
  window.addEventListener("resize", resize);
  resize();

  function fitMapToCanvas() {
    const imgW = mapLoaded ? mapImage.width : 4380;
    const imgH = mapLoaded ? mapImage.height : 2548;
    const cw = canvas.width / dpr;
    const ch = canvas.height / dpr;
    const zx = (cw * 0.94) / imgW;
    const zy = (ch * 0.94) / imgH;
    view.zoom = Math.max(0.12, Math.min(zx, zy));
    view.panX = imgW * 0.5;
    view.panY = imgH * 0.5;
    view.hasFitted = true;
    hudZoom.textContent = view.zoom.toFixed(2) + "x";
  }

  // World (X, Z) -> Map Pixel (mx, my), strictly NORTH (+Z) UP!
  function worldToMapPixel(wx, wz) {
    const b = state.bounds;
    const w = (b.maxX - b.minX) || 1.0;
    const h = (b.maxZ - b.minZ) || 1.0;
    const imgW = mapLoaded ? mapImage.width : 2190;
    const imgH = mapLoaded ? mapImage.height : 1274;
    const nx = (wx - b.minX) / w;
    const nz = (b.maxZ - wz) / h;
    return { x: nx * imgW, y: nz * imgH };
  }
)raw_html" + R"raw_html(
  // -------------------------------------------------------------------------
  // UI Event Handlers
  // -------------------------------------------------------------------------
  btnDrawer.onclick = () => {
    const isCollapsed = drawer.classList.toggle("collapsed");
    btnDrawer.classList.toggle("accent", !isCollapsed);
    setTimeout(resize, 170);
  };

  btnCenter.onclick = () => {
    if (state.local && state.local.valid) {
      const lp = worldToMapPixel(state.local.x, state.local.z);
      view.panX = lp.x;
      view.panY = lp.y;
      chkFollow.checked = true;
    }
  };

  btnReset.onclick = () => {
    chkRotate.checked = false;
    chkFollow.checked = false;
    fitMapToCanvas();
  };

  btnZoomIn.onclick = () => {
    view.zoom = Math.min(view.zoom * 1.25, 14.0);
    hudZoom.textContent = view.zoom.toFixed(2) + "x";
  };
  btnZoomOut.onclick = () => {
    view.zoom = Math.max(view.zoom * 0.8, 0.12);
    hudZoom.textContent = view.zoom.toFixed(2) + "x";
  };

  rngScale.oninput = () => {
    iconScale = parseFloat(rngScale.value) || 1.0;
    valScale.textContent = iconScale.toFixed(2).replace(/\.00$/, ".0") + "x";
  };

  mapSelect.onchange = () => {
    if (mapSelect.value) {
      menuMapSelect.value = mapSelect.value;
      fetch("/api/setmap?name=" + encodeURIComponent(mapSelect.value));
    }
  };
  menuMapSelect.onchange = () => {
    if (menuMapSelect.value) {
      mapSelect.value = menuMapSelect.value;
      fetch("/api/setmap?name=" + encodeURIComponent(menuMapSelect.value));
    }
  };
  btnPreviewMap.onclick = () => {
    previewingInMenu = true;
    menuOverlay.classList.add("hidden");
    fitMapToCanvas();
  };

  // Keyboard shortcuts
  window.addEventListener("keydown", (e) => {
    if (e.target && (e.target.tagName === "INPUT" || e.target.tagName === "SELECT")) return;
    if (e.key === "f" || e.key === "F") chkFollow.checked = !chkFollow.checked;
    else if (e.key === "r" || e.key === "R") chkRotate.checked = !chkRotate.checked;
    else if (e.key === " ") { e.preventDefault(); btnCenter.onclick(); }
    else if (e.key === "+" || e.key === "=") btnZoomIn.onclick();
    else if (e.key === "-") btnZoomOut.onclick();
  });

  // Mouse Pan & Zoom
  canvas.addEventListener("wheel", (e) => {
    e.preventDefault();
    const rect = canvas.getBoundingClientRect();
    const factor = e.deltaY < 0 ? 1.15 : 0.87;
    const newZoom = Math.min(Math.max(view.zoom * factor, 0.12), 10.0);
    const mx = (e.clientX - rect.left) - rect.width * 0.5;
    const my = (e.clientY - rect.top) - rect.height * 0.5;
    if (!chkFollow.checked) {
      view.panX -= mx / view.zoom - mx / newZoom;
      view.panY -= my / view.zoom - my / newZoom;
    }
    view.zoom = newZoom;
    hudZoom.textContent = view.zoom.toFixed(2) + "x";
  }, { passive: false });

  canvas.addEventListener("mousedown", (e) => {
    if (e.button === 0 || e.button === 1) {
      view.isDragging = true;
      view.dragStartX = e.clientX;
      view.dragStartY = e.clientY;
      view.panStartX = view.panX;
      view.panStartY = view.panY;
    }
  });
  window.addEventListener("mousemove", (e) => {
    if (!view.isDragging) return;
    const dx = (e.clientX - view.dragStartX) / view.zoom;
    const dy = (e.clientY - view.dragStartY) / view.zoom;
    view.panX = view.panStartX - dx;
    view.panY = view.panStartY - dy;
    chkFollow.checked = false;
  });
  window.addEventListener("mouseup", () => { view.isDragging = false; });

  // Touch Pan / Pinch
  let touchDist = 0, touchZoom = 1.0;
  canvas.addEventListener("touchstart", (e) => {
    if (e.touches.length === 1) {
      view.isDragging = true;
      view.dragStartX = e.touches[0].clientX;
      view.dragStartY = e.touches[0].clientY;
      view.panStartX = view.panX;
      view.panStartY = view.panY;
    } else if (e.touches.length === 2) {
      view.isDragging = false;
      touchDist = Math.hypot(
        e.touches[0].clientX - e.touches[1].clientX,
        e.touches[0].clientY - e.touches[1].clientY
      );
      touchZoom = view.zoom;
    }
  }, { passive: true });
  canvas.addEventListener("touchmove", (e) => {
    if (e.touches.length === 1 && view.isDragging) {
      const dx = (e.touches[0].clientX - view.dragStartX) / view.zoom;
      const dy = (e.touches[0].clientY - view.dragStartY) / view.zoom;
      view.panX = view.panStartX - dx;
      view.panY = view.panStartY - dy;
      chkFollow.checked = false;
    } else if (e.touches.length === 2 && touchDist > 0) {
      const dist = Math.hypot(
        e.touches[0].clientX - e.touches[1].clientX,
        e.touches[0].clientY - e.touches[1].clientY
      );
      view.zoom = Math.min(Math.max(touchZoom * (dist / touchDist), 0.12), 10.0);
      hudZoom.textContent = view.zoom.toFixed(2) + "x";
    }
  }, { passive: true });
  canvas.addEventListener("touchend", () => { view.isDragging = false; touchDist = 0; });

  // -------------------------------------------------------------------------
  // High-Speed SSE Stream (/api/stream) with /api/state Fallback
  // -------------------------------------------------------------------------
  let lastPlayerListRender = 0;

  function applyStatePayload(data) {
    msgCount++;
    const now = performance.now();
    if (now - lastRateTick >= 1000) {
      hudRate.textContent = Math.round((msgCount * 1000) / (now - lastRateTick)) + " Hz";
      msgCount = 0;
      lastRateTick = now;
    }

    // Preserve cached loot if delta frame omitted it
    const prevLoot = state.loot;
    state = data;
    if (!data.loot) state.loot = prevLoot;

    // Sync menu accent color live
    if (data.accent) {
      document.documentElement.style.setProperty("--accent", data.accent);
      if (data.accentBright)
        document.documentElement.style.setProperty("--accent-bright", data.accentBright);
    }

    // Apply VeoxMenu In-Game Map defaults once on first connect
    if (!defaultsApplied && data.defaults) {
      defaultsApplied = true;
      const d = data.defaults;
      chkPlayers.checked = !!d.players;
      chkVehicles.checked = !!d.vehicles;
      chkNames.checked = d.names !== undefined ? !!d.names : true;
      chkBags.checked = !!d.bags;
      chkLoot.checked = !!d.valuables;
      chkCargo.checked = !!d.cargo;
      if (d.minRarity !== undefined) selMinRarity.value = String(d.minRarity);
      if (d.cargoMinRarity !== undefined) selCargoRarity.value = String(d.cargoMinRarity);
      if (d.scale !== undefined) {
        iconScale = Math.max(0.5, Math.min(2.0, d.scale));
        rngScale.value = String(iconScale);
        valScale.textContent = iconScale.toFixed(2).replace(/\.00$/, ".0") + "x";
      }
    }

    // Update interpolation targets
    if (data.local && data.local.valid) {
      updateTarget("local", data.local.x, data.local.z, data.local.yaw || 0);
    }
    if (data.players) {
      for (const p of data.players) {
        if (!p.isLocal) updateTarget("p:" + p.id, p.x, p.z, p.yaw || 0);
      }
    }
    if (data.vehicles) {
      for (const v of data.vehicles) {
        updateTarget("v:" + v.id, v.x, v.z, 0);
      }
    }

    // Status & HUD + In-Menu screen detection
    const inMatch = !!(data.local && data.local.valid);
    if (inMatch) {
      wasInMatch = true;
      previewingInMenu = false;
      menuOverlay.classList.add("hidden");
      statusPill.className = "status-pill online";
      statusText.textContent = "LIVE";
      hudLoc.textContent = data.locationId || "In Battle";
      hudPos.textContent = `${Math.round(data.local.x)}, ${Math.round(data.local.z)}`;
    } else {
      if (wasInMatch) {
        wasInMatch = false;
        previewingInMenu = false;
      }
      statusPill.className = "status-pill waiting";
      statusText.textContent = "IN MENU";
      hudLoc.textContent = data.locationId || "Menu";
      hudPos.textContent = "-";
      menuTitle.textContent = "In Menu";
      menuSub.textContent = "Waiting for a battle to start. The map will switch and lock onto your position automatically on spawn.";
      menuConn.textContent = "Connected";
      menuLoc.textContent = data.locationId || "Lobby";
      if (!previewingInMenu) menuOverlay.classList.remove("hidden");
    }

    // Map selector sync (topbar + menu card)
    if (data.availableMaps && data.availableMaps.length > 0) {
      if (mapSelect.options.length !== data.availableMaps.length) {
        mapSelect.innerHTML = "";
        menuMapSelect.innerHTML = "";
        for (const m of data.availableMaps) {
          const label = m.replace(/\.png$/i, "").replace(/_map$/i, " (BVH)").replace(/_/g, " ").toUpperCase();
          const opt1 = document.createElement("option");
          opt1.value = m; opt1.textContent = label;
          mapSelect.appendChild(opt1);
          const opt2 = document.createElement("option");
          opt2.value = m; opt2.textContent = label;
          menuMapSelect.appendChild(opt2);
        }
      }
      if (data.mapName && mapSelect.value !== data.mapName) {
        mapSelect.value = data.mapName;
        menuMapSelect.value = data.mapName;
      }
    }

    // Map image load
    if (data.mapUrl && data.mapUrl !== currentMapUrl) {
      currentMapUrl = data.mapUrl;
      mapLoaded = false;
      view.hasFitted = false;
      mapImage = new Image();
      mapImage.onload = () => {
        mapLoaded = true;
        mapBgColor = detectMapEdgeColor(mapImage);
        if (!view.hasFitted) fitMapToCanvas();
      };
      mapImage.src = data.mapUrl;
    }

    // Update live player list at 4 Hz (keeps DOM fast)
    if (now - lastPlayerListRender > 250) {
      lastPlayerListRender = now;
      renderPlayerList(data.players || []);
    }
  }

  function renderPlayerList(players) {
    const others = players.filter(p => !p.isLocal).sort((a, b) => (a.distance || 0) - (b.distance || 0));
    playerCountBadge.textContent = String(others.length);
    const frag = document.createDocumentFragment();
    for (const p of others) {
      const row = document.createElement("div");
      row.className = "player-item";
      row.onclick = () => {
        chkFollow.checked = false;
        const pt = worldToMapPixel(p.x, p.z);
        view.panX = pt.x;
        view.panY = pt.y;
        if (view.zoom < 1.8) {
          view.zoom = 2.0;
          hudZoom.textContent = view.zoom.toFixed(2) + "x";
        }
      };
      const left = document.createElement("div");
      left.className = "player-left";
      const dot = document.createElement("span");
      dot.className = "role-dot";
      dot.style.background = p.color || "#ff4444";
      const nm = document.createElement("span");
      nm.className = "player-name";
      nm.textContent = p.name || p.roleLabel || "Player";
      left.appendChild(dot);
      left.appendChild(nm);

      const meta = document.createElement("span");
      meta.className = "player-meta";
      meta.textContent = (p.inVehicle ? "VEH · " : "") + Math.round(p.distance || 0) + "m";

      row.appendChild(left);
      row.appendChild(meta);
      frag.appendChild(row);
    }
    playerListEl.innerHTML = "";
    playerListEl.appendChild(frag);
  }

  async function fetchBvhLines() {
    const now = Date.now();
    if (now - lastBvhFetch < 3000) return;
    lastBvhFetch = now;
    try {
      const res = await fetch("/api/bvh");
      if (res.ok) bvhLines = await res.json();
    } catch (_) {}
  }

  let sse = null;
  function connectStream() {
    if (sse) { try { sse.close(); } catch (_) {} sse = null; }
    try {
      sse = new EventSource("/api/stream");
      sse.onmessage = (ev) => {
        try { applyStatePayload(JSON.parse(ev.data)); } catch (_) {}
      };
      sse.onerror = () => {
        statusPill.className = "status-pill waiting";
        statusText.textContent = "RECONNECTING";
      };
    } catch (_) {
      // Fallback polling if EventSource unavailable
      setInterval(async () => {
        try {
          const r = await fetch("/api/state");
          if (r.ok) applyStatePayload(await r.json());
        } catch (_) {
          statusPill.className = "status-pill";
          statusText.textContent = "OFFLINE";
        }
      }, 33);
    }
  }
  connectStream();
)raw_html" + R"raw_html(
  // -------------------------------------------------------------------------
  // Crisp Name Pill Helper (matches Radar.cpp::DrawInGameMapOverlay)
  // -------------------------------------------------------------------------
  function drawNamePill(x, y, text, color, fontPx) {
    ctx.font = `700 ${fontPx}px "Maven Pro", "Segoe UI", sans-serif`;
    const w = ctx.measureText(text).width;
    const padX = 5, padY = 2.5;
    const bx = x - w * 0.5 - padX;
    const by = y - fontPx - padY * 2;
    const bw = w + padX * 2;
    const bh = fontPx + padY * 2;

    ctx.fillStyle = "rgba(22, 24, 30, 0.88)";
    ctx.beginPath();
    ctx.roundRect(bx, by, bw, bh, 4);
    ctx.fill();

    ctx.textAlign = "center";
    ctx.textBaseline = "middle";
    ctx.fillStyle = "#000";
    ctx.fillText(text, x + 0.8, by + bh * 0.5 + 0.8);
    ctx.fillStyle = color;
    ctx.fillText(text, x, by + bh * 0.5);
  }

  function drawSpriteIcon(slot, x, y, halfPx, color, angleRad) {
    const img = getTintedSprite(slot, color);
    if (img) {
      if (angleRad) {
        ctx.save();
        ctx.translate(x, y);
        ctx.rotate(angleRad);
        ctx.drawImage(img, -halfPx, -halfPx, halfPx * 2, halfPx * 2);
        ctx.restore();
      } else {
        ctx.drawImage(img, x - halfPx, y - halfPx, halfPx * 2, halfPx * 2);
      }
      return true;
    }
    // Lightweight fallback while PNGs decode on first frame
    ctx.fillStyle = color;
    ctx.strokeStyle = "#1c1e24";
    ctx.lineWidth = 1.6;
    ctx.beginPath();
    ctx.arc(x, y, halfPx * 0.55, 0, Math.PI * 2);
    ctx.fill();
    ctx.stroke();
    return false;
  }

  // -------------------------------------------------------------------------
  // Main 60fps Render Loop (screen-space crisp icons + interpolated positions)
  // -------------------------------------------------------------------------
  let lastFrameMs = performance.now();

  function draw(now) {
    requestAnimationFrame(draw);
    const dt = Math.min(0.1, Math.max(0.001, (now - lastFrameMs) / 1000));
    lastFrameMs = now;
    const smooth = 1 - Math.exp(-dt * 24); // ~40ms crisp follow

    // Step interpolation
    for (const e of interp.values()) {
      e.x += (e.tx - e.x) * smooth;
      e.z += (e.tz - e.z) * smooth;
      e.yaw = lerpAngle(e.yaw, e.tyaw, smooth);
    }

    const cw = canvas.width / dpr;
    const ch = canvas.height / dpr;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);

    // Seamless map-edge background fill (matches ocean/sand/dark map borders)
    ctx.fillStyle = mapLoaded ? mapBgColor : "#101016";
    ctx.fillRect(0, 0, cw, ch);

    // Follow local player smoothly
    const localI = interp.get("local");
    if (chkFollow.checked && state.local && state.local.valid && localI) {
      const lp = worldToMapPixel(localI.x, localI.z);
      view.panX = lp.x;
      view.panY = lp.y;
    }

    const cx = cw * 0.5;
    const cy = ch * 0.5;
    const rotYaw = (chkRotate.checked && state.local && state.local.valid && localI)
      ? localI.yaw
      : 0;

    compass.style.transform = `rotate(${(-rotYaw * 180 / Math.PI).toFixed(1)}deg)`;

    const cosR = Math.cos(-rotYaw);
    const sinR = Math.sin(-rotYaw);

    // Transform map pixel -> screen pixel (so icons & text stay razor-sharp at constant screen size!)
    function mapToScreen(mx, my) {
      const dx = (mx - view.panX) * view.zoom;
      const dy = (my - view.panY) * view.zoom;
      if (rotYaw === 0) return { x: cx + dx, y: cy + dy };
      return {
        x: cx + dx * cosR - dy * sinR,
        y: cy + dx * sinR + dy * cosR
      };
    }
    function worldToScreen(wx, wz) {
      const mp = worldToMapPixel(wx, wz);
      return mapToScreen(mp.x, mp.y);
    }

    // 1. Draw Map Image + Optional BVH Walls in Map Transform
    ctx.save();
    ctx.translate(cx, cy);
    if (rotYaw !== 0) ctx.rotate(-rotYaw);
    ctx.scale(view.zoom, view.zoom);
    ctx.translate(-view.panX, -view.panY);

    const imgW = mapLoaded ? mapImage.width : 4380;
    const imgH = mapLoaded ? mapImage.height : 2548;
    if (mapLoaded) {
      ctx.imageSmoothingEnabled = true;
      ctx.imageSmoothingQuality = "high";
      ctx.drawImage(mapImage, 0, 0, imgW, imgH);
    } else {
      ctx.fillStyle = "#14141c";
      ctx.fillRect(0, 0, imgW, imgH);
      ctx.strokeStyle = "#1e1e29";
      ctx.lineWidth = 2 / view.zoom;
      ctx.strokeRect(0, 0, imgW, imgH);
    }

    if (chkBvh.checked) {
      fetchBvhLines();
      if (bvhLines && bvhLines.length > 0) {
        ctx.strokeStyle = "rgba(139, 92, 246, 0.55)";
        ctx.lineWidth = 1.4 / view.zoom;
        ctx.beginPath();
        for (let i = 0; i < bvhLines.length; i++) {
          const ln = bvhLines[i];
          const p1 = worldToMapPixel(ln[0], ln[1]);
          const p2 = worldToMapPixel(ln[2], ln[3]);
          ctx.moveTo(p1.x, p1.y);
          ctx.lineTo(p2.x, p2.y);
        }
        ctx.stroke();
      }
    }
    ctx.restore();

    const sc = iconScale * 1.15;
    const showNames = chkNames.checked;
    const fontPx = Math.max(11, Math.round(11.5 * sc));

    // 2. Draw Loot, Cargo & Dropped Bags (exact Radar.cpp filtering & rarity icons)
    const wantBags = chkBags.checked;
    const wantLoot = chkLoot.checked;
    const wantCargo = chkCargo.checked;
    if ((wantBags || wantLoot || wantCargo) && state.loot && state.loot.length > 0) {
      const minLootTier = parseInt(selMinRarity.value, 10) + 1;
      const minCargoTier = parseInt(selCargoRarity.value, 10) + 1;
      const halfLoot = 9.5 * sc;
      const halfBag = 10.5 * sc;

      for (let i = 0; i < state.loot.length; i++) {
        const it = state.loot[i];
        if (it.cat === "bag") {
          if (!wantBags) continue;
        } else if (it.cat === "cargo") {
          if (!wantCargo) continue;
          if ((it.tier || 1) < minCargoTier) continue;
        } else {
          if (!wantLoot) continue;
          if (!it.alwaysShow && (it.tier || 1) < minLootTier) continue;
        }

        const sp = worldToScreen(it.x, it.z);
        if (sp.x < -24 || sp.x > cw + 24 || sp.y < -24 || sp.y > ch + 24) continue;
        drawSpriteIcon(it.sprite || "generic", sp.x, sp.y, it.cat === "bag" ? halfBag : halfLoot, it.color || "#64ff8c", 0);
      }
    }

    // 3. Draw Vehicles (Cars, Police Cars, Helicopters — role-tinted with occupant/owner pills)
    if (chkVehicles.checked && state.vehicles && state.vehicles.length > 0) {
      const vehScale = sc * 0.95;
      for (let i = 0; i < state.vehicles.length; i++) {
        const v = state.vehicles[i];
        const vi = interp.get("v:" + v.id);
        const vx = vi ? vi.x : v.x;
        const vz = vi ? vi.z : v.z;
        const sp = worldToScreen(vx, vz);
        if (sp.x < -36 || sp.x > cw + 36 || sp.y < -36 || sp.y > ch + 36) continue;

        const half = (v.sprite === "helicopter" ? 15.0 : (v.sprite === "police_car" ? 14.5 : 14.0)) * vehScale;
        drawSpriteIcon(v.sprite || "car", sp.x, sp.y, half, v.color || "#d2d8e0", 0);

        if (showNames && v.label) {
          drawNamePill(sp.x, sp.y - half + 1, v.label, v.color || "#e4e4ee", fontPx);
        }
      }
    }

    // 4. Draw Other Players (exact Rob Run rotated player_arrow sprite)
    if (chkPlayers.checked && state.players && state.players.length > 0) {
      const arrowHalf = 12.5 * sc;
      for (let i = 0; i < state.players.length; i++) {
        const p = state.players[i];
        if (p.isLocal) continue;

        // When vehicles layer is ON, players inside a matched vehicle are represented
        // cleanly by their role-colored vehicle icon + name pill (exact Radar.cpp logic)
        if (chkVehicles.checked && p.inVehicle && p.matchedVeh) continue;

        const pi = interp.get("p:" + p.id);
        const px = pi ? pi.x : p.x;
        const pz = pi ? pi.z : p.z;
        const pyaw = pi ? pi.yaw : (p.yaw || 0);
        const sp = worldToScreen(px, pz);
        if (sp.x < -36 || sp.x > cw + 36 || sp.y < -36 || sp.y > ch + 36) continue;

        const col = p.color || "#ff4444";
        if (chkVehicles.checked && p.inVehicle) {
          drawSpriteIcon("car", sp.x, sp.y, 13.5 * sc, col, 0);
        } else {
          drawSpriteIcon("player_arrow", sp.x, sp.y, arrowHalf, col, pyaw - rotYaw);
        }

        if (showNames && p.name) {
          drawNamePill(sp.x, sp.y - arrowHalf - 1, p.name, col, fontPx);
        }
      }
    }

    // 5. Draw Local Player (YOU) — ALWAYS ON TOP in crisp pure white + accent halo
    if (state.local && state.local.valid && localI) {
      const sp = worldToScreen(localI.x, localI.z);
      const relYaw = localI.yaw - rotYaw;

      if (chkFov.checked) {
        const coneR = Math.max(48, 74 * sc);
        const fovHalf = Math.PI * 0.22;
        const a = relYaw - Math.PI * 0.5;
        const grad = ctx.createRadialGradient(sp.x, sp.y, 4, sp.x, sp.y, coneR);
        grad.addColorStop(0, "rgba(255, 255, 255, 0.26)");
        grad.addColorStop(1, "rgba(255, 255, 255, 0.0)");
        ctx.fillStyle = grad;
        ctx.beginPath();
        ctx.moveTo(sp.x, sp.y);
        ctx.arc(sp.x, sp.y, coneR, a - fovHalf, a + fovHalf);
        ctx.closePath();
        ctx.fill();
      }

      const myHalf = 14.5 * sc;
      // Subtle high-visibility outer halo so YOU always stand out on top
      ctx.strokeStyle = "rgba(255, 255, 255, 0.55)";
      ctx.lineWidth = 1.6;
      ctx.beginPath();
      ctx.arc(sp.x, sp.y, myHalf * 0.92, 0, Math.PI * 2);
      ctx.stroke();

      drawSpriteIcon("player_arrow", sp.x, sp.y, myHalf, "#ffffff", relYaw);
    }
  }

  requestAnimationFrame(draw);
})();
</script>
</body>
</html>
)raw_html";
	return kHtml;
}

} // namespace WebRadar
