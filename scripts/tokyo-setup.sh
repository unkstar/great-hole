#!/bin/bash
# Tokyo VPS setup script — run as root on a fresh Debian 12/13 install
#
# Usage:
#   1. Copy this script to the Tokyo VPS:  scp -P 29832 tokyo-setup.sh root@202.144.195.145:/tmp/
#   2. Run as root:  bash /tmp/tokyo-setup.sh
#
# This script does NOT install great-hole-fec (use the .deb package).
# It sets up everything else: packages, users, lighttpd, bestroutetb,
# EdgeOS-Utils, iptables, cron, and SSH hardening.
set -euo pipefail

echo "=== Tokyo VPS Setup ==="
echo "Started at $(date -u '+%Y-%m-%d %H:%M:%S UTC')"

# ────────────────────────────────────────────────────────────
# 1. System update
# ────────────────────────────────────────────────────────────
echo "[1/12] Updating system packages..."
apt update && apt upgrade -y

# ────────────────────────────────────────────────────────────
# 2. Install required packages
# ────────────────────────────────────────────────────────────
echo "[2/12] Installing packages..."
apt install -y \
    nodejs \
    npm \
    lighttpd \
    curl \
    git \
    iperf3 \
    build-essential \
    iptables-persistent \
    ca-certificates

# ────────────────────────────────────────────────────────────
# 3. Create ggcuser (if not exists)
# ────────────────────────────────────────────────────────────
echo "[3/12] Creating ggcuser..."
if ! id -u ggcuser &>/dev/null; then
    useradd -m ggcuser
    echo "  User ggcuser created."
else
    echo "  User ggcuser already exists, skipping."
fi

# Set up SSH authorized_keys for ggcuser
mkdir -p /home/ggcuser/.ssh
chmod 700 /home/ggcuser/.ssh
touch /home/ggcuser/.ssh/authorized_keys
chmod 600 /home/ggcuser/.ssh/authorized_keys
chown -R ggcuser:ggcuser /home/ggcuser/.ssh

# ────────────────────────────────────────────────────────────
# 4. Harden SSH configuration
# ────────────────────────────────────────────────────────────
echo "[4/12] Configuring SSH..."
SSHD_CONFIG="/etc/ssh/sshd_config"

# Backup original config
if [ ! -f "${SSHD_CONFIG}.orig" ]; then
    cp "$SSHD_CONFIG" "${SSHD_CONFIG}.orig"
fi

# Change port
sed -i 's/^#*Port .*/Port 29832/' "$SSHD_CONFIG"
# Disable password authentication
sed -i 's/^#*PasswordAuthentication .*/PasswordAuthentication no/' "$SSHD_CONFIG"
# Disable root login
sed -i 's/^#*PermitRootLogin .*/PermitRootLogin no/' "$SSHD_CONFIG"
# Ensure key authentication is enabled
sed -i 's/^#*PubkeyAuthentication .*/PubkeyAuthentication yes/' "$SSHD_CONFIG"

systemctl reload sshd
echo "  SSH reconfigured: port=29832, password=no, root=no"

# ────────────────────────────────────────────────────────────
# 5. Install bestroutetb (npm global)
# ────────────────────────────────────────────────────────────
echo "[5/12] Installing bestroutetb..."
BESTROUTE_DIR="/usr/local/lib/node_modules/bestroutetb"
TMP_BESTROUTE="/tmp/bestroutetb"

if [ -d "$BESTROUTE_DIR" ]; then
    echo "  bestroutetb already installed at $BESTROUTE_DIR, skipping."
