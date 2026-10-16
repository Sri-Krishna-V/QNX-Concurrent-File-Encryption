/* QNX Concurrent Encryption Dashboard Application Logic */

// Global State
let currentWorkers = 4;
let activeTab = 'overview';

// Configuration Constants (matching src/core/config.h)
const ENC_MAX_BLOCK = 2097152; // 2 MB
const ENC_SINGLE_SEGMENT_MAX = 65536; // 64 KB
const ENC_MIN_SEGMENT = 16384; // 16 KB
const ENC_TASKS_PER_WORKER = 4;
const ENC_CIPHER_BLOCK = 16;

// Tab Switching
function switchTab(tabId) {
  activeTab = tabId;
  document.querySelectorAll('.tab-pane').forEach(el => el.classList.remove('active'));
  document.querySelectorAll('.nav-tab').forEach(el => el.classList.remove('active'));

  const targetPane = document.getElementById(`tab-${tabId}`);
  if (targetPane) targetPane.classList.add('active');

  const targetBtn = document.querySelector(`.nav-tab[data-tab="${tabId}"]`);
  if (targetBtn) targetBtn.classList.add('active');

  window.location.hash = tabId;

  // Trigger chart re-render if simulator tab is activated
  if (tabId === 'simulator') {
    setTimeout(renderBenchmarkChart, 50);
  }
}

// Phase Scrolling Navigation
function scrollToPhase(phaseId) {
  const el = document.getElementById(phaseId);
  if (el) {
    el.scrollIntoView({ behavior: 'smooth', block: 'start' });
    document.querySelectorAll('.phase-nav-item').forEach(item => item.classList.remove('active'));
    const link = document.querySelector(`.phase-nav-item[href="#${phaseId}"]`);
    if (link) link.classList.add('active');
  }
}

// Copy to Clipboard with Visual Feedback
function copySnippet(elementId) {
  const el = document.getElementById(elementId);
  if (!el) return;
  const text = el.innerText || el.textContent;

  navigator.clipboard.writeText(text).then(() => {
    showToast('Copied to clipboard!');
  }).catch(() => {
    // Fallback
    const textarea = document.createElement('textarea');
    textarea.value = text;
    document.body.appendChild(textarea);
    textarea.select();
    document.execCommand('copy');
    document.body.removeChild(textarea);
    showToast('Copied to clipboard!');
  });
}

// Toast Notification
function showToast(msg) {
  const toast = document.getElementById('toast');
  const toastMsg = document.getElementById('toastMsg');
  if (!toast || !toastMsg) return;

  toastMsg.textContent = msg;
  toast.classList.add('show');
  setTimeout(() => {
    toast.classList.remove('show');
  }, 2200);
}

// Segment Planning Math (exact match to src/core/segment.c)
function roundUp(value, multiple) {
  return Math.floor((value + multiple - 1) / multiple) * multiple;
}

function calculateSegmentSize(len, numWorkers) {
  if (len <= ENC_SINGLE_SEGMENT_MAX) {
    return len;
  }
  if (numWorkers === 0) numWorkers = 1;

  const target = Math.floor((len + numWorkers * ENC_TASKS_PER_WORKER - 1) / (numWorkers * ENC_TASKS_PER_WORKER));
  const boundedTarget = Math.max(ENC_MIN_SEGMENT, target);
  return roundUp(boundedTarget, ENC_CIPHER_BLOCK);
}

// Concurrency Simulation Engine
function setWorkers(w) {
  currentWorkers = w;
  document.querySelectorAll('.btn-option').forEach(btn => {
    btn.classList.toggle('active', parseInt(btn.dataset.workers) === w);
  });
  updateSimulation();
}

function formatBytes(bytes) {
  if (bytes < 1024) return bytes + ' B';
  if (bytes < 1048576) return (bytes / 1024).toFixed(1) + ' KB';
  return (bytes / 1048576).toFixed(2) + ' MB';
}

