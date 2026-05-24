/**
 * RPMsg-Lite Web Simulator - Main Application
 * Handles WebSocket communication and UI updates.
 */

// ============================================================================
// WebSocket Connection
// ============================================================================

let ws = null;
let wsReconnectTimer = null;
let currentState = null;

function wsConnect() {
    const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
    const url = `${protocol}//${window.location.host}/ws`;

    ws = new WebSocket(url);

    ws.onopen = () => {
        console.log('[WS] Connected');
        updateWsStatus(true);
        // Request initial state
        wsSend({ cmd: 'get_state' });
    };

    ws.onclose = () => {
        console.log('[WS] Disconnected');
        updateWsStatus(false);
        // Auto-reconnect
        wsReconnectTimer = setTimeout(wsConnect, 2000);
    };

    ws.onerror = (err) => {
        console.error('[WS] Error:', err);
    };

    ws.onmessage = (event) => {
        try {
            const msg = JSON.parse(event.data);
            handleMessage(msg);
        } catch (e) {
            console.error('[WS] Parse error:', e, event.data);
        }
    };
}

function wsSend(obj) {
    if (ws && ws.readyState === WebSocket.OPEN) {
        ws.send(JSON.stringify(obj));
    }
}

function updateWsStatus(connected) {
    const el = document.getElementById('ws-status');
    if (connected) {
        el.textContent = '● Connected';
        el.className = 'ws-connected';
    } else {
        el.textContent = '● Disconnected';
        el.className = 'ws-disconnected';
    }
}

// ============================================================================
// Message Handler
// ============================================================================

function handleMessage(msg) {
    switch (msg.type) {
        case 'state':
            currentState = msg.state;
            updateUI();
            break;
        case 'event':
            appendEvent(msg.event);
            // Request fresh state after each event
            wsSend({ cmd: 'get_state' });
            break;
        case 'ok':
            // Command succeeded, request state
            wsSend({ cmd: 'get_state' });
            break;
        case 'error':
            appendLog(`ERROR: ${msg.msg}`, 'error');
            break;
        case 'waiting_step':
            document.getElementById('btn-step').disabled = false;
            document.getElementById('btn-step').classList.add('waiting-step');
            break;
    }
}

// ============================================================================
// UI Update
// ============================================================================

