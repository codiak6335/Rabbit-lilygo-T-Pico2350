(function () {
    'use strict';
    const $ = (id) => document.getElementById(id);
    const screen = $('audioScreen'), panel = $('audioControls');
    if (!screen || !panel) return;
    const mode = $('audioMode'), volume = $('audioVolume'), level = $('audioLevel');
    const button = (action) => panel.querySelector('[data-audio="' + action + '"]');
    const visible = () => screen.classList.contains('is-active');
    let latest = null, busy = false, polling = false, revision = 0;
    let settingsPending = false, settingsSending = false, settingsVersion = 0, settingsTimer = null;
    let nearby = false, scanned = false, scanning = false, full = false, devices = [], deviceSignature = '';
    let waitingAddress = '', connectStarted = 0, testFinished = false, testAddress = '';
    let savedDevices = [], savedSignature = '';
    function error(text) {
        $('audioError').textContent = text;
        $('audioError').classList.toggle('is-hidden', !text);
    }
    async function call(action, body, offset) {
        const url = new URL('/api/audio/' + action, location.origin);
        const token = new URLSearchParams(location.search).get('token') || localStorage.getItem('rabbitToken');
        if (token) url.searchParams.set('token', token);
        if (offset !== undefined) url.searchParams.set('offset', offset);
        const controller = new AbortController(), timeout = setTimeout(() => controller.abort(), 8000);
        const options = { cache: 'no-store', method: ['status', 'devices', 'saved'].includes(action) ? 'GET' : 'POST', signal: controller.signal };
        if (body) { options.headers = { 'Content-Type': 'application/json' }; options.body = JSON.stringify(body); }
        try {
            const response = await fetch(url, options), data = await response.json();
            if (!response.ok) throw new Error(data.error || 'Audio request failed');
            return data;
        } catch (problem) {
            if (problem.name === 'AbortError') throw new Error('Controller did not respond. Check its connection and retry.');
            throw problem;
        } finally { clearTimeout(timeout); }
    }
    function settings() { return { mode: mode.value, volume: Number(volume.value) }; }
    function renderDevices() {
        const signature = JSON.stringify(devices);
        if (signature === deviceSignature) return;
        const focused = document.activeElement?.dataset.address;
        deviceSignature = signature;
        const fragment = document.createDocumentFragment();
        devices.forEach((device) => {
            const row = document.createElement('button'); row.type = 'button'; row.className = 'tool-row audio-speaker-row';
            row.dataset.audio = 'connect'; row.dataset.address = device.address;
            const text = document.createElement('span'), title = document.createElement('strong'), detail = document.createElement('small'), action = document.createElement('b');
            title.textContent = device.name || 'Unnamed audio device'; detail.textContent = device.address;
            action.textContent = 'Connect'; text.append(title, detail); row.append(text, action); fragment.append(row);
        });
        $('audioDeviceList').replaceChildren(fragment);
        if (focused) Array.from($('audioDeviceList').querySelectorAll('button')).find((item) => item.dataset.address === focused)?.focus();
    }
    function renderSavedDevices() {
        const list = savedDevices.slice();
        if ((latest?.paired || waitingAddress) && !list.some((item) => item.address === latest.address)) {
            list.push({ address: latest.address, name: latest.name, soundConfirmed: latest.soundConfirmed });
        }
        $('audioNoSpeaker').classList.toggle('is-hidden', list.length > 0);
        const signature = JSON.stringify(list) + (latest?.address || '') + (latest?.connection || '');
        if (signature === savedSignature) return;
        const focused = document.activeElement?.dataset.address, focusedAction = document.activeElement?.dataset.audio;
        savedSignature = signature;
        const fragment = document.createDocumentFragment();
        list.forEach((device) => {
            const card = document.createElement('div'); card.className = 'audio-saved-device'; card.dataset.savedAddress = device.address;
            const row = document.createElement('button'); row.type = 'button'; row.className = 'tool-row audio-speaker-row';
            row.dataset.audio = 'reconnect'; row.dataset.address = device.address;
            const text = document.createElement('span'), title = document.createElement('strong'), detail = document.createElement('small'), action = document.createElement('b');
            title.textContent = device.name || 'Paired speaker'; detail.textContent = 'Not connected'; action.textContent = 'Connect';
            if (!device.name || list.filter((item) => item.name === device.name).length > 1) {
                title.textContent += ' · ' + device.address;
            }
            text.append(title, detail); row.append(text, action); card.append(row);
            const actions = document.createElement('div'); actions.className = 'audio-actions';
            if (device.address === latest?.address && ['streaming', 'connecting', 'connected'].includes(latest.connection)) {
                const disconnect = document.createElement('button'); disconnect.type = 'button'; disconnect.className = 'button button-quiet';
                disconnect.dataset.audio = 'disconnect'; disconnect.textContent = 'Disconnect'; actions.append(disconnect);
            }
            if (savedDevices.some((item) => item.address === device.address)) {
                const forget = document.createElement('button'); forget.type = 'button'; forget.className = 'button button-quiet';
                forget.dataset.audio = 'forget'; forget.dataset.address = device.address; forget.textContent = 'Forget'; actions.append(forget);
            }
            card.append(actions); fragment.append(card);
        });
        $('audioSavedList').replaceChildren(fragment);
        if (focused) Array.from($('audioSavedList').querySelectorAll('button')).find((item) => item.dataset.address === focused && item.dataset.audio === focusedAction)?.focus();
    }
    async function readSaved() {
        const list = []; let offset = 0;
        for (let i = 0; i < 2; ++i) {
            const page = await call('saved', undefined, offset); list.push(...page.devices);
            if (page.next == null) break;
            if (page.next <= offset) throw new Error('Invalid saved-device list.');
            offset = page.next;
        }
        return list;
    }
    function render(data) {
        const old = latest; latest = data;
        if (!settingsPending) { mode.value = data.mode; volume.value = data.volume; level.value = data.volume + '%'; }
        if (waitingAddress === data.address && data.connection === 'streaming') waitingAddress = '';
        if (old?.testActive && !data.testActive && testAddress === data.address && data.connection === 'streaming') testFinished = true;
        if (data.connection !== 'streaming' || data.address !== testAddress) testFinished = false;
        const states = { disabled: 'Bluetooth off', idle: 'Not connected', scanning: 'Searching', connecting: 'Connecting…',
            connected: 'Starting audio…', streaming: 'Connected', error: 'Could not connect' };
        $('audioMessage').textContent = (settingsPending || settingsSending ? 'Applying…' : data.supported ? states[data.connection] : 'Bluetooth unavailable') +
            (data.workoutRunning ? ' · stop the workout to pair or test' : '') +
            (!data.buzzerAvailable && ['buzzer', 'both'].includes(data.mode) ? ' · buzzer unavailable' : '');
        renderSavedDevices();
        $('audioTestState').textContent = data.testActive ? 'Listen for 3 beeps, one second apart.' :
            testFinished ? 'Did you hear all 3 beeps from this speaker?' :
            data.soundConfirmed && data.connection === 'streaming' ? 'Sound confirmed.' : '';
        renderControls();
    }
    function renderControls() {
        const data = latest || {}, locked = busy || settingsSending || settingsPending || !latest, idle = !data.workoutRunning;
        panel.querySelectorAll('button').forEach((item) => { item.disabled = locked; });
        // Keep the slider responsive while its previous value is being sent.
        mode.disabled = volume.disabled = !latest || busy;
        button('add').disabled = locked || !idle || !data.supported || nearby || !!waitingAddress || data.testActive;
        $('audioSavedList').querySelectorAll('[data-audio="reconnect"]').forEach((row) => {
            const current = row.dataset.address === data.address;
            const connected = current && data.connection === 'streaming';
            row.disabled = locked || !idle || !!waitingAddress || data.testActive || connected;
            row.querySelector('b').textContent = connected ? 'Connected' : current && data.connection === 'connecting' ? 'Connecting…' : 'Connect';
            const device = savedDevices.find((item) => item.address === row.dataset.address);
            row.querySelector('small').textContent = (current ? { streaming: 'Connected', connecting: 'Connecting…', connected: 'Starting audio…', error: 'Could not connect' }[data.connection] : '') || 'Not connected';
            if (device?.soundConfirmed) row.querySelector('small').textContent += ' · Sound confirmed';
        });
        $('audioSavedList').querySelectorAll('[data-audio="forget"]').forEach((row) => {
            row.disabled = locked || !idle || !!waitingAddress || data.testActive;
        });
        button('test').disabled = locked || !idle || scanning || !!waitingAddress || data.testActive || data.volume === 0 || data.mode === 'off' ||
            !(data.connection === 'streaming' || (data.buzzerAvailable && ['buzzer', 'both'].includes(data.mode)));
        const canConfirm = testFinished && data.connection === 'streaming' && !data.testActive;
        button('confirm').classList.toggle('is-hidden', !canConfirm);
        button('not-heard').classList.toggle('is-hidden', !canConfirm);
        button('confirm').disabled = locked || !idle || !canConfirm;
        button('not-heard').disabled = locked || !idle || !canConfirm;
        $('audioNearby').classList.toggle('is-hidden', !nearby);
        button('scan').textContent = scanning ? 'Scanning…' : scanned ? 'Scan again' : 'Scan';
        button('scan').disabled = locked || !idle || scanning || !!waitingAddress || data.testActive;
        button('scan-stop').classList.toggle('is-hidden', !scanning);
        $('audioScanState').textContent = scanning ? 'Searching for nearby speakers… Tap a device to connect.' :
            !scanned ? 'Scan when your speaker is ready.' : devices.length ? 'Tap a device to connect.' : 'No devices found. Check pairing mode and scan again.';
        if (full) $('audioScanState').textContent += ' Showing the first 16 devices. Move closer and scan again if yours is missing.';
        $('audioDeviceList').querySelectorAll('button').forEach((row) => {
            const connected = row.dataset.address === data.address && data.connection === 'streaming';
            row.disabled = locked || !idle || !!waitingAddress || data.testActive || connected;
            row.querySelector('b').textContent = connected ? 'Connected' : row.dataset.address === waitingAddress ? 'Connecting…' : 'Connect';
        });
    }
    function queueSettings(immediate) {
        settingsPending = true; ++settingsVersion; ++revision; error('');
        level.value = volume.value + '%'; clearTimeout(settingsTimer); renderControls();
        settingsTimer = setTimeout(flushSettings, immediate ? 0 : 200);
    }
    async function flushSettings() {
        if (!settingsPending || settingsSending || busy || !latest) return;
        settingsSending = true; const version = settingsVersion, value = settings(); renderControls();
        try {
            const data = await call('config', value);
            if (version === settingsVersion) settingsPending = false;
            render(data);
        } catch (problem) {
            if (version === settingsVersion) settingsPending = false;
            render(latest); error('Could not apply audio setting. ' + problem.message);
        } finally {
            settingsSending = false; renderControls();
            if (settingsPending) settingsTimer = setTimeout(flushSettings, 0);
            else if (latest) render(latest);
        }
    }
    async function readDevices() {
        const list = []; let offset = 0, page;
        for (let i = 0; i < 4; ++i) {
            page = await call('devices', undefined, offset); list.push(...page.devices);
            if (page.next == null) break;
            if (page.next <= offset) throw new Error('Invalid speaker list. Scan again.');
            offset = page.next;
        }
        return { devices: list, scanning: page.scanning, full: page.full };
    }
    async function refresh() {
        if (busy || settingsSending || settingsPending || polling || !visible()) return;
        polling = true; const version = revision;
        try {
            const data = await call('status'), remembered = await readSaved(), found = nearby && scanned ? await readDevices() : null;
            if (version !== revision || busy || settingsPending || !visible()) return;
            savedDevices = remembered;
            if (found) { devices = found.devices; scanning = found.scanning; full = found.full; renderDevices(); }
            render(data);
            if (waitingAddress && (Date.now() - connectStarted > 35000 || data.connection === 'error')) {
                waitingAddress = ''; render(await call('disconnect'));
                error('Could not connect. Put the speaker in pairing mode and tap its name to retry. Your previous speaker is still saved.');
            }
        } catch (problem) { error(problem.message); }
        finally { polling = false; }
    }
    async function closeSetup() {
        if (nearby) await call('scan-stop');
        if (waitingAddress) { waitingAddress = ''; render(await call('disconnect')); }
        nearby = false; scanning = false; renderControls();
    }
    volume.addEventListener('input', () => queueSettings(false));
    volume.addEventListener('change', () => queueSettings(true));
    mode.addEventListener('change', () => queueSettings(true));
    panel.addEventListener('click', async (event) => {
        const item = event.target.closest('[data-audio]');
        if (!item || item.disabled || busy || settingsSending || settingsPending || !latest) return;
        const action = item.dataset.audio; busy = true; ++revision; error(''); renderControls();
        try {
            if (action === 'add') { nearby = true; scanned = false; scanning = false; devices = []; deviceSignature = ''; renderDevices(); $('audioNearbyTitle').focus(); }
            else if (action === 'scan') {
                render(await call('scan')); scanned = true; scanning = true; devices = []; full = false; deviceSignature = ''; renderDevices();
            } else if (action === 'scan-stop') { render(await call(action)); scanning = false; }
            else if (action === 'connect' || action === 'reconnect') {
                const address = item.dataset.address;
                const value = settings(); if (!['bluetooth', 'both'].includes(value.mode)) value.mode = 'bluetooth';
                const data = await call('connect', Object.assign({ address }, value));
                waitingAddress = address; connectStarted = Date.now(); scanning = false; testFinished = false; render(data);
            } else if (action === 'test') { testFinished = false; testAddress = latest.address; render(await call('test')); }
            else if (action === 'confirm') { const data = await call(action); testFinished = false; render(data); }
            else if (action === 'not-heard') { testFinished = false; render(latest); error('Raise the volume and tap Test 3 beeps again. Check that this speaker is powered on.'); }
            else if (action === 'close') await closeSetup();
            else {
                if (action === 'disconnect') waitingAddress = '';
                render(await call(action, action === 'forget' ? { address: item.dataset.address } : undefined));
            }
            if (['connect', 'reconnect', 'forget', 'confirm', 'disconnect'].includes(action)) {
                savedDevices = await readSaved(); render(latest);
            }
        } catch (problem) { error(problem.message); }
        finally {
            busy = false; renderControls();
            if (settingsPending) flushSettings();
            if (!visible() && (nearby || waitingAddress)) { try { await closeSetup(); } catch (problem) { error(problem.message); } }
        }
    });
    new MutationObserver(async () => {
        if (visible()) refresh();
        else if ((nearby || waitingAddress) && !busy) {
            busy = true; ++revision;
            try { await closeSetup(); } catch (problem) { error(problem.message); }
            finally { busy = false; renderControls(); }
        }
    }).observe(screen, { attributes: true, attributeFilter: ['class'] });
    setInterval(refresh, 1500);
    renderControls();
})();