function updateSimulation() {
  const blockSizeSlider = document.getElementById('sliderBlockSize');
  const jobsSlider = document.getElementById('sliderJobs');
  if (!blockSizeSlider) return;

  const len = parseInt(blockSizeSlider.value);
  const jobs = jobsSlider ? parseInt(jobsSlider.value) : 4;

  // Labels
  document.getElementById('lblBlockSize').textContent = `${len.toLocaleString()} B (${formatBytes(len)})`;
  document.getElementById('lblWorkers').textContent = `${currentWorkers} Worker${currentWorkers > 1 ? 's' : ''}`;
  if (document.getElementById('lblJobs')) {
    document.getElementById('lblJobs').textContent = `${jobs} Job${jobs > 1 ? 's' : ''} in flight`;
  }

  // Segment Computation
  const segSize = calculateSegmentSize(len, currentWorkers);
  const totalSegs = len === 0 ? 0 : Math.ceil(len / segSize);

  // Update Metric Cards
  document.getElementById('valSegSize').textContent = `${segSize.toLocaleString()} B`;
  document.getElementById('valSegSizeLabel').textContent = formatBytes(segSize);
  document.getElementById('valTotalSegs').textContent = totalSegs;

  const segsPerWorker = (totalSegs / currentWorkers).toFixed(1);
  document.getElementById('valSegRatio').textContent = `~${segsPerWorker} per worker`;

  // Theoretical Throughput on Pi 4B (1 Core = ~10.3 MB/s pure AES-CTR)
  const singleCoreSpeedMB = 10.3;
  let speedup = 1.0;
  if (currentWorkers === 2) speedup = 1.98;
  else if (currentWorkers === 4) speedup = 3.84;
  else if (currentWorkers === 8) speedup = 3.86; // 4 hardware cores plateau
  else if (currentWorkers === 16) speedup = 3.82;

  const totalSpeedMB = singleCoreSpeedMB * speedup;
  const timeMs = (len / (totalSpeedMB * 1048576)) * 1000;

  document.getElementById('valEstTime').textContent = `${timeMs.toFixed(1)} ms`;
  document.getElementById('valEstSpeed').textContent = `~${totalSpeedMB.toFixed(1)} MiB/s`;
  document.getElementById('valSpeedup').textContent = `${speedup.toFixed(2)}x`;

  document.getElementById('lblSegSummary').textContent = `${totalSegs} Task${totalSegs !== 1 ? 's' : ''} distributed over ${currentWorkers} threads`;

  // Render Visual Segment Blocks
  renderSegmentBlocks(totalSegs, segSize, len, currentWorkers);
}

function renderSegmentBlocks(totalSegs, segSize, totalLen, numWorkers) {
  const container = document.getElementById('segmentBlocksGrid');
  if (!container) return;

  container.innerHTML = '';
  const displayLimit = Math.min(totalSegs, 48); // limit DOM elements for performance

  for (let i = 0; i < displayLimit; i++) {
    const workerId = i % numWorkers;
    const isLast = i === totalSegs - 1;
    const thisLen = isLast ? (totalLen - i * segSize) : segSize;

    const block = document.createElement('div');
    block.className = `segment-block worker-${workerId % 8}`;
    block.title = `Segment #${i + 1}: ${formatBytes(thisLen)} -> Assigned to Worker ${workerId}`;
    block.innerHTML = `
      <div style="display:flex; justify-content:space-between; font-weight:700;">
        <span>Seg #${i + 1}</span>
        <span style="opacity:0.85;">W${workerId}</span>
      </div>
      <div style="font-size:0.68rem; opacity:0.9;">${formatBytes(thisLen)}</div>
    `;
    container.appendChild(block);
  }

  if (totalSegs > displayLimit) {
    const overflow = document.createElement('div');
    overflow.className = 'segment-block';
    overflow.style.background = '#64748b';
    overflow.style.justifyContent = 'center';
    overflow.style.alignItems = 'center';
    overflow.innerHTML = `<span>+${totalSegs - displayLimit} more</span>`;
    container.appendChild(overflow);
  }
}

