# openWeatherFaxSSB — Spécification fonctionnelle et technique

| | |
|---|---|
| **Version** | 1.1 — 2026-11-09 (Debian 12 comme plancher Linux, phase de développement en solo) |
| **Statut** | Validée pour démarrage du développement |
| **Licence du projet** | AGPL-3.0-or-later |
| **Dépôt** | GitHub (public) |

Convention : chaque exigence porte un identifiant (`DOM-NNN`) pour la traçabilité dans les issues, PR et tests.
Niveaux : **DOIT** (obligatoire pour la version indiquée), **DEVRAIT** (fortement souhaité), **PEUT** (optionnel).

---

## 1. Contexte et objectifs

Les cartes météo marines en fac-similé (WEFAX) sont émises en HF et reçues en BLU (USB). Le mode est ancien, lent (10 à 20 min par carte) et sensible au bruit, mais reste disponible gratuitement en mer, sans Internet ni satellite.

openWeatherFaxSSB est un logiciel libre et multiplateforme qui :

1. reçoit et décode les cartes WEFAX, ainsi que les bulletins texte RTTY météo et NAVTEX ;
2. fonctionne **en tâche de fond**, sans monopoliser le terminal ;
3. tourne **en best effort sur du matériel ancien ou recyclé**, pour qu'un skipper puisse l'essayer sans rien acheter, puis évoluer vers une installation plus complète (Raspberry Pi, SDR, transceiver piloté en CAT) ;
4. améliore la qualité des images par un traitement DSP poussé, **sans jamais inventer d'information météo** ;
5. peut fonctionner en **station autonome** (Raspberry Pi sans écran), consultable et paramétrable depuis n'importe quel navigateur du bord, y compris iPhone, iPad et Mac.

### Hors périmètre

