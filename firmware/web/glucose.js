// SPDX-License-Identifier: GPL-3.0-or-later
//
// The glucose clock's settings page. Four tabs, laid out like the TC001
// nightscout-clock page (Display, Glucose, Alarms, WiFi & system), over the
// one API this device has: /api/v1/*.
//
// The model: the settings document as the device last returned it, a draft
// copy the controls edit, and the set of paths that differ. Save sends one
// PATCH with just those paths; the device validates the whole and answers
// with the settings as they now stand, or 422 and changes nothing.
// Passwords never appear in the document - they are typed, sent once, and
// forgotten by the page.

(function () {
    'use strict';

    // --- talking to the device ------------------------------------------------

    // The emulator routes requests straight into the WASM module, because
    // there is no socket to talk to. On a real device it is absent.
    const bridge = window.STIPPLE_BRIDGE || null;

    function decode(status, text) {
        let payload = null;
        if (text) {
            try { payload = JSON.parse(text); } catch (e) { payload = null; }
        }
        if (status >= 200 && status < 300) {
            return payload;
        }
        const message = payload && payload.error && payload.error.message
            ? payload.error.message : 'the clock answered ' + status;
        const error = new Error(message);
        error.status = status;
        throw error;
    }

    async function send(method, path, body) {
        if (bridge) {
            const result = await bridge(method, path, body === undefined ? '' : JSON.stringify(body));
            return decode(result.status, result.body);
        }
        const init = { method, headers: {} };
        if (body !== undefined) {
            init.headers['Content-Type'] = 'application/json';
            init.body = JSON.stringify(body);
        }
        const response = await fetch(path, init);
        return decode(response.status, await response.text());
    }

    // --- tiny DOM helpers -----------------------------------------------------

    const $ = (id) => document.getElementById(id);

    function h(tag, attrs, ...children) {
        const parts = tag.split('.');
        const node = document.createElement(parts[0]);
        if (parts.length > 1) { node.className = parts.slice(1).join(' '); }
        for (const [key, value] of Object.entries(attrs || {})) {
            if (value === undefined || value === null || value === false) { continue; }
            if (key.startsWith('on')) { node.addEventListener(key.slice(2), value); }
            else if (key === 'text') { node.textContent = value; }
            else if (key === 'html') { node.innerHTML = value; }
            else if (key in node && typeof value !== 'string') { node[key] = value; }
            else { node.setAttribute(key, value === true ? '' : value); }
        }
        for (const child of children.flat()) {
            if (child === null || child === undefined || child === false) { continue; }
            node.appendChild(typeof child === 'string' ? document.createTextNode(child) : child);
        }
        return node;
    }

    function card(title, subtitle, ...body) {
        return h('section.card', {},
            h('div.card-head', {}, h('h2', { text: title }), subtitle ? h('p', { text: subtitle }) : null),
            h('div.stack', {}, ...body));
    }

    function field(label, control, help) {
        const id = control.id || ('f' + Math.random().toString(36).slice(2, 9));
        control.id = id;
        return h('div.field', {}, h('label', { for: id, text: label }), control,
            help ? h('p.help', { text: help }) : null);
    }

    function toggleRow(title, help, input) {
        return h('div.switch-row', {},
            h('div.text', {}, h('span.label', { text: title }), help ? h('span.help.small', { text: help }) : null),
            h('label.switch', {}, input, h('span')));
    }

    let toastTimer = null;
    function toast(text, bad) {
        const node = $('toast');
        node.textContent = text;
        node.className = 'toast notice ' + (bad ? 'bad' : 'ok');
        node.hidden = false;
        clearTimeout(toastTimer);
        toastTimer = setTimeout(() => { node.hidden = true; }, bad ? 6000 : 2500);
    }

    // --- the model --------------------------------------------------------------

    let server = null;   // settings as the device last returned them
    let draft = null;    // what the controls show
    const dirty = new Set();
    const secrets = {};  // path -> typed password, sent on save, then dropped
    const appChanges = {}; // app id -> enabled
    let apps = [];
    let device = {};
    let diag = {};
    let network = {};
    let health = {};

    const clone = (value) => JSON.parse(JSON.stringify(value));

    function get(path, from) {
        return path.split('.').reduce((node, key) => (node == null ? undefined : node[key]), from || draft);
    }

    function setIn(target, path, value) {
        const keys = path.split('.');
        let node = target;
        for (let i = 0; i < keys.length - 1; ++i) {
            if (node[keys[i]] === undefined || node[keys[i]] === null) { node[keys[i]] = {}; }
            node = node[keys[i]];
        }
        node[keys[keys.length - 1]] = value;
    }

    function set(path, value, rerender) {
        setIn(draft, path, value);
        if (JSON.stringify(get(path, server)) === JSON.stringify(value)) { dirty.delete(path); }
        else { dirty.add(path); }
        updateSavebar();
        if (rerender) { render(); }
    }

    function updateSavebar() {
        const count = dirty.size + Object.keys(secrets).length + Object.keys(appChanges).length;
        $('savebar').hidden = count === 0;
        $('dirty_text').textContent = count === 1 ? '1 unsaved change' : count + ' unsaved changes';
    }

    // Bound controls: each writes its path in the draft.
    function bindInput(path, attrs, convert) {
        const input = h('input', attrs);
        const value = get(path);
        if (attrs.type === 'checkbox') { input.checked = !!value; }
        else { input.value = value === undefined || value === null ? '' : value; }
        input.addEventListener(attrs.type === 'checkbox' || attrs.type === 'range' ? 'input' : 'change', () => {
            let next = attrs.type === 'checkbox' ? input.checked : input.value;
            if (convert) { next = convert(next); }
            set(path, next, attrs['data-rerender'] === '1');
        });
        return input;
    }

    function bindSelect(path, options, convert, rerender) {
        const select = h('select', {});
        for (const [value, label] of options) {
            select.appendChild(h('option', { value: String(value), text: label }));
        }
        const current = get(path);
        select.value = current === undefined || current === null ? '' : String(current);
        select.addEventListener('change', () => {
            set(path, convert ? convert(select.value) : select.value, rerender);
        });
        return select;
    }

    // A password: blank means "leave it", the placeholder says whether one is set.
    function secretInput(path, isSet, autocomplete) {
        const input = h('input', {
            type: 'password', autocomplete: autocomplete || 'new-password', maxlength: '128',
            placeholder: isSet ? 'Saved - type to replace' : 'Not set',
        });
        input.value = secrets[path] || '';
        input.addEventListener('input', () => {
            if (input.value === '') { delete secrets[path]; } else { secrets[path] = input.value; }
            updateSavebar();
        });
        return input;
    }

    // --- shared vocabulary --------------------------------------------------------

    const FACES = [
        ['hero', 'Simple', 'The number, as big as the panel allows, and its direction'],
        ['hero-delta', 'Value and delta', 'The value with the change since the last reading and its age'],
        ['hero-graph', 'Glucose graph and value', 'Three hours of readings beside the current value'],
        ['clock', 'Current time and BG value', 'The time and the current value, for the bedside'],
        ['big-graph', 'Full glucose graph', 'Three hours of readings across the whole panel'],
    ];
    const faceName = (id) => (FACES.find((f) => f[0] === id) || [id, id])[1];

    const CYCLES = [[0, 'Off'], [10, '10 s'], [30, '30 s'], [60, '1 min'], [120, '2 min'], [180, '3 min'], [300, '5 min']];

    // Ten levels like the TC001, over the panel's 0-255.
    const LEVELS = Array.from({ length: 10 }, (_, i) => [Math.round(((i + 1) / 10) * 255), 'Level ' + (i + 1)]);
    LEVELS[0][0] = 8;
    function nearestLevel(value) {
        let best = LEVELS[0][0];
        for (const [level] of LEVELS) { if (Math.abs(level - value) < Math.abs(best - value)) { best = level; } }
        return best;
    }

    const ZONES = [
        ['', 'Fixed offset'], ['UTC0', 'UTC'],
        ['EST5EDT,M3.2.0,M11.1.0', 'US Eastern'], ['CST6CDT,M3.2.0,M11.1.0', 'US Central'],
        ['MST7MDT,M3.2.0,M11.1.0', 'US Mountain'], ['MST7', 'Arizona (no daylight saving)'],
        ['PST8PDT,M3.2.0,M11.1.0', 'US Pacific'], ['AST4ADT,M3.2.0,M11.1.0', 'Atlantic Canada'],
        ['GMT0BST,M3.5.0/1,M10.5.0', 'United Kingdom, Ireland, Portugal'],
        ['CET-1CEST,M3.5.0,M10.5.0/3', 'Central Europe'], ['EET-2EEST,M3.5.0/3,M10.5.0/4', 'Eastern Europe'],
        ['MSK-3', 'Moscow'], ['IST-5:30', 'India'], ['<+04>-4', 'Gulf (Dubai)'],
        ['CST-8', 'China, Singapore, Hong Kong'], ['JST-9', 'Japan'], ['KST-9', 'Korea'],
        ['AEST-10AEDT,M10.1.0,M4.1.0/3', 'Sydney, Melbourne'], ['AEST-10', 'Brisbane'],
        ['NZST-12NZDT,M9.5.0,M4.1.0/3', 'New Zealand'], ['SAST-2', 'South Africa'],
        ['<-03>3', 'Brazil (Sao Paulo)'], ['<-05>5', 'Colombia, Peru'],
    ];

    const SOURCES = [['nightscout', 'Nightscout'], ['dexcom', 'Dexcom'], ['librelinkup', 'LibreLinkUp'], ['medtrum', 'Medtrum Easy Follow']];
    const DEXCOM_SERVERS = [['us', 'US'], ['ous', 'Non-US'], ['jp', 'Japan']];
    const LLU_REGIONS = [
        ['AE', 'United Arab Emirates'], ['AP', 'Asia Pacific'], ['AU', 'Australia'], ['CA', 'Canada'],
        ['DE', 'Germany'], ['EU', 'Europe'], ['EU2', 'Europe 2'], ['FR', 'France'], ['JP', 'Japan'],
        ['US', 'United States'], ['LA', 'Latin America'], ['RU', 'Russia'],
    ];

    const ALARMS = [
        { key: 'urgentLow', title: 'Urgent low', threshold: 'Sound at or below (mg/dL)', until: 'Until it is above the threshold' },
        { key: 'low', title: 'Low', threshold: 'Sound below (mg/dL)', until: 'Until it is back in range' },
        { key: 'high', title: 'High', threshold: 'Sound at or above (mg/dL)', until: 'Until it is back in range' },
        { key: 'noData', title: 'No data', threshold: null, until: 'Until a reading arrives' },
    ];
    // nightscout-clock's defaults and presets, verbatim, so a melody copied
    // from one of the TC001 clocks sounds the same here.
    const DEFAULT_MELODY = {
        urgentLow: 'urgent_low:d=4,o=5,b=230:4e6,4p,4e6,4p,4e6,4p,4e6',
        low: 'low:d=4,o=5,b=200:4e5,4p,4e5,4p,4e5',
        high: 'high:d=4,o=5,b=125:4e7,p,4e7',
        noData: 'doublebeep:d=8,o=6,b=180:c,p,c',
    };
    const SOUNDS = [
        ['doublebeep', 'Double beep', 'doublebeep:d=8,o=6,b=180:c,p,c'],
        ['triplebeep', 'Triple beep', 'triplebeep:d=16,o=6,b=200:c,p,c,p,c'],
        ['siren', 'Two tone siren', 'siren:d=4,o=5,b=100:a,d6,a,d6'],
        ['urgent', 'Urgent pulse', 'urgent:d=32,o=7,b=220:c,p,c,p,c,p,c,p,c,p,c'],
        ['ping', 'Soft ping', 'ping:d=4,o=6,b=140:8e,16p,8c'],
        ['longtone', 'Long tone', 'longtone:d=1,o=5,b=90:a'],
    ];
    const DAYS = ['S', 'M', 'T', 'W', 'T', 'F', 'S'];
    const DAY_NAMES = ['Sunday', 'Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday'];

    const ARROWS = {
        DOUBLE_UP: '⇈', SINGLE_UP: '↑', FORTY_FIVE_UP: '↗', FLAT: '→',
        FORTY_FIVE_DOWN: '↘', SINGLE_DOWN: '↓', DOUBLE_DOWN: '⇊', NONE: '',
    };

    const minutesToTime = (m) => String(Math.floor(m / 60)).padStart(2, '0') + ':' + String(m % 60).padStart(2, '0');
    const timeToMinutes = (t) => { const [hh, mm] = t.split(':').map(Number); return hh * 60 + mm; };

    // --- Display tab ----------------------------------------------------------------

    let panelTimer = null;
    function panelCard() {
        const canvas = h('canvas', { width: '520', height: '160', 'aria-label': 'What the panel shows now' });
        const note = h('p.help.small', { text: 'What the panel is showing, refreshed every two seconds.' });
        async function draw() {
            try {
                const frame = await send('GET', '/api/v1/display/frame');
                const bytes = atob(frame.pixels);
                const ctx = canvas.getContext('2d');
                ctx.fillStyle = '#000';
                ctx.fillRect(0, 0, 520, 160);
                for (let y = 0; y < frame.height; ++y) {
                    for (let x = 0; x < frame.width; ++x) {
                        const i = (y * frame.width + x) * 3;
                        const r = bytes.charCodeAt(i), g = bytes.charCodeAt(i + 1), b = bytes.charCodeAt(i + 2);
                        if (r | g | b) {
                            ctx.fillStyle = 'rgb(' + r + ',' + g + ',' + b + ')';
                            ctx.fillRect(x * 10 + 1, y * 10 + 1, 8, 8);
                        }
                    }
                }
            } catch (e) { /* the next tick tries again */ }
        }
        clearInterval(panelTimer);
        panelTimer = setInterval(() => { if (currentTab === 'display' && !document.hidden) { draw(); } }, 2000);
        draw();
        return h('section.card', {}, h('div.panel-view', {}, canvas, note));
    }

    function facesCard() {
        const active = get('glucose.faces') || [];
        const tiles = FACES.map(([id, name, help]) => {
            const on = active.includes(id);
            return h('button.face', {
                type: 'button', 'aria-pressed': on ? 'true' : 'false', title: help,
                onclick: () => {
                    let next = FACES.map((f) => f[0]).filter((f) => (f === id ? !on : active.includes(f)));
                    if (next.length === 0) { toast('At least one face has to stay in use', true); return; }
                    set('glucose.faces', next);
                    if (!next.includes(get('glucose.face'))) { set('glucose.face', next[0]); }
                    if (next.length < 2 && get('glucose.cycleSeconds') > 0) { set('glucose.cycleSeconds', 0); }
                    render();
                },
            }, h('span', {}, h('b', { text: name }), h('br'), h('span.small.muted', { text: help })),
            h('span.face-status', { text: on ? (get('glucose.face') === id ? '✓ Active, default' : '✓ Active') : 'Not active' }));
        });
        const usable = FACES.filter((f) => active.includes(f[0])).map((f) => [f[0], f[1]]);
        const scheduleOn = !!get('glucose.schedule.enabled');
        const cycle = bindSelect('glucose.cycleSeconds', CYCLES, Number, true);
        cycle.disabled = usable.length < 2 || scheduleOn;
        return card('Clock faces',
            'Tap the faces you use. Turning the knob moves only between these, and cycling runs through them in the order shown.',
            h('div.faces', {}, tiles),
            field('Face shown by default', bindSelect('glucose.face', usable, null, true),
                'The face the panel rests on when neither cycling nor the schedule says otherwise.'),
            field('Cycle through faces', cycle,
                scheduleOn ? 'Turn off the daily schedule to cycle faces.'
                    : usable.length < 2 ? 'Cycling needs at least two faces in use.'
                        : 'Moves to the next face on its own. The knob still changes it in between.'));
    }

    function scheduleCard() {
        const enabled = h('input', { type: 'checkbox' });
        enabled.checked = !!get('glucose.schedule.enabled');
        const rows = clone(get('glucose.schedule.rows') || []);
        const active = get('glucose.faces') || [];
        const usable = FACES.filter((f) => active.includes(f[0])).map((f) => [f[0], f[1]]);
        enabled.addEventListener('input', () => {
            if (enabled.checked && rows.length === 0) {
                rows.push({ from: '07:00', face: get('glucose.face'), brightness: null });
                rows.push({ from: '20:00', face: get('glucose.face'), brightness: 8 });
                set('glucose.schedule.rows', clone(rows));
            }
            set('glucose.schedule.enabled', enabled.checked, true);
        });
        const commit = () => {
            const sorted = clone(rows).sort((a, b) => timeToMinutes(a.from) - timeToMinutes(b.from));
            set('glucose.schedule.rows', sorted);
        };
        const list = h('div.rows', {}, rows.map((row, i) => {
            const time = h('input', { type: 'time', value: row.from, 'aria-label': 'From' });
            time.addEventListener('change', () => { row.from = time.value || '00:00'; commit(); });
            const face = h('select', { 'aria-label': 'Face' }, usable.map(([v, l]) => h('option', { value: v, text: l })));
            face.value = row.face;
            face.addEventListener('change', () => { row.face = face.value; commit(); });
            const level = h('select', { 'aria-label': 'Brightness' },
                h('option', { value: '', text: 'Brightness as set' }),
                LEVELS.map(([v, l]) => h('option', { value: String(v), text: l })));
            level.value = row.brightness === null || row.brightness === undefined ? '' : String(nearestLevel(row.brightness));
            level.addEventListener('change', () => { row.brightness = level.value === '' ? null : Number(level.value); commit(); });
            const remove = h('button.btn.icon', {
                type: 'button', 'aria-label': 'Remove this time', text: '×',
                onclick: () => {
                    rows.splice(i, 1);
                    commit();
                    if (rows.length === 0) { set('glucose.schedule.enabled', false); }
                    render();
                },
            });
            return h('div.sched-row', {}, time, face, level, remove);
        }));
        const add = h('button.btn', {
            type: 'button', text: 'Add time', disabled: rows.length >= 6,
            onclick: () => { rows.push({ from: '12:00', face: get('glucose.face'), brightness: null }); commit(); render(); },
        });
        return card('Daily schedule',
            'For example Big graph in the morning and the bedside clock at level 1 at night.',
            toggleRow('Change face and brightness on a schedule',
                'From each time the clock shows that face at that brightness until the next row; the last row runs overnight. The knob still changes the face in between.',
                enabled),
            enabled.checked ? list : null, enabled.checked ? h('div.row', {}, add) : null);
    }

    function brightnessCard() {
        const level = bindSelect('display.brightness', LEVELS, Number);
        level.value = String(nearestLevel(get('display.brightness')));
        const night = h('input', { type: 'checkbox' });
        night.checked = !!get('display.night.enabled');
        night.addEventListener('input', () => set('display.night.enabled', night.checked, true));
        const from = h('input', { type: 'time', value: minutesToTime(get('display.night.startMinutes') || 0) });
        from.addEventListener('change', () => set('display.night.startMinutes', timeToMinutes(from.value || '22:00')));
        const to = h('input', { type: 'time', value: minutesToTime(get('display.night.endMinutes') || 0) });
        to.addEventListener('change', () => set('display.night.endMinutes', timeToMinutes(to.value || '07:00')));
        const nightLevel = bindSelect('display.night.brightness', [[1, 'Barely lit'], ...LEVELS], Number);
        nightLevel.value = String(get('display.night.brightness') <= 2 ? 1 : nearestLevel(get('display.night.brightness')));
        return card('Brightness level', null,
            field('Brightness', level, 'Level 1 is the dimmest. The - and + buttons on the clock change it too. A schedule row with its own brightness overrides this.'),
            toggleRow('Dim overnight', 'Sits on top of everything else, including the schedule. An alarm always lights the panel.', night),
            night.checked ? h('div.grid', {}, field('From', from), field('Until', to), field('Overnight level', nightLevel)) : null);
    }

    function timeCard() {
        const zone = bindSelect('clock.timezone', ZONES, null, true);
        const children = [field('Time zone', zone), toggleRow('24-hour time', null, bindInput('clock.twentyFourHour', { type: 'checkbox' }))];
        if (!get('clock.timezone')) {
            const hours = [];
            for (let o = -12 * 3600; o <= 14 * 3600; o += 1800) {
                const sign = o < 0 ? '-' : '+';
                const a = Math.abs(o);
                hours.push([o, 'UTC' + sign + Math.floor(a / 3600) + (a % 3600 ? ':30' : '')]);
            }
            children.push(field('Fixed offset', bindSelect('clock.utcOffsetSeconds', hours, Number),
                'Used only with no time zone chosen - and an hour out for half the year anywhere that changes its clocks.'));
        }
        return card('Time', null, ...children);
    }

    function appsCard() {
        const others = apps.filter((a) => a.id !== 'glucose');
        if (others.length === 0) { return null; }
        return card('Other apps',
            'Stipple\'s other apps take turns with the glucose display when the middle button leaves it. Switch off what this clock does not need.',
            ...others.map((app) => {
                const input = h('input', { type: 'checkbox' });
                input.checked = app.id in appChanges ? appChanges[app.id] : app.enabled;
                input.addEventListener('input', () => {
                    if (input.checked === app.enabled) { delete appChanges[app.id]; } else { appChanges[app.id] = input.checked; }
                    updateSavebar();
                });
                return toggleRow(app.name, app.id === 'clock' ? 'Where the middle button goes from the glucose display.' : null, input);
            }));
    }

    function displayTab() {
        return [panelCard(), facesCard(), scheduleCard(), brightnessCard(), timeCard(), appsCard()];
    }

    // --- Glucose tab ------------------------------------------------------------------

    function sourceStatus() {
        const g = diag.glucose || {};
        if (!g.configured) { return h('p.notice.warn', { text: 'Not set up yet: the clock shows its no-data face until it has somewhere to read from.' }); }
        if (g.lastFailure) {
            return h('p.notice.bad', { text: 'Last attempt: ' + g.lastFailure + (g.holdSeconds > 0 ? ' - trying again in ' + Math.ceil(g.holdSeconds / 60) + ' min' : '') });
        }
        if (g.lastSuccessAgeSeconds >= 0) {
            return h('p.notice.ok', { text: 'Connected - last read ' + (g.lastSuccessAgeSeconds < 90 ? 'just now' : Math.round(g.lastSuccessAgeSeconds / 60) + ' min ago') + ', ' + g.samples + ' readings held.' });
        }
        return h('p.notice.info', { text: 'Waiting for the first reading.' });
    }

    function sourceCard() {
        const source = get('glucose.source');
        const parts = [field('Glucose data source', bindSelect('glucose.source', SOURCES, null, true))];
        if (source === 'nightscout') {
            parts.push(h('div.grid', {},
                field('Nightscout URL', bindInput('glucose.url', { type: 'url', maxlength: '200', placeholder: 'https://my-site.example.com', spellcheck: 'false' }, (v) => v.trim().replace(/\/+$/, '')),
                    'The site\'s address, with http:// or https://.'),
                field('API secret', secretInput('glucose.apiSecret', get('glucose.apiSecretSet')),
                    'Only for a site that does not let anyone read it. The clock keeps its SHA-1, never the secret.')));
        } else if (source === 'dexcom') {
            parts.push(h('p.help', { text: 'Use the same login you use for the Dexcom app on your phone. Dexcom Share must be on, with at least one follower.' }),
                h('div.grid', {},
                    field('Dexcom username', bindInput('glucose.dexcomUsername', { type: 'text', autocomplete: 'username', maxlength: '128', spellcheck: 'false' })),
                    field('Dexcom password', secretInput('glucose.dexcomPassword', get('glucose.dexcomPasswordSet'), 'current-password')),
                    field('Dexcom server', bindSelect('glucose.dexcomServer', DEXCOM_SERVERS))));
        } else if (source === 'librelinkup') {
            const patients = (diag.glucose && diag.glucose.patients) || [];
            const picker = patients.length > 1
                ? field('Select patient', bindSelect('glucose.librePatientId', [['', 'Choose…'], ...patients.map((p) => [p.id, p.name || p.id])]),
                    'This account follows more than one person.')
                : null;
            parts.push(h('p.help', { text: 'The LibreLinkUp follower login - not the LibreLink app\'s own.' }),
                h('div.grid', {},
                    field('LibreLink Up email', bindInput('glucose.libreEmail', { type: 'email', autocomplete: 'username', maxlength: '128' })),
                    field('LibreLink Up password', secretInput('glucose.librePassword', get('glucose.librePasswordSet'), 'current-password')),
                    field('LibreLink Up server', bindSelect('glucose.libreRegion', LLU_REGIONS))),
                picker);
        } else if (source === 'medtrum') {
            parts.push(h('div.grid', {},
                field('Medtrum email', bindInput('glucose.medtrumEmail', { type: 'email', autocomplete: 'username', maxlength: '128' })),
                field('Medtrum password', secretInput('glucose.medtrumPassword', get('glucose.medtrumPasswordSet'), 'current-password'))));
        }
        parts.push(sourceStatus());
        return card('Glucose data source', null, ...parts);
    }

    function updatesCard() {
        return card('Glucose-related settings', null,
            field('Ask for a new reading every', bindSelect('glucose.pollSeconds', [[30, '30 seconds'], [60, '1 minute'], [120, '2 minutes'], [300, '5 minutes']], Number),
                'Dexcom, LibreLinkUp and Medtrum are asked at most once a minute whatever this says, so the account is never locked for asking too often.'),
            toggleRow('Keep the glucose display on screen',
                'Turning the knob moves between faces instead of apps. The middle button still leaves, and the clock comes back to glucose on its own.',
                bindInput('glucose.pinned', { type: 'checkbox' })),
            h('div.notice', {}, h('b', { text: 'Colours: ' }),
                h('span.band-green', { text: 'green 70–180' }), ', ',
                h('span.band-amber', { text: 'yellow 56–69 and 181–249' }), ', ',
                h('span.band-red', { text: 'red 55 and under, 250 and over' }), ', ',
                h('span.band-gray', { text: 'grey once 20 minutes old' }), '. Fixed, to match the other renderers.'));
    }

    function glucoseTab() { return [sourceCard(), updatesCard()]; }

    // --- Alarms tab ---------------------------------------------------------------------

    function presetFor(key, melody) {
        if (melody === DEFAULT_MELODY[key]) { return 'default'; }
        const found = SOUNDS.find((s) => s[2] === melody);
        return found ? found[0] : 'custom';
    }

    function windowsEditor(key) {
        const path = 'glucose.alarms.' + key + '.windows';
        const windows = clone(get(path) || []);
        const commit = () => {
            const complete = windows.every((w) => w.days && w.from && w.to && w.from !== w.to);
            if (complete) { set(path, clone(windows)); }
        };
        const list = h('div.windows', {}, windows.map((w, i) => {
            const days = h('div.days', {}, DAYS.map((letter, d) => h('button', {
                type: 'button', title: DAY_NAMES[d], text: letter, 'aria-pressed': w.days.includes(String(d)) ? 'true' : 'false',
                onclick: (event) => {
                    const on = w.days.includes(String(d));
                    w.days = (on ? w.days.replace(String(d), '') : w.days + d).split('').sort().join('');
                    event.currentTarget.setAttribute('aria-pressed', on ? 'false' : 'true');
                    commit();
                },
            })));
            const from = h('input', { type: 'time', value: w.from, 'aria-label': 'From' });
            from.addEventListener('change', () => { w.from = from.value; commit(); });
            const to = h('input', { type: 'time', value: w.to, 'aria-label': 'To' });
            to.addEventListener('change', () => { w.to = to.value; commit(); });
            return h('div.window', {}, h('div', {}, days, h('div.times', {}, from, '–', to)),
                h('button.btn.icon', { type: 'button', 'aria-label': 'Remove window', text: '×', onclick: () => { windows.splice(i, 1); set(path, clone(windows), true); } }));
        }));
        return h('div.field', {}, h('span.label', { text: 'When it may sound' }), list,
            h('div.row', {}, h('button.btn.sm', { type: 'button', text: 'Add a time window', disabled: windows.length >= 8, onclick: () => { windows.push({ days: '0123456', from: '22:00', to: '07:00' }); set(path, clone(windows), true); } })),
            h('p.help', { text: windows.length ? 'Only inside one of these windows. A window that ends before it starts runs past midnight.' : 'Any time. Add a window to keep it to certain hours.' }),
            key === 'urgentLow' && windows.length ? h('p.notice.warn', { text: 'A window here can keep an urgent low quiet. Think hard before limiting it.' }) : null);
    }

    function alarmCard(alarm) {
        const k = alarm.key;
        const base = 'glucose.alarms.' + k;
        const enabled = bindInput(base + '.enabled', { type: 'checkbox', 'data-rerender': '1' });
        const melodyPath = base + '.melody';
        const melody = h('input', { type: 'text', maxlength: '256', spellcheck: 'false', autocomplete: 'off', value: get(melodyPath) || '' });
        const preset = h('select', { 'aria-label': 'Sound' },
            h('option', { value: 'default', text: 'Default for this alarm' }),
            SOUNDS.map(([v, l]) => h('option', { value: v, text: l })),
            h('option', { value: 'custom', text: 'Custom (RTTTL)' }));
        preset.value = presetFor(k, get(melodyPath));
        melody.hidden = preset.value !== 'custom';
        preset.addEventListener('change', () => {
            if (preset.value === 'custom') { melody.hidden = false; melody.focus(); return; }
            const text = preset.value === 'default' ? DEFAULT_MELODY[k] : SOUNDS.find((s) => s[0] === preset.value)[2];
            melody.value = text;
            melody.hidden = true;
            set(melodyPath, text);
        });
        melody.addEventListener('change', () => set(melodyPath, melody.value.trim()));
        const tryIt = h('button.btn', {
            type: 'button', text: 'Try',
            onclick: async () => {
                try { await send('POST', '/api/v1/glucose/alarm/test', { melody: get(melodyPath) }); }
                catch (e) { toast(e.message, true); }
            },
        });
        const snooze = bindSelect(base + '.snoozeMinutes',
            [[5, '5 minutes'], [10, '10 minutes'], [15, '15 minutes'], [30, '30 minutes'], [60, '1 hour'], [120, '2 hours'], [0, alarm.until]], Number);
        const threshold = alarm.threshold
            ? field(alarm.threshold, bindInput(base + '.mgdl', { type: 'number', min: '30', max: '399', step: '1' }, Number))
            : field('After no reading for', bindSelect('glucose.alarms.noData.minutes', [[20, '20 minutes'], [30, '30 minutes'], [45, '45 minutes'], [60, '1 hour']], Number));
        const on = !!get(base + '.enabled');
        return h('section.card.alarm', {},
            h('div.switch-row', {}, h('h3', { text: alarm.title + ' alert' }), h('label.switch', {}, enabled, h('span'))),
            on ? h('fieldset.stack', {},
                h('div.grid', {}, threshold, field('Snooze for', snooze, 'Press the knob while it sounds.')),
                h('div.field', {}, h('span.label', { text: 'Sound' }), h('div.row.melody-row', {}, preset, tryIt), melody),
                windowsEditor(k)) : null);
    }

    function alarmsTab() {
        const out = [];
        if (diag.alarm && diag.alarm.speaker === false) {
            out.push(h('p.notice.bad', { text: 'This clock has no working speaker, so no alarm can sound.' }));
        }
        out.push(h('p.notice.info', { text: 'Like the TC001 clocks: each alert has its own threshold, snooze and sound. The panel switches to the reading while an alert sounds, and a stale reading silences the glucose alerts - the no-data alert is the net under that.' }));
        for (const alarm of ALARMS) { out.push(alarmCard(alarm)); }
        const volume = bindInput('glucose.alarms.volumePercent', { type: 'range', min: '20', max: '100', step: '5' }, Number);
        const shown = h('output', { text: String(get('glucose.alarms.volumePercent')) + '%' });
        volume.addEventListener('input', () => { shown.textContent = volume.value + '%'; });
        out.push(card('Repeat', null,
            field('Repeat every', bindSelect('glucose.alarms.repeatSeconds', [[60, '1 minute'], [120, '2 minutes'], [300, '5 minutes']], Number)),
            toggleRow('Intensive', 'Repeat two seconds after the sound ends, until snoozed.', bindInput('glucose.alarms.intensive', { type: 'checkbox' })),
            h('div.field', {}, h('span.label', { text: 'Alert volume' }), h('div.range-row', {}, h('span.small.muted', { text: 'Quiet' }), volume, shown),
                h('p.help', { text: 'The alerts\' own level. The clock\'s - and + never make an alert quieter; to silence one, turn it off.' }))));
        return out;
    }

    // --- WiFi & system tab -------------------------------------------------------------

    function wifiCard() {
        const rows = [];
        rows.push(h('dl.kv', {},
            h('dt', { text: 'Connected' }), h('dd', { text: network.connected ? 'yes' : 'no' }),
            h('dt', { text: 'Address' }), h('dd', { text: network.ipv4 || '-' }),
            network.ssid ? h('dt', { text: 'Network' }) : null, network.ssid ? h('dd', { text: network.ssid }) : null));
        if (network.canJoin) {
            const pick = h('select', { 'aria-label': 'Network' },
                h('option', { value: '', text: (network.networks || []).length ? 'Choose a network…' : 'Scan to list networks' }),
                (network.networks || []).map((n) => h('option', { value: n.ssid, text: n.ssid + (n.secured ? '' : ' (open)') + ' ' + n.signalDbm + ' dBm' })));
            const password = h('input', { type: 'password', autocomplete: 'new-password', maxlength: '63', placeholder: 'Password' });
            rows.push(h('div.row', {},
                h('button.btn', { type: 'button', text: 'Scan', disabled: !network.canScan, onclick: async () => {
                    try { await send('POST', '/api/v1/network/scan'); toast('Scanning…'); setTimeout(refreshNetwork, 4000); }
                    catch (e) { toast(e.message, true); } } })));
            rows.push(h('div.grid', {}, field('Network', pick), field('Password', password)));
            rows.push(h('div.row', {}, h('button.btn.primary', { type: 'button', text: 'Join', onclick: async () => {
                if (!pick.value) { toast('Choose a network first', true); return; }
                try {
                    await send('POST', '/api/v1/network/join', { ssid: pick.value, password: password.value });
                    toast('Joining ' + pick.value + ' - the clock may move to a new address');
                } catch (e) { toast(e.message, true); }
            } })));
            if (network.join) {
                rows.push(h('p.notice.' + (network.join.stage === 'failed' ? 'bad' : network.join.stage === 'succeeded' ? 'ok' : 'info'),
                    { text: network.join.ssid + ': ' + network.join.detail }));
            }
        }
        rows.push(h('p.help', { text: 'If the clock cannot reach any network it opens its own, called Stipple-setup: join it and open http://192.168.4.1/. Holding the knob for five seconds does that on purpose.' }));
        return card('Wireless network', null, ...rows);
    }

    function extraWifiCard() {
        const remembered = network.remembered || [];
        const ssid = h('input', { type: 'text', maxlength: '32', spellcheck: 'false', autocomplete: 'off', placeholder: 'Network name' });
        const password = h('input', { type: 'password', maxlength: '63', autocomplete: 'new-password', placeholder: 'Password' });
        const list = remembered.map((n) => h('div.net-row', {},
            h('span', {}, h('b', { text: n.ssid }), n.current ? h('span.small.muted', { text: ' - in use' }) : null),
            n.current ? null : h('button.btn.sm', { type: 'button', text: 'Forget', onclick: async () => {
                try { await send('POST', '/api/v1/network/forget', { ssid: n.ssid }); toast('Forgotten'); refreshNetwork(); }
                catch (e) { toast(e.message, true); } } })));
        return card('Additional WiFi network',
            'Networks the clock joins on its own when the one in use is gone - grandparents\', a phone hotspot. Adding one does not leave the network the clock is on.',
            remembered.length ? h('div.rows', {}, list) : null,
            network.canRemember
                ? h('div.stack', {}, h('div.grid', {}, field('Network name', ssid), field('Password', password)),
                    h('div.row', {}, h('button.btn', { type: 'button', text: 'Add network', onclick: async () => {
                        if (!ssid.value.trim()) { toast('Type the network name', true); return; }
                        try {
                            await send('POST', '/api/v1/network/remember', { ssid: ssid.value.trim(), password: password.value });
                            toast('Added ' + ssid.value.trim());
                            refreshNetwork();
                        } catch (e) { toast(e.message, true); }
                    } })))
                : h('p.help', { text: 'Available once the clock is connected to a network.' }));
    }

    function systemTab() {
        return [
            wifiCard(),
            extraWifiCard(),
            card('Device name', null, field('Name', bindInput('deviceName', { type: 'text', maxlength: '64' }), 'Shown at the top of this page and in diagnostics.')),
            card('Web interface authentication', 'Anyone on the network can open this page unless a password is set.',
                h('div.grid', {},
                    field('Username', bindInput('web.username', { type: 'text', maxlength: '64', autocomplete: 'off' })),
                    field('Password', secretInput('web.password', get('web.passwordSet'))))),
            card('Version', null,
                h('dl.kv', {},
                    h('dt', { text: 'Firmware' }), h('dd', { text: (device.version || '?') + ' (' + (device.platform || '?') + ')' }),
                    h('dt', { text: 'Up for' }), h('dd', { text: Math.round((health.uptimeMillis || 0) / 60000) + ' min' })),
                h('div.row', {},
                    h('a.btn', { href: '/advanced.html', text: 'Advanced settings' }),
                    h('button.btn', { type: 'button', text: 'Restart the clock', onclick: async () => {
                        if (!window.confirm('Restart the clock now?')) { return; }
                        try { await send('POST', '/api/v1/system/reboot'); toast('Restarting…'); } catch (e) { toast(e.message, true); }
                    } })),
                h('p.help', { text: 'Advanced has firmware updates, backup and restore, MQTT, scripts and the log.' })),
        ];
    }

    // --- header ---------------------------------------------------------------------------

    function pill(id, state, text) {
        const node = $(id);
        node.querySelector('.dot').className = 'dot ' + state;
        node.querySelector('b').textContent = text;
    }

    function updateHeader() {
        $('device_name').textContent = (server && server.deviceName) || 'Glucose clock';
        $('device_sub').textContent = 'v' + (device.version || '?') + ' · ' + (network.ipv4 || location.hostname) + ' · online';
        pill('pill_wifi', network.connected ? 'ok' : 'warn', network.connected ? 'Connected' : 'Setup mode');
        pill('pill_time', health.wallClockValid ? 'ok' : 'warn', health.wallClockValid ? 'Set' : 'Not yet');
        const g = diag.glucose || {};
        const sourceLabel = (SOURCES.find((s) => s[0] === g.source) || ['', 'Source'])[1];
        if (!g.configured) { pill('pill_source', 'warn', 'Not set up'); }
        else if (g.lastFailure) { pill('pill_source', 'bad', g.lastFailure); }
        else { pill('pill_source', g.lastSuccessAgeSeconds >= 0 ? 'ok' : 'info', sourceLabel); }
        const r = g.reading || {};
        if (!r.present) { pill('pill_reading', 'warn', 'None'); }
        else if (r.stale) { pill('pill_reading', 'warn', r.minutesAgo + ' min old'); }
        else {
            const state = r.sgv <= 55 || r.sgv >= 250 ? 'bad' : r.sgv < 70 || r.sgv > 180 ? 'warn' : 'ok';
            pill('pill_reading', state, r.sgv + ' mg/dL ' + (ARROWS[r.trend] || '') + (r.minutesAgo ? ' · ' + r.minutesAgo + 'm' : ''));
        }
    }

    // --- tabs and rendering ---------------------------------------------------------------

    const TABS = { display: displayTab, glucose: glucoseTab, alarms: alarmsTab, system: systemTab };
    let currentTab = 'display';
    try { const saved = sessionStorage.getItem('tab'); if (TABS[saved]) { currentTab = saved; } } catch (e) { /* private mode */ }

    function render() {
        if (!draft) { return; }
        const scroll = window.scrollY;
        for (const name of Object.keys(TABS)) {
            const panel = $('tab_' + name);
            panel.hidden = name !== currentTab;
            if (name === currentTab) { panel.replaceChildren(...TABS[name]().filter(Boolean)); }
        }
        document.querySelectorAll('[data-tab]').forEach((button) => {
            button.setAttribute('aria-selected', button.dataset.tab === currentTab ? 'true' : 'false');
        });
        updateHeader();
        window.scrollTo(0, scroll);
    }

    function showTab(name) {
        currentTab = name;
        try { sessionStorage.setItem('tab', name); } catch (e) { /* private mode */ }
        render();
        window.scrollTo(0, 0);
    }

    // --- loading, refreshing, saving -------------------------------------------------------

    async function refreshNetwork() {
        try { network = await send('GET', '/api/v1/network'); } catch (e) { /* keep the last */ }
        if (currentTab === 'system') { render(); } else { updateHeader(); }
    }

    async function refreshStatus() {
        try {
            [diag, health] = await Promise.all([send('GET', '/api/v1/diagnostics'), send('GET', '/api/v1/health')]);
            updateHeader();
        } catch (e) {
            $('device_sub').textContent = 'not answering';
        }
    }

    async function load() {
        $('loading_text').textContent = 'Loading settings from the clock…';
        $('loading_retry').hidden = true;
        try {
            const [settings, deviceInfo, appList] = await Promise.all([
                send('GET', '/api/v1/settings'), send('GET', '/api/v1/device'), send('GET', '/api/v1/apps')]);
            server = settings;
            draft = clone(settings);
            device = deviceInfo || {};
            apps = (appList && appList.apps) || [];
            await Promise.all([refreshStatus(), refreshNetwork()]);
            $('loading_screen').hidden = true;
            $('app').hidden = false;
            document.body.dataset.state = 'ready';
            render();
        } catch (e) {
            $('loading_text').textContent = 'Could not reach the clock: ' + e.message;
            $('loading_retry').hidden = false;
        }
    }

    function buildPatch() {
        const patch = {};
        for (const path of dirty) { setIn(patch, path, get(path)); }
        for (const [path, value] of Object.entries(secrets)) { setIn(patch, path, value); }
        // The block's rules are cross-field: send the faces block together.
        if (patch.glucose && (patch.glucose.faces || patch.glucose.face !== undefined ||
            patch.glucose.cycleSeconds !== undefined || patch.glucose.schedule)) {
            patch.glucose.face = get('glucose.face');
            patch.glucose.faces = get('glucose.faces');
            patch.glucose.cycleSeconds = get('glucose.cycleSeconds');
            patch.glucose.schedule = get('glucose.schedule');
        }
        return patch;
    }

    async function save() {
        const button = $('save');
        button.disabled = true;
        try {
            if (dirty.size || Object.keys(secrets).length) {
                server = await send('PATCH', '/api/v1/settings', buildPatch());
            }
            for (const [id, enabled] of Object.entries(appChanges)) {
                await send('PATCH', '/api/v1/apps/' + encodeURIComponent(id), { enabled });
                delete appChanges[id];
            }
            const appList = await send('GET', '/api/v1/apps');
            apps = (appList && appList.apps) || apps;
            draft = clone(server);
            dirty.clear();
            for (const key of Object.keys(secrets)) { delete secrets[key]; }
            updateSavebar();
            render();
            toast('Saved');
            setTimeout(refreshStatus, 3000);
        } catch (e) {
            toast(e.message, true);
        } finally {
            button.disabled = false;
        }
    }

    function discard() {
        draft = clone(server);
        dirty.clear();
        for (const key of Object.keys(secrets)) { delete secrets[key]; }
        for (const key of Object.keys(appChanges)) { delete appChanges[key]; }
        updateSavebar();
        render();
    }

    document.querySelectorAll('[data-tab]').forEach((button) => {
        button.addEventListener('click', () => showTab(button.dataset.tab));
    });
    $('save').addEventListener('click', save);
    $('discard').addEventListener('click', discard);
    $('loading_retry').addEventListener('click', load);
    window.addEventListener('beforeunload', (event) => {
        if (dirty.size || Object.keys(secrets).length || Object.keys(appChanges).length) {
            event.preventDefault();
            event.returnValue = '';
        }
    });
    setInterval(() => { if (!document.hidden && draft) { refreshStatus(); } }, 15000);

    load();
})();