// Benchmark Chart Rendering (Hi-DPI Canvas)
function renderBenchmarkChart() {
  const canvas = document.getElementById('benchmarkChart');
  if (!canvas) return;

  const ctx = canvas.getContext('2d');
  const dpr = window.devicePixelRatio || 1;
  const width = canvas.parentElement.clientWidth - 48;
  const height = 280;

  canvas.width = width * dpr;
  canvas.height = height * dpr;
  canvas.style.width = width + 'px';
  canvas.style.height = height + 'px';
  ctx.scale(dpr, dpr);

  ctx.clearRect(0, 0, width, height);

  // Data Points from Actual Lab Benchmarks (2MB blocks on Pi 4B)
  const data = [
    { workers: '1 Worker', speed: 10.3, speedup: '1.00x', time: '194.2 ms' },
    { workers: '2 Workers', speed: 20.4, speedup: '1.98x', time: '98.1 ms' },
    { workers: '4 Workers (Cores)', speed: 39.5, speedup: '3.84x', time: '50.6 ms' },
    { workers: '8 Workers', speed: 39.8, speedup: '3.86x', time: '50.2 ms' }
  ];

  const padding = { top: 35, right: 30, bottom: 45, left: 55 };
  const chartW = width - padding.left - padding.right;
  const chartH = height - padding.top - padding.bottom;
  const maxSpeed = 50;

  // Grid Lines
  ctx.strokeStyle = '#e2e8f0';
  ctx.lineWidth = 1;
  for (let s = 0; s <= maxSpeed; s += 10) {
    const y = padding.top + chartH - (s / maxSpeed) * chartH;
    ctx.beginPath();
    ctx.moveTo(padding.left, y);
    ctx.lineTo(width - padding.right, y);
    ctx.stroke();

    ctx.fillStyle = '#94a3b8';
    ctx.font = '11px Inter, sans-serif';
    ctx.textAlign = 'right';
    ctx.fillText(`${s} MB/s`, padding.left - 10, y + 4);
  }

  // Draw Bars
  const barWidth = Math.min(65, chartW / data.length - 25);
  data.forEach((d, i) => {
    const x = padding.left + (i * (chartW / data.length)) + (chartW / data.length - barWidth) / 2;
    const barH = (d.speed / maxSpeed) * chartH;
    const y = padding.top + chartH - barH;

    // Gradient Fill
    const grad = ctx.createLinearGradient(x, y, x, padding.top + chartH);
    if (i === 2) {
      // 4 Cores Highlight
      grad.addColorStop(0, '#4f46e5');
      grad.addColorStop(1, '#818cf8');
    } else {
      grad.addColorStop(0, '#06b6d4');
      grad.addColorStop(1, '#67e8f9');
    }

    ctx.fillStyle = grad;
    roundRect(ctx, x, y, barWidth, barH, 6);
    ctx.fill();

    // Value Labels on top of bar
    ctx.fillStyle = '#0f172a';
    ctx.font = 'bold 12px Inter, sans-serif';
    ctx.textAlign = 'center';
    ctx.fillText(`${d.speed} MiB/s`, x + barWidth / 2, y - 10);

    // Speedup Subtext
    ctx.fillStyle = i === 2 ? '#4f46e5' : '#0891b2';
    ctx.font = 'bold 11px Inter, sans-serif';
    ctx.fillText(d.speedup, x + barWidth / 2, y - 24);

    // X Axis Labels
    ctx.fillStyle = '#475569';
    ctx.font = '12px Inter, sans-serif';
    ctx.fillText(d.workers, x + barWidth / 2, padding.top + chartH + 20);

    // Latency Label
    ctx.fillStyle = '#94a3b8';
    ctx.font = '10px Inter, sans-serif';
    ctx.fillText(d.time, x + barWidth / 2, padding.top + chartH + 34);
  });
}

function roundRect(ctx, x, y, width, height, radius) {
  ctx.beginPath();
  ctx.moveTo(x + radius, y);
  ctx.lineTo(x + width - radius, y);
  ctx.quadraticCurveTo(x + width, y, x + width, y + radius);
  ctx.lineTo(x + width, y + height);
  ctx.lineTo(x, y + height);
  ctx.lineTo(x, y + radius);
  ctx.quadraticCurveTo(x, y, x + radius, y);
  ctx.closePath();
}

// Troubleshooting Filter & Toggle
function filterTrouble(category, btn) {
  document.querySelectorAll('.filter-btn').forEach(b => b.classList.remove('active'));
  btn.classList.add('active');

  const cards = document.querySelectorAll('.trouble-card');
  cards.forEach(card => {
    if (category === 'all' || card.dataset.category === category) {
      card.style.display = 'block';
    } else {
      card.style.display = 'none';
    }
  });
}

function toggleTrouble(header) {
  const card = header.parentElement;
  const content = card.querySelector('.trouble-content');
  const span = header.querySelector('span:last-child');
  if (content.style.display === 'block') {
    content.style.display = 'none';
    span.textContent = 'Expand ▾';
  } else {
    content.style.display = 'block';
    span.textContent = 'Collapse ▴';
  }
}

