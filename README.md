# Cloud Play

Cloud Play is a game streaming client for PS5 based on
[chiaki-ngc](https://github.com/tertiumndatur/chiaki-ngc), which itself is based
on [chiaki-ng](https://github.com/streetpea/chiaki-ng). It streams games available
through the premium subscription catalog. It downloads the Cloud Play catalog
over verified HTTPS and starts PS5 Cloud or PS Now sessions. Video uses
the PS5 VideoDec2 hardware decoder with AGC presentation, audio uses Opus and
SDL, and controller input is forwarded to the stream.

The launcher uses the GPL-3.0-or-later
[ps5-homebrew-ui](https://github.com/blackbearreloaded/ps5-homebrew-ui) rendering
kit and its Paper Library design language. It renders through the
[ps5-opengl](https://github.com/blackbearreloaded/ps5-opengl) OpenGL 4.6 stack;
SDL remains responsible only for streaming audio, timing, and events. The
launcher releases EGL before the native AGC stream presenter takes the video
output, then opens the Paper interface again when the stream ends.

## First launch and access settings

The package contains no NPSSO, PSN token, account ID, or DNS address. A fresh
installation starts with all five PSN fields and all DNS resolves empty. The
Games page stays blocked until a complete access document is accepted by the
local HTTP receiver.

The application shows its setup address at the bottom of the screen. Open that
address from a computer or phone on the same network, for example
`http://PS5_IP:9055`. The built-in page opens the account sign-in and
NPSSO pages in separate browser tabs. Paste the final redirect URL and the
NPSSO response into the original setup page, then select **Complete setup**.

The browser resolves every required host through DNS over HTTPS and sends those
addresses with the two pasted values. The PS5 exchanges the authorization code,
retrieves the access token, refresh token, and account ID, saves the complete
access settings, and unlocks the Games page. No separate setup program is
required.

The setup page remains available for the entire time the application is open,
including while the current tokens are valid. It can replace the tokens at any
time. Cloud Play reads the stored access-token expiry and shows an expiry
warning with the setup address. Expiry is informational: the catalog, refresh,
and stream launch remain available. If the service rejects the old token, the
same setup page can be used to sign in again and replace it.

The receiver also retains `POST /settings` for advanced integrations. Its JSON
body must contain exactly `psnTokens` and `dnsResolves`. All five token fields
and all required DNS hosts must be present and nonempty. A partial document is
rejected without changing the active settings.

```json
{
  "psnTokens": {
    "npsso": "...",
    "psn_auth_token": "...",
    "psn_refresh_token": "...",
    "psn_auth_token_expiry": "...",
    "psn_account_id": "..."
  },
  "dnsResolves": {
    "psnow.playstation.com": ["203.0.113.10"],
    "www.playstation.com": ["203.0.113.11"],
    "commerce.api.np.km.playstation.net": ["203.0.113.12"],
    "web.np.playstation.com": ["203.0.113.13"],
    "accounts.api.playstation.com": ["203.0.113.14"],
    "apollo2.dl.playstation.net": ["203.0.113.15"],
    "vulcan.dl.playstation.net": ["203.0.113.16"],
    "image.api.playstation.com": ["203.0.113.17"],
    "gs2-sec.ww.prod.dl.playstation.net": ["203.0.113.18"]
  }
}
```

The addresses above are documentation placeholders. Successful responses never
return secrets. The receiver uses HTTP only on the local network, limits request
bodies to 64 KiB, and handles one connection at a time. Redirect URLs, NPSSO,
tokens, and HTTP bodies are excluded from the diagnostic log.

After a successful POST, the app merges only those access fields into
`/download0/settings.json`. Streaming preferences cannot be changed through
HTTP. The PS5 Cloud and PS Now profiles each keep their own language,
resolution, bitrate, datacenter, measured datacenter list, and video recovery
mode.

`Show stream statistics` adds an in-game overlay with incoming and rendered
frame rates, packet loss, cloud RTT, decoder timing, and client frame timing.
It also shows an input-to-cloud estimate made from the measured controller
feedback send delay plus half of the cloud RTT. This estimate stops at the
cloud host; it does not include server rendering, video return, display scanout,
or television latency.

There are no credential, settings, catalog, selection, or cover migration
paths. The current catalog cache is `/download0/catalog.json`; covers are
decoded directly into memory.

## Controls

- `L1` / `R1`: switch pages.
- D-pad: move or change the selected setting.
- `Cross`: activate or change a value.
- `Circle`: cancel the game-version dialog. Pages change only with `L1` / `R1`.
- `Options`: refresh the Sony catalog from the Games page. It is ignored while
  streaming, including when used in the session-ending chord.
- `Options` + `Circle`: end an active streaming session.

Search uses the native PS5 keyboard. Cross on a title with multiple variants
opens the PS5/PS4 version picker.
The Games page stays disabled until every required PSN token and DNS resolve has
been received through the built-in setup page or `POST /settings`.

## Legal and responsible use

Cloud Play is an independent, unofficial project. It is not affiliated with,
endorsed by, certified by, or sponsored by Sony.
PlayStation and related trademarks are the property of their respective
owners.

## Current limitations

- VideoDec2/AGC supports H.264 and HEVC. The 1440p and 2160p modes still need
  broader runtime testing.
- Buttons, analog triggers, sticks, touch contacts, controller motion, and
  standard rumble are forwarded. Adaptive trigger effects and DualSense haptic
  audio are not yet supported.
- Catalog availability depends on account region, PlayStation Plus tier, and
  title entitlement; Sony can decline a listed title during allocation.
