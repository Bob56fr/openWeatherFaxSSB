# Journal de développement

Notes de travail, du plus récent au plus ancien. Heures en UTC (Paris = UTC+1 en hiver).

## Nuit du 9 au 10 novembre 2026 — travail autonome (4 étapes)

Cadre convenu : pas d'accès radio hors des sessions cron, pas de changement de
spécification, commits signés sur `main` avec CI verte, seule publication : la
release `corpus-v1` (validée). Le PC s'est mis en veille entre ~00:20 et ~06:30 ;
la station `minix` a continué seule.

### Résumé

| Étape | État | Résultat |
|---|---|---|
| 1. Corpus réel | ✅ | Release [`corpus-v1`](https://github.com/Bob56fr/openWeatherFaxSSB/releases/tag/corpus-v1) : 7 émissions (2 DWD, 5 GYA), job CI de non-régression |
| 2. Découpage GYA | ✅ | Ton APT haché par le QRM désormais détecté |
| 3. Clics FM (DSP-020) | ✅ | Option `--clicks` ; −60 % de points parasites sur carte réelle |
| 4. Décodeur en flux (DSP-007) | ✅ | `StreamDecoder` + `owfax-cli stream`, validé sur réceptions réelles |

CI : 14 jobs verts (dont réceptions réelles). Tests : 33.

### Étape 1 — corpus de réceptions réelles

- 7 émissions du 9/11 à 11 025 Hz + PNG de référence + métriques ; mention de source
  DWD (GeoNutzV) dans la release et `tests/corpus/README.md`.
- La porteuse « blanche » que la DWD garde entre les cartes produisait une fausse
  émission : `decode_all` rejette maintenant les segments sans texture.
- Décodage identique au chiffre près sous Windows et Linux.

### Correction importante — horloge de GYA

- L'estimation sur le phasage donnait −57 ppm sur GYA 23:12 alors que le cadre de la
  carte est vertical à −43 ppm (trait penché de 23 px). L'affinage sur l'image cherche
  maintenant ±20 ppm puis ±1 ppm par pas de 0,1.
- Les 5 cartes GYA donnent maintenant −40,8 à −44,3 ppm : **le rythme de ligne de GYA
  est décalé d'environ −41 ppm** par rapport à la DWD (vrai décalage d'émetteur, la
  carte son étant la même). L'horloge doit donc être estimée par émission.
- DWD 22:00 : valeur vraie ≈ −1,7 ppm (mesurée par régression sur le cadre), estimée
  −3,6 ppm : ~3 px sur toute la carte.

### Étape 2 — découpage des émissions GYA

- Le ton de début de 23:12 (300 Hz, ~4 s) arrivait en morceaux d'~1 s séparés de trous
  de 0,25 s ; un trou isolé est désormais toléré (2 s de ton cumulé restent exigées).

### Étape 3 — suppression des clics FM (DSP-020)

- Synthétique, QRN fort : corrélation 0,858 → 0,940 ; image propre intacte.
- DWD 22:00 réelle : points noirs isolés 273 → 108 ; lignes striées inchangées
  (elles viennent des creux de fading : prochaine piste DSP-021 / DSP-040).
- La qualité par ligne est calculée sur le signal reçu, avant correction.

### Étape 4 — décodeur en flux (DSP-007)

- Événements : début (APT ou démarrage manuel), verrouillage du phasage, chaque ligne
  en direct, fin avec image finale (pipeline complet sur le signal conservé à 8 kHz,
  ~38 Mo pour 20 min).
- Ton d'arrêt pris en compte dès 1,5 s ; lignes en direct retardées de 2,5 s pour
  qu'aucune ne tombe dans le ton.
- Affinage progressif de l'horloge après 300 et 600 lignes : l'aperçu GYA reste à
  quelques pixels de l'image finale (≈ 40 px d'écart sans cet affinage).
- Validé sur DWD 22:00 et GYA 23:12 / 23:24 rejoués morceau par morceau.

### Incident — session cron DWD de 04:24 sans capture

- Cause : le balayage a lieu 6 min avant la carte de 04:30, pendant la porteuse de
  repos ; mon correctif de `probe` (rejet des porteuses) servait aussi à choisir la
  fréquence. Le choix se fait maintenant sur la présence de porteuse (énergie dans la
  bande × cohérence FM). Session relancée à 06:48 sur 7880 kHz (cartes 06:51, 07:04).

## Questions pour toi

1. **Aperçu en direct** : l'interface doit-elle afficher l'aperçu ligne par ligne puis
   le remplacer par l'image finale à la fin, ou garder les deux ?
2. **Planning DWD d'hiver** : les cartes « planning » de 14:25 / 14:45 UTC seront
   capturées aujourd'hui par cron — à relire ensemble pour corriger le catalogue.
3. **Diversité de fréquence** (DWD 3855 + 7880 kHz en simultané, IC-7300 + RSP2pro) :
   c'est le remède aux stries de fading. Quand veux-tu qu'on attaque le RSP2pro ?
   (API SDRplay propriétaire : uniquement en outil de capture, hors binaires.)