function updateUI() {
    if (!currentState) return;

    // Update Master panel
    updateCorePanel('master', currentState.master);

    // Update Remote panel
    updateCorePanel('remote', currentState.remote);

    // Update VRing views
    if (currentState.master && currentState.master.tvq) {
        updateVringView('vring0', currentState.master.tvq);
    }
    if (currentState.master && currentState.master.rvq) {
        updateVringView('vring1', currentState.master.rvq);
    }

    // Update buffer view
    if (currentState.buffers) {
        updateBufferView(currentState.buffers);
    }

    // Update mode controls
    const modeEl = document.getElementById('sel-mode');
    if (currentState.mode && modeEl.value !== currentState.mode) {
        modeEl.value = currentState.mode;
    }

    // Step button state
    if (currentState.step_pending) {
        document.getElementById('btn-step').disabled = false;
        document.getElementById('btn-step').classList.add('waiting-step');
    } else {
        document.getElementById('btn-step').classList.remove('waiting-step');
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

    // Endpoints
    eptsEl.innerHTML = '';
    srcEl.innerHTML = '';
    if (state.endpoints) {
        state.endpoints.forEach(ept => {
            // Endpoint item
            const div = document.createElement('div');
            div.className = 'endpoint-item';
            div.innerHTML = `
                <span class="addr">EPT:${ept.addr}</span>
                <button onclick="destroyEpt('${core}', ${ept.addr})">✕</button>
            `;
            eptsEl.appendChild(div);

            // Send source option
            const opt = document.createElement('option');
            opt.value = ept.addr;
            opt.textContent = `EPT:${ept.addr}`;
            srcEl.appendChild(opt);
        });
    }
}

function updateVringView(prefix, vq) {
    if (!vq) return;

    const descEl = document.getElementById(`${prefix}-desc`);
    const availEl = document.getElementById(`${prefix}-avail`);
    const usedEl = document.getElementById(`${prefix}-used`);

    // Descriptors
    descEl.innerHTML = '<span class="vring-label">Desc:</span>';
    if (vq.desc) {
        vq.desc.forEach((d, i) => {
            const cell = document.createElement('div');
            cell.className = 'desc-cell';
            // Determine if this descriptor is free or in-use
            const isFree = isDescFree(vq, i);
            cell.classList.add(isFree ? 'free' : 'in-use');
            cell.textContent = i;
            cell.title = `addr:0x${d.addr.toString(16)} len:${d.len} flags:${d.flags} next:${d.next}`;
            descEl.appendChild(cell);
        });
    }

    // Available ring
    availEl.innerHTML = '<span class="vring-label">Avail:</span>';
    if (vq.avail) {
        const idxSpan = document.createElement('span');
        idxSpan.className = 'vring-label';
        idxSpan.textContent = `idx=${vq.avail.idx}`;
        availEl.appendChild(idxSpan);
        vq.avail.ring.forEach((val, i) => {
            const cell = document.createElement('div');
            cell.className = 'ring-cell';
            if (i < vq.avail.idx % vq.nentries || (vq.avail.idx > vq.nentries && i < vq.nentries)) {
                cell.classList.add('active');
            }
            cell.textContent = val;
            availEl.appendChild(cell);
        });
    }

    // Used ring
    usedEl.innerHTML = '<span class="vring-label">Used:</span>';
    if (vq.used) {
        const idxSpan = document.createElement('span');
        idxSpan.className = 'vring-label';
        idxSpan.textContent = `idx=${vq.used.idx}`;
        usedEl.appendChild(idxSpan);
        vq.used.ring.forEach((item, i) => {
            const cell = document.createElement('div');
            cell.className = 'ring-cell';
            if (i < vq.used.idx % vq.nentries) {
                cell.classList.add('active');
            }
            cell.textContent = `${item.id}`;
            cell.title = `id:${item.id} len:${item.len}`;
            usedEl.appendChild(cell);
        });
    }
}

function isDescFree(vq, idx) {
    // Walk the free chain from desc_head_idx
    let head = vq.desc_head_idx;
    let visited = new Set();
    while (head < vq.nentries && !visited.has(head)) {
        if (head === idx) return true;
        visited.add(head);
        if (vq.desc[head]) {
            head = vq.desc[head].next;
        } else {
            break;
        }
    }
    return false;
}

function updateBufferView(buffers) {
    const container = document.getElementById('buffer-view');
    container.innerHTML = '';

    buffers.forEach(buf => {
        const card = document.createElement('div');
        card.className = 'buffer-card';
        if (buf.len > 0) card.classList.add('has-data');

        const payloadText = buf.data ? hexToAsciiPreview(buf.data) : '';

        card.innerHTML = `
            <div class="buf-header">
                <span>Buffer[${buf.idx}]</span>
                <span>${buf.len > 0 ? buf.len + 'B' : 'empty'}</span>
            </div>
            ${buf.len > 0 ? `
            <div class="buf-hdr-fields">
                src:${buf.src} → dst:${buf.dst} flags:${buf.flags}
            </div>
            <div class="buf-payload" title="${buf.data}">${payloadText}</div>
            ` : ''}
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
// Event Log
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

    // Trim old entries
    while (logCount > MAX_LOG_ENTRIES) {
        log.removeChild(log.firstChild);
        logCount--;
    }

    // Auto-scroll
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
// Command Functions
// ============================================================================

function destroyEpt(core, addr) {
    wsSend({ cmd: 'destroy_ept', core: core, addr: addr });
}

// ============================================================================
// Event Bindings
// ============================================================================

document.addEventListener('DOMContentLoaded', () => {
    // Connect WebSocket
    wsConnect();

    // Reset
    document.getElementById('btn-reset').addEventListener('click', () => {
        wsSend({ cmd: 'reset' });
    });

    // Init Master
    document.getElementById('btn-init-master').addEventListener('click', () => {
        wsSend({ cmd: 'init_master' });
    });

    // Init Remote
    document.getElementById('btn-init-remote').addEventListener('click', () => {
        wsSend({ cmd: 'init_remote' });
    });

    // Mode change
    document.getElementById('sel-mode').addEventListener('change', (e) => {
        const delay = parseInt(document.getElementById('range-delay').value);
        wsSend({ cmd: 'set_mode', mode: e.target.value, delay_ms: delay });
    });

    // Delay slider
    document.getElementById('range-delay').addEventListener('input', (e) => {
        document.getElementById('lbl-delay').textContent = e.target.value + 'ms';
    });
    document.getElementById('range-delay').addEventListener('change', (e) => {
        const mode = document.getElementById('sel-mode').value;
        wsSend({ cmd: 'set_mode', mode: mode, delay_ms: parseInt(e.target.value) });
    });

    // Step button
    document.getElementById('btn-step').addEventListener('click', () => {
        wsSend({ cmd: 'step' });
        document.getElementById('btn-step').disabled = true;
        document.getElementById('btn-step').classList.remove('waiting-step');
    });

    // Master create endpoint
    document.getElementById('btn-master-create-ept').addEventListener('click', () => {
        const addr = parseInt(document.getElementById('master-ept-addr').value);
        if (addr > 0) {
            wsSend({ cmd: 'create_ept', core: 'master', addr: addr });
        }
    });

    // Remote create endpoint
    document.getElementById('btn-remote-create-ept').addEventListener('click', () => {
        const addr = parseInt(document.getElementById('remote-ept-addr').value);
        if (addr > 0) {
            wsSend({ cmd: 'create_ept', core: 'remote', addr: addr });
        }
    });

    // Master send
    document.getElementById('btn-master-send').addEventListener('click', () => {
        const src = parseInt(document.getElementById('master-send-src').value);
        const dst = parseInt(document.getElementById('master-send-dst').value);
        const text = document.getElementById('master-send-data').value;
        if (src && dst && text) {
            const data = btoa(text); // Base64 encode
            wsSend({ cmd: 'send', core: 'master', src: src, dst: dst, data: data });
        }
    });

    // Remote send
    document.getElementById('btn-remote-send').addEventListener('click', () => {
        const src = parseInt(document.getElementById('remote-send-src').value);
        const dst = parseInt(document.getElementById('remote-send-dst').value);
        const text = document.getElementById('remote-send-data').value;
        if (src && dst && text) {
            const data = btoa(text);
            wsSend({ cmd: 'send', core: 'remote', src: src, dst: dst, data: data });
        }
    });

    // Clear log
    document.getElementById('btn-clear-log').addEventListener('click', () => {
        document.getElementById('event-log').innerHTML = '';
        logCount = 0;
    });

    // Periodic state refresh
    setInterval(() => {
        if (ws && ws.readyState === WebSocket.OPEN) {
            wsSend({ cmd: 'get_state' });
        }
    }, 2000);
});
