# HomeLab — Full Reference Summary

## Hardware

**Machine:** Dell Optiplex (repurposed)
- CPU: Intel Core i7-7700
- RAM: 16 GB
- Storage: 476.9 GB NVMe SSD (single drive — OS, containers, and media all share this)
- OS: Proxmox VE 9.2.2 (no-subscription repo enabled)

---

## Proxmox Host Configuration

The Proxmox host runs bare-metal and manages LXC containers. Key host-level config:

**Media storage directories on host:**
```
/mnt/media/Movies
/mnt/media/TV Shows
/mnt/media/Downloads
```

**No-subscription repo** enabled via `/etc/apt/sources.list.d/pve-no-subscription.list`.

**LXC containers:**
| CT ID | Name | IP |
|---|---|---|
| 100 | Pi-hole | 192.168.86.100 |
| 101 | Docker / Portainer | 192.168.86.101 |
| 102 | Budget App | 192.168.86.102 |
| 103 | Home Assistant | 192.168.86.103 |

---

## CT 100 — Pi-hole

Network-wide DNS ad blocker. Runs as an LXC container, listens on `192.168.86.100`. All devices on the `192.168.86.x` subnet are pointed at it for DNS.

---

## CT 102 — Budget App

Personal budgeting web app. Runs as a **single Node.js process** (Express serves both API and built React client) with a local Postgres database. Intentionally NOT Docker — native Node + systemd, separate from CT 101's Docker stacks.

- **Repo:** `Cassequel/Budget` (private), cloned to `/opt/budget`
- **Service:** `/etc/systemd/system/budget.service` — runs `node server/dist/index.js` as the `budget` system user
- **Port:** `3001`
- **Public URL:** `budget.aidenswanson.com` — added as a second hostname on CT 101's existing Cloudflare tunnel (dashboard-managed)
- **Database:** PostgreSQL (local to CT 102), role `budget`, database `budget`. `DATABASE_SSL=false` (LAN only). Schema managed via Drizzle Kit migrations (`npm run db:migrate --workspace=server`)
- **`.env`** at `/opt/budget/.env` (gitignored) — holds `DATABASE_URL`, `JWT_SECRET`, `ADMIN_PASSWORD`, `ENCRYPTION_KEY`, `PLAID_CLIENT_ID`, `PLAID_SECRET`, `PLAID_ENV=production`, `PLAID_WEBHOOK_URL`, `CLIENT_URL`, `ANTHROPIC_API_KEY`
- ⚠️ **Never rotate `ENCRYPTION_KEY`** after accounts are linked — it decrypts stored Plaid access tokens. Re-linking is the only recovery.

### Stack

| Layer | Tech |
|---|---|
| Server | Node.js 20, Express, TypeScript, Drizzle ORM + `pg` |
| Client | React 19, TypeScript, Vite, TailwindCSS 4, Recharts |
| Database | PostgreSQL 16 (local) |
| Auth | Single-user JWT (`ADMIN_PASSWORD` env var) |

### Key Features

- **Plaid** (production) — bank + Venmo connections via Plaid Link. Access tokens stored AES-256-encrypted. Webhook at `https://budget.aidenswanson.com/api/plaid/webhook`
- **Auto-categorization** — hybrid: deterministic map from Plaid's `personal_finance_category.primary` for obvious cases; ambiguous ones batched to `claude-haiku-4-5` via `@anthropic-ai/sdk`. Runs after every Plaid sync + manual button. Only touches `NULL` category rows (never overwrites manual edits)
- **Dashboard** — net worth, monthly spend/income, runway, monthly spending trend line chart (filterable by category)
- **Breakdown** — month-by-month spend-by-category bar chart + transaction list (clickable bars filter the list). Excludes Loan Payments & Transfers from all aggregates
- **Transactions** — full list with account source badges (Venmo/MACU/Amex), category filter chips, per-category totals, inline category editing
- **Budget** — category cards with monthly limits, progress bars, inline editing (name/limit/color), 17 default categories seeded at boot
- **Plans & Savings** — financial planning + savings goal tracking

