(() => {
    'use strict';
    const dialog = document.querySelector('#onboarding');
    if (!dialog) return;
    const storageKey = 'biomes-onboarding-completed-v1';
    const $ = selector => dialog.querySelector(selector);
    // Static authored content only; never insert profile data into this markup.
    const slides = [
        { number:'one', title:'Launch your <em>biome.</em><br>Trigger your routine.', subtitle:'Welcome to <em>biomes</em>',
          description:'Your intelligent workspace orchestrator for Windows.',
          illustration:'<div class="onboarding-banner"><img src="images/download (11).jpg" alt=""><span>Welcome to <em>biomes</em></span></div>' },
        { number:'two', title:'Organize your <em>windows.</em><br>Trigger your routine.', subtitle:'What is <em>biomes?</em>',
          description:'biomes groups your apps into custom workspaces linked to instant hotkeys. It turns workspace setup into a simple habit signal, so you can jump straight into deep work.',
          illustration:'<div class="onboarding-grid"><i></i><i></i><i></i><i></i></div>' },
        { number:'three', title:'One key to<br>start the <em>habit.</em>', subtitle:'The hotkey <em>signal</em>',
          description:'Press your hotkey to launch and snap your workspace into place. No manual dragging—just a simpler way to focus.',
          illustration:'<div class="onboarding-shortcuts"><figure><img src="images/download (1).jpg" alt=""><figcaption>Ctrl + Alt + C</figcaption></figure><figure><img src="images/clouds sky.jpg" alt=""><figcaption>Ctrl + Alt + Y</figcaption></figure><figure><img src="images/download (14).jpg" alt=""><figcaption>Ctrl + Alt + O</figcaption></figure></div>' },
        { number:'four', title:'A quick note on<br><em>heavy software.</em>', subtitle:'Let Windows handle the load.',
          description:'Heavy tools like Adobe apps, Blender, and large IDEs can take a little longer to launch. Give them time to open and snap. If an app asks you to choose a project, make your selection first.',
          illustration:'<svg class="onboarding-caution" viewBox="0 0 32 32"><path d="M13.3 5.5a3.1 3.1 0 0 1 5.4 0l10 17.5a3.1 3.1 0 0 1-2.7 4.7H6a3.1 3.1 0 0 1-2.7-4.7Z"/><path d="M16 12v7m0 4v.1"/></svg>' },
        { number:'five', title:'Privacy &amp;<br><em>community.</em>', subtitle:'Your data stays on your machine. <em>Always.</em>',
          description:'Your layouts and hotkeys stay on your device. No workspace telemetry. Optional links use external services; newsletter signup sends only your email and form fields to MailerLite.',
          illustration:'<div class="onboarding-banner"><img src="images/download (11).jpg" alt=""><span>Privacy <em>First</em></span></div>' }
    ];
    let current = 0;
    let previousFocus = null;
    let motion;
    let navigationReadyAt = 0;
    let completedThisSession = false;
    const dots = $('.onboarding-dots');
    slides.forEach((slide, index) => {
        const button = document.createElement('button');
        button.type = 'button';
        button.setAttribute('aria-label', `Card ${index + 1} of ${slides.length}`);
        button.addEventListener('click', () => navigate(index));
        dots.append(button);
    });
    function render(index, focus = false) {
        current = Math.max(0, Math.min(slides.length - 1, index));
        const slide = slides[current];
        $('#onboarding-title').innerHTML = slide.title;
        $('#onboarding-subtitle').innerHTML = slide.subtitle;
        $('#onboarding-description').textContent = slide.description;
        $('.onboarding-illustration').innerHTML = slide.illustration;
        $('.onboarding-photo').src = slide.number === 'one' ? 'cardsimages/card image one edited.png' : `cardsimages/card image ${slide.number}.jpg`;
        $('.onboarding-photo').dataset.card = slide.number;
        $('.onboarding-back').disabled = current === 0;
        $('.onboarding-next-label').textContent = current === slides.length - 1 ? 'Get started' : 'Next';
        [...dots.children].forEach((button, index) => {
            if (index === current) button.setAttribute('aria-current', 'step');
            else button.removeAttribute('aria-current');
        });
        $('.onboarding-announcement').textContent = `Card ${current + 1} of ${slides.length}`;
        $('.onboarding-count').textContent = `${current + 1} of ${slides.length}`;
        $('.onboarding-copy').scrollTop = 0;
        motion?.cancel();
        if (dialog.open && !matchMedia('(prefers-reduced-motion: reduce)').matches) {
            motion = $('.onboarding-slide').animate([{opacity:0, transform:'translateY(6px)'}, {opacity:1, transform:'none'}],
                {duration:240, easing:'cubic-bezier(.2,.8,.2,1)'});
        }
        if (focus) $('#onboarding-title').focus({preventScroll:true});
    }
    function open() {
        if (dialog.open) return;
        navigationReadyAt = 0;
        previousFocus = document.activeElement;
        render(0);
        dialog.showModal();
        $('#onboarding-title').focus({preventScroll:true});
    }
    function finish() {
        completedThisSession = true;
        try { localStorage.setItem(storageKey, 'true'); } catch {
            // Storage-disabled previews still remain usable; do not trap users.
        }
        dialog.close();
    }
    function navigate(index) {
        if (performance.now() < navigationReadyAt) return;
        navigationReadyAt = performance.now() + 300;
        if (index >= slides.length) finish();
        else render(index, true);
    }
    $('.onboarding-next').addEventListener('click', () => navigate(current + 1));
    $('.onboarding-back').addEventListener('click', () => navigate(current - 1));
    $('.onboarding-skip').addEventListener('click', finish);
    dialog.addEventListener('keydown', event => {
        if (event.altKey || event.ctrlKey || event.metaKey || event.shiftKey) return;
        if (event.key === 'ArrowRight' || event.key === 'ArrowLeft') {
            event.preventDefault();
            if (!event.repeat) navigate(Math.min(slides.length - 1, current + (event.key === 'ArrowRight' ? 1 : -1)));
        }
    });
    // Escape dismisses this visit without marking the introduction completed.
    dialog.addEventListener('close', () => {
        motion?.cancel();
        const focusTarget = previousFocus?.isConnected && !previousFocus.closest('dialog:not([open])')
            ? previousFocus : document.querySelector('#page-title');
        focusTarget?.focus({preventScroll:true});
    });
    document.querySelector('#replay-onboarding')?.addEventListener('click', () => {
        document.querySelector('.creation-guide')?.close(); open();
    });
    let completed = false;
    try { completed = localStorage.getItem(storageKey) === 'true'; } catch { /* Show once this visit. */ }
    // Query opt-in supports repeatable previews without clearing user settings.
    const preview = new URLSearchParams(location.search).get('onboarding') === '1';
    if (preview || (!completed && !completedThisSession)) open();
})();
