#!/bin/bash
# Copie l'APK signée vers le serveur web (F5ZBC) : tools/deploy-apk.sh [utilisateur@hote] [dossier]
# Accès par clé conseillé (une fois) : ssh-copy-id utilisateur@192.168.0.13
set -e
APK="$(dirname "$0")/../dist/android/fr.f4dvk.uvlink-Signed.apk"
HOST="${1:-stephane@192.168.0.13}"
DIR="${2:-/var/www/f5zbc/logiciel}"
[ -f "$APK" ] || { echo "APK absente : $APK (lancer la publication Android d'abord)"; exit 1; }
echo "-> $HOST:$DIR/uvlink.apk ($(du -h "$APK" | cut -f1))"
scp "$APK" "$HOST:$DIR/uvlink.apk"
ssh "$HOST" "ls -la $DIR/uvlink.apk"
