(() => {
    'use strict';
    const host = window.chrome?.webview;
    const panel = document.querySelector('.panel');
    const $ = selector => panel.querySelector(selector);
    let session = null, state = '', timer, exitTimer, morph, exitMotion;
    const send = action => host?.postMessage({ action, session });
    // Continuous corner curvature: a cubic approximation of a superellipse,
    // scaled with the panel rather than a circular border-radius alone.
    const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
    svg.setAttribute('width', '0'); svg.setAttribute('height', '0');
    svg.innerHTML = '<defs><clipPath id="panel-curve" clipPathUnits="objectBoundingBox"><path d="M .15 0 H .85 C .96 0 1 .04 1 .18 V .82 C 1 .96 .96 1 .85 1 H .15 C .04 1 0 .96 0 .82 V .18 C 0 .04 .04 0 .15 0 Z"/></clipPath></defs>';
    document.body.append(svg);
    const leave = () => {
        clearTimeout(timer); clearTimeout(exitTimer);
        morph?.cancel(); exitMotion?.cancel();
        const height = panel.offsetHeight;
        $('.content').style.height = `${height}px`;
        panel.classList.add('leaving');
        const reduced = matchMedia('(prefers-reduced-motion: reduce)').matches;
        exitMotion = panel.animate([
            {height:`${height}px`, opacity:1, transform:'scaleX(1)', offset:0},
            {height:`${height * 1.04}px`, opacity:1, transform:'scaleX(1.015)', offset:.22},
            {height:'0px', opacity:0, transform:'scaleX(1)', offset:1}
        ], {duration:reduced ? 1 : 760, easing:'cubic-bezier(.65,0,.25,1)', fill:'forwards'});
        const token = session;
        exitTimer = setTimeout(() => { if (session === token) send('DISMISS'); }, reduced ? 20 : 800);
    };
    const update = data => {
        if (data.action === 'THEME') { document.documentElement.dataset.theme = data.theme === 'dark' ? 'dark' : 'light'; return; }
        if (data.action !== 'LAUNCH_PROGRESS') return;
        if (session !== null && data.session < session) return;
        const previousHeight = panel.offsetHeight;
        const newSession = session !== data.session;
        if (newSession) { clearTimeout(timer); clearTimeout(exitTimer); exitMotion?.cancel(); $('.content').style.height = ''; panel.classList.remove('leaving'); state = ''; }
        session = data.session;
        const changed = state !== data.state;
        state = data.state;
        const attention = state === 'partial' || data.failed > 0;
        const layoutChanged = changed || panel.dataset.attention !== String(attention);
        panel.dataset.attention = String(attention);
        if (changed) { clearTimeout(timer); clearTimeout(exitTimer); exitMotion?.cancel(); $('.content').style.height = ''; panel.classList.remove('leaving'); }
        panel.dataset.state = state;
        if (state === 'cancelled') { leave(); return; }
        if (state === 'opening') { clearTimeout(timer); clearTimeout(exitTimer); panel.classList.remove('leaving'); }
        $('h1').innerHTML = state === 'success' ? 'Your biome is <em>ready</em>' : state === 'partial' ? 'Almost <em>there</em>' : 'Opening your <em>biome</em>';
        $('.subtitle').textContent = state === 'success' ? 'Everything is in place. Make yourself at home.' : state === 'partial'
            ? (data.total ? 'Your workspace is open, but a few things need attention.' : 'No apps could be placed on your connected screens.')
            : data.failed ? 'Some apps need attention. The others are still opening.'
            : data.waiting ? 'Choose a project in your app. We’ll take it from there.' : 'A little moment. Your workspace is coming together.';
        $('.count').textContent = `${data.ready}/${data.total} apps ready`;
        $('.name').textContent = data.name || '';
        $('.name').title = data.name || '';
        $('.meter span').style.width = `${data.total ? Math.min(100, 100 * data.ready / data.total) : 0}%`;
        $('.actions').hidden = !attention;
        const constrained = (data.items || []).filter(item => item.state === 'constrained');
        const note = $('.size-note');
        note.textContent = state === 'success' && constrained.length
            ? `${constrained.map(item => item.app).join(', ')}: opened with app size limits.` : '';
        note.title = note.textContent;
        if (state === 'success' && constrained.length) $('.subtitle').textContent = 'Your apps are open. Make yourself at home.';
        const details = $('.details'); details.replaceChildren();
        for (const item of data.items || []) if (item.state === 'failed') {
            const line = document.createElement('p'); line.textContent = `${item.app}: ${item.detail || 'Could not finish opening.'}`; details.append(line);
        }
        if (data.skipped) { const line = document.createElement('p'); line.textContent = `${data.skipped} zone${data.skipped === 1 ? '' : 's'} skipped: monitor disconnected.`; details.append(line); }
        details.hidden = !attention || !details.children.length;
        if (layoutChanged && !newSession && !matchMedia('(prefers-reduced-motion: reduce)').matches) {
            morph?.cancel();
            const nextHeight = panel.offsetHeight;
            morph = panel.animate([{height:`${previousHeight}px`}, {height:`${nextHeight}px`}],
                {duration:520, easing:'cubic-bezier(.22,1,.36,1)'});
        }
        if (state === 'success' && changed) timer = setTimeout(leave, constrained.length ? 4200 : 2300);
    };
    $('.continue').addEventListener('click', leave);
    $('.report').addEventListener('click', () => send('REPORT'));
    document.addEventListener('keydown', event => { if (event.key === 'Escape') leave(); });
    host?.addEventListener('message', event => update(event.data));
    const sendBounds = () => {
        const rect = panel.getBoundingClientRect();
        host?.postMessage({ action: 'BOUNDS', x:rect.x, y:rect.y, width:rect.width, height:rect.height });
    };
    new ResizeObserver(sendBounds).observe(panel);
    panel.addEventListener('animationend', sendBounds);
    panel.addEventListener('transitionend', sendBounds);
    host?.postMessage({ action: 'READY' });
    // Read-only browser preview; never simulates completion inside the native app.
    if (!host) {
        const params = new URLSearchParams(location.search);
        document.documentElement.dataset.theme = params.get('theme') || 'light';
        update({ action:'LAUNCH_PROGRESS', session:1, name:'Creative space', state:params.get('state') || 'opening', ready:params.get('state') === 'success' ? 6 : 5, total:6, skipped:0, waiting:0, items:[{app:'Affinity',state:'failed',detail:'Waiting for a workspace window timed out.'}] });
    }
})();
