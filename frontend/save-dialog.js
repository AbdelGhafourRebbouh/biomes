(() => {
    const dialog = document.querySelector('#save-dialog');
    const form = document.querySelector('#save-form');
    const name = document.querySelector('#biome-name');
    const recorder = document.querySelector('#shortcut-record');
    const shortcutHelp = document.querySelector('#shortcut-help');
    const error = document.querySelector('#save-error');
    const progress = document.querySelector('#save-progress');
    const file = document.querySelector('#cover-file');
    const image = document.querySelector('#save-image-preview');
    const placeholder = document.querySelector('.layout-placeholder');
    const removeCover = document.querySelector('#remove-cover');
    const submit = document.querySelector('#save-submit');
    const picker = document.querySelector('#cover-picker');
    const coverStatus = document.querySelector('#cover-status');
    const defaultCovers = [
        'images/khgfh.jpg',
        'images/image for the banner1.jpg',
        'images/image for the banner.jpg',
        'images/adsfwer.jpg',
        "images/'lkjhgfddwerw.jpg"
    ];
    const updateCoverSelection = () => {
        document.querySelectorAll('.default-cover').forEach(button =>
            button.setAttribute('aria-pressed', String(button.dataset.cover === draft?.coverImagePath)));
    };
    const host = window.chrome?.webview;
    let draft = null;
    let recording = false;
    let readingImage = false;
    let imageVersion = 0;
    let pending = false;
    let saveTimer;
    let saved = [];
    let repairContext = null;
    let activeWindows = [];
    document.addEventListener('biomes:repair-context', event => { repairContext = event.detail; });
    const setPending = (value) => {
        pending = value;
        form.querySelectorAll('input,button,select').forEach(control => {
            if (!['save-close', 'save-cancel'].includes(control.id)) control.disabled = value;
        });
    };
    const label = (value) => String(value || '').split(/[\\/]/).pop();
    const normalize = (value) => String(value || '').toUpperCase().replace(/\s/g, '');
    const stopRecording = () => {
        if (recording) host?.postMessage({action:'HOTKEY_RECORDING', recording:false});
        recording = false;
        recorder.setAttribute('aria-pressed', 'false');
        recorder.textContent = draft?.hotkey ? draft.hotkey.split('+').join(' + ') : 'Press to record a shortcut';
    };
    const chooseShortcut = (value) => {
        if (!draft || pending) return;
        if (value && saved.some((biome) => biome.id !== draft.id && normalize(biome.hotkey) === normalize(value))) {
            shortcutHelp.textContent = 'Another biome already uses this shortcut. Choose a different one.';
            return;
        }
        draft.hotkey = value;
        stopRecording();
        shortcutHelp.textContent = value ? 'Shortcut selected. You can change it at any time before saving.' : 'No shortcut selected. You can launch from the home card.';
    };
    const clearImage = () => {
        imageVersion++;
        readingImage = false;
        if (draft) draft.coverImagePath = '';
        image.removeAttribute('src');
        image.hidden = true;
        placeholder.hidden = false;
        removeCover.hidden = true;
        file.value = '';
        coverStatus.textContent = '';
        updateCoverSelection();
    };
    function renderZones() {
        const list = document.querySelector('#save-zone-list');
        list.replaceChildren();
        draft.boxes.forEach((box, index) => {
            const item = document.createElement('li');
            const select = document.createElement('select');
            select.setAttribute('aria-label', 'Application for zone ' + (index + 1));
            const current = document.createElement('option');
            current.value = '';
            current.textContent = label(box.exeName || box.assignedApp || box.aumid) || 'No app assigned';
            select.append(current);
            activeWindows.forEach((app, appIndex) => {
                const option = document.createElement('option');
                option.value = String(appIndex);
                option.textContent = (app.title || app.process) + ' — ' + label(app.path);
                select.append(option);
            });
            select.addEventListener('change', () => {
                if (select.value === '' || pending) return;
                const app = activeWindows[Number(select.value)];
                Object.assign(box, {assignedApp:app.path, exeName:app.process, aumid:app.aumid || '', titleHint:app.title || '', launchUri:''});
                renderZones();
            });
            item.append('Screen ' + (Number(box.monitorIndex || 0) + 1) + ' · zone ' + (index + 1), select);
            list.append(item);
        });
    }
    function openDraft(boxes) {
        if (pending) return;
        const context = repairContext;
        repairContext = null;
        draft = { id: context?.id || crypto.randomUUID(), boxes: boxes.map(box => ({ ...box })), hotkey: context?.hotkey || '', coverImagePath: '' };
        form.reset();
        clearImage();
        if (picker.open) picker.close();
        stopRecording();
        error.textContent = '';
        progress.textContent = '';
        submit.disabled = false;
        submit.textContent = 'Save biome';
        name.value = context?.name || '';
        if (context?.cover) {
            draft.coverImagePath=context.cover; image.src=context.cover; image.hidden=false; placeholder.hidden=true; removeCover.hidden=false;
        }
        document.querySelector('#save-preview-note').textContent = 'Saved locally on this device.';
        shortcutHelp.textContent = 'Choose a suggestion or record Ctrl + Alt + a letter.';
        activeWindows = [];
        renderZones();
        host?.postMessage({action:'GET_ACTIVE_WINDOWS'});
        document.querySelector('#zone-count').textContent = '(' + boxes.length + ' zones)';
        if (!dialog.open) dialog.showModal();
        name.focus();
    }
    document.addEventListener('biomes:resume-or-create', () => {
        if (draft) { if (!dialog.open) dialog.showModal(); return; }
        repairContext=null;
        document.dispatchEvent(new CustomEvent('biomes:create-overlay'));
    });
    const close = () => {
        stopRecording();
        // Keep the current draft in memory until it is saved or replaced.
        dialog.close();
    };
    document.querySelector('#save-close').addEventListener('click', close);
    document.querySelector('#save-cancel').addEventListener('click', close);
    dialog.addEventListener('cancel', (event) => {
        if (recording) {
            event.preventDefault();
            if (recording) stopRecording();
        }
    });
    recorder.addEventListener('click', () => {
        if (pending) return;
        if (recording) { stopRecording(); return; }
        recording = true;
        recorder.setAttribute('aria-pressed', 'true');
        host?.postMessage({action:'HOTKEY_RECORDING', recording:true});
        recorder.textContent = 'Press your shortcut…';
        shortcutHelp.textContent = 'Use Ctrl + Alt or Ctrl + Shift with a letter or number. Escape cancels; Tab moves on.';
    });
    recorder.addEventListener('blur', stopRecording);
    recorder.addEventListener('keydown', (event) => {
        if (!recording) return;
        if (event.key === 'Tab') { stopRecording(); return; }
        event.preventDefault();
        event.stopPropagation();
        if (event.key === 'Escape') { stopRecording(); return; }
        if (event.repeat || ['Control', 'Shift', 'Alt', 'Meta'].includes(event.key)) return;
        const key = event.key.toUpperCase();
        if (event.metaKey || event.getModifierState('AltGraph') || !event.ctrlKey || !(event.altKey || event.shiftKey) || !/^[A-Z0-9]$/.test(key)) {
            shortcutHelp.textContent = 'Choose Ctrl + Alt or Ctrl + Shift plus one letter or number. Windows shortcuts and AltGr are not supported.';
            return;
        }
        if (event.ctrlKey && event.shiftKey && ['T', 'W', 'N', 'Q', 'I', 'J', 'C'].includes(key) && !event.altKey) {
            shortcutHelp.textContent = 'That combination is commonly used by browsers. Try Ctrl + Alt plus a letter.';
            return;
        }
        chooseShortcut(['CTRL', ...(event.altKey ? ['ALT'] : []), ...(event.shiftKey ? ['SHIFT'] : []), key].join('+'));
    });
    document.querySelectorAll('[data-shortcut]').forEach(button => button.addEventListener('click', () => chooseShortcut(button.dataset.shortcut)));
    document.querySelector('#clear-shortcut').addEventListener('click', () => chooseShortcut(''));
    document.querySelector('#choose-cover').addEventListener('click', () => {
        if (pending) return;
        if (!picker.open) picker.showModal();
    });
    document.querySelector('#upload-cover').addEventListener('click', () => { if (!pending) file.click(); });
    defaultCovers.forEach((path, index) => {
        const button = document.createElement('button');
        button.type = 'button';
        button.className = 'default-cover';
        button.dataset.cover = path;
        button.setAttribute('aria-label', 'Choose default cover ' + (index + 1));
        button.setAttribute('aria-pressed', 'false');
        const thumbnail = document.createElement('img');
        thumbnail.src = path;
        thumbnail.alt = '';
        thumbnail.loading = 'lazy';
        button.append(thumbnail);
        document.querySelector('#default-covers').append(button);
        button.addEventListener('click', async () => {
            if (!draft || pending) return;
            const version = ++imageVersion;
            readingImage = true;
            error.textContent = '';
            coverStatus.textContent = 'Loading cover…';
            try {
                const probe = new Image();
                probe.src = path;
                await probe.decode();
                if (version !== imageVersion) return;
                draft.coverImagePath = path;
                image.src = path;
                image.hidden = false;
                placeholder.hidden = true;
                removeCover.hidden = false;
                file.value = '';
                updateCoverSelection();
                coverStatus.textContent = 'Default cover selected.';
            } catch {
                if (version === imageVersion) {
                    error.textContent = 'This default image could not be loaded. Choose another cover.';
                    coverStatus.textContent = error.textContent;
                }
            } finally {
                if (version === imageVersion) readingImage = false;
            }
        });
    });
    removeCover.addEventListener('click', () => { if (!pending) clearImage(); });
    file.addEventListener('change', async () => {
        const selected = file.files[0];
        if (!selected || !draft || pending) return;
        error.textContent = '';
        if (!['image/png', 'image/jpeg', 'image/webp'].includes(selected.type)) {
            error.textContent = 'Choose a JPG, PNG or WebP image.';
            coverStatus.textContent = error.textContent;
            file.value = '';
            return;
        }
        const version = ++imageVersion;
        readingImage = true;
        coverStatus.textContent = 'Preparing your cover…';
        let bitmap;
        try {
            // Decode the file directly instead of creating an oversized base64
            // copy first. Store only a cover-sized derivative; leave the source alone.
            bitmap = await createImageBitmap(selected);
            if (version !== imageVersion) return;
            const scale = Math.min(1, 1600 / Math.max(bitmap.width, bitmap.height));
            const canvas = document.createElement('canvas');
            canvas.width = Math.max(1, Math.round(bitmap.width * scale));
            canvas.height = Math.max(1, Math.round(bitmap.height * scale));
            const context = canvas.getContext('2d');
            if (!context) throw new Error('canvas');
            context.drawImage(bitmap, 0, 0, canvas.width, canvas.height);
            bitmap.close();
            bitmap = null;
            const optimized = await new Promise((resolve, reject) => canvas.toBlob(
                blob => blob ? resolve(blob) : reject(new Error('encode')), 'image/webp', .85));
            const data = await new Promise((resolve, reject) => {
                const reader = new FileReader();
                reader.onload = () => resolve(reader.result);
                reader.onerror = () => reject(new Error('read'));
                reader.readAsDataURL(optimized);
            });
            const probe = new Image();
            probe.src = data;
            await probe.decode();
            if (version !== imageVersion) return;
            draft.coverImagePath = data;
            image.src = data;
            image.hidden = false;
            placeholder.hidden = true;
            removeCover.hidden = false;
            updateCoverSelection();
            coverStatus.textContent = 'Cover ready. Your original file has not been changed.';
        } catch {
            if (version === imageVersion) {
                error.textContent = 'This image could not be processed. Try another image or a smaller export.';
                coverStatus.textContent = error.textContent;
            }
        } finally {
            bitmap?.close();
            if (version === imageVersion) readingImage = false;
            file.value = '';
        }
    });
    function finishSave() {
        clearTimeout(saveTimer);
        setPending(false);
        const result = { ...draft, name: name.value.trim() };
        dialog.close();
        document.dispatchEvent(new CustomEvent('biomes:draft-saved', { detail: result }));
        draft = null;
    }
    form.addEventListener('submit', (event) => {
        event.preventDefault();
        if (!draft || pending) return;
        error.textContent = '';
        if (!name.value.trim()) { error.textContent = 'Give your biome a name.'; name.focus(); return; }
        if (readingImage) { error.textContent = 'Your image is still loading. Try saving in a moment.'; return; }
        if (!draft.boxes.some(box => box.assignedApp || box.aumid)) { error.textContent = 'Assign at least one app on the grid before saving.'; return; }
        if (draft.hotkey && saved.some(biome => biome.id !== draft.id && normalize(biome.hotkey) === normalize(draft.hotkey))) { error.textContent = 'Another biome uses this shortcut. Please choose a different one.'; return; }
        stopRecording();
        if (!host) { error.textContent = 'Open the Windows app to save this layout.'; return; }
        setPending(true);
        progress.textContent = 'Saving your biome…';
        try {
            host.postMessage({ action: 'SAVE_BIOME', id:draft.id, name: name.value.trim(), hotkey: draft.hotkey, coverImagePath: draft.coverImagePath, boxes: draft.boxes });
            saveTimer = setTimeout(() => {
                setPending(false);
                progress.textContent = 'Confirmation is taking longer than expected. You can save again safely.';
            }, 10000);
        } catch {
            setPending(false);
            progress.textContent = '';
            error.textContent = 'Could not send your biome to the app. Your draft is still here.';
        }
    });
    // Native persistence and overlay events.
    host?.addEventListener('message', (event) => {
        let data;
        try { data = typeof event.data === 'string' ? JSON.parse(event.data) : event.data; } catch { return; }
        if (data?.action === 'GRID_LAYOUT_READY' && Array.isArray(data.boxes)) openDraft(data.boxes);
        if (data?.action === 'ACTIVE_WINDOWS_LIST' && draft && Array.isArray(data.windows)) { activeWindows=data.windows; renderZones(); }
        if (data?.action === 'LOADED_BIOMES' && Array.isArray(data.biomes)) saved = data.biomes;
        if (data?.action === 'BIOME_SAVED' && draft && data.id === draft.id) finishSave();
        if (data?.action === 'SAVE_FAILED' && draft) {
            clearTimeout(saveTimer); setPending(false); progress.textContent='';
            error.textContent=data.payload || 'Could not save this biome.';
        }
        if (data?.action === 'STATUS' && pending && typeof data.payload === 'string') {
            const saveErrors = ['A Biome name and layout boxes are required.', 'Could not read saved Biomes.', 'Could not save this Biome.'];
            if (saveErrors.includes(data.payload)) {
                clearTimeout(saveTimer);
                setPending(false);
                progress.textContent = '';
                error.textContent = data.payload.replace(/Biomes?/g, word => word.toLowerCase());
            } else progress.textContent = data.payload;
        }
    });
})();
