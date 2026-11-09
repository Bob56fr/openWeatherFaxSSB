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
| Linux | Debian 11, Raspberry Pi OS |
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

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure      # CMake ≥ 3.20, sinon : cd build && ctest
./build/apps/cli/owfax-cli --capabilities
```

Avec CMake ≥ 3.21, des presets sont disponibles : `cmake --preset dev`, `msvc-x64`, `msvc-x86`, `linux-armhf`, `android-arm64`, `android-armv7` (voir [CMakePresets.json](CMakePresets.json)).

## Structure du dépôt

```
core/        Bibliothèque C++17 portable : sources, DSP, décodeurs, planificateur, API
apps/cli/    Outil en ligne de commande (développement, usage headless)
tests/       Tests unitaires (sans dépendance externe)
cmake/       Modules CMake et toolchains de cross-compilation
docs/        Spécification et documentation
```

À venir : `web/` (interface web), `android/` (application et service), `station/` (image Raspberry Pi), `catalog/` (schéma du catalogue des stations).

## Contribuer

Les contributions sont bienvenues, en français ou en anglais : code, traductions, enregistrements WAV de réceptions réelles pour le corpus de tests, mise à jour des horaires de stations. Lire [CONTRIBUTING.md](CONTRIBUTING.md). Chaque commit doit être signé (`git commit -s`, voir [DCO](DCO)).

## Licence

Copyright © 2026 les contributeurs d'openWeatherFaxSSB.

Ce programme est un logiciel libre, distribué sous **GNU Affero General Public License v3.0 ou ultérieure** ([LICENSE](LICENSE)). Toute personne qui utilise le logiciel, y compris via son interface web, a le droit d'obtenir son code source.

---

## English summary

openWeatherFaxSSB is a free (AGPL-3.0-or-later), **receive-only** decoder for HF radiofacsimile weather charts, weather RTTY and NAVTEX, aimed at sailors.

- Works on old, recycled hardware so anyone can try it at no cost: Android 5+, Windows 10/11 (7 best effort), Debian, Raspberry Pi.
- Audio input from the SSB receiver (line, headset, USB, Bluetooth, or the phone's microphone), SDRs, or Hamlib-controlled transceivers.
- Runs in the background; a Raspberry Pi acts as its own Wi-Fi access point and serves a web UI to any device on board, including iOS.
- The specification ([docs/SPECIFICATION.md](docs/SPECIFICATION.md)) is written in French. Issues and pull requests are welcome in English or French.