// AES-CTR Counter Arithmetic Calculator (Big-endian addition matching cipher_common.c)
function calculateCounter() {
  const ivInput = document.getElementById('calcIV');
  const offsetInput = document.getElementById('calcOffset');
  const resultDisplay = document.getElementById('calcResult');
  if (!ivInput || !offsetInput || !resultDisplay) return;

  let ivHex = ivInput.value.replace(/[^0-9a-fA-F]/g, '');
  while (ivHex.length < 32) ivHex = '0' + ivHex;
  ivHex = ivHex.substring(0, 32);

  const offset = parseInt(offsetInput.value) || 0;
  const blockIndex = Math.floor(offset / 16);

  // Convert IV hex string to 16 bytes
  const counter = [];
  for (let i = 0; i < 16; i++) {
    counter.push(parseInt(ivHex.substr(i * 2, 2), 16) || 0);
  }

  // 128-bit big-endian addition of blockIndex into counter[15..0]
  let carry = 0;
  let remainingIndex = BigInt(blockIndex);

  for (let i = 15; i >= 0; i--) {
    const addVal = Number(remainingIndex & 0xffn);
    const sum = counter[i] + addVal + carry;
    counter[i] = sum & 0xff;
    carry = sum >> 8;
    remainingIndex = remainingIndex >> 8n;
  }

  // Format as Hex
  const resultHex = counter.map(b => b.toString(16).padStart(2, '0')).join('');
  resultDisplay.textContent = resultHex;
}

// Window Event Listeners & Hash Routing
window.addEventListener('DOMContentLoaded', () => {
  // Check URL hash for initial tab
  const hash = window.location.hash.replace('#', '');
  if (hash && document.getElementById(`tab-${hash}`)) {
    switchTab(hash);
  } else {
    switchTab('overview');
  }

  // Initialize simulation and calculator
  updateSimulation();
  calculateCounter();
  initLiveControlCenter();

  // Resize listener for chart
  window.addEventListener('resize', () => {
    if (activeTab === 'simulator') {
      renderBenchmarkChart();
    }
  });
});

/* =========================================================================
   LIVE CONTROL CENTER BACKEND INTEGRATION
   ========================================================================= */

let eventSource = null;
let serverAutoScroll = true;
let clientAutoScroll = true;

function initLiveControlCenter() {
  if (window.EventSource) {
    connectEventStream();
  }
  // Initial status check
  refreshSystemStatus();
}

function connectEventStream() {
  if (eventSource) {
    eventSource.close();
  }

  try {
    eventSource = new EventSource('/api/stream');

    eventSource.addEventListener('status_update', (e) => {
      const data = JSON.parse(e.data);
      updateStatusBadges(data);
    });

    eventSource.addEventListener('server_log', (e) => {
      const data = JSON.parse(e.data);
      appendTerminalLine('liveServerTerm', data.line, data.style);
    });

    eventSource.addEventListener('client_log', (e) => {
      const data = JSON.parse(e.data);
      appendTerminalLine('liveClientTerm', data.line, data.style);
    });

    eventSource.onerror = () => {
      // Retry in 5 seconds
      setTimeout(connectEventStream, 5000);
    };
  } catch (err) {
    console.warn('SSE stream not available, falling back to static polling');
  }
}

function updateStatusBadges(data) {
  // Update Mode UI
  const modeVal = data.mode || 'simulation';
  document.querySelectorAll('.mode-btn').forEach(btn => {
    btn.classList.toggle('active', btn.dataset.mode === (data.configured_mode || 'auto'));
  });

  const modeBadge = document.getElementById('statusModeBadge');
  if (modeBadge) {
    if (modeVal === 'hardware') {
      modeBadge.className = 'link-pill online';
      modeBadge.innerHTML = '<span>⚡ Live Hardware (Pi 4B)</span>';
    } else {
      modeBadge.className = 'link-pill offline';
      modeBadge.innerHTML = '<span>🧪 Virtual Simulation Mode</span>';
    }
  }

  // Pi Link Status
  const piPill = document.getElementById('statusPiLink');
  if (piPill) {
    if (data.ssh) {
      piPill.className = 'link-pill online';
      piPill.innerHTML = '<span class="pulse-dot"></span> <span>Pi 4B: Online (10.0.0.1)</span>';
    } else if (data.ping) {
      piPill.className = 'link-pill offline';
      piPill.innerHTML = '<span>⚠️ Pi: Ping OK, SSH connecting...</span>';
    } else {
      piPill.className = 'link-pill offline';
      piPill.innerHTML = '<span>🔌 Pi: Offline (10.0.0.1)</span>';
    }
  }

  // SDP Status
  const sdpPill = document.getElementById('statusSdp');
  if (sdpPill) {
    if (data.sdp_path) {
      sdpPill.className = 'link-pill online';
      sdpPill.innerHTML = '<span>✔ QNX 8.0 SDP Toolchain</span>';
      sdpPill.title = data.sdp_path;
    } else {
      sdpPill.className = 'link-pill offline';
      sdpPill.innerHTML = '<span>⚠️ SDP Toolchain Not Found</span>';
    }
  }

  // Server Process Status
  const serverPill = document.getElementById('statusServerState');
  const btnStartServer = document.getElementById('btnStartServer');
  const btnStopServer = document.getElementById('btnStopServer');

  if (serverPill) {
    if (data.server_running) {
      serverPill.className = 'link-pill active-running';
      serverPill.innerHTML = '<span class="pulse-dot" style="background:#6366f1;"></span> <span>encserver: RUNNING</span>';
      if (btnStartServer) btnStartServer.disabled = true;
      if (btnStopServer) btnStopServer.disabled = false;
    } else {
      serverPill.className = 'link-pill';
      serverPill.innerHTML = '<span>encserver: IDLE</span>';
      if (btnStartServer) btnStartServer.disabled = false;
      if (btnStopServer) btnStopServer.disabled = true;
    }
  }
}