else
    mkdir -p /usr/local/lib/node_modules
    cd /tmp
    if [ ! -d "$TMP_BESTROUTE" ]; then
        git clone git@github.com:unkstar/bestroutetb.git -b npm "$TMP_BESTROUTE" || {
            echo "  WARNING: git clone via SSH failed."
            echo "  Trying HTTPS fallback..."
            git clone https://github.com/unkstar/bestroutetb.git -b npm "$TMP_BESTROUTE" || {
                echo "  WARNING: bestroutetb clone failed (no SSH key yet?)."
                echo "  You can install it later with:"
                echo "    git clone git@github.com:unkstar/bestroutetb.git -b npm /usr/local/lib/node_modules/bestroutetb"
                echo "    cd /usr/local/lib/node_modules/bestroutetb && npm install"
            }
        }
    fi
    if [ -d "$TMP_BESTROUTE" ]; then
        cd "$TMP_BESTROUTE" && npm install
        cp -r "$TMP_BESTROUTE" "$BESTROUTE_DIR"
        echo "  bestroutetb installed to $BESTROUTE_DIR"
    fi
fi

# ────────────────────────────────────────────────────────────
# 6. Deploy EdgeOS-Utils
# ────────────────────────────────────────────────────────────
echo "[6/12] Deploying EdgeOS-Utils..."
EDGEOS_DIR="/var/www/EdgeOS-Utils"

if [ -d "$EDGEOS_DIR/.git" ]; then
    echo "  EdgeOS-Utils already exists, pulling latest..."
    cd "$EDGEOS_DIR" && git pull || echo "  WARNING: git pull failed"
else
    mkdir -p /var/www
    cd /var/www
    git clone git@github.com:unkstar/EdgeOS-Utils.git "$EDGEOS_DIR" || {
        echo "  WARNING: git clone via SSH failed."
        echo "  Trying HTTPS fallback..."
        git clone https://github.com/unkstar/EdgeOS-Utils.git "$EDGEOS_DIR" || {
            echo "  WARNING: EdgeOS-Utils clone failed (no SSH key yet?)."
        }
    }
fi

if [ -d "$EDGEOS_DIR" ]; then
    chown -R www-data:www-data "$EDGEOS_DIR"
    chmod +x "$EDGEOS_DIR/makebestroutetb" 2>/dev/null || true
    chmod +x "$EDGEOS_DIR/gen-routes.js" 2>/dev/null || true
    # Also ensure makebestroutetb-all is executable if it exists
    [ -f "$EDGEOS_DIR/makebestroutetb-all" ] && chmod +x "$EDGEOS_DIR/makebestroutetb-all"
    [ -f "$EDGEOS_DIR/update_gfwlist_dnsmasq.sh" ] && chmod +x "$EDGEOS_DIR/update_gfwlist_dnsmasq.sh"
    echo "  EdgeOS-Utils deployed to $EDGEOS_DIR"
fi

# ────────────────────────────────────────────────────────────
# 7. Create routes directory
# ────────────────────────────────────────────────────────────
echo "[7/12] Creating routes directory..."
mkdir -p /var/www/routes
chown www-data:www-data /var/www/routes
echo "  /var/www/routes ready"

# ────────────────────────────────────────────────────────────
# 8. Create lighttpd symlink for web access
# ────────────────────────────────────────────────────────────
echo "[8/12] Setting up lighttpd web root..."
mkdir -p /var/www/html
ln -sf /var/www/routes /var/www/html/routes 2>/dev/null || true
chown -R www-data:www-data /var/www/html
echo "  Symlink: /var/www/html/routes -> /var/www/routes"

# ────────────────────────────────────────────────────────────
# 9. Configure lighttpd (bind fec-tokyo only: 172.31.30.2:80)
# ────────────────────────────────────────────────────────────
echo "[9/12] Configuring lighttpd..."
cat > /etc/lighttpd/lighttpd.conf << 'LIGHTTPD'
server.modules = (
    "mod_access",
    "mod_alias",
    "mod_accesslog",
)

server.document-root = "/var/www/html"
server.bind = "172.31.30.2"
server.port = 80
server.username = "www-data"
server.groupname = "www-data"

index-file.names = ("index.html")

