// Steam UI 1794536281. Extend the stock app-actions menu through its
// additionalActions prop. The native broker owns all snapshot operations.
function installXboxCloudBackup(runtime, send, available) {
    const menuModule = 43394;
    const factory = runtime.m?.[menuModule];
    if (typeof factory !== 'function' || typeof runtime.d !== 'function' || runtime.c?.[menuModule]) return;

    function backupDialog({appId, canRestore, closeModal}) {
        const {jsx, jsxs} = runtime(62540);
        const {useEffect, useRef, useState} = runtime(63696);
        const {o0: Dialog} = runtime(64243);
        const {$n: Button} = runtime(50777);
        const [summary, setSummary] = useState('Checking saved backups…');
        const [message, setMessage] = useState('');
        const [loading, setLoading] = useState(true);
        const [confirming, setConfirming] = useState(false);
        const mounted = useRef(false);
        const transferring = useRef(false);

        function refresh() {
            return send('cloud-backup-status', appId).catch(() => 'failed').then(status => {
                if (mounted.current) setSummary(steamCloud.backups(status));
            });
        }
        useEffect(() => {
            mounted.current = true;
            refresh().then(() => { if (mounted.current) setLoading(false); });
            return () => { mounted.current = false; };
        }, [appId]);

        function transfer(restore) {
            if (loading || transferring.current || (restore && !canRestore)) return;
            transferring.current = true;
            setConfirming(false);
            setLoading(true);
            setMessage(restore ? 'Restoring Steam Cloud saves…' : 'Downloading Steam Cloud saves…');
            send(restore ? 'cloud-backup-restore' : 'cloud-backup-download', appId).catch(() => 'failed').then(status => {
                if (!mounted.current) return;
                setMessage(steamCloud.message(status));
                return refresh();
            }).then(() => {
                transferring.current = false;
                if (mounted.current) setLoading(false);
            });
        }

        if (confirming) return jsx(Dialog, {
            strTitle: 'Use Steam Cloud saves?', closeModal, bCloseAfterOK: false, bDestructiveWarning: true,
            strDescription: 'This applies the current Steam Cloud saves to this Xbox and replaces matching files. The existing Xbox save folders will be backed up.',
            strOKButtonText: 'Download and use saves', strCancelButtonText: 'Cancel', focusButton: 'secondary',
            onOK: () => transfer(true), onCancel: () => { mounted.current = false; }
        }, 'confirm-restore');

        return jsx(Dialog, {strTitle: 'Cloud backups', closeModal, bCloseAfterOK: false, bOKDisabled: loading,
            focusButton: loading ? 'secondary' : 'primary', strOKButtonText: 'Download backup', strCancelButtonText: 'Close',
            onOK: () => transfer(false), onCancel: () => { mounted.current = false; }, children: jsxs('div', {children: [
                jsx('p', {role: 'status', children: summary}),
                message && jsx('p', {role: 'status', children: message}),
                canRestore && jsx(Button, {disabled: loading, onClick: () => setConfirming(true),
                    children: 'Download and use saves'})
            ]})});
    }

    function withBackupAction(Original) {
        return function XboxCloudBackupMenu(props) {
            const {jsx} = runtime(62540);
            const appId = props.overview?.appid;
            const owner = props.ownerWindow;
            const cloud = available(appId);
            // The pinned game-details gear supplies this set of props. Leave
            // grid menus, overlays, desktop menus, and multi-select untouched.
            if (props.bInGamepadUI !== true || props.omitPrimaryAction !== true || props.client !== 'selected' ||
                props.includeMultiSelect || !owner || !cloud) return jsx(Original, props);
            const {kt: MenuItem} = runtime(12711);
            const open = () => {
                const {pg: showModal} = runtime(12302);
                void showModal(jsx(backupDialog, {appId, canRestore: cloud.canRestore}), owner, {bNeverPopOut: true}).catch(() => {});
            };
            const backup = jsx(MenuItem, {onSelected: open, children: 'Cloud backups'}, 'xbox-cloud-backup');
            return jsx(Original, {...props, additionalActions: [props.additionalActions, backup]});
        };
    }

    runtime.m[menuModule] = function (module, exports, require) {
        const scopedRequire = new Proxy(require, {get(target, property, receiver) {
            if (property !== 'd') return Reflect.get(target, property, receiver);
            return (destination, definitions) => {
                if (destination !== exports || typeof definitions.uU !== 'function') return require.d(destination, definitions);
                const getOriginal = definitions.uU;
                let wrapped;
                return require.d(destination, {...definitions, uU: () => wrapped ??= withBackupAction(getOriginal())});
            };
        }});
        return factory.call(this, module, exports, scopedRequire);
    };
}
