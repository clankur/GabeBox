const assert = require('node:assert/strict');
const fs = require('node:fs');
const test = require('node:test');
const vm = require('node:vm');

const source = fs.readFileSync(require.resolve('../big-picture-bridge/xbox-storage.js'), 'utf8');
const plain = value => JSON.parse(JSON.stringify(value));
const tick = () => new Promise(resolve => setImmediate(resolve));

function setup(request) {
    const client = {};
    let moved = 0;
    const control = vm.runInNewContext(source + '\ninstallXboxStorage(client, request, appName, errors, moved)', {
        client, request, setTimeout, console, appName: id => id === 42 ? 'Fixture' : undefined,
        errors: () => ({VIA: 0, vyo: 10}), moved: () => ++moved
    });
    return {client, control, moved: () => moved};
}

test('the initial subscription makes the Valve store fetch a drive snapshot', async () => {
    const apps = [{appId: 42, sizeBytes: 1200}];
    let lists = 0;
    const {client} = setup(operation => {
        assert.equal(operation, 'list');
        ++lists;
        return {defaultId: 'E', folders: [{id: 'E', name: 'USB storage', path: 'E:\\SteamLibrary', mounted: true,
            totalBytes: 5000, freeBytes: 2000, apps}]};
    });
    const storage = client.InstallFolder;
    let inventory;
    let unregistered = 0;
    const sub = storage.RegisterForInstallFolderChanges(async () => { inventory = await storage.GetInstallFolders(); });
    storage.RegisterForInstallFolderChanges(() => { ++unregistered; }).unregister();
    await tick();
    assert.equal(lists, 1);
    assert.equal(unregistered, 0);
    assert.deepEqual(plain(inventory.map(row => [row.nFolderIndex, row.bIsMounted, row.bIsDefaultFolder])),
        [[0, false, false], [1, true, true]]);
    assert.equal(inventory[1].nCapacity, 5000);
    assert.equal(inventory[1].nFreeSpace, 2000);
    assert.equal(inventory[1].nAppSize, 1200);
    assert.equal(inventory[1].vecApps[0].strAppName, 'Fixture');
    sub.unregister();
});

test('an unavailable inventory renders a read-only row and cannot receive writes', async () => {
    const {client} = setup(operation => {
        assert.equal(operation, 'list');
        throw new Error('storage unavailable');
    });
    const storage = client.InstallFolder;
    const [row] = await storage.GetInstallFolders();
    assert.equal(row.strUserLabel, 'Storage unavailable');
    assert.equal(row.nCapacity, 0);
    assert.equal(await storage.SetDefaultInstallFolder(0), false);
    await assert.rejects(storage.MoveInstallFolderForApp(42, 0), error => error === 10);
});

test('move admission precedes progress and closing the dialog skips its queued games', async () => {
    let complete;
    const {client, moved} = setup((operation, argument) => {
        if (operation === 'list') return {defaultId: 'D', folders: [{id: 'D', mounted: true}]};
        assert.equal(operation, 'move');
        assert.deepEqual(plain(argument), {appIds: [42], destination: 'D'});
        return new Promise(resolve => { complete = resolve; });
    });
    const storage = client.InstallFolder;
    await storage.GetInstallFolders();
    let accepted = false;
    const progressed = new Promise(resolve => storage.RegisterForMoveContentProgress(progress => {
        assert(accepted);
        resolve(plain(progress));
    }));
    await storage.MoveInstallFolderForApp(42, 0);
    accepted = true;
    await assert.rejects(storage.MoveInstallFolderForApp(43, 0), error => error === 10);
    await new Promise(resolve => setTimeout(resolve, 0));
    complete({ok: true});
    assert.deepEqual(await progressed, {appid: 42, eError: 0, flProgress: 100,
        strBytesMoved: '', strTotalBytesToMove: '', nFilesMoved: 0});
    assert.equal(moved(), 1);

    let closedProgress = 0;
    storage.RegisterForMoveContentProgress(() => { ++closedProgress; });
    await storage.MoveInstallFolderForApp(42, 0);
    storage.CancelMove();
    await new Promise(resolve => setTimeout(resolve, 0));
    complete({ok: true});
    await tick();
    assert.equal(moved(), 2);
    assert.equal(closedProgress, 0);
});
