(function () {
    'use strict';

    const state = {
        screen: 'home',
        history: [],
        sets: {},
        pools: [],
        defaultPool: '',
        selectedName: '',
        selectedSet: null,
        server: null,
        deckPhase: 'idle',
        jsonPath: '',
        poll: null,
        statusBusy: false,
        busy: false,
        workoutModel: null,
        workoutPlan: null,
    };

    const $ = (id) => document.getElementById(id);
    const screens = {
        home: $('homeScreen'), review: $('reviewScreen'), editor: $('editorScreen'),
        builderChoice: $('builderChoiceScreen'), workoutEditor: $('workoutEditorScreen'),
        deck: $('deckScreen'), tools: $('toolsScreen'), json: $('jsonScreen'), led: $('ledScreen'), audio: $('audioScreen'),
    };

    const WORKOUT_EXAMPLES = {
        pyramid: (pool) => `workout "Pyramid 100–500"
pool "${pool}"
direction near
audio yes

ladder 100 to 500 to 100 step 100 on 1:15 per 100 hold 1:00 per 100`,
        rounds: (pool) => `workout "Descending quality 50s"
pool "${pool}"
direction near
audio yes

repeat 4 rounds {
    10 x 50 on :40 hold :27 on-change -5s/round hold-change -0.75s/round
}`,
        negativeSplit: (pool) => `workout "Five 500 negative splits"
pool "${pool}"
direction near
audio yes

5 x 500 negative split by :04 on 5:30 hold 4:40`,
        continuous: (pool) => `workout "Continuous sprint 50s"
pool "${pool}"
direction near
audio yes

repeat until stopped {
    1 x 50 sprint on :40 hold :25
}`,
    };
    const MAX_PACKED_PLAN_BYTES = 12 * 1024;
    let deckScriptChatGptPrompt = '';

    function token() {
        const queryToken = new URLSearchParams(location.search).get('token');
        if (queryToken) localStorage.setItem('rabbitToken', queryToken);
        return queryToken || localStorage.getItem('rabbitToken') || '';
    }

    function url(path) {
        const value = token();
        if (!value) return path;
        const target = new URL(path, location.origin);
        target.searchParams.set('token', value);
        return target.pathname + target.search;
    }

    async function request(path, options) {
        const response = await fetch(url(path), Object.assign({ cache: 'no-store' }, options || {}));
        const text = await response.text();
        let data = {};
        try { data = text ? JSON.parse(text) : {}; } catch (_) { data = { text: text }; }
        if (!response.ok) throw new Error(data.error || `Request failed (${response.status})`);
        return data;
    }

    function show(screen, push) {
        if (!screens[screen]) return;
        if (push !== false && state.screen !== screen) state.history.push(state.screen);
        Object.keys(screens).forEach((name) => screens[name].classList.toggle('is-active', name === screen));
        state.screen = screen;
        $('backButton').classList.toggle('is-hidden', screen === 'home' || screen === 'deck');
        $('menuButton').classList.toggle('is-hidden', screen === 'deck');
        window.scrollTo(0, 0);
    }

    function goBack() {
        show(state.history.pop() || 'home', false);
    }

    function toast(message) {
        const node = $('toast');
        node.textContent = message;
        node.classList.add('is-visible');
        clearTimeout(toast.timer);
        toast.timer = setTimeout(() => node.classList.remove('is-visible'), 2400);
    }

    function applyTheme(choice) {
        const selected = choice || localStorage.getItem('coachTheme') || 'dark';
        const systemDark = window.matchMedia && window.matchMedia('(prefers-color-scheme: dark)').matches;
        const resolved = selected === 'dark' || (selected === 'system' && systemDark) ? 'dark' : 'light';
        document.documentElement.dataset.theme = selected;
        document.documentElement.dataset.resolvedTheme = resolved;
        localStorage.setItem('coachTheme', selected);
        document.querySelector('meta[name="theme-color"]').setAttribute('content', resolved === 'dark' ? '#071a2b' : '#0878c9');
        document.querySelectorAll('[data-theme-choice]').forEach((button) => {
            button.classList.toggle('is-selected', button.dataset.themeChoice === selected);
            button.setAttribute('aria-pressed', button.dataset.themeChoice === selected ? 'true' : 'false');
        });
    }

    function setConnection(connected) {
        document.querySelector('.app-header').classList.toggle('is-offline', !connected);
        $('poolLabel').textContent = connected ? (state.defaultPool || 'No pool selected') : 'Controller unavailable';
    }

    function formatStrategy(value) {
        return ({ even: 'Even pace', negative_split: 'Descend', surge: 'Surge' })[value] || value;
    }

    function isWorkoutSet(set) {
        return !!set && set.format === 'deckscript' && set.version === 2;
    }

    function canPrepareSet(set) {
        return isWorkoutSet(set) || !!(set && set.pool && set.duration && set.interval);
    }

    function workoutFromSet(set) {
        if (!isWorkoutSet(set)) throw new Error('This is not a DeckScript 2 workout.');
        const model = DeckScript.parse(set.source);
        if (!model.pool) model.pool = state.defaultPool;
        return model;
    }

    function workoutStats(plan) {
        const swims = plan.entries.filter((entry) => entry.kind === 'swim');
        return {
            swims: swims.length,
            distance: swims.reduce((total, entry) => total + Number(entry.distance || 0), 0),
            rests: plan.entries.filter((entry) => entry.kind === 'rest').length,
            activities: plan.entries.filter((entry) => entry.kind === 'activity').length,
        };
    }

    function generatedName(set) {
        if (isWorkoutSet(set)) {
            try { return workoutFromSet(set).name; } catch (_) { return 'DeckScript workout'; }
        }
        if (set.mode === 'sprint') return `${set.duration} sprint`;
        return `${set.repetitions} × ${set.distance} @ ${set.interval}`;
    }

    function describe(set) {
        if (isWorkoutSet(set)) {
            try {
                const plan = DeckScript.compile(workoutFromSet(set));
                const stats = workoutStats(plan);
                return `${plan.continuous ? 'Continuous' : `${stats.swims} swims`} · ${stats.distance} total distance`;
            } catch (error) {
                return `Needs review · ${error.message.split('\n')[0]}`;
            }
        }
        if (set.mode === 'sprint') return `${set.duration} target · ${formatStrategy(set.strategy)}`;
        return `${set.repetitions} reps · ${set.distance} · ${set.duration} target`;
    }

    function renderSets() {
        const list = $('setList');
        const entries = Object.entries(state.sets);
        list.innerHTML = '';
        if (!entries.length) {
            list.innerHTML = '<div class="empty-state">No saved sets yet.<br>Create one to get on deck.</div>';
            return;
        }
        entries.sort((a, b) => a[0].localeCompare(b[0])).forEach(([name, set]) => {
            const card = document.createElement('button');
            card.type = 'button';
            card.className = 'set-card';
            card.innerHTML =
                `<div class="set-card-top"><h3>${escapeHtml(name || generatedName(set))}</h3><span class="chevron">›</span></div>` +
                `<p>${escapeHtml(describe(set))}</p>` +
                (isWorkoutSet(set)
                    ? '<div class="set-card-tags"><span class="tag">Workout</span><span class="tag">DeckScript 2</span></div>'
                    : `<div class="set-card-tags"><span class="tag">${set.mode === 'sprint' ? 'Sprint' : 'Pace'}</span>` +
                      `<span class="tag">${escapeHtml(set.direction || 'Near')} end</span>` +
                      `<span class="tag">${set.audio === 'No' ? 'Silent' : 'Audio'}</span></div>`);
            card.addEventListener('click', () => selectSet(name, set));
            list.appendChild(card);
        });
    }

    function escapeHtml(value) {
        return String(value == null ? '' : value)
            .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
            .replace(/"/g, '&quot;').replace(/'/g, '&#039;');
    }

    function selectSet(name, set) {
        state.selectedName = name || generatedName(set);
        state.selectedSet = Object.assign({}, set);
        try {
            renderReview();
            show('review');
        } catch (error) {
            toast(error.message);
            if (isWorkoutSet(set)) {
                populateWorkoutEditor(set.source);
                show('workoutEditor');
            }
        }
    }

    function renderReview() {
        const set = state.selectedSet;
        if (!set) return;
        $('deleteSetButton').classList.toggle(
            'is-hidden',
            !Object.prototype.hasOwnProperty.call(state.sets, state.selectedName)
        );
        if (isWorkoutSet(set)) {
            const model = workoutFromSet(set);
            const plan = DeckScript.compile(model);
            const missingTargets = plan.entries.filter((entry) => entry.kind === 'swim' && entry.targetSeconds == null);
            if (missingTargets.length) throw new Error(`${missingTargets.length} swim${missingTargets.length === 1 ? '' : 's'} need a Hold target.`);
            const stats = workoutStats(plan);
            state.workoutModel = model;
            state.workoutPlan = plan;
            $('reviewTitle').textContent = state.selectedName || model.name;
            $('reviewSummary').textContent = plan.continuous
                ? `${stats.swims}-swim cycle repeats until stopped`
                : `${stats.swims} paced swims · ${stats.distance} total distance`;
            $('reviewReps').textContent = plan.continuous ? 'Continuous' : stats.swims;
            $('reviewDistance').textContent = stats.distance;
            $('reviewInterval').textContent = 'Varied';
            $('reviewTarget').textContent = 'Per step';
            $('reviewStrategy').textContent = stats.activities ? `${stats.activities} skill steps` : 'Structured';
            $('reviewDirection').textContent = `${plan.direction} end`;
            $('reviewAudio').textContent = plan.audio === 'No' ? 'Off' : 'On';
            return;
        }
        $('reviewTitle').textContent = state.selectedName || generatedName(set);
        $('reviewSummary').textContent = describe(set);
        $('reviewReps').textContent = set.mode === 'sprint' ? 'Continuous' : set.repetitions;
        $('reviewDistance').textContent = set.distance;
        $('reviewInterval').textContent = set.interval;
        $('reviewTarget').textContent = set.duration;
        $('reviewStrategy').textContent = formatStrategy(set.strategy);
        $('reviewDirection').textContent = `${set.direction || 'Near'} end`;
        $('reviewAudio').textContent = set.audio === 'No' ? 'Off' : 'On';
    }

    function populateEditor(set, name) {
        const value = set || {
            mode: 'pace', pool: state.defaultPool, direction: 'Near', audio: 'Yes',
            duration: '1:20.0', distance: 100, repetitions: 10, interval: '1:30.0',
            strategy: 'even', variation: '8',
        };
        $('editorTitle').textContent = set ? 'Edit set' : 'Create a set';
        $('setName').value = name || '';
        $('setMode').value = value.mode || 'pace';
        $('setPool').value = value.pool || state.defaultPool;
        $('setDirection').value = value.direction || 'Near';
        $('setAudio').value = value.audio || 'Yes';
        $('setDuration').value = value.duration || '';
        $('setDistance').value = value.distance || 25;
        $('setRepetitions').value = value.repetitions || 1;
        $('setInterval').value = value.interval || '';
        $('setStrategy').value = value.strategy || 'even';
        $('setVariation').value = value.variation == null ? '8' : value.variation;
        syncModeFields();
        syncStrategyFields(false);
        $('formError').classList.add('is-hidden');
    }

    function populateWorkoutEditor(source) {
        $('workoutEditorTitle').textContent = source ? 'Edit workout' : 'Build a workout';
        $('workoutSource').value = source || WORKOUT_EXAMPLES.pyramid(state.defaultPool || 'Bellevue East');
        previewWorkout();
    }

    function previewWorkout() {
        const diagnostics = $('workoutDiagnostics');
        const preview = $('workoutPreview');
        try {
            const model = DeckScript.parse($('workoutSource').value);
            if (!model.pool) model.pool = state.defaultPool;
            if (state.pools.length && !state.pools.includes(model.pool)) {
                throw new Error(`Unknown pool “${model.pool}”.`);
            }
            const plan = DeckScript.compile(model);
            const stats = workoutStats(plan);
            const missingTargets = plan.entries.filter((entry) => entry.kind === 'swim' && entry.targetSeconds == null);
            const packedText = JSON.stringify({ plan: DeckScript.pack(plan) });
            const packedBytes = typeof TextEncoder === 'function'
                ? new TextEncoder().encode(packedText).length
                : unescape(encodeURIComponent(packedText)).length;
            const tooManySteps = plan.entries.length > DeckScript.MAX_ENTRIES;
            const packedTooLarge = packedBytes > MAX_PACKED_PLAN_BYTES;
            const tooLarge = tooManySteps || packedTooLarge;
            state.workoutModel = model;
            state.workoutPlan = plan;

            if (missingTargets.length || tooLarge) {
                const messages = [];
                if (missingTargets.length) messages.push(`${missingTargets.length} swim step${missingTargets.length === 1 ? '' : 's'} need a Hold target before this can run.`);
                if (tooManySteps) messages.push(`${plan.entries.length} steps exceed the controller limit of ${DeckScript.MAX_ENTRIES}.`);
                if (packedTooLarge) messages.push('This plan is too detailed for the controller memory budget; shorten labels or split it into two workouts.');
                diagnostics.textContent = messages.join(' ');
                diagnostics.classList.add('is-error');
            } else {
                diagnostics.textContent = `Ready to run · ${plan.entries.length} controller steps · DeckScript ${plan.version}`;
                diagnostics.classList.remove('is-error');
            }

            const stepBars = plan.entries.slice(0, 80).map((entry) =>
                `<i class="preview-step is-${escapeHtml(entry.kind)}" title="${escapeHtml(entry.label || entry.kind)}"></i>`
            ).join('');
            preview.innerHTML =
                `<div class="preview-summary"><span><strong>${escapeHtml(model.name)}</strong>` +
                `<small>${stats.swims} swims · ${stats.distance} total distance · ${stats.rests} rests</small></span>` +
                `<span>${plan.continuous ? 'REPEATS' : `${plan.entries.length} STEPS`}</span></div>` +
                `<div class="preview-steps">${stepBars}</div>`;
            return { model, plan, missingTargets, tooLarge, packedBytes };
        } catch (error) {
            state.workoutModel = null;
            state.workoutPlan = null;
            diagnostics.textContent = error.message;
            diagnostics.classList.add('is-error');
            preview.innerHTML = '';
            return null;
        }
    }

    function workoutSetFromEditor(requireRunnable) {
        const result = previewWorkout();
        if (!result) throw new Error('Fix the DeckScript error before continuing.');
        if (requireRunnable && result.missingTargets.length) {
            throw new Error('Every swim needs a Hold target before it can run.');
        }
        if (result.tooLarge) throw new Error('This workout exceeds the controller memory budget.');
        return {
            name: result.model.name,
            set: { format: 'deckscript', version: 2, source: DeckScript.format(result.model) },
            model: result.model,
            plan: result.plan,
        };
    }

    function reviewWorkout() {
        try {
            const workout = workoutSetFromEditor(true);
            state.selectedName = workout.name;
            state.selectedSet = workout.set;
            state.workoutModel = workout.model;
            state.workoutPlan = workout.plan;
            renderReview();
            show('review');
        } catch (error) { toast(error.message); }
    }

    function importWorkout() {
        const message = $('importMessage');
        try {
            const result = DeckScript.importShorthand($('importSource').value, {
                pool: state.defaultPool,
                direction: 'Near',
                audio: 'Yes',
                holdPer100: $('importHold').value.trim(),
            });
            $('workoutSource').value = DeckScript.format(result.model);
            message.textContent = result.warnings.length
                ? result.warnings.join(' ')
                : 'Converted. Review the controller steps below.';
            previewWorkout();
            $('workoutSource').focus();
        } catch (error) {
            message.textContent = error.message;
        }
    }

    async function saveWorkout() {
        try {
            const workout = workoutSetFromEditor(false);
            state.sets[workout.name] = workout.set;
            await request('/db/sets.json', {
                method: 'POST', headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ sets: state.sets }),
            });
            renderSets();
            toast(`Saved “${workout.name}”`);
            state.selectedName = workout.name;
            state.selectedSet = workout.set;
        } catch (error) { toast(error.message); }
    }

    function syncModeFields() {
        const sprint = $('setMode').value === 'sprint';
        $('setRepetitions').disabled = sprint;
        $('setDistance').disabled = sprint;
        if (sprint) {
            $('setRepetitions').value = 0;
            $('setDistance').value = 25;
        } else {
            if (Number($('setRepetitions').value) < 1) $('setRepetitions').value = 10;
        }
    }

    function syncStrategyFields(resetValue) {
        const strategy = $('setStrategy').value;
        const variation = $('setVariation');
        $('setVariationField').classList.toggle('is-hidden', strategy === 'even');
        variation.disabled = strategy === 'even';
        if (strategy === 'negative_split') {
            $('setVariationLabel').textContent = 'Final rep target';
            variation.placeholder = '4:40';
            if (resetValue && variation.value === '8') variation.value = '';
        } else if (strategy === 'surge') {
            $('setVariationLabel').textContent = 'Surge change (%)';
            variation.placeholder = '8';
            if (resetValue && (!variation.value || variation.value.includes(':'))) variation.value = '8';
        }
    }

    function setFromForm() {
        const sprint = $('setMode').value === 'sprint';
        const set = {
            mode: sprint ? 'sprint' : 'pace',
            pool: $('setPool').value,
            direction: $('setDirection').value,
            audio: $('setAudio').value,
            duration: $('setDuration').value.trim(),
            distance: sprint ? 25 : Number($('setDistance').value),
            repetitions: sprint ? 0 : Number($('setRepetitions').value),
            interval: $('setInterval').value.trim(),
            strategy: $('setStrategy').value,
            variation: $('setVariation').value.trim(),
        };
        if (!set.pool) throw new Error('Choose a pool.');
        if (!set.duration || !set.interval) throw new Error('Enter target time and send-off.');
        if (!sprint && (!set.distance || !set.repetitions)) throw new Error('Distance and repetitions must be greater than zero.');
        const targetSeconds = DeckScript.timeSeconds(set.duration);
        const intervalSeconds = DeckScript.timeSeconds(set.interval);
        if (!(targetSeconds > 0) || intervalSeconds < targetSeconds) {
            throw new Error('Send-off must be greater than or equal to target time.');
        }
        if (set.strategy === 'negative_split') {
            const finalSeconds = DeckScript.timeSeconds(set.variation);
            if (!(finalSeconds > 0 && finalSeconds < targetSeconds)) {
                throw new Error('Final rep target must be faster than the first target.');
            }
        } else if (set.strategy === 'surge') {
            const change = Number(set.variation);
            if (!(change > 0 && change <= 45)) throw new Error('Surge change must be between 0 and 45%.');
        }
        return set;
    }

    async function saveSet() {
        try {
            const set = setFromForm();
            const customName = $('setName').value.trim();
            const name = customName || generatedName(set);
            state.sets[name] = set;
            await request('/db/sets.json', {
                method: 'POST', headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ sets: state.sets }),
            });
            renderSets();
            toast(`Saved “${name}”`);
            selectSet(name, set);
        } catch (error) { showFormError(error.message); }
    }

    async function deleteSet() {
        const name = state.selectedName;
        if (!Object.prototype.hasOwnProperty.call(state.sets, name)) return;
        if (!confirm(`Delete “${name}”? This cannot be undone.`)) return;

        const button = $('deleteSetButton');
        button.disabled = true;
        try {
            const sets = Object.assign({}, state.sets);
            delete sets[name];
            await request('/db/sets.json', {
                method: 'POST', headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ sets: sets }),
            });
            state.sets = sets;
            state.selectedName = '';
            state.selectedSet = null;
            state.workoutModel = null;
            state.workoutPlan = null;
            state.history = [];
            renderSets();
            show('home', false);
            toast(`Deleted “${name}”`);
        } catch (error) {
            toast(error.message);
        } finally {
            button.disabled = false;
        }
    }

    function showFormError(message) {
        $('formError').textContent = message;
        $('formError').classList.remove('is-hidden');
    }

    function prepPath(set) {
        const params = new URLSearchParams(set);
        return `/prep?${params.toString()}`;
    }

    async function prepare() {
        if (!state.selectedSet || state.busy) return;
        setBusy($('prepareButton'), true, 'Preparing…');
        try {
            if (isWorkoutSet(state.selectedSet)) {
                const model = workoutFromSet(state.selectedSet);
                const plan = DeckScript.compile(model);
                const missingTargets = plan.entries.filter((entry) => entry.kind === 'swim' && entry.targetSeconds == null);
                if (missingTargets.length) throw new Error('Every swim needs a Hold target before it can run.');
                await request('/api/workout/prepare', {
                    method: 'POST', headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ plan: DeckScript.pack(plan) }),
                });
                state.workoutModel = model;
                state.workoutPlan = plan;
            } else {
                await request(prepPath(state.selectedSet));
            }
            state.deckPhase = 'prepared';
            state.history = [];
            show('deck', false);
            await refreshStatus();
        } catch (error) {
            toast(error.message);
        } finally { setBusy($('prepareButton'), false, 'Prepare set'); }
    }

    function setBusy(button, busy, label) {
        state.busy = busy;
        button.disabled = busy;
        const span = button.querySelector('span');
        if (span) span.textContent = label; else button.textContent = label;
    }

    function updateDeck(status) {
        if (!status) return;
        state.server = status;
        const details = status.setDetails || {};
        const workout = status.workout || null;
        const entry = workout && workout.entry;
        const total = entry && entry.kind === 'swim'
            ? (entry.swimCount || '∞')
            : details.repetitions || (state.selectedSet && state.selectedSet.repetitions) || '∞';
        const current = entry && entry.kind === 'swim'
            ? (entry.swimIndex || 1)
            : details.currentRep || 1;
        $('progressLabel').textContent = workout && entry && entry.kind !== 'swim' ? 'STEP' : 'REP';
        $('deckSetName').textContent = (workout && workout.name) || state.selectedName || (state.selectedSet ? generatedName(state.selectedSet) : 'Current set');
        $('currentRep').textContent = current;
        $('totalReps').textContent = total || '∞';

        const primary = $('deckPrimaryButton');
        const cancel = $('deckCancelButton');
        primary.classList.remove('is-start', 'is-stop');
        cancel.classList.toggle('is-hidden', !status.prepped || status.running || status.complete);
        if (status.running) {
            state.deckPhase = 'running';
            $('deckState').textContent = entry && entry.kind !== 'swim' ? entry.kind : 'Running';
            $('countdownLabel').textContent = entry && entry.kind === 'rest' ? 'Rest remaining' : 'Next start';
            $('countdownValue').textContent = details.timeUntilNextRepText || '—:—';
            if (entry && entry.kind === 'swim') {
                const target = DeckScript.formatTime(entry.targetSeconds);
                const sendoff = DeckScript.formatTime(entry.intervalSeconds || entry.targetSeconds);
                let strategy = '';
                if (entry.strategy === 'negativeSplit') {
                    strategy = ` · Back half ${entry.splitDeltaSeconds}s faster`;
                } else if (entry.strategy === 'surge') {
                    strategy = ` · Surge ${entry.variation * 100}%`;
                }
                $('deckMessage').textContent = `${entry.distance} ${entry.label || ''} · Hold ${target} · On ${sendoff}${strategy}`.replace(/\s+/g, ' ').trim();
            } else if (entry) {
                $('deckMessage').textContent = entry.label || entry.activity || 'Next step';
            } else {
                $('deckMessage').textContent = 'Stop ends this rep and queues the next one.';
            }
            primary.textContent = 'Stop';
            primary.classList.add('is-stop');
        } else if (status.complete) {
            const canRunAgain = canPrepareSet(state.selectedSet);
            state.deckPhase = canRunAgain ? 'complete' : 'completeUnavailable';
            $('deckState').textContent = 'Complete';
            $('countdownLabel').textContent = 'Set finished';
            $('countdownValue').textContent = 'DONE';
            $('deckMessage').textContent = canRunAgain
                ? 'Nice work. Run it again or choose another set.'
                : 'Nice work. Choose the saved set to run it again.';
            primary.textContent = canRunAgain ? 'Run again' : 'Choose a set';
            primary.classList.add('is-start');
        } else if (status.prepped) {
            const advanced = workout && workout.entryIndex > 0;
            const paused = advanced || current > 1;
            state.deckPhase = paused ? 'paused' : 'prepared';
            $('deckState').textContent = paused ? 'Paused' : 'Ready';
            $('countdownLabel').textContent = paused ? 'Next up' : 'Ready to start';
            $('countdownValue').textContent = paused ? (entry && entry.kind === 'swim' ? `REP ${current}` : 'NEXT') : '—:—';
            $('deckMessage').textContent = paused
                ? (entry && entry.kind === 'swim' ? `${entry.distance} ${entry.label || ''}`.trim() : (entry && (entry.label || entry.activity)) || 'Continue to the next step.')
                : 'Timing and LEDs are prepared.';
            primary.textContent = paused ? 'Continue' : 'Start';
            primary.classList.add('is-start');
        } else {
            state.deckPhase = 'idle';
        }
    }

    async function deckPrimary() {
        if (state.busy) return;
        const button = $('deckPrimaryButton');
        state.busy = true;
        button.disabled = true;
        try {
            if (state.deckPhase === 'running') {
                await request('/stop');
                await refreshStatus();
            } else if (state.deckPhase === 'complete') {
                await prepare();
            } else if (state.deckPhase === 'completeUnavailable') {
                await mainMenu();
            } else {
                const path = state.server && state.server.mode === 'workout'
                    ? '/api/workout/start'
                    : state.selectedSet && state.selectedSet.mode === 'sprint' ? '/startsprint' : '/start';
                await request(path, path === '/api/workout/start' ? { method: 'POST' } : undefined);
                await refreshStatus();
            }
        } catch (error) { toast(error.message); }
        finally { state.busy = false; button.disabled = false; }
    }

    async function cancelSet() {
        if (state.busy) return;
        const button = $('deckCancelButton');
        state.busy = true;
        button.disabled = true;
        try {
            await request('/cancel-prep');
            state.deckPhase = 'idle';
            await refreshStatus();
            renderReview();
            show('review', false);
            toast('Set canceled');
        } catch (error) { toast(error.message); }
        finally { state.busy = false; button.disabled = false; }
    }

    async function refreshStatus() {
        if (state.statusBusy) return;
        state.statusBusy = true;
        const controller = typeof AbortController !== 'undefined' ? new AbortController() : null;
        const timeout = controller ? setTimeout(() => controller.abort(), 3500) : null;
        try {
            const status = await request('/api/set-status', controller ? { signal: controller.signal } : undefined);
            setConnection(true);
            state.server = status;
            if (state.screen === 'deck') updateDeck(status);
            const resumable = !status.complete && (status.prepped || status.running);
            $('resumeCard').classList.toggle('is-hidden', !resumable);
            if (resumable) {
                const details = status.setDetails || {};
                $('resumeTitle').textContent = status.running ? 'Set running' : 'Set ready';
                $('resumeMeta').textContent = status.mode === 'workout' && status.workout
                    ? `${status.workout.name} · step ${Math.min(status.workout.entryIndex + 1, status.workout.entryCount)} of ${status.workout.entryCount}`
                    : `Rep ${details.currentRep || 1} of ${details.repetitions || '∞'}`;
                $('resumeButton').textContent = status.running ? 'View' : 'Return';
            }
        } catch (_) {
            setConnection(false);
        } finally {
            if (timeout) clearTimeout(timeout);
            state.statusBusy = false;
        }
    }

    async function mainMenu() {
        show('home', false);
    }

    function restoreSelectedSet(status) {
        if (!status) return;
        if (status.mode === 'workout' && status.workout) {
            const match = Object.entries(state.sets).find((entry) => {
                try {
                    return isWorkoutSet(entry[1]) && workoutFromSet(entry[1]).name === status.workout.name;
                } catch (_) {
                    return false;
                }
            });
            if (match) {
                state.selectedName = match[0];
                state.selectedSet = Object.assign({}, match[1]);
            }
            return;
        }
        const details = status.setDetails || {};
        const match = Object.entries(state.sets).find((entry) => {
            const set = entry[1];
            if (isWorkoutSet(set) || set.mode !== (status.mode || 'pace')) return false;
            try {
                return Number(set.distance) === Number(details.distance)
                    && Number(set.repetitions) === Number(details.repetitions)
                    && Math.abs(DeckScript.timeSeconds(set.duration) - Number(details.totalTargetDurationSeconds)) < 0.01;
            } catch (_) {
                return false;
            }
        });
        if (match) {
            state.selectedName = match[0];
            state.selectedSet = Object.assign({}, match[1]);
        } else {
            state.selectedSet = { mode: status.mode || 'pace', repetitions: details.repetitions || 0 };
            state.selectedName = 'Current set';
        }
    }

    async function openJson(kind) {
        state.jsonPath = kind === 'pool' ? '/db/pools.json' : '/db/wifi.json';
        $('jsonTitle').textContent = kind === 'pool' ? 'Pool settings' : 'Network settings';
        $('jsonMessage').textContent = 'Loading…';
        show('json');
        try {
            const data = await request(state.jsonPath);
            $('jsonEditor').value = JSON.stringify(data, null, 2);
            $('jsonMessage').textContent = '';
        } catch (error) { $('jsonMessage').textContent = error.message; }
    }

    async function saveJson() {
        try {
            const data = JSON.parse($('jsonEditor').value);
            await request(state.jsonPath, {
                method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(data),
            });
            $('jsonMessage').textContent = 'Saved successfully.';
            if (state.jsonPath.indexOf('pools') >= 0) await loadData();
        } catch (error) { $('jsonMessage').textContent = error.message; }
    }

    async function loadData() {
        const [poolData, setData] = await Promise.all([request('/db/pools.json'), request('/db/sets.json')]);
        state.defaultPool = poolData.defaultPool || '';
        state.pools = Object.keys(poolData.pools || {});
        state.sets = setData.sets || {};
        $('poolLabel').textContent = state.defaultPool || 'No pool selected';
        $('setPool').innerHTML = state.pools.map((name) => `<option${name === state.defaultPool ? ' selected' : ''}>${escapeHtml(name)}</option>`).join('');
        renderSets();
    }

    async function loadDeckScriptChatGptPrompt() {
        const data = await request('/static/deckscript-chatgpt-prompt.txt');
        if (!data.text || !data.text.trim()) throw new Error('The DeckScript ChatGPT prompt is empty.');
        deckScriptChatGptPrompt = data.text.trim();
        const button = $('copyDeckScriptPromptButton');
        button.disabled = false;
        button.title = 'Copy the DeckScript assistant prompt to the clipboard';
    }

    function copyDeckScriptChatGptPrompt() {
        if (!deckScriptChatGptPrompt) {
            toast('ChatGPT prompt is not available');
            return;
        }

        const copied = () => toast('ChatGPT prompt copied');
        const legacyCopy = () => {
            const field = document.createElement('textarea');
            field.value = deckScriptChatGptPrompt;
            field.setAttribute('readonly', '');
            field.setAttribute('aria-hidden', 'true');
            field.style.position = 'fixed';
            field.style.opacity = '0';
            document.body.appendChild(field);
            field.focus();
            field.select();
            field.setSelectionRange(0, field.value.length);
            const succeeded = document.execCommand('copy');
            field.remove();
            if (succeeded) copied(); else toast('Could not copy the ChatGPT prompt');
        };

        if (navigator.clipboard && window.isSecureContext) {
            navigator.clipboard.writeText(deckScriptChatGptPrompt).then(copied).catch(legacyCopy);
        } else {
            legacyCopy();
        }
    }

    function bind() {
        document.querySelectorAll('[data-theme-choice]').forEach((button) => {
            button.addEventListener('click', () => applyTheme(button.dataset.themeChoice));
        });
        $('backButton').addEventListener('click', goBack);
        $('menuButton').addEventListener('click', () => show('tools'));
        $('newSetButton').addEventListener('click', () => show('builderChoice'));
        $('quickBuilderButton').addEventListener('click', () => { populateEditor(); show('editor'); });
        $('workoutBuilderButton').addEventListener('click', () => { populateWorkoutEditor(); show('workoutEditor'); });
        $('copyDeckScriptPromptButton').addEventListener('click', copyDeckScriptChatGptPrompt);
        $('editSetButton').addEventListener('click', () => {
            if (isWorkoutSet(state.selectedSet)) {
                populateWorkoutEditor(state.selectedSet.source);
                show('workoutEditor');
            } else {
                populateEditor(state.selectedSet, state.selectedName);
                show('editor');
            }
        });
        $('prepareButton').addEventListener('click', prepare);
        $('setMode').addEventListener('change', syncModeFields);
        $('setStrategy').addEventListener('change', () => syncStrategyFields(true));
        $('setForm').addEventListener('submit', (event) => {
            event.preventDefault();
            try {
                const set = setFromForm();
                state.selectedSet = set;
                state.selectedName = $('setName').value.trim() || generatedName(set);
                renderReview();
                show('review');
            } catch (error) { showFormError(error.message); }
        });
        $('saveSetButton').addEventListener('click', saveSet);
        $('deleteSetButton').addEventListener('click', deleteSet);
        $('reviewWorkoutButton').addEventListener('click', reviewWorkout);
        $('saveWorkoutButton').addEventListener('click', saveWorkout);
        $('importWorkoutButton').addEventListener('click', importWorkout);
        $('workoutSource').addEventListener('input', () => {
            clearTimeout(previewWorkout.timer);
            previewWorkout.timer = setTimeout(previewWorkout, 220);
        });
        document.querySelectorAll('[data-workout-example]').forEach((button) => {
            button.addEventListener('click', () => {
                $('workoutSource').value = WORKOUT_EXAMPLES[button.dataset.workoutExample](state.defaultPool || 'Bellevue East');
                previewWorkout();
            });
        });
        $('deckPrimaryButton').addEventListener('click', deckPrimary);
        $('deckCancelButton').addEventListener('click', cancelSet);
        $('deckMenuButton').addEventListener('click', mainMenu);
        $('resumeButton').addEventListener('click', () => {
            if (state.server && !state.selectedSet) restoreSelectedSet(state.server);
            updateDeck(state.server);
            show('deck', false);
        });
        document.querySelectorAll('[data-tool]').forEach((button) => button.addEventListener('click', () => {
            const tool = button.dataset.tool;
            if (tool === 'led' || tool === 'audio') show(tool); else openJson(tool);
        }));
        document.querySelectorAll('[data-api]').forEach((button) => button.addEventListener('click', async () => {
            try { await request(button.dataset.api); $('ledMessage').textContent = 'Command completed.'; }
            catch (error) { $('ledMessage').textContent = error.message; }
        }));
        $('saveJsonButton').addEventListener('click', saveJson);
        $('resetButton').addEventListener('click', async () => {
            if (!confirm('Restart the controller? This interrupts the current session.')) return;
            try { await request('/HardReset'); toast('Controller restarting'); } catch (error) { toast(error.message); }
        });
        if (location.port === '5000') {
            $('emulatorLink').classList.remove('is-hidden');
            $('emulatorLink').addEventListener('click', () => { location.href = '/emulator'; });
        }
    }

    async function init() {
        bind();
        applyTheme(document.documentElement.dataset.theme || 'dark');
        if (window.matchMedia) {
            const media = window.matchMedia('(prefers-color-scheme: dark)');
            if (media.addEventListener) {
                media.addEventListener('change', () => {
                    if (document.documentElement.dataset.theme === 'system') applyTheme('system');
                });
            }
        }
        try { await loadData(); }
        catch (error) { $('setList').innerHTML = `<div class="empty-state">Could not load sets.<br>${escapeHtml(error.message)}</div>`; }
        try { await loadDeckScriptChatGptPrompt(); }
        catch (error) { $('copyDeckScriptPromptButton').title = error.message; }
        await refreshStatus();
        state.poll = setInterval(refreshStatus, 2000);
    }

    init();
}());