- Émission de quelque nature que ce soit : le logiciel est **strictement en réception**.
- Application iOS native : iOS est couvert par l'interface web d'une station.
- Android Auto, Wear OS, Android TV.
- Fichiers GRIB (reçus par d'autres canaux, par exemple e-mail satellite).

---

## 2. Plateformes et niveaux de matériel

### 2.1 Systèmes supportés

| Plateforme | Plancher | Architectures | Statut |
|---|---|---|---|
| Android smartphone / tablette | 5.0 (API 21) | armeabi-v7a, arm64-v8a, x86_64 | Supporté |
| Windows | 10 / 11 | x64, x86 | Supporté |
| Windows | 7 / 8.1 | x64, x86 | Best effort |
| Linux Debian / Raspberry Pi OS | Debian 12 (bookworm) | amd64, arm64, armhf | Supporté |
| Linux Debian / Raspberry Pi OS Legacy | Debian 11 (bullseye, fin de vie) | amd64, arm64, armhf | Best effort |
| Raspberry Pi (station) | Pi 3B / Zero 2 W | arm64, armhf | Supporté |

### 2.2 Niveaux de matériel

Le logiciel **DOIT** détecter les capacités du terminal au démarrage (CPU, RAM, version OS, périphériques) et activer ou désactiver les fonctions en conséquence, avec un message clair (« fonction indisponible sur ce matériel, raison : … »). Il ne doit jamais planter faute de ressources.

| Niveau | Matériel type | Fonctions disponibles |
|---|---|---|
| **Découverte** | Android 5–7, PC Windows 7, Pi 3B, Pi Zero 2 W | Entrées audio (jack, micro, USB, Bluetooth), WEFAX complet, détection APT, correction slant et calage, DSP de base, catalogue, interface web |
| **Standard** | Android 8+, Windows 10/11, Pi 4 | + CAT Hamlib (hors Android), 1 SDR, planification automatique, RTTY et NAVTEX, DSP avancé |
| **Avancé** | Pi 4 (4 Go+), Pi 5, PC récent | + plusieurs SDR, diversité de fréquence, réception simultanée de 2 stations |

- **PLT-001 (DOIT, V1)** — Le décodage WEFAX à partir d'une entrée audio fonctionne sur tout le niveau Découverte.
- **PLT-002 (DOIT, V1)** — Une seule base de code pour le cœur, compilée pour toutes les cibles listées.
- **PLT-003 (DEVRAIT, V1)** — Sur Windows 7, l'application fonctionne avec les navigateurs figés de cette plateforme (Chrome 109, Firefox ESR 115).

---

## 3. Architecture

```
┌──────────────────────── Cœur owfax-core (C++17, headless) ────────────────────────┐
│ Sources     : audio système (WASAPI / ALSA·PulseAudio / Oboe), USB audio,         │
│               Bluetooth SCO, fichier WAV, SDR (SoapySDR · librtlsdr · rtl_tcp)     │
│ Contrôle    : CAT via Hamlib (desktop et Pi uniquement)                           │
│ DSP         : démodulateurs WEFAX / RTTY / NAVTEX, synchro, slant, diversité       │
│ Planificateur : catalogue, horaires UTC, déclenchement APT, pilotage CAT/SDR       │
│ Stockage    : images brutes + traitées + métadonnées (SQLite)                      │
│ API         : HTTP REST + WebSocket (diffusion ligne par ligne en direct)          │
│ Serveur web : sert l'interface web (fichiers statiques)                            │
└───────────────────────────────────────────────────────────────────────────────────┘
        ▲ JNI                         ▲ service/daemon                ▲ daemon systemd
  App Android (Kotlin)          Windows / Linux desktop           Station Raspberry Pi
  - Foreground Service          - icône zone de notification      - point d'accès Wi-Fi
  - WebView sur l'UI web        - navigateur sur l'UI web         - UI web pour tous les clients
  - notifications natives       - notifications natives           - GPIO (LED/buzzer)
```

- **ARC-001 (DOIT)** — Le cœur est une bibliothèque C++17 sans dépendance graphique, compilable avec les NDK Android (API 21), MSVC et MinGW (Windows 7+), GCC et Clang (Linux armhf, arm64, amd64).
- **ARC-002 (DOIT)** — **Une seule interface utilisateur, en web**, servie par le cœur sur toutes les plateformes. Sur Android, elle est affichée dans un WebView, avec un repli « ouvrir dans le navigateur ».
- **ARC-003 (DOIT)** — L'interface web cible un JavaScript conservateur (ES2017), sans dépendance à un CDN, et fonctionne hors ligne. Elle est testée sur Chrome 109, Firefox ESR 115, Safari iOS 15 et un WebView Android 5.
- **ARC-004 (DOIT)** — Toutes les fonctions et tous les paramètres sont accessibles par l'interface web (parité totale, aucune configuration par fichier ou SSH nécessaire).
- **ARC-005 (DOIT)** — L'API du cœur est documentée (OpenAPI) et versionnée.
- **ARC-006 (DEVRAIT)** — Le moteur DSP est découplé des sources : toute source produit un flux d'échantillons normalisé (audio réel ou IQ complexe + horodatage + fréquence d'échantillonnage mesurée).

Bibliothèques pressenties (toutes compatibles AGPL-3.0) : Hamlib (LGPL), SoapySDR (Boost), librtlsdr (GPLv2+), liquid-dsp (MIT), pocketfft (BSD), Oboe (Apache-2.0), SQLite (domaine public), libpng/libjpeg-turbo, cpp-httplib ou équivalent (MIT).

---

## 4. Sources de signal

### 4.1 Entrées audio