### Update Workflow

```bash
# On dev machine
git push

# On CT 102
cd /opt/budget && git pull && bash deploy/deploy.sh && sudo systemctl restart budget
```

`deploy.sh` runs: `npm install` → `npm run build` (server + client) → `npm run db:migrate` → done.

**Troubleshooting:**
- `journalctl -u budget -f` — app logs
- If `git pull` fails with "dubious ownership": `git config --global --add safe.directory /opt/budget`
- If `git pull` fails with local changes: `git checkout -- package-lock.json` (npm install regenerates it)

---

## CT 101 — Docker / Portainer

Main workhorse container. Runs all Docker stacks via Portainer (accessible at `https://192.168.86.101:9443`).

### TUN Device Passthrough (required for Gluetun VPN)

Added to `/etc/pve/lxc/101.conf` on the Proxmox host:
```
lxc.cgroup2.devices.allow: c 10:200 rwm
lxc.mount.entry: /dev/net/tun dev/net/tun none bind,create=file
```

### Media Bind Mount into CT 101

Run on Proxmox host:
```bash
pct set 101 -mp0 /mnt/media,mp=/mnt/media
```

### Permissions Fix

Because CT 101 is an unprivileged container, UIDs are shifted by 100000. Run on the Proxmox host (not inside the LXC):
```bash
chown -R 101000:101000 /mnt/media/Movies
chown -R 101000:101000 /mnt/media/"TV Shows"
chown -R 101000:101000 /mnt/media/Downloads
```

---

## Docker Stack 1 — Media Pipeline (`/opt/media-pipeline`)

Handles torrent downloading, indexing, and library management — all routed through a VPN.

| Container | Port | Purpose |
|---|---|---|
| **Gluetun** | — | ProtonVPN OpenVPN kill switch. `FIREWALL_INPUT_PORTS=8080` to allow qBittorrent traffic through |
| **qBittorrent** | 8080 | Torrent client. Uses `network_mode: service:gluetun` — all traffic through VPN |
| **Radarr** | 7878 | Movie library manager. Root folder: `/movies` → `/mnt/media/Movies` on host |
| **Sonarr** | 8989 | TV show library manager. Root folder: `/tv` → `/mnt/media/TV Shows` on host |
| **Prowlarr** | 9696 | Indexer aggregator. Feeds Radarr + Sonarr. Indexers: YTS, EZTV, 1337x |
| **Watchtower** | — | Auto-updates all running containers |
| **Uptime Kuma** | 3001 | Service uptime monitoring dashboard |
| **Cloudflared** | — | Cloudflare tunnel — exposes Rockflix at `watch.aidenswanson.com` |

**Gluetun credentials** are stored as Portainer environment variables (not in compose files).

**qBittorrent config** is persisted in a named Docker volume:
`media-pipeline_qbittorrent-config`

**Download client wiring:** Both Radarr and Sonarr point to qBittorrent at `http://localhost:8080` (reachable because they share the Gluetun network namespace via `network_mode`).

---

## Docker Stack 2 — Rockflix (`/opt/rockflix`)

Custom Netflix-style media server. Source: private GitHub repo, cloned to `/opt/rockflix` on CT 101.

### Containers

| Container | Image | Port | Notes |
|---|---|---|---|
| **db** | `postgres:16` | internal | Named volume `postgres_data`. DB name: `rockflix` |
| **api** | Custom (.NET 10) | 5244 (internal) | Built from `./Rockflix.API`. Includes ffmpeg |
| **client** | Custom (nginx + React 19) | `3000:80` | Built from `./rockflix-client` |

All three share a `rockflix-net` Docker bridge network.

### Compose Configuration

