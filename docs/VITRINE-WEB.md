# openWeatherFaxSSB — Cahier des charges : vitrine web en rediffusion

| | |
|---|---|
| **Version** | 0.1 — 2026-11-10 |
| **Statut** | Brouillon, à valider avant développement |
| **Document parent** | [SPECIFICATION.md](SPECIFICATION.md) (exigences générales, réception seule, CAT-009) |
| **Composants** | owfaxd (station) · plugin WordPress « openWeatherFaxSSB Vitrine » |

Convention identique au document parent : chaque exigence porte un identifiant, et un niveau **DOIT**, **DEVRAIT** ou **PEUT**.
Domaines propres à ce document :

| Préfixe | Domaine |
|---|---|
| `VIT` | Principes généraux de la vitrine |
| `PUB` | Côté station : sélection, paquet carte, envoi |
| `SAT` | Perturbations locales : saturation du récepteur, parasites du bord |
| `WPP` | Plugin WordPress : réception, stockage, purge, pages |
| `PLY` | Lecteur de rediffusion dans le navigateur |

---

## 1. Contexte et objectifs

L'objectif est de faire connaître openWeatherFaxSSB en montrant, sur un site WordPress public, **ce que donne réellement une réception** : une vraie carte, reçue par une vraie station, avec son vrai son, ses vrais niveaux et ses vrais défauts.

Le choix retenu n'est **pas** le direct mais une **rediffusion différée** :

1. la station reçoit une carte complète ;
2. elle la publie, avec son son et ses mesures, sur le WordPress ;
3. le visiteur la rejoue comme s'il la recevait : l'image se dessine ligne par ligne au rythme réel, le son du fax est audible, le S-mètre et le SNR bougent.

Pourquoi pas le direct :

- **Ressources** : un direct impose un flux permanent ou une détection des visiteurs. Avec une rediffusion, la station envoie une seule fois par carte, et sans visiteur la charge du WordPress est nulle, puisqu'il ne sert que des fichiers statiques.
- **Sécurité** : la station ne fait que des connexions sortantes. Aucun port n'est ouvert et le dashboard n'est jamais exposé.
- **Qualité** : on ne publie que des cartes complètes et déjà triées. Les réceptions ratées ne sont jamais montrées.

### Hors périmètre

- Diffusion en direct (streaming ligne par ligne vers le public).
- Récepteurs distants ou publics : seule la station locale alimente la vitrine.
- Toute commande d'émission : la vitrine ne fait qu'observer. La détection des perturbations (§ 5) est passive, et l'émetteur de la station n'est jamais piloté (NFR-001, CAT-002).
- Usage opérationnel des cartes : la vitrine est une démonstration (VIT-004).

---

## 2. Architecture

```
┌──────────────────────── Station (minix) ────────────────────────┐
│ Scheduler ─► IC-7300 / SDR ─► owfaxd ─► carte + son + télémétrie │
│                                   │                              │
│                       Sélection (§ 4.1) + perturbations (§ 5)   │
│                                   │ refusée → reste en local     │
│                       File de publication persistante            │
└───────────────────────────────────┼──────────────────────────────┘
                                    │ HTTPS sortant uniquement, à chaque carte
                                    ▼ POST /wp-json/owfax/v1/charts
┌──────────────────────── WordPress ──────────────────────────────┐
│ Plugin : validation ─► wp-content/uploads/owfax/<id>/ ─► purge  │
│          type de contenu « owfax_chart » (1 page par carte)      │
└───────────────────────────────────┼──────────────────────────────┘
                                    │ fichiers statiques (Apache/nginx, sans PHP)
                                    ▼ CDN optionnel (ex. CDN de l'hébergement OVH)
                               Navigateur : lecteur JS (§ 7)
```

- **VIT-001 (DOIT)** — Flux à sens unique : seule la station envoie au WordPress, toujours par HTTPS et en connexion sortante. Le WordPress n'initie jamais de connexion vers la station.
- **VIT-002 (DOIT)** — Pendant la lecture, le lecteur ne charge que des fichiers statiques, sans aucun appel PHP. Sans visiteur, la vitrine ne consomme rien côté WordPress en dehors de l'ingestion des cartes.
- **VIT-003 (DOIT)** — La publication est **désactivée par défaut** dans owfaxd, et il faut l'activer explicitement. C'est la seule exception à NFR-005, et l'interface de la station l'indique clairement : « publication vitrine active vers <site> ».
  > Lot 5 : l'envoi est fait par `owfax-vitrine send`, qui ne tourne que si on l'a installé avec `--site`. Lancé avec `--publish-queue <file>`, owfaxd lit l'état écrit par l'envoi chaque minute (`/api/v1/vitrine/publish`). Le tableau de bord affiche alors le badge « Publication vitrine active vers <site> » dans sa barre, et le détail sur la page Vitrine : cartes en attente, refusées, dernier envoi, dernier échec.
- **VIT-004 (DOIT)** — Chaque lecture et chaque page de carte affichent :
  - le badge **« Rediffusion »** et la date et l'heure UTC de la réception. Le mot « direct » ou « live » n'apparaît jamais ;
  - la mention « Démonstration — ne pas utiliser pour la navigation, consultez les bulletins officiels ».
