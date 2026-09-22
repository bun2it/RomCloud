# RomCloud — Google OAuth 2.0 Device Flow Specification (Phase 4)

This document details the RFC 8628 OAuth 2.0 Device Authorization Grant implementation in **RomCloud** for the TrimUI Brick Pro.

---

## 1. Authentication Architecture (RFC 8628)

Because the TrimUI Brick Pro is a gaming handheld without a web browser or virtual keyboard, RomCloud implements the **OAuth 2.0 Device Authorization Flow**:

```
 ┌──────────────────────┐                           ┌──────────────────────┐
 │   TrimUI Brick Pro   │                           │ Google OAuth Server  │
 └──────────┬───────────┘                           └──────────┬───────────┘
            │                                                  │
            │  1. POST /device/code (client_id, scope)         │
            ├─────────────────────────────────────────────────►│
            │  2. JSON (device_code, user_code, url, interval) │
            │◄─────────────────────────────────────────────────┤
            │                                                  │
  [Renders QR Code & User Code on 1024x768 Display]            │
            │                                                  │
            │  3. Poll POST /token (device_code, grant_type)   │
            ├─────────────────────────────────────────────────►│
            │     HTTP 428 / "authorization_pending"           │
            │◄─────────────────────────────────────────────────┤
            │                                                  │
            │       ┌───────────────────────────────────┐      │
            │       │ User scans QR Code on Smartphone  │      │
            │       │ & Approves Access on Google Login │      │
            │       └─────────────────┬─────────────────┘      │
            │                         ▼                        │
            │  4. Poll POST /token (Every 5 seconds)           │
            ├─────────────────────────────────────────────────►│
            │  5. 200 OK (access_token, refresh_token)         │
            │◄─────────────────────────────────────────────────┤
            │                                                  │
  [Saves securely to SQLite settings: auth_refresh_token]      │
            ▼                                                  ▼
```

---

## 2. Core Modules

| Module | Location | Responsibility |
| :--- | :--- | :--- |
| **`HttpClient`** | `src/network/` | libcurl wrapper for SSL-verified HTTPS GET, POST, Form-encoding, and User-Agent management. |
| **`JsonHelper`** | `src/network/` | Zero-dependency JSON key-value extraction for OAuth responses. |
| **`QrRenderer` / `qrcodegen`** | `src/ui/` | Native C++ ISO/IEC 18004 QR Code generator and direct SDL2 renderer. |
| **`AuthManager`** | `src/auth/` | Manages authorization lifecycle, asynchronous token polling, automatic token refresh, user email resolution, and SQLite token storage. |

---

## 3. Token Security & Storage

- **Persistence:** Tokens are stored in `/mnt/SDCARD/Apps/RomCloud/data/library.db` in table `settings`.
- **Sensitive Fields:**
  - `auth_access_token`: Short-lived OAuth bearer token.
  - `auth_refresh_token`: Long-lived token used to generate new access tokens.
  - `auth_expires_at`: Epoch timestamp for auto-refresh calculations.
  - `auth_user_email`: Account display identifier.
- **Log Privacy:** Tokens and secrets are never output to log files or standard output.

---

## 4. UI Screen: Device Authorization (`UIState::CLOUD_LOGIN`)

- **Left Section:** High-resolution 340x340 QR Code linking to `https://www.google.com/device?user_code=XXXX-XXXX`.
- **Right Section:**
  - Step-by-step instructions in Vietnamese / English.
  - Neon bordered User Code box (`XXXX-XXXX`).
  - Animated status spinner: `● Waiting for approval on phone / browser...`.
  - Button `[B] Cancel` to abort polling and return to Settings.
