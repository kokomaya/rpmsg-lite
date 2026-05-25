/**
 * RPMsg-Lite Web Simulator - Main Application
 *
 * UI behavior
 * -----------
 * The backend now drives the UI: every batch of events is followed by a
 * single full state snapshot. The frontend does NOT poll get_state or
 * request state per-event. This avoids state-message races (older state
 * overwriting newer) under slow-motion mode.
 *
 * VRing 0 (vqs[0]) is master.rvq / remote.tvq -> direction: remote -> master
 * VRing 1 (vqs[1]) is master.tvq / remote.rvq -> direction: master -> remote
 */

// ============================================================================
// WebSocket
// ============================================================================

let ws = null;
let wsReconnectTimer = null;
let currentState = null;

function wsConnect() {
    const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
    const url = `${protocol}//${window.location.host}/ws`;
    ws = new WebSocket(url);

    ws.onopen = () => {
        updateWsStatus(true);
        wsSend({ cmd: 'get_state' });
    };
    ws.onclose = () => {
        updateWsStatus(false);
        wsReconnectTimer = setTimeout(wsConnect, 2000);
    };
    ws.onerror = (e) => console.error('[WS] error', e);
    ws.onmessage = (e) => {
        try { handleMessage(JSON.parse(e.data)); }
        catch (err) { console.error('[WS] parse error', err, e.data); }
    };
}

function wsSend(obj) {
    if (ws && ws.readyState === WebSocket.OPEN)
        ws.send(JSON.stringify(obj));
}

function updateWsStatus(connected) {
    const el = document.getElementById('ws-status');
    el.textContent = connected ? '● Connected' : '● Disconnected';
    el.className = connected ? 'ws-connected' : 'ws-disconnected';
}

// ============================================================================
// Message dispatch
// ============================================================================

function handleMessage(msg) {
    switch (msg.type) {
        case 'state':
            currentState = msg.state;
            updateUI();
            recordStateSnapshot('state_update', null);
            break;
        case 'event':
            appendEvent(msg.event);
            if (msg.event.type === 'rx_callback') appendRxMessage(msg.event);
            recordStateSnapshot('event', msg.event);
            // backend pushes a fresh state after each event batch; no polling.
            break;
        case 'ok':
        case 'queued':
            // nothing — state will arrive separately
            break;
        case 'error':
            appendLog(`ERROR: ${msg.msg}`, 'error');
            break;
        case 'waiting_step':
            const btn = document.getElementById('btn-step');
            btn.disabled = false;
            btn.classList.add('waiting-step');
            break;
    }
}

// ============================================================================
// Render
// ============================================================================

function updateUI() {
    if (!currentState) return;

    updateCorePanel('master', currentState.master);
    updateCorePanel('remote', currentState.remote);

    // VRing 1 = master.tvq = remote.rvq  -> direction master -> remote
    // VRing 0 = master.rvq = remote.tvq  -> direction remote -> master
    const m2rVq = (currentState.master && currentState.master.tvq) ||
                  (currentState.remote && currentState.remote.rvq);
    const r2mVq = (currentState.remote && currentState.remote.tvq) ||
                  (currentState.master && currentState.master.rvq);

    if (m2rVq) updateVringView('vring-m2r', m2rVq);
    if (r2mVq) updateVringView('vring-r2m', r2mVq);

    if (currentState.buffers) updateBufferView(currentState.buffers);

    const modeEl = document.getElementById('sel-mode');
    if (currentState.mode && modeEl.value !== currentState.mode)
        modeEl.value = currentState.mode;

    const stepBtn = document.getElementById('btn-step');
    if (currentState.step_pending) {
        stepBtn.disabled = false;
        stepBtn.classList.add('waiting-step');
    } else {
        stepBtn.disabled = currentState.mode !== 'step';
        stepBtn.classList.remove('waiting-step');
    }
}

