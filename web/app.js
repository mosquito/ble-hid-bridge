const { createApp, ref, reactive, computed, provide, inject, onMounted, watch } = Vue;

// SVG Icons
const Icons = {
    bluetooth: '<svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M6.5 6.5l11 11L12 23V1l5.5 5.5-11 11"/></svg>',
    usb: '<svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="4" cy="20" r="2"/><circle cx="12" cy="8" r="2"/><circle cx="20" cy="16" r="2"/><path d="M12 10v8l-6 4M12 14l6 4"/><path d="M12 2v4"/></svg>',
    scan: '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="11" cy="11" r="8"/><path d="M21 21l-4.35-4.35"/></svg>',
    connect: '<svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M5 12h14M12 5l7 7-7 7"/></svg>',
    disconnect: '<svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M18 6L6 18M6 6l12 12"/></svg>',
    trash: '<svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M3 6h18M19 6v14a2 2 0 01-2 2H7a2 2 0 01-2-2V6M8 6V4a2 2 0 012-2h4a2 2 0 012 2v2"/></svg>',
    wifi: '<svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M5 12.55a11 11 0 0114 0M1.42 9a16 16 0 0121.16 0M8.53 16.11a6 6 0 016.95 0M12 20h.01"/></svg>',
    restart: '<svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M23 4v6h-6M1 20v-6h6"/><path d="M3.51 9a9 9 0 0114.85-3.36L23 10M1 14l4.64 4.36A9 9 0 0020.49 15"/></svg>',
    check: '<svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M20 6L9 17l-5-5"/></svg>',
    devices: '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect x="5" y="2" width="14" height="20" rx="2"/><line x1="12" y1="18" x2="12" y2="18"/></svg>',
    settings: '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="3"/><path d="M12 1v2M12 21v2M4.22 4.22l1.42 1.42M18.36 18.36l1.42 1.42M1 12h2M21 12h2M4.22 19.78l1.42-1.42M18.36 5.64l1.42-1.42"/></svg>',
    empty: '<svg width="48" height="48" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5"><rect x="3" y="3" width="18" height="18" rx="2"/><path d="M9 9l6 6M15 9l-6 6"/></svg>',
    logo: '<svg width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M6.5 6.5L17.5 17.5M6.5 17.5L17.5 6.5"/><circle cx="12" cy="12" r="9"/></svg>'
};

// API
const API = {
    async request(method, path, data = null) {
        const opts = { method };
        if (data) {
            opts.headers = { 'Content-Type': 'application/json' };
            opts.body = JSON.stringify(data);
        }
        const res = await fetch(`/api${path}`, opts);
        return res.json();
    },
    getStatus: () => API.request('GET', '/status'),
    scan: () => API.request('POST', '/scan'),
    connect: (address) => API.request('POST', '/connect', { address }),
    disconnect: (address) => address ? API.request('POST', '/disconnect', { address }) : API.request('POST', '/disconnect'),
    disconnectAll: () => API.request('POST', '/disconnect'),
    getSaved: () => API.request('GET', '/saved'),
    remove: (address) => API.request('POST', '/remove', { address }),
    restart: () => API.request('POST', '/restart'),
    factoryReset: () => API.request('POST', '/factory-reset', { confirm: 'RESET' }),
    getWiFi: () => API.request('GET', '/wifi'),
    setWiFi: (settings) => API.request('POST', '/wifi', settings)
};

