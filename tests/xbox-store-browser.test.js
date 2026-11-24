const test = require('node:test');
const assert = require('node:assert/strict');
const {readFileSync} = require('node:fs');
const {runInNewContext} = require('node:vm');
const source = readFileSync(require.resolve('../ui/big-picture-bridge/xbox-store-browser.js'),'utf8');
const blank = 'data:text/html,%3Cbody%3E%3C%2Fbody%3E';
const options = {strName:'MainBrowser', parentPopupBrowserID:2, strUserAgentIdentifier:'Valve Steam Gamepad',
    strInitialURL:blank, bPreventCloseFromJavascript:true};
function environment() {
    const sent = [], owner = new EventTarget(), host = new EventTarget();
    const timers = new Map();
    let timerId = 0, now = 0;
    Object.assign(owner, {innerWidth:800, innerHeight:600});
    host.postMessage = msg => sent.push(structuredClone(msg));
    const client = {BrowserView:{Destroy(view) { sent.push({destroyPopup:view}); }}};
    const adapter = runInNewContext(source + '\ninstallXboxStoreBrowser', {
        URL, setTimeout(callback, delay) { timers.set(++timerId, {callback, end: now + delay}); return timerId; },
        clearTimeout(id) { timers.delete(id); }
    })(client,host,() => owner);
    function advance(ms) {
        now += ms;
        for (const [id, {callback, end}] of timers) {
            if (end > now) continue;
            timers.delete(id);
            callback();
        }
    }
    const receive = (id, kind, fields = {}) => host.dispatchEvent(new MessageEvent('message',
        {data:{event:'store-browser',id,kind,request:0,...fields}}));
    const view = client.BrowserView.Create(options);
    return {client,adapter,owner,sent,view,receive,advance,id:sent[0].id};
}
test('only committed native navigation advances stock history, including the real blank bootstrap', () => {
    const {sent,view,receive,id} = environment(), history = [], finished = [], errors = [];
    view.on('history-changed', value => history.push(structuredClone(value)));
    view.on('finished-request', (...args) => finished.push(args));
    view.on('load-error', (...args) => errors.push(args));
    const url = 'https://store.steampowered.com/', next = url + 'explore/';
    receive(id+9,'complete',{url:'about:blank',success:true});
    receive(id,'start',{url:'about:blank'});
    assert.equal(history.length,0);
    receive(id,'complete',{url:'about:blank',success:true});
    assert.deepEqual(history[0], {index:0,entries:[{url:blank}]});
    view.LoadURL(url);
    const request = sent.at(-1).request;
    assert.deepEqual(sent.at(-1),{method:'store-browser',id,action:'navigate',url,request});
    receive(id,'start',{url,request});
    receive(id,'title',{title:'Steam Store'});
    receive(id,'complete',{url,request,success:true});
    assert.deepEqual(finished.at(-1),[url,'Steam Store']);
    receive(id,'complete',{url:next,request,success:false,code:503});
    assert.equal(errors.at(-1)[0],503); assert.equal(history.length,2);
    assert.deepEqual(finished.at(-1),[next,'']);
    receive(id,'complete',{url:next,request,success:true});
    receive(id,'history',{canGoBack:true,canGoForward:false});
    view.GoBack();
    assert.equal(sent.at(-1).action,'back');
    receive(id,'complete',{url,request,success:true});
    assert.equal(history.at(-1).index,1); assert.equal(history.at(-1).entries.length,3);
    view.GoBack();
    receive(id,'start',{url:'about:blank',request});
    receive(id,'complete',{url:'about:blank',request,success:true});
    assert.equal(history.at(-1).index,0); assert.equal(history.at(-1).entries.length,3);
});
test('a public request with no native callback expires and Reload ignores stale receipts', () => {
    const {client,sent,view,receive,advance,id} = environment();
    const history = [], finished = [], errors = [];
    view.on('history-changed', value => history.push(structuredClone(value)));
    view.on('finished-request', (...args) => finished.push(args));
    view.on('load-error', (...args) => errors.push(args));
    receive(id,'initial-document',{url:'about:blank'});
    const url = 'https://store.steampowered.com/';
    view.LoadURL(url);
    const first = sent.at(-1).request;
    receive(id,'start',{url:'about:blank'});
    receive(id,'complete',{url:'about:blank',success:true});
    advance(19999);
    assert.equal(errors.length,0);
    assert.equal(finished.length,0);
    advance(1);
    assert.deepEqual(sent.at(-1),{method:'store-browser',id,action:'stop',request:first});
    assert.deepEqual(finished,[[url,'']]);
    assert.deepEqual(errors,[[-7,url,'Steam Store navigation timed out']]);
    view.Reload();
    const retry = sent.at(-1).request;
    assert.notEqual(retry,first);
    assert.deepEqual(sent.at(-1),{method:'store-browser',id,action:'navigate',url,request:retry});
    receive(id,'complete',{url,request:first,success:true});
    receive(id,'start',{url:'about:blank'});
    receive(id,'complete',{url:'about:blank',success:true});
    assert.deepEqual(history,[{index:0,entries:[{url:blank}]}]);
    receive(id,'controller-reset',{request:retry});
    receive(id,'start',{url,request:retry});
    receive(id,'title',{title:'Steam Store'});
    receive(id,'complete',{url,request:retry,success:true});
    advance(20000);
    assert.equal(errors.length,1);
    assert.deepEqual(finished.at(-1),[url,'Steam Store']);
    assert.deepEqual(history.at(-1),{index:0,entries:[{url}]});
    view.LoadURL(url + 'explore/');
    client.BrowserView.Destroy(view);
    advance(20000);
    assert.equal(sent.at(-1).action,'close');
});
test('denies other constructors, origins and credential-bearing URLs and isolates stale instances', () => {
    const {client,sent,view,receive,id} = environment();
    for (const url of ['http://store.steampowered.com/','https://store.steampowered.com.example/',
        'https://u:p@store.steampowered.com/','https://store.steampowered.com:444/',blank,'file:///x','javascript:1']) view.LoadURL(url);
    assert.deepEqual(sent.map(r => r.action),['create']);
    client.BrowserView.Destroy({});
    assert.deepEqual(sent.at(-1),{destroyPopup:{}});
    client.BrowserView.Destroy(view);
    assert.equal(sent.at(-1).action,'close');
    for (const extra of [{strName:'ExternalBrowser'},{parentPopupBrowserID:1},{strInitialURL:'about:blank'},{arbitrary:true}])
        assert.throws(() => client.BrowserView.Create({...options,...extra}));
    const fresh = client.BrowserView.Create(options);
    assert.notEqual(sent.at(-1).id,id);
    const size = sent.length;
    view.LoadURL('https://store.steampowered.com/');
    receive(id,'closed');
    assert.equal(sent.length,size);
    fresh.Reload();
    assert.equal(sent.at(-1).action,'reload');
});
test('clips native bounds and forwards controller edges only while Valve focuses the Store', () => {
    const {adapter,view,sent,owner} = environment();
    view.SetBounds(-20,60,420,900);
    const {x,y,width,height} = sent.at(-1); assert.deepEqual({x,y,width,height},{x:0,y:0.1,width:0.5,height:0.9});
    adapter.setActionset(true);
    assert.equal(adapter.button(0,0,true),false);
    view.SetVisible(true); view.SetFocus(true);
    assert.equal(adapter.button(0,1,true),false);
    assert.equal(adapter.button(0,20,true),true);
    adapter.button(1,20,true); adapter.button(0,20,false);
    view.SetVisible(false);
    assert.deepEqual(sent.filter(r => r.action === 'input').map(({key,down}) => [key,down]),[['up',true],['up',false]]);
    owner.dispatchEvent(new Event('pagehide'));
    assert.equal(sent.at(-1).action,'close');
});