function updateCorePanel(core, state) {
    const stateEl = document.getElementById(`${core}-state`);
    const linkEl = document.getElementById(`${core}-link`);
    const eptsEl = document.getElementById(`${core}-endpoints`);
    const srcEl = document.getElementById(`${core}-send-src`);

    if (state.initialized) {
        stateEl.textContent = 'INITIALIZED';
        stateEl.classList.add('initialized');
    } else {
        stateEl.textContent = 'UNINITIALIZED';
        stateEl.classList.remove('initialized');
    }

    if (state.link_state) {
        linkEl.textContent = 'Link: UP';
        linkEl.classList.add('up');
    } else {
        linkEl.textContent = 'Link: DOWN';
        linkEl.classList.remove('up');
    }

    eptsEl.innerHTML = '';
    const prevSrcValue = srcEl.value;
    srcEl.innerHTML = '';

    if (state.endpoints) {
        state.endpoints.forEach(ept => {
            const div = document.createElement('div');
            div.className = 'endpoint-item';
            div.innerHTML = `
                <span class="addr">EPT:${ept.addr}</span>
                <button onclick="destroyEpt('${core}', ${ept.addr})">✕</button>
            `;
            eptsEl.appendChild(div);

            const opt = document.createElement('option');
            opt.value = ept.addr;
            opt.textContent = `EPT:${ept.addr}`;
            srcEl.appendChild(opt);
        });
    }

    if (prevSrcValue && srcEl.querySelector(`option[value="${prevSrcValue}"]`))
        srcEl.value = prevSrcValue;
}

// Map backend desc.state -> css class
function descStateClass(state) {
    switch (state) {
        case 'avail': return 'state-avail';
        case 'used':  return 'state-used';
        default:      return 'state-free';
    }
}

function updateVringView(prefix, vq) {
    if (!vq) return;

    const descEl  = document.getElementById(`${prefix}-desc`);
    const availEl = document.getElementById(`${prefix}-avail`);
    const usedEl  = document.getElementById(`${prefix}-used`);
    const metaEl  = document.getElementById(`${prefix}-meta`);

    if (metaEl) {
        metaEl.textContent =
            `name=${vq.name}  n=${vq.nentries}  ` +
            `desc_head=${vq.desc_head_idx}  avail_cursor=${vq.avail_idx}  ` +
            `used_cons=${vq.used_cons_idx}  queued=${vq.queued_cnt}`;
    }

    // Descriptors (driven by backend-supplied state per slot)
    descEl.innerHTML = '<span class="vring-label">Desc:</span>';
    if (vq.desc) {
        vq.desc.forEach((d, i) => {
            const cell = document.createElement('div');
            cell.className = 'desc-cell ' + descStateClass(d.state);
            cell.textContent = i;
            cell.title =
                `desc[${i}]\n` +
                `addr=0x${d.addr.toString(16)} len=${d.len} ` +
                `flags=${d.flags} next=${d.next}\n` +
                `state=${d.state}`;
            descEl.appendChild(cell);
        });
    }

    // Available ring — highlight the window between consumer cursor and producer idx
    availEl.innerHTML = '';
    const availLabel = document.createElement('span');
    availLabel.className = 'vring-label';
    availLabel.textContent = `Avail idx=${vq.avail.idx} (cur=${vq.avail_idx})`;
    availEl.appendChild(availLabel);

    if (vq.avail) {
        const n = vq.nentries;
        const fromA = vq.avail_idx & (n - 1);
        const cntA = (vq.avail.idx - vq.avail_idx) & 0xFFFF;
        vq.avail.ring.forEach((val, i) => {
            const cell = document.createElement('div');
            cell.className = 'ring-cell';
            // Highlight slots in the "pending for consumer" window
            const inWindow = isInRingWindow(i, fromA, cntA, n);
            if (inWindow) cell.classList.add('active');
            cell.textContent = val;
            cell.title = `avail.ring[${i}] = desc ${val}`;
            availEl.appendChild(cell);
        });
    }

    // Used ring
    usedEl.innerHTML = '';
    const usedLabel = document.createElement('span');
    usedLabel.className = 'vring-label';
    usedLabel.textContent = `Used idx=${vq.used.idx} (cur=${vq.used_cons_idx})`;
    usedEl.appendChild(usedLabel);

    if (vq.used) {
        const n = vq.nentries;
        const fromU = vq.used_cons_idx & (n - 1);
        const cntU = (vq.used.idx - vq.used_cons_idx) & 0xFFFF;
        vq.used.ring.forEach((item, i) => {
            const cell = document.createElement('div');
            cell.className = 'ring-cell';
            const inWindow = isInRingWindow(i, fromU, cntU, n);
            if (inWindow) cell.classList.add('active');
            cell.textContent = `${item.id}`;
            cell.title = `used.ring[${i}] -> desc ${item.id} len=${item.len}`;
            usedEl.appendChild(cell);
        });
    }
}

function isInRingWindow(pos, from, cnt, n) {
    if (cnt <= 0) return false;
    if (cnt >= n) return true;
    for (let k = 0; k < cnt; k++) {
        if (((from + k) & (n - 1)) === pos) return true;
    }
    return false;
}

