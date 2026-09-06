(() => {
    const root = document.documentElement;
    const themeButton = document.querySelector('.theme-toggle');
    const sidebarToggle = document.querySelector('.sidebar-toggle');
    const appShell = document.querySelector('.app-shell');
    const status = document.querySelector('.live-status');
    const host = window.chrome?.webview;
    const send = (action, payload = {}) => {
        if (!host) { announce('Open biomes.exe to use your saved workspaces.'); return false; }
        host.postMessage({ action, ...payload });
        return true;
    };
    let profiles = [];
    let activeId = '';
    let loaded = false;
    let pendingDelete = '';
    // Local-file previews can run with browser storage disabled.
    const readPreference = (key) => {
        try { return window.localStorage.getItem(key); } catch { return null; }
    };
    const savePreference = (key, value) => {
        try { window.localStorage.setItem(key, value); } catch { /* Session-only preference. */ }
    };
    const storedTheme = readPreference('biomes-theme');
    let statusTimeout;
    const announce = (message) => {
        clearTimeout(statusTimeout);
        status.textContent = message;
        statusTimeout = setTimeout(() => { status.textContent = ''; }, 4000);
    };

    const applyTheme = (theme) => {
        root.dataset.theme = theme;
        const dark = theme === 'dark';
        themeButton.setAttribute('aria-pressed', String(dark));
        themeButton.setAttribute('aria-label', `Switch to ${dark ? 'light' : 'dark'} mode`);
    };

    applyTheme(storedTheme === 'dark' ? 'dark' : 'light');
    themeButton.addEventListener('click', () => {
        const nextTheme = root.dataset.theme === 'dark' ? 'light' : 'dark';
        savePreference('biomes-theme', nextTheme);
        applyTheme(nextTheme);
    });

    const storedSidebarState = readPreference('biomes-sidebar-collapsed');
    const applySidebarState = (collapsed) => {
        appShell.classList.toggle('is-sidebar-collapsed', collapsed);
        sidebarToggle.setAttribute('aria-expanded', String(!collapsed));
        sidebarToggle.setAttribute('aria-label', collapsed ? 'Expand navigation' : 'Collapse navigation');
    };
    applySidebarState(window.innerWidth < 600 || storedSidebarState === 'true');
    document.querySelectorAll('.nav-link').forEach((link) => {
        link.setAttribute('title', link.textContent.trim());
    });
    sidebarToggle.addEventListener('click', () => {
        const collapsed = !appShell.classList.contains('is-sidebar-collapsed');
        savePreference('biomes-sidebar-collapsed', String(collapsed));
        applySidebarState(collapsed);
    });

    const pages = {
        insights: {
            title: 'Your insights', accent: 'insights',
            heading: 'A clearer view of your work.',
            description: 'This space is planned for insights into your biomes and workspace habits. It is not available yet.'
        },
        customization: {
            title: 'Your customization', accent: 'customization',
            heading: 'Make room for your way of working.',
            description: 'Choose the number of rows and columns in your grid, change its colors, and personalize the appearance of biomes. These customization options are planned for a future update.'
        },
        developer: {
            title: 'Meet the developer', accent: 'developer'
        },
        privacy: {
            title: 'Your privacy', accent: 'privacy',
            heading: 'Your data stays on your machine. Always.',
            banner: 'Privacy First',
            badge: 'Local & Open Source',
            description: 'Every grid configuration, hotkey, and workspace layout is saved strictly on your device. biomes operates entirely offline with zero cloud tracking, no external servers, and zero telemetry.'
        },
        feedback: {
            title: 'Help shape what comes next', accent: 'comes next'
        }
    };
    const heading = document.querySelector('#page-title');
    const homePage = document.querySelector('#home-page');
    // Clone a non-rendered template so no demo data enters the real dashboard.
    const cardTemplate = document.querySelector('#biome-card-template').content.firstElementChild;
    const emptyPage = document.querySelector('#empty-page');
    const comingPage = document.querySelector('#coming-page');
    const developerPage = document.querySelector('#developer-page');
    const feedbackPage = document.querySelector('#feedback-page');
    const loadingPage = document.querySelector('#loading-page');
    const contentFrame = document.querySelector('.content-frame');
    const renderPage = (moveFocus = false) => {
        const hash = window.location.hash.slice(1);
        // Keep bookmarks to the former navigation names working.
        const requestedPage = hash === 'layout' ? 'customization' : hash === 'support' ? 'developer' : hash;
        const page = Object.hasOwn(pages, requestedPage) ? requestedPage : 'biomes';
        const details = pages[page];
        // Only show the empty state after the native collection has loaded.
        const isEmpty = loaded && profiles.length === 0;
        homePage.hidden = Boolean(details) || isEmpty || !loaded;
        emptyPage.hidden = Boolean(details) || !isEmpty || !loaded;
        comingPage.hidden = !details || page === 'developer' || page === 'feedback';
        developerPage.hidden = page !== 'developer';
        feedbackPage.hidden = page !== 'feedback';
        loadingPage.hidden = Boolean(details) || loaded;
        contentFrame.removeAttribute('aria-busy');
        const accent = document.createElement('span');
        accent.className = 'accent';
        accent.textContent = details ? details.accent : 'biomes';
        const title = details ? details.title : 'Your biomes';
        heading.replaceChildren(title.slice(0, -accent.textContent.length), accent);
        if (details && page !== 'developer' && page !== 'feedback') {
            const bannerImage = document.querySelector('#coming-page .coming-banner img');
            bannerImage.src = page === 'privacy' ? 'images/image for the banner.jpg' : 'assets/coming-later.png';
            bannerImage.classList.toggle('soft-banner-image', page === 'privacy');
            document.querySelector('#feature-title').textContent = details.heading;
            document.querySelector('#feature-description').textContent = details.description;
            const bannerAccent = document.createElement('span');
            bannerAccent.className = 'accent';
            bannerAccent.textContent = details.beforeRelease ? 'release' : 'later';
            document.querySelector('#coming-title').replaceChildren(details.beforeRelease ? 'Before ' : 'Coming ', bannerAccent);
            if (details.banner) {
                const split = details.banner.lastIndexOf(' ');
                bannerAccent.textContent = details.banner.slice(split + 1);
                document.querySelector('#coming-title').replaceChildren(details.banner.slice(0, split + 1), bannerAccent);
            }
            document.querySelector('.coming-kicker').textContent = details.beforeRelease
                ? 'Planned before launch'
                : details.badge || 'A little more biomes, on the way';
            document.querySelector('.coming-kicker').classList.toggle('privacy-badge', page === 'privacy');
        }
        document.querySelectorAll('.nav-link').forEach((link) => {
            const selected = link.hash === '#' + page;
            link.classList.toggle('is-active', selected);
            if (selected) link.setAttribute('aria-current', 'page');
            else link.removeAttribute('aria-current');
        });
        document.title = title + ' — biomes';
        clearTimeout(statusTimeout);
        status.textContent = '';
        if (moveFocus) heading.focus({ preventScroll: true });
    };
    window.addEventListener('hashchange', () => renderPage(true));
    renderPage();

    // Future asynchronous page requests can dispatch this event while pending.
    // The static pages above need no artificial loading delay.
    document.addEventListener('biomes:page-loading', (event) => {
        if (event.detail?.loading) {
            homePage.hidden = true;
            emptyPage.hidden = true;
            comingPage.hidden = true;
            developerPage.hidden = true;
            feedbackPage.hidden = true;
            loadingPage.hidden = false;
            contentFrame.setAttribute('aria-busy', 'true');
        } else {
            renderPage();
        }
    });

    const guide = document.querySelector('.creation-guide');
    document.querySelectorAll('[data-open-guide]').forEach((button) => {
        button.addEventListener('click', () => {
            if (!guide.open) guide.showModal();
        });
    });
    document.querySelector('#create-first-biome').addEventListener('click', () => {
        document.querySelector('.create-card').click();
    });


    document.querySelector('.create-card').addEventListener('click', () => {
        document.dispatchEvent(new CustomEvent('biomes:resume-or-create'));
    });
    document.addEventListener('biomes:create-overlay', () => send('CREATE_NEW_BIOME'));
    document.addEventListener('biomes:draft-saved', () => {
        window.location.hash = 'biomes';
        send('GET_SAVED_BIOMES');
        announce('biome saved.');
    });
    const deleteDialog = document.querySelector('#delete-dialog');
    let deletingId = '';
    let openCardMenu = null;
    function closeCardMenu() {
        if (!openCardMenu) return;
        openCardMenu.querySelector('.card-options').setAttribute('aria-expanded', 'false');
        openCardMenu.querySelector('.card-options-panel').hidden = true;
        openCardMenu = null;
    }
    function renderCards() {
        closeCardMenu();
        homePage.querySelectorAll('.biome-card').forEach(card => card.remove());
        profiles.forEach(profile => {
            const card = cardTemplate.cloneNode(true);
            card.dataset.biomeId = profile.id;
            card.classList.toggle('is-running', profile.id === activeId);
            card.querySelector('h2').textContent = profile.name;
            card.querySelector('.hotkey').textContent = profile.hotkey || 'No shortcut';
            const summary = card.querySelector('.app-summary');
            summary.textContent = (profile.apps || []).join(', ') || 'No apps assigned';
            const appList = document.createElement('div');
            appList.className = 'app-list';
            summary.replaceWith(appList);
            appList.append(summary);
            summary.tabIndex = 0;
            summary.setAttribute('aria-label', 'Applications in ' + profile.name);
            const more = document.createElement('button');
            more.type = 'button'; more.className = 'app-list-more';
            more.textContent = '⌄'; more.setAttribute('aria-label', 'Scroll through applications');
            appList.append(more);
            const updateScrollHint = () => {
                more.hidden = summary.scrollHeight <= summary.clientHeight + 1;
                more.textContent = summary.scrollTop + summary.clientHeight >= summary.scrollHeight - 1 ? '⌃' : '⌄';
            };
            more.addEventListener('click', () => {
                summary.scrollBy({top: summary.scrollTop + summary.clientHeight >= summary.scrollHeight - 1 ? -summary.scrollHeight : summary.clientHeight, behavior:window.matchMedia('(prefers-reduced-motion: reduce)').matches ? 'auto' : 'smooth'});
            });
            summary.addEventListener('scroll', updateScrollHint);
            requestAnimationFrame(updateScrollHint);
            const health = profile.monitorHealth || {};
            const screens = health.openingScreens ?? Math.min(health.required || 1, health.connected ?? 1);
            const badge = card.querySelector('.screen-count');
            badge.querySelector(':scope > span').textContent = 'Opens on ' + screens + (screens === 1 ? ' screen' : ' screens');
            badge.title = 'Saved for ' + (health.required || 1) + ' screen(s). Opens on ' + screens + ' with your current setup.';
            const svg = card.querySelector('.map-icon');
            svg.replaceChildren();
            const count = Math.min(screens, 3);
            for (let i = 0; i < count; i++) {
                const x = (42 - (count * 11 + (count - 1) * 3)) / 2 + i * 14;
                const rect = document.createElementNS('http://www.w3.org/2000/svg', 'rect');
                Object.entries({x,y:4,width:11,height:12,rx:1.5,fill:'none',stroke:'currentColor','stroke-width':1.2}).forEach(([key,value]) => rect.setAttribute(key,value));
                svg.append(rect);
                const stand = document.createElementNS('http://www.w3.org/2000/svg', 'path');
                stand.setAttribute('d', `M${x + 5.5} 16v4m-3 0h6`);
                stand.setAttribute('stroke', 'currentColor');
                stand.setAttribute('stroke-width', '1.2');
                stand.setAttribute('fill', 'none');
                svg.append(stand);
            }
            const run = card.querySelector('.run-button');
            run.replaceChildren(profile.id === activeId ? 'Close biome' : 'Run biome');
            run.addEventListener('click', () => send(profile.id === activeId ? 'CLOSE_BIOME' : 'ACTIVATE_BIOME', { id: profile.id }));
            if (profile.cover) {
                const cover = card.querySelector('.cover');
                const image = document.createElement('img');
                image.className = 'saved-cover-image';
                image.alt = '';
                // Covers are packaged assets, local files or data URLs, never remote resources.
                if (/^(data:image\/|images\/|file:)/i.test(profile.cover) || /^[A-Z]:[\\/]/i.test(profile.cover)) {
                    image.src = profile.cover;
                    image.addEventListener('error', () => image.remove(), { once:true });
                    cover.prepend(image);
                    cover.querySelectorAll('.cover-sun,.cover-ridge').forEach(node => node.remove());
                }
            }
            if (health.missingZones > 0 || health.topologyMatch === false) {
                badge.title += health.missingZones > 0 ? ' Some screens are disconnected; unavailable zones are skipped.' : ' Display setup has changed.';
            }
            badge.setAttribute('aria-label', badge.title);
            addCardActions(card, profile);
            homePage.insertBefore(card, document.querySelector('.create-card'));
        });
        renderPage();
    }
    function addCardActions(card, profile) {
        const toggle = document.createElement('button');
        toggle.type = 'button'; toggle.className = 'run-button card-options'; toggle.textContent = '⋯';
        toggle.setAttribute('aria-label', 'Options for ' + profile.name);
        toggle.setAttribute('aria-expanded', 'false');
        const panel = document.createElement('div');
        panel.className = 'card-options-panel'; panel.hidden = true;
        const repair = document.createElement('button');
        repair.type = 'button'; repair.textContent = 'Adjust layout';
        repair.addEventListener('click', () => {
            closeCardMenu();
            document.dispatchEvent(new CustomEvent('biomes:repair-context', { detail: profile }));
            send('FIX_BIOME_LAYOUT', { id:profile.id });
        });
        const remove = document.createElement('button');
        remove.type = 'button'; remove.textContent = 'Delete biome';
        remove.addEventListener('click', () => {
            closeCardMenu();
            deletingId = profile.id;
            document.querySelector('#delete-description').textContent = '“' + profile.name + '” will be removed from your saved biomes.' + (activeId === profile.id ? ' Its active session will close first.' : '');
            document.querySelector('#confirm-delete').textContent = activeId === profile.id ? 'Close and delete' : 'Delete biome';
            document.querySelector('#delete-error').textContent = '';
            deleteDialog.showModal();
            document.querySelector('#keep-biome').focus();
        });
        panel.append(repair, remove);
        card.querySelector('.cover').append(toggle, panel);
        toggle.addEventListener('click', () => {
            const wasOpen = openCardMenu === card;
            closeCardMenu();
            if (!wasOpen) { panel.hidden=false; toggle.setAttribute('aria-expanded','true'); openCardMenu=card; repair.focus(); }
        });
    }
    document.addEventListener('click', event => {
        if (openCardMenu && !event.target.closest('.card-options,.card-options-panel')) closeCardMenu();
        const link = event.target.closest('a[href^="https://"]');
        if (link && host) { event.preventDefault(); send('OPEN_EXTERNAL', { url:link.href }); }
    });
    document.addEventListener('keydown', event => {
        if (event.key === 'Escape' && openCardMenu) {
            const toggle = openCardMenu.querySelector('.card-options');
            closeCardMenu(); toggle.focus(); event.preventDefault();
        }
    });
    document.querySelector('#keep-biome').addEventListener('click', () => { if (!pendingDelete) deleteDialog.close(); });
    deleteDialog.addEventListener('cancel', event => { if (pendingDelete) event.preventDefault(); });
    document.querySelector('#confirm-delete').addEventListener('click', () => {
        if (!deletingId || pendingDelete) return;
        if (send('DELETE_BIOME', {id:deletingId})) {
            pendingDelete=deletingId;
            document.querySelector('#confirm-delete').disabled=true;
        }
    });
    document.querySelectorAll('.native-control').forEach(button =>
        button.addEventListener('click', () => send('WINDOW_CONTROL', { command:button.dataset.windowAction })));
    const chromeBar = document.querySelector('.window-chrome');
    chromeBar.addEventListener('pointerdown', event => {
        if (event.button === 0 && !event.target.closest('button')) send('WINDOW_CONTROL', { command:'drag' });
    });
    if (host) {
        for (const edge of ['n','s','e','w','nw','ne','sw','se']) {
            const grip = document.createElement('div');
            grip.className = 'window-resize-grip resize-' + edge;
            grip.setAttribute('aria-hidden', 'true');
            grip.addEventListener('pointerdown', event => {
                if (event.button !== 0) return;
                event.preventDefault(); event.stopPropagation();
                send('WINDOW_CONTROL', {command:'resize', edge});
            });
            document.body.append(grip);
        }
    }
    chromeBar.addEventListener('dblclick', event => {
        if (!event.target.closest('button')) send('WINDOW_CONTROL', {command:'maximize'});
    });
    document.querySelector('#retry-load').addEventListener('click', () => send('GET_SAVED_BIOMES'));
    host?.addEventListener('message', event => {
        let data;
        try { data = typeof event.data === 'string' ? JSON.parse(event.data) : event.data; } catch { return; }
        if (data?.action === 'LOADED_BIOMES' && Array.isArray(data.biomes)) {
            profiles=data.biomes; activeId=data.activeId || ''; loaded=true;
            renderCards();
            if (pendingDelete && !profiles.some(profile => profile.id === pendingDelete)) {
                pendingDelete=''; deletingId='';
                document.querySelector('#confirm-delete').disabled=false;
                deleteDialog.close();
                announce('biome deleted.');
            }
        } else if (data?.action === 'LOAD_FAILED') {
            document.querySelector('#loading-message').textContent=data.payload;
            document.querySelector('.loading-blocks').hidden=true;
            announce(data.payload);
        } else if (data?.action === 'ACTIVE_BIOME_CHANGED') {
            activeId=data.id || ''; renderCards();
        } else if (data?.action === 'MONITORS_CHANGED') {
            announce('Display setup changed. Review your biome layouts.');
            send('GET_SAVED_BIOMES');
        } else if (data?.action === 'STATUS' && data.payload) {
            announce(data.payload);
            if (pendingDelete && /could not|not found|missing|failed/i.test(data.payload)) {
                document.querySelector('#delete-error').textContent=data.payload;
                pendingDelete=''; document.querySelector('#confirm-delete').disabled=false;
            }
        }
    });
    window.addEventListener('DOMContentLoaded', () => {
        if (host) send('GET_SAVED_BIOMES');
        else {
            document.querySelector('#loading-message').textContent='Open biomes.exe to load your saved workspaces.';
            document.querySelector('.loading-blocks').hidden=true;
        }
    });
})();
