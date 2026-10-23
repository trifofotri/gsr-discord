// ==UserScript==
// @name         davecast bridge
// @namespace    https://github.com/trifofotri/gsr-discord
// @version      1.0.0
// @description  Adds a button to Discord that hands  stream info to GPU Screen Recorder (davecast).
// @author       3443
// @match        https://discord.com/*
// @run-at       document-idle
// @grant        GM_xmlhttpRequest
// @connect      127.0.0.1
// @homepageURL  https://github.com/trifofotri/gsr-discord
// @downloadURL  https://raw.githubusercontent.com/trifofotri/gsr-discord/master/davecast-bridge.user.js
// @updateURL    https://raw.githubusercontent.com/trifofotri/gsr-discord/master/davecast-bridge.user.js
// ==/UserScript==

// Requires the Vencord userscript to be installed and enabled (it provides the webpack helpers used below).

(function wait() {
    const V = unsafeWindow.Vencord;
    const C = V?.Webpack?.Common;
    const Auth = V?.Webpack?.findByProps?.("getSessionId");
    if (!C?.FluxDispatcher || !C?.UserStore || !Auth) return setTimeout(wait, 1000);
 
    const PORT = 47813;
    let armed = false, create = null;
 
    const btn = document.createElement("button");
    btn.textContent = "davecast: start";
    Object.assign(btn.style, {
        position: "fixed", right: "16px", bottom: "16px", zIndex: 99999,
        padding: "8px 12px", borderRadius: "8px", border: "none",
        background: "#5865f2", color: "#fff", cursor: "pointer", font: "13px sans-serif",
    });
    document.body.appendChild(btn);
    const status = (t) => (btn.textContent = "davecast: " + t);
 
    btn.onclick = () => {
        armed = true; create = null;
        status("armed...");
        
        try {
            const chId = C.SelectedChannelStore.getVoiceChannelId();
            if (!chId) return status("join a call first");
            const guildId = C.ChannelStore.getChannel(chId)?.guild_id ?? null;
            const gw = V.Webpack.findStore("GatewayConnectionStore");
            gw.getSocket().streamCreate(guildId ? "guild" : "call", guildId, chId, null);
        } catch (e) {
            console.warn("[davecast] auto-create failed, click Share Screen manually", e);
            status("armed - click Share Screen");
        }
    };
 
    C.FluxDispatcher.addInterceptor((e) => {
        if (!armed) return;
        if (e.type === "STREAM_CREATE") create = e;
        if (e.type === "STREAM_SERVER_UPDATE" && create) {
            const body = JSON.stringify({
                user_id: C.UserStore.getCurrentUser().id,
                session_id: Auth.getSessionId(),
                rtc_server: create.rtcServerId ?? create.rtc_server_id,
                rtc_channel: create.rtcChannelId ?? create.rtc_channel_id,
                stream_key: create.streamKey ?? create.stream_key,
                endpoint: e.endpoint,
                token: e.token,
            });
            armed = false;
            GM_xmlhttpRequest({
                method: "POST",
                url: `http://127.0.0.1:${PORT}/inject`,
                headers: { "X-Davecast": "1" },
                data: body,
                onload: (r) => status(r.status === 200 ? "streaming" : "error " + r.status),
                onerror: () => status("GSR not listening"),
            });
            return true; 
        }
    });
    status("ready");
})();