test('two B taps remain Steam navigation while the Store has focus', () => {
    const {client, owner, view, sent, adapter} = environment();
    const inputSource = readFileSync(require.resolve('../ui/big-picture-bridge/xbox-input.js'), 'utf8');
    let tick, pressed = false;
    Object.assign(owner, {document: {hidden: false}, setTimeout(callback) { tick = callback; return 1; }});
    client.System = {};
    const gamepads = {read() {
        const buttons = Array.from({length: 16}, (_, index) => ({pressed: index === 1 && pressed}));
        return [{id: 'pad', index: 0, mapping: 'standard', axes: [0, 0], buttons}];
    }};
    const install = runInNewContext(inputSource + '\ninstallXboxInput', {
        window: owner, getXboxGamepadWindow: () => owner, notifyXboxControllerListChanged() {}
    });
    install(client, gamepads, adapter);
    const valve = [];
    client.Input.RegisterForControllerInputMessages((slot, code, down) => valve.push([slot, code, down]));
    adapter.setActionset(true);
    view.SetVisible(true); view.SetFocus(true);
    const beforeBack = sent.length;
    for (const edge of [true, false, true, false]) { pressed = edge; tick(); }
    assert.deepEqual(valve, [[0, 1, true], [0, 1, false], [0, 1, true], [0, 1, false]]);
    assert.deepEqual(sent.slice(beforeBack), []);
});