mimetype.assign = (
    ".html" => "text/html",
    ".txt"  => "text/plain",
    ".sh"   => "text/plain",
)

alias.url = ("/routes" => "/var/www/routes")
LIGHTTPD

echo "  lighttpd configured: bind 172.31.30.2:80 (fec-tokyo)"
echo "  Note: lighttpd will fail to start until fec-tokyo interface exists."
echo "  Start it after great-hole-fec creates fec-tokyo."

# ────────────────────────────────────────────────────────────
# 10. Create iptables persistent rules
# ────────────────────────────────────────────────────────────
echo "[10/12] Setting up iptables persistent rules..."
mkdir -p /etc/iptables
cat > /etc/iptables/rules.v4 << 'IPTABLES'
*nat
-A POSTROUTING -s 172.31.17.0/24 -o eth0 -j MASQUERADE
-A POSTROUTING -s 172.31.16.0/24 -o eth0 -j MASQUERADE
COMMIT
*filter
:INPUT ACCEPT [0:0]
:FORWARD ACCEPT [0:0]
:OUTPUT ACCEPT [0:0]
COMMIT
IPTABLES

echo "  iptables rules written to /etc/iptables/rules.v4"

# ────────────────────────────────────────────────────────────
# 11. Set up cron jobs
# ────────────────────────────────────────────────────────────
echo "[11/12] Setting up cron jobs..."
cat > /tmp/tokyo-cron << 'CRON'
# Tokyo VPS crontab — installed by tokyo-setup.sh
00 23 * * * /var/www/EdgeOS-Utils/makebestroutetb-all /var/www/routes/ >> /var/log/update-routes.log 2>&1
00 23 * * * /var/www/EdgeOS-Utils/update_gfwlist_dnsmasq.sh
CRON

crontab -u root /tmp/tokyo-cron
rm -f /tmp/tokyo-cron

# Ensure log file exists
touch /var/log/update-routes.log
chmod 644 /var/log/update-routes.log

echo "  Cron installed: daily route generation at 23:00 UTC"

# ────────────────────────────────────────────────────────────
# 12. Set timezone to UTC
# ────────────────────────────────────────────────────────────
echo "[12/12] Setting timezone..."
timedatectl set-timezone UTC
echo "  Timezone set to UTC (cron runs at 23:00 UTC = 08:00 JST)"

# ────────────────────────────────────────────────────────────
# Done
# ────────────────────────────────────────────────────────────
echo ""
echo "=== Tokyo VPS setup complete ==="
echo "Completed at $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
echo ""
echo "Next steps (manual):"
echo "  1. Install great-hole-fec .deb package"
echo "  2. Copy FEC config files:"
echo "     scp fec-tokyo.lua root@202.144.195.145:/etc/great-hole/fec/tokyo.lua"
echo "     scp scripts/tokyo-up.sh root@202.144.195.145:/etc/great-hole/fec/tokyo-up.sh"
echo "  3. Set up great-hole-fec systemd service to run on boot"
echo "  4. Start great-hole-fec: systemctl start great-hole-fec-tokyo"
echo "  5. Verify fec-tokyo interface exists: ip addr show fec-tokyo"
echo "  6. Start lighttpd: systemctl start lighttpd"
echo "  7. Test from Ali: ping 172.31.30.2"
echo "  8. Test web access from Ali: curl http://172.31.30.2/routes/"
echo "  9. Add SSH public key to /home/ggcuser/.ssh/authorized_keys"
echo " 10. Add SSH deploy key for GitHub (for git clone of bestroutetb, EdgeOS-Utils)"
echo " 11. Run initial route generation:"
echo "     sudo -u www-data /var/www/EdgeOS-Utils/makebestroutetb-all /var/www/routes/"
echo ""
echo "Firewall ports needed (inbound):"
echo "  UDP 11555, 11556 — great-hole-fec"
echo "  TCP 29832 — SSH"