function refreshSystemStatus() {
  fetch('/api/status')
    .then(r => r.json())
    .then(data => updateStatusBadges(data))
    .catch(() => {
      // Local server might not be running backend
    });
}

function setControlMode(mode) {
  fetch('/api/mode', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ mode: mode })
  }).then(r => r.json())
    .then(d => {
      showToast(`Switched mode to: ${d.mode}`);
      refreshSystemStatus();
    });
}

function triggerServerStart() {
  const btn = document.getElementById('btnStartServer');
  if (btn) btn.disabled = true;
  showToast('Starting encserver -v...');

  fetch('/api/server/start', { method: 'POST' })
    .then(r => r.json())
    .then(data => {
      showToast(data.message);
      refreshSystemStatus();
    });
}

function triggerServerStop() {
  const btn = document.getElementById('btnStopServer');
  if (btn) btn.disabled = true;
  showToast('Stopping encserver (slay)...');

  fetch('/api/server/stop', { method: 'POST' })
    .then(r => r.json())
    .then(data => {
      showToast(data.message);
      refreshSystemStatus();
    });
}

function triggerCommand(commandId, customCmd) {
  showToast(`Executing: ${commandId}...`);
  const activeBtn = document.querySelector(`[data-cmd="${commandId}"]`);
  if (activeBtn) {
    activeBtn.classList.add('running');
    activeBtn.innerHTML = '<span>⏳ Running...</span>';
  }

  fetch('/api/run', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ command_id: commandId, custom_cmd: customCmd })
  }).then(r => r.json())
    .then(data => {
      setTimeout(() => {
        if (activeBtn) {
          activeBtn.classList.remove('running');
          activeBtn.innerHTML = '<span>▶ Run Again</span>';
        }
      }, 1500);
    }).catch(err => {
      if (activeBtn) {
        activeBtn.classList.remove('running');
        activeBtn.innerHTML = '<span>▶ Run</span>';
      }
    });
}

function appendTerminalLine(termId, line, styleClass) {
  const term = document.getElementById(termId);
  if (!term) return;

  const span = document.createElement('span');
  if (styleClass) span.className = styleClass;

  // Simple ANSI color parsing
  let cleanLine = line
    .replace(/\x1b\[1;32m/g, '<span class="success">')
    .replace(/\x1b\[1;31m/g, '<span class="error-text">')
    .replace(/\x1b\[36m/g, '<span class="info-text">')
    .replace(/\x1b\[33m/g, '<span class="cmd">')
    .replace(/\x1b\[2m/g, '<span class="dim">')
    .replace(/\x1b\[0m/g, '</span>');

  span.innerHTML = cleanLine;
  term.appendChild(span);

  if ((termId === 'liveServerTerm' && serverAutoScroll) ||
      (termId === 'liveClientTerm' && clientAutoScroll)) {
    term.scrollTop = term.scrollHeight;
  }
}

function clearTerm(termId) {
  const term = document.getElementById(termId);
  if (term) term.innerHTML = '';
}

function toggleAutoScroll(termId, btn) {
  if (termId === 'liveServerTerm') {
    serverAutoScroll = !serverAutoScroll;
    btn.classList.toggle('active', serverAutoScroll);
    btn.textContent = serverAutoScroll ? 'Scroll: ON' : 'Scroll: OFF';
  } else {
    clientAutoScroll = !clientAutoScroll;
    btn.classList.toggle('active', clientAutoScroll);
    btn.textContent = clientAutoScroll ? 'Scroll: ON' : 'Scroll: OFF';
  }
}