- **VIT-005 (DOIT)** — Toutes les valeurs affichées (niveaux, SNR, qualité, S-mètre) sont **mesurées**. Une grandeur qui n'est pas disponible est masquée ou étiquetée comme telle, jamais simulée. On reste dans l'esprit du document parent : ne jamais inventer d'information.

---

## 3. Sources, distances et planification

### 3.1 Sources multiples et distance

- **PUB-001 (DOIT)** — La configuration de la station contient sa position, sous forme de locator Maidenhead (6 caractères minimum) ou de latitude et longitude. Le site affiche par défaut le locator à 6 caractères (précision d'environ 5 km), pas les coordonnées exactes.
- **PUB-002 (DOIT)** — Pour chaque carte, owfaxd calcule, à partir de `position` dans le catalogue ([catalog/stations/](../catalog/stations/)) :
  - la **distance orthodromique**, en km et en milles nautiques ;
  - l'**azimut** depuis la station réceptrice.
- **PUB-003 (DOIT)** — Chaque carte publiée affiche son origine, par exemple : « DWD Pinneberg — DDK6 13 882,5 kHz — 10 kW — 1 050 km (567 NM), azimut 52° ».
- **PUB-004 (DEVRAIT)** — Indication jour/nuit sur le trajet au milieu de la réception, calculée à partir de l'heure UTC et des deux positions. Elle explique pourquoi telle fréquence est utilisée.
- **PUB-005 (PEUT)** — Carte des sources : la station réceptrice, les stations émettrices reçues et les trajets en arc de grand cercle (WPP-031).

### 3.2 Planification

Le scheduler reprend PLN-001 à PLN-004 du document parent. La vitrine y ajoute :

- **PUB-010 (DOIT)** — Une case à cocher « publier sur la vitrine » par créneau planifié. Une réception lancée hors planning n'est pas publiée, sauf si l'opérateur le demande.
- **PUB-011 (DEVRAIT)** — Une stratégie de rotation simple : à qualité suffisante, on privilégie une source qui n'a pas été publiée récemment, pour varier la galerie.
- **PUB-012 (DOIT)** — Le scheduler ne modifie jamais le délai entre changements de fréquence. CAT-009 (5 s ou plus) reste imposé par la couche CAT.
- **PUB-013 (PEUT)** — D'autres stratégies, à définir plus tard : quotas par station, plages horaires, priorités saisonnières.

---

## 4. Côté station : sélection et paquet carte

### 4.1 Règles de sélection

Une carte n'est publiée que si toutes les conditions suivantes sont réunies :

- **PUB-020 (DOIT)** — La réception est complète : APT de démarrage, phasage, image, et APT d'arrêt ou durée nominale atteinte.
- **PUB-021 (DOIT)** — La qualité moyenne des lignes (`line_quality`, DSP-008) est au moins égale à un seuil réglable, par défaut **60 %**.
- **PUB-022 (DOIT)** — La carte n'est pas refusée pour perturbation locale (SAT-010).
- **PUB-023 (DOIT)** — La station émettrice est autorisée à la publication dans le catalogue (PUB-024).
- **PUB-024 (DOIT)** — Le catalogue reçoit un bloc `publication` par station : `allowed` (booléen), `attribution` (texte obligatoire à afficher, par exemple « Source : Deutscher Wetterdienst ») et `terms_url`. Si le bloc est absent, la carte n'est **pas publiée**. Les conditions de réutilisation de chaque service météo sont à vérifier avant d'activer une station (§ 10).
- **PUB-025 (DEVRAIT)** — Mode de modération réglable :
  - **automatique** : la carte est publiée directement ;
  - **brouillon** : la carte est créée en brouillon dans WordPress, à valider à la main.
- **PUB-026 (DOIT)** — Une carte refusée reste dans la galerie locale de la station, avec le motif du refus. Ces cas servent à régler les seuils.

### 4.2 Contenu du paquet carte

Chaque paquet est un ensemble de fichiers identifié par `chart_id`, au format `<AAAAMMJJTHHMMZ>_<station>_<kHz>`, par exemple `20261110T1424Z_DWD_13882.5kHz`. Le paquet est **toujours complet** : image et son vont ensemble. On ne publie jamais une carte sans son, ni un son sans carte.

| Fichier | Contenu | Format | Taille estimée |
|---|---|---|---|
| `raw.webp` | Image finale brute (référence, DSP-030), pleine largeur (1810 px en IOC 576) | WebP qualité 85 | 0,3 à 1 Mo |
| `enh.webp` | Image améliorée, si un traitement a été appliqué | WebP qualité 85 | 0,3 à 1 Mo |
| `thumb.webp` | Vignette, largeur 400 px | WebP | 20 à 40 Ko |
| `audio.m4a` | Son réel complet, de l'ouverture de la réception à l'APT d'arrêt | AAC-LC mono 32 kbit/s, 16 kHz | 2,4 Mo pour 10 min, 4,7 Mo pour 20 min |
| `meta.json` | Métadonnées (§ 4.3) | JSON, schéma `owfax-vitrine/0.1` | environ 3 Ko |
| `telemetry.json` | Mesures horodatées (§ 4.4) | JSON | 30 à 80 Ko (20 à 30 Ko compressé) |

Soit **environ 3 à 7 Mo par carte**, dont le son représente les deux tiers. Ces chiffres sont à confirmer sur les premières cartes réelles (PUB-034).

- **PUB-030 (DOIT)** — Le format audio doit se lire sur toutes les cibles de ARC-003, y compris Safari iOS 15, d'où le choix d'AAC-LC en `.m4a`. Opus est possible plus tard en complément, pas à la place.
- **PUB-031 (DOIT)** — **Le son est rendu muet sur les zones de perturbation locale** (SAT-011). Il est remplacé par un silence de même durée, pour garder la synchronisation. Pendant une saturation, le récepteur peut restituer la modulation de l'émetteur de la station, c'est-à-dire la voix de l'opérateur ou une autre liaison, qui ne doit pas être rediffusée.
- **PUB-032 (DOIT)** — L'encodage (WebP, AAC) se fait sur la station. Le WordPress ne transcode rien, puisqu'un hébergement mutualisé n'a pas de ffmpeg.
- **PUB-033 (DOIT)** — L'image et le son sont **synchronisés** par `meta.json`. On y trouve le décalage dans le son (en secondes) du début de chaque phase (APT de démarrage, phasage, image, APT d'arrêt), ainsi que le décalage de la première ligne et le LPM. Un tableau optionnel `line_offsets_s` est prévu si le rythme réel s'écarte du nominal (slant corrigé).
- **PUB-034 (DEVRAIT)** — owfaxd journalise la taille réelle de chaque paquet. Le tableau ci-dessus est révisé après une semaine de fonctionnement.