```yaml
services:
  db:
    image: postgres:16
    environment:
      POSTGRES_DB: rockflix
      POSTGRES_USER: postgres
      POSTGRES_PASSWORD: ${DB_PASSWORD}
    volumes:
      - postgres_data:/var/lib/postgresql/data

  api:
    build: ./Rockflix.API
    environment:
      ConnectionStrings__DefaultConnection: "Host=db;Port=5432;..."
      ASPNETCORE_URLS: "http://+:5244"
      Media__RootPath: "/mnt/media"
      Jwt__Secret / Issuer / Audience: ...
      Tmdb__ApiKey: ...
      Radarr__BaseUrl: "http://192.168.86.101:7878"
      Sonarr__BaseUrl: "http://192.168.86.101:8989"
      Telegram__BotToken / InviteCode: ...
      Anthropic__ApiKey: ...
      Radarr__RootFolderPath: "/movies"
      Sonarr__RootFolderPath: "/tv"
    volumes:
      - /mnt/media:/mnt/media

  client:
    build: ./rockflix-client
    ports:
      - "3000:80"
```

### `.env` file on CT 101 (gitignored)

```
DB_PASSWORD=
JWT_SECRET=
TMDB_API_KEY=
RADARR_API_KEY=
SONARR_API_KEY=
TELEGRAM_BOT_TOKEN=
TELEGRAM_INVITE_CODE=iamrockhard
ANTHROPIC_API_KEY=
RADARR_QUALITY_PROFILE_ID=
SONARR_QUALITY_PROFILE_ID=
```

---

## Rockflix — Application Architecture

### API (Rockflix.API — .NET 10)

**Dockerfile:**
- Build stage: `mcr.microsoft.com/dotnet/sdk:10.0` → `dotnet publish -c Release`
- Runtime stage: `mcr.microsoft.com/dotnet/aspnet:10.0` + `ffmpeg` installed via apt

**Controllers:**
- `AuthController` — register/login, JWT issuance
- `MoviesController` — movie library CRUD
- `TvShowsController` — TV show + episode library
- `MediaController` — general media endpoints
- `StreamController` — video streaming with range request support
- `RequestController` — media request management
- `TelegramController` / `WebhookController` — Telegram bot webhook handling
- `FavoritesController` — user favorites
- `WatchHistoryController` — playback progress tracking
- `AdminController` — admin-only operations
- `SmsController` — SMS-related features

**Services:**
- `MediaScannerService` — scans `/mnt/media` and populates the DB with discovered movies/shows
- `TmdbService` — fetches metadata (posters, descriptions, ratings) from TMDB API
- `TokenService` — JWT generation and validation
- `MediaRequestService` — uses **Claude Haiku** (`claude-haiku-4-5-20251001`) to parse natural language requests from Telegram, then calls Radarr/Sonarr APIs to queue downloads

**Database (EF Core + PostgreSQL 16):**

Tables managed via EF Core migrations:
- `Users` — app accounts (Email, Username, TelegramChatId unique indexes)
- `Movies` — scanned movie library
- `TvShows` + `Episodes` — scanned TV library
- `WatchHistory` — per-user playback progress (unique per user+movie and user+episode)
- `Favorites` — saved favorites per user
- `TelegramUsers` — authorized Telegram users (join code: `iamrockhard`)
- `TelegramRequests` — messages/requests from Telegram
- `MediaRequests` — structured download requests

Key `OnModelCreating` configuration:
- `TelegramRequest.ChatId` → FK to `TelegramUser.ChatId` (not UserId — prevents phantom column crash)
- Unique partial indexes on WatchHistory to allow upsert on progress

**Migrations timeline:**
- `20260405` — InitialCreate
- `20260420` — AddFavorites
- `20260520` — AddTelegramUsers
- `20260604` — AddMediaRequests

### Client (rockflix-client — React 19 + Vite)

**Dockerfile:** Node 20 Alpine build → nginx:alpine runtime. Vite builds to `/dist`, served from `/usr/share/nginx/html`.