// Buffer classification -> css class + label
const BUFFER_STATE_META = {
    m_tx_pool:     { cls: 'buf-pool',     label: 'M-pool' },
    r_tx_pool:     { cls: 'buf-pool',     label: 'R-pool' },
    inflight_m2r:  { cls: 'buf-inflight', label: '→ R' },
    inflight_r2m:  { cls: 'buf-inflight', label: '← M' },
    free:          { cls: 'buf-free',     label: 'free' },
};

function updateBufferView(buffers) {
    const container = document.getElementById('buffer-view');
    container.innerHTML = '';

    buffers.forEach(buf => {
        const meta = BUFFER_STATE_META[buf.state] || BUFFER_STATE_META.free;
        const card = document.createElement('div');
        card.className = `buffer-card ${meta.cls}`;

        const hasPayload = buf.len > 0 && buf.data;
        const payloadText = hasPayload ? hexToAsciiPreview(buf.data) : '';

        card.innerHTML = `
            <div class="buf-header">
                <span>Buf[${buf.idx}] · ${buf.dir}</span>
                <span class="buf-tag">${meta.label}</span>
            </div>
            ${hasPayload ? `
                <div class="buf-hdr-fields">src:${buf.src} → dst:${buf.dst} (${buf.len}B)</div>
                <div class="buf-payload" title="${buf.data}">${payloadText}</div>
            ` : `
                <div class="buf-payload buf-empty">— empty —</div>
            `}
        `;
        container.appendChild(card);
    });
}

function hexToAsciiPreview(hex) {
    let result = '';
    for (let i = 0; i < hex.length; i += 2) {
        const byte = parseInt(hex.substr(i, 2), 16);
        result += (byte >= 32 && byte < 127) ? String.fromCharCode(byte) : '.';
    }
    return result;
}

// ============================================================================
// Event log
// ============================================================================

const MAX_LOG_ENTRIES = 200;
let logCount = 0;

function appendEvent(evt) {
    const log = document.getElementById('event-log');
    const entry = document.createElement('div');
    entry.className = 'event-entry';

    const timeStr = (evt.timestamp_us / 1000).toFixed(1) + 'ms';
    const coreClass = evt.core || '';
    let detail = '';
    if (evt.src || evt.dst) detail += `src:${evt.src} dst:${evt.dst} `;
    if (evt.addr) detail += `addr:${evt.addr} `;
    if (evt.desc_idx || evt.avail_idx || evt.used_idx)
        detail += `desc:${evt.desc_idx} avail:${evt.avail_idx} used:${evt.used_idx} `;
    if (evt.buffer_len) detail += `len:${evt.buffer_len} `;
    if (evt.payload) detail += `[${hexToAsciiPreview(evt.payload)}]`;

    entry.innerHTML = `
        <span class="event-seq">#${evt.seq}</span>
        <span class="event-time">${timeStr}</span>
        <span class="event-type ${coreClass}">${evt.core}:${evt.type}</span>
        <span class="event-detail">${detail}</span>
    `;
    log.appendChild(entry);
    logCount++;
    while (logCount > MAX_LOG_ENTRIES) {
        log.removeChild(log.firstChild);
        logCount--;
    }
    log.scrollTop = log.scrollHeight;
}

function appendLog(text, cls) {
    const log = document.getElementById('event-log');
    const entry = document.createElement('div');
    entry.className = `event-entry ${cls || ''}`;
    entry.innerHTML = `<span class="event-detail">${text}</span>`;
    log.appendChild(entry);
    log.scrollTop = log.scrollHeight;
}

// ============================================================================
// Commands
// ============================================================================

function destroyEpt(core, addr) {
    wsSend({ cmd: 'destroy_ept', core, addr });
}

// UTF-8 -> base64 (handles non-ASCII like Chinese characters correctly)
function strToB64(s) {
    const bytes = new TextEncoder().encode(s);
    let bin = '';
    for (const b of bytes) bin += String.fromCharCode(b);
    return btoa(bin);
}

// ============================================================================
// Wiring
// ============================================================================

