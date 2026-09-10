// Isolated Edge rendering/interaction tests. No user browser profile or biomes
// configuration is loaded. Run: node tests/onboarding_tests.mjs
import {spawn} from 'node:child_process';
import {mkdtemp, readFile, writeFile, mkdir} from 'node:fs/promises';
import {resolve, join} from 'node:path';
import {pathToFileURL} from 'node:url';
import assert from 'node:assert/strict';

const output = resolve('build-stability/onboarding-qa');
await mkdir(output, {recursive:true});
const profile = await mkdtemp(join(output, 'isolated-profile-'));
const edge = spawn('C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',
    ['--headless=new', '--no-first-run', '--no-default-browser-check', '--remote-debugging-port=0',
     `--user-data-dir=${profile}`, 'about:blank'], {windowsHide:true, stdio:'ignore'});
let socket;
try {
    let port;
    for (let retry=0; retry<100 && !port; retry++) {
        try { port = (await readFile(join(profile, 'DevToolsActivePort'), 'utf8')).split('\n')[0]; }
        catch { await new Promise(done => setTimeout(done, 100)); }
    }
    assert(port, 'isolated Edge started');
    const tabs = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
    socket = new WebSocket(tabs.find(tab => tab.type === 'page').webSocketDebuggerUrl);
    await new Promise((done, reject) => { socket.onopen=done; socket.onerror=reject; });
    let nextId=0;
    const pending=new Map();
    socket.onmessage=event => {
        const data=JSON.parse(event.data), task=pending.get(data.id);
        if (!task) return;
        pending.delete(data.id);
        data.error ? task.reject(new Error(JSON.stringify(data.error))) : task.resolve(data.result);
    };
    const call=(method, params={}) => new Promise((resolve,reject) => {
        const id=++nextId; pending.set(id,{resolve,reject}); socket.send(JSON.stringify({id,method,params}));
    });
    const evaluate=async expression => {
        if (expression.includes('.click()')) await new Promise(done => setTimeout(done, 320));
        const result=await call('Runtime.evaluate',{expression,awaitPromise:true,returnByValue:true});
        assert(!result.exceptionDetails, JSON.stringify(result.exceptionDetails));
        return result.result.value;
    };
    const waitFor=async expression => {
        for(let i=0;i<100;i++) {
            if(await evaluate(expression)) return;
            await new Promise(done=>setTimeout(done,50));
        }
        throw new Error(`Timed out: ${expression}`);
    };
    await call('Page.enable');
    await call('Emulation.setDeviceMetricsOverride',{width:1100,height:760,deviceScaleFactor:1,mobile:false});
    const url=pathToFileURL(resolve('frontend/index.html')).href;
    await call('Page.navigate',{url});
    await waitFor('document.querySelector("#onboarding")?.open === true');
    await evaluate('document.fonts.ready');
    await evaluate('Promise.all(document.querySelector("#onboarding").getAnimations().map(a=>a.finished))');
    const nextPosition = await evaluate('document.querySelector(".onboarding-next").getBoundingClientRect().x');
    await evaluate('for(let i=0;i<20;i++) document.querySelector(".onboarding-next").click()');
    assert.equal(await evaluate('document.querySelector(".onboarding-count").textContent'),'2 of 5','rapid clicks advance only once');
    assert.equal(await evaluate('document.querySelector(".onboarding-next").getBoundingClientRect().x'),nextPosition,'Next stays fixed when Back appears');
    await evaluate('document.querySelector(".onboarding-back").click()');
    for(let i=0;i<5;i++) {
        await waitFor('[...document.querySelectorAll("#onboarding img")].every(i=>i.complete && i.naturalWidth>0)');
        await evaluate('Promise.all(document.querySelector("#onboarding .onboarding-slide").getAnimations().map(a=>a.finished))');
        assert.equal(await evaluate('getComputedStyle(document.querySelector("#onboarding")).backgroundColor'),'rgb(255, 255, 255)');
        assert.equal(await evaluate('document.querySelector(".onboarding-dots [aria-current=step]").getAttribute("aria-label")'),`Card ${i+1} of 5`);
        const screenshot=await call('Page.captureScreenshot');
        await writeFile(join(output,`card-${i+1}.png`),Buffer.from(screenshot.data,'base64'));
        await evaluate('document.querySelector(".onboarding-next").click()');
    }
    assert.equal(await evaluate('document.querySelector("#onboarding").open'),false);
    assert.equal(await evaluate('localStorage.getItem("biomes-onboarding-completed-v1")'),'true');
    await call('Page.reload');
    await waitFor('document.readyState === "complete"');
    assert.equal(await evaluate('document.querySelector("#onboarding").open'),false,'completion survives reload');
    await evaluate('document.querySelector("#replay-onboarding").click(); document.documentElement.dataset.theme="dark"');
    assert.equal(await evaluate('document.querySelector("#onboarding").open'),true,'replay opens');
    assert.equal(await evaluate('getComputedStyle(document.querySelector("#onboarding")).color'),'rgb(34, 34, 34)','dark dashboard does not recolor tour');
    await call('Input.dispatchKeyEvent',{type:'keyDown',key:'ArrowRight',code:'ArrowRight'});
    assert.equal(await evaluate('document.querySelector(".onboarding-dots [aria-current=step]").getAttribute("aria-label")'),'Card 2 of 5');
    await evaluate('document.querySelector(".onboarding-back").click()');
    assert.equal(await evaluate('document.querySelector(".onboarding-back").disabled'),true);
    for (const [width,height] of [[770,590],[560,500],[390,680]]) {
        await call('Emulation.setDeviceMetricsOverride',{width,height,deviceScaleFactor:1,mobile:false});
        assert(await evaluate('(()=>{const r=document.querySelector("#onboarding").getBoundingClientRect();return r.left>=0&&r.top>=0&&r.right<=innerWidth&&r.bottom<=innerHeight})()'),'dialog fits viewport');
        const screenshot=await call('Page.captureScreenshot');
        await writeFile(join(output,`viewport-${width}.png`),Buffer.from(screenshot.data,'base64'));
    }
    await evaluate('document.querySelector(".onboarding-skip").click()');
    assert.equal(await evaluate('document.querySelector("#onboarding").open'),false,'skip exits');
    await evaluate('localStorage.removeItem("biomes-onboarding-completed-v1"); document.querySelector("#replay-onboarding").click()');
    await call('Input.dispatchKeyEvent',{type:'keyDown',key:'Escape',code:'Escape',windowsVirtualKeyCode:27});
    await call('Input.dispatchKeyEvent',{type:'keyUp',key:'Escape',code:'Escape',windowsVirtualKeyCode:27});
    await waitFor('document.querySelector("#onboarding").open === false');
    assert.equal(await evaluate('localStorage.getItem("biomes-onboarding-completed-v1")'),null,'Escape does not mark completed');
    await call('Page.addScriptToEvaluateOnNewDocument',{source:'Storage.prototype.getItem = Storage.prototype.setItem = function(){throw new Error("Storage disabled for test")};'});
    await call('Page.reload');
    await waitFor('document.querySelector("#onboarding")?.open === true');
    await evaluate('document.querySelector(".onboarding-skip").click()');
    assert.equal(await evaluate('document.querySelector("#onboarding").open'),false,'unavailable storage never traps user');
    // Mock the native bridge: verify dispatch without opening a browser/payment page.
    await call('Network.enable');
    await call('Network.setBlockedURLs', {urls:['*ko-fi.com*']});
    await call('Page.addScriptToEvaluateOnNewDocument', {source:'window.supportMessages=[]; window.chrome.webview={postMessage:m=>window.supportMessages.push(m),addEventListener:()=>{}};'});
    await call('Page.reload');
    await waitFor('document.querySelector("#onboarding")?.open === true');
    await evaluate('document.querySelector(".onboarding-skip").click()');
    await evaluate('location.hash="developer"');
    await waitFor('document.querySelector("#developer-page").hidden === false');
    assert.equal(await evaluate('document.querySelector("#kofi-dialog, script[src*=kofi]")'),null,'embedded widget removed');
    assert.equal(await evaluate('document.querySelector("#open-kofi").href'),'https://ko-fi.com/abdelghafourrebbouh');
    await evaluate('document.querySelector("#open-kofi").click()');
    assert.deepEqual(await evaluate('window.supportMessages.filter(m=>m.action === "OPEN_EXTERNAL")'),[{action:'OPEN_EXTERNAL',url:'https://ko-fi.com/abdelghafourrebbouh'}]);
    // Never send real subscriptions. All newsletter requests are intercepted locally.
    await evaluate(`window.newsletterCalls=[]; window.fetch=(url,options)=>{
        window.newsletterCalls.push({url,method:options.method,body:options.body.toString(),credentials:options.credentials});
        return new Promise((resolve,reject)=>{window.finishNewsletter=resolve;window.failNewsletter=reject;});
    };`);
    await evaluate('document.querySelector("#newsletter-email").value="invalid"; document.querySelector("#newsletter-form").requestSubmit()');
    assert.equal(await evaluate('newsletterCalls.length'),0,'invalid email never sent');
    await evaluate('document.querySelector("#newsletter-email").value="biomes-test@example.com"; document.querySelector("#newsletter-form").requestSubmit(); document.querySelector("#newsletter-form").requestSubmit()');
    assert.equal(await evaluate('newsletterCalls.length'),1,'double submission blocked');
    assert.equal(await evaluate('document.querySelector("#newsletter-form button").disabled'),true);
    assert.deepEqual(await evaluate('newsletterCalls[0]'),{
        url:'https://assets.mailerlite.com/jsonp/1620793/forms/198235701885535513/subscribe',method:'POST',
        body:'fields%5Bemail%5D=biomes-test%40example.com&ml-submit=1&anticsrf=true&ajax=1',credentials:'omit'
    });
    await evaluate('finishNewsletter({ok:true,json:async()=>({success:false})})');
    await waitFor('document.querySelector("#newsletter-status").textContent.includes("did not accept")');
    for (const [reply,expected] of [
        ['finishNewsletter({ok:true,json:async()=>({success:false})})','did not accept'],
        ['finishNewsletter({ok:false,status:429})','Too many attempts'],
        ['finishNewsletter({ok:true,json:async()=>{throw new Error("not JSON")}})','couldn’t confirm'],
        ['failNewsletter(new TypeError("network failed"))','couldn’t confirm']
    ]) {
        await evaluate('document.querySelector("#newsletter-email").value="another-test@example.com"; document.querySelector("#newsletter-form").requestSubmit()');
        await evaluate(reply);
        await waitFor(`document.querySelector("#newsletter-status").textContent.includes(${JSON.stringify(expected)})`);
        assert.equal(await evaluate('document.querySelector("#newsletter-form button").disabled'),false,'retry enabled');
    }
    const countBeforeOffline=await evaluate('newsletterCalls.length');
    await evaluate('Object.defineProperty(navigator,"onLine",{configurable:true,value:false}); document.querySelector("#newsletter-form").requestSubmit()');
    assert.equal(await evaluate('newsletterCalls.length'),countBeforeOffline,'offline never submits');
    assert(await evaluate('document.querySelector("#newsletter-status").textContent.includes("offline")'));
    assert.equal(await evaluate('document.querySelector("#newsletter-success").hidden'),true,'errors do not show success');
    await evaluate('document.querySelector("#newsletter-email").focus()');
    assert.equal(await evaluate('getComputedStyle(document.querySelector("#newsletter-email")).outlineStyle'),'none','no inner input outline');
    assert(await evaluate('document.querySelector("#newsletter-optional").getBoundingClientRect().bottom <= document.querySelector("#newsletter-form").getBoundingClientRect().top'),'optional label above form');
    await evaluate('Object.defineProperty(navigator,"onLine",{configurable:true,value:true}); document.querySelector("#newsletter-form").requestSubmit()');
    await evaluate('finishNewsletter({ok:true,json:async()=>({success:true})})');
    await waitFor('document.querySelector("#newsletter-success").hidden === false');
    assert.equal(await evaluate('getComputedStyle(document.querySelector("#newsletter-form")).display'),'none','success replaces entire form');
    assert.equal(await evaluate('document.querySelector("#newsletter-email").value'),'','clear email after success');
    assert.equal(await evaluate('document.activeElement.id'),'newsletter-success','focus moves to confirmation');
    const acceptedCount=await evaluate('newsletterCalls.length');
    await evaluate('document.querySelector("#newsletter-email").value="biomes-test@example.com"; document.querySelector("#newsletter-form").requestSubmit()');
    assert.equal(await evaluate('newsletterCalls.length'),acceptedCount,'no repeat signup after success');
    for (const [theme,background] of [['dark','rgb(255, 255, 255)'],['light','rgb(34, 34, 34)']]) {
        await evaluate(`document.documentElement.dataset.theme=${JSON.stringify(theme)}; document.querySelector("#newsletter-success").scrollIntoView({block:"center"})`);
        assert.equal(await evaluate('getComputedStyle(document.querySelector("#newsletter-success")).backgroundColor'),background);
        const screenshot=await call('Page.captureScreenshot');
        await writeFile(join(output,`newsletter-success-${theme}.png`),Buffer.from(screenshot.data,'base64'));
    }
    console.log('PASS: onboarding, Ko-fi browser dispatch and newsletter validation, payload, duplicate guard, success, rejection, rate limit, network failure and offline handling (mocked; no subscriptions sent)');
} finally {
    socket?.close(); edge.kill(); // Only the isolated browser created by this test.
}