### 4.3 `meta.json`

Champs minimum (**PUB-040, DOIT**) :

```json
{
  "schema": "owfax-vitrine/0.1",
  "chart_id": "20261110T1424Z_DWD_13882.5kHz",
  "software": { "name": "openWeatherFaxSSB", "version": "0.3.0", "commit": "1803658" },
  "station":  { "id": "dwd-pinneberg", "name": "Deutscher Wetterdienst — Hamburg/Pinneberg",
                "callsign": "DDK6", "assigned_khz": 13882.5, "power_kw": 10.0,
                "attribution": "Source : Deutscher Wetterdienst" },
  "receiver": { "locator": "JN18xx", "rig": "IC-7300", "input": "usb-audio",
                "dial_khz": 13880.6, "mode": "USB-D" },
  "path":     { "distance_km": 1050, "distance_nm": 567, "azimuth_deg": 52, "daylight": "day" },
  "time":     { "start_utc": "2026-11-10T14:24:05Z", "end_utc": "2026-11-10T14:44:00Z", "duration_s": 1195 },
  "wefax":    { "lpm": 120, "ioc": 576, "lines": 2351, "width_px": 1810 },
  "schedule": { "content": "Schedule part 1", "slot_utc": "14:25" },
  "quality":  { "mean": 0.97, "snr_db_median": 18.5, "lines_bad_fraction": 0.01 },
  "diversity": { "enabled": false, "sources": [] },
  "disturbances": { "detector": "audio-v1", "local_fraction": 0.0, "fading_fraction": 0.0, "zones": [] },
  "sync":     { "apt_start_s": 3.2, "phasing_s": 8.4, "image_s": 38.4, "apt_stop_s": 1180.1,
                "first_line_s": 38.4 },
  "treatments": ["slant", "denoise-1"],
  "files":    { "raw": "raw.webp", "enh": "enh.webp", "thumb": "thumb.webp",
                "audio": "audio.m4a", "telemetry": "telemetry.json" }
}
```

- **PUB-041 (DOIT)** — `diversity` précise si la carte résulte d'une combinaison de plusieurs récepteurs (DSP-040). Si oui, il liste les sources (récepteur, fréquence, contribution en %). En diversité, le son publié est celui de la source qui a la meilleure contribution, et `meta.json` le précise.
- **PUB-042 (DOIT)** — Un schéma JSON `owfax-vitrine/0.1` est publié dans le dépôt et validé par les tests (TST).

### 4.4 `telemetry.json`