document.addEventListener('DOMContentLoaded', () => {
    wsConnect();

    document.getElementById('btn-reset').addEventListener('click', () => {
        wsSend({ cmd: 'reset' });
    });
    document.getElementById('btn-init-master').addEventListener('click', () => {
        wsSend({ cmd: 'init_master' });
    });
    document.getElementById('btn-init-remote').addEventListener('click', () => {
        wsSend({ cmd: 'init_remote' });
    });
    document.getElementById('btn-deinit-master').addEventListener('click', () => {
        wsSend({ cmd: 'deinit', core: 'master' });
    });
    document.getElementById('btn-deinit-remote').addEventListener('click', () => {
        wsSend({ cmd: 'deinit', core: 'remote' });
    });

    document.getElementById('sel-mode').addEventListener('change', (e) => {
        const delay = parseInt(document.getElementById('range-delay').value);
        wsSend({ cmd: 'set_mode', mode: e.target.value, delay_ms: delay });
    });
    document.getElementById('range-delay').addEventListener('input', (e) => {
        document.getElementById('lbl-delay').textContent = e.target.value + 'ms';
    });
    document.getElementById('range-delay').addEventListener('change', (e) => {
        const mode = document.getElementById('sel-mode').value;
        wsSend({ cmd: 'set_mode', mode, delay_ms: parseInt(e.target.value) });
    });

    document.getElementById('btn-step').addEventListener('click', () => {
        wsSend({ cmd: 'step' });
        const btn = document.getElementById('btn-step');
        btn.disabled = true;
        btn.classList.remove('waiting-step');
    });

    document.getElementById('btn-master-create-ept').addEventListener('click', () => {
        const addr = parseInt(document.getElementById('master-ept-addr').value);
        if (addr > 0) wsSend({ cmd: 'create_ept', core: 'master', addr });
    });
    document.getElementById('btn-remote-create-ept').addEventListener('click', () => {
        const addr = parseInt(document.getElementById('remote-ept-addr').value);
        if (addr > 0) wsSend({ cmd: 'create_ept', core: 'remote', addr });
    });

    document.getElementById('btn-master-send').addEventListener('click', () => {
        const src = parseInt(document.getElementById('master-send-src').value);
        const dst = parseInt(document.getElementById('master-send-dst').value);
        const text = document.getElementById('master-send-data').value;
        if (src && dst && text)
            wsSend({ cmd: 'send', core: 'master', src, dst, data: strToB64(text) });
    });
    document.getElementById('btn-remote-send').addEventListener('click', () => {
        const src = parseInt(document.getElementById('remote-send-src').value);
        const dst = parseInt(document.getElementById('remote-send-dst').value);
        const text = document.getElementById('remote-send-data').value;
        if (src && dst && text)
            wsSend({ cmd: 'send', core: 'remote', src, dst, data: strToB64(text) });
    });

    document.getElementById('btn-clear-log').addEventListener('click', () => {
        document.getElementById('event-log').innerHTML = '';
        logCount = 0;
    });

    // Recording controls
    document.getElementById('btn-record').addEventListener('click', startRecording);
    document.getElementById('btn-stop-record').addEventListener('click', stopRecording);
    document.getElementById('btn-replay').addEventListener('click', replayRecording);
    document.getElementById('btn-export-record').addEventListener('click', exportRecording);
    document.getElementById('btn-import-record').addEventListener('click', () => {
        document.getElementById('file-import-record').click();
    });
    document.getElementById('file-import-record').addEventListener('change', (e) => {
        importRecording(e.target.files[0]);
    });
});

// ============================================================================
// Received messages list
// ============================================================================

const MAX_RX_MESSAGES = 50;
let rxCountMaster = 0;
let rxCountRemote = 0;

function appendRxMessage(evt) {
    const core = evt.core;
    const listEl = document.getElementById(`${core}-rx-list`);
    if (!listEl) return;

    const item = document.createElement('div');
    item.className = 'rx-message-item';
    const timeStr = (evt.timestamp_us / 1000).toFixed(1) + 'ms';
    const payloadText = evt.payload ? hexToAsciiPreview(evt.payload) : '';

    item.innerHTML = `
        <div class="rx-meta">${timeStr} | src:${evt.src} → dst:${evt.dst} | ${evt.buffer_len || 0}B</div>
        <div class="rx-payload">${payloadText}</div>
    `;
    listEl.appendChild(item);

    if (core === 'master') {
        rxCountMaster++;
        while (rxCountMaster > MAX_RX_MESSAGES) { listEl.removeChild(listEl.firstChild); rxCountMaster--; }
    } else {
        rxCountRemote++;
        while (rxCountRemote > MAX_RX_MESSAGES) { listEl.removeChild(listEl.firstChild); rxCountRemote--; }
    }
    listEl.scrollTop = listEl.scrollHeight;
}

// ============================================================================
// Recording & replay
// ============================================================================

let isRecording = false;
let recordedActions = [];
let recordedSnapshots = [];
let recordStartTime = 0;

