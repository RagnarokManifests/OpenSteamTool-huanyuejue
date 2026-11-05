# cloud_redirect.dll

Prebuilt CloudRedirect 2.6.7 (Steam Cloud save redirection for unlocked
games), shipped with the release zips so OpenSteamTool's `[cloud]` feature
works out of the box.

- Source and binary: https://github.com/huanyuejue/CloudRedirect/releases/tag/v2.6.7
  (`cloud_redirect.dll`, SHA-256 `5b3c3ec53b71bacbc66461fc24690455fa2b2bf9b1dcb22cc0f92ecc596e741f`,
  the same hash huanyuejue publishes).
- License: MIT, Copyright (c) 2026 Selectively11 — see `CloudRedirect-LICENSE.txt`.
- Loaded only when `opensteamtool.toml` has `[cloud] enabled = true`.
- The update notice installs and updates it like the other DLLs.
