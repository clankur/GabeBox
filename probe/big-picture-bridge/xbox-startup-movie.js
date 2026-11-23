function installXboxStartupMovieProbe(host, missingOverride) {
    if (!missingOverride || typeof xboxNativeWindow === 'undefined' ||
        xboxNativeWindow.deviceFamily !== 'Windows.Xbox') return;
    const fetch = window.fetch;
    let reported = false;
    window.fetch = function (input, options) {
        // Match the pinned owner's exact override probe. The package was checked
        // for this optional file before signing, and the shipped movie stays local.
        if (input !== '/uioverrides/movies/bigpicture_startup.webm' ||
            options?.method !== 'HEAD' || Object.keys(options).length !== 1)
            return Reflect.apply(fetch, this, arguments);
        if (!reported) {
            reported = true;
            try { host.postMessage({method: 'startup-movie-override-js', state: 'absent-404'}); } catch (_) {}
        }
        return Promise.resolve(new Response(null, {status: 404, statusText: 'Not Found'}));
    };
}
