// Valve's Windows release 1794536281 requests these nine native URL entries
// in steamui/sp.js (CURLStore.Init). steamui.dll supplies them separately
// from public/url_list.txt. Values below use the public production CDN
// configuration Valve served on 2026-11-21:
// https://store.steampowered.com/app/620/Portal_2/?l=english
// The page's application_config data-config provides AVATAR_BASE_URL,
// BASE_URL_SHARED_CDN, CLAN_CDN_ASSET_URL, COMMUNITY_CDN_URL,
// MEDIA_CDN_COMMUNITY_URL, STORE_CDN_URL, VIDEO_CDN_URL and WEBAPI_BASE_URL.
// StoreAppImages is the prefix of the same page's application image URLs.
// Its path also occurs in the pinned steamui.dll. CURLStore appends "/<appid>"
// itself, so this entry intentionally has no trailing slash.
const xboxUrlRoots = Object.freeze({
    AvatarBaseURL: "https://avatars.fastly.steamstatic.com/",
    BaseURLSharedCDN: "https://shared.fastly.steamstatic.com/",
    ClanAssetCDN: "https://clan.fastly.steamstatic.com/",
    CommunityCDN: "https://community.fastly.steamstatic.com/",
    CommunityImages: "https://shared.fastly.steamstatic.com/community_assets/",
    StoreAppImages: "https://shared.fastly.steamstatic.com/store_item_assets/steam/apps",
    StoreCDN: "https://store.fastly.steamstatic.com/",
    VideoCDN: "https://video.fastly.steamstatic.com/",
    WebAPI: "https://api.steampowered.com/"
});
