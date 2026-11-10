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