const app = createApp({
    setup() {
        // Shared state
        const state = reactive({
            connections: [],
            foundDevices: [],
            savedDevices: [],
            scanning: false,
            stats: {},
            bondedDevices: [],
            statsHistory: [],
            statsVersion: 0,
            restartRequired: false,
            currentPage: 'devices',
            toasts: [],
            wifiName: '',
            wifiPassword: '',
            wifiDefaultName: 'BLE-USB-Bridge'
        });

        // Computed
        const isConnected = computed(() => state.connections.length > 0);

        const availableDevices = computed(() => {
            const connAddrs = new Set(state.connections.map(c => c.address));
            return state.foundDevices.filter(d => !connAddrs.has(d.address));
        });

        const savedWithStatus = computed(() => {
            const connAddrs = new Set(state.connections.map(c => c.address));
            const savedAddrs = new Set(state.savedDevices.map(d => d.address));
            // Merge saved + bonded-only into one list
            const merged = state.savedDevices.map(d => ({
                ...d,
                connected: connAddrs.has(d.address)
            }));
            for (const b of state.bondedDevices) {
                if (!savedAddrs.has(b.address)) {
                    merged.push({ ...b, name: b.name || '', connected: connAddrs.has(b.address) });
                }
            }
            return merged;
        });

        // Stats history tracking
        let _prevStats = null;

        // Toast
        function toast(message, type = 'info') {
            const t = { message, type };
            state.toasts.push(t);
            setTimeout(() => {
                const idx = state.toasts.indexOf(t);
                if (idx !== -1) state.toasts.splice(idx, 1);
            }, 3000);
        }

        // Actions
        async function refreshStatus() {
            try {
                const data = await API.getStatus();
                state.connections = data.connections || [];
                state.foundDevices = data.foundDevices || [];
                state.savedDevices = data.savedDevices || [];
                state.scanning = data.scanning || false;
                state.bondedDevices = data.bondedDevices || [];
                state.stats = data.stats || {};
                if (_prevStats && Object.keys(state.stats).length > 0) {
                    const d = {};
                    for (const k of ['notifyIn', 'notifyDrop', 'usbOk', 'usbFail']) {
                        d[k] = Math.max(0, (state.stats[k] || 0) - (_prevStats[k] || 0));
                    }
                    state.statsHistory.push(d);
                    if (state.statsHistory.length > 60) state.statsHistory.shift();
                    state.statsVersion++;

                    // Frame timing: compute average from cumulative deltas
                    const st = state.stats;
                    const ps = _prevStats;
                    const btDc = (st.btFrameCount || 0) - (ps.btFrameCount || 0);
                    if (btDc > 0) {
                        const btDs = (st.btFrameSumUs || 0) - (ps.btFrameSumUs || 0);
                        st._btFrameMs = btDs / btDc / 1000;
                    }
                    const usbDc = (st.usbFrameCount || 0) - (ps.usbFrameCount || 0);
                    if (usbDc > 0) {
                        const usbDs = (st.usbFrameSumUs || 0) - (ps.usbFrameSumUs || 0);
                        st._usbFrameMs = usbDs / usbDc / 1000;
                    }
                    const minUs = st.usbFrameMinUs;
                    const maxUs = st.usbFrameMaxUs;
                    if (minUs != null && maxUs != null && maxUs > 0) {
                        st._usbFrameMinMs = minUs / 1000;
                        st._usbFrameMaxMs = maxUs / 1000;
                    }
                }
                if (Object.keys(state.stats).length > 0) _prevStats = { ...state.stats };
                state.restartRequired = data.restartRequired || false;
            } catch (e) {
                console.error('Failed to refresh status:', e);
            }
        }

        async function loadWiFi() {
            try {
                const data = await API.getWiFi();
                state.wifiName = data.name || '';
                state.wifiDefaultName = data.defaultName || 'BLE-USB-Bridge';
            } catch (e) {
                console.error('Failed to load WiFi:', e);
            }
        }

        async function connectDevice(address) {
            toast('Connecting...');
            try {
                await API.connect(address);
                // Poll for connection
                let attempts = 0;
                const check = async () => {
                    attempts++;
                    await refreshStatus();
                    const isConn = state.connections.some(c => c.address === address);
                    if (isConn) {
                        toast('Connected!', 'success');
                    } else if (attempts < 20) {
                        setTimeout(check, 500);
                    } else {
                        toast('Connection timeout', 'error');
                    }
                };
                setTimeout(check, 500);
            } catch (e) {
                toast('Connection failed', 'error');
            }
        }

        async function disconnectDevice(address) {
            try {
                await API.disconnect(address);
                toast('Disconnected', 'success');
                await refreshStatus();
            } catch (e) {
                toast('Failed to disconnect', 'error');
            }
        }

        async function disconnectAll() {
            try {
                await API.disconnectAll();
                toast('All devices disconnected', 'success');
                await refreshStatus();
            } catch (e) {
                toast('Failed to disconnect', 'error');
            }
        }

        async function removeDevice(address) {
            if (!confirm('Remove this device?')) return;
            try {
                await API.remove(address);
                toast('Device removed', 'success');
                await refreshStatus();
            } catch (e) {
                toast('Failed to remove', 'error');
            }
        }

        async function saveWiFi() {
            if (!state.wifiName) {
                toast('AP name is required', 'error');
                return;
            }
            if (state.wifiPassword && state.wifiPassword.length < 8) {
                toast('Password must be at least 8 characters', 'error');
                return;
            }

            const settings = { name: state.wifiName };
            if (state.wifiPassword) settings.password = state.wifiPassword;

            try {
                const data = await API.setWiFi(settings);
                if (data.success) {
                    state.restartRequired = true;
                    state.wifiPassword = '';
                    toast('Settings saved! Restart to apply.', 'success');
                } else {
                    toast(data.error || 'Failed to save', 'error');
                }
            } catch (e) {
                toast('Failed to save settings', 'error');
            }
        }

        async function restartDevice() {
            if (!confirm('Restart the device now?')) return;
            try {
                await API.restart();
                toast('Device restarting...', 'warning');
            } catch (e) {}
        }

        async function factoryReset() {
            if (!confirm('This will erase ALL settings, saved devices, and WiFi configuration.\n\nAre you sure?')) return;
            if (!confirm('FINAL WARNING: This cannot be undone!\n\nProceed with factory reset?')) return;
            try {
                await API.factoryReset();
                toast('Factory reset... Device will reboot.', 'warning');
            } catch (e) {}
        }

        async function startScan() {
            try {
                await API.scan();
                toast('Scanning...', 'info');
                setTimeout(refreshStatus, 1000);
            } catch (e) {
                toast('Failed to start scan', 'error');
            }
        }

        function navigate(page) {
            state.currentPage = page;
        }

        // Provide for child components
        provide('app', {
            state,
            isConnected,
            availableDevices,
            savedWithStatus,
            Icons,
            API,
            toast,
            refreshStatus,
            loadWiFi,
            connectDevice,
            disconnectDevice,
            disconnectAll,
            removeDevice,
            saveWiFi,
            restartDevice,
            factoryReset,
            startScan,
            navigate
        });

        // Init
        onMounted(() => {
            refreshStatus();
            loadWiFi();
            setInterval(refreshStatus, 3000);
        });

        // Return for root template (toasts, navigation)
        return { state, Icons };
    }
});

app.mount('#app');