function startRecording() {
    isRecording = true;
    recordedActions = [];
    recordedSnapshots = [];
    recordStartTime = Date.now();
    if (currentState) {
        recordedSnapshots.push({
            timestamp: 0, trigger: 'recording_start',
            state: JSON.parse(JSON.stringify(currentState))
        });
    }
    document.getElementById('btn-record').classList.add('recording');
    document.getElementById('btn-record').disabled = true;
    document.getElementById('btn-stop-record').disabled = false;
    document.getElementById('btn-replay').disabled = true;
    document.getElementById('btn-export-record').disabled = true;
    appendLog('⏺ Recording...', '');
}

function stopRecording() {
    isRecording = false;
    document.getElementById('btn-record').classList.remove('recording');
    document.getElementById('btn-record').disabled = false;
    document.getElementById('btn-stop-record').disabled = true;
    document.getElementById('btn-replay').disabled = recordedActions.length === 0;
    document.getElementById('btn-export-record').disabled = recordedActions.length === 0;
    appendLog(`⏹ Stopped. ${recordedActions.length} actions, ${recordedSnapshots.length} snapshots.`, '');
}

function recordAction(cmd) {
    if (!isRecording) return;
    recordedActions.push({ timestamp: Date.now() - recordStartTime, cmd });
}

function recordStateSnapshot(trigger, event) {
    if (!isRecording) return;
    const snap = {
        timestamp: Date.now() - recordStartTime, trigger,
        state: currentState ? JSON.parse(JSON.stringify(currentState)) : null
    };
    if (event) snap.event = JSON.parse(JSON.stringify(event));
    recordedSnapshots.push(snap);
}

const originalWsSend = wsSend;
wsSend = function(obj) {
    if (obj.cmd !== 'get_state') recordAction(obj);
    originalWsSend(obj);
};

async function replayRecording() {
    if (recordedActions.length === 0) return;
    if (!confirm(`Replay ${recordedActions.length} recorded actions?\nThis will reset the simulation first.`)) return;

    document.getElementById('btn-replay').disabled = true;
    appendLog('▶ Replaying...', '');
    originalWsSend({ cmd: 'reset' });
    await sleep(500);
    for (let i = 0; i < recordedActions.length; i++) {
        const action = recordedActions[i];
        const delay = i > 0 ? action.timestamp - recordedActions[i - 1].timestamp : action.timestamp;
        await sleep(Math.min(delay, 3000));
        appendLog(`▶ [${i+1}/${recordedActions.length}] ${action.cmd.cmd}`, '');
        originalWsSend(action.cmd);
    }
    appendLog('▶ Replay complete.', '');
    document.getElementById('btn-replay').disabled = false;
}

function exportRecording() {
    if (recordedActions.length === 0 && recordedSnapshots.length === 0) return;
    const data = {
        version: 2,
        timestamp: new Date().toISOString(),
        actions: recordedActions,
        snapshots: recordedSnapshots
    };
    const jsonStr = JSON.stringify(data, null, 2);
    fetch('/api/save_recording', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: jsonStr
    }).then(r => r.json()).then(res => {
        if (res.ok) appendLog(`💾 Saved: ${res.filename}`, '');
        else { appendLog(`Save failed: ${res.msg}`, 'error'); downloadRecording(data); }
    }).catch(err => {
        appendLog(`Save failed (${err.message})`, 'error');
        downloadRecording(data);
    });
}

function downloadRecording(data) {
    const blob = new Blob([JSON.stringify(data, null, 2)], { type: 'application/json' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = `rpmsg-sim-recording-${Date.now()}.json`;
    a.click();
    URL.revokeObjectURL(url);
}

function importRecording(file) {
    if (!file) return;
    const reader = new FileReader();
    reader.onload = (e) => {
        try {
            const data = JSON.parse(e.target.result);
            if (!data.actions || !Array.isArray(data.actions)) {
                alert('Invalid recording file format.');
                return;
            }
            recordedActions = data.actions;
            recordedSnapshots = data.snapshots || [];
            isRecording = false;
            document.getElementById('btn-record').classList.remove('recording');
            document.getElementById('btn-record').disabled = false;
            document.getElementById('btn-stop-record').disabled = true;
            document.getElementById('btn-replay').disabled = false;
            document.getElementById('btn-export-record').disabled = false;
            appendLog(`📂 Imported ${recordedActions.length} actions.`, '');
        } catch (err) {
            alert('Failed to parse recording file: ' + err.message);
        }
    };
    reader.readAsText(file);
}

function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }
