# openWeatherFaxSSB

**Récepteur libre de cartes météo marines en fac-similé (WEFAX), RTTY et NAVTEX, reçues en HF/BLU.**
*Free, receive-only HF weather fax (WEFAX), RTTY and NAVTEX decoder for sailors — see [English summary](#english-summary).*

[![CI](https://github.com/Bob56fr/openWeatherFaxSSB/actions/workflows/ci.yml/badge.svg)](https://github.com/Bob56fr/openWeatherFaxSSB/actions/workflows/ci.yml)
[![License: AGPL-3.0-or-later](https://img.shields.io/badge/license-AGPL--3.0--or--later-blue.svg)](LICENSE)

> ⚠️ **Projet en phase de démarrage.** Aucune version utilisable n'est encore publiée.
> La spécification fait référence : [docs/SPECIFICATION.md](docs/SPECIFICATION.md).

---

## Pourquoi

Les cartes météo en fac-similé restent disponibles gratuitement en mer, sans Internet ni satellite. Les logiciels existants sont souvent anciens, fermés ou réservés au PC. openWeatherFaxSSB vise à :

- **Essayer sans rien acheter** : un vieux téléphone Android ou un vieux PC branché sur le casque de la BLU suffit. Au pire, on pose le micro du téléphone devant le haut-parleur.
- **Recevoir en tâche de fond** : une carte prend 10 à 20 minutes, inutile de monopoliser le terminal.
- **Évoluer ensuite** : Raspberry Pi en station autonome, un ou plusieurs SDR, transceiver piloté en CAT (Hamlib).
- **Consulter depuis n'importe quel appareil du bord**, iPhone compris, grâce à l'interface web. Le Raspberry Pi crée son propre réseau Wi-Fi, aucun réseau existant n'est nécessaire.
- **Améliorer les images par DSP** sans jamais inventer d'information : l'image brute est toujours conservée.

Le logiciel est **strictement en réception** : il n'émet jamais, y compris quand il pilote un transceiver.

## Plateformes

| Plateforme | Minimum |
|---|---|
| Android (smartphone, tablette) | 5.0 |
| Windows | 10 / 11 (7 et 8.1 en best effort) |
| Linux | Debian 12, Raspberry Pi OS (Debian 11 en best effort) |
| Raspberry Pi (station autonome) | Pi 3B, Pi Zero 2 W |
| iOS, macOS | via l'interface web d'une station |

## Feuille de route

| Version | Contenu |
|---|---|
| **V1** | Entrées audio (jack, micro, USB, Bluetooth), décodage WEFAX complet, tâche de fond, interface web FR/EN, station Raspberry Pi en point d'accès Wi-Fi |
| **V2** | SDR, CAT Hamlib, réception planifiée, catalogue mis à jour automatiquement, RTTY et NAVTEX |
| **V3** | Plusieurs SDR, diversité de fréquence, export OpenCPN |

Détails : [docs/SPECIFICATION.md §16](docs/SPECIFICATION.md#16-feuille-de-route).

## Compiler le cœur

Prérequis : CMake ≥ 3.18 et un compilateur C++17 (GCC ≥ 8, Clang ≥ 7, MSVC 2019+).
Linux : `libasound2-dev` en plus pour l'entrée carte son ALSA (détectée automatiquement).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure      # CMake ≥ 3.20, sinon : cd build && ctest
./build/apps/cli/owfax-cli --capabilities
```

Décoder un enregistrement, ou tester avec un signal synthétique (désaccord BLU, erreur d'horloge, bruit) :

```sh
owfax-cli synth test.wav --offset 60 --ppm 250 --snr 15 --png source.png
owfax-cli decode test.wav carte.png
```

Réception en continu avec le moteur `owfaxd` (receive only : il ne pilote pas le TRX). Chaque carte donne `<stem>_live.png` (aperçu direct, réécrit pendant la réception), `<stem>.png` (brut), `<stem>_enh.png` (amélioré) et `<stem>.json` ; `status.json` est rafraîchi toutes les 5 s :

```sh
owfaxd --list-inputs
owfaxd --input alsa:plughw:CARD=Device,DEV=0 --out cartes --label DDH3 --shift 850 --journal journal.log
owfaxd --input wav:capture.wav --out cartes          # rejouer un enregistrement
arecord -t raw -f S16_LE -c1 -r 48000 | owfaxd --input stdin --rate 48000 --out cartes
```

Une carte démarre sur le ton APT, ou, s'il a été manqué ou haché par le QRM, sur le signal fax lui-même (des lignes qui se répètent d'une ligne à l'autre, ce que ne font ni le bruit ni la porteuse). Elle se termine sur le ton de fin, ou après 60 s de bruit si ce ton est perdu ; deux cartes dont les tons ont tous été perdus sont séparées par leur phasing. Le champ `start` du JSON indique `apt`, `signal` ou `forced`. `--no-auto-start` revient au démarrage sur APT seul.

Avec CMake ≥ 3.21, des presets sont disponibles : `cmake --preset dev`, `msvc-x64`, `msvc-x86`, `linux-armhf`, `android-arm64`, `android-armv7` (voir [CMakePresets.json](CMakePresets.json)).

## Structure du dépôt

```
core/        Bibliothèque C++17 portable : sources, DSP, décodeurs, moteur de réception
io/          Entrées matérielles (ALSA ; WASAPI et Android à venir), hors du cœur
apps/cli/    Outil en ligne de commande (développement, usage headless)
apps/daemon/ owfaxd, moteur de réception en tâche de fond
tests/       Tests unitaires (sans dépendance externe)
cmake/       Modules CMake et toolchains de cross-compilation
docs/        Spécification et documentation
```

À venir : `web/` (interface web), `android/` (application et service), `station/` (image Raspberry Pi), `catalog/` (schéma du catalogue des stations).

## Contribuer

Le projet est **développé en solo pour le moment** : les pull requests ne sont pas acceptées. Les **issues sont bienvenues**, en français ou en anglais : retours d'usage, enregistrements WAV de réceptions réelles, horaires de stations à corriger. Voir [CONTRIBUTING.md](CONTRIBUTING.md).

## Licence

Copyright © 2026 les contributeurs d'openWeatherFaxSSB.

Ce programme est un logiciel libre, distribué sous **GNU Affero General Public License v3.0 ou ultérieure** ([LICENSE](LICENSE)). Toute personne qui utilise le logiciel, y compris via son interface web, a le droit d'obtenir son code source.

---

## English summary

openWeatherFaxSSB is a free (AGPL-3.0-or-later), **receive-only** decoder for HF radiofacsimile weather charts, weather RTTY and NAVTEX, aimed at sailors.

- Works on old, recycled hardware so anyone can try it at no cost: Android 5+, Windows 10/11 (7 best effort), Debian, Raspberry Pi.
- Audio input from the SSB receiver (line, headset, USB, Bluetooth, or the phone's microphone), SDRs, or Hamlib-controlled transceivers.
- Runs in the background; a Raspberry Pi acts as its own Wi-Fi access point and serves a web UI to any device on board, including iOS.
- The specification ([docs/SPECIFICATION.md](docs/SPECIFICATION.md)) is written in French.
- Currently developed by a single maintainer: pull requests are not accepted yet, but issues (feedback, WAV recordings, schedule fixes) are welcome in English or French.