- **PUB-050 (DOIT)** — Une mesure par seconde, sur toute la durée du son. Le fichier est **en colonnes** (un tableau par grandeur, l'indice étant la seconde dans le son), ce qui le rend plus compact qu'une liste d'objets :
  - `audio_dbfs` : niveau d'entrée ;
  - `snr_db` : SNR estimé, comme le rapport entre l'énergie dans la bande WEFAX (noir − 100 Hz à blanc + 100 Hz, mesurés sur la carte) et la densité de bruit hors de cette bande (médiane sur 300 à 900 Hz et 2700 à 2900 Hz), ramené à 2,5 kHz comme dans le modulateur de test. L'estimation est juste à ±1 dB jusqu'à 20 dB et plafonne vers 25 dB au-delà, car les bandes latérales FM atteignent les bandes de référence ;
  - `disturbance` : cause de la perturbation qui couvre la seconde, 0 si aucune (§ 5) ;
  - si un S-mètre est disponible : `smeter_db_s9`, en dB par rapport à S9 (valeur `STRENGTH` de Hamlib).
- **PUB-051 (DOIT)** — `line_quality`, une valeur par ligne d'image.
- **PUB-052 (DOIT)** — Le S-mètre sert d'**effet visuel animé** : sa précision n'est pas un objectif (décision du 2026-11-10), et il n'y a donc pas de table d'étalonnage. La source est la valeur `STRENGTH` de Hamlib, en dB par rapport à S9 :
  - aujourd'hui, le fichier `<capture>.smeter.csv` écrit toutes les 10 s par `tools/capture/ic7300_capture.sh` ;
  - plus tard (lot 6), une lecture CAT en lecture seule par owfaxd, au travers de la couche CAT existante.

  Le lecteur interpole entre les mesures. La conversion en points S est simple : 6 dB par point S sous S9, en dB au-delà (S9+20…).
- **PUB-053 (DOIT)** — Sans source de S-mètre, le champ `smeter_db_s9` est absent. Le lecteur affiche alors « niveau audio (dBFS) » à la place du S-mètre (VIT-005).

### 4.5 Envoi

- **PUB-060 (DOIT)** — L'envoi se fait par `POST /wp-json/owfax/v1/charts` en multipart, authentifié par un **mot de passe d'application** WordPress (fonction native). Ce mot de passe appartient à un compte dédié qui n'a que le droit `owfax_publish`.
- **PUB-061 (DOIT)** — L'envoi est **idempotent**, indexé sur `chart_id` : un renvoi ne crée pas de doublon.
- **PUB-062 (DOIT)** — Une **file persistante** sur la station gère les envois. Elle survit à un redémarrage et réessaie avec un délai croissant de 1 min à 6 h. Si Internet est coupé, les cartes attendent, et les paquets en attente depuis plus de 48 h sont abandonnés, avec une ligne dans le journal.
- **PUB-063 (DOIT)** — Le mot de passe d'application est stocké sur la station avec des droits restreints (lisible par le seul utilisateur du service). Il n'apparaît jamais dans les journaux.
- **PUB-064 (DEVRAIT)** — Envoi par morceaux si le paquet dépasse la limite d'upload de l'hébergement, détectée par `GET /wp-json/owfax/v1/limits`.
  > Lot 5 : la station lit ces limites avant chaque série d'envois. Un paquet trop gros pour l'hébergeur est mis de côté avec le motif (`upload_max_filesize` ou `post_max_size`), au lieu d'être envoyé pour rien. L'envoi par morceaux n'est pas fait : il demande aussi une route dans le plugin. Avec 3 à 7 Mo par carte, la limite courante de 8 Mo suffit.
- **PUB-065 (DOIT)** — Réalisation (lot 5) : `owfax-vitrine`.
  - `package … --enqueue <file>`, ou `enqueue --queue <file> <paquet>…`, copie les paquets publiables dans la file. Un paquet non publiable, non encodé ou incomplet est refusé à l'entrée.
  - `send --queue <file> --site https://… --credentials <fichier> --loop` envoie les cartes une par une, de la plus ancienne à la plus récente, et s'arrête au premier échec qui peut passer : pas de réseau, site en panne, 429, 5xx, 507. Le délai d'attente double ensuite à chaque échec.
  - Un refus définitif du site (400, 413, 415, 422) met le paquet de côté dans `refused/`, avec son motif, et la file continue. Un nouvel `enqueue` le remet en file.
  - Le transport est le programme `curl`, qui reçoit toute sa configuration, mot de passe compris, sur son entrée standard.
  - Le fichier d'identifiants contient une ligne `identifiant:mot de passe d'application`. Il est refusé s'il est lisible par d'autres utilisateurs.
  - En http, seule une adresse locale est acceptée, pour les essais.
  - La file est un dossier : `pending/`, `refused/`, `sent.log`, `journal.log`, `state.json` et `status.json`. Chaque fichier y est écrit à côté puis renommé, pour qu'une coupure de courant ne laisse jamais de fichier à moitié écrit.
  - `queue --queue <file>` montre ce qui attend.
  - Service : `tools/station/install-publish-service.sh --site https://…`.
- **PUB-066 (DOIT)** — Aucun doublon. Quand une réponse se perd, la station renvoie la carte et le site répond « exists », ce qui compte comme envoyé. Une carte déjà dans `sent.log` n'est plus mise en file. Si la station s'arrête entre l'accord du site et la sortie de la file, elle ne renvoie pas la carte.

---

## 5. Perturbations locales : saturation du récepteur et parasites du bord

### 5.1 Le phénomène

Deux causes locales font perdre la carte pendant de longues secondes :

- **la saturation du récepteur** par un émetteur de la station. En phonie, une émission dure couramment **15 s ou plus** ;
- **les parasites d'un équipement du bord** : régulateurs solaires MPPT, pilotes automatiques dont le vérin est commandé en PWM à fort courant, convertisseurs, onduleurs.

La plupart des utilisateurs reçoivent avec une **clé SDR**, pas avec un transceiver radioamateur. La détection ne peut donc compter ni sur un indicateur de surcharge propre à un modèle, ni sur un S-mètre. Elle repose sur **le son seul**, que toutes les sources fournissent. Le S-mètre ou l'indicateur de surcharge d'un SDR restent des indices en plus quand ils existent.

Ces perturbations se distinguent d'un fading naturel par des signatures mesurables chaque seconde :

| Indice | Saturation | Parasite PWM / MPPT | Fading naturel |
|---|---|---|---|
| Écrêtage, saut de niveau | oui | parfois | non |
| Bruit impulsif (kurtosis élevé) | souvent (voix, distorsion) | **oui** | non (bruit gaussien) |
| Enveloppe périodique (rythme de découpage, 2 à 200 ms) | non | **oui** | non |
| Spectre plat (bruit large bande) | oui | oui | oui |
| Fronts francs, en 2 s au plus, au début et à la fin | **oui** | **oui** | non (évolution sur plusieurs secondes) |

### 5.2 Détection

- **SAT-001 (DOIT)** — **La zone** : au moins `min_run_s` secondes (par défaut **15 s**, soit 30 lignes à 120 lpm) où `line_quality` reste sous **0,8**. Un bruit seul donne 0,6 à 0,7, car l'essentiel du bruit FM tombe dans la plage noir-blanc, et une ligne propre donne 1. Des retours de moins de 2 s ne coupent pas une zone.
- **SAT-002 (DOIT)** — **Les mesures audio, chaque seconde** : part d'échantillons écrêtés, niveau, kurtosis, planéité spectrale entre 300 et 2900 Hz, part de puissance dans la bande WEFAX, et périodicité de l'enveloppe. Elles sont comparées aux valeurs de la même carte en dehors des zones.
- **SAT-003 (DEVRAIT)** — **Le S-mètre**, quand il existe, bloqué à S9+30 dB ou plus et 15 dB au-dessus de son niveau sur la carte.
- **SAT-004 (DEVRAIT, V2 SDR)** — **La surcharge du convertisseur du SDR** : échantillons IQ en butée, quel que soit le modèle de clé. C'est l'indice le plus direct, disponible dès que owfaxd lira les SDR.
- **SAT-005 (PEUT)** — Un indicateur de surcharge propre au récepteur (OVF de l'IC-7300 en CI-V), ou une entrée « TX actif » (GPIO, séquenceur), en **lecture seule**.

Le classement d'une zone (**SAT-006, DOIT**), dans cet ordre :

| Cause | Condition | Locale |
|---|---|---|
| `impulsive_interference` | enveloppe périodique (≥ 0,5) : c'est la signature du découpage PWM, même s'il écrête | oui |
| `overload` | écrêtage, ou niveau 6 dB au-dessus de celui de la carte, ou S-mètre bloqué | oui |
| `impulsive_interference` | sinon, kurtosis ≥ 6 (impulsions irrégulières) | oui |
| `broadband_interference` | spectre plat **et** fronts francs | oui |
| `fading` | aucun des cas précédents | non |

- **SAT-007 (DOIT)** — Une zone `fading` n'est pas une perturbation locale : elle compte dans la qualité (PUB-021) et dans `fading_fraction`, mais son son n'est pas coupé.
- **SAT-008 (DOIT)** — Les seuils sont réglables. Leurs valeurs par défaut sont fixées sur des signaux synthétiques et doivent être confirmées sur des cas réels (§ 10) : une émission de la station, un MPPT, un pilote automatique.

### 5.3 Conséquences

- **SAT-010 (DOIT)** — La carte est **refusée** (motif `local_disturbance`) si la part de lignes en perturbation locale dépasse `max_local_fraction`, par défaut **15 %**.
- **SAT-011 (DOIT)** — En dessous de ce seuil, la carte peut être publiée. Dans ce cas :
  - les zones sont listées dans `meta.json` (`disturbances`), avec début et fin en secondes et en lignes, cause et indices ;
  - le son des zones locales est rendu muet (PUB-031). Les mesures de `telemetry.json` gardent les niveaux réels, et une colonne `disturbance` donne la cause seconde par seconde ;
  - le lecteur hachure ces lignes et affiche « réception perturbée localement (émission de la station ou parasite du bord) ».
- **SAT-012 (DOIT)** — L'image elle-même n'est **pas retouchée** dans les zones. On montre la réalité, sans interpolation ni comblement.
- **SAT-013 (DOIT)** — Tests automatisés sur signaux synthétiques :
  - une carte propre ne donne aucune zone ;
  - une saturation écrêtée de 30 s donne une zone `overload` ;
  - un parasite impulsif à 400 Hz donne une zone `impulsive_interference` ;
  - un bruit large bande commuté donne une zone `broadband_interference` ;
  - un fading à 2 dB/s reste `fading` ;
  - une perturbation de 10 s est ignorée ;
  - 90 s de saturation font refuser la carte et couper son son.

  Sur le corpus réel, aucune zone locale ne doit être trouvée (fausses alarmes).
- **SAT-014 (DEVRAIT)** — **Diagnostic de bord** : les mêmes mesures, appliquées en continu par owfaxd, permettent de signaler à l'utilisateur un parasite récurrent (« perturbation impulsive périodique à chaque mise en route du pilote ») sur la page de diagnostic (UI-007). Cela aide à trouver l'équipement en cause, ce qui sert bien au-delà de la vitrine.

---

## 6. Plugin WordPress

### 6.1 Ingestion et stockage

- **WPP-001 (DOIT)** — Le point d'entrée `POST /wp-json/owfax/v1/charts` :
  - valide le schéma de `meta.json` ;
  - vérifie les types MIME réels et les tailles maximales par fichier ;
  - refuse tout fichier inattendu.
- **WPP-002 (DOIT)** — Les fichiers sont stockés dans `wp-content/uploads/owfax/<chart_id>/`, **hors médiathèque** : pas de pièces jointes WordPress, pas de miniatures générées par WordPress.
- **WPP-003 (DOIT)** — Le répertoire `uploads/owfax/` interdit l'exécution de scripts (`.htaccess` pour Apache, documentation équivalente pour nginx) et le listage des répertoires.
- **WPP-004 (DOIT)** — Chaque carte crée une entrée du type de contenu personnalisé `owfax_chart`, publiée ou en brouillon selon PUB-025. Les métadonnées clés (station, date, fréquence, distance, qualité) sont indexées pour le tri et les filtres.
- **WPP-005 (DOIT)** — Une carte supprimée dans l'administration WordPress supprime aussi son répertoire, et inversement à la purge.

### 6.2 Rétention et purge

Une carte se conserve **avec son son** : c'est l'intérêt de la rediffusion. La purge supprime donc toujours le paquet entier (image, son, métadonnées, page).

- **WPP-010 (DOIT)** — Limites réglables, la première atteinte déclenchant la purge :
  - **quota disque**, par défaut 2 Go ;
  - **âge maximum**, par défaut 60 jours ;
  - **nombre maximum**, par défaut 300 cartes.
- **WPP-011 (DOIT)** — La purge supprime d'abord les paquets les plus anciens. Les cartes marquées **« à conserver »** ne sont jamais purgées, et elles sont plafonnées à 20 % du quota.
- **WPP-012 (DOIT)** — La purge se lance **à chaque ingestion**, avant l'écriture du nouveau paquet. Elle ne dépend pas de WP-Cron, qui ne se déclenche qu'avec des visites. Un WP-Cron quotidien complète le dispositif pour l'âge maximum.
- **WPP-013 (DOIT)** — L'administration affiche l'espace occupé, le nombre de cartes, la plus ancienne, et la projection « quota atteint dans N jours » calculée sur les 7 derniers jours.
- **WPP-014 (DOIT)** — Si l'ingestion ne peut pas libérer assez de place (cartes « à conserver » trop nombreuses), elle répond `507 Insufficient Storage`. La station garde alors la carte dans sa file et le signale.

Ordre de grandeur, à 5 à 15 cartes par jour et 3 à 7 Mo par carte : 15 à 105 Mo par jour. Le quota de 2 Go représente donc environ 20 à 130 jours d'historique.

### 6.3 Pages et blocs

- **WPP-020 (DOIT)** — Un bloc Gutenberg et un shortcode équivalent sont fournis :
  - `[owfax_player]` : le lecteur de rediffusion, sur la dernière carte ou en boucle sur les N dernières (`playlist="10"`) ;
  - `[owfax_gallery]` : la galerie en grille de vignettes, filtrable par station, avec la distance, la date et un badge de qualité ;
  - `[owfax_chart id="…"]` : une carte précise.
- **WPP-021 (DOIT)** — Chaque carte a sa page publique (permalien `/fax/<chart_id>/`) avec le lecteur et les métadonnées complètes. Elle comporte les balises Open Graph et Twitter Card (vignette, titre « DWD Pinneberg — 13 882,5 kHz — 10/11/2026 14:24 UTC ») pour le partage sur les réseaux sociaux. Le permalien reprend le `chart_id` sous la forme que WordPress donne aux adresses : en minuscules, le point remplacé par un tiret (`/fax/20261110t1007z_ddk6_13882-5khz/`).
- **WPP-022 (DOIT)** — Chaque page de carte affiche l'attribution de la source (PUB-024), le badge « Rediffusion » et l'avertissement de VIT-004, un lien vers le projet, et la version d'openWeatherFaxSSB qui a reçu la carte.
- **WPP-023 (DEVRAIT)** — Un widget « prochaine réception planifiée » montre la prochaine carte que la station prévoit de recevoir. owfaxd l'envoie avec chaque paquet, sous forme d'une liste des 24 h suivantes.
- **WPP-030 (DEVRAIT)** — `[owfax_stations]` : la liste des sources reçues, avec la distance, le nombre de cartes publiées et la meilleure qualité obtenue.
- **WPP-031 (PEUT)** — Une carte géographique des trajets (PUB-005), avec des tuiles OpenStreetMap et le fichier JS embarqué dans le plugin.

### 6.4 Exigences techniques du plugin

- **WPP-040 (DOIT)** — Versions minimales : WordPress 6.0 et PHP 7.4. Aucune extension PHP hors du socle courant des hébergements mutualisés (pas de ffmpeg, pas d'Imagick obligatoire).
- **WPP-041 (DOIT)** — Les scripts et styles du lecteur ne sont chargés que sur les pages qui contiennent un bloc ou un shortcode owfax.
- **WPP-042 (DOIT)** — Les fichiers JS sont embarqués dans le plugin, sans appel à un CDN tiers, comme le demande ARC-003.
- **WPP-043 (DOIT)** — Interface d'administration et lecteur en français et en anglais (fichiers `.po`/`.mo`, domaine de texte `owfax-vitrine`).
- **WPP-044 (DOIT)** — Désinstallation propre : une option demande s'il faut supprimer les cartes et les fichiers ou les conserver.
- **WPP-045 (DEVRAIT)** — Le plugin est compatible avec un CDN (en-têtes de cache sur les fichiers statiques de `uploads/owfax/`, qui ne changent jamais pour un `chart_id` donné). Le CDN reste optionnel.

---

## 7. Lecteur de rediffusion

L'objectif est que le visiteur ait l'impression de **regarder un vrai logiciel de réception en train de travailler**.

### 7.1 Déroulement

- **PLY-001 (DOIT)** — **Le son sert d'horloge.** La ligne affichée se déduit de la position de lecture du son et de `sync` (PUB-033). Le canvas révèle l'image **ligne par ligne**, du haut vers le bas, au rythme réel (2 lignes/s à 120 lpm). Avancer ou reculer dans le son réaffiche l'image jusqu'à la ligne correspondante. La période de ligne se calcule comme (`image_end_s` − `image_s`) / nombre de lignes, car `line_period_s` est arrondi : sinon la dernière ligne ne serait jamais complète.
- **PLY-002 (DOIT)** — Les phases sont signalées : « Tonalité de démarrage (APT) », « Phasage », « Réception de l'image », « Tonalité d'arrêt ».
- **PLY-003 (DOIT)** — Vitesses de lecture ×1 (défaut, temps réel), ×4 et ×10. En accéléré, la hauteur du son est conservée (`preservesPitch`).
- **PLY-004 (DOIT)** — Le son est coupé par défaut, à cause des règles d'autoplay des navigateurs. Un bouton « Écouter » bien visible le déclenche, et la lecture sans son reste possible.
- **PLY-005 (DOIT)** — L'image révélée est l'image **brute** (`raw.webp`), comme sur le logiciel au moment de la réception. À la fin, un bouton **Avant / Après** permet de passer à l'image améliorée, comme UI-009.
- **PLY-006 (DOIT)** — Les zones de perturbation locale sont hachurées, avec une légende (SAT-011).
- **PLY-007 (DEVRAIT)** — Mode playlist : enchaînement des N dernières cartes, avec en option une version accélérée (×10) en page d'accueil pour donner l'effet « station qui tourne en permanence ».

### 7.2 Instruments

- **PLY-010 (DOIT)** — **Le S-mètre à aiguille**, gradué en points S (S1 à S9, puis S9+10 à S9+60). C'est un effet visuel (PUB-052). Il est animé par interpolation de `telemetry.json`. À défaut de S-mètre, c'est un vumètre audio en dBFS (PUB-053).
- **PLY-011 (DOIT)** — Les **valeurs numériques** du moment : SNR (dB), qualité de la ligne en cours (%), niveau audio (dBFS), ligne n / total, temps écoulé / durée.
- **PLY-012 (DOIT)** — Une **bande de qualité** le long de l'image : une colonne colorée qui donne `line_quality` ligne par ligne.
- **PLY-013 (DEVRAIT)** — Une **chute d'eau (waterfall)** calculée **dans le navigateur** à partir du son réel (Web Audio `AnalyserNode`), sur la bande 1000 à 2800 Hz, avec des repères à 1500 Hz (noir) et 2300 Hz (blanc). Elle ne demande aucun stockage supplémentaire.
- **PLY-014 (DEVRAIT)** — Un mini-graphe de SNR et de qualité sur toute la durée de la carte, avec un curseur à la position courante. Il sert aussi de barre de déplacement, à la souris, au doigt et au clavier.
- **PLY-015 (DOIT)** — Un bandeau d'information : source, fréquence, puissance, distance et azimut, jour/nuit, récepteur, diversité oui/non, traitements appliqués.

### 7.3 Contraintes techniques

- **PLY-020 (DOIT)** — JavaScript ES2017 sans framework, avec une cible de navigateurs alignée sur ARC-003 (Safari iOS 15 inclus).
- **PLY-021 (DOIT)** — **Rien n'est chargé avant l'action du visiteur**, à part la vignette et `meta.json`. L'image complète, le son (`preload="none"`) et la télémétrie ne sont téléchargés qu'au clic sur Lecture, ou quand le lecteur devient visible en mode playlist (`IntersectionObserver`).
- **PLY-022 (DOIT)** — La lecture se met en pause quand l'onglet est caché (`visibilitychange`), en mode playlist automatique.
- **PLY-023 (DOIT)** — L'affichage est responsive jusqu'à 360 px de large : sur téléphone, les instruments passent sous l'image. Modes clair et sombre.
- **PLY-024 (DOIT)** — Accessibilité : commandes utilisables au clavier, libellés ARIA, et contrastes conformes WCAG AA.
- **PLY-025 (DEVRAIT)** — Le lecteur est aussi intégré au dashboard local de la station, pour tester un paquet avant sa publication.
- **PLY-026 (DOIT)** — Le lecteur tient dans deux fichiers autonomes, `web/vitrine/owfax-player.js` et `owfax-player.css`, que le plugin WordPress reprend tels quels. Dans le dashboard, la page Vitrine lit les paquets que `owfaxd --vitrine <dossier>` sert sur `/api/v1/vitrine`, avec les requêtes `Range` dont les navigateurs ont besoin pour se déplacer dans le son.

---

## 8. Sécurité et vie privée

- **SEC-001 (DOIT)** — La station n'expose **aucun service** pour la vitrine : la publication ne fait que des connexions sortantes. Le dashboard (port 8090) et l'API de la station restent réservés au réseau local (NFR-006).
- **SEC-002 (DOIT)** — Le compte WordPress de publication n'a que le droit `owfax_publish` : il ne peut ni publier d'articles, ni modifier d'extension, ni gérer d'utilisateur. Son mot de passe d'application peut être révoqué depuis l'administration WordPress.
- **SEC-003 (DOIT)** — Le point d'entrée d'ingestion limite le nombre d'envois (par défaut 30 par heure), et le plugin journalise les refus.
- **SEC-004 (DOIT)** — Le son publié ne contient jamais de zone de perturbation locale (PUB-031). L'opérateur peut aussi rendre muet un intervalle à la main avant publication, en mode brouillon.
- **SEC-005 (DOIT)** — Le lecteur ne fait aucun suivi des visiteurs et ne pose aucun cookie (cohérent avec NFR-005).

---

## 9. Lots de réalisation et critères de recette

| Lot | Contenu | Recette |
|---|---|---|
| **L1 — Paquet carte** | `meta.json` et `telemetry.json` (sans S-mètre), encodage WebP et AAC, distance et azimut, schéma JSON, règles PUB-020 à PUB-026 | Un paquet généré à partir d'une carte du corpus est validé par le schéma. Sa taille est mesurée. |
| **L2 — Perturbations locales** | SAT-001 à SAT-003, SAT-006 à SAT-013, son rendu muet | Les tests SAT-013 passent. Aucune fausse alarme sur le corpus réel. Une carte perturbée à plus de 15 % est refusée, une carte avec une zone de 20 s est publiée avec sa zone. |
| **L3 — Lecteur local** | PLY-001 à PLY-012 et PLY-020 à PLY-025 dans le dashboard | La lecture est synchronisée à la ligne près, à ×1 et à ×10, sur Chrome, Firefox et Safari iOS. |
| **L4 — Plugin WordPress** | Ingestion, stockage, purge, type de contenu, shortcodes, pages | Ingestion idempotente. La purge respecte le quota. Une page de carte est rendue sans appel PHP pendant la lecture. |
| **L5 — Envoi** | File persistante, mot de passe d'application, reprise | Une coupure Internet de 2 h n'entraîne aucune perte de carte, et aucun doublon après reprise. *(Test `publish_two_hour_cut_loses_nothing_and_duplicates_nothing` sur une horloge simulée, et essai réel minix → WordPress : coupure, reprise, réponse perdue.)* |
| **L6 — CAT et compléments** | S-mètre et OVF par CAT (PUB-052, SAT-002), waterfall, scheduler avec PUB-010 et PUB-011, prochaine réception, carte des trajets | Les lectures CAT ne provoquent aucun changement de fréquence. Le test CAT-009 reste vert. |

---

## 10. Points restant à préciser

1. **Droits de rediffusion des cartes** par service météo, à vérifier et à consigner dans `publication` (PUB-024) :
   - DWD : *GeoNutzV*, attribution exigée, à confirmer ;
   - Royal Navy / Met Office (GYA Northwood) : conditions à vérifier ;
   - autres stations à leur ajout.
2. ~~Licence du plugin~~ — **Réglé le 2026-11-10** : le plugin est personnel et non distribué, la question ne se pose pas.
3. **Commande CI-V exacte** de lecture de l'état OVF, à confirmer sur le manuel (lot 6). Pour le S-mètre, aucun étalonnage n'est prévu (PUB-052).
4. **Seuils par défaut** (`min_run_s` = 15 s, `max_local_fraction` = 15 %, qualité minimale = 60 %) : à ajuster après un mois de réceptions réelles, à partir des cartes refusées conservées en local (PUB-026).
5. ~~Emplacement du plugin dans le dépôt~~ — **Réglé le 2026-11-13** : `integrations/wordpress/owfax-vitrine/`, avec ses tests dans `integrations/wordpress/tests/`.
6. **Hébergement du WordPress** et activation éventuelle du CDN de l'hébergeur (WPP-045).