test('a controller release follows its press across Store focus changes and disconnects', () => {
    const inputSource = readFileSync(require.resolve('../ui/big-picture-bridge/xbox-input.js'), 'utf8');
    for (const [index, code, key] of [[0, 0, 'enter'], [12, 20, 'up']]) {
        const {client, owner, view, sent, adapter} = environment();
        let tick, connected = true, pressed = false;
        Object.assign(owner, {document: {hidden: false}, setTimeout(callback) { tick = callback; return 1; }});
        client.System = {};
        const gamepads = {read() {
            const buttons = Array.from({length: 16}, (_, i) => ({pressed: i === index && pressed}));
            return connected ? [{id: 'pad', index: 1, mapping: 'standard', axes: [0, 0], buttons}] : [];
        }};
        const install = runInNewContext(inputSource + '\ninstallXboxInput', {
            window: owner, getXboxGamepadWindow: () => owner, notifyXboxControllerListChanged() {}
        });
        install(client, gamepads, adapter);
        const valve = [];
        client.Input.RegisterForControllerInputMessages((slot, code, down) => valve.push([slot, code, down]));
        adapter.setActionset(true);
        pressed = true; tick();
        view.SetVisible(true); view.SetFocus(true);
        pressed = false; tick();
        assert.deepEqual(valve, [[1, code, true], [1, code, false]]);
        assert.deepEqual(sent.filter(x => x.action === 'input'), []);

        pressed = true; tick();
        view.SetVisible(false);
        connected = false; tick();
        assert.deepEqual(valve, [[1, code, true], [1, code, false]]);
        assert.deepEqual(sent.filter(x => x.action === 'input').map(x => [x.key, x.down]),
            [[key, true], [key, false]]);

        // Reconnecting starts a new press, even in the same controller slot.
        connected = true; tick();
        pressed = false; tick();
        assert.deepEqual(valve.slice(2), [[1, code, true], [1, code, false]]);

        view.SetVisible(true);
        pressed = true; tick();
        client.Input.SetKeyboardActionset(true, false);
        client.Input.SetKeyboardActionset(true, false); // Steam's watchdog reasserts the same set.
        pressed = false; tick();
        assert.deepEqual(sent.filter(x => x.action === 'input').map(x => x.down), [true, false, true, false]);
        assert.equal(valve.length, 4);
        pressed = true; tick();
        client.Input.SetKeyboardActionset(false, false);
        client.Input.ModalKeyboardDismissed();
        pressed = false; tick();
        assert.deepEqual(valve.slice(4), [[1, code, true], [1, code, false]]);
        pressed = true; tick();
        pressed = false; tick();
        assert.deepEqual(sent.filter(x => x.action === 'input').map(x => x.down),
            [true, false, true, false, true, false]);
    }
});