4. **Réparation des lignes (DSP-021)** : interpolation verticale des lignes isolées
   dégradées, signalée comme « traitée ». D'accord pour la faire en option comme les clics ?

Réponses (10 novembre) : 1) garder les deux versions (UI-008) ; 2) fait, voir plus bas ;
3) pas de SDR pour l'instant — et la diversité se fera uniquement avec des récepteurs
locaux, pas de récepteurs distants ; 4) d'accord en option, avec un bouton Avant/Après
(UI-009) — la réparation reste expérimentale, sans gain visible.

## 10 novembre 2026 — moteur de réception owfaxd et planning DWD d'hiver

### owfaxd (BG-001)

- Entrées : `wav:`, `stdin` (s16le), `alsa:` (Linux, optionnel). Un fil de capture
  remplit une file bornée ; la file est sans perte pour les fichiers et les tubes,
  et ne jette l'audio le plus ancien que pour une vraie carte son.
- Chaque carte : `_live.png` (réécrit toutes les 50 lignes), `.png` brut, `_enh.png`,
  `.json` ; `status.json` toutes les 5 s. Réception seule, pas de CAT.
- Sur la capture DWD 22:00 rejouée, l'image brute est identique octet pour octet à
  celle de `owfax-cli decode`. Corpus : 5 cartes sur 7 en flux ; les 2 manquées n'ont
  pas d'APT de démarrage (capture commencée en cours de carte, APT GYA haché) →
  à faire : démarrage automatique sur signal fax structuré sans APT.
- Premier défaut trouvé en essai : la file jetait de l'audio en lisant un fichier
  (le lecteur va plus vite que le décodeur). Corrigé (`is_live()`).

### Essai en direct sur l'IC-7300 (ALSA, 13882,5 kHz, 11:21 – 15:05 UTC)

- 11 cartes reçues, toutes avec phasage, −3,3 à −6 ppm, qualité 99 – 100 %,
  aucune perte d'audio ni dépassement de tampon en 3 h 40.
- Relance à 14:11 pour le planning : garde-fou des relais respecté (≥ 6 s entre la
  remise des réglages et le nouvel accord) ; la session cron de 14:19 s'est annulée
  seule (verrou radio).

### Planning DWD d'hiver

- Cartes « Schedule part 1 / part 2 » reçues à 14:25 et 14:45, transcrites dans
  `catalog/stations/dwd-pinneberg.json` (48 créneaux, 04:30 – 22:00 UTC), avec les
  réceptions réelles en `observed`. Le ton APT tombe à la minute annoncée.
- Puissance diffusée : 10 kW sur les trois fréquences (le catalogue disait 20 kW).
- Rien après 22:00 : cohérent avec la porteuse blanche vue le 9 de 22:11 à 22:45.
- 11:23 et 13:08 : aucun démarrage détecté alors qu'owfaxd écoutait (non émis ou APT
  manqué, non tranché). La même carte figure deux fois au planning (11:23 et 11:35).

### Démarrage sans APT (DSP-006), sur le corpus uniquement

- Détecteur : toutes les secondes, les 10 dernières secondes de signal sont notées
  (dispersion de la luminance, corrélation d'une ligne à la suivante, corrélation à
  une demi-ligne en référence). Seuils fixés sur 7 h de captures (DWD 3855 / 7880 /
  13882,5 kHz, GYA 4610 kHz) : dans les cartes, dispersion ≥ 0,15 et corrélation
  0,55 – 0,95 ; bruit GYA entre cartes, dispersion ≈ 0,25 mais corrélation ≤ 0,2 ;
  porteuse DWD au repos, dispersion ≈ 0. Les tons APT corrèlent aussi (0,97) : la
  fenêtre n'en contient jamais. Départ après 5 notes positives de suite.
- Fin sans ton de fin : 60 s de bruit. Un seuil à 30 s coupait en deux une vraie
  carte du corpus (GYA 23:24, évanouissement de 20 à 30 s selon l'enregistrement) :
  une durée seule ne sépare pas un évanouissement du trou de 41 s entre deux cartes
  dont les tons sont perdus (GYA 07:35). Après un trou ≥ 15 s, une nouvelle carte est
  donc reconnue à son phasing.
- Fragments démarrés sans APT et plus courts qu'une minute : rejetés (fin bruitée
  d'une carte au début de la capture GYA 07:21).
- Résultats : corpus de référence en flux 7 cartes sur 7 (5 avant), toutes égales à
  leur référence ; captures de 7 h, 22 cartes (17 avant), aucun faux départ
  (16 min de bruit DDH3 après 22:46, pauses DWD). Cartes ajoutées : fin de carte DDH3
  21:45, GYA 23:03, GYA 07:23 et 07:35 (collées avant), GYA 08:11 (30 min dont 20 de
  bruit avant, 11 min maintenant). Les cartes démarrées sur APT sont inchangées.
- Limite : une carte démarrée en cours d'image, sans phasing, garde un décalage
  horizontal (comme le décodage hors ligne). À valider en direct sur GYA 4610 kHz.