- **SRC-001 (DOIT, V1)** — Entrée ligne ou micro de la carte son (jack casque-micro relié à la sortie du récepteur BLU).
- **SRC-002 (DOIT, V1)** — Micro intégré du terminal (couplage acoustique devant le haut-parleur), présenté comme mode de dernier recours.
- **SRC-003 (DOIT, V1)** — Sur Android, la capture utilise `AudioSource.UNPROCESSED` lorsque disponible, sinon `VOICE_RECOGNITION`, afin de désactiver l'AGC et la suppression de bruit qui détruisent les tons FAX.
- **SRC-004 (DOIT, V1)** — Carte son USB (classe UAC, y compris l'audio USB intégré aux transceivers).
- **SRC-005 (DOIT, V1)** — Casque ou adaptateur Bluetooth en profil HFP/SCO. L'interface signale que la qualité est réduite (bande étroite, compression).
- **SRC-006 (DOIT, V1)** — Fichier WAV en entrée (relecture, tests, analyse a posteriori).
- **SRC-007 (DOIT, V1)** — Indicateur de niveau d'entrée et aide au réglage du volume (zone verte, saturation).
- **SRC-008 (DEVRAIT, V1)** — Enregistrement optionnel de l'audio brut d'une réception, pour retraitement ultérieur et rapport de bug.

### 4.2 SDR

- **SRC-020 (DOIT, V2)** — Support d'au moins un SDR HF : RTL-SDR Blog V4 (HF natif), RTL-SDR V3 en échantillonnage direct, Airspy HF+.
- **SRC-021 (DOIT, V2)** — Desktop et Pi : accès via SoapySDR. Android : librtlsdr via USB host (libusb + permission USB Android) et client rtl_tcp.
- **SRC-022 (DOIT, V2)** — Démodulation USB interne avec calage automatique sur la fréquence de la station (aucun réglage manuel d'offset nécessaire).
- **SRC-023 (DOIT, V3)** — Plusieurs SDR simultanés (niveau Avancé) :
  - même station sur plusieurs fréquences (diversité, voir DSP-040) ;
  - deux stations différentes en parallèle.
- **SRC-024 (PEUT, V3)** — Un SDR large bande unique couvrant plusieurs fréquences d'une même bande (canaliseur).
- **SRC-025 (DOIT)** — L'API SDRplay (propriétaire) n'est pas liée dans les binaires officiels. Un pont externe (SoapyRemote) peut être documenté.
- **SRC-026 (DOIT, V2)** — Calibration de l'erreur de fréquence du SDR (ppm), mesurée automatiquement sur une porteuse connue ou saisie manuellement.

### 4.3 Transceiver piloté en CAT (desktop et Pi uniquement)

Le CAT n'est **pas** supporté sur Android.

- **CAT-001 (DOIT, V2)** — Pilotage via Hamlib, lié directement ou via un `rigctld` local ou distant.
- **CAT-002 (DOIT, V2)** — **Réception seule garantie par le code** : aucune commande PTT, émission, tuner ou modification de puissance n'est jamais envoyée. Cette garantie n'est pas une option désactivable.
- **CAT-003 (DOIT, V2)** — Réglage automatique selon le planning :
  - fréquence de cadran = fréquence assignée − 1,9 kHz ;
  - mode USB ou USB-DATA selon le modèle ;
  - filtre ≥ 2,8 kHz si réglable.
- **CAT-004 (DOIT, V2)** — Association explicite entre un TRX et son périphérique audio USB, avec détection automatique lorsque possible (même numéro de série ou même port USB).
- **CAT-005 (DEVRAIT, V2)** — Lecture du S-mètre, journalisée avec chaque réception.
- **CAT-006 (DEVRAIT, V2)** — Réglages de réception recommandés et applicables : AGC lente, noise blanker selon le QRN, réduction de bruit DSP du TRX désactivée.
- **CAT-007 (DEVRAIT, V2)** — Si la qualité est mauvaise au début d'une émission, basculement vers une autre fréquence de la même station selon le planning.
- **CAT-008 (DOIT, V2)** — Pour les configurations sans CAT, l'application affiche la fréquence de cadran à régler manuellement et le mode attendu.

---

## 5. Moteur de réception en tâche de fond

- **BG-001 (DOIT, V1)** — La réception se poursuit quand l'application n'est pas au premier plan, quand l'écran est éteint et quand l'interface web est fermée.
- **BG-002 (DOIT, V1)** — Android :
  - Foreground Service de type `microphone` (et `connectedDevice` pour l'USB) avec notification persistante ;
  - WakeLock partiel pendant les réceptions ;
  - demande guidée d'exclusion de l'optimisation de batterie ;
  - écran d'aide spécifique par constructeur (Xiaomi, Huawei, Samsung, Oppo…).
- **BG-003 (DOIT, V2)** — Android : démarrage planifié via `AlarmManager` exact (permission `SCHEDULE_EXACT_ALARM`). Le service est rendu actif par l'utilisateur avant la première réception planifiée, conformément aux contraintes Android 14+.
- **BG-004 (DOIT, V1)** — Windows : processus résident avec icône dans la zone de notification. Démarrage avec la session en option.
- **BG-005 (DOIT, V1)** — Linux et Pi : service systemd, démarrage au boot.
- **BG-006 (DOIT, V1)** — Reprise automatique après perte de la source audio ou SDR (débranchement, coupure Bluetooth), avec notification.
- **BG-007 (DEVRAIT, V1)** — Mode économie d'énergie : pas de calcul d'image avancé pendant la réception (seulement le décodage), post-traitement après la fin de l'émission.

---

## 6. Décodage WEFAX et traitement du signal

### 6.1 Paramètres du mode

- Sous-porteuse : noir 1500 Hz, blanc 2300 Hz (centre 1900 Hz).
- IOC 576 et 288 ; vitesses 120, 90 et 60 LPM.
- APT : démarrage 300 Hz (IOC 576) ou 675 Hz (IOC 288), arrêt 450 Hz ; signal de phasage.

### 6.2 Exigences de décodage

- **DSP-001 (DOIT, V1)** — Démodulation FM par signal analytique (filtre de Hilbert) et discriminateur de phase, avec filtrage adapté à l'IOC et au LPM.
- **DSP-002 (DOIT, V1)** — Détection automatique de l'APT de démarrage et d'arrêt, de l'IOC et du LPM.
- **DSP-003 (DOIT, V1)** — Alignement horizontal automatique à partir du signal de phasage, avec repli par corrélation de la marge si le phasage est manqué.
- **DSP-004 (DOIT, V1)** — **Correction automatique du calage BLU** : estimation des fréquences réelles du noir et du blanc, recentrage du démodulateur et indication à l'utilisateur de l'erreur de calage en Hz.
- **DSP-005 (DOIT, V1)** — **Correction automatique du slant** : estimation de l'erreur d'horloge de la carte son (ppm) par corrélation entre lignes successives, mémorisée par périphérique et affinée à chaque réception.
- **DSP-006 (DOIT, V1)** — Démarrage manuel possible en cours d'émission (APT manqué), avec recalage a posteriori.
- **DSP-007 (DOIT, V1)** — Affichage de l'image ligne par ligne pendant la réception (local et via WebSocket).
- **DSP-008 (DOIT, V1)** — Indicateur de qualité par ligne (SNR estimé, impulsions), conservé dans les métadonnées.

### 6.3 Amélioration du signal et de l'image

- **DSP-020 (DOIT, V1)** — Blanker d'impulsions adaptatif (QRN, orages, parasites de bord).
- **DSP-021 (DOIT, V1)** — Détection des lignes dégradées ou perdues ; interpolation verticale ou marquage visuel, selon le choix de l'utilisateur.
- **DSP-022 (DOIT, V2)** — Post-traitement d'image :
  - débruitage morphologique adapté aux cartes (traits, texte) ;
  - nettoyage des points isolés ;
  - seuillage adaptatif optionnel ;
  - réglage de contraste automatique.
- **DSP-023 (DEVRAIT, V2)** — AGC logicielle et suivi lent du calage pendant l'émission (dérive du récepteur).
- **DSP-030 (DOIT, toutes versions)** — **Intégrité de l'information** :
  - l'image brute est toujours conservée ;
  - toute image traitée est marquée comme telle ;
  - aucun traitement génératif (IA, super-résolution par apprentissage ou autre) susceptible de créer des isobares, fronts, chiffres ou textes absents du signal n'est autorisé.

### 6.4 Diversité de fréquence

- **DSP-040 (DOIT, V3)** — Pour une même station reçue sur plusieurs fréquences :
  - alignement temporel des lignes ;
  - combinaison pixel par pixel pondérée par le SNR de chaque ligne et chaque source ;
  - repli par sélection de la meilleure ligne.
- **DSP-041 (DEVRAIT, V3)** — Affichage de la contribution de chaque source (carte de qualité).

---

## 7. Modes texte : RTTY météo et NAVTEX

- **TXT-001 (DOIT, V2)** — Décodage RTTY météo public, en premier lieu DWD Pinneberg (50 Bd, shift 450 Hz, Baudot ITA2), avec réglage automatique du shift et de l'inversion.
- **TXT-002 (DOIT, V2)** — Décodage NAVTEX (SITOR-B, 100 Bd, shift 170 Hz) sur 518 kHz, 490 kHz et 4209,5 kHz, lorsque la source le permet :
  - filtrage par zone et par type de message ;
  - dédoublonnage par identifiant de message.
- **TXT-003 (DEVRAIT, V2)** — Mise en forme lisible des bulletins (en-têtes, zones, validité). Traduction des messages SYNOP (FM 12/13) en texte clair dans la langue de l'interface.
- **TXT-004 (DOIT, V2)** — Les bulletins texte sont stockés, consultables et exportables comme les images.

---

## 8. Catalogue des stations et planification

### 8.1 Catalogue

- **CAT-100 (DOIT, V1)** — Catalogue embarqué. Pour chaque station :
  - indicatif, pays, position, fréquences assignées ;
  - modes (WEFAX, RTTY, NAVTEX) ;
  - horaires UTC et contenu de chaque émission : type de carte, zone, échéance, IOC/LPM.
- **CAT-101 (DOIT, V1)** — Format : JSON versionné, avec schéma publié, maintenu dans un dépôt GitHub dédié et alimenté par la communauté (PR + revue). Sources documentées : publications des services météo nationaux, programme mondial NOAA.
- **CAT-102 (DOIT, V2)** — **Mise à jour automatique** dès qu'une connexion Internet est disponible :
  - téléchargement différentiel ;
  - **signature vérifiée** avant application ;
  - retour à la version précédente en cas d'échec.
- **CAT-103 (DOIT, V2)** — Mise à jour hors ligne par import de fichier (transfert depuis un autre appareil).
- **CAT-104 (DOIT, V1)** — Ajout et modification locale de stations et d'horaires par l'utilisateur, conservés lors des mises à jour.
- **CAT-105 (DEVRAIT, V2)** — Suggestion des stations et des fréquences selon :
  - la position (GPS ou saisie manuelle) ;
  - l'heure UTC (propagation jour/nuit) ;
  - l'historique de qualité de réception.

### 8.2 Planification

- **PLN-001 (DOIT, V2)** — Sélection des émissions à recevoir (par station, type de carte ou zone), déclenchement automatique de la réception et du réglage CAT ou SDR.
- **PLN-002 (DOIT, V1)** — Vue « prochaines émissions » avec compte à rebours et fréquence de cadran à régler.
- **PLN-003 (DOIT, V2)** — Gestion des conflits (deux émissions simultanées sur une seule source) avec priorités définies par l'utilisateur.
- **PLN-004 (DOIT, V2)** — Ouverture de la réception quelques minutes avant l'horaire, déclenchement effectif sur l'APT, fermeture sur l'APT d'arrêt ou après un délai maximal.

---

## 9. Gestion des images et transfert

- **IMG-001 (DOIT, V1)** — Galerie triée par date, station et type de carte, avec vignettes, zoom et déplacement adaptés au tactile.
- **IMG-002 (DOIT, V1)** — Métadonnées par image :
  - date et heure UTC, station, fréquence, source ;
  - qualité moyenne, erreurs de calage et de slant ;
  - traitements appliqués.
- **IMG-003 (DOIT, V1)** — Formats : PNG (sans perte) par défaut, JPEG optionnel pour un transfert léger.
- **IMG-004 (DOIT, V1)** — Rétention configurable (nombre, âge, espace disque maximal) avec purge automatique ; protection des images marquées favorites.
- **IMG-005 (DOIT, V1)** — Transfert vers d'autres appareils :
  - consultation et téléchargement depuis l'interface web (toute plateforme, y compris iOS) ;
  - partage natif Android (Bluetooth, e-mail, messagerie) ;
  - copie dans un dossier synchronisé (Syncthing, partage réseau) sur desktop et Pi.
- **IMG-006 (DEVRAIT, V3)** — Export géoréférencé pour OpenCPN (compatible avec le plugin weatherfax_pi), à partir de calages de projection fournis dans le catalogue.
- **IMG-007 (PEUT, V2)** — Comparaison côte à côte et animation des échéances successives d'une même carte.

---

## 10. Interface web et paramétrage

- **UI-001 (DOIT, V1)** — Interface responsive, utilisable sur un téléphone de 4,5″ comme sur un écran de PC, en mode clair et sombre, avec un **mode nuit rouge** pour préserver la vision nocturne à la passerelle.
- **UI-002 (DOIT, V1)** — Deux rôles :
  - **Équipage** : consultation (réception en cours, galerie, bulletins, planning) ;
  - **Skipper** : paramétrage complet, protégé par mot de passe.
- **UI-003 (DOIT, V1)** — Assistant de première configuration :
  - choix de la source ;
  - test et réglage du niveau audio ;
  - choix de la zone de navigation ;
  - sélection des stations.
- **UI-004 (DOIT, V1)** — Gestion de plusieurs clients simultanés ; verrou d'édition pour le paramétrage.
- **UI-005 (DOIT, V1)** — Langues : français et anglais dès la V1. Fichiers de traduction standards, ouverts aux contributions.
- **UI-006 (DOIT, V1)** — Lien « Code source » visible, pointant vers le dépôt et la version exacte en cours d'exécution (obligation AGPL-3.0, article 13).
- **UI-007 (DOIT, V1)** — Page de diagnostic :
  - version et niveau de matériel détecté ;
  - sources disponibles ;
  - spectre et chute d'eau (waterfall) de l'audio d'entrée ;
  - journaux téléchargeables pour les rapports de bug.

---

## 11. Mode station (Raspberry Pi)

### 11.1 Réseau

- **STA-001 (DOIT, V1)** — Par défaut, le Pi est un **point d'accès Wi-Fi autonome**, sans aucune dépendance à un réseau existant. IP fixe `192.168.4.1`, serveur DHCP et DNS local.
- **STA-002 (DOIT, V1)** — Mot de passe WPA2 unique, généré au premier démarrage et affiché dans l'interface. Une étiquette QR code (Wi-Fi + URL) est générée pour impression.
- **STA-003 (DOIT, V1)** — Portail captif : à la connexion d'un téléphone, la page d'accueil s'ouvre automatiquement. La documentation et le portail guident l'utilisateur pour « rester connecté sans Internet » sur Android et iOS.
- **STA-004 (DEVRAIT, V1)** — Nom `owfax.local` (mDNS) en complément de l'adresse IP.
- **STA-005 (DOIT, V1)** — Bonus réseau existant :
  - si un réseau Wi-Fi connu est disponible au démarrage, le Pi s'y connecte en client ; sinon il bascule en point d'accès après N secondes ;
  - l'Ethernet est utilisé dès qu'il est branché, sans désactiver le point d'accès.
- **STA-006 (DOIT, V1)** — Retour forcé en mode point d'accès par un moyen physique (bouton GPIO ou procédure documentée), pour ne jamais perdre l'accès à la station.
- **STA-007 (PEUT, V2)** — Point d'accès et client simultanés via une clé Wi-Fi USB additionnelle.

### 11.2 Heure système

Le Pi n'a pas d'horloge RTC. Or toute la planification est en UTC.

- **STA-020 (DOIT, V1)** — Sources d'heure, par ordre de priorité :
  1. GPS (USB, ou NMEA 0183/2000 du bord via passerelle) ;
  2. NTP si Internet ;
  3. module RTC DS3231 si présent ;
  4. heure du navigateur client, proposée à la connexion du skipper.
- **STA-021 (DOIT, V1)** — Affichage permanent de l'état de l'heure (source, fiabilité). Alerte si l'heure n'est pas fiable et qu'une réception planifiée approche.

### 11.3 Robustesse et alimentation

- **STA-030 (DOIT, V1)** — Système en lecture seule (overlayfs) ; données sur une partition séparée journalisée. Tolérance aux coupures d'alimentation brutales.
- **STA-031 (DOIT, V1)** — Documentation de l'alimentation 12 V (convertisseur abaisseur filtré) et de la limitation des parasites HF (ferrites, câbles USB blindés).
- **STA-032 (DEVRAIT, V1)** — Sortie GPIO pour LED d'état et buzzer (réception en cours, carte disponible, erreur).

### 11.4 Distribution et mises à jour

- **STA-040 (DOIT, V1)** — Image système prête à flasher (Raspberry Pi Imager), basée sur Raspberry Pi OS Lite.
- **STA-041 (DOIT, V1)** — Paquet `.deb` pour un Debian ou Raspberry Pi OS existant.
- **STA-042 (DOIT, V1)** — Mises à jour du logiciel en ligne, et hors ligne par téléversement d'un paquet **signé** via l'interface web. Retour à la version précédente en cas d'échec.

### 11.5 Station Android

- **STA-050 (DEVRAIT, V2)** — Un terminal Android peut jouer le rôle de station (serveur web ouvert sur le réseau local), via son partage de connexion ou `LocalOnlyHotspot` (Android 8+), avec affichage d'un QR code de connexion.

---

## 12. Notifications

- **NOT-001 (DOIT, V1)** — Événements notifiés :
  - réception démarrée ;
  - carte ou bulletin disponible ;
  - réception de mauvaise qualité ;
  - source perdue ;
  - heure non fiable (station) ;
  - mise à jour du catalogue disponible.
- **NOT-002 (DOIT, V1)** — Android : notifications natives par canal (configurables séparément) ; notification persistante du service avec progression de la carte en cours.
- **NOT-003 (DOIT, V1)** — Windows 10/11 : notifications toast. Windows 7 : bulle de la zone de notification.
- **NOT-004 (DOIT, V1)** — Linux desktop : libnotify (freedesktop).
- **NOT-005 (DOIT, V1)** — Interface web : alertes dans la page avec signal sonore optionnel. Les notifications navigateur ne sont pas utilisables en HTTP sur réseau local et ne sont pas requises.
- **NOT-006 (DEVRAIT, V2)** — L'application Android peut se connecter à une station distante (Pi ou autre terminal) et relayer ses événements en notifications natives.
- **NOT-007 (DEVRAIT, V1)** — Station : LED et buzzer GPIO (voir STA-032).

---

## 13. Exigences non fonctionnelles

- **NFR-001 (DOIT)** — **Réception seule** : aucun chemin de code ne peut provoquer une émission radio.
- **NFR-002 (DOIT)** — Charge CPU pour un décodage WEFAX audio en temps réel : < 25 % d'un cœur sur Pi Zero 2 W, et compatible avec un SoC Cortex-A7 d'un téléphone Android 5.
- **NFR-003 (DOIT)** — Empreinte mémoire du cœur (WEFAX audio seul) : < 150 Mo.
- **NFR-004 (DOIT)** — Fonctionnement intégral **sans Internet**. Internet ne sert qu'aux mises à jour (catalogue, logiciel) et à l'heure NTP.
- **NFR-005 (DOIT)** — Aucune télémétrie ni collecte de données ; aucune connexion sortante autre que les mises à jour, désactivables.
- **NFR-006 (DOIT)** — Sécurité de la station :
  - interface accessible uniquement sur le réseau local ;
  - mot de passe skipper requis pour tout paramétrage ;
  - paquets de mise à jour et catalogue signés.
- **NFR-007 (DEVRAIT)** — Consommation : le Pi Zero 2 W en station (point d'accès + décodage audio) DEVRAIT rester sous 2 W.
- **NFR-008 (DOIT)** — Accessibilité : interface utilisable avec de grandes polices et des gants (cibles tactiles ≥ 44 px).

---

## 14. Qualité et tests

- **TST-001 (DOIT, V1)** — Corpus public de fichiers WAV réels (diverses stations, conditions de QRN, de fading, d'erreur de calage et de slant), servant aux tests de non-régression automatisés du décodeur.
- **TST-002 (DOIT, V1)** — Métriques objectives sur le corpus (taux de lignes exploitables, erreur de slant résiduelle, alignement). Tout recul bloque la PR.
- **TST-003 (DOIT, V1)** — Intégration continue GitHub Actions : compilation de toutes les cibles (Android, Windows x86/x64, Linux amd64/arm64/armhf) et tests unitaires.
- **TST-004 (DEVRAIT, V1)** — Matrice de tests manuels sur matériel ancien de référence : un Android 5, un PC Windows 7, un Pi 3B, un Pi Zero 2 W.
- **TST-005 (DEVRAIT, V2)** — Tests de l'interface web sur les navigateurs cibles (ARC-003).

---

## 15. Licence et gouvernance

- **LIC-001** — Licence **AGPL-3.0-or-later** pour tout le code du projet. Le catalogue des stations est publié sous une licence de données ouverte (ODbL ou CC-BY-SA 4.0, à fixer à la création du dépôt catalogue).
- **LIC-005** — **Phase actuelle : développement par un mainteneur unique.** Les pull requests externes ne sont pas acceptées pour l'instant. Les issues (retours d'usage, enregistrements WAV, horaires de stations) restent bienvenues. LIC-002 s'appliquera à l'ouverture aux contributions ; d'ici là, les commits du mainteneur sont tout de même signés pour garder un historique conforme.
- **LIC-002** — Contributions sous **DCO** (Developer Certificate of Origin) : chaque commit porte une ligne `Signed-off-by`, ajoutée par `git commit -s`. Le contributeur garde ses droits d'auteur et certifie avoir le droit de contribuer sous la licence du projet. Pas de CLA. Un changement ultérieur de licence nécessiterait l'accord de tous les contributeurs.
- **LIC-003** — Toute dépendance ajoutée doit être compatible AGPL-3.0. La liste des dépendances et de leurs licences est maintenue dans le dépôt.
- **LIC-004** — Distribution :
  - GitHub Releases (APK, installeurs Windows, `.deb`, image Pi) ;
  - F-Droid ;
  - Play Store optionnel.

  L'APK direct est indispensable pour les appareils sans compte Google.

---

## 16. Feuille de route

| Version | Contenu principal |
|---|---|
| **V1 — MVP** | Cœur C++, entrées audio (jack, micro, USB, Bluetooth, WAV), WEFAX complet avec APT, phasage, correction de calage et de slant, DSP de base, tâche de fond Android / Windows / Linux, interface web unique FR/EN avec rôles, catalogue embarqué et vue des prochaines émissions, station Pi en point d'accès avec portail captif et gestion de l'heure, notifications, image Pi et `.deb` |
| **V2** | SDR (RTL-SDR V4, Airspy HF+, rtl_tcp), CAT Hamlib (desktop et Pi), planification automatique, mise à jour signée du catalogue, RTTY DWD et NAVTEX, DSP avancé, station Android, application Android cliente d'une station |
| **V3** | Plusieurs SDR, diversité de fréquence, deux stations simultanées, export géoréférencé OpenCPN, outils de comparaison et d'animation |

---

## 17. Points restant à préciser

1. Licence du catalogue de données : ODbL ou CC-BY-SA 4.0.
2. Clé de signature (catalogue, mises à jour) : qui la détient, et la procédure de renouvellement.
3. Liste des modèles de TRX testés et officiellement supportés en V2.
4. Projections cartographiques à documenter par carte pour l'export OpenCPN (V3).

---

## Annexe A — Glossaire

| Terme | Définition |
|---|---|
| **APT** | Automatic Picture Transmission : tons de démarrage et d'arrêt d'une émission FAX |
| **BLU / SSB / USB** | Bande latérale unique ; USB = bande latérale supérieure, utilisée pour le WEFAX |
| **CAT** | Computer Aided Transceiver : pilotage d'une radio par l'ordinateur |
| **Fréquence de cadran** | Fréquence à afficher sur le récepteur en USB : fréquence assignée − 1,9 kHz |
| **IOC** | Index of Cooperation : définit le rapport largeur/hauteur de l'image (576 ou 288) |
| **LPM** | Lignes par minute (120 le plus souvent) |
| **NAVTEX** | Service international de diffusion de bulletins de sécurité maritime en texte |
| **RTTY** | Radiotélétype : transmission de texte en FSK |
| **SDR** | Software Defined Radio : récepteur dont la démodulation est faite par logiciel |
| **Slant** | Inclinaison de l'image due à l'écart entre l'horloge de l'émetteur et celle de la carte son |
| **WEFAX** | Weather Facsimile : transmission de cartes météo en fac-similé |
