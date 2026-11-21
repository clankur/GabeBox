// The pinned UI uses Messaging for both local screen navigation and messages
// from desktop Steam services. Only route local UI messages in this WebView.
function installXboxMessaging(client) {
    const subscriptions = new Map();
    const topics = new Map([
        ['FriendsUI', new Set(['ChatTabActivated', 'NavigateToInvites', 'OpenFriendChat',
            'OpenChatRoomGroup', 'OpenQuickAccessVoiceChat', 'ShowQuickAccessVoiceChat'])],
        ['Library', new Set(['shared-apps'])],
        ['LibraryCommands', new Set(['ShowFriendChatDialog', 'ToggleDockedMode'])],
        ['PostToSteamUI', new Set(['show-main-window'])]
    ]);
    client.Messaging = {
        RegisterForMessages(channel, callback) {
            if (typeof channel !== 'string' || !/^[a-z0-9_]{1,48}$/i.test(channel) || typeof callback !== 'function')
                throw new TypeError('Expected a Steam UI message channel and callback');
            const listeners = subscriptions.get(channel) || new Set();
            subscriptions.set(channel, listeners);
            listeners.add(callback);
            return {unregister() {
                listeners.delete(callback);
                if (!listeners.size && subscriptions.get(channel) === listeners) subscriptions.delete(channel);
            }};
        },
        PostMessage(channel, topic, payload) {
            if (!topics.get(channel)?.has(topic)) throw new Error('Steam UI message service unavailable');
            if (typeof payload !== 'string' || payload.length > 65536) throw new TypeError('Invalid Steam UI message');
            const listeners = subscriptions.get(channel);
            if (!listeners?.size) return;
            for (const listener of [...listeners]) queueMicrotask(() => {
                if (listeners.has(listener)) listener(channel, topic, payload);
            });
        }
    };
    // Native activation diagnostics are local to this renderer. Keep one
    // timestamp per known initialization category in the browser timeline.
    const categories = new Set(['CloudStorage.Download', 'CloudStorage.Upload', 'CloudStorage.Notification', 'Showcases']);
    client.Stats = {RecordActivationEvent(category) {
        if (categories.has(category)) {
            const name = 'SteamUI.' + category;
            performance.clearMarks(name);
            performance.mark(name);
        }
    }};
}
