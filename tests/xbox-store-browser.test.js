const test = require('node:test');
const assert = require('node:assert/strict');
const {readFileSync} = require('node:fs');
const {runInNewContext} = require('node:vm');
const source = readFileSync(require.resolve('../probe/big-picture-bridge/xbox-store-browser.js'),'utf8');
const blank = 'data:text/html,%3Cbody%3E%3C%2Fbody%3E';
const options = {strName:'MainBrowser', parentPopupBrowserID:2, strUserAgentIdentifier:'Valve Steam Gamepad',
    strInitialURL:blank, bPreventCloseFromJavascript:true};
function environment() {
    const sent = [], owner = new EventTarget(), host = new EventTarget();
    Object.assign(owner, {innerWidth:800, innerHeight:600});
    host.postMessage = msg => sent.push(structuredClone(msg));
    const client = {BrowserView:{Destroy(view) { sent.push({destroyPopup:view}); }}};
    const adapter = runInNewContext(source + '\ninstallXboxStoreBrowser', {URL})(client,host,() => owner);
    const receive = (id, kind, fields = {}) => host.dispatchEvent(new MessageEvent('message',
        {data:{event:'store-browser',id,kind,...fields}}));
    const view = client.BrowserView.Create(options);
    return {client,adapter,owner,sent,view,receive,id:sent[0].id};
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
    assert.deepEqual(sent.at(-1),{method:'store-browser',id,action:'navigate',url});
    receive(id,'start',{url});
    receive(id,'title',{title:'Steam Store'});
    receive(id,'complete',{url,success:true});
    assert.deepEqual(finished.at(-1),[url,'Steam Store']);
    receive(id,'complete',{url:next,success:false,code:503});
    assert.equal(errors.at(-1)[0],503); assert.equal(history.length,2);
    receive(id,'complete',{url:next,success:true});
    receive(id,'history',{canGoBack:true,canGoForward:false});
    view.GoBack();
    assert.equal(sent.at(-1).action,'back');
    receive(id,'complete',{url,success:true});
    assert.equal(history.at(-1).index,1); assert.equal(history.at(-1).entries.length,3);
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