**Pages:**
- `LoginPage` / `RegisterPage` — auth
- `HomePage` — main library browse
- `MovieDetailPage` — movie info + request/play
- `TvShowPage` — season/episode browser
- `PlayerPage` — video player
- `FavoritesPage` — saved favorites
- `TelegramUsersPage` — admin view of authorized Telegram users

**Routing:** React Router DOM

**Auth:** `AuthContext.jsx` — JWT stored in memory/context, passed via Authorization header

**API layer:** `src/services/api.js` — centralized Axios/fetch wrapper

### nginx (Client Container)

```nginx
location / {
    root /usr/share/nginx/html;
    try_files $uri $uri/ /index.html;  # SPA routing
}

location /api {
    proxy_pass http://api:5244;
    proxy_buffering off;               # Video streaming
    proxy_read_timeout 3600s;
    proxy_set_header Range $http_range;
    proxy_force_ranges on;             # Seeking support
}
```

---

## Public Access — Cloudflare Tunnel

Single dashboard-managed tunnel running in the Cloudflared container on CT 101 (`media-pipeline` stack). No ports exposed to the public internet — all traffic goes through Cloudflare's edge.

| Hostname | Routes to | Purpose |
|---|---|---|
| `watch.aidenswanson.com` | `http://localhost:3000` (CT 101) | Rockflix media server |
| `budget.aidenswanson.com` | `http://192.168.86.102:3001` (CT 102) | Budget app |

**Telegram webhook:** `https://watch.aidenswanson.com/api/telegram`
**Plaid webhook:** `https://budget.aidenswanson.com/api/plaid/webhook`

To add more hostnames: Zero Trust dashboard → Networks → Tunnels → open the tunnel → Public Hostname → Add.

---

## Telegram Bot

- Users join by messaging the bot with the invite code (`iamrockhard`)
- Once authorized, users can send natural language requests ("add Inception", "download Breaking Bad season 2")
- `MediaRequestService` passes the message to Claude Haiku, which parses intent and media title, then calls Radarr or Sonarr accordingly
- Webhook receives updates at `/api/telegram`

---

## Update Workflow

To deploy code changes to CT 101:

```bash
# 1. On dev machine — push to GitHub
git push

# 2. SSH into CT 101
ssh aiden@192.168.86.101

# 3. Pull and rebuild
cd /opt/rockflix
git pull
docker compose build api --no-cache
docker compose up -d api
```

---

## CT 103 — Home Assistant

Receives webhook POSTs from the washer/dryer sensor (ESP32-C3 + MPU6050) and triggers automations (phone notifications, etc.).

### LXC Setup (on Proxmox host)

Home Assistant runs as **Home Assistant Container** (Docker) inside a dedicated unprivileged LXC.

```bash
# 1. Create the LXC — Debian 12, 2 cores, 2GB RAM, 8GB disk
pct create 103 /var/lib/vz/template/cache/<debian-12-template>.tar.zst \
  --hostname homeassistant \
  --cores 2 \
  --memory 2048 \
  --net0 name=eth0,bridge=vmbr0,ip=192.168.86.103/24,gw=192.168.86.1 \
  --storage local-lvm \
  --rootfs local-lvm:8 \
  --unprivileged 1 \
  --features nesting=1

pct start 103
pct enter 103
```

```bash
# 2. Inside CT 103 — install Docker
apt update && apt install -y curl
curl -fsSL https://get.docker.com | sh

# 3. Create config directory
mkdir -p /opt/homeassistant/config

# 4. Run Home Assistant Container
docker run -d \
  --name homeassistant \
  --restart unless-stopped \
  --network host \
  -e TZ=America/Denver \
  -v /opt/homeassistant/config:/config \
  ghcr.io/home-assistant/home-assistant:stable
```

Home Assistant UI will be available at `http://192.168.86.103:8123` after first-run setup (~60 seconds).

### Washer Webhook Configuration

After completing onboarding in the HA UI:

1. Go to **Settings → Automations → Create Automation → Start with an empty automation**
2. Add trigger: **Webhook** — set Webhook ID to `washer_started`
3. Add action: **Send notification** (or any action you want)
4. Save. Repeat for a second automation with Webhook ID `washer_done`.

Alternatively, add this to `configuration.yaml` in `/opt/homeassistant/config/`:

```yaml
automation:
  - alias: "Washer Started"
    trigger:
      - platform: webhook
        webhook_id: washer_started
    action:
      - service: notify.mobile_app_<your_phone>
        data:
          message: "Washer is running."

  - alias: "Washer Done"
    trigger:
      - platform: webhook
        webhook_id: washer_done
    action:
      - service: notify.mobile_app_<your_phone>
        data:
          message: "Washer is done!"
```

Restart HA after editing `configuration.yaml`: **Developer Tools → Restart**.

The full webhook URLs the ESP32 hits:
```
POST http://192.168.86.103:8123/api/webhook/washer_started
POST http://192.168.86.103:8123/api/webhook/washer_done
```

### Useful Commands

```bash
docker logs homeassistant -f          # live logs
docker restart homeassistant          # restart HA
docker pull ghcr.io/home-assistant/home-assistant:stable && docker restart homeassistant  # update
```

---

## Washer/Dryer Sensor

An ESP32-C3 (Seeed XIAO) + MPU6050 vibration sensor that detects when the washing machine is running and POSTs webhooks to Home Assistant.

### Hardware

| Component | Details |
|---|---|
| Microcontroller | Seeed Studio XIAO ESP32-C3 |
| Accelerometer | MPU6050 (I2C) — SDA: GPIO6, SCL: GPIO7 |
| Power | USB-C wired |
| Mounting | Magnets on washer side panel |

**Before flashing**, fill in the three constants at the top of `sens_light_up.ino`:

```cpp
const char* SSID      = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";
const char* HA_HOST   = "http://192.168.86.103:8123";
```

### How It Works

- Reads X/Y/Z acceleration every 50ms and diffs against the previous reading
- If any axis changes by more than `THRESHOLD` (default `0.3g`), counts as a shake
- **5 consecutive seconds of shaking** → fires `washer_started` webhook, state = WASHING
- **60 consecutive seconds of quiet** while WASHING → fires `washer_done` webhook, state = IDLE

### Tuning

```cpp
const float THRESHOLD = 0.3;          // g-force delta per tick — raise if false triggers, lower if it misses gentle cycles
const int SHAKE_CONFIRM_TICKS = 100;  // 100 ticks × 50ms = 5s to confirm STARTED
const int QUIET_CONFIRM_TICKS = 1200; // 1200 ticks × 50ms = 60s to confirm DONE
```

**Calibration note:** The MPU6050 runs `calcOffsets()` on boot — sensor must be still. If the washer is already running when the device powers on, re-seat the sensor and reboot.

---

## Pending / Remaining Phases

- **Phase 9:** Proxmox backup jobs
- **Phase 10:** Tailscale VPN + Pi-hole on mobile
- **Phase 11:** Telegram bot for Proxmox host monitoring
- **DHCP reservations** for all LXC IPs (via Google Home app / router)
- **Media storage expansion** — NVMe will fill fast, need a large HDD

---

## Network Map

```
192.168.86.1    — Router / gateway
192.168.86.31   — Proxmox host
192.168.86.100  — CT 100: Pi-hole (DNS)
192.168.86.101  — CT 101: Docker (Portainer :9443, Rockflix :3000, Radarr :7878, Sonarr :8989, qBittorrent :8080, Prowlarr :9696, Uptime Kuma :3001, Cloudflared)
192.168.86.102  — CT 102: Budget App (Express+React :3001, Postgres :5432)
192.168.86.103  — CT 103: Home Assistant (:8123) — washer/dryer webhook target

Public: watch.aidenswanson.com  → Cloudflare tunnel → CT 101:3000
        budget.aidenswanson.com → Cloudflare tunnel → CT 102:3001
```
