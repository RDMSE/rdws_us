#!/usr/bin/env bash
# db/init_server.sh — configure PostgreSQL on Fedora server
# run on server via SSH:
#   ssh user@fedora-server.local 'bash -s' < db/init_server.sh
# Or copy and run directly:
#   scp db/init_server.sh user@fedora-server.local:~/ && ssh user@fedora-server.local 'sudo bash init_server.sh'
set -euo pipefail

DB_NAME="rdws_us_dev"
DB_USER="rdws_user"
DB_PASSWORD="rdws_password"

echo "==> Installing PostgreSQL + PostGIS + pgcrypto..."
dnf install -y postgresql-server postgresql-contrib postgis

echo "==> Initializing PostgreSQL cluster..."
if [[ ! -f /var/lib/pgsql/data/PG_VERSION ]]; then
    postgresql-setup --initdb
else
    echo "    SKIP — cluster already initialized"
fi

echo "==> Enabling and starting PostgreSQL..."
systemctl enable --now postgresql

echo "==> Creating user and database..."
sudo -u postgres psql <<SQL
DO \$\$
BEGIN
    IF NOT EXISTS (SELECT FROM pg_roles WHERE rolname = '${DB_USER}') THEN
        CREATE USER ${DB_USER} WITH PASSWORD '${DB_PASSWORD}';
        RAISE NOTICE 'User ${DB_USER} created.';
    ELSE
        RAISE NOTICE 'User ${DB_USER} already exists — skipped.';
    END IF;
END
\$\$;

SELECT 1 FROM pg_database WHERE datname = '${DB_NAME}' \gset
\if :{?1}
    \echo 'Database ${DB_NAME} already exists — skipped.'
\else
    CREATE DATABASE ${DB_NAME}
        OWNER = ${DB_USER}
        ENCODING = 'UTF8'
        LC_COLLATE = 'en_US.UTF-8'
        LC_CTYPE = 'en_US.UTF-8'
        TEMPLATE = template0;
    \echo 'Banco ${DB_NAME} criado.'
\endif

GRANT ALL PRIVILEGES ON DATABASE ${DB_NAME} TO ${DB_USER};
SQL

echo "==> Installing extensions in ${DB_NAME}..."
sudo -u postgres psql -d "$DB_NAME" <<SQL
CREATE EXTENSION IF NOT EXISTS postgis;
CREATE EXTENSION IF NOT EXISTS pgcrypto;
GRANT ALL ON SCHEMA public TO ${DB_USER};
SQL

echo ""
echo "==> Configuring pg_hba.conf to accept password connections..."
PG_HBA=$(sudo -u postgres psql -tAc "SHOW hba_file;")
# Add md5 rule for rdws_user if it doesn't exist
if ! grep -q "${DB_USER}" "$PG_HBA" 2>/dev/null; then
    echo "host    ${DB_NAME}    ${DB_USER}    0.0.0.0/0    md5" >> "$PG_HBA"
    echo "    Rule added to pg_hba.conf"
    systemctl reload postgresql
else
    echo "    SKIP — rule already exists in pg_hba.conf"
fi

echo ""
echo "==> Done. Check the connection with:"
echo "    psql -h fedora-server.local -U ${DB_USER} -d ${DB_NAME}"
